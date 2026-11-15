/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAction.h"
#include "SlowScope.h"  // [perf26]
#include "BotPath.h"  // [guild12]
#include "QuestStats.h"  // [guild90]
#include "SmartScriptMgr.h"  // [guild15] talk-to objectives
#include "CellImpl.h"           // [skin12]
#include "Formulas.h"           // [skin12]
#include "GridNotifiers.h"      // [skin12]
#include "GridNotifiersImpl.h"  // [skin12]
#include <limits>
#include <set>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // [guild90] one wait-respawn line per bot, quest and 30 s step of the wait
    std::mutex waitLogLock;
    std::unordered_map<uint32, std::pair<uint32, uint32>> waitLogged;

    bool FirstWaitLog(Player* bot, uint32 questId, uint32 step)
    {
        std::lock_guard<std::mutex> guard(waitLogLock);
        auto& last = waitLogged[bot->GetGUID().GetCounter()];
        if (last.first == questId && last.second == step)
            return false;
        last = {questId, step};
        return true;
    }

    // [guild15] talk-to objectives (b325 The Kessel Run q9663: the bot stood next to Exarch Menelaous and nothing
    // happened): the npc's SmartAI gives the kill credit on gossip hello (event 64) or on a gossip option (event 62)
    // with SMART_ACTION_CALL_KILLEDMONSTER; the bot talks as a player's client does, once per npc spawn
    std::mutex talkLock;
    // [guild15] bots whose objective spot has a guessed height (see DoIncompleteQuest)
    std::mutex guessedZLock;
    std::set<uint32> guessedZBots;
    std::unordered_map<uint32, std::set<std::pair<uint32, uint32>>> talked;  // bot -> (quest, npc spawn)

    struct TalkCredit
    {
        bool hello = false;
        bool select = false;
        uint32 menu = 0;
        uint32 option = 0;
    };

    TalkCredit TalkCreditOf(uint32 entry)
    {
        TalkCredit credit;
        for (SmartScriptHolder const& e : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
        {
            if (e.GetActionType() != SMART_ACTION_CALL_KILLEDMONSTER)
                continue;
            if (e.GetEventType() == SMART_EVENT_GOSSIP_HELLO && e.event.gossipHello.filter != 2)
                credit.hello = true;
            else if (e.GetEventType() == SMART_EVENT_GOSSIP_SELECT)
            {
                credit.select = true;
                credit.menu = e.event.gossip.sender;
                credit.option = e.event.gossip.action;
            }
        }
        return credit;
    }
}
#include "SoloSafety.h"  // [guild13]
#include "SpellAuras.h"  // [deaths14] Resurrection Sickness
#include "TerrainHazard.h"  // [guild15] SafeFloorZ
#include "NewRpgCoop.h"  // [coop]
#include "IssueKey.h"  // [jrn43] the deduplication key of the issue row, as a seam a test can assert
#include "Journal.h"  // [jrn40] the wait-behind stuck row and its issue
#include "LeaderOrders.h"  // [deaths20] Leader::Route
#include "RouteSearch.h"  // [perf215] Leader::RouteSearchesOnThread
#include "AreaDefines.h"
#include "Group.h"
#include "NewRpgTriggers.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "DBCStores.h"
#include "GossipDef.h"
#include "IVMapMgr.h"
#include "MapMgr.h"  // [guild14]
#include "MotionMaster.h"
#include "GuildCoordinator.h"  // [guild30]
#include "GuildHonest.h"       // [group115]
#include "GuildRiding.h"       // [red140] NoteDismount
#include "MoveSpline.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "NewRpgProfession.h"  // [guild20]
#include "NewRpgFishing.h"  // [fish14]
#include "NewRpgDoGather.h"    // [guild14] NoSafeNodes
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "QuestDef.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "BuyAction.h"
#include "GuildGear.h"  // [supply27] VendorsOf  // [pace25] RESTOCK_*, CountBagFood
#include "DetourNavMeshQuery.h"  // [nav24]
#include "MapDefines.h"         // [nav24]
#include "WaypointMovementGenerator.h"
#include "World.h"  // [guild15] durability rates
#include "BudgetValues.h"  // [eco201] the consumables budget
#include "WorldPacket.h"
#include "G3D/Vector2.h"
#include "GuildLedger.h"
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <mutex>

namespace
{
    // [sick51] the walks the sickness hold issued (a step out of a reach, the walk out to its hub): the hold keeps
    // them going instead of stopping every move (it stopped its own steps the tick after)
    std::mutex sickWalkLock;
    std::unordered_set<uint32> sickWalking;
    void SickWalk(Player* bot, bool on)
    {
        std::lock_guard<std::mutex> guard(sickWalkLock);
        if (on)
            sickWalking.insert(bot->GetGUID().GetCounter());
        else
            sickWalking.erase(bot->GetGUID().GetCounter());
    }
    bool SickWalking(Player* bot)
    {
        std::lock_guard<std::mutex> guard(sickWalkLock);
        return sickWalking.count(bot->GetGUID().GetCounter()) != 0;
    }
}

namespace
{
    // [guild3] Vendor spawns of the world, read once from the creature spawn table.
    struct VendorSpawn
    {
        uint32 entry;
        uint16 mapId;
        float x, y, z;
        ObjectGuid::LowType spawnId;
        bool walks;  // [vendor23] a waypoint path (its spawn's MovementType or addon path): not met at its spawn
    };

    std::vector<VendorSpawn> const& VendorSpawns()
    {
        static std::vector<VendorSpawn> spawns;
        static std::once_flag once;
        std::call_once(once, []()
        {
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(data.id);
                if (!info || !(info->npcflag & UNIT_NPC_FLAG_VENDOR) || !sObjectMgr->GetNpcVendorItemList(data.id))
                    continue;
                CreatureAddon const* addon = sObjectMgr->GetCreatureAddon(spawnId);
                bool const walks = data.movementType == WAYPOINT_MOTION_TYPE || (addon && addon->path_id);
                spawns.push_back({data.id, data.mapid, data.posX, data.posY, data.posZ, spawnId, walks});
            }
        });
        return spawns;
    }

    // The ammo the bot's ranged weapon fires: ITEM_SUBCLASS_ARROW / ITEM_SUBCLASS_BULLET, 0 for none.
    uint32 RequiredAmmoSubClass(Player* bot)
    {
        Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        ItemTemplate const* proto = ranged ? ranged->GetTemplate() : nullptr;
        if (!proto || proto->Class != ITEM_CLASS_WEAPON)
            return 0;
        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_GUN:
                return ITEM_SUBCLASS_BULLET;
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                return ITEM_SUBCLASS_ARROW;
            default:
                return 0;
        }
    }

    // The cheapest stack of usable ammo of that subclass the vendor sells, 0 when none.
    uint32 AmmoPrice(Player* bot, uint32 entry, uint32 subClass)
    {
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry);
        if (!items)
            return 0;
        uint32 best = 0;
        for (VendorItem const* item : items->m_items)
        {
            ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
            if (!proto || proto->Class != ITEM_CLASS_PROJECTILE || proto->SubClass != subClass ||
                proto->RequiredLevel > bot->GetLevel() || item->ExtendedCost)
                continue;
            uint32 const price = std::max<uint32>(1, proto->BuyPrice);
            if (!best || price < best)
                best = price;
        }
        return best;
    }

    // [pace25] the cheapest food (SPELL_AURA_OBS_MOD_HEALTH) or drink (SPELL_AURA_OBS_MOD_POWER) of its level the
    // vendor sells (BuyAction's restock items, spell category 11 / 59); 0 when none
    uint32 RestockPrice(Player* bot, uint32 entry, uint32 spellCategory)
    {
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry);
        if (!items)
            return 0;
        // [judge16] the walk's judge and the purchase's judge are one test now (IsRestockFood, the [supply41] rule:
        // the real Food / Drink auras behind the spell, and the item's own subclass). This priced a vendor by its own
        // copy of that test which had no subclass term at all, so any consumable carrying spell category 11 / 59 - a
        // potion, an elixir, a scroll - made the vendor "sell food": the bot budgeted for it, walked there, and
        // PickVendorFood (which does ask IsRestockFood) bought none. b475 logged 16 `[eco204] ... restock bought
        // nothing: no vendor within 25 yd sells food of its level it can use`, bots carrying 5712-14785 c.
        uint32 best = 0;
        for (VendorItem const* item : items->m_items)
        {
            ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
            // [eco201] ... and one it can use (RestockFoodAction::PickVendorFood's own check)
            if (!proto || item->ExtendedCost || !IsRestockFood(proto, spellCategory) ||
                proto->RequiredLevel > bot->GetLevel() || !proto->BuyPrice ||
                bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            if (!best || proto->BuyPrice < best)
                best = proto->BuyPrice;
        }
        return best;
    }

    thread_local uint32 restockWanted = 0;
    // [eco205] the restock's refused vendors (NewRpgInfo::restockRefused) for the vendor choice below
    thread_local NewRpgInfo const* restockRefusedBy = nullptr;
    // [eco201] the money its restock may spend (free money for consumables): a vendor whose cheapest food / drink of
    // its level costs more is no restock vendor for it
    thread_local uint32 restockBudget = 0;

    // [eco201] the cheapest food (bit 0) / drink (bit 1) of its level the vendors of its faction on its map sell for
    // gold, 0 = none sells it (a failed look waits until its budget covers it)
    uint32 CheapestRestock(Player* bot, uint32 wanted)
    {
        FactionTemplateEntry const* own = bot->GetFactionTemplateEntry();
        uint32 food = 0, drink = 0;
        for (VendorSpawn const& spawn : VendorSpawns())
        {
            if (spawn.mapId != bot->GetMapId())
                continue;
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(spawn.entry);
            FactionTemplateEntry const* faction = info ? sFactionTemplateStore.LookupEntry(info->faction) : nullptr;
            if (!faction || !own || faction->IsHostileTo(*own) || own->IsHostileTo(*faction))
                continue;
            if (uint32 const f = (wanted & 1) ? RestockPrice(bot, spawn.entry, RESTOCK_SPELL_CATEGORY_FOOD) : 0;
                f && (!food || f < food))
                food = f;
            if (uint32 const d = (wanted & 2) ? RestockPrice(bot, spawn.entry, RESTOCK_SPELL_CATEGORY_DRINK) : 0;
                d && (!drink || d < drink))
                drink = d;
        }
        return std::max(food, drink);
    }

    // [eco205] coordinator 2026-11-11: a vendor that could not stock it is skipped - that vendor only, not every
    // vendor of its map - until its consumables budget is more than it was there; a new map, a new list
    void RefuseRestockVendor(Player* bot, NewRpgInfo& info, uint32 entry, uint32 budget)
    {
        if (info.restockRefusedMap != bot->GetMapId())
        {
            info.restockRefused.clear();
            info.restockRefusedMap = bot->GetMapId();
        }
        if (entry)
            info.restockRefused[entry] = budget;
    }
    // [vend20] the vendor spawn a bot's errand walks to, and the spawns where it stood and met no vendor there. A
    // vendor errand's verdict is the vendor's own (bought, nothing it sells, no money, nobody there) - never the quest
    // giver's. b493 Geron L22 walked Thomas Miller, Khole Jinglepocket and Guchie Jinglepocket in 15 s, each one
    // "finds no vendor N at its spawn", each ending "IDLE(no quest giver here with anything to give or take)" and
    // each followed by the next vendor at once: the first is a waypoint walker who had walked off, the other two are
    // game-event spawns (Winter Veil) that stand in `creature` and never in the world. Nobody there is the vendor's
    // own fact, not the budget's ([eco205] restockRefused keeps the budget ones, and only for a restock run), so the
    // spawn the bot looked at is no place for it to meet that vendor again
    std::mutex vendorLookLock;
    std::unordered_map<uint32, ObjectGuid::LowType> vendorRunSpawn;
    std::unordered_map<uint32, std::unordered_set<ObjectGuid::LowType>> vendorNotThere;

    void NoteVendorRunSpawn(Player* bot, ObjectGuid::LowType spawnId)
    {
        std::lock_guard<std::mutex> guard(vendorLookLock);
        vendorRunSpawn[bot->GetGUID().GetCounter()] = spawnId;
    }

    // the bot stood at its errand's vendor spawn and no vendor was there: that spawn is marked for its later choices
    ObjectGuid::LowType NoteVendorNotThere(Player* bot)
    {
        std::lock_guard<std::mutex> guard(vendorLookLock);
        auto const found = vendorRunSpawn.find(bot->GetGUID().GetCounter());
        if (found == vendorRunSpawn.end() || !found->second)
            return 0;
        vendorNotThere[bot->GetGUID().GetCounter()].insert(found->second);
        return found->second;
    }

    bool VendorNotThere(Player* bot, ObjectGuid::LowType spawnId)
    {
        std::lock_guard<std::mutex> guard(vendorLookLock);
        auto const found = vendorNotThere.find(bot->GetGUID().GetCounter());
        return found != vendorNotThere.end() && found->second.count(spawnId) != 0;
    }

    thread_local std::vector<uint32> const* reagentsWanted = nullptr;  // [supply27] the vendor sells one of them  // [pace25] the spell categories the vendor choice needs (bit 0 food, 1 drink)

    // The nearest vendor spawn on the bot's map that is not hostile to it (and sells its ammo when it needs some).
    // [guild14] repair: only those that repair (UNIT_NPC_FLAG_REPAIR); a solo bot's trip there must be safe
    // (NewRpgBaseAction::TripRefused), else the next nearest
    // [vendor23] a vendor that walks a waypoint path is where the map has it now: its live position on the bot's map
    // (no creature loaded there or a dead one: no place to meet it). b372 Ravelandy L14: Antonio Perelli (the
    // Traveling Salesman, path 811810) chosen at his spawn 0 yd off 35 times - "finds no vendor 844 at its spawn",
    // the leader's marked sale sending the run again each time, VENDOR 1200 s
    thread_local std::vector<VendorSpawn> walkingNow;

    VendorSpawn const* NearestVendorOf(Player* bot, uint32 ammoSubClass, bool repair, float& dist)
    {
        FactionTemplateEntry const* own = bot->GetFactionTemplateEntry();
        std::vector<std::pair<float, VendorSpawn const*>> candidates;
        walkingNow.clear();
        walkingNow.reserve(64);
        for (VendorSpawn const& listed : VendorSpawns())
        {
            if (listed.mapId != bot->GetMapId())
                continue;
            VendorSpawn const* at = &listed;
            if (listed.walks)
            {
                if (walkingNow.size() == walkingNow.capacity())
                    continue;  // the reserved slots hold the pointers taken below (a map's walking vendors fit)
                auto const range = bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(listed.spawnId);
                Creature* live = range.first != range.second ? range.first->second : nullptr;
                if (!live || !live->IsAlive() || !live->IsInWorld())
                    continue;
                VendorSpawn now = listed;
                now.x = live->GetPositionX();
                now.y = live->GetPositionY();
                now.z = live->GetPositionZ();
                walkingNow.push_back(now);
                at = &walkingNow.back();
            }
            VendorSpawn const& spawn = *at;
            if (VendorNotThere(bot, spawn.spawnId))  // [vend20] it stood there and met no vendor
                continue;
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(spawn.entry);
            if (!info || (repair && !(info->npcflag & UNIT_NPC_FLAG_REPAIR)))
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->faction);
            if (!faction || !own || faction->IsHostileTo(*own) || own->IsHostileTo(*faction))
                continue;
            if (ammoSubClass && !AmmoPrice(bot, spawn.entry, ammoSubClass))
                continue;
            // [supply27] a five member's reagent run goes to a vendor that sells one it lacks
            if (reagentsWanted && !reagentsWanted->empty())
            {
                VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(spawn.entry);
                bool sells = false;
                for (VendorItem const* item : items ? items->m_items : std::vector<VendorItem*>())
                    sells = sells || (item && !item->ExtendedCost &&
                                      std::find(reagentsWanted->begin(), reagentsWanted->end(), item->item) !=
                                          reagentsWanted->end());
                if (!sells)
                    continue;
            }
            // [pace25] a restock run goes to a vendor that sells what it lacks
            // [eco201] coordinator 2026-11-11: ... at a price its restock may spend (b432: 30 "buy drink" visits in
            // 25 min bought nothing - Ainne 6, Namwe 7 vendors - their consumables budget below the cheapest drink)
            if (restockWanted)
            {
                uint32 const food =
                    (restockWanted & 1) ? RestockPrice(bot, spawn.entry, RESTOCK_SPELL_CATEGORY_FOOD) : 0;
                uint32 const drink =
                    (restockWanted & 2) ? RestockPrice(bot, spawn.entry, RESTOCK_SPELL_CATEGORY_DRINK) : 0;
                if (((restockWanted & 1) && (!food || food > restockBudget)) ||
                    ((restockWanted & 2) && (!drink || drink > restockBudget)))
                    continue;
                // [eco205] ... and not one that could not stock it with this budget
                if (restockRefusedBy && restockRefusedBy->restockRefusedMap == bot->GetMapId())
                    if (auto const refused = restockRefusedBy->restockRefused.find(spawn.entry);
                        refused != restockRefusedBy->restockRefused.end() && restockBudget <= refused->second)
                        continue;
            }
            candidates.emplace_back(bot->GetExactDist(spawn.x, spawn.y, spawn.z), &spawn);
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](auto const& a, auto const& b) { return a.first < b.first; });
        bool refusedOne = false;
        size_t repairTries = 0;
        for (auto const& [d, spawn] : candidates)
        {
            // after a refused trip only the nearer vendors of the zone the bot stands in are looked at (each look
            // searches a route): the first one outside it ends the search
            // [repair38] ... a repairer is looked for across its map (the continent), nearest first, until one's trip
            // is not refused: every repairer near it refused ([trip37]) left it unrepaired until its gear broke
            constexpr size_t REPAIR_TRIES = 16;  // a search cost bound (each a route search, cached per level)
            if (repair)
            {
                if (refusedOne && ++repairTries > REPAIR_TRIES)
                    break;
            }
            else if (refusedOne &&
                     sMapMgr->GetZoneId(PHASEMASK_NORMAL, spawn->mapId, spawn->x, spawn->y, spawn->z) !=
                         bot->GetZoneId())
                break;
            if (!NewRpgBaseAction::TripRefused(bot, spawn->x, spawn->y, spawn->z,
                                               repair ? "vendor (repair)" : "vendor"))
            {
                dist = d;
                return spawn;
            }
            refusedOne = true;
        }
        dist = FLT_MAX;
        if (repair && refusedOne)
            LOG_INFO("playerbots", "[repair38] {} L{}: no repairer on its map with a trip it takes ({} looked at): it "
                     "quests on, looked again at its next level",
                     bot->GetName(), bot->GetLevel(), std::min(repairTries, candidates.size()));
        return nullptr;
    }

    VendorSpawn const* NearestVendor(Player* bot, uint32 ammoSubClass, float& dist)
    {
        return NearestVendorOf(bot, ammoSubClass, false, dist);
    }

    // [guild13] the nearest friendly vendor spawn on the bot's map that repairs (UNIT_NPC_FLAG_REPAIR)
    VendorSpawn const* NearestRepairer(Player* bot, float& dist) { return NearestVendorOf(bot, 0, true, dist); }

    // Low ammo by the hunters' "low ammo" line (100 rounds).
    constexpr uint32 VENDOR_LOW_AMMO = 100;
}

void NewRpgBaseAction::BuyAmmoDirect(Creature* vendor, uint32 subClass, uint32 have)
{
    VendorItemData const* items = vendor->GetVendorItems();
    if (!items)
        return;
    int32 bestSlot = -1;
    uint32 bestPrice = 0;
    ItemTemplate const* best = nullptr;
    for (size_t i = 0; i < items->m_items.size(); ++i)
    {
        VendorItem const* item = items->m_items[i];
        ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
        if (!proto || proto->Class != ITEM_CLASS_PROJECTILE || proto->SubClass != subClass ||
            proto->RequiredLevel > bot->GetLevel() || item->ExtendedCost || bot->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;
        uint32 const price = std::max<uint32>(1, proto->BuyPrice);
        if (!best || price < bestPrice)
        {
            best = proto;
            bestPrice = price;
            bestSlot = int32(i);
        }
    }
    std::string why;
    uint32 bought = 0;
    if (!best)
        why = "the vendor sells no ammo it can use";
    else
    {
        uint32 const stack = std::max<uint32>(1, best->BuyCount);
        while (have + bought < VENDOR_LOW_AMMO && bot->GetMoney() >= bestPrice)
        {
            if (!bot->BuyItemFromVendorSlot(vendor->GetGUID(), uint32(bestSlot), best->ItemId, uint8(1), NULL_BAG,
                                            NULL_SLOT))
            {
                why = "the core refused the purchase (bags / money / stock)";
                break;
            }
            bought += stack;
        }
    }
    // [ammo90] the ammo it bought is the ammo it fires (PLAYER_AMMO_ID), as a player drags it to the ammo slot: b411
    // Ganna's Fine Longbow had Light Shot (bullets) set, every Shoot failed (no ammo)
    if (bought && best && bot->GetUInt32Value(PLAYER_AMMO_ID) != best->ItemId)
        bot->SetAmmo(best->ItemId);
    LOG_INFO("playerbots", "[vendor12] {} L{} ammo at {}: had {}, bought {} of {}{}", bot->GetName(), bot->GetLevel(),
             vendor->GetName(), have, bought, best ? best->Name1 : std::string("-"), why.empty() ? "" : " - " + why);
}

float NewRpgBaseAction::DeathDurabilityLoss(Player* bot)
{
    return (sWorld->getRate(RATE_DURABILITY_LOSS_ON_DEATH) +
            (int32(bot->GetLevel()) < sWorld->getIntConfig(CONFIG_DEATH_SICKNESS_LEVEL)
                 ? sWorld->getRate(RATE_DURABILITY_LOSS_ON_SPIRIT_RESURRECT)
                 : 0.0f)) /
           100.0f;
}

WorldPosition NewRpgBaseAction::NearestRepairSpawn(Player* bot)
{
    FactionTemplateEntry const* own = bot->GetFactionTemplateEntry();
    VendorSpawn const* best = nullptr;
    float bestDist = FLT_MAX;
    for (VendorSpawn const& spawn : VendorSpawns())
    {
        if (spawn.mapId != bot->GetMapId())
            continue;
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(spawn.entry);
        if (!info || !(info->npcflag & UNIT_NPC_FLAG_REPAIR))
            continue;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->faction);
        if (!faction || !own || faction->IsHostileTo(*own) || own->IsHostileTo(*faction))
            continue;
        float const d = bot->GetExactDist(spawn.x, spawn.y, spawn.z);
        if (d < bestDist)
        {
            bestDist = d;
            best = &spawn;
        }
    }
    return best ? WorldPosition(best->mapId, best->x, best->y, best->z) : WorldPosition();
}

bool NewRpgBaseAction::TownTaskHere()
{
    // a party member's town stop it stands at (its leader's route takes its stops): the global leader's step for it
    // runs here (NextStep acts on a convenient town task only in its town: BUSY); any other step waits for its party
    std::vector<std::pair<WorldPosition, std::string>> towns;
    sGuildCoordinator.TownStops(bot, towns);
    for (auto const& [pos, what] : towns)
    {
        // the town stop's reach (GO_CAMP's, NewRpgQuestRoute TOWN_STOP_REACHED)
        if (bot->IsInCombat() || bot->GetDistance2d(pos.GetPositionX(), pos.GetPositionY()) > 10.0f)
            continue;
        GuildRunStep const step = sGuildCoordinator.NextStep(bot, botAI);
        if (step.kind == GuildRunStep::BUSY)
        {
            LOG_INFO("playerbots", "[town12] {} L{} runs its town task {} with its party", bot->GetName(),
                     bot->GetLevel(), what);
            return true;
        }
        return false;
    }
    return false;
}

// [quest16] a party member's complete quest handed in where its leader's route stands it at the ender (the leader
// routes its members' hand-ins: NewRpgBaseAction::CurrentRoute, "HAND_IN ... (for <member>)"): an npc in sight that
// ends one of its complete quests, its leader in sight of it too (the party is there) - walked to and handed in. b355

// Gilduck L11 held q315 complete 1079 s (its ender 2nd in its own route) while it followed Bhogagnuth's help party
bool NewRpgBaseAction::MemberHandInHere()
{
    Group* group = bot->GetGroup();
    if (!group || bot->IsInCombat() || group->GetLeaderGUID() == bot->GetGUID())
        return false;
    Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
    if (!leader || leader->GetMapId() != bot->GetMapId())
        return false;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest npcs"))
    {
        Creature* npc = botAI->GetCreature(guid);
        if (!npc || !npc->IsAlive() || !npc->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER) ||
            leader->GetExactDist2d(npc) > sPlayerbotAIConfig.SightDistance)  // the party is at it
            continue;
        uint32 ends = 0;
        QuestRelationBounds const bounds = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(npc->GetEntry());
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            if (bot->GetQuestStatus(itr->second) == QUEST_STATUS_COMPLETE)
                ends = itr->second;
        if (!ends)
            continue;
        if (IsWithinInteractionDist(npc))
        {
            LOG_INFO("playerbots", "[quest16] {} L{} hands in q{} at {} where its leader {} stands", bot->GetName(),
                     bot->GetLevel(), ends, npc->GetName(), leader->GetName());
            return InteractWithNpcOrGameObjectForQuest(npc->GetGUID());
        }
        if (MoveTo(npc->GetMapId(), npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ(), false, false,
                   false, true))
            return true;
    }
    return false;
}

bool NewRpgBaseAction::GuildStepGivenUp(GuildRunStep const& step)
{
    if (step.pos == WorldPosition() || step.pos.GetMapId() != bot->GetMapId() ||
        !WalkGivenUp(bot, step.pos.GetPositionX(), step.pos.GetPositionY()))
        return false;
    // one line per bot, spot and level
    static std::mutex lock;
    static std::unordered_map<uint32, std::tuple<uint8, int32, int32>> last;
    std::tuple<uint8, int32, int32> const key{bot->GetLevel(), int32(step.pos.GetPositionX()),
                                              int32(step.pos.GetPositionY())};
    bool fresh = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto& previous = last[bot->GetGUID().GetCounter()];
        fresh = previous != key;
        previous = key;
    }
    if (fresh)
    {
        LOG_INFO("playerbots", "[rpg] {} L{} GUILD_STEP_GIVEN_UP ({:.0f},{:.0f}) {:.0f} yd: {} - its walk there was "
                 "given up at L{}; it quests on",
                 bot->GetName(), bot->GetLevel(), step.pos.GetPositionX(), step.pos.GetPositionY(),
                 bot->GetExactDist2d(step.pos.GetPositionX(), step.pos.GetPositionY()), step.reason, bot->GetLevel());
        sGuildCoordinator.SetOrderStatus(bot, "no safe walk there (given up): quests on");
    }
    return true;
}

bool NewRpgBaseAction::HearthOut(std::string const& why, WorldPosition const* camp)
{
    constexpr uint32 ITEM_HEARTHSTONE = 6948;
    constexpr uint32 SPELL_HEARTHSTONE = 8690;
    if (bot->IsInCombat() || !bot->IsAlive() || bot->IsInFlight() || bot->GetTransport())
        return false;
    WorldPosition const bind(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ);
    NewRpgInfo& info = botAI->rpgInfo;
    std::string note;
    if (!bot->HasItemCount(ITEM_HEARTHSTONE, 1))
        note = "no Hearthstone in its bags";
    else if (bind.GetMapId() == bot->GetMapId() && bot->GetExactDist(bind) <= sPlayerbotAIConfig.SightDistance)
        note = "its bind point is here";
    if (!note.empty())
    {
        if (info.hearthNote != note)
        {
            info.hearthNote = note;
            LOG_INFO("playerbots", "[rpg] {} L{} HEARTH reason: {}; not used: {}", bot->GetName(), bot->GetLevel(),
                     why, note);
        }
        return false;
    }
    if (bot->HasSpellCooldown(SPELL_HEARTHSTONE))
    {
        // the stone's cooldown is the game's: it waits it out where it stands (out of combat, nothing on it)
        uint32 const left = bot->GetSpellCooldownDelay(SPELL_HEARTHSTONE) / IN_MILLISECONDS;
        note = "cooldown";
        if (info.hearthNote != note)
        {
            info.hearthNote = note;
            LOG_INFO("playerbots", "[rpg] {} L{} HEARTH reason: {}; its Hearthstone is ready in {} s: waits here",
                     bot->GetName(), bot->GetLevel(), why, left);
            // [hearth23] coordinator 2026-11-09: walled in with the stone on its cooldown, it asks for help with the
            // camp (the help21 / help22 pricing decides); refused or no helper, it waits the cooldown out at the
            // safest spot. b372 Aevmae L11 waited 1269 s, Hobliir L15 279 s where the walls stood round it
            if (camp && *camp != WorldPosition() && !NewRpgCoop::HelpAsked(bot))
            {
                LOG_INFO("playerbots", "[hearth23] {} L{} walled in, its Hearthstone ready in {} s: asks for help "
                         "with the camp at ({:.0f},{:.0f})",
                         bot->GetName(), bot->GetLevel(), left, camp->GetPositionX(), camp->GetPositionY());
                NewRpgCoop::OnCampRefused(botAI, bot, *camp, why);
            }
        }
        // [hearth23] the wait is taken out of every idle mob's and wanderer's reach (the RestEscape spots)
        if (!bot->IsInCombat() && !bot->isMoving())
        {
            std::string who;
            std::vector<G3D::Vector3> spots;
            if (SoloSafety::StandsInReach(bot, who, spots))
                for (G3D::Vector3 const& at : spots)
                {
                    Leader::RouteResult const route = Leader::Route(bot, Position(at.x, at.y, at.z));
                    if (!route.found || !route.complete || route.throughRefused)
                        continue;
                    if (bot->IsSitState())
                        bot->SetStandState(UNIT_STAND_STATE_STAND);
                    if (!MoveTo(bot->GetMapId(), at.x, at.y, at.z, false, false, false, true))
                        continue;
                    LOG_INFO("playerbots", "[hearth23] {} L{} waits for its Hearthstone out of the reach of {}: "
                             "steps to ({:.0f},{:.0f}), {:.0f} yd walk",
                             bot->GetName(), bot->GetLevel(), who, at.x, at.y, route.length);
                    return true;
                }
            if (!who.empty())
                SoloSafety::NoteNoWayOut(bot, who, spots.size(), "Hearthstone cooldown");  // [deaths140]
        }
        if (bot->isMoving())
            return true;  // its step out of the reaches
        if (GrindWhileHeld("its Hearthstone's cooldown"))  // [lag352]
            return true;
        info.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    // [hearth30] the Hearthstone's own cast takes 10 s: a second use re-issued over it cancels the running one and
    // the cast never ends. While it runs nothing is issued and the decision is not logged again
    if (bot->IsNonMeleeSpellCast(false))
    {
        note = "its Hearthstone's cast is under way";
        if (info.hearthNote != note)
        {
            info.hearthNote = note;
            LOG_INFO("playerbots", "[hearth30] {} L{} HEARTH reason: {}; its cast is under way: nothing re-issued",
                     bot->GetName(), bot->GetLevel(), why);
        }
        return true;
    }
    // [hearth31] the Hearthstone (spell 8690, Attributes 0x08010000) carries SPELL_ATTR0_NOT_SHAPESHIFT:
    // SpellInfo::CheckShapeshift answers SPELL_FAILED_NOT_SHAPESHIFT to a druid standing in an animal form, so the
    // core refuses the use - no cast, no cooldown - while the bot's own check lets NOT_SHAPESHIFT pass and issues
    // the use again every 10 s. b475 Lyleria L22, a bear tank ([b32_tank_aoe] holds its Dire Bear Form out of
    // combat), stood in Orgrimmar's Cleft of Shadow over 26 server sessions on that loop. It leaves the form first,
    // as a player's client does, and the same call casts ([dun221] does this for the leader's externals)
    bool leftForm = false;
    if (ShapeshiftForm const form = bot->GetShapeshiftForm(); form != FORM_NONE)
        if (SpellInfo const* stoneSpell = sSpellMgr->GetSpellInfo(SPELL_HEARTHSTONE);
            stoneSpell && stoneSpell->CheckShapeshift(form) != SPELL_CAST_OK)
        {
            bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            leftForm = true;
        }
    // [red140] the same class as [hearth31], one state further: Spell::CheckCast's "not let players cast spells at
    // mount" answers SPELL_FAILED_NOT_MOUNTED to a mounted caster of a spell without SPELL_ATTR0_ALLOW_WHILE_MOUNTED,
    // which the Hearthstone is. The name reads like "it must be mounted" and is the opposite: it IS mounted. There is
    // no second judge here - CheckCastNow runs the core's own Spell::CheckCast (PlayerbotAI::RunCheckCast) and nothing
    // else answers - so the refusal is honest and the bot's own decision to use the stone is unchanged; what was
    // missing is the client's own step before the use. b486: Kylantino, Ginde, Bhogagnuth, Lanbuzz, Uraela and
    // Rumanatol were held this way, every one of them with a [mount5] walk behind it, each re-issuing the use every
    // 10 s with no cast and no cooldown. It gets off first, as a player's client does, and the same call casts
    bool leftMount = false;
    if (bot->IsMounted())
    {
        GuildRiding::NoteDismount(bot, "its Hearthstone");  // [mount7]
        bot->Dismount();
        leftMount = true;
    }
    // [hearth32] the game's own answer for the use is named in the line, and one line stands for one decision: the
    // same reason with the same answer is not logged again (the cooldown branch's hearthNote does the same)
    // [cap70] a bind point on another map has no distance from here: the line said "0 yd" for it
    PlayerbotAI::CastCheck const check =
        botAI->CheckCastNow(SPELL_HEARTHSTONE, bot, bot->GetItemByEntry(ITEM_HEARTHSTONE));
    note = std::string("use|") + (leftForm ? "form|" : "") + (leftMount ? "mount|" : "") +
           PlayerbotAI::CastResultName(check.result) + "|" + why;
    if (info.hearthNote != note)
    {
        info.hearthNote = note;
        if (leftMount)
            LOG_INFO("playerbots", "[red140] {} L{} gets off its mount for its Hearthstone (a mounted caster is "
                     "refused SPELL_FAILED_NOT_MOUNTED): {}", bot->GetName(), bot->GetLevel(), why);
        if (leftForm)
            LOG_INFO("playerbots", "[hearth31] {} L{} leaves its animal form for its Hearthstone (the form forbids "
                     "its use): {}", bot->GetName(), bot->GetLevel(), why);
        LOG_INFO("playerbots", "[rpg] {} L{} HEARTH reason: {}; to its bind point ({:.0f},{:.0f}) map {}, {}; "
                 "[hearth32] the game answers {}",
                 bot->GetName(), bot->GetLevel(), why, bind.GetPositionX(), bind.GetPositionY(), bind.GetMapId(),
                 bind.GetMapId() == bot->GetMapId()
                     ? Acore::StringFormat("{:.0f} yd", bot->GetExactDist(bind))
                     : std::string("another map"),
                 PlayerbotAI::CastResultName(check.result));
    }
    return botAI->DoSpecificAction("hearthstone", Event("new rpg hearth"), true);
}

namespace
{
    // [hold58] bot -> its walled-in hold: the refused walk's spot (its camp), why, where and at which level it began
    struct WalledHold
    {
        WorldPosition camp;
        WorldPosition at;
        std::string why;
        uint8 level = 0;
    };
    std::mutex walledHoldLock;
    std::unordered_map<uint32, WalledHold> walledHolds;
}

void NewRpgBaseAction::NoteWalledHold(WorldPosition const& camp, std::string const& why)
{
    // nothing to wait for - no help asked and no Hearthstone to take it out (none, or bound where it stands): no hold
    constexpr uint32 ITEM_HEARTHSTONE = 6948;
    if (!NewRpgCoop::HelpAsked(bot) &&
        (!bot->HasItemCount(ITEM_HEARTHSTONE, 1) ||
         (bot->m_homebindMapId == bot->GetMapId() &&
          bot->GetExactDist(bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ) <= sPlayerbotAIConfig.SightDistance)))
        return;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> guard(walledHoldLock);
        WalledHold& hold = walledHolds[bot->GetGUID().GetCounter()];
        fresh = hold.level == 0;
        if (fresh)
            hold = WalledHold{camp, WorldPosition(bot), why, bot->GetLevel()};
    }
    if (fresh)
        LOG_INFO("playerbots", "[hold58] {} L{} held at ({:.0f},{:.0f}): {} - no plan until its help arrives, the camp "
                 "at ({:.0f},{:.0f}) is cleared, its level changes or its Hearthstone takes it out",
                 bot->GetName(), bot->GetLevel(), bot->GetPositionX(), bot->GetPositionY(), why, camp.GetPositionX(),
                 camp.GetPositionY());
}

// [hold58] coordinator 2026-11-09: b397 Emdyl L14, walled in by the Rotten Ghouls and Leprithus L19 in Westfall with
// its Hearthstone on cooldown ("waits here"), sat to REST at full health - the rest ended at once, the planner took
// q12, q102 and a sweep from inside the walls, the sweep's leg ran past two ghouls and Leprithus joined: dead 27 s
// later. Salleas L12 (help14 "stays for its help request") re-planned 13 far zones from inside its walls. The hold is
// the game's: its help (a party), the wall's camp cleared (ForgetWalkGivenUp), its next level (every given-up walk
// is its choice again) or the stone's cooldown end
bool NewRpgBaseAction::WalledHoldStep()
{
    WalledHold hold;
    {
        std::lock_guard<std::mutex> guard(walledHoldLock);
        auto const found = walledHolds.find(bot->GetGUID().GetCounter());
        if (found == walledHolds.end())
            return false;
        hold = found->second;
    }
    std::string released;
    if (!bot->IsAlive())
        released = "it died";
    else if (bot->GetLevel() != hold.level)
        released = "its level changed: its given-up walks are its choices again";
    else if (NewRpgCoop::InParty(bot->GetGUID().GetCounter()))
        released = "its help arrived (a party holds it)";
    else if (hold.camp.GetMapId() != bot->GetMapId() ||
             !WalkGivenUp(bot, hold.camp.GetPositionX(), hold.camp.GetPositionY()))
        released = "the walk its walls refused is its choice again (the camp cleared)";
    else if (hold.at.GetMapId() != bot->GetMapId() ||
             bot->GetExactDist(hold.at) > sPlayerbotAIConfig.SightDistance)
        released = "it is out of its walls (its Hearthstone, a fight's run)";
    if (!released.empty())
    {
        {
            std::lock_guard<std::mutex> guard(walledHoldLock);
            walledHolds.erase(bot->GetGUID().GetCounter());
        }
        LOG_INFO("playerbots", "[hold58] {} L{} hold ends: {}", bot->GetName(), bot->GetLevel(), released);
        return false;
    }
    if (bot->IsInCombat() || bot->IsInFlight() || bot->GetTransport())
        return false;
    if (bot->IsNonMeleeSpellCast(false))
        return true;  // its Hearthstone's cast under way
    // its help request is its way out while the global leader serves it ([help14]): it waits out of every reach
    if (NewRpgCoop::HelpAsked(bot))
    {
        if (!bot->isMoving())
        {
            std::string who;
            std::vector<G3D::Vector3> spots;
            if (SoloSafety::StandsInReach(bot, who, spots))
                for (G3D::Vector3 const& at : spots)
                {
                    Leader::RouteResult const route = Leader::Route(bot, Position(at.x, at.y, at.z));
                    if (!route.found || !route.complete || route.throughRefused)
                        continue;
                    if (bot->IsSitState())
                        bot->SetStandState(UNIT_STAND_STATE_STAND);
                    if (!MoveTo(bot->GetMapId(), at.x, at.y, at.z, false, false, false, true))
                        continue;
                    LOG_INFO("playerbots", "[hold58] {} L{} waits for its help out of the reach of {}: steps to "
                             "({:.0f},{:.0f}), {:.0f} yd walk",
                             bot->GetName(), bot->GetLevel(), who, at.x, at.y, route.length);
                    return true;
                }
            if (!who.empty())
                SoloSafety::NoteNoWayOut(bot, who, spots.size(), "walled-in hold");  // [deaths140]
        }
        if (bot->isMoving())
            return true;
        if (GrindWhileHeld("its help on the way"))  // [lag352]
            return true;
        botAI->rpgInfo.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    // no help asked or served: its Hearthstone out (waited out where it is safest while on its cooldown)
    if (HearthOut(hold.why, &hold.camp))
        return true;
    {
        std::lock_guard<std::mutex> guard(walledHoldLock);
        walledHolds.erase(bot->GetGUID().GetCounter());
    }
    LOG_INFO("playerbots", "[hold58] {} L{} hold ends: no help and no Hearthstone to wait for ({})", bot->GetName(),
             bot->GetLevel(), botAI->rpgInfo.hearthNote);
    return false;
}

// [lag352] coordinator 2026-11-11 (user: a bot out of choices still has grinding near it - honest experience, never
// an idle wait for a level it cannot reach): b446 Rollothan L14 walled in among the Westfall Riverpaws L14-15 sat
// 1643 s for his Hearthstone's cooldown, the same-level mobs round him alive. The wait is spent on the mob its judge
// wins in reach (GrindTargetValue under the grind where it stands, WANDER_RANDOM: its pack, links, assists and rest
// priced as every pull); a fight's kills may clear the very camp that walls it. None in reach: it rests as before
bool NewRpgBaseAction::GrindWhileHeld(char const* why)
{
    if (bot->IsInCombat() || !bot->IsAlive() || !SoloSafety::Applies(bot) || bot->IsNonMeleeSpellCast(false))
        return false;
    NewRpgInfo& info = botAI->rpgInfo;
    bool const wandering = info.GetStatus() == RPG_WANDER_RANDOM;
    NewRpgInfo::RpgData const before = info.data;
    uint32 const beforeT = info.startT;
    if (!wandering)
        info.ChangeToWanderRandom();
    Unit* const target = AI_VALUE(Unit*, "grind target");
    if (!target)
    {
        if (!wandering)
        {
            info.data = before;  // no fight here: the state it held, no status change
            info.startT = beforeT;
        }
        return false;
    }
    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    static std::mutex heldLock;  // a log cadence per bot and target, no decision reads it
    static std::unordered_map<uint32, ObjectGuid> heldLogged;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> guard(heldLock);
        ObjectGuid& last = heldLogged[bot->GetGUID().GetCounter()];
        fresh = last != target->GetGUID();
        last = target->GetGUID();
    }
    if (fresh)
        LOG_INFO("playerbots", "[lag352] {} L{} held ({}): grinds {} L{} {:.0f} yd off where it stands", bot->GetName(),
                 bot->GetLevel(), why, target->GetName(), target->GetLevel(), bot->GetExactDist(target));
    return true;
}

bool NewRpgBaseAction::BindAtInnHere()
{
    if (bot->IsInCombat() || !SoloSafety::Applies(bot))
        return false;
    // [red73] no home in a zone red for it: its Hearthstone is its way out of such a place (b406 Gilduck L14, bound at
    // Darkshire (area level 25) in Duskwood, hearthed out of Brightwood Grove into Darkshire - "its bind point is
    // here" - and walked back through the Grove to his Westfall quests: 5 deaths to the Nightbane Dark Runners)
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetAreaId()))
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId() ? bot->GetZoneId() : area->ID))
            for (AreaTableEntry const* a : {area, zone})
                if (a->area_level > 0 &&
                    Acore::XP::GetColorCode(bot->GetLevel(), uint8(std::min(a->area_level, 255))) == XP_RED)
                    return false;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest npcs"))
    {
        Creature* innkeeper = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_INNKEEPER);
        if (!innkeeper)
            continue;
        if (bot->m_homebindMapId == innkeeper->GetMapId() &&
            innkeeper->GetExactDist(bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ) <=
                sPlayerbotAIConfig.SightDistance)
            return false;  // bound at this inn already
        LOG_INFO("playerbots", "[rpg] {} L{} BIND at {}'s inn (was map {} ({:.0f},{:.0f}))", bot->GetName(),
                 bot->GetLevel(), innkeeper->GetName(), bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY);
        bot->GetSession()->SendBindPoint(innkeeper);
        return true;
    }
    // [lag322] the bind above needs the innkeeper within its interaction reach, and a bot stands there only by chance
    // (b435: 0 BIND lines of 75 bots in 1.8 h). A player whose home lies on another landmass than the town it stands
    // in (only a ride joins the two: its Hearthstone would take it off its questing land) walks up to the town's
    // innkeeper and makes it home. b435 Sylaenas L13, bound at Auberdine (Darkshore), hearthed there out of Bloodmyst
    // after a death and left q9624 at 22/24 and q9641 at 15/24 (6713 xp) on the isle; Rollothan L13 went to Loch Modan
    // bound at Sentinel Hill (Westfall). Once per innkeeper and level, as [night305]'s walk to a flight master
    WorldPosition const bind(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ);
    if (bind.GetMapId() == bot->GetMapId() &&
        TravelRouteMgr::Landmass(bind) == TravelRouteMgr::Landmass(WorldPosition(bot)))
        return false;
    if (bot->isMoving() || bot->IsInFlight() || bot->GetTransport() || bot->GetMap()->Instanceable() ||
        !GuildHonest::IsHonestBot(bot))
        return false;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest npcs"))
    {
        Creature* innkeeper = ObjectAccessor::GetCreature(*bot, guid);
        if (!innkeeper || !innkeeper->IsAlive() || !innkeeper->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER) ||
            innkeeper->GetReactionTo(bot) <= REP_UNFRIENDLY || innkeeper->GetZoneId() != bot->GetZoneId())
            continue;
        // map thread: the bot -> (innkeeper spawn, level, steps taken: 1 the walk, 2 the last step up)
        thread_local std::unordered_map<uint64, std::tuple<ObjectGuid::LowType, uint8, uint8>> tried;
        auto& [spawn, level, steps] = tried[bot->GetGUID().GetRawValue()];
        if (spawn != innkeeper->GetSpawnId() || level != bot->GetLevel())
        {
            spawn = innkeeper->GetSpawnId();
            level = bot->GetLevel();
            steps = 0;
        }
        float const x = innkeeper->GetPositionX(), y = innkeeper->GetPositionY(), z = innkeeper->GetPositionZ();
        float const dist = bot->GetExactDist(innkeeper);
        // GO_CAMP ends within its arrival reach of the spot (RPG_GO_CAMP's 10 yd): the last yards up to the innkeeper
        constexpr float GO_CAMP_ARRIVAL = 10.0f;
        if (dist <= GO_CAMP_ARRIVAL && steps < 2)
        {
            steps = 2;
            return MoveTo(innkeeper->GetMapId(), x, y, z, false, false, false, true);
        }
        if (steps || WalkGivenUp(bot, x, y) || TripRefused(bot, x, y, z, "innkeeper"))
            return false;
        steps = 1;
        LOG_INFO("playerbots", "[lag322] {} L{} walks to {}'s inn ({:.0f} yd) to make it home: its bind point (map "
                 "{} ({:.0f},{:.0f})) lies on another landmass",
                 bot->GetName(), bot->GetLevel(), innkeeper->GetName(), dist, bind.GetMapId(), bind.GetPositionX(),
                 bind.GetPositionY());
        botAI->rpgInfo.ChangeToGoCamp(WorldPosition(innkeeper));
        return true;
    }
    return false;
}

std::string NewRpgBaseAction::FiveGearShort(Player* bot, std::vector<uint32>* missingReagents)
{
    if (!bot)
        return "";
    std::string out;
    // ammo: the vendor run's own line (VENDOR_LOW_AMMO) for the ammo its ranged weapon fires
    // [ammo84] any class whose ranged weapon fires ammo (a warrior / rogue tank's gun, bow or crossbow is its pull)
    if (uint32 const sub = RequiredAmmoSubClass(bot))
    {
        // [ammo90] the ammo slot holds the kind its weapon fires when its bags have it
        if (ItemTemplate const* set = sObjectMgr->GetItemTemplate(bot->GetUInt32Value(PLAYER_AMMO_ID));
            !set || set->SubClass != sub || !bot->HasItemCount(set->ItemId, 1))
        {
            uint32 fit = 0;
            auto const look = [&](Item* item)
            {
                ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
                if (!fit && proto && proto->Class == ITEM_CLASS_PROJECTILE && proto->SubClass == sub &&
                    bot->CanUseItem(proto) == EQUIP_ERR_OK)
                    fit = proto->ItemId;
            };
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                look(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
            for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
                if (Bag* container = bot->GetBagByPos(bag))
                    for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                        look(container->GetItemByPos(uint8(slot)));
            if (fit)
                bot->SetAmmo(fit);
        }
        uint32 have = 0;
        auto const count = [&](Item* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (proto && proto->Class == ITEM_CLASS_PROJECTILE && proto->SubClass == sub)
                have += item->GetCount();
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            count(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = bot->GetBagByPos(bag))
                for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                    count(container->GetItemByPos(uint8(slot)));
        if (have < VENDOR_LOW_AMMO)
            out = "ammo " + std::to_string(have);
    }
    // reagents: each one its known spells use that a vendor sells (GuildGear::VendorsOf) and it holds none of
    std::set<uint32> seen;
    for (auto const& [spellId, spell] : bot->GetSpellMap())
    {
        if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        for (uint8 r = 0; info && r < MAX_SPELL_REAGENTS; ++r)
        {
            if (info->Reagent[r] <= 0 || !seen.insert(uint32(info->Reagent[r])).second)
                continue;
            uint32 const item = uint32(info->Reagent[r]);
            std::vector<GuildGear::VendorSpawn> const* vendors = GuildGear::VendorsOf(item);
            if (!vendors || vendors->empty() || bot->GetItemCount(item, false))
                continue;
            if (missingReagents)
                missingReagents->push_back(item);
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            out += (out.empty() ? "" : ", ") + std::string("reagent ") + (proto ? proto->Name1 : std::to_string(item));
        }
    }
    return out;
}

bool NewRpgBaseAction::StartVendorRun()
{
    SlowScope slow("vendor run look", &bot->GetName());  // [perf26]
    NewRpgInfo& info = botAI->rpgInfo;
    // [regress10] the global leader's REPAIR task (single decider): its call, read once
    GuildCoordinator::RepairCall const call = sGuildCoordinator.Repair(bot);
    if (!call.need)
        info.repairVisited = false;
    // URGENT: now; CONVENIENT: in the repairer's town (the leader's look) or with the repairer in sight (its town
    // stop reached before that look)
    bool const repairGo = call.ready && (call.urgent || call.inTown ||
                                         (call.at.GetMapId() == bot->GetMapId() &&
                                          bot->GetExactDist(call.at) <= sPlayerbotAIConfig.SightDistance));
    // [guild15] a vendor run another state took over (a death's recovery, a quest, a guild order) is over: it kept
    // vendorRunEntry set and no run started again (b308: Farret L10 with 6 of 7 items broken logged "vendor: repair"
    // 750 times without walking there; 12 of 50 bots ended the run with broken gear)
    if (info.vendorRunEntry && info.GetStatus() != RPG_GO_CAMP && info.GetStatus() != RPG_WANDER_NPC)
    {
        // [regress10] a run for the leader's REPAIR task is not over: only a task above it took the bot away (the
        // death's recovery, a fight, an URGENT order), and it resumes now (b345 Ginde L5: "sell junk + repair"
        // dropped after a death and planned again only after a money gain - both weapons broke, two more deaths)
        if (repairGo && info.vendorRunReason.find("repair") != std::string::npos)
        {
            LOG_INFO("playerbots", "[tasks] {} REPAIR resumes: its vendor run ({}) was taken over by status {}",
                     bot->GetName(), info.vendorRunReason, uint32(info.GetStatus()));
            info.vendorRunEntry = 0;
            info.repairVisited = false;
        }
        else
        {
            LOG_INFO("playerbots", "[New RPG] {} vendor run ({}) dropped by another state: planned again after its "
                     "next loot or money gain", bot->GetName(), info.vendorRunReason);
            info.vendorRunEntry = 0;
            // counted as a visit: tried again once the bags or the purse grow (b311: Hobliir's unreachable Exodar
            // vendor 29 yd off was walked to, given up and chosen again every few seconds)
            info.vendorVisited = true;
            info.vendorVisitStocked = false;  // [eco193]
            RefuseRestockVendor(bot, info, info.vendorRunLast,  // [eco205]
                                AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::consumables)));
            info.vendorVisitMap = bot->GetMapId();
            info.vendorMissed = true;  // [vendor23]
            info.vendorVisitBagUsed = AI_VALUE(uint8, "bag space");
            info.vendorVisitMoney = bot->GetMoney();
        }
    }
    if (info.vendorRunEntry || bot->IsInCombat() || !bot->IsAlive())
        return false;

    uint8 const bagUsed = AI_VALUE(uint8, "bag space");
    // [coop] a co-op party is a task above the convenient ones: its sell run waits while a bag slot is free
    // [vendor15] a sell run when its bags hold no more than the slots its log's open item objectives still need (an
    // item of each it holds none of yet takes a slot; with none, the bag is full: loot is lost from the next kill
    // on). Before, the generic "should sell" sent it at 80 % used: b355 128 vendor visits, 62 of the 148 runs at 81 %
    // (a sale of greys left the kept items near 80 %, the next few loots crossed it again), VENDOR 11 % of the time
    // (the item objectives bound the need: with more slots free than they could take, no item count is read)
    uint32 const freeSlots = bot->GetFreeInventorySpace();
    // [vendor21] the slots its loot needs now: the item objectives of the quest it works (its DO_QUEST), not of every
    // log quest - those are looted one quest at a time, a slot each when it gets there. b368: 274 vendor trips (b355:
    // 148), 137 of them "5 bag slots free, its open item objectives need 5": a run at 5 free slots that freed none
    // (nothing for sale), again and again; Thamesh 902 s, Bhogagnuth 887 s at the vendor
    std::vector<uint32> objectiveItems;
    uint32 const working = std::holds_alternative<NewRpgInfo::DoQuest>(info.data)
                               ? std::get<NewRpgInfo::DoQuest>(info.data).questId
                               : 0;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (uint32 const questId = bot->GetQuestSlotQuestId(slot); questId && questId == working)
            if (bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))

                    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                        if (quest->RequiredItemId[i] && quest->RequiredItemCount[i])
                            objectiveItems.push_back(quest->RequiredItemId[i]);
    uint32 slotsNeeded = 0;
    if (freeSlots <= objectiveItems.size())
        for (uint32 item : objectiveItems)
            if (!bot->GetItemCount(item))
                ++slotsNeeded;
    bool const bagsNeeded = freeSlots <= slotsNeeded;

    bool const sell = bagsNeeded && AI_VALUE(bool, "can sell") &&
                      (!info.vendorVisited || bagUsed > info.vendorVisitBagUsed ||
                       // [vendor23] ... unless its last run missed its vendor (gone from its spawn, out of walk):
                       // then a bag or money gain sends it again (b372 Ravelandy: 35 runs to one spot)
                       (sGuildCoordinator.HasMarkedSale(bot) &&
                        (!info.vendorMissed || bot->GetMoney() > info.vendorVisitMoney))) &&  // [vendor20] the leader's marked sale is due

                      (bagUsed >= 100 || !NewRpgCoop::InParty(bot->GetGUID().GetCounter()));

    uint32 ammoSubClass = 0;
    // [ammo84] ... and its vendor run buys it as a hunter's does
    if (RequiredAmmoSubClass(bot))
    {
        uint32 const subClass = RequiredAmmoSubClass(bot);
        if (subClass && AI_VALUE2(uint32, "item count", "ammo") < VENDOR_LOW_AMMO && bot->GetMoney() > 0 &&
            (!info.vendorVisited || bot->GetMoney() > info.vendorVisitMoney))
            ammoSubClass = subClass;
    }
    // [guild13] a broken item (0 durability: a hunter's bow shoots no more) is repaired, as a player does - at a vendor
    // that repairs, paid from its own money; [guild15] ... and before the next death breaks it
    // [regress10] the global leader decides (its REPAIR task: URGENT now, CONVENIENT in the repairer's town, the
    // money for it checked against the core's repair cost); one try per money gain after a visit that left the need
    // [broken40] user 2026-11-09: the run goes even when its money pays only a part - the repairer buys its sale
    // first and "repair" pays what it can, weapons first (RepairAllAction); before, a bot short of the full cost made
    // no run at all: b388 Peny / Irrua L14 at 0 durability with full bags stood IDLE all run, 5 deaths each
    bool repair = repairGo && (bot->GetMoney() >= call.cost || bot->GetMoney() > 0 || sell) &&
                  (!info.repairVisited || bot->GetMoney() > info.repairVisitMoney);
    // [pace25] coordinator 2026-11-09: out of food (or, using mana, out of drink) it buys some, as a player restocks:
    // b373's bots "sat to regenerate: nothing to eat or drink" 7263 times (Orton 1336: mana 5 % to 17 % over minutes)
    // - the 17,623 s they stood in walking states were mostly that. RestockFoodAction buys only at a vendor within 25
    // yd; nothing sent them to one. One run per money gain after a visit, as the ammo run
    bool const usesMana = bot->GetMaxPower(POWER_MANA) > 0;
    // [supply27] a five member before its five goes in restocks below the restock line, not only when out
    bool const fiveSupply = sGuildCoordinator.SupplyWanted(bot);
    uint32 const line = fiveSupply ? RESTOCK_FOOD_MIN : 1;
    // [eco206] ... the drinks its run needs ([dun194]) for a mana user of a gathering five
    uint32 const fiveDrinks = fiveSupply && usesMana ? sGuildCoordinator.FiveDrinkTarget(bot) : 0;
    uint32 const drinkLine = fiveDrinks ? fiveDrinks : line;
    bool const noFood = CountBagFood(bot, RESTOCK_SPELL_CATEGORY_FOOD) < line;
    bool const noDrink = usesMana && CountBagFood(bot, RESTOCK_SPELL_CATEGORY_DRINK) < drinkLine;
    // [five43] a five member's supply run before its five goes in passes the "one run per money gain" gate once:
    // its last visit (a food vendor without drink, a sale) left it short and the five waited for a run that never
    // started (b390: "[supply27] Orenem / Thokhen supply run could not be made (no vendor run started)", the five
    // went into RFC short of drink and food)
    if (!fiveSupply)
        info.fiveRestockTried = false;
    bool const fiveRetry = fiveSupply && !info.fiveRestockTried;
    // [eco193] coordinator 2026-11-11: the money-gain gate holds a restock only after a visit or look that could not
    // stock it (a vendor missed, none found, the visit left it out) - and only on that map. After a visit that
    // stocked it, running dry again is a new need: b428 198 "[supply34] out of drink: no money gained since its last
    // vendor visit" (Alnia 46, Pherua 10562c and Orenem resting dry 810 / 784 times) - they had bought their drink
    // there, drunk it, and the gate waited for a coin that a healer resting dry does not loot
    // [eco201] coordinator 2026-11-11: ... and after one, the restock waits until the money it may spend covers the
    // cheapest food / drink it lacked there, not for any coin ([eco205]: that vendor only): b432 each loot gain sent
    // Ainne / Namwe / Galiver back to a vendor whose drink their budget could not pay
    uint32 const spendable =
        (noFood || noDrink) ? AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::consumables)) : 0;
    // [eco202] what the consumables budget keeps back (BudgetValues: saveMoneyFor), for the [supply34] line
    auto const reserveText = [&]()
    {
        auto const need = [&](NeedMoneyFor what) { return AI_VALUE2(uint32, "money needed for", uint32(what)); };
        return Acore::StringFormat("kept back: repair {}c (now {}c + one death), ammo {}c, class spells {}c, guild "
                                   "{}c; after the restock: travel {}c [eco203]",
                                   need(NeedMoneyFor::repair), AI_VALUE(uint32, "repair cost"),
                                   need(NeedMoneyFor::ammo), need(NeedMoneyFor::spells), need(NeedMoneyFor::guild),
                                   need(NeedMoneyFor::travel));
    };
    // [eco205] coordinator 2026-11-11: no map-wide gate after a visit - the vendor that could not stock it is skipped
    // by the vendor choice (restockRefused), the others stay open. b433: Namwe (13439c), Ainne (may spend 4850c) held
    // by "[eco193] its last vendor visit on this map could not stock it and no money was gained since". Only a look
    // that found no vendor it could pay waits, for the budget that may find one (restockLookPrice)
    bool const lookCovers = !info.restockLookPrice || info.restockLookMap != bot->GetMapId() ||
                            spendable >= info.restockLookPrice;
    bool const restock = (noFood || noDrink) && bot->GetMoney() > 0 && (fiveRetry || lookCovers);
    if (restock && fiveRetry)
        info.fiveRestockTried = true;
    // [supply34] why a bot out of food / drink makes no run: one line per money value it has (b386: 1.10 h sat
    // "nothing to eat or drink", Bierya 193, Orenem 135, Jurdogs 110 - and no "goes to vendor" line of them)
    auto const noRunWhy = [&](char const* why)
    {
        if (!(noFood || noDrink))
            return;
        static std::mutex supplyLogLock;
        static std::unordered_map<uint32, uint32> supplyLogged;  // bot -> money logged with
        {
            std::lock_guard<std::mutex> guard(supplyLogLock);
            uint32& last = supplyLogged[bot->GetGUID().GetCounter()];
            if (last == bot->GetMoney() + 1)
                return;
            last = bot->GetMoney() + 1;
        }
        LOG_INFO("playerbots", "[supply34] {} L{} out of {}: no vendor run - {} (money {}c, last visit at {}c)",
                 bot->GetName(), bot->GetLevel(), noFood && noDrink ? "food and drink" : noFood ? "food" : "drink",
                 why, bot->GetMoney(), info.vendorVisited ? info.vendorVisitMoney : 0);
    };
    if ((noFood || noDrink) && !restock)
    {
        std::string const why =
            bot->GetMoney() == 0 ? std::string("no money")
            : Acore::StringFormat("its last look found no vendor on this map it could pay: it may spend {}c of its {}c "
                                  "({}), it waits for {}c [eco205]",
                                  spendable, bot->GetMoney(), reserveText(), info.restockLookPrice);
        noRunWhy(why.c_str());
    }
    // [supply27] a five member before its five goes in buys the vendor-sold reagents its spells use and has none of
    std::vector<uint32> reagents;
    if (fiveSupply && bot->GetMoney() > 0 && (!info.vendorVisited || bot->GetMoney() > info.vendorVisitMoney))
        FiveGearShort(bot, &reagents);
    bool const reagentRun = !reagents.empty();
    if (!sell && !ammoSubClass && !repair && !restock && !reagentRun)
        return false;

    float dist = 0.0f;
    restockWanted = restock && !sell && !ammoSubClass && !repair ? (noFood ? 1u : 0u) | (noDrink ? 2u : 0u) : 0u;
    restockBudget = spendable;  // [eco201]
    restockRefusedBy = &info;   // [eco205]
    uint32 const lookedFor = restockWanted;
    reagentsWanted = reagentRun && !sell && !ammoSubClass && !repair && !restock ? &reagents : nullptr;
    VendorSpawn const* vendor = repair ? NearestRepairer(bot, dist) : NearestVendor(bot, ammoSubClass, dist);
    restockWanted = 0;
    restockRefusedBy = nullptr;
    reagentsWanted = nullptr;
    if (repair && !vendor)
    {
        repair = false;
        if (!sell && !ammoSubClass)
            return false;
        vendor = NearestVendor(bot, ammoSubClass, dist);
    }
    if (ammoSubClass && (!vendor || bot->GetMoney() < AmmoPrice(bot, vendor->entry, ammoSubClass)))
    {
        ammoSubClass = 0;
        if (!sell)
            return false;
        vendor = NearestVendor(bot, 0, dist);
    }
    if (!vendor)
    {
        if (restock)
            noRunWhy(Acore::StringFormat("no vendor of its faction sells it food / drink of its level it can pay on a "
                                         "walk it takes: it may spend {}c of its {}c ({}) [eco202]",
                                         spendable, bot->GetMoney(), reserveText())
                         .c_str());
        // [supply26] no vendor sells it a way it walks: looked again after its next money gain (the look is a scan of
        // the map's vendors and their trips, not for every tick of its rest)
        info.vendorVisited = true;
        info.vendorVisitStocked = false;  // [eco193]
        info.vendorVisitMap = bot->GetMapId();
        // [eco201] ... once its budget covers the cheapest of it on its map and is more than it is now (a vendor it
        // can pay may then be in reach; the same budget finds the same vendors)
        if (lookedFor)
        {
            info.restockLookPrice = std::max(CheapestRestock(bot, lookedFor), spendable + 1);
            info.restockLookMap = bot->GetMapId();
        }
        info.vendorVisitBagUsed = bagUsed;
        info.vendorVisitMoney = bot->GetMoney();
        return false;
    }

    std::string reason = sell ? "sell junk" : "";
    if (repair)  // [guild13]
        reason += std::string(reason.empty() ? "" : " + ") + "repair";
    if (ammoSubClass)
        reason += std::string(!reason.empty() ? " + " : "") +
                  (ammoSubClass == ITEM_SUBCLASS_ARROW ? "buy arrows" : "buy bullets");
    if (reagentRun)  // [supply27]
        reason += std::string(!reason.empty() ? " + " : "") + "buy reagents";
    if (restock)  // [pace25]
        reason += std::string(!reason.empty() ? " + " : "") +
                  (noFood && noDrink ? "buy food and drink" : noFood ? "buy food" : "buy drink");
    info.ChangeToGoCamp(WorldPosition(vendor->mapId, vendor->x, vendor->y, vendor->z));
    info.vendorRunEntry = vendor->entry;
    info.vendorRunLast = vendor->entry;  // [eco205]
    NoteVendorRunSpawn(bot, vendor->spawnId);  // [vend20] the spawn this errand walks to
    info.vendorRunReason = reason;
    CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(vendor->entry);
    QuestStats::TownStart(bot, "vendor run");  // [log28]
    LOG_INFO("playerbots", "[New RPG] {} goes to vendor {} ({:.0f} yd): {} (bags {}% full, {} ammo, {}c)",
             bot->GetName(), tmpl ? tmpl->Name : "?", dist, reason, bagUsed,
             AI_VALUE2(uint32, "item count", "ammo"), bot->GetMoney());
    if (sell)
        LOG_INFO("playerbots", "[vendor15] {} L{} sells: {} bag slots free, its open item objectives need {}",
                 bot->GetName(), bot->GetLevel(), freeSlots, slotsNeeded);

    return true;
}

void TellRpgStatusAction::WhisperStatusChange(Player* owner, std::string const& statusName)
{
    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        RPG_STATUS_CHANGED_KEY, RPG_STATUS_CHANGED_DEFAULT,
        {{"%status", statusName}});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
}

bool TellRpgStatusAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    if (text.empty())
    {
        std::string out = botAI->rpgInfo.ToString();
        bot->Whisper(out.c_str(), LANG_UNIVERSAL, owner);
        return true;
    }

    Player* master = botAI->GetMaster();
    bool isMaster = master && master->GetGUID() == owner->GetGUID();
    bool isGM = owner->GetSession() && owner->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
    if (!isMaster && !isGM)
    {
        std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "rpg_debug_permission_error",
            "Only your master or a GM can change my rpg status.", {});
        bot->Whisper(msg, LANG_UNIVERSAL, owner);
        return false;
    }

    std::string name = text;
    uint32 questId = 0;
    static std::string const doQuestPrefix = "do quest ";
    size_t doQuestPos = text.find(doQuestPrefix);
    if (doQuestPos != std::string::npos)
    {
        name = "do quest";
        std::string idStr = text.substr(doQuestPos + doQuestPrefix.length());
        try
        {
            questId = static_cast<uint32>(std::stoul(idStr));
        }
        catch (std::exception const&)
        {
            questId = 0;
        }
    }

    NewRpgStatus status = NewRpgInfo::StatusFromString(name);
    NewRpgInfo& info = botAI->rpgInfo;

    if (status == RPG_IDLE)
    {
        info.ChangeToIdle("its owner set it idle");
        WhisperStatusChange(owner, "IDLE");
        return true;
    }
    else if (status == RPG_REST)
    {
        info.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        WhisperStatusChange(owner, "REST");
        return true;
    }
    else if (status == RPG_WANDER_RANDOM)
    {
        info.ChangeToWanderRandom();
        WhisperStatusChange(owner, "WANDER_RANDOM");
        return true;
    }
    else if (status == RPG_WANDER_NPC)
    {
        info.ChangeToWanderNpc();
        WhisperStatusChange(owner, "WANDER_NPC");
        return true;
    }
    else if (status == RPG_GO_GRIND)
    {
        WorldPosition pos = SelectRandomGrindPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_grind_pos_error", "No grind position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoGrind(pos);
        WhisperStatusChange(owner, "GO_GRIND");
        return true;
    }
    else if (status == RPG_GO_CAMP)
    {
        WorldPosition pos = SelectRandomCampPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_camp_pos_error", "No camp position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoCamp(pos);
        WhisperStatusChange(owner, "GO_CAMP");
        return true;
    }
    else if (status == RPG_TRAVEL_FLIGHT)
    {
        uint32 flightMasterEntry = 0;
        WorldPosition flightMasterPos;
        std::vector<uint32> path;
        if (!SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_flight_path_error", "No flight path available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, std::move(path));
        WhisperStatusChange(owner, "TRAVEL_FLIGHT");
        return true;
    }
    else if (status == RPG_OUTDOOR_PVP)
    {
        info.ChangeToOutdoorPvp();
        WhisperStatusChange(owner, "OUTDOOR_PVP");
        return true;
    }
    else if (status == RPG_DO_GATHER)
    {
        if (!botAI->HasSkill(SKILL_HERBALISM) && !botAI->HasSkill(SKILL_MINING))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_gather_no_profession_error", "I have neither Herbalism nor Mining, so I can't gather.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoGather();
        WhisperStatusChange(owner, "DO_GATHER");
        return true;
    }
    else if (status == RPG_DO_QUEST)
    {
        WorldPosition giverPos;
        if (!questId)
            questId = ChooseBestQuest(giverPos);

        if (!questId)
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_quest_error", "No quest available; use 'do quest <id>'.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatus questStatus = bot->GetQuestStatus(questId);
        bool const toPickUp = questStatus == QUEST_STATUS_NONE && giverPos != WorldPosition();
        if (!quest || (questStatus != QUEST_STATUS_INCOMPLETE && questStatus != QUEST_STATUS_COMPLETE && !toPickUp))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_invalid_quest_error", "Invalid quest %quest_id",
                {{"%quest_id", std::to_string(questId)}});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoQuest(questId, quest, giverPos);
        WhisperStatusChange(owner, "DO_QUEST " + std::to_string(questId));
        return true;
    }

    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "rpg_unknown_status_error",
        "Unknown rpg status. Options: idle, rest, wander random, wander npc, "
        "go grind, go camp, do quest [<id>], travel flight, outdoor pvp, do gather.", {});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
    return false;
}

bool StartRpgDoQuestAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(owner);
    uint32 questId = ch.extractQuestId(text);
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        botAI->rpgInfo.ChangeToDoQuest(questId, quest);
        bot->Whisper("Start to do quest " + std::to_string(questId), LANG_UNIVERSAL, owner);
        return true;
    }
    bot->Whisper("Invalid quest " + text, LANG_UNIVERSAL, owner);
    return false;
}

namespace
{
    // [qeff31] the quest-efficiency triage's lines, one per event with t=: the worked quest (focus) on / off with its
    // reason, a wait at the objective (respawn, no target) start / end, the arrival at the objective area, a quest
    // object's use start / end. Log state only, no decision reads it
    struct FocusLog
    {
        uint32 quest = 0;
        uint32 waitQuest = 0;
        std::string waitWhy;
        uint32 waitMs = 0;
        uint32 useQuest = 0;
        uint32 useEntry = 0;
        uint32 useMs = 0;
    };
    std::mutex focusLogLock;
    std::unordered_map<uint32, FocusLog> focusLogs;

    void QuestWaitEnd(Player* bot, FocusLog& f, char const* why)
    {
        if (!f.waitQuest)
            return;
        LOG_INFO("playerbots", "[quest] wait {} q{} ends after {:.0f} s: {} t={}", bot->GetName(), f.waitQuest,
                 float(getMSTimeDiff(f.waitMs, getMSTime())) / float(IN_MILLISECONDS), why, QuestStats::NowMs());
        f.waitQuest = 0;
        f.waitWhy.clear();
    }

    void QuestUseEnd(Player* bot, FocusLog& f, char const* why)
    {
        if (!f.useQuest)
            return;
        LOG_INFO("playerbots", "[quest] use {} q{} object {} ends after {:.1f} s: {} t={}", bot->GetName(), f.useQuest,
                 f.useEntry, float(getMSTimeDiff(f.useMs, getMSTime())) / float(IN_MILLISECONDS), why,
                 QuestStats::NowMs());
        f.useQuest = 0;
        f.useEntry = 0;
    }

    // the worked quest now: DO_QUEST's quest unless a detour holds it (a vendor run, a group wait, a meet walk, a
    // co-op / help party it follows); `why` the detour or the status
    uint32 FocusNow(Player* bot, NewRpgInfo& info, std::string& why)
    {
        static char const* const names[] = {"IDLE", "GO_GRIND", "GO_CAMP", "WANDER_RANDOM", "WANDER_NPC",
                                             "DO_QUEST", "TRAVEL_FLIGHT", "REST", "OUTDOOR_PVP", "DO_GATHER",
                                             "DO_PROFESSION"};
        NewRpgStatus const status = info.GetStatus();
        auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data);
        if (!bot->IsAlive())
            why = "dead";
        else if (status != RPG_DO_QUEST || !quest)
            why = std::string("status ") + (status < RPG_STATUS_END ? names[status] : "?");
        else if (info.vendorRunEntry)
            why = "vendor run: " + info.vendorRunReason;
        else if (info.groupWaiting)
            why = "group wait";
        // [wait20] no "walks to meet a member" here any more: that walk is gone and so is the flag it read
        else if (NewRpgCoop::IsMember(bot))
            why = "follows its co-op / help party";
        else
        {
            why = quest->giverPos != WorldPosition() ? "to its giver" : "DO_QUEST";
            return quest->questId;
        }
        return 0;
    }
}

void NewRpgBaseAction::NoteQuestWait(Player* bot, uint32 questId, std::string const& why)
{
    std::lock_guard<std::mutex> guard(focusLogLock);
    FocusLog& f = focusLogs[bot->GetGUID().GetCounter()];
    if (!questId)
    {
        QuestWaitEnd(bot, f, why.c_str());
        return;
    }
    if (f.waitQuest == questId && f.waitWhy == why)
        return;
    QuestWaitEnd(bot, f, "another wait");
    f.waitQuest = questId;
    f.waitWhy = why;
    f.waitMs = getMSTime();
    LOG_INFO("playerbots", "[quest] wait {} q{}: {} t={}", bot->GetName(), questId, why, QuestStats::NowMs());
}

void NewRpgBaseAction::NoteQuestUse(Player* bot, uint32 questId, uint32 entry)
{
    std::lock_guard<std::mutex> guard(focusLogLock);
    FocusLog& f = focusLogs[bot->GetGUID().GetCounter()];
    if (!questId)
    {
        // the use is over once no cast and no loot window hold it (a log reading, no decision)
        if (f.useQuest && !bot->IsNonMeleeSpellCast(false) && !bot->GetLootGUID() &&
            getMSTimeDiff(f.useMs, getMSTime()) >= IN_MILLISECONDS)
            QuestUseEnd(bot, f, "done");
        return;
    }
    QuestUseEnd(bot, f, "another use");
    f.useQuest = questId;
    f.useEntry = entry;
    f.useMs = getMSTime();
    LOG_INFO("playerbots", "[quest] use {} q{} object {} starts t={}", bot->GetName(), questId, entry,
             QuestStats::NowMs());
}

// [group115] one leave per member and group: the leave is queued to the world thread, the member still shows the group
// until it runs
static bool LeaveOnce(Player* bot, Group* group)
{
    static std::mutex lock;
    static std::unordered_map<uint32, ObjectGuid> asked;  // member -> the group it asked to leave
    std::lock_guard<std::mutex> guard(lock);
    ObjectGuid& last = asked[bot->GetGUID().GetCounter()];
    if (last == group->GetGUID())
        return false;
    last = group->GetGUID();
    return true;
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus status = info.GetStatus();
    // [qeff31] the worked quest's change: one off line and one on line, with the reason
    {
        std::string why;
        uint32 const focus = FocusNow(bot, info, why);
        std::lock_guard<std::mutex> guard(focusLogLock);
        FocusLog& f = focusLogs[bot->GetGUID().GetCounter()];
        if (focus != f.quest)
        {
            if (f.quest)
            {
                QuestWaitEnd(bot, f, "off the quest");
                QuestUseEnd(bot, f, "off the quest");
                LOG_INFO("playerbots", "[focus] {} off q{}: {} t={}", bot->GetName(), f.quest,
                         focus ? "q" + std::to_string(focus) + " worked instead" : why, QuestStats::NowMs());
            }
            if (focus)
                LOG_INFO("playerbots", "[focus] {} on q{}: {} t={}", bot->GetName(), focus, why, QuestStats::NowMs());
            f.quest = focus;
        }
    }
    // [guild90] questing counters (yards walked, the active quest's progress) for tools/guild_stats.py
    {
        auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data);
        // [budget12] the bucket its time since the last tick goes to (metric only, no decision reads it)
        char const* bucket = "IDLE";
        bool const skin = (std::holds_alternative<NewRpgInfo::GoGrind>(info.data) &&
                           std::get<NewRpgInfo::GoGrind>(info.data).forSkin) ||
                          (std::holds_alternative<NewRpgInfo::WanderRandom>(info.data) &&
                           std::get<NewRpgInfo::WanderRandom>(info.data).forSkin);
        if (info.groupWaiting)
            bucket = "GROUP_WAIT";
        else if (skin)
            bucket = "SKIN";
        else if (info.vendorRunEntry)
            bucket = "VENDOR";
        else
            switch (status)
            {
                case RPG_GO_GRIND: bucket = "GO_GRIND"; break;
                case RPG_GO_CAMP: bucket = "GO_CAMP"; break;
                case RPG_WANDER_RANDOM: bucket = "GRIND"; break;
                case RPG_WANDER_NPC: bucket = "NPC"; break;
                case RPG_DO_QUEST: bucket = "QUEST"; break;
                case RPG_TRAVEL_FLIGHT: bucket = "FLIGHT"; break;
                case RPG_REST: bucket = "REST"; break;
                case RPG_OUTDOOR_PVP: bucket = "PVP"; break;
                case RPG_DO_GATHER: bucket = "GATHER"; break;
                case RPG_DO_PROFESSION: bucket = "PROFESSION"; break;
                default: break;
            }
        QuestStats::OnRpgTick(bot, quest ? quest->questId : 0, bucket);
    }

    // [guild7] one line per status change for the guild stats (tools/guild_stats.py "activity")
    // [status40] ... and one line per reason behind an IDLE: the status alone says nothing, and the wait it names
    // changes while the status does not (a five forming, a Hearthstone cooldown, a postponed plan)
    bool const idleReasonChanged = status == RPG_IDLE && info.loggedIdleReason != info.idleReason;
    if (info.loggedStatus != uint8(status) || idleReasonChanged)
    {
        static char const* const names[] = {"IDLE", "GO_GRIND", "GO_CAMP", "WANDER_RANDOM", "WANDER_NPC",
                                             "DO_QUEST", "TRAVEL_FLIGHT", "REST", "OUTDOOR_PVP", "DO_GATHER", "DO_PROFESSION"};
        // [status40] IDLE(<what it waits for>), [status41] NO_DECISION(<what was being decided>): never a bare word
        // [feed20] composed first, then handed to the coordinator, then logged: see below
        std::string word = status < RPG_STATUS_END ? names[status] : "?";
        if (status == RPG_IDLE)
            word = std::string(info.idleUndecided ? "NO_DECISION" : "IDLE") + "(" +
                   (info.idleReason.empty() ? "no reason recorded" : info.idleReason) + ")";
        std::string detail;
        if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data))
            detail = " q" + std::to_string(quest->questId) + (quest->quest ? " " + quest->quest->GetTitle() : "");
        else if (auto const* grind = std::get_if<NewRpgInfo::GoGrind>(&info.data))
            detail = " " + std::to_string(uint32(bot->GetExactDist(grind->pos))) + " yd";
        else if (auto const* camp = std::get_if<NewRpgInfo::GoCamp>(&info.data))
            detail = " " + std::to_string(uint32(bot->GetExactDist(camp->pos))) + " yd";
        if (info.vendorRunEntry)
            detail += " vendor: " + info.vendorRunReason;
        // [feed20] the line is composed BEFORE the status change is handed over, because the LOOP alert OnRpgStatus
        // raises prints this very text as its reason= - the words the triage reads out of the log beside an alert.
        // The composition above only reads info and the bot's position, so moving it ahead changes no behaviour.
        sGuildCoordinator.OnRpgLine(bot, word + detail);
        if (info.loggedStatus != uint8(status))
        {
            info.loggedStatus = uint8(status);
            sGuildCoordinator.OnRpgStatus(bot, uint8(status));  // [guild30] LOOP alert
            if (status != RPG_DO_QUEST)
                ClaimSpot(bot, 0, WorldPosition());  // [guild90] off questing: its camp is free for other bots
        }
        info.loggedIdleReason = info.idleReason;  // [status40]
        LOG_INFO("playerbots", "[rpg] {} L{} {}{} t={}", bot->GetName(), bot->GetLevel(), word, detail,
                 QuestStats::NowMs());  // [log28] t=
        if (status == RPG_DO_PROFESSION)
            QuestStats::TownStart(bot, "DO_PROFESSION");  // [log28]
    }

    // [regress10] the first look after its login: a login spot inside idle mobs' reach is left first (b347 Sehdrurmund
    // L6, Ganna L10 at Iceflow Lake and Gilduck L10 on Shimmer Ridge logged in where b345 had left them and died to
    // packs of 3-5 within seconds)
    if (!info.loginChecked)
    {
        info.loginChecked = true;
        if (LoginEscape())
            return true;
    }
    // [login26] the server announced its shutdown (World::IsShuttingDown, the countdown every player sees): a bot out
    // of a fight steps out of every idle mob's reach and stays there, as a player logs out away from a camp - its
    // session ends there and its next login is no login inside a camp (b375: 6 deaths "logged in inside the camp",
    // attacked before its first look: Irrua at 40 % in Amberweb Pass, Mytaeleas among the Blackwood)
    if (sWorld->IsShuttingDown() && !bot->IsInCombat() &&
        (SoloSafety::Applies(bot) || NewRpgCoop::JudgesPulls(bot)))  // [group38] a group's leader leads it out
    {
        if (bot->isMoving())
            return true;
        std::string who;
        std::vector<G3D::Vector3> spots;
        // [login67] the reaches of its next login: the dead and fighting mobs at their homes too (the restart
        // respawns them). b399 Rumanatol L16 rested at shutdown 4 yd from the home of the Veridian Broodling L18 it had
        // just killed; at b400's login the Broodling stood there again, attacked before its first look, and killed it
        if (!SoloSafety::StandsInReach(bot, who, spots, true))
            return HoldPosition(3 * IN_MILLISECONDS);
        for (G3D::Vector3 const& at : spots)
        {
            Leader::RouteResult const route = Leader::Route(bot, Position(at.x, at.y, at.z));
            if (!route.found || !route.complete || route.throughRefused)  // [hold58] no walk through a wall
                continue;
            // [login67] a bot sitting to eat or drink stands up for it (as RestEscape does): b399 Rumanatol L16 sat
            // eating at the shutdown 4 yd from a respawn
            if (bot->IsSitState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);
            if (!MoveTo(bot->GetMapId(), at.x, at.y, at.z, false, false, false, true))
                continue;
            LOG_INFO("playerbots", "[login26] {} L{} the server shuts down in {} s: steps out of the reach of {} to "
                     "({:.0f},{:.0f})",
                     bot->GetName(), bot->GetLevel(), sWorld->GetShutDownTimeLeft(), who, at.x, at.y);
            return true;
        }
    }
    // [red63] inside an area red for it with no walk out: out by its Hearthstone
    if (RedAreaEscape())
        return true;
    // [deaths13] hurt, out of a fight, in a wanderer's reach: the rest is taken out of every reach
    if (RestEscape())
        return true;
    // [cap65] standing in the other faction's capital (its guards L75 everywhere; cleft62 covers only a login there):
    // out by its Hearthstone, as a player leaves - b400 Feette L19 (Alliance) walked from Durotar into Orgrimmar's
    // north and stood walled by Orgrimmar Grunts L75 for the whole run, 31 dead-end postpones
    if (!bot->IsInCombat() && bot->IsAlive() && SoloSafety::Applies(bot) && !bot->GetMap()->Instanceable())
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
        {
            uint32 const own = bot->GetTeamId() == TEAM_ALLIANCE ? AREATEAM_ALLY : AREATEAM_HORDE;
            if ((zone->flags & AREA_FLAG_CAPITAL) && zone->team && zone->team != own &&
                zone->team != AREATEAM_ANY &&
                HearthOut(std::string("[cap65] stands in the enemy capital ") + zone->area_name[0]))
                return true;
        }
    // [deaths20] an IDLE bot (between its tasks: a fight's end, a loot, a hand-in) standing in an idle hostile's reach
    // steps out of every reach before it plans, as a hurt one does: its next walk starts from there. b367: of 9
    // "attacked, no judged pull" deaths, 5 had the attacker idle 5-6 yd from its home at 1-5 yd from the bot standing
    // (Ainne, Tuihgrelfim, Thyllyn x2 by the Defias and Blackwood camps) - the bot stood in its reach
    // [rest64] ... a co-op / help party's member too (it stands where the party fought: the same reaches)
    // [reach66] ... and in any status (a quest's spot, a grind, a camp walk) when it stands in the reach of the mob its
    // judge just refused: b400 Kylanor L14 (DO_QUEST q?, Elwynn murlocs) landed inside two murloc reaches off a failed
    // walk, refused a Murloc Lurker ("loses even at full ... fought where it stands") and stood there until a Forager
    // walked into it - the step-out ran only in IDLE
    std::string refusedWho;
    bool const anyStatus = status != RPG_IDLE && status != RPG_REST && !bot->IsInCombat() && !bot->isMoving() &&
                           SoloSafety::InRefusedReach(bot, refusedWho);
    if ((anyStatus || status == RPG_IDLE || status == RPG_REST) && !bot->IsInCombat() && !bot->isMoving() &&
        (SoloSafety::Applies(bot) || NewRpgCoop::InParty(bot->GetGUID().GetCounter())))
    {
        std::string who;
        std::vector<G3D::Vector3> spots;
        if (SoloSafety::StandsInReach(bot, who, spots))
            for (G3D::Vector3 const& at : spots)
            {
                Leader::RouteResult const route = Leader::Route(bot, Position(at.x, at.y, at.z));
                if (!route.found || !route.complete || route.throughRefused)
                    continue;
                if (!MoveTo(bot->GetMapId(), at.x, at.y, at.z, false, false, false, true))
                    continue;
                LOG_INFO("playerbots", "[deaths20] {} L{} {} within the reach of {}: steps out to ({:.0f},{:.0f}), "
                         "{:.0f} yd walk",
                         bot->GetName(), bot->GetLevel(),
                         anyStatus ? "[reach66] status " + std::to_string(status) + ", its judge refused " + refusedWho +
                                         ";"
                                   : std::string("IDLE"),
                         who, at.x, at.y, route.length);
                return true;
            }
    }


    // [deaths14] Resurrection Sickness (spell 15007, Player::SpiritHealer... ResurrectPlayer: all stats and the damage it
    // deals at a quarter for (level - CONFIG_DEATH_SICKNESS_LEVEL + 1) minutes, ten from the tenth level above it): the
    // judge's numbers hold only for the bot without it - a wanderer or a camp it walks past meets a quarter of the bot.
    // b354: Laaral L14 (3 deaths, 83-151 s after its rise) and Rumanatol L13 (87 s) rose at the Blood Watch spirit
    // healer, walked out at 139 max health and 2.7 dps and died to single Elder Brown Bears L15-16. The bot stays where
    // it rose (the spirit healer's graveyard) until the aura is gone - a game mechanic's end, not a clock of its own
    // [deaths21] ... a co-op / help party member too: b368 Peny L13 (help party of Guinudrond in Westfall) died twice
    // under the sickness, 33 s and 174 s after its spirit-healer rise at 132 max health, following its party into the
    // Riverpaw camp; its leader waits for it ("under Resurrection Sickness", NewRpgGroupNotReadyTrigger)
    // [sick37] ... before the recovery's walk to its hub and any vendor run: the hold sat after both, so a sick bot
    // walked its RECOVER walk (b388: Aestymro 12 deaths at Longshore, rising sick and walking back through the camp)
    // and its vendor runs into camps - 15 deaths under the sickness in two windows
    if (!bot->IsInCombat() && !bot->IsInFlight() && !bot->GetTransport())
    {
        constexpr uint32 SPELL_RESURRECTION_SICKNESS = 15007;

        static std::mutex sickLock;
        static std::unordered_set<uint32> sickLogged;
        uint32 const low = bot->GetGUID().GetCounter();
        if (Aura* sickness = bot->GetAura(SPELL_RESURRECTION_SICKNESS))
        {
            bool fresh = false;
            {
                std::lock_guard<std::mutex> guard(sickLock);
                fresh = sickLogged.insert(low).second;
            }
            if (fresh)
                LOG_INFO("playerbots", "[deaths14] {} L{} waits out Resurrection Sickness at ({:.0f},{:.0f}): {} s left, "
                         "max health {} - no field work until it ends",
                         bot->GetName(), bot->GetLevel(), bot->GetPositionX(), bot->GetPositionY(),
                         std::max(0, sickness->GetDuration()) / IN_MILLISECONDS, bot->GetMaxHealth());
            if (fresh)  // [idle45] the hold's own decision line: its IDLE is a sickness wait, not an unlabelled idle
                LOG_INFO("playerbots", "[rpg] {} L{} RECOVER reason: waits out Resurrection Sickness ({} s)",
                         bot->GetName(), bot->GetLevel(), std::max(0, sickness->GetDuration()) / IN_MILLISECONDS);
            if (status != RPG_IDLE)
                info.ChangeToIdle("waits out Resurrection Sickness");
            // [sick37] it waits at a safe spot: a rise in an idle mob's reach (a wanderer's area, a patrol's beat)
            // steps out of every reach first, as the login escape does
            {
                std::string who;
                std::vector<G3D::Vector3> spots;
                if (!bot->isMoving() && SoloSafety::StandsInReach(bot, who, spots))
                    for (G3D::Vector3 const& at : spots)
                    {
                        Leader::RouteResult const route = Leader::Route(bot, Position(at.x, at.y, at.z));
                        if (!route.found || !route.complete || route.throughRefused)  // [hold58]
                            continue;
                        if (!MoveTo(bot->GetMapId(), at.x, at.y, at.z, false, false, false, true))
                            continue;
                        LOG_INFO("playerbots", "[sick37] {} L{} under Resurrection Sickness in the reach of {}: steps "
                                 "out to ({:.0f},{:.0f}) to wait there",
                                 bot->GetName(), bot->GetLevel(), who, at.x, at.y);
                        SickWalk(bot, true);
                        return true;
                    }
            }
            // [sick42] ... and out of every orange / red or elite roamer's round (a wanderer's area, a patrol's path):
            // StandsInReach sees the idle mobs in sight where they stand now, a roamer away on its round is none of
            // them. b389: Bami L13 12 deaths to a Black Ravager Mastiff L25 (wanderer, aggro 30), each ~21 s after its
            // spirit-healer rise; Ganna L14 9 deaths to a Starving Buzzard L36 (patrol); [sick37] logged nothing
            if (!bot->isMoving())
            {
                G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
                std::string roamer;
                // one search per spot: a spot with no way out is not searched again until the bot has moved (the
                // search is a look's cost, no rule)
                static std::mutex sickSearchLock;
                static std::unordered_map<uint32, std::pair<int32, int32>> sickSearched;
                std::pair<int32, int32> const spotKey{int32(here.x), int32(here.y)};
                bool searchedHere = false;
                {
                    std::lock_guard<std::mutex> guard(sickSearchLock);
                    auto const found = sickSearched.find(bot->GetGUID().GetCounter());
                    searchedHere = found != sickSearched.end() && found->second == spotKey;
                }
                // [sick51] no spot out of the rounds and its Hearthstone on its cooldown (or none): it walks out to its
                // hub (the nearest ender of a quest in its log or an inn: a place of its level) by a trip it does not
                // refuse, instead of waiting in the round
                constexpr uint32 SPELL_HEARTHSTONE = 8690;
                constexpr uint32 ITEM_HEARTHSTONE = 6948;
                bool const stoneReady = bot->HasItemCount(ITEM_HEARTHSTONE, 1) && !bot->HasSpellCooldown(SPELL_HEARTHSTONE);
                auto const walkOut = [&]() -> bool
                {
                    WorldPosition const hub = NearestHub(bot);
                    if (hub == WorldPosition() || hub.GetMapId() != bot->GetMapId() ||
                        TripRefused(bot, hub.GetPositionX(), hub.GetPositionY(), hub.GetPositionZ(), "hub (sick)"))
                        return false;
                    static std::mutex walkLock;  // one line per bot and hub: a log cadence, no decision reads it
                    static std::unordered_map<uint32, std::pair<int32, int32>> walkLogged;
                    bool fresh = false;
                    {
                        std::lock_guard<std::mutex> guard(walkLock);
                        auto& last = walkLogged[bot->GetGUID().GetCounter()];
                        std::pair<int32, int32> const key{int32(hub.GetPositionX()), int32(hub.GetPositionY())};
                        fresh = last != key;
                        last = key;
                    }
                    if (fresh)
                        LOG_INFO("playerbots", "[sick51] {} L{} under Resurrection Sickness with no spot out of the "
                                 "roamers' rounds, its Hearthstone not ready: walks out to its hub ({:.0f},{:.0f}) "
                                 "{:.0f} yd",
                                 bot->GetName(), bot->GetLevel(), hub.GetPositionX(), hub.GetPositionY(),
                                 bot->GetExactDist(hub));
                    bool const walking = MoveFarTo(hub);
                    if (walking)
                        SickWalk(bot, true);
                    return walking;
                };
                if (searchedHere && !stoneReady && walkOut())
                    return true;
                // a spot searched without a way out: its Hearthstone again (its cooldown may have run out)
                if (searchedHere && HearthOut("under Resurrection Sickness in a roamer's round with no spot out of "
                                              "it: a zone above its level"))
                    return true;
                if (!searchedHere && SoloSafety::RoamerReaches(bot, here, roamer))
                {
                    constexpr uint32 DIRECTIONS = 12;  // the search's directions all round (a look's cost, no rule)
                    for (float const ring : {15.0f, 30.0f, 45.0f, 60.0f})
                        for (uint32 i = 0; i < DIRECTIONS; ++i)
                        {
                            float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
                            float x = here.x + std::cos(angle) * ring;
                            float y = here.y + std::sin(angle) * ring;
                            float z = here.z;
                            bot->UpdateAllowedPositionZ(x, y, z);
                            std::string other;
                            if (SoloSafety::RoamerReaches(bot, G3D::Vector3(x, y, z), other))
                                continue;
                            Leader::RouteResult const route = Leader::Route(bot, Position(x, y, z));
                            if (!route.found || !route.complete || route.throughRefused)
                                continue;
                            if (!MoveTo(bot->GetMapId(), x, y, z, false, false, false, true))
                                continue;
                            LOG_INFO("playerbots", "[sick42] {} L{} under Resurrection Sickness in the round of {}: "
                                     "steps out to ({:.0f},{:.0f}) to wait there",
                                     bot->GetName(), bot->GetLevel(), roamer, x, y);
                            SickWalk(bot, true);
                            return true;
                        }
                    // no spot out of the roamers' rounds near the healer's yard: the zone is above its level - it
                    // leaves by its Hearthstone (a game action to leave a zone, not a navmesh workaround)
                    {
                        std::lock_guard<std::mutex> guard(sickSearchLock);
                        sickSearched[bot->GetGUID().GetCounter()] = spotKey;  // searched without a way out
                    }
                    if (!stoneReady && walkOut())  // [sick51]
                        return true;
                    if (HearthOut("under Resurrection Sickness in the round of " + roamer +
                                  " with no spot out of it: a zone above its level"))
                    {
                        LOG_INFO("playerbots", "[sick42] {} L{} leaves by its Hearthstone: the healer's yard lies in the "
                                 "round of {}, no spot out of it",
                                 bot->GetName(), bot->GetLevel(), roamer);
                        return true;
                    }
                }
            }
            if (bot->isMoving() && SickWalking(bot))
                return true;  // [sick51] its own step / walk out goes on
            SickWalk(bot, false);
            if (bot->isMoving())
                bot->StopMoving();
            return true;
        }
        std::lock_guard<std::mutex> guard(sickLock);
        sickLogged.erase(low);
    }

    // [hold58] a walled-in bot is held: no plan from inside its walls (its help, the camp cleared, its level, its
    // Hearthstone end the hold)
    if (WalledHoldStep())
        return true;

    // [regress10] the recovery after a death comes first: before, only a far walk's MoveFarTo took it, so a quest spot
    // in reach of the corpse was fought at once (b345 Guinudrond L11 at the Silver Stream Mine)
    if (!bot->IsInCombat() && SoloSafety::HoldsPulls(bot) && SoloSafetyStep())
        return true;

    // [deaths21] a party member under Resurrection Sickness does not follow: it holds where it rose until the aura
    // ends (the hold below; its leader waits for it)
    if (NewRpgCoop::IsMember(bot) && !bot->IsInCombat() && bot->HasAura(15007))
    {
        if (bot->isMoving())
            bot->StopMoving();
        return true;
    }
    // [group115] b421: Dekorhan 11 deaths, Alnia 4 in the Bloodmyst camps (Amberweb Pass, the Foul Pool) - both in
    // Brelir's group the session restored, each playing its own New RPG plan, and as group members attacking the
    // group's target (Brelir's Webbed Creature, 335 yd off) through the camps. [group43]'s check of a restored group
    // runs in the follow action, which a member on its own plan never runs. A member playing its own plan in a group
    // no co-op / help party and no active five holds leaves that group (an honest bot, outside an instance)
    if (Group* group = bot->GetGroup();
        group && group->GetLeaderGUID() != bot->GetGUID() && GuildHonest::IsHonestBot(bot) &&
        !bot->GetMap()->Instanceable() && !bot->IsInCombat())
        if (Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
            leader && GuildHonest::IsHonestBot(leader) && !NewRpgCoop::InParty(bot->GetGUID().GetCounter()) &&
            !NewRpgCoop::InParty(leader->GetGUID().GetCounter()) &&
            !sGuildCoordinator.InSameActiveFive(bot->GetGUID().GetCounter(), leader->GetGUID().GetCounter()) &&
            LeaveOnce(bot, group) &&
            NewRpgCoop::LeaveStaleGroup(
                bot, Acore::StringFormat("[group115] a group no party or five holds while it plays its own plan "
                                         "(its leader {} {:.0f} yd)",
                                         leader->GetName(), bot->GetExactDist2d(leader))))
            return true;
    // [coop] a co-op party member goes with its party's leader; its own plan waits (NewRpgCoop.h)
    {
        NewRpgCoop::Order const coop = NewRpgCoop::Tick(botAI, bot);

        // [fly109] its party's leader flew past the walls with the party: its own flight runs (to its flight master,
        // the flight, the landing), the follow resumes after it
        bool const flying = info.GetStatus() == RPG_TRAVEL_FLIGHT || bot->IsInFlight();
        if (coop.kind == NewRpgCoop::Order::Kind::Follow && !flying)
        {
            if (TownTaskHere())  // [town12] the leader's route brought it to its own town stop
                return true;
            if (MemberHandInHere())  // [quest16] ... or to the ender of its complete quest
                return true;
            NewRpgStatus const before = info.GetStatus();
            if (coop.farWalk ? MoveFarTo(coop.pos) : Follow(coop.leader))
                return true;
            // MoveFarTo's false is also "its last move still runs" / "it cannot step now": only the facts that
            // there is no walk from here end the party - the route search found none from this spot
            // (moveFarFailed), or the walk closed a loop / was refused and gave its state up
            if (coop.farWalk && info.moveFarFailed)
                NewRpgCoop::NoWalk(bot, "its route search finds no walk from where it stands");
            else if (coop.farWalk && info.GetStatus() != before)
                NewRpgCoop::NoWalk(bot, "its walk gave up (MOVE_FAR_LOOP or a refused spot)");
            return false;
        }
        if (coop.kind == NewRpgCoop::Order::Kind::Stay)
            return TownTaskHere() || MemberHandInHere();  // [town12] [quest16] its own task where the party stands

    }

    // [guild3] full bags or low ammo: to a vendor first, as a player does (not out of a flight, rest or pvp)
    if ((status == RPG_IDLE || status == RPG_GO_GRIND || status == RPG_WANDER_RANDOM || status == RPG_DO_QUEST ||
         status == RPG_DO_GATHER || ((status == RPG_GO_CAMP || status == RPG_WANDER_NPC) && !info.vendorRunEntry)) &&
        StartVendorRun())
        return true;

    // [fish14] the Fishing Pole left in hand by a fishing task that ended without its Stop: the weapon back first
    if (NewRpgFishing::PutPoleAway(bot))
        return true;

    // [craft410] an enchanter's disenchant queue between fights, as it loots (not out of a flight, rest or pvp)
    if ((status == RPG_IDLE || status == RPG_GO_GRIND || status == RPG_WANDER_RANDOM || status == RPG_DO_QUEST ||
         status == RPG_DO_GATHER || status == RPG_GO_CAMP || status == RPG_WANDER_NPC) &&
        sGuildCoordinator.DisenchantNow(bot, botAI))
        return true;

    // [inv380] ... and in the same slot the quest-starter items and quest items the leader's rule found it carries
    // for nothing. [econ70] nothing is destroyed: what a vendor buys goes onto its SELL task, the rest it keeps and
    // the leader's bags line reports the slot. No game action, so this takes no step and never returns for it.
    if (status == RPG_IDLE || status == RPG_GO_GRIND || status == RPG_WANDER_RANDOM || status == RPG_DO_QUEST ||
        status == RPG_DO_GATHER || status == RPG_GO_CAMP || status == RPG_WANDER_NPC)
        sGuildCoordinator.MarkDeadWeightNow(bot);

    // [build147] the steps above can change the state without returning (b438 crash: std::get<GoCamp> on another
    // state, bad_variant_access): the switch dispatches on the state the bot is in now
    status = info.GetStatus();
    // [build148] a case whose state is not the one held (it cannot be now, the status is read just above): no throw,
    // the bot goes IDLE (the next tick picks its target) and the line names both
    auto const mismatch = [&](char const* expected)
    {
        LOG_ERROR("playerbots", "[build148] {} L{} status switch: case {} but the state is {} - back to IDLE",
                  bot->GetName(), bot->GetLevel(), expected, uint32(info.GetStatus()));
        info.ChangeToNoDecision(std::string("the status switch expected ") + expected + ", the state held another");
        return true;
    };
    switch (status)
    {
        case RPG_IDLE:
        {
            // [status41] the look before this one decided nothing and left the bot with no plan: a defect, reported
            // where it is seen. Nothing acts on it - no timer, no give-up: the look below runs as it always does
            if (info.idleUndecided)
                LOG_INFO("playerbots", "[status41] {} L{} comes back to the choice with nothing decided: {}",
                         bot->GetName(), bot->GetLevel(),
                         info.idleReason.empty() ? "no reason recorded" : info.idleReason);
            if (BindAtInnHere())  // [regress10] its home is the town it quests from ([lag322] a walk up to it)
                return true;
            // [night305] the town's flight master whose node it has not discovered: it learns it in passing
            if (SoloSafety::Applies(bot) && DiscoverTownFlightMaster("passes through the town"))
                return true;
            // [roles59] the role the global leader planned for the fives: its spec, at the class trainer
            if (sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter()) &&
                NewRpgProfession::StartRoleSpec(botAI, bot))
                return true;
            // A level-up makes quests that were given up on worth another try.
            if (info.lastQuestLevel != bot->GetLevel())
            {
                botAI->lowPriorityQuest.clear();
                info.lastQuestLevel = bot->GetLevel();
                LogDungeonCandidates();  // [guild11] hook for the dungeon step, once per level
            }

            // [help12] the requester of a camp help party grinds the camp until it is cleared (its party judges)
            if (WorldPosition camp;
                NewRpgCoop::HelpCampOf(bot, camp) && !WalkGivenUp(bot, camp.GetPositionX(), camp.GetPositionY()))
            {
                LOG_INFO("playerbots", "[help] camp {} L{} grinds the camp at ({:.0f},{:.0f}) with its helpers",
                         bot->GetName(), bot->GetLevel(), camp.GetPositionX(), camp.GetPositionY());
                info.ChangeToGoGrind(camp);
                std::get<NewRpgInfo::GoGrind>(info.data).forCamp = true;  // [help13]
                return true;
            }
            // [help12] the requester of a quest help party back from a pause (a sell run, a rest): its help quest
            {
                uint32 helpQuest = 0;
                int32 helpIdx = 0;
                WorldPosition helpAt;
                Quest const* quest = nullptr;
                if (NewRpgCoop::HelpQuestOf(bot, helpQuest, helpIdx, helpAt) &&
                    (quest = sObjectMgr->GetQuestTemplate(helpQuest)) != nullptr &&
                    bot->GetQuestStatus(helpQuest) == QUEST_STATUS_INCOMPLETE &&
                    (helpAt == WorldPosition() || !WalkGivenUp(bot, helpAt.GetPositionX(), helpAt.GetPositionY())))
                {
                    LOG_INFO("playerbots", "[help] resume {} L{} q{}: its helpers wait at the camp", bot->GetName(),
                             bot->GetLevel(), helpQuest);
                    botAI->lowPriorityQuest.erase(helpQuest);
                    info.ChangeToDoQuest(helpQuest, quest);
                    if (auto* data = std::get_if<NewRpgInfo::DoQuest>(&info.data); data && helpAt != WorldPosition())
                    {
                        data->pos = helpAt;
                        data->objectiveIdx = helpIdx;
                        data->lastReachPOI = 0;
                    }
                    return true;
                }
            }
            // [guild30] an order of the guild coordinator first; QUEST (the default) = the choice below
            {
                GuildRunStep const step = sGuildCoordinator.NextStep(bot, botAI);
                if (step.kind == GuildRunStep::BUSY)
                    return true;
                // [stuck12] a walk the bot gave up at this level (RefusedWalk, MOVE_FAR_LOOP) is not walked again for
                // the leader's order: it quests on and the order's status says why (b350 Baridrol L10 / Blahadrin L13:
                // the same refused walk taken again on every IDLE look, 51 / 148 STUCK alerts)
                bool const givenUp = (step.kind == GuildRunStep::WALK || step.kind == GuildRunStep::GRIND) &&
                                     GuildStepGivenUp(step);
                if (step.kind == GuildRunStep::WALK && !givenUp)
                {
                    LOG_INFO("playerbots", "[rpg] {} L{} GUILD reason: {}", bot->GetName(), bot->GetLevel(),
                             step.reason);
                    info.ChangeToGoCamp(step.pos);
                    return true;
                }
                if (step.kind == GuildRunStep::GRIND && !givenUp)  // [gather] a skinning place: killed and skinned
                {
                    LOG_INFO("playerbots", "[rpg] {} L{} GUILD GRIND reason: {}", bot->GetName(), bot->GetLevel(),
                             step.reason);
                    info.ChangeToGoGrind(step.pos);
                    // [skin12] the grind is for the skins: a gray mob of the place counts (GrindTargetValue)
                    std::get<NewRpgInfo::GoGrind>(info.data).forSkin = true;
                    std::get<NewRpgInfo::GoGrind>(info.data).orderEntries = step.entries;  // [craft434]
                    return true;
                }
                if (step.kind == GuildRunStep::GATHER)
                {
                    if (sPlayerbotAIConfig.RpgStatusProbWeight[RPG_DO_GATHER] &&
                        CheckRpgStatusAvailable(RPG_DO_GATHER) && !NewRpgDoGatherAction::NoSafeNodes(bot))
                    {
                        LOG_INFO("playerbots", "[rpg] {} L{} GATHER reason: {}", bot->GetName(),
                                 bot->GetLevel(), step.reason);
                        info.ChangeToDoGather();
                        return true;
                    }
                    sGuildCoordinator.SetOrderStatus(bot, "no nodes for its skill in this zone - quests on");
                }
            }
            // [guild10] a purposeful target only, never an aimless walk: a quest (hand in, objective, then a
            // starter of its level - ChooseBestQuest keeps that order), else mobs of its level, else stay put.
            {
                // [prof80] the leader's profession call due where the bot stands runs before anything that would
                // take it away - the route's next town stop first of all. A call is due only in its own town
                // (ProfessionCall: TownReady for a TRAIN / BUY, the craft's town or a safe place for a CRAFT), and
                // the moment the bot arrives there the call stops being a town stop (waitsTown false, TownStops
                // drops it), so the route simply moved on to its next one and the executor below never looked.
                // b475: Orenem L24 stood in Auberdine with "PROFESSION/train[C,R,runnable] rank to 150" - Tailoring
                // 75/75 at the Apprentice cap, 22 passes waiting for that town - and the very next decision was
                // "TOWN reason: route stop BUY Cragpool Lake (4825 yd)"; the call was then parked by [prof272] "its
                // trainer / vendor visit at this level did not do it". Over the whole 11 h log: 5 runnable train
                // calls, 1 learned, 0 recipes
                bool const planned = sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter());
                bool const callDue = planned && sGuildCoordinator.ProfessionCall(bot) != GuildProfessionKind::NONE;
                if (callDue && NewRpgProfession::StartTask(botAI, bot))
                {
                    static thread_local std::set<std::pair<uint32, uint32>> here;  // (bot, zone): once per zone
                    if (here.emplace(bot->GetGUID().GetCounter(), bot->GetZoneId()).second)
                        LOG_INFO("playerbots", "[prof80] {} L{} works its due profession call here, before its "
                                 "route's next stop", bot->GetName(), bot->GetLevel());
                    return true;
                }
                // [questroute] a town task the route passes: walk there, the guild's NextStep runs it on arrival
                WorldPosition townPos;
                std::string townTask;
                if (RouteTownStop(townPos, townTask))
                {
                    LOG_INFO("playerbots", "[rpg] {} L{} TOWN reason: route stop {} ({} yd)", bot->GetName(),
                             bot->GetLevel(), townTask, uint32(bot->GetExactDist2d(townPos)));
                    info.ChangeToGoCamp(townPos);
                    return true;
                }
                WorldPosition giverPos;
                // [perf214] the zone choice's grind-spot sweep paced over ticks: a look not finished this tick is
                // asked again next tick (the status stays IDLE, the verdicts found so far kept)
                choicePacing = true;
                choicePending = false;
                uint32 const questId = ChooseBestQuest(giverPos);
                choicePacing = false;
                if (choicePending)
                    return true;
                Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
                // [guild20] after the hand-ins: the planned profession's trainer or tool vendor
                // [tasks] single decider: a guild member works its profession only on the global leader's PROFESSION
                // call, never on its own choice (a bot outside the guild plan leads itself)
                // [craft390] ... and a leader's call due where it stands (ProfessionCall: a craft in this safe place, a
                // trainer / vendor of this town) runs before a hand-in elsewhere: the town stop was walked for it.
                // b452 Dekorhan reached the Darnassus and Dolanaar inns with the Virtuoso Inking Set, 85 Light
                // Parchment and 40 Silverleaf, and each time "QUEST q9432 hand in (hand-in first)" - its ender 9655 yd
                // off in Astranaar - took him away before the executor looked: Inscription stayed 1, no mill
                // [prof80] a planned member's due call had its look above, before the route's town stop and before
                // this quest choice - [craft390]'s "the call runs before a hand-in elsewhere" now holds one step
                // earlier; only a bot outside the guild plan reaches the executor here
                bool const handIn = quest && bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE;
                if (!planned && !handIn && NewRpgProfession::StartTask(botAI, bot))
                    return true;
                // [lag361] the route's next stop is a pickup or an objective, and the grind of its level nearby earns
                // more over the route's time: it grinds there (a hand-in of a complete quest is taken as planned)
                if (quest && bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                    if (WorldPosition grindPos; [&]()
                        {
                            std::string why;
                            if (!GrindBeatsRoute(grindPos, why))
                                return false;
                            LOG_INFO("playerbots", "[lag361] {} L{} GRIND instead of q{}: {}", bot->GetName(),
                                     bot->GetLevel(), questId, why);
                            return true;
                        }())
                    {
                        info.ChangeToGoGrind(grindPos);
                        return true;
                    }
                if (quest)
                {
                    QuestStatus const questStatus = bot->GetQuestStatus(questId);
                    std::string const reason =
                        questStatus == QUEST_STATUS_COMPLETE     ? "hand in"
                        : questStatus == QUEST_STATUS_INCOMPLETE ? "objective"
                                                                 : "pick up, giver " +
                                                                       std::to_string(uint32(bot->GetExactDist(giverPos))) +
                                                                       " yd";
                    // [guild90] and why the planner took it (kept the active quest, or why it left it)
                    std::string const note = QuestChoiceNote(bot);
                    LOG_INFO("playerbots", "[rpg] {} L{} QUEST q{} reason: {}{}", bot->GetName(), bot->GetLevel(),
                             questId, reason, note.empty() ? "" : " (" + note + ")");
                    info.ChangeToDoQuest(questId, quest, giverPos);
                    return true;
                }
            }
            // [guild11] no pvp, no aimless grind: gathering is the gatherer group's (random bots) work; else grind
            // near the giver of the quest it is closest to being allowed, until it has the level
            if (sRandomPlayerbotMgr.IsRandomBot(bot) && sPlayerbotAIConfig.RpgStatusProbWeight[RPG_DO_GATHER] &&
                CheckRpgStatusAvailable(RPG_DO_GATHER) && !NewRpgDoGatherAction::NoSafeNodes(bot))
            {
                LOG_INFO("playerbots", "[rpg] {} L{} GATHER reason: no quest it can take now", bot->GetName(),
                         bot->GetLevel());
                info.ChangeToDoGather();
                return true;
            }
            {
                WorldPosition giverPos;
                WorldPosition grindPos;
                bool sweepPending = false;  // [perf214] its grind-spot sweep paced over ticks
                uint32 const questId = ChooseNextLevelQuest(giverPos, grindPos, true, &sweepPending);
                if (sweepPending)
                    return true;
                Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
                // [stuck8] no grind spot of its level on this map and the giver under its feet: a GO_GRIND there
                // reaches it at once and WANDER_RANDOM finds no mob (b343 Orton on the Deeprun Tram, 160 alerts of
                // IDLE / GO_GRIND / WANDER_RANDOM); the reason is logged, it rests until the choice runs again
                if (quest && grindPos == WorldPosition() && giverPos.GetMapId() == bot->GetMapId() &&
                    bot->GetExactDist(giverPos) < 10.0f)
                {
                    // [stuck31] b475: Uraela L22 logged this STAY 62 times in The Exodar and sat between them
                    // (IDLE -> STAY -> REST -> IDLE, "stood by state: GO_GRIND 161 s"). The whole picker is this
                    // map's: ChooseNextLevelQuest skips every giver whose `start.mapId != bot->GetMapId()` and the
                    // grind spots are filtered the same way, so on map 530 - L1-20 in the draenei zones, L58+ in
                    // Outland - a level 22 bot has nothing to find and nothing to walk to, and standing on the
                    // giver of a quest that needs L26 is the end of the line. "No grind spot of its level on this
                    // map and no quest it may take" is not a reason to sit: it is the reason to leave the map. The
                    // same choice is made again over every map the bot has a route to (the landmass route check,
                    // the faction, enemy-zone and plannability filters all still run), and the giver it finds is
                    // walked to like any other - MoveFarTo takes the flight or the boat.
                    WorldPosition awayGiver;
                    WorldPosition awayGrind;
                    if (uint32 const awayQuest = ChooseNextLevelQuest(awayGiver, awayGrind, true, nullptr, true);
                        awayQuest && awayGiver != WorldPosition() && awayGiver.GetMapId() != bot->GetMapId())
                    {
                        Quest const* away = sObjectMgr->GetQuestTemplate(awayQuest);
                        WorldPosition const target = awayGrind != WorldPosition() ? awayGrind : awayGiver;
                        LOG_INFO("playerbots",
                                 "[stuck31] {} L{} GRIND reason: nothing of its level on map {} (q{} needs L{}, no "
                                 "grind spot here) - off the map to q{} needs L{} on map {}, {} spot",
                                 bot->GetName(), bot->GetLevel(), bot->GetMapId(), questId, quest->GetMinLevel(),
                                 awayQuest, away ? away->GetMinLevel() : 0, awayGiver.GetMapId(),
                                 awayGrind != WorldPosition() ? "its grind" : "its giver's");
                        info.ChangeToGoGrind(target);
                        return true;
                    }
                    LOG_INFO("playerbots", "[rpg] {} L{} STAY reason: q{} needs L{}, no grind spot of its level on "
                             "map {} and no quest it may take now, nor one on any map it has a route to [stuck31]",
                             bot->GetName(), bot->GetLevel(), questId, quest->GetMinLevel(), bot->GetMapId());
                    info.ChangeToRest();
                    bot->SetStandState(UNIT_STAND_STATE_SIT);
                    return true;
                }
                if (quest)
                {
                    WorldPosition target = grindPos != WorldPosition() ? grindPos : giverPos;
                    // [guild14] every quest postponed or dangerous round it (b303 Thokhen L3 between Root Threshers
                    // L8: IDLE / DANGER / RECOVER / POSTPONE in a loop): a grind spot whose walk is refused is
                    // replaced by its hub (the nearest ender of a quest in its log or an inn on a safe way) when that
                    // hub is a walk away and safe, else the grind spot stays (b308: a "stay" and a hub under its feet
                    // left 100+ bots-minutes idle); a level-up gives the postponed quests another try
                    if (target.GetMapId() == bot->GetMapId() &&
                        TripRefused(bot, target.GetPositionX(), target.GetPositionY(), target.GetPositionZ(),
                                    "grind spot"))
                    {
                        WorldPosition const hub = NearestHub(bot);
                        if (hub != WorldPosition() && bot->GetExactDist(hub) >= 10.0f &&
                            !TripRefused(bot, hub.GetPositionX(), hub.GetPositionY(), hub.GetPositionZ(), "hub"))
                        {
                            LOG_INFO("playerbots", "[rpg] {} L{} GRIND reason: quests postponed or dangerous round it, "
                                     "its hub {} yd", bot->GetName(), bot->GetLevel(), uint32(bot->GetExactDist(hub)));
                            info.ChangeToGoGrind(hub);
                            return true;
                        }
                        // [regress10] no quest it may do here, its grind spot and its hub both walks it refuses
                        // (every way passes mobs it does not live through, the run-past chases counted): its
                        // Hearthstone out (b347 Lyleria L9 / Mytaeleas L11 among the Ashenvale bears L16-18: 1025 s
                        // and 710 s STUCK, every quest postponed)
                        if (HearthOut(Acore::StringFormat("q{} needs L{}, its grind spot and its hub are walks it "
                                                          "refuses",
                                                          questId, quest->GetMinLevel())))
                            return true;
                        // [holds9] the target is a walk given up at this level (refused, or proven out of walk) and
                        // no hub replaces it: a GO_GRIND there is refused again at once (b345 Ellosera: GRIND -> MAZE
                        // -> IDLE, 32 HOLD / LOOP lines); the reason is logged, it rests until the choice runs again
                        if (WalkGivenUp(bot, target.GetPositionX(), target.GetPositionY()))
                        {
                            LOG_INFO("playerbots", "[rpg] {} L{} STAY reason: q{} needs L{}, no grind spot of its "
                                     "level left and its giver is a walk given up at this level",
                                     bot->GetName(), bot->GetLevel(), questId, quest->GetMinLevel());
                            info.ChangeToRest();
                            bot->SetStandState(UNIT_STAND_STATE_SIT);
                            return true;
                        }
                    }
                    LOG_INFO("playerbots",
                             "[rpg] {} L{} GRIND reason: q{} {} needs L{}, giver {} yd, grind spot {} yd from it",
                             bot->GetName(), bot->GetLevel(), questId, quest->GetTitle(), quest->GetMinLevel(),
                             uint32(bot->GetExactDist(giverPos)), uint32(giverPos.GetExactDist(target)));
                    info.ChangeToGoGrind(target);
                    return true;
                }
            }
            // [stuck31] no quest of any level here is the same end of the line: the choice runs over every map the
            // bot has a route to before it sits down
            {
                WorldPosition awayGiver;
                WorldPosition awayGrind;
                if (uint32 const awayQuest = ChooseNextLevelQuest(awayGiver, awayGrind, true, nullptr, true);
                    awayQuest && awayGiver != WorldPosition() && awayGiver.GetMapId() != bot->GetMapId())
                {
                    Quest const* away = sObjectMgr->GetQuestTemplate(awayQuest);
                    WorldPosition const target = awayGrind != WorldPosition() ? awayGrind : awayGiver;
                    LOG_INFO("playerbots",
                             "[stuck31] {} L{} GRIND reason: no quest of any level on map {} - off the map to q{} "
                             "needs L{} on map {}, {} spot",
                             bot->GetName(), bot->GetLevel(), bot->GetMapId(), awayQuest,
                             away ? away->GetMinLevel() : 0, awayGiver.GetMapId(),
                             awayGrind != WorldPosition() ? "its grind" : "its giver's");
                    info.ChangeToGoGrind(target);
                    return true;
                }
            }
            LOG_INFO("playerbots",
                     "[rpg] {} L{} STAY reason: no quest of any level on this map, nor one on any map it has a route "
                     "to [stuck31]", bot->GetName(), bot->GetLevel());
            info.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }

        case RPG_GO_GRIND:
        {
            auto* const state = std::get_if<NewRpgInfo::GoGrind>(&info.data);  // [build148]
            if (!state)
                return mismatch("GoGrind");
            auto& data = *state;
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_GRIND -> WANDER_RANDOM: at the grind spot the bot fights the mobs there, no random walk
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                LOG_INFO("playerbots", "[rpg] {} L{} WANDER reason: grind spot reached", bot->GetName(),
                         bot->GetLevel());
                bool const forSkin = data.forSkin;
                bool const forCamp = data.forCamp;
                std::vector<uint32> orderEntries = std::move(data.orderEntries);
                info.ChangeToWanderRandom();
                std::get<NewRpgInfo::WanderRandom>(info.data).forSkin = forSkin;  // [skin12]
                std::get<NewRpgInfo::WanderRandom>(info.data).forCamp = forCamp;  // [help13]
                std::get<NewRpgInfo::WanderRandom>(info.data).orderEntries = std::move(orderEntries);  // [craft434]
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto* const state = std::get_if<NewRpgInfo::GoCamp>(&info.data);  // [build148]
            if (!state)
                return mismatch("GoCamp");
            auto& data = *state;
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC only to visit the vendor this walk came for; else the next purposeful target
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                // [guild30] a guild bank walk: deposit / withdraw at the vault
                if (uint8 const bank = sGuildCoordinator.OnCampReached(bot))
                {
                    if (bank == 1)
                        info.ChangeToIdle("its guild bank errand at the vault is done");
                    return true;
                }
                if (!info.vendorRunEntry)
                {
                    info.ChangeToIdle("its camp walk ended, the next stop not chosen yet");
                    return true;
                }
                LOG_INFO("playerbots", "[rpg] {} L{} WANDER reason: vendor visit ({})", bot->GetName(),
                         bot->GetLevel(), info.vendorRunReason);
                info.ChangeToWanderNpc();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // [timers] WANDER_RANDOM -> IDLE by facts, no status duration: the grind was chosen for the level a quest
            // needs (or quests postponed round it until the next level); a level-up re-runs the choice. No mob of its
            // level left in reach also ends it (NewRpgWanderRandomAction); the global leader's URGENT task interrupts
            auto* const state = std::get_if<NewRpgInfo::WanderRandom>(&info.data);  // [build148]
            if (!state)
                return mismatch("WanderRandom");
            auto& data = *state;
            // [help13] a camp party's grind: at the camp's live mobs while the party lasts (the nearest one beyond
            // sight: walked to), over with the party (camp cleared, party gone). b353 Dandolar: GO_GRIND to the camp,
            // then WANDER_RANDOM on the Pygmy Tide Crawlers round it for 15 min, ended only by its level-up
            if (data.forCamp && !bot->IsInCombat())
            {
                WorldPosition camp;
                if (!NewRpgCoop::HelpCampOf(bot, camp))
                {
                    LOG_INFO("playerbots", "[help13] {} L{} IDLE reason: its camp help party is over, the choice runs "
                             "again", bot->GetName(), bot->GetLevel());
                    info.ChangeToIdle("its camp help party is over");
                    return true;
                }
                if (camp.GetMapId() == bot->GetMapId() && bot->GetExactDist(camp) > sPlayerbotAIConfig.SightDistance)
                {
                    LOG_INFO("playerbots", "[help13] {} L{} GO_GRIND reason: the camp's nearest live mob {:.0f} yd off",
                             bot->GetName(), bot->GetLevel(), bot->GetExactDist(camp));
                    info.ChangeToGoGrind(camp);
                    std::get<NewRpgInfo::GoGrind>(info.data).forCamp = true;
                    return true;
                }
            }
            if (!data.level)
                data.level = bot->GetLevel();
            else if (data.level != bot->GetLevel())
            {
                LOG_INFO("playerbots", "[rpg] {} L{} IDLE reason: level-up while grinding, the choice runs again",
                         bot->GetName(), bot->GetLevel());
                info.ChangeToIdle("it levelled up while grinding, the choice runs again");
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
            // [timers] no status duration: the visit ends when its job at the npc is done, the npc is gone or no walk
            // reaches it (NewRpgWanderNpcAction)
            break;
        case RPG_DO_QUEST:
        {
            auto* const state = std::get_if<NewRpgInfo::DoQuest>(&info.data);  // [build148]
            if (!state)
                return mismatch("DoQuest");
            auto& data = *state;

            // [tasks] passing through a town: its town tasks run now (NextStep runs a convenient task only in its own
            // town, the bot's quests go on after it)
            if (bot->GetAreaId() != info.townCheckArea)
            {
                info.townCheckArea = bot->GetAreaId();
                GuildRunStep const step = sGuildCoordinator.NextStep(bot, botAI);
                if (step.kind == GuildRunStep::BUSY)
                    return true;
                if (step.kind == GuildRunStep::WALK && !GuildStepGivenUp(step))  // [stuck12]
                {
                    LOG_INFO("playerbots", "[rpg] {} L{} GUILD on the way (q{} waits) reason: {}", bot->GetName(),
                             bot->GetLevel(), data.questId, step.reason);
                    info.ChangeToGoCamp(step.pos);
                    return true;
                }
            }

            // [timers] DO_QUEST -> IDLE by facts, no status duration: the quest is handed in, postponed (no walk to
            // its spots, unsupported, all camps spent and another quest cheaper than the respawn) or the global
            // leader's URGENT task outranks it (GuildCoordinator::UpdateOrders)
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            auto* const state = std::get_if<NewRpgInfo::TravelFlight>(&info.data);  // [build148]
            if (!state)
                return mismatch("TravelFlight");
            auto& data = *state;
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival
                info.ChangeToIdle("its flight landed, the next stop not chosen yet");
                return true;
            }
            break;
        }
        case RPG_REST:
        {
            // REST -> IDLE: [timers] class (b), the cadence the IDLE choice (ChooseBestQuest, expensive) is asked again
            // at while nothing is to do; IDLE decides from the facts alone and the same facts give REST again
            if (info.HasStatusPersisted(statusRestDuration))
            {
                info.ChangeToIdle("it rested its fill, the choice runs again");
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
            // [timers] no status duration: the command stays until replaced or its capture point is gone / held
            // (NewRpgOutdoorPvpAction)
            break;
        case RPG_DO_GATHER:
        {
            // [timers] DO_GATHER -> IDLE by facts, no status duration: nothing (left) to gather in the zone or full
            // bags (NewRpgDoGatherAction); a guild member's gather task gone or outranked
            // (GuildCoordinator::UpdateOrders);
            // a bot outside the guild plan gathers because no quest of its level was there: a level-up re-runs that
            auto* const state = std::get_if<NewRpgInfo::DoGather>(&info.data);  // [build148]
            if (!state)
                return mismatch("DoGather");
            auto& data = *state;
            if (!data.level)
                data.level = bot->GetLevel();
            else if (data.level != bot->GetLevel() && !sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter()))
            {
                LOG_INFO("playerbots", "[rpg] {} L{} IDLE reason: level-up while gathering, the choice runs again",
                         bot->GetName(), bot->GetLevel());
                info.ChangeToIdle("it levelled up while gathering, the choice runs again");
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

namespace
{
    // [stuck8] a GO_GRIND / GO_CAMP walk that gives no move while the bot stands out of combat: one HOLD line per
    // change of its cause (b343: Ryarii, Febineysh, Aevmae stood 60 s in GO_CAMP to a vendor with no line, "reason=none")
    void NoteWalkHold(Player* bot, NewRpgInfo& info, char const* status, WorldPosition const& pos,
                      std::string const& refusal)
    {
        if (bot->isMoving() || bot->IsInCombat())
        {
            info.holdNote.clear();
            return;
        }
        std::string const cause = !refusal.empty()     ? refusal
                                  : info.moveFarFailed ? std::string("the route search finds no walk from here")
                                                       : std::string("MoveFarTo gave no step and no refusal");
        std::string const key = std::string(status) + "|" + cause;
        if (key == info.holdNote)
            return;
        info.holdNote = key;
        LOG_INFO("playerbots",
                 "[rpg] {} L{} HOLD reason: {} walk to ({:.0f},{:.0f},{:.0f}) {:.0f} yd gives no move: {}",
                 bot->GetName(), bot->GetLevel(), status, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                 bot->GetExactDist(pos), cause);
    }
}

bool NewRpgBaseAction::FarWalkStep(WorldPosition const& pos, char const* status)
{
    NewRpgInfo& info = botAI->rpgInfo;
    // [holds9] b345 Emdyl, Manue: the group leader's GO_CAMP walk ran between the group waits (NewRpgWaitForGroupAction
    // stops it each time): the walk waits with the group, its reason is the GROUP_WAIT line
    if (info.groupWaiting)
    {
        info.holdNote.clear();
        return true;
    }
    if (MoveFarTo(pos))
    {
        info.holdNote.clear();
        return true;
    }
    std::string const refusal = LastMoveRefusal();
    bool const searchFailed = info.moveFarFailed;
    // Small nudge so the next tick's MoveFarTo starts from a
    // slightly different position. Kept small so it doesn't look
    // like the bot is abandoning its destination.
    if (MoveRandomNear(10.0f))
        return true;
    // [holds9] b345: 27 STUCK "no route / unreachable destination" (670 s): the walk is refused, or no route and no
    // step leads on from here, and the nudge finds no step either - nothing the bot does here changes it. The walk
    // is given up (RefusedWalk: out of the choices until the next level, the state ends, IDLE takes the next target);
    // a state of the moment (the last walk under way, a root, a cast) holds with its reason
    bool const momentary = refusal.rfind("IsWaitingForLastMove", 0) == 0 ||
                           refusal.rfind("IsDuplicateMove", 0) == 0 || refusal.rfind("IsMovingAllowed", 0) == 0;
    if (!momentary && (!refusal.empty() || searchFailed) && !bot->IsInCombat())
    {
        info.holdNote.clear();
        return RefusedWalk(pos, std::string(status) + " walk: " +
                                    (refusal.empty() ? "the route search finds no walk from here" : refusal));
    }
    NoteWalkHold(bot, info, status, pos, refusal);
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
        return FarWalkStep(data->pos, "GO_GRIND");

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
        return FarWalkStep(data->pos, "GO_CAMP");

    return false;
}

Creature* NewRpgWanderRandomAction::NextPlaceMob(std::vector<uint32> const& entries, bool forSkin)
{
    bool const skin = forSkin && bot->HasSkill(SKILL_SKINNING);
    if (!skin && entries.empty())
        return nullptr;
    // the map's view of the bot (Map::GetVisibilityRange): the mobs the core shows it, a fact of where it stands
    float const range = bot->GetMap()->GetVisibilityRange();
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(bot, range);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, range);
    Creature* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (Unit* unit : units)
    {
        Creature* mob = unit->ToCreature();
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || mob->GetCharmerOrOwnerGUID() ||
            mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE) || !bot->IsValidAttackTarget(mob))
            continue;
        CreatureTemplate const* tmpl = mob->GetCreatureTemplate();
        if (!tmpl || tmpl->rank > CREATURE_ELITE_NORMAL ||
            Acore::XP::GetColorCode(bot->GetLevel(), mob->GetLevel()) <= XP_ORANGE)
            continue;
        // [craft434] a mob of the leader's gathering place (a drop place's gray coyotes too), or one that carries skins
        if (std::find(entries.begin(), entries.end(), mob->GetEntry()) == entries.end() &&
            !(skin && tmpl->SkinLootId))
            continue;
        float const d = bot->GetExactDist(mob);
        // [nav200] a mob of the place the route already found no walk to from this spot is not the next one either:
        // the picker takes the one after it (SoloSafety::Unreachable: the memo ends when the bot or the mob moves)
        if (d >= bestDist || InDangerZone(bot, mob->GetPositionX(), mob->GetPositionY()) ||
            WalkGivenUp(bot, mob->GetPositionX(), mob->GetPositionY()) || SoloSafety::Unreachable(bot, mob))
            continue;
        bestDist = d;
        best = mob;
    }
    return best;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    // [guild10] no random steps: the grind strategy takes the bot to the next mob of its level; none left here
    // -> IDLE picks the next purposeful target
    if (AI_VALUE(Unit*, "grind target"))
        return false;

    // [skin12] a skinning place's mobs spread over more than its sight (b350 the Grizzled Den: 40 Wendigo spawns
    // over 290 yd, Keenonk / Ainne at the one spot: 47 STUCK + 56 LOOP alerts, "grind spot cleared" 130 times while
    // the den's far end stood alive): the nearest live skinnable mob it may fight within the map's view of it
    std::vector<uint32> orderEntries;
    bool forSkin = false;
    if (auto const* wander = std::get_if<NewRpgInfo::WanderRandom>(&botAI->rpgInfo.data))
    {
        orderEntries = wander->orderEntries;
        forSkin = wander->forSkin;
    }
    if (forSkin || !orderEntries.empty())
        if (Creature* next = NextPlaceMob(orderEntries, forSkin))
        {
            LOG_INFO("playerbots", "[skin12] {} L{} place grind: no mob in reach here, the nearest live {} L{} {:.0f} "
                     "yd off at ({:.0f},{:.0f})",
                     bot->GetName(), bot->GetLevel(), next->GetName(), next->GetLevel(), bot->GetExactDist(next),
                     next->GetPositionX(), next->GetPositionY());
            botAI->rpgInfo.ChangeToGoGrind(WorldPosition(next));
            std::get<NewRpgInfo::GoGrind>(botAI->rpgInfo.data).forSkin = forSkin;
            std::get<NewRpgInfo::GoGrind>(botAI->rpgInfo.data).orderEntries = std::move(orderEntries);
            return true;
        }
    // [craft434] b465 Sehdrurmund L22 (108 STUCK) / Maronic L22 (47): drop places of gray Coyotes / Crag Boars - the
    // leader saw live mobs of the place and sent the grind again, the grind took no gray mob ("no mob of its level"):
    // GO_GRIND / WANDER_RANDOM / IDLE for 330 s and more. At the place, with nothing of it the bot can take in its
    // view (alive, out of combat, below orange, a walk not given up, out of danger), the place goes back to the plan
    if (!orderEntries.empty())
    {
        LOG_INFO("playerbots", "[craft434] {} L{} at its gathering place: no mob of it it can take in its view - the "
                 "place goes back to the plan",
                 bot->GetName(), bot->GetLevel());
        sGuildCoordinator.SetOrderStatus(bot, "no mob of the place it can take in its view");
    }
    LOG_INFO("playerbots", "[rpg] {} L{} grind spot cleared: no mob of its level in reach", bot->GetName(),
             bot->GetLevel());
    botAI->rpgInfo.ChangeToIdle("its grind spot is cleared of mobs of its level");
    return true;
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo && info.vendorRunEntry)
    {
        // [guild3] the vendor this walk came for; gone (dead, despawned): the visit counts as done
        if (Creature* vendor = bot->FindNearestCreature(info.vendorRunEntry, 60.0f))
        {
            data.npcOrGo = vendor->GetGUID();
            data.lastReach = 0;
            data.bestDist = 0.0f;  // [vendor23] a new npc: its own walks
            return true;
        }
        LOG_INFO("playerbots", "[New RPG] {} finds no vendor {} at its spawn", bot->GetName(), info.vendorRunEntry);
        // [vend20] the errand's own verdict: nobody there. The spawn is marked so the next choice does not send it
        // back, and the state ends on the vendor's fact instead of falling through to the quest giver's judge below
        ObjectGuid::LowType const marked = NoteVendorNotThere(bot);
        LOG_INFO("playerbots", "[vend20] {} L{} vendor errand ({}) ends: no vendor {} at its spawn {} - that spawn is "
                 "no place for it to meet that vendor",
                 bot->GetName(), bot->GetLevel(), info.vendorRunReason.empty() ? "-" : info.vendorRunReason,
                 info.vendorRunEntry, marked);
        info.vendorRunEntry = 0;
        info.vendorVisited = true;
        info.vendorVisitStocked = false;  // [eco193]
        RefuseRestockVendor(bot, info, info.vendorRunLast,  // [eco205]
                            AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::consumables)));
        info.vendorVisitMap = bot->GetMapId();
        info.vendorMissed = true;  // [vendor23]
        info.vendorVisitBagUsed = AI_VALUE(uint8, "bag space");
        info.vendorVisitMoney = bot->GetMoney();
        // [vend20] the errand ends on its own fact. The quest giver's judge below is for a WANDER_NPC that came for
        // a giver, and it answered this errand "no quest giver here with anything to give or take" every time
        info.ChangeToIdle("its vendor errand met no vendor at that spawn");
        return true;
    }
    if (!data.npcOrGo)
    {
        // [guild10] only a giver with something to accept or reward, never a random npc; none -> IDLE
        ObjectGuid npcOrGo = NewRpgHelpers::FindQuestGiver(botAI);
        if (npcOrGo.IsEmpty())
        {
            info.ChangeToIdle("no quest giver here with anything to give or take");
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        data.bestDist = 0.0f;  // [vendor23] a new npc: its own walks
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);

            if (Creature* creature = object->ToCreature())
            {
                constexpr uint32 MIN_VENDOR_BALANCE = 1000;  // 10 silver

                bool const vendorRun = info.vendorRunEntry && creature->GetEntry() == info.vendorRunEntry;
                if (creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) && (AI_VALUE(bool, "can sell") || vendorRun))
                {
                    uint32 const moneyBefore = bot->GetMoney();
                    uint32 const ammoBefore = AI_VALUE2(uint32, "item count", "ammo");
                    uint8 const bagBefore = AI_VALUE(uint8, "bag space");
                    if (AI_VALUE(bool, "can sell"))
                        botAI->DoSpecificAction("sell", Event("sell", "vendor"), true);
                    sGuildCoordinator.SellMarked(botAI);  // [guild30] what the coordinator gave up

                    // Buying sorts the vendor list through the weight calculator; skip it for
                    // bots without money for cheap consumables, unless they came here for ammo.
                    if (bot->GetMoney() >= MIN_VENDOR_BALANCE || vendorRun)
                        botAI->DoSpecificAction("buy", Event("buy", "vendor"), true);
                    // [pace25] its food and drink, as RestockFoodAction buys them standing at the vendor
                    if (vendorRun)
                        botAI->DoSpecificAction("restock food", Event("restock food"), true);
                    // [supply27] a five member's missing reagents this vendor sells: a stack of each, its own money
                    if (vendorRun && sGuildCoordinator.SupplyWanted(bot))
                    {
                        std::vector<uint32> missing;
                        FiveGearShort(bot, &missing);
                        if (VendorItemData const* items = creature->GetVendorItems())
                            for (size_t i = 0; i < items->m_items.size(); ++i)
                            {
                                VendorItem const* item = items->m_items[i];
                                ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
                                if (!proto || item->ExtendedCost ||
                                    std::find(missing.begin(), missing.end(), item->item) == missing.end() ||
                                    bot->GetMoney() < proto->BuyPrice)
                                    continue;
                                bool const bought = bot->BuyItemFromVendorSlot(creature->GetGUID(), uint32(i),
                                                                               item->item, uint8(1), NULL_BAG, NULL_SLOT);
                                LOG_INFO("playerbots", "[supply27] {} L{} buys reagent {} at {}: {}", bot->GetName(),
                                         bot->GetLevel(), proto->Name1, creature->GetName(),
                                         bought ? "bought" : "refused by the core");
                            }
                    }

                    // [vendor12] a run for ammo that the generic buy left at the same count (its item-usage /
                    // weight look found no use for it: b350 Sehdrurmund L6, four runs to Adlin Pridedrift, ammo
                    // 0 -> 0 each time): the cheapest usable stack of the ammo its ranged weapon fires, as a player
                    // buys it, until its count passes the low-ammo line that sent it (VENDOR_LOW_AMMO), within its
                    // money and bags
                    if (vendorRun)
                        if (uint32 const sub = RequiredAmmoSubClass(bot))
                        {
                            botAI->GetAiObjectContext()->GetValue<uint32>("item count", "ammo")->Reset();
                            uint32 const after = AI_VALUE2(uint32, "item count", "ammo");
                            if (after <= ammoBefore && after < VENDOR_LOW_AMMO)
                                BuyAmmoDirect(creature, sub, after);
                        }
                    if (vendorRun)
                    {
                        botAI->GetAiObjectContext()->GetValue<uint32>("item count", "ammo")->Reset();
                        botAI->GetAiObjectContext()->GetValue<uint8>("bag space")->Reset();
                        LOG_INFO("playerbots",
                                 "[New RPG] {} at vendor {} ({}): money {}c -> {}c, ammo {} -> {}, bags {}% -> {}%",
                                 bot->GetName(), creature->GetName(), info.vendorRunReason, moneyBefore,
                                 bot->GetMoney(), ammoBefore, AI_VALUE2(uint32, "item count", "ammo"), bagBefore,
                                 AI_VALUE(uint8, "bag space"));
                        QuestStats::TownDone(bot->GetGUID().GetCounter(), bot->GetName(), "vendor visit");
                        info.vendorRunEntry = 0;
                        info.vendorVisited = true;
                        info.vendorMissed = false;  // [vendor23]
                        // [eco193] the visit did not fail to stock it: not below the restock run's line of food, nor
                        // of drink using mana - or this vendor sells none of the kind it lacks (a sale's vendor: no
                        // try was made). [eco197] the run's own line: a five member's restock line (RESTOCK_FOOD_MIN),
                        // not only an empty bag - b430 Manue (11 drinks, no money its budget let it spend) was
                        // "stocked" at 1, went straight back and visited Innkeeper Heather again and again (LOOP)
                        uint32 const line = sGuildCoordinator.SupplyWanted(bot) ? RESTOCK_FOOD_MIN : 1;
                        uint32 const fiveDrinks =  // [eco206] its five's [dun194] drinks
                            line > 1 && bot->GetMaxPower(POWER_MANA) ? sGuildCoordinator.FiveDrinkTarget(bot) : 0;
                        bool const outFood = CountBagFood(bot, RESTOCK_SPELL_CATEGORY_FOOD) < line;
                        bool const outDrink = bot->GetMaxPower(POWER_MANA) &&
                                              CountBagFood(bot, RESTOCK_SPELL_CATEGORY_DRINK) <
                                                  (fiveDrinks ? fiveDrinks : line);
                        uint32 const foodPrice =
                            outFood ? RestockPrice(bot, creature->GetEntry(), RESTOCK_SPELL_CATEGORY_FOOD) : 0;
                        uint32 const drinkPrice =
                            outDrink ? RestockPrice(bot, creature->GetEntry(), RESTOCK_SPELL_CATEGORY_DRINK) : 0;
                        info.vendorVisitStocked = !foodPrice && !drinkPrice;
                        // [eco201] what it lacked costs this much here: the next restock waits for that budget (and
                        // more than the budget this visit could not buy with: bags full, a sold-out stock)
                        uint32 const budgetNow =
                            AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::consumables));
                        // [eco205] that vendor only: skipped until the budget is more than now; stocked, the
                        // vendor and the map's look are open again
                        if (info.vendorVisitStocked)
                        {
                            info.restockRefused.erase(creature->GetEntry());
                            info.restockLookPrice = 0;
                        }
                        else
                            RefuseRestockVendor(bot, info, creature->GetEntry(), budgetNow);
                        info.vendorVisitMap = bot->GetMapId();
                        info.vendorVisitBagUsed = AI_VALUE(uint8, "bag space");
                        info.vendorVisitMoney = bot->GetMoney();
                    }
                }

                // [guild15] what its money pays for, weapons first (RepairAllAction), not all or nothing: "can repair"
                // wants the whole bill within its budget (b311: Thokhen L5 with 283c kept 8 of 8 items broken)
                if (creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && AI_VALUE(uint8, "durability") < 100 &&
                    bot->GetMoney() > 0)
                {
                    // [guild15] only broken gear (0 durability) loses its stats in 3.3.5; worn gear deals the same:
                    // a repair of worn gear kept the measurement (b325 Cassand: every repair sent the judge back to
                    // the white weapon damage, 3.2 dps, and it refused every mob of its level)
                    bool broken = false;
                    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END && !broken; ++slot)
                        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                            broken = item->IsBroken();
                    botAI->DoSpecificAction("repair", Event("repair"), true);
                    // [regress10] the leader's REPAIR task: this visit counts (tried again after a money gain while
                    // the need stays); its [gear] durability line follows with the leader's next look
                    info.repairVisited = true;
                    info.repairVisitMoney = bot->GetMoney();
                    // [guild15] its damage and the fights it refused were measured with the broken gear
                    if (broken)
                    {
                        SoloSafety::OnRepaired(bot);
                        NewRpgBaseAction::ForgetRefusals(bot);
                    }
                }
            }
        }

        // [guild10] the job at this npc is done: on to the next purposeful target, no next random npc
        info.ChangeToIdle("its errand at the npc is done");
        return true;
    }
    else
    {
        // [vendor13] the bot stands at the end of the walk to the npc and the game's interaction reach is still not
        // met (a counter on a floor above, behind a rail): no walk takes it closer, the visit ends as for an npc out of
        // walk. b353 Sylaenas L2 walked 7649 yd round vendor Keina (14 yd) for 1203 s, re-walking random spots by it
        bool atWalkEnd = false;
        // [vendor15] a walk to the npc under way is let finish: the walk-end check above needs the bot standing, and
        // a new MoveWorldObjectTo every tick (a new spot round the npc each time) kept it moving for the whole run -
        // b355 Sylaenas L2 again 1203 s and 7660 yd round vendor Keina in Aldrassil, [vendor13] / [vendor14] never
        // fired for it
        if (object && bot->isMoving() &&
            bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
            return true;
        // [vendor23] the npc is gone from the bot's map view (despawned, its guid stale): the visit ends as for a
        // vendor gone from its spawn - before, random steps round the spot every tick for the whole run
        if (!object)
        {
            LOG_INFO("playerbots", "[vendor23] {} L{} npc of its visit no longer seen: the visit ends", bot->GetName(),
                     bot->GetLevel());
            if (info.vendorRunEntry)
            {
                info.vendorRunEntry = 0;
                info.vendorVisited = true;
                info.vendorVisitStocked = false;  // [eco193]
                RefuseRestockVendor(bot, info, info.vendorRunLast,  // [eco205]
                                    AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::consumables)));
                info.vendorVisitMap = bot->GetMapId();
                info.vendorMissed = true;  // [vendor23]
                info.vendorVisitBagUsed = AI_VALUE(uint8, "bag space");
                info.vendorVisitMoney = bot->GetMoney();
            }
            info.ChangeToIdle("its vendor visit is over");
            return true;
        }
        // [vendor23] a walk to the npc that ended no nearer than the nearest it stood after an earlier walk of this
        // visit gets it no closer (the MOVE_FAR_LOOP fact): no walk reaches the npc, the visit ends. b372 Sylaenas
        // L2 walked 3392 yd up and down Aldrassil for 1200 s to its vendor 43 yd off, [vendor13] / [vendor14] silent
        // (the walk still to go, along its navmesh way: a ramp round a tree leads away before it leads up)
        bool noCloser = false;
        if (!bot->isMoving() && object->GetMapId() == bot->GetMapId() && !IsWithinInteractionDist(object))
        {
            BotPath const toGo =
                FindBotPath(bot, object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
            float const d = (toGo.type & PATHFIND_NOPATH) || toGo.points.size() < 2 ? bot->GetExactDist(object)
                                                                                     : toGo.length;
            noCloser = data.bestDist > 0.0f && d + bot->GetCombatReach() >= data.bestDist;  // a step of its own reach
            if (noCloser)
                LOG_INFO("playerbots", "[vendor23] {} L{} npc {} not reached: its walk ended {:.1f} yd of walk off, "
                         "no nearer than {:.1f} yd after an earlier walk - the visit ends",
                         bot->GetName(), bot->GetLevel(), object->GetName(), d, data.bestDist);
            data.bestDist = data.bestDist > 0.0f ? std::min(data.bestDist, d) : d;
        }
        if (!noCloser && !bot->isMoving() && object->GetMapId() == bot->GetMapId())
        {
            BotPath const path =
                FindBotPath(bot, object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
            atWalkEnd = !(path.type & PATHFIND_NOPATH) &&
                        bot->GetExactDist(path.actualEnd.x, path.actualEnd.y, path.actualEnd.z) <=
                            bot->GetCombatReach() &&
                        !IsWithinInteractionDist(object);
            if (atWalkEnd)
                LOG_INFO("playerbots", "[vendor13] {} L{} npc {} out of reach: at its walk's end, {:.1f} yd from it "
                         "(z {:+.1f})",
                         bot->GetName(), bot->GetLevel(), object->GetName(), bot->GetExactDist(object),
                         object->GetPositionZ() - bot->GetPositionZ());
            // [vendor14] the walk goes to the walk's own end, not a random spot round the npc: b354 Sylaenas L2 walked
            // 7677 yd round vendor Keina in Aldrassil for 1204 s - each MoveWorldObjectTo picked a new spot within
            // reach of Keina's position (another floor of the tree, collision-clipped), the bot stood there, the walk
            // from there ended elsewhere, so it never stood at its walk's end and [vendor13] never fired
            else if (!(path.type & PATHFIND_NOPATH) && path.points.size() >= 2 && !IsWithinInteractionDist(object))
            {
                LOG_INFO("playerbots", "[vendor14] {} L{} walks to the end of its walk to npc {} ({:.1f} yd from it, "
                         "{:.1f} yd off)",
                         bot->GetName(), bot->GetLevel(), object->GetName(),
                         object->GetExactDist(path.actualEnd.x, path.actualEnd.y, path.actualEnd.z),
                         bot->GetExactDist(path.actualEnd.x, path.actualEnd.y, path.actualEnd.z));
                if (MoveTo(bot->GetMapId(), path.actualEnd.x, path.actualEnd.y, path.actualEnd.z, false, false, false,
                           true))
                    return true;
            }
        }
        if (!atWalkEnd && !noCloser && MoveWorldObjectTo(data.npcOrGo))
            return true;
        // [guild15] b319 GO_CAMP "sell junk" STUCK (Kylantino, Gnetlibozz, Rumanatol): no walk reaches the npc from
        // here - the visit ends (a vendor run counts as visited, as when the vendor is gone)
        if (object && (atWalkEnd || noCloser || NpcOutOfWalk(object)))
        {
            if (info.vendorRunEntry)
            {
                if (info.vendorRunReason.find("repair") != std::string::npos)
                {
                    info.repairVisited = true;  // [regress10] its REPAIR try there ends too
                    info.repairVisitMoney = bot->GetMoney();
                }
                info.vendorRunEntry = 0;
                info.vendorVisited = true;
                info.vendorVisitStocked = false;  // [eco193]
                RefuseRestockVendor(bot, info, info.vendorRunLast,  // [eco205]
                                    AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::consumables)));
                info.vendorVisitMap = bot->GetMapId();
                info.vendorMissed = true;  // [vendor23]
                info.vendorVisitBagUsed = AI_VALUE(uint8, "bag space");
                info.vendorVisitMoney = bot->GetMoney();
            }
            info.ChangeToIdle("its vendor visit is over");
            return true;
        }
        // NPC pathing failed (random offset in a wall, mmap hiccup, etc).
        // Take a small random step so the next tick retries from a
        // different spot instead of staring at the NPC from afar.
        return MoveRandomNear(15.0f);
    }

    return true;
}

bool NewRpgDoQuestAction::Execute(Event /*event*/)
{
    NoteQuestUse(bot, 0, 0);  // [qeff31] a quest object's use under way ends here once its cast / loot is over
    if (SettleQuestUse())  // [questitems]
        return true;

    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    uint32 questId = data.questId;
    uint8 questStatus = bot->GetQuestStatus(questId);

    // The quest planner picked a quest that still has to be picked up: walk to its giver. Once in range,
    // SearchQuestGiverAndAcceptOrReward above takes it (and anything else that giver offers).
    if (questStatus == QUEST_STATUS_NONE && data.giverPos != WorldPosition())
    {
        if (data.giverPos.GetMapId() != bot->GetMapId() || bot->GetExactDist(data.giverPos) > 30.0f)
            return MoveFarTo(data.giverPos);

        // [giver15] a giver whose script gives the quest-giver flag: its trigger (a cast, an item, a gossip, a kill
        // near it, its fight), then the quest
        if (TriggerQuestGiver(questId))
            return true;
        // reached the giver but it did not offer the quest (despawned, phased, wrong spawn)
        botAI->lowPriorityQuest.insert(questId);
        botAI->rpgInfo.ChangeToIdle("the giver did not offer q" + std::to_string(questId));
        return true;
    }
    // [questroute] the pickup is done (the giver search took its quests): the route, built again with them, says
    // which stop comes next - not this quest's objective by default
    if (data.giverPos != WorldPosition() && questStatus != QUEST_STATUS_NONE)
    {
        botAI->rpgInfo.ChangeToIdle("it picked up q" + std::to_string(questId) + ", the route says which stop is next");
        return true;
    }

    // The level 74 Dalaran chain: up to the city, the crystal down and back, Archmage Celindra.
    WorldPosition dalaranTarget;
    if (GetDalaranQuestTarget(questId, dalaranTarget))
    {
        if (questStatus == QUEST_STATUS_INCOMPLETE && UseQuestGameObject(questId))
            return true;

        if (bot->GetExactDist(dalaranTarget) > INTERACTION_DISTANCE * 2.0f)
            return MoveFarTo(dalaranTarget);

        // next to Celindra: SearchQuestGiverAndAcceptOrReward above hands the quest in
        return ForceToWait(5 * IN_MILLISECONDS);
    }

    switch (questStatus)
    {
        case QUEST_STATUS_INCOMPLETE:
            return DoIncompleteQuest(data);
        case QUEST_STATUS_COMPLETE:
            return DoCompletedQuest(data);
        default:
            break;
    }
    info.ChangeToNoDecision("q" + std::to_string(questId) + " is in a state it has no step for");
    return true;
}

namespace
{
    // [perf215] coordinator 2026-11-11: the objective spot sweep (the nearest unspent spot with a walk: a route
    // search each) paced over ticks while the bot stands - the walk verdicts found are kept per bot, quest and spot it
    // stands on, a call adds at least one and stops at its search budget; the next call goes on where it stopped (the
    // same spots in the same order, the same verdicts, the same choice). b439 Peny L19: 21 unspent spots of q20, 28
    // searches in one 1052 ms update; Gilduck L14 q263, 1016 ms
    struct SpotSweep
    {
        uint32 questId = 0;
        std::tuple<uint32, int32, int32, int32> from{0, 0, 0, 0};  // map, x, y, z of the bot
        std::vector<std::pair<G3D::Vector3, uint8>> verdicts;      // spot -> SPOT_NO_WALK / _REFUSED / _WALK
        bool pending = false;                                      // stopped at its budget, taken up next call
    };
    constexpr uint8 SPOT_NO_WALK = 0;
    constexpr uint8 SPOT_REFUSED = 1;
    constexpr uint8 SPOT_WALK = 2;
    constexpr uint64 SPOT_SWEEP_SEARCHES_PER_TICK = 2;  // compute pacing per call, not a behaviour rule
    std::mutex spotSweepLock;
    std::unordered_map<uint32, SpotSweep> spotSweeps;
}

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    // [guild15] a quest in the log whose objective needs its tool item used (taken before this check): postponed at
    // once, never a wait for the respawn of something no kill gives credit for (b333 Kianey q6661 580 s STUCK)
    if (std::string why; NewRpgBaseAction::NeedsItemUseCredit(data.quest, &why) ||
                         NewRpgBaseAction::NeedsSpellCredit(bot, data.quest, true, &why))  // [flit]
    {
        LOG_INFO("playerbots", "[rpg] {} L{} POSTPONE q{} reason: unsupported: {}", bot->GetName(), bot->GetLevel(),
                 questId, why);
        NotePostpone(bot, questId, "unsupported: " + why);
        botAI->lowPriorityQuest.insert(questId);
        botAI->rpgInfo.ChangeToIdle("its plan postponed q" + std::to_string(questId));
        return true;
    }
    // [fish14] an item only fishing gives: fished at the nearest water that yields it (the core's own fishing,
    // NewRpgFishing), else the quest is postponed - no sweep of spots where nothing drops it
    if (uint32 fishItem = 0; true)
        if (WorldPosition water; NewRpgBaseAction::FishOnlyItem(bot, data.quest, fishItem, water))
        {
            std::string why;
            if (!NewRpgBaseAction::CanFishFor(bot, fishItem) || water == WorldPosition())
                why = water == WorldPosition() ? "no water on its map yields the item"
                                               : "the item comes from fishing only (no Fishing at its skill or no pole)";
            else
            {
                WorldPosition walkTo;
                NewRpgFishing::Step const step = NewRpgFishing::Tick(botAI, bot, water, walkTo, why);
                if (step == NewRpgFishing::Step::WALK)
                {
                    if (_fishLogged != questId)
                    {
                        _fishLogged = questId;
                        LOG_INFO("playerbots", "[fish14] {} L{} q{} fishes item {} at ({:.0f},{:.0f})", bot->GetName(),
                                 bot->GetLevel(), questId, fishItem, water.GetPositionX(), water.GetPositionY());
                    }
                    return MoveFarTo(walkTo);
                }
                if (step == NewRpgFishing::Step::BUSY)
                    return true;
                if (step == NewRpgFishing::Step::NONE)
                    return false;
                NewRpgFishing::Stop(botAI, bot);
            }
            LOG_INFO("playerbots", "[fish14] {} L{} POSTPONE q{} reason: {}", bot->GetName(), bot->GetLevel(), questId,
                     why);
            NotePostpone(bot, questId, why);
            botAI->lowPriorityQuest.insert(questId);
            botAI->rpgInfo.ChangeToIdle("its plan postponed q" + std::to_string(questId));
            return true;
        }
        else if (_fishLogged == questId)
        {
            _fishLogged = 0;  // [fish14] its catch is in: the pole off, its weapons back
            NewRpgFishing::Stop(botAI, bot);
        }
    // [deaths14] b354 Silver Stream Mine, q416 Rat Catching: Januel L11, Manue L11, Geron L10 died among the tunnel
    // rats - every pull judged 1-2, then the wanderers and patrols of the tunnels walked in one after another (3-5
    // attackers a fight, "no way out clear of other mobs"). At its objective a solo bot standing where no spot within
    // sight is out of every reach, two or more roamers reaching it, leaves the objective to a party: postponed, and
    // the co-op / help seek of a refused spot (RefusedWalk -> NewRpgCoop::OnSpotRefused)
    if (std::string denseWhy; data.pos != WorldPosition() && !bot->isMoving() &&
                              bot->GetMapId() == data.pos.GetMapId() &&
                              bot->GetExactDist(data.pos) <= sPlayerbotAIConfig.SightDistance &&
                              SoloSafety::DenseCamp(bot, denseWhy))
    {
        LOG_INFO("playerbots", "[deaths14] {} L{} q{} leaves its objective to a party: {}", bot->GetName(),
                 bot->GetLevel(), questId, denseWhy);
        return RefusedWalk(data.pos, denseWhy);
    }
    if (data.pos != WorldPosition())
    {
        /// @TODO: extract to a new function
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has completed
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        bool completed = true;
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] < quest->RequiredNpcOrGoCount[currentObjective])
                completed = false;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] <
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                completed = false;
        }
        // [quest13] the explore objective (its index past the kill and item objectives) is done once explored; before,
        // it counted as done at once and the spot was planned again every tick (b353 q62: 2300 sweep lines)
        else if (!q_status.Explored && bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
            completed = false;
        // the current objective is completed, clear and find a new objective later
        if (completed)
        {
            data.lastReachPOI = 0;
            data.pos = WorldPosition();
            data.objectiveIdx = 0;
        }
    }
    if (data.pos == WorldPosition())
    {
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        {
            // [guild90] every camp of the area spent (or taken by other bots): wait here for the first respawn
            // (spawntimesecs) while the cheapest other quest costs more than the wait, else go to that one
            // [guild15] ... unless this resolve postponed it: its unspent camps lie among mobs it does not pull, the
            // spent one comes back empty of what it may fight (b323 Kianey L9 q46: wait 180 s, sweep, spent, postpone,
            // wait again at Stone Cairn Lake for 10 min, 3 STUCK)
            if (botAI->lowPriorityQuest.count(questId))
            {
                botAI->rpgInfo.ChangeToIdle("q" + std::to_string(questId) + " is postponed, the choice runs again");
                return true;
            }
            if (uint32 const wait = EarliestRespawn(bot, questId))
            {
                std::string const resolved = LastResolveStats();  // before OtherQuestSeconds resolves the others
                uint32 other = 0;
                float otherWalk = 0.0f;
                float const otherSeconds = OtherQuestSeconds(questId, other, &otherWalk);
                // [qeff32] the wait is the bot's time idle; another open objective is worked meanwhile unless its
                // walk there and back here costs more than the wait (it comes back at the respawn: NoteResume).
                // Before, it waited whenever the respawn was shorter than the other quest's whole remaining time
                if (!other || float(wait) <= 2.0f * otherWalk)
                {
                    if (FirstWaitLog(bot, questId, wait / 30 + 1))
                    {
                        QuestStats::LogEvent(bot, "wait-respawn", questId,
                                             std::to_string(wait) + "s other=" +
                                                 (other ? "q" + std::to_string(other) + " " +
                                                              std::to_string(uint32(otherSeconds)) + "s"
                                                        : std::string("none")) +
                                                 " " + resolved);
                    }
                    NoteQuestWait(bot, questId, "every objective spot spent: the first respawn");  // [qeff31]
                    // the respawn is the world's clock: re-checked in short steps, the grind and loot strategies
                    // take what appears (and wanderers) meanwhile
                    return ForceToWait(std::min<uint32>(wait, 10) * IN_MILLISECONDS);
                }
                QuestStats::LogEvent(bot, "target-switch", other,
                                     "from=q" + std::to_string(questId) + " wait_s=" + std::to_string(wait) +
                                         " other_s=" + std::to_string(uint32(otherSeconds)) + " walk_back_s=" +
                                         std::to_string(uint32(2.0f * otherWalk)) +
                                         " reason=all camps spent, the other quest is worked until the respawn "
                                         "[qeff32]");
                NoteResume(bot, questId);  // [flit] back to it at its respawn or once the other is done
            }
            // can't find a poi pos to go, stop doing quest for now
            // [questroute] the route leaves it out until its camps' respawn (or the next change of the log)
            InvalidateRoute(bot, questId, "no unspent objective spot");
            botAI->rpgInfo.ChangeToIdle("q" + std::to_string(questId) + " has no unspent objective spot left");
            return true;
        }
        // [guild12] the nearest objective spot the bot can walk to (the leader's route search finds a walk)
        std::sort(poiInfo.begin(), poiInfo.end(), [this](POIInfo const& a, POIInfo const& b)
                  { return bot->GetDistance2d(a.pos.x, a.pos.y) < bot->GetDistance2d(b.pos.x, b.pos.y); });
        float dx = 0.0f, dy = 0.0f, dz = INVALID_HEIGHT;
        bool guessedZ = false;
        int32 objectiveIdx = 0;
        bool reachable = false;
        size_t spent = 0;
        std::optional<WorldPosition> roamerSpot;  // [spot35] the first spot a roamer's round covers
        std::optional<WorldPosition> dangerSpot;  // [danger46] the first spot in one of its danger zones
        std::string roamerWho;
        // [perf215] the sweep taken up where the last call stopped it, else a new one (its verdicts afresh)
        {
            std::lock_guard<std::mutex> guard(spotSweepLock);
            SpotSweep& sweep = spotSweeps[bot->GetGUID().GetCounter()];
            std::tuple<uint32, int32, int32, int32> const from{bot->GetMapId(), int32(bot->GetPositionX()),
                                                               int32(bot->GetPositionY()),
                                                               int32(bot->GetPositionZ())};
            if (!sweep.pending || sweep.questId != questId || sweep.from != from)
            {
                sweep = SpotSweep();
                sweep.questId = questId;
                sweep.from = from;
            }
            sweep.pending = false;
        }
        uint64 const sweepSearchesBefore = Leader::RouteSearchesOnThread();
        uint32 sweepSearched = 0;
        for (POIInfo const& poi : poiInfo)
        {
            if (IsSpentSpot(bot, questId, poi.pos.x, poi.pos.y))  // [guild13]
            {
                ++spent;
                continue;
            }
            dx = poi.pos.x;
            dy = poi.pos.y;
            // [guild90] a spawn's own height when the spot is a spawn (a mine, a cellar, a tree's floor); else the
            // top of the column
            if (poi.z != INVALID_HEIGHT)
                dz = bot->GetMap()->GetHeight(dx, dy, poi.z + 2.0f) != INVALID_HEIGHT
                         ? bot->GetMap()->GetHeight(dx, dy, poi.z + 2.0f)
                         : poi.z;
            else
                // z = MAX_HEIGHT as we do not know accurate z
                dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));
            // [guild13] a grid not loaded yet has no height: the route search snaps the bot's own height to the
            // navmesh column there (the spot is not skipped for it)
            guessedZ = dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE;
            if (guessedZ)
                dz = bot->GetPositionZ();
            // [guild15] a floor the walk can end on: the navmesh polygon at the spot's height, else a real collision
            // floor; the map grid's fallback height is no floor (b333 Mutil, Gnetlibozz: spots 11-22 yd under the bot,
            // "near walk refused: the walk ends where the bot stands")
            // [postpone34] a spot under water is swum to (a sea floor's bones, a lake's chest): the navmesh holds
            // only the water's surface, the grid's height there is the real floor. b386: q4681 Washed Ashore's
            // (6311,857,-14) off Auberdine skipped as "no floor" by every bot - 10 postpones
            else if (!SafeFloorZ(bot, dx, dy, dz) && IsGridFallbackZ(bot, dx, dy, dz) &&
                     !bot->GetMap()->IsInWater(bot->GetPhaseMask(), dx, dy, dz + 1.0f, bot->GetCollisionHeight()))
            {
                LOG_INFO("playerbots", "[rpg] {} L{} SKIP spot of q{} at ({:.0f},{:.0f},{:.0f}): no floor there (the "
                         "map grid's fallback height)",
                         bot->GetName(), bot->GetLevel(), questId, dx, dy, dz);
                continue;
            }
            // [danger46] user 2026-11-09: a spot in one of its danger zones (where it died, a pack it refused) is its
            // quest's own camp grown dangerous - not skipped in silence: when no other spot is left, the objective goes
            // to the camp-help request (a party), as a walled camp does
            if (SoloSafety::Applies(bot) && InDangerZone(bot, dx, dy))
            {
                if (!dangerSpot)
                    dangerSpot = WorldPosition(bot->GetMapId(), dx, dy, dz);
                continue;
            }
            // [perf215] the spot's walk verdict kept by this sweep, else searched within the call's budget
            uint8 verdict = SPOT_NO_WALK;
            bool known = false;
            {
                std::lock_guard<std::mutex> guard(spotSweepLock);
                for (auto const& [at, kept] : spotSweeps[bot->GetGUID().GetCounter()].verdicts)
                    if (at.x == dx && at.y == dy && at.z == dz)
                    {
                        verdict = kept;
                        known = true;
                        break;
                    }
            }
            if (!known)
            {
                if (sweepSearched > 0 &&
                    Leader::RouteSearchesOnThread() - sweepSearchesBefore >= SPOT_SWEEP_SEARCHES_PER_TICK)
                {
                    std::lock_guard<std::mutex> guard(spotSweepLock);
                    spotSweeps[bot->GetGUID().GetCounter()].pending = true;
                    return true;  // the tick is the sweep's: the next call goes on from this spot
                }
                BotPath const spotPath = FindBotPath(bot, dx, dy, dz);
                verdict = (spotPath.type & PATHFIND_NOPATH) ? SPOT_NO_WALK
                          : spotPath.throughRefused        ? SPOT_REFUSED
                                                           : SPOT_WALK;
                ++sweepSearched;
                std::lock_guard<std::mutex> guard(spotSweepLock);
                spotSweeps[bot->GetGUID().GetCounter()].verdicts.emplace_back(G3D::Vector3(dx, dy, dz), verdict);
            }
            if (verdict == SPOT_NO_WALK)
                continue;
            // [sweep33] user 2026-11-09: a spot whose only walk passes the reach of a mob the bot must never fight
            // (orange / red, elite, a danger zone: RouteCosts' refusals, no complete walk round) is no sweep spot -
            // the walk is priced like any other. b385 Kianey L14 (Bami following) swept q116 to a spot 798 yd off
            // through The Darkened Bank's L18-21 spiders and wolves: both died there
            if (verdict == SPOT_REFUSED)
            {
                static std::mutex sweepLogLock;  // one line per bot and spot: a log cadence, no decision reads it
                static std::unordered_map<uint32, std::pair<int32, int32>> sweepLogged;
                bool fresh = false;
                {
                    std::lock_guard<std::mutex> guard(sweepLogLock);
                    auto& last = sweepLogged[bot->GetGUID().GetCounter()];
                    fresh = last != std::make_pair(int32(dx), int32(dy));
                    last = {int32(dx), int32(dy)};
                }
                if (fresh)
                LOG_INFO("playerbots", "[sweep33] {} L{} SKIP spot of q{} at ({:.0f},{:.0f}): its only walk passes the "
                         "reach of a mob it must not fight",
                         bot->GetName(), bot->GetLevel(), questId, dx, dy);
                continue;
            }
            // [lag364] a spot beyond sight whose camp it loses even at full by the static numbers: no walk there
            if (std::string why; SoloSafety::Applies(bot) &&
                                 bot->GetExactDist2d(dx, dy) > sPlayerbotAIConfig.SightDistance &&
                                 StaticCampLoses(bot, dx, dy, dz, why))
            {
                static std::mutex campLogLock;  // one line per bot and spot: a log cadence, no decision reads it
                static std::unordered_map<uint32, std::pair<int32, int32>> campLogged;
                bool fresh = false;
                {
                    std::lock_guard<std::mutex> guard(campLogLock);
                    auto& last = campLogged[bot->GetGUID().GetCounter()];
                    fresh = last != std::make_pair(int32(dx), int32(dy));
                    last = {int32(dx), int32(dy)};
                }
                if (fresh)
                    LOG_INFO("playerbots", "[lag364] {} L{} SKIP spot of q{} at ({:.0f},{:.0f}): {}", bot->GetName(),
                             bot->GetLevel(), questId, dx, dy, why);
                continue;
            }
            // [spot35] user 2026-11-09: no spot in the round of an orange / red or elite roamer (a wanderer's area,
            // a patrol's path, each with its aggro reach): it walks into the bot while it fights or rests there.
            // b386: 11 of 33 deaths "attacked, no judged pull" - Mountain Buzzard L16, Starving Dire Wolf L19, Black
            // Bear Patriarch L16, Vultros L26, Green Recluse L22 came to the bots at their spots
            if (std::string roamer; SoloSafety::Applies(bot) &&
                                    SoloSafety::RoamerReaches(bot, G3D::Vector3(dx, dy, dz), roamer))
            {
                if (!roamerSpot)
                    roamerSpot = WorldPosition(bot->GetMapId(), dx, dy, dz);
                roamerWho = roamer;
                continue;
            }
            objectiveIdx = poi.objectiveIdx;
            reachable = true;
            break;
        }
        if (!reachable && dangerSpot && !roamerSpot)
        {
            std::string const why = "its quest's camp lies in a danger zone of its own (where it died or refused a pack)";
            LOG_INFO("playerbots", "[danger46] {} L{} q{}: {}: asks for help", bot->GetName(), bot->GetLevel(), questId,
                     why);
            NewRpgCoop::OnCampRefused(botAI, bot, *dangerSpot, why);
        }
        if (!reachable && roamerSpot)
        {
            // [spot35] every spot left lies in a roamer's round: the objective goes to the camp-help request
            std::string const why = "every unspent spot lies in the round of " + roamerWho;
            LOG_INFO("playerbots", "[spot35] {} L{} q{}: {}: asks for help", bot->GetName(), bot->GetLevel(),
                     questId, why);
            NewRpgCoop::OnCampRefused(botAI, bot, *roamerSpot, why);
        }
        if (!reachable)
        {
            // [guild90] the spots are every unspent camp of its objectives (spent ones stay skipped until their
            // respawn): none of them has a walk - the quest waits for the next level
            std::string const why = "no walk to any of its " + std::to_string(poiInfo.size()) +
                                    " unspent objective spots";
            LOG_INFO("playerbots", "[rpg] {} L{} POSTPONE q{} reason: {}", bot->GetName(), bot->GetLevel(), questId,
                     why);
            NotePostpone(bot, questId, why);
            botAI->lowPriorityQuest.insert(questId);
            botAI->rpgInfo.ChangeToIdle("its plan postponed q" + std::to_string(questId));
            return true;
        }

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = objectiveIdx;
        {
            std::lock_guard<std::mutex> guard(guessedZLock);
            if (guessedZ)
                guessedZBots.insert(bot->GetGUID().GetCounter());
            else
                guessedZBots.erase(bot->GetGUID().GetCounter());
        }
        NoteQuestWait(bot, 0, "an objective spot is unspent again");  // [qeff31]
        // [guild90] the sweep: next unspent camp nearest the bot, claimed (other bots skip it)
        ClaimSpot(bot, questId, pos);
        QuestStats::LogEvent(bot, "sweep", questId,
                             "obj=" + std::to_string(objectiveIdx) + " to=(" + std::to_string(int32(dx)) + "," +
                                 std::to_string(int32(dy)) + ") " + std::to_string(uint32(bot->GetDistance2d(dx, dy))) +
                                 "yd " + LastResolveStats());
    }

    // [guild15] a spot planned with the bot's own height (its grid had none then) gets the ground's height once that
    // grid is loaded (b327 Bami L9: Kharanos planned at Ironforge's z 504, 100 yd above the ground, 33 WALK_FAIL
    // "on a navmesh island" next to the spot)
    {
        std::lock_guard<std::mutex> guard(guessedZLock);
        auto const guessed = guessedZBots.find(bot->GetGUID().GetCounter());
        float const px = data.pos.GetPositionX();
        float const py = data.pos.GetPositionY();
        if (guessed != guessedZBots.end() && bot->GetMap()->IsGridLoaded(px, py))
        {
            float const ground =
                std::max(bot->GetMap()->GetHeight(px, py, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(px, py));
            if (ground != INVALID_HEIGHT && ground != VMAP_INVALID_HEIGHT_VALUE)
            {
                data.pos = WorldPosition(data.pos.GetMapId(), px, py, ground);
                guessedZBots.erase(guessed);
            }
        }
    }
    // [quest13] the explore objective: the trigger it stands in is reported (its client's report); at the spot the
    // walk goes on into the trigger's radius - the 10 yd of a camp spot is no trigger reach
    if (data.objectiveIdx >= QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
    {
        WorldPosition inside;
        if (ReportExploreTrigger(questId, &inside))
            return true;
        if (inside != WorldPosition() && bot->GetDistance(data.pos) <= 10.0f && !bot->isMoving() &&
            bot->GetExactDist(inside) > 1.0f)
        {
            // the walk's end: where the bot stands already means no walk takes it closer (the spent-spot rule below)
            BotPath const path = FindBotPath(bot, inside.GetPositionX(), inside.GetPositionY(), inside.GetPositionZ());
            if (!(path.type & PATHFIND_NOPATH) &&
                bot->GetExactDist(path.actualEnd.x, path.actualEnd.y, path.actualEnd.z) > bot->GetCombatReach() &&
                MoveTo(inside.GetMapId(), path.actualEnd.x, path.actualEnd.y, path.actualEnd.z))
            {
                LOG_INFO("playerbots", "[quest13] {} L{} q{} explore: walks into its trigger ({:.0f},{:.0f},{:.0f}), "
                         "{:.1f} yd", bot->GetName(), bot->GetLevel(), questId, inside.GetPositionX(),
                         inside.GetPositionY(), inside.GetPositionZ(), bot->GetExactDist(inside));
                return true;
            }
        }
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        // [guild12] a needed quest object passed on the way is looted on the way
        if (UseQuestGameObject(questId))
            return true;
        if (UseQuestTool(questId))  // [questitems] a target of its tool in sight on the way
            return true;
        if (MoveFarTo(data.pos))
            return true;
        // Long-range sampler couldn't land a candidate — nudge the
        // bot a short distance so the next tick retries from a
        // different position instead of sitting idle.
        return MoveRandomNear(10.0f);
    }
    // Now we are near the quest objective
    // kill mobs and looting quest should be done automatically by grind strategy

    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        // [qeff31] the arrival at the objective area
        QuestStats::LogEvent(bot, "arrive", questId,
                             "obj=" + std::to_string(data.objectiveIdx) + " spot=(" +
                                 std::to_string(int32(data.pos.GetPositionX())) + "," +
                                 std::to_string(int32(data.pos.GetPositionY())) + ")");
        return true;
    }
    // [timers] no stay time at the spot: it is left when its objective is done (the completed check above), when
    // nothing at it is left to do (the spent-spot check below: no mob it pulls, no quest object, no fight, no rest)
    // or when the quest is postponed (no walk, unsupported, all camps spent and another quest cheaper than the respawn)

    // [spot88] its objective spot lies in the reach of a mob its judge refused (a camp of mobs it must not fight): the
    // walk there is refused as one through those mobs - its quest postponed and a help party asked where mobs wall it
    // (RefusedWalk), not a stand inside the camp. b410 Mutil L16 on q2038 stood at its objective spot among Gradok /
    // Thragomm / Haren Swifthoof L19-21 (its route "complete, through a refused reach"), the patrol walked in, dead
    if (std::string refusedWho; !bot->IsInCombat() && SoloSafety::InRefusedReach(bot, refusedWho))
    {
        LOG_INFO("playerbots", "[spot88] {} L{} q{}: its objective spot lies in the reach of {} its judge refused: "
                 "postponed",
                 bot->GetName(), bot->GetLevel(), questId, refusedWho);
        return RefusedWalk(data.pos, "its objective spot lies in the reach of " + refusedWho + " (refused)");
    }

    // objects to use for the quest (#2725)
    if (UseQuestGameObject(questId))
        return true;

    if (UseQuestTool(questId))  // [questitems]
        return true;

    if (TalkToQuestNpc(questId))  // [guild15]
        return true;

    if (BuyQuestVendorItem(questId))  // [guild15]
        return true;

    // [guild13] nothing to do at this spot (no mob it pulls, no quest object, no fight, no rest): the spot is spent,
    // the next objective spot is planned - and with none left the quest is postponed (DO_QUEST is no black hole)
    // [qeff32] ... no mob that serves the quest: another mob in reach to grind kept the spot "unspent" for as long as
    // mobs came - b400 Lanbuzz camped q88's Princess (one spawn) 765 s, 20 kills of other mobs, no spent / respawn
    // line; spent now, the spot waits for the Princess's respawn (the map's clock) and the quest yields to another
    // open objective meanwhile (EarliestRespawn against OtherQuestSeconds below)
    if (!bot->IsInCombat() && !ObjectiveTargetHere(bot, questId, AI_VALUE(Unit*, "grind target")) &&
        !FindNeededQuestObject(bot) &&
        !SoloSafety::WantsRest(bot, false) && !SoloSafety::WantsRest(bot, true) && !bot->IsSitState())
    {
        NoteQuestWait(bot, 0, "the spot is spent");  // [qeff31]
        NoteSpentSpot(bot, questId, WorldPosition(bot));  // [guild90] the circle it scanned from where it stands
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        return true;
    }

    // [qeff31] at its spot out of a fight: what holds it there (a target not engaged yet, its rest, a quest object)
    if (bot->IsInCombat())
        NoteQuestWait(bot, 0, "its fight starts");
    else
    {
        Unit* const grind = ObjectiveTargetHere(bot, questId, AI_VALUE(Unit*, "grind target"))
                                ? AI_VALUE(Unit*, "grind target")
                                : nullptr;
        NoteQuestWait(bot, questId,
                      grind ? "at its spot, target " + grind->GetName() + " not engaged"
                      : bot->IsSitState() || SoloSafety::WantsRest(bot, false) || SoloSafety::WantsRest(bot, true)
                          ? std::string("at its spot, resting")
                          : std::string("at its spot, an objective mob or a quest object in reach"));
    }
    // At the POI: keep the bot actively placed but avoid large
    // random 20yd hops that look like pacing back and forth. A small
    // ~8yd wander reads as the bot looking around while grind/loot
    // strategies do their work.
    return MoveRandomNear(8.0f);
}

// [guild15] a talk-to objective at hand: walk up to the npc and talk (gossip hello, and the option its script credits)
bool NewRpgDoQuestAction::TalkToQuestNpc(uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest || bot->IsInCombat())
        return false;
    auto const statusIt = bot->getQuestStatusMap().find(questId);
    if (statusIt == bot->getQuestStatusMap().end())
        return false;
    QuestStatusData const& status = statusIt->second;
    uint32 const counter = bot->GetGUID().GetCounter();
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        int32 const entry = quest->RequiredNpcOrGo[i];
        if (entry <= 0 || status.CreatureOrGOCount[i] >= quest->RequiredNpcOrGoCount[i])
            continue;
        TalkCredit const credit = TalkCreditOf(uint32(entry));
        if (!credit.hello && !credit.select)
            continue;
        Creature* npc = bot->FindNearestCreature(uint32(entry), sPlayerbotAIConfig.SightDistance);
        if (!npc || !npc->IsAlive() || npc->IsInCombat() || bot->IsValidAttackTarget(npc))
            continue;
        {
            std::lock_guard<std::mutex> guard(talkLock);
            if (talked[counter].count({questId, npc->GetSpawnId()}))
                continue;
        }
        if (!bot->IsWithinDistInMap(npc, INTERACTION_DISTANCE))
            return MoveWorldObjectTo(npc->GetGUID(), INTERACTION_DISTANCE - 1.0f);
        bot->StopMoving();
        bot->SetFacingToObject(npc);
        WorldPacket hello(CMSG_GOSSIP_HELLO, 8);
        hello << npc->GetGUID();
        bot->GetSession()->HandleGossipHelloOpcode(hello);
        if (credit.select)
        {
            WorldPacket select(CMSG_GOSSIP_SELECT_OPTION, 8 + 4 + 4);
            select << npc->GetGUID() << credit.menu << credit.option;
            bot->GetSession()->HandleGossipSelectOptionOpcode(select);
        }
        bot->PlayerTalkClass->SendCloseGossip();
        {
            std::lock_guard<std::mutex> guard(talkLock);
            talked[counter].insert({questId, npc->GetSpawnId()});
        }
        LOG_INFO("playerbots", "[rpg] {} L{} talks to {} for q{} {} ({}): objective {} now {}/{}", bot->GetName(),
                 bot->GetLevel(), npc->GetName(), questId, quest->GetTitle(),
                 credit.select ? "gossip option " + std::to_string(credit.option) : std::string("gossip hello"), i,
                 status.CreatureOrGOCount[i], quest->RequiredNpcOrGoCount[i]);
        return true;
    }
    return false;
}

bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    Quest const* quest = data.quest;

    if (data.objectiveIdx != -1)
    {
        // [questroute] done with its objectives: the walk to the ender only when the route hands it in next (its hub
        // batches the turn-ins of the quests done on the way); else the route's next stop
        if (!RouteHandsInNow(questId))
        {
            botAI->rpgInfo.ChangeToIdle("q" + std::to_string(questId) + " is done, its route hands it in later");
            return true;
        }
        // if quest is completed, back to poi with -1 idx to reward
        BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
        botAI->RecordQuestCompleted(questId);
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            // can't find a poi pos to reward, stop doing quest for now
            InvalidateRoute(bot, questId, "no hand-in spot");
            botAI->rpgInfo.ChangeToIdle("no hand-in spot for q" + std::to_string(questId));
            return false;
        }
        assert(poiInfo.size() > 0);
        // now we get the place to get rewarded
        float dx = poiInfo[0].pos.x, dy = poiInfo[0].pos.y;
        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // [guild13] the ender's own spawn when it has one: its floor, not the top of the column (Dandolar L1 stood
        // the whole night under Ayanna Everstride, who stands on an upper floor of Aldrassil: the height from
        // MAX_HEIGHT was the tree's top), and no height lookup on a grid not loaded yet (b294-b297: 20 bots at the
        // intro hand-ins q2160 / q2158, 500 yd off, failed the action every tick on GetHeight's INVALID_HEIGHT)
        WorldPosition ender;
        bool const hasEnder = NearestEnderSpot(bot, questId, ender);

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (!hasEnder && (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE))
            return false;

        WorldPosition pos = hasEnder ? ender : WorldPosition(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = -1;
    }

    if (data.pos == WorldPosition())
        return false;

    // [quest23] the hand-in walk reached another landmass than its route was built on (off the boat, out of a
    // portal): the route is built again there - the quests done or open by it get their stops (RouteKey)
    if (QuestRoute::BotRoute const& route = QuestRoute::Of(bot->GetGUID().GetCounter());
        route.built && route.key != RouteKey() && !bot->GetTransport() && !bot->IsInFlight() &&
        botAI->rpgInfo.trip.phase == NewRpgInfo::TripPhase::None)
    {
        LOG_INFO("playerbots", "[quest23] {} L{} q{} hand-in walk on a landmass its route was not built on: the "
                 "route is built again here",
                 bot->GetName(), bot->GetLevel(), questId);
        CurrentRoute();  // built now (its key the bot's again): the IDLE choice reads it, no second pass here
        botAI->rpgInfo.ChangeToIdle("its route is built again on this landmass");
        return true;
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        bool const walks = MoveFarTo(data.pos);
        // [quest19] the hand-in walk: a line when the bot is farther from the ender than the nearest it came by a
        // sight, or the walk returns none - the target, the walk's state, where the last move went. b363 Manue L12
        // q1715: 804 s in DO_QUEST for Gakin in Stormwind's Mage Quarter, its kills drifting through Elwynn, no line
        {
            static std::mutex handLock;
            static std::unordered_map<uint32, std::tuple<uint32, float, float>> handWalk;  // quest, nearest, logged
            // [quest20] the walk still to go along its far route (its rest from the point nearest the bot, then on
            // to the ender), not the straight line: a route that first leaves the ender's direction (the Rut'theran
            // boat, Ironforge's gate) is no walk away. b367: 14 [quest19] lines, nearly all such detours (Dandolar
            // to Darkshore over the boat, Namwe out of Ironforge)
            float d = bot->GetDistance(data.pos);
            if (std::vector<G3D::Vector3> const& route = botAI->rpgInfo.farRoute; route.size() >= 2)
            {
                G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
                size_t nearestPoint = 0;
                float bestSq = std::numeric_limits<float>::max();
                for (size_t i = 0; i < route.size(); ++i)
                    if (float const sq = (route[i] - here).squaredLength(); sq < bestSq)
                    {
                        bestSq = sq;
                        nearestPoint = i;
                    }
                float rest = std::sqrt(bestSq);
                for (size_t i = nearestPoint + 1; i < route.size(); ++i)
                    rest += (route[i] - route[i - 1]).length();
                rest += data.pos.GetExactDist(route.back().x, route.back().y, route.back().z);
                d = rest;
            }
            bool log = false;

            float nearest = d;
            {
                std::lock_guard<std::mutex> guard(handLock);
                auto& [quest, best, logged] = handWalk[bot->GetGUID().GetCounter()];
                if (quest != data.questId)
                {
                    quest = data.questId;
                    best = d;
                    logged = 0.0f;
                }
                best = std::min(best, d);
                nearest = best;
                if ((!walks || d > best + sPlayerbotAIConfig.SightDistance) && d > logged + 50.0f)
                {
                    logged = d;
                    log = true;
                }
            }
            if (log)
            {
                LastMovement const& last = AI_VALUE(LastMovement&, "last movement");
                LOG_INFO("playerbots",
                         "[quest19] {} L{} q{} hand-in walk: ender at map {} ({:.0f},{:.0f},{:.0f}) {:.0f} yd to go, "
                         "nearest {:.0f} yd; walk {}; far route {} points; last move to ({:.0f},{:.0f}); status {}",
                         bot->GetName(), bot->GetLevel(), data.questId, data.pos.GetMapId(),
                         data.pos.GetPositionX(), data.pos.GetPositionY(), data.pos.GetPositionZ(), d, nearest,
                         walks ? "under way" : "none (" + s_moveRefusal + ")", botAI->rpgInfo.farRoute.size(),
                         last.lastMoveToX, last.lastMoveToY, botAI->rpgInfo.ToString());
            }
        }
        if (walks)
            return true;
        return MoveRandomNear(10.0f);
    }

    // Now we are near the qoi of reward

    // the quest should be rewarded by SearchQuestGiverAndAcceptOrReward
    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // [guild15] at the ender's spot and the giver search (SearchQuestGiverAndAcceptOrReward, first in Execute) did
    // not hand it in: why, from the game's own checks - never a wait on a clock (b333 Rollothan q83, Aevmae q9616
    // stood 300 s each at the ender, then the quest was dropped)
    return HandInBlocked(data);
}

// [guild15] an item objective a vendor in sight sells (b327-b333 Kharanos: q384's Rhapsody Malt from Innkeeper Belm -
// 3-4 bots a run stood at the inn, the spot "spent" with no spawn of its source, silent STUCK): bought as a player
// does, on its own money; a vendor it cannot buy from (money, a failed buy before) is logged and the quest postponed
bool NewRpgDoQuestAction::BuyQuestVendorItem(uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest || bot->IsInCombat())
        return false;
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        uint32 const itemId = quest->RequiredItemId[i];
        if (!itemId || bot->GetItemCount(itemId, true) >= quest->RequiredItemCount[i])
            continue;
        for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible new rpg targets"))
        {
            Creature* npc = botAI->GetCreature(guid);
            VendorItemData const* items = npc && npc->IsAlive() ? npc->GetVendorItems() : nullptr;
            if (!items || !items->FindItemCostPair(itemId, 0))
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            uint32 const cost = proto ? proto->BuyPrice : 0;
            std::string why;
            if (NewRpgProfession::BuyFailed(bot, npc->GetEntry(), itemId))
                why = "the buy at " + npc->GetName() + " failed before";
            else if (bot->GetMoney() < cost)
                why = Acore::StringFormat("{} costs {}c at {}, it has {}c", proto->Name1, cost, npc->GetName(),
                                          bot->GetMoney());
            if (!why.empty())
            {
                LOG_INFO("playerbots", "[rpg] {} L{} POSTPONE q{} reason: its item objective is sold, not dropped: {}",
                         bot->GetName(), bot->GetLevel(), questId, why);
                NotePostpone(bot, questId, why);
                botAI->lowPriorityQuest.insert(questId);
                botAI->rpgInfo.ChangeToIdle("its plan postponed q" + std::to_string(questId));
                return true;
            }
            LOG_INFO("playerbots", "[rpg] {} L{} QUEST_BUY q{}: {} from {} ({}c)", bot->GetName(), bot->GetLevel(),
                     questId, proto ? proto->Name1 : std::to_string(itemId), npc->GetName(), cost);
            NewRpgProfession::StartBuy(botAI, bot, itemId, npc->GetEntry(), WorldPosition(npc), cost);
            return true;
        }
        // [vendor60] no seller in sight: an item only vendors sell is bought at the nearest one on its map (b400
        // Kylanor / Ravelandy L14 on q116 Dry Times: its kegs and casks from Barkeep Hann / Roberto Pupellyverbos,
        // never sought - the objective walk led to no vendor)
        uint32 npcEntry = 0;
        WorldPosition at;
        std::string why;
        if (!NewRpgProfession::QuestVendor(bot, itemId, npcEntry, at, why))
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        uint32 const cost = proto ? proto->BuyPrice : 0;
        if (why.empty() && bot->GetMoney() < cost)
            why = Acore::StringFormat("{} costs {}c, it has {}c", proto ? proto->Name1 : "?", cost, bot->GetMoney());
        if (!why.empty())
        {
            LOG_INFO("playerbots", "[rpg] {} L{} POSTPONE q{} reason: [vendor60] its item objective is sold, not "
                     "dropped: {}",
                     bot->GetName(), bot->GetLevel(), questId, why);
            NotePostpone(bot, questId, why);
            botAI->lowPriorityQuest.insert(questId);
            botAI->rpgInfo.ChangeToIdle("its plan postponed q" + std::to_string(questId));
            return true;
        }
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(npcEntry);
        LOG_INFO("playerbots", "[rpg] {} L{} QUEST_BUY q{}: [vendor60] {} from {} {:.0f} yd off ({}c)", bot->GetName(),
                 bot->GetLevel(), questId, proto ? proto->Name1 : std::to_string(itemId), tmpl ? tmpl->Name : "?",
                 bot->GetExactDist(at), cost);
        NewRpgProfession::StartBuy(botAI, bot, itemId, npcEntry, at, cost);
        return true;
    }
    return false;
}

bool NewRpgDoQuestAction::HandInBlocked(NewRpgInfo::DoQuest& data)
{
    uint32 const questId = data.questId;
    Quest const* quest = data.quest ? data.quest : sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;
    auto const postpone = [&](std::string const& why)
    {
        LOG_INFO("playerbots", "[rpg] {} L{} HANDIN_BLOCKED q{} {}: {}", bot->GetName(), bot->GetLevel(), questId,
                 quest->GetTitle(), why);
        NotePostpone(bot, questId, why);
        botAI->lowPriorityQuest.insert(questId);
        botAI->RecordQuestAbandoned(questId, QuestAbandonReason::CannotTurnIn);
        botAI->rpgInfo.ChangeToIdle("q" + std::to_string(questId) + " cannot be handed in");
        return true;
    };
    // the core's reward checks (Player::CanRewardQuest): the quest items, the money it takes, the daily limit
    if (!bot->CanRewardQuest(quest, false))
    {
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemCount[i] &&
                bot->GetItemCount(quest->RequiredItemId[i]) < quest->RequiredItemCount[i])
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]);
                return postpone(Acore::StringFormat("its item {} is not in the bags ({} of {})",
                                                    proto ? proto->Name1 : std::to_string(quest->RequiredItemId[i]),
                                                    bot->GetItemCount(quest->RequiredItemId[i]),
                                                    quest->RequiredItemCount[i]));
            }
        if (quest->GetRewOrReqMoney() < 0 && !bot->HasEnoughMoney(-quest->GetRewOrReqMoney()))
            return postpone(Acore::StringFormat("the turn-in takes {}c, it has {}c", -quest->GetRewOrReqMoney(),
                                                bot->GetMoney()));
        return postpone("the core refuses the reward (already rewarded, or a daily / weekly limit)");
    }
    // the reward items must fit its bags (Player::CanRewardQuest with the reward: the choice and the fixed items)
    std::vector<std::pair<uint32, uint32>> rewards;
    if (quest->GetRewChoiceItemsCount() > 0 && quest->RewardChoiceItemId[0])
        rewards.emplace_back(quest->RewardChoiceItemId[0], quest->RewardChoiceItemCount[0]);
    for (uint32 i = 0; i < quest->GetRewItemsCount(); ++i)
        if (quest->RewardItemId[i])
            rewards.emplace_back(quest->RewardItemId[i], quest->RewardItemIdCount[i]);
    for (auto const& [itemId, count] : rewards)
    {
        ItemPosCountVec dest;
        InventoryResult const res = bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count);
        if (res == EQUIP_ERR_OK)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        std::string const name = proto ? proto->Name1 : std::to_string(itemId);
        if (res == EQUIP_ERR_INVENTORY_FULL || res == EQUIP_ERR_BAG_FULL)
        {
            // a free slot first: what it may sell goes to a vendor now (its visit before was at the same fill).
            // [vendor20] ... unless that visit at this fill freed nothing: the bags hold nothing a vendor takes, a new
            // run would free nothing again. b367 Thamesh L11 q419: 17 yd from its ender, six runs to Gothor Brumn
            // (218-595 yd), "bags 100% -> 100%" each, the hand-in never made
            NewRpgInfo& info = botAI->rpgInfo;
            if (!(info.vendorVisited && AI_VALUE(uint8, "bag space") <= info.vendorVisitBagUsed) ||
                sGuildCoordinator.HasMarkedSale(bot))  // the leader's bag relief marked what goes now
                info.vendorVisited = false;
            else
                LOG_INFO("playerbots", "[vendor20] {} L{} q{}: its last vendor visit at this fill freed no slot - no "
                         "run again",
                         bot->GetName(), bot->GetLevel(), questId);

            if (StartVendorRun())
            {
                LOG_INFO("playerbots", "[rpg] {} L{} HANDIN_BLOCKED q{} {}: the bags hold no slot for {}, to a vendor "
                         "first", bot->GetName(), bot->GetLevel(), questId, quest->GetTitle(), name);
                return true;
            }
            return postpone("the bags hold no slot for its reward " + name + " and nothing in them is for sale");
        }
        return postpone(Acore::StringFormat("its reward {} cannot be stored (equip error {})", name, uint32(res)));
    }
    // rewardable: the ender in sight, and why the giver search passed it
    for (char const* candidates : {"possible new rpg targets", "possible new rpg game objects"})
        for (ObjectGuid const& guid : AI_VALUE(GuidVector, candidates))
        {
            WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
            if (!object || !object->hasInvolvedQuest(questId))
                continue;
            if (botAI->rpgInfo.ignoredQuestGivers.count(guid))
                return postpone("its ender " + object->GetName() + " was given up: no walk reaches it");
            if (!NewRpgHelpers::CanInteractWithQuestGiver(bot, object))
            {
                // a dead ender respawns (the world's clock); the rest (hostile, no quest flag now) does not change
                if (Creature* creature = object->ToCreature(); creature && !creature->IsAlive())
                {
                    time_t const now = time(nullptr);
                    uint32 const left =
                        creature->GetRespawnTimeEx() > now ? uint32(creature->GetRespawnTimeEx() - now) : 1;
                    thread_local std::unordered_map<uint32, ObjectGuid> loggedDead;  // one line per bot and ender
                    ObjectGuid& last = loggedDead[bot->GetGUID().GetCounter()];
                    if (last != guid)
                    {
                        last = guid;
                        LOG_INFO("playerbots", "[rpg] {} L{} hand-in q{}: its ender {} is dead, respawns in {} s",
                                 bot->GetName(), bot->GetLevel(), questId, object->GetName(), left);
                    }
                    return ForceToWait(std::min<uint32>(left, 10) * IN_MILLISECONDS);
                }
                return postpone("its ender " + object->GetName() + " does not talk to it (hostile or no quest flag)");
            }
            // the giver search looks only so far (QUEST_GIVER_SEARCH_DISTANCE): walk up to the ender itself; within
            // it the search should have taken it, so that is a computed failure
            if (bot->GetDistance(object) <= NewRpgHelpers::QUEST_GIVER_SEARCH_DISTANCE)
                return postpone("its ender " + object->GetName() + " is in reach of the giver search, which does not " +
                                "take the turn-in");
            data.pos = WorldPosition(object);
            data.lastReachPOI = 0;
            LOG_INFO("playerbots", "[rpg] {} L{} hand-in q{}: walks up to its ender {} ({:.0f} yd)", bot->GetName(),
                     bot->GetLevel(), questId, object->GetName(), bot->GetExactDist(object));
            return true;
        }
    return postpone("no ender of it in sight at its spot (despawned, phased or elsewhere)");
}

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    if (bot->IsInFlight())
    {
        data.inFlight = true;
        ContinueCrossMapTaxi();
        return false;
    }

    if (bot->GetDistance(data.flightMasterPos) > INTERACTION_DISTANCE)
        return MoveFarTo(data.flightMasterPos);

    Creature* flightMaster = bot->FindNearestCreature(data.flightMasterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
    {
        info.ChangeToIdle("its flight master is gone from its spawn");
        return true;
    }
    if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
        return MoveFarTo(flightMaster);

    std::vector<uint32> nodes = data.path;

    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();

    bot->GetSession()->SendLearnNewTaxiNode(flightMaster);

    WorldPacket taxiPacket(CMSG_ACTIVATETAXIEXPRESS, 8 + 4 + 4 * nodes.size());
    taxiPacket << flightMaster->GetGUID() << uint32(nodes.size());
    for (uint32 node : nodes)
        taxiPacket << node;
    RelayPacketToGroupBots(taxiPacket);

    GuildLedger::Cause const ledger("flight");  // [eco190]
    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes.empty() ? 0 : nodes.front(), nodes.empty() ? 0 : nodes.back());
        info.ChangeToIdle("its flight could not be started");
        return true;
    }
    return true;
}

void NewRpgTravelFlightAction::ContinueCrossMapTaxi()
{
    if (bot->IsBeingTeleported())
        return;

    if (!bot->movespline->Finalized())
        return;

    MotionMaster* mm = bot->GetMotionMaster();
    if (!mm || mm->GetCurrentMovementGeneratorType() != FLIGHT_MOTION_TYPE)
        return;

    // Check if we are at our destination.
    uint32 nextDest = bot->m_taxi.GetTaxiDestination();
    if (!nextDest)
        return;

    // Confirm next node needs different map.
    TaxiNodesEntry const* nextNode = sTaxiNodesStore.LookupEntry(nextDest);
    if (!nextNode || nextNode->map_id == bot->GetMapId())
        return;

    FlightPathMovementGenerator* flight = dynamic_cast<FlightPathMovementGenerator*>(mm->top());
    if (!flight)
        return;

    LOG_DEBUG("playerbots", "[New RPG] {} continuing taxi across map boundary (next node {} on map {})",
              bot->GetName(), nextDest, nextNode->map_id);

    flight->SetCurrentNodeAfterTeleport();

    if (flight->HasArrived())
        return;

    TaxiPathNodeEntry const* node = flight->GetPath()[flight->GetCurrentNode()];
    flight->SkipCurrentNode();

    bot->TeleportTo(nextNode->map_id, node->x, node->y, node->z, bot->GetOrientation(), TELE_TO_NOT_LEAVE_TAXI);
}

bool NewRpgWaitForGroupAction::Execute(Event /*event*/)
{
    // [wait14] a member beyond the group reward distance that comes to the leader (its follow, no plan of its own).
    // [wait20] THE LEADER NO LONGER WALKS TO MEET IT (user 2026-11-15) - it records what the member is doing at its
    // own end instead. It used to walk its own routed walk there, and that is how b354's Orton L11 stood 928 s in
    // Elwynn for Febineysh L7 4 551 yd away in Loch Modan, and Sycina L11 1 074 s for Nelaeleas 3 849 yd off: those
    // distances are not lags that a walk closes, they are causes at the other end (a stalled follow, a graveyard, a
    // quest of its own), and the walk only took the group away from its work. The row below names the cause.
    NewRpgInfo& info = botAI->rpgInfo;
    // [wait19] coordinator 2026-11-09: only while the group has a purpose that needs them together - the co-op / help
    // party both stand in, its leader this bot. A group with no such purpose is left by its member ([group18] /
    // [group19]), not walked to: b363 Sehdrurmund walked to meet Cassand 4796 yd off, Sycina Ryarii 3912, Ginde Ganna
    // 3139, Septimon Kylanor 2281, Tuihgrelfim Peny 1819 (173 maze give-ups), all from groups an earlier session left
    ObjectGuid memberParty;
    Player* behind = info.groupWaitBehind ? ObjectAccessor::FindPlayer(info.groupWaitBehind) : nullptr;
    // (or a guild five the global leader formed, gathering at its entrance or heading in: coordinator 2026-11-09)
    bool const purpose = behind && ((NewRpgCoop::PartyLeaderOf(behind->GetGUID().GetCounter(), memberParty) &&
                                     memberParty == bot->GetGUID()) ||
                                    sGuildCoordinator.InSameActiveFive(bot->GetGUID().GetCounter(),
                                                                       behind->GetGUID().GetCounter()));
    if (behind && !purpose && _meetLogged != behind->GetGUID())
    {
        _meetLogged = behind->GetGUID();
        LOG_INFO("playerbots", "[wait19] {} L{} does not walk to meet {} L{} {:.0f} yd behind: no party of its holds "
                 "it",
                 bot->GetName(), bot->GetLevel(), behind->GetName(), behind->GetLevel(), bot->GetExactDist(behind));
    }
    // [lag320] a meet walk only where a walk joins the two: the same landmass (TravelRouteMgr::Landmass) and neither
    // on a transport. Across a ride (the Deeprun Tram's platforms, a boat's piers) the member's own trip brings it;
    // the leader's ride towards it crossed the member's ride the other way. b435: Rollothan L13 (help party with
    // Geefeelli L18, q399) rode the tram back and forth for 47 min, 148 "travels by" legs: each stop it "walks to meet"
    // Geefeelli on the other platform while Geefeelli boarded the car the other way
    bool const walkJoins =
        behind && !behind->GetTransport() && !bot->GetTransport() &&
        TravelRouteMgr::Landmass(WorldPosition(behind)) == TravelRouteMgr::Landmass(WorldPosition(bot));
    if (behind && purpose && !walkJoins && _meetLogged != behind->GetGUID())
    {
        _meetLogged = behind->GetGUID();
        LOG_INFO("playerbots", "[lag320] {} L{} does not walk to meet {} L{} {:.0f} yd behind: {} - it waits for "
                 "the member's own trip",
                 bot->GetName(), bot->GetLevel(), behind->GetName(), behind->GetLevel(), bot->GetExactDist(behind),
                 behind->GetTransport() || bot->GetTransport() ? "a transport carries one of them"
                                                               : "another landmass (only a ride joins them)");
    }
    // [wait20] USER 2026-11-15: THE GROUP IS NOT WALKED AFTER A MEMBER THAT IS BEHIND. Measured over the live
    // `wait_behind` rows: 17 of 19 cases were a member more than a KILOMETRE away (4 038 yd the farthest - Rollothan
    // for Alnia, 16 rows of one episode; Lanbuzz for Nelaeleas 4 037 yd inside one zone; Relara for Dekorhan
    // 6 894 yd), and one episode ran 1 073 s. Four kilometres is not a lag: it is the other end of the world, and
    // whatever put the member there is a cause of its own at that end. The leader's walk could never answer it - it
    // only moved the whole group away from what it was doing - so what used to be a walk is now a RECORD of the
    // member: where it is, whether it is walking at all, where to, and the status it is running. The wait itself and
    // every other rule here (the purpose check [wait19], the landmass check [lag320]) are untouched.
    if (Player* member = purpose && walkJoins ? behind : nullptr;
        member && member->IsAlive() && member->IsInWorld() && member->GetMapId() == bot->GetMapId() &&
        !bot->IsInCombat())
    {
        if (_meetLogged != member->GetGUID())
        {
            _meetLogged = member->GetGUID();
            PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
            // what the member itself is doing, in its own domain's words, and where its own walk was headed
            std::string const doing = memberAI ? std::string(NewRpgInfo::StatusName(memberAI->rpgInfo.GetStatus()))
                                               : std::string("human playing");
            WorldPosition const headed = memberAI ? memberAI->rpgInfo.moveFarPos : WorldPosition();
            bool const headedSomewhere = headed != WorldPosition();
            LOG_INFO("playerbots", "[wait20] {} L{} does NOT walk to meet {} L{} {:.0f} yd behind: its own cause is "
                     "at its end - {} at ({:.0f},{:.0f}) zone {}, {}{}",
                     bot->GetName(), bot->GetLevel(), member->GetName(), member->GetLevel(),
                     bot->GetExactDist(member), doing, member->GetPositionX(), member->GetPositionY(),
                     member->GetZoneId(), member->isMoving() ? "walking" : "standing",
                     headedSomewhere ? Acore::StringFormat(" to ({:.0f},{:.0f})", headed.GetPositionX(),
                                                           headed.GetPositionY())
                                     : std::string());

            // [jrn40] the same event as a row. `_meetLogged` is the criterion and it is the domain's own: one row
            // per member walked to, not one per tick of the walk - b354 had a bot stand 928 s for one member, and a
            // row per tick would have made that one problem into thousands.
            Journal::Stuck stuck;
            stuck.kind = "wait_behind";
            stuck.botName = bot->GetName();
            stuck.botLow = bot->GetGUID().GetCounter();
            stuck.botLevel = bot->GetLevel();
            stuck.mapId = bot->GetMapId();
            stuck.zoneId = bot->GetZoneId();
            stuck.x = bot->GetPositionX();
            stuck.y = bot->GetPositionY();
            stuck.z = bot->GetPositionZ();
            stuck.targetName = member->GetName();
            stuck.targetLow = member->GetGUID().GetCounter();
            stuck.targetLevel = member->GetLevel();
            stuck.targetYd = bot->GetExactDist(member);
            // [jrn43] the member's OWN zone, so that the key's cause field below can be checked by a query
            stuck.targetZoneId = member->GetZoneId();
            // [wait20] the other end of the gap, in columns: a row about a gap that cannot say where the other one
            // is and what it is doing names no cause, and naming the cause is now all this row is for
            stuck.targetX = member->GetPositionX();
            stuck.targetY = member->GetPositionY();
            stuck.targetZ = member->GetPositionZ();
            stuck.targetMoving = member->isMoving();
            stuck.targetDoing = doing;
            if (headedSomewhere)
            {
                stuck.targetToX = headed.GetPositionX();
                stuck.targetToY = headed.GetPositionY();
                stuck.targetToZ = headed.GetPositionZ();
            }
            stuck.inCombat = false;  // the guard above requires it
            stuck.why = Acore::StringFormat(
                "a member is beyond the group reward distance and its own follow does not close the gap; this bot "
                "does NOT walk to meet it ([wait20]): it is {} at ({:.0f},{:.0f}) in zone {} and {}",
                doing, member->GetPositionX(), member->GetPositionY(), member->GetZoneId(),
                member->isMoving() ? "walking" : "standing");
            stuck.runId = Leader::Runs::OfBot(bot);
            uint64 const stuckRowId = Journal::RecordStuck(stuck);

            Journal::Issue issue;
            issue.kind = "wait_behind";
            issue.severity = "mid";
            // A property of the world over time (where two bots happen to be and whether a follow closes): there
            // is nothing to replay offline, so a run without the sign is the proof, not a test.
            issue.reproClass = "not_offline";
            // the key is the shape, not the pair: the two names and the distance are what would fan one cause into
            // hundreds. [jrn43] AND ITS ONE CAUSE FIELD COULD NOT COME OUT FALSE: the field was whether both are on
            // one map, while the guard four lines above requires `member->GetMapId() == bot->GetMapId()` - so it
            // read "same map" in all 43 live rows and in every row this kind could ever write. A statement that
            // cannot be false is counted here as a defect of its own, and it left the kind with no cause at all:
            // 84 yd behind in the bot's own zone and 6 894 yd behind across the map shared one row (43 rows over 7
            // zones, 14 bots, 16 members). The zone is the same fact one step finer and it does differ.
            issue.dedupKey = Journal::Key::WaitBehind(member->GetZoneId() == bot->GetZoneId());
            issue.why = Acore::StringFormat("{} waits for {} {:.0f} yd behind ({} at ({:.0f},{:.0f}) zone {}, {})",
                                            bot->GetName(), member->GetName(), *stuck.targetYd, doing,
                                            member->GetPositionX(), member->GetPositionY(), member->GetZoneId(),
                                            member->isMoving() ? "walking" : "standing");
            issue.realWhen = "the member is outside the group reward distance, out of combat, and its own follow "
                             "has not closed the gap";
            issue.real = "yes";
            // [wait20] and the defect is now at the member's end, which is where it always was: the leader walking
            // kilometres only moved the group away from its own work
            issue.realWhy = "until the gap closes neither bot earns group credit; the leader no longer walks to it, "
                            "so the cause to fix is whatever holds the member where it is";
            issue.realSource = "emitter";
            issue.value = double(*stuck.targetYd);
            issue.valueUnit = "yd";
            issue.botName = stuck.botName;
            issue.botLow = stuck.botLow;
            issue.mapId = stuck.mapId;
            issue.zoneId = stuck.zoneId;
            issue.x = stuck.x;
            issue.y = stuck.y;
            issue.z = stuck.z;
            issue.runId = stuck.runId;
            issue.sourceTable = "pb_journal_stuck";
            issue.sourceId = stuckRowId;
            Journal::RecordIssue(issue);
        }
        // [wait20] and no walk follows: the bot waits where it is (out of every idle mob's reach, below), and the
        // row above is what this action produces. `NewRpgInfo::meetWalk` went with the walk - it had no other
        // writer, and a flag nothing can set is worse than none, because its readers look like live rules
    }
    else if (!behind)
        _meetLogged.Clear();

    // [death436] the wait is taken out of every idle mob's reach (its walk priced a run past, not a stand there): b461
    // Brelir L24 met Thyllyn inside Zoram'gar Outpost, both stood in a Splintertree Guard's (L40) reach, it engaged
    // Thyllyn; b462 Brelir logged in there and died to it
    if (_stepOut && bot->isMoving() && !bot->IsInCombat())
        return true;
    _stepOut = false;
    if (!bot->IsInCombat())
    {
        std::string who;
        std::vector<G3D::Vector3> spots;
        if (SoloSafety::StandsInReach(bot, who, spots))
            for (G3D::Vector3 const& at : spots)
            {
                Leader::RouteResult const route = Leader::Route(bot, Position(at.x, at.y, at.z));
                if (!route.found || !route.complete || route.throughRefused)
                    continue;
                if (!MoveTo(bot->GetMapId(), at.x, at.y, at.z, false, false, false, true))
                    continue;
                LOG_INFO("playerbots", "[death436] {} L{} waits for its group out of the reach of {}: steps to "
                         "({:.0f},{:.0f}), {:.0f} yd walk",
                         bot->GetName(), bot->GetLevel(), who, at.x, at.y, route.length);
                _stepOut = true;
                return true;
            }
        if (!who.empty())
            SoloSafety::NoteNoWayOut(bot, who, spots.size(), "group wait");  // [deaths140]
    }

    if (bot->isMoving())
        bot->StopMoving();

    return ForceToWait(2 * IN_MILLISECONDS);
}

bool NewRpgRestAtInnAction::isUseful()
{
    return bot->HasRestFlag(REST_FLAG_IN_TAVERN) ||
           sTravelRouteMgr.GetNearestInn(bot, INN_SEARCH_DISTANCE) != WorldPosition();
}

bool NewRpgRestAtInnAction::Execute(Event /*event*/)
{
    WorldPosition const inn = sTravelRouteMgr.GetNearestInn(bot, INN_SEARCH_DISTANCE);
    if (bot->HasRestFlag(REST_FLAG_IN_TAVERN) || (inn != WorldPosition() && bot->GetExactDist(inn) <= INN_REACHED))
    {
        if (bot->isMoving())
            bot->StopMoving();

        if (!bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_SIT);

        return ForceToWait(10 * IN_MILLISECONDS);
    }

    if (inn == WorldPosition())
        return false;

    return MoveFarTo(inn);
}

bool NewRpgTaxiUnstallAction::Execute(Event /*event*/)
{
    bool recovered = false;
    TaxiStallWatch::ForEachWatched(bot, [&recovered](Player* player, NewRpgInfo& info)
    {
        // [timers] the stall proven by TaxiStallWatch::IsStalled (the trigger's check), no stall timer
        if (!info.taxiStalled || !player->IsInFlight())
            return;

        recovered = true;
        info.taxiStalled = false;
        info.taxiStillSince = 0;
        MotionMaster* mm = player->GetMotionMaster();
        FlightPathMovementGenerator* flight = nullptr;
        if (mm && mm->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE)
            flight = dynamic_cast<FlightPathMovementGenerator*>(mm->top());

        // Re-request: relaunch the spline from the node the flight is at.
        if (flight && !flight->HasArrived() && info.taxiRecoveries < MAX_RESTARTS)
        {
            ++info.taxiRecoveries;
            LOG_DEBUG("playerbots", "[New RPG] {} taxi stalled, restarting the flight (attempt {})",
                      player->GetName(), info.taxiRecoveries);
            flight->DoReset(player);
            return;
        }

        // Fall back: end the flight here and set the bot down on the ground below; it walks on from there.
        LOG_DEBUG("playerbots", "[New RPG] {} taxi stalled for good, ending the flight", player->GetName());
        info.taxiRecoveries = 0;
        if (mm)
            mm->Clear();

        player->CleanupAfterTaxiFlight();
        float const x = player->GetPositionX();
        float const y = player->GetPositionY();
        float const z = player->GetPositionZ();
        float const ground = player->GetMap()->GetHeight(player->GetPhaseMask(), x, y, z, true, GROUND_SEARCH);
        // [nav24] the set-down is the navmesh's floor under the flight (its nearest polygon below), not the first
        // height the collision ray meets: a model's footprint has terrain under it and no polygon (b350 Blahadrin's
        // 12x12 yd hole south of Goldshire); no polygon below: the ground as before, logged
        float landX = x, landY = y, landZ = ground;
        bool onMesh = false;
        if (dtNavMeshQuery const* query = player->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery())
        {
            dtQueryFilter filter;
            filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
            float const point[3] = {y, ground > INVALID_HEIGHT ? ground : z, x};
            float const extents[3] = {10.0f, 30.0f, 10.0f};
            float nearest[3] = {0.0f, 0.0f, 0.0f};
            dtPolyRef ref = 0;
            if (dtStatusSucceed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) && ref)
            {
                landX = nearest[2];
                landY = nearest[0];
                landZ = nearest[1];
                onMesh = true;
            }
        }
        LOG_INFO("playerbots", "[nav24] {} taxi stall set-down from ({:.1f},{:.1f},{:.1f}) to ({:.1f},{:.1f},{:.1f}){}",
                 player->GetName(), x, y, z, landX, landY, landZ, onMesh ? "" : " (no polygon below: the ground)");
        if ((onMesh || ground > INVALID_HEIGHT) && landZ < z)
            player->NearTeleportTo(landX, landY, landZ, player->GetOrientation());
    });

    return recovered;
}

bool NewRpgDiscoverFlightMasterAction::Execute(Event /*event*/)
{
    bool learned = false;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest npcs"))
    {
        Creature* flightMaster = botAI->GetCreature(guid);
        if (!flightMaster || !flightMaster->IsAlive() || !flightMaster->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER) ||
            bot->GetDistance(flightMaster) > DISCOVER_FLIGHT_MASTER_DISTANCE || flightMaster->IsHostileTo(bot))
            continue;

        uint32 const node = sObjectMgr->GetNearestTaxiNode(flightMaster->GetPositionX(), flightMaster->GetPositionY(),
                                                           flightMaster->GetPositionZ(), flightMaster->GetMapId(),
                                                           bot->GetTeamId());
        if (!node)
            continue;

        auto discover = [&learned, node, flightMaster](Player* player)
        {
            if (player->m_taxi.IsTaximaskNodeKnown(node) ||
                player->GetDistance(flightMaster) > DISCOVER_FLIGHT_MASTER_DISTANCE)
                return;

            player->GetSession()->SendLearnNewTaxiNode(flightMaster);
            learned = true;
        };

        discover(bot);
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != bot && member->IsInWorld() && member->IsAlive() &&
                    member->GetMapId() == bot->GetMapId() && GET_PLAYERBOT_AI(member) &&
                    !flightMaster->IsHostileTo(member))
                    discover(member);
            }
        }

        if (learned)
            LOG_DEBUG("playerbots", "[New RPG] {} discovered flight path {}", bot->GetName(), node);
    }

    return learned;
}
