/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAction.h"
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
#include "WaypointMovementGenerator.h"
#include "World.h"  // [guild15] durability rates
#include "WorldPacket.h"
#include "G3D/Vector2.h"
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <mutex>

namespace
{
    // [guild3] Vendor spawns of the world, read once from the creature spawn table.
    struct VendorSpawn
    {
        uint32 entry;
        uint16 mapId;
        float x, y, z;
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
                spawns.push_back({data.id, data.mapid, data.posX, data.posY, data.posZ});
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

    // The nearest vendor spawn on the bot's map that is not hostile to it (and sells its ammo when it needs some).
    // [guild14] repair: only those that repair (UNIT_NPC_FLAG_REPAIR); a solo bot's trip there must be safe
    // (NewRpgBaseAction::TripRefused), else the next nearest
    VendorSpawn const* NearestVendorOf(Player* bot, uint32 ammoSubClass, bool repair, float& dist)
    {
        FactionTemplateEntry const* own = bot->GetFactionTemplateEntry();
        std::vector<std::pair<float, VendorSpawn const*>> candidates;
        for (VendorSpawn const& spawn : VendorSpawns())
        {
            if (spawn.mapId != bot->GetMapId())
                continue;
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(spawn.entry);
            if (!info || (repair && !(info->npcflag & UNIT_NPC_FLAG_REPAIR)))
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->faction);
            if (!faction || !own || faction->IsHostileTo(*own) || own->IsHostileTo(*faction))
                continue;
            if (ammoSubClass && !AmmoPrice(bot, spawn.entry, ammoSubClass))
                continue;
            candidates.emplace_back(bot->GetExactDist(spawn.x, spawn.y, spawn.z), &spawn);
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](auto const& a, auto const& b) { return a.first < b.first; });
        bool refusedOne = false;
        for (auto const& [d, spawn] : candidates)
        {
            // after a refused trip only the nearer vendors of the zone the bot stands in are looked at (each look
            // searches a route): the first one outside it ends the search
            if (refusedOne &&
                sMapMgr->GetZoneId(PHASEMASK_NORMAL, spawn->mapId, spawn->x, spawn->y, spawn->z) != bot->GetZoneId())
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

bool NewRpgBaseAction::HearthOut(std::string const& why)
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
        }
        info.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    info.hearthNote.clear();
    LOG_INFO("playerbots", "[rpg] {} L{} HEARTH reason: {}; to its bind point ({:.0f},{:.0f}) map {}, {:.0f} yd",
             bot->GetName(), bot->GetLevel(), why, bind.GetPositionX(), bind.GetPositionY(), bind.GetMapId(),
             bind.GetMapId() == bot->GetMapId() ? bot->GetExactDist(bind) : 0.0f);
    return botAI->DoSpecificAction("hearthstone", Event("new rpg hearth"), true);
}

bool NewRpgBaseAction::BindAtInnHere()
{
    if (bot->IsInCombat() || !SoloSafety::Applies(bot))
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
    return false;
}

bool NewRpgBaseAction::StartVendorRun()
{
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
    std::vector<uint32> objectiveItems;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (uint32 const questId = bot->GetQuestSlotQuestId(slot))
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
                      (!info.vendorVisited || bagUsed > info.vendorVisitBagUsed) &&
                      (bagUsed >= 100 || !NewRpgCoop::InParty(bot->GetGUID().GetCounter()));

    uint32 ammoSubClass = 0;
    if (bot->getClass() == CLASS_HUNTER)
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
    bool repair = repairGo && bot->GetMoney() >= call.cost &&
                  (!info.repairVisited || bot->GetMoney() > info.repairVisitMoney);
    if (!sell && !ammoSubClass && !repair)
        return false;

    float dist = 0.0f;
    VendorSpawn const* vendor = repair ? NearestRepairer(bot, dist) : NearestVendor(bot, ammoSubClass, dist);
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
        return false;

    std::string reason = sell ? "sell junk" : "";
    if (repair)  // [guild13]
        reason += std::string(reason.empty() ? "" : " + ") + "repair";
    if (ammoSubClass)
        reason += std::string(!reason.empty() ? " + " : "") +
                  (ammoSubClass == ITEM_SUBCLASS_ARROW ? "buy arrows" : "buy bullets");
    info.ChangeToGoCamp(WorldPosition(vendor->mapId, vendor->x, vendor->y, vendor->z));
    info.vendorRunEntry = vendor->entry;
    info.vendorRunReason = reason;
    CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(vendor->entry);
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
        info.ChangeToIdle();
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

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus status = info.GetStatus();
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
    if (info.loggedStatus != uint8(status))
    {
        static char const* const names[] = {"IDLE", "GO_GRIND", "GO_CAMP", "WANDER_RANDOM", "WANDER_NPC",
                                             "DO_QUEST", "TRAVEL_FLIGHT", "REST", "OUTDOOR_PVP", "DO_GATHER", "DO_PROFESSION"};
        info.loggedStatus = uint8(status);
        sGuildCoordinator.OnRpgStatus(bot, uint8(status));  // [guild30] LOOP alert
        if (status != RPG_DO_QUEST)
            ClaimSpot(bot, 0, WorldPosition());  // [guild90] off questing: its camp is free for other bots
        std::string detail;
        if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data))
            detail = " q" + std::to_string(quest->questId) + (quest->quest ? " " + quest->quest->GetTitle() : "");
        else if (auto const* grind = std::get_if<NewRpgInfo::GoGrind>(&info.data))
            detail = " " + std::to_string(uint32(bot->GetExactDist(grind->pos))) + " yd";
        else if (auto const* camp = std::get_if<NewRpgInfo::GoCamp>(&info.data))
            detail = " " + std::to_string(uint32(bot->GetExactDist(camp->pos))) + " yd";
        if (info.vendorRunEntry)
            detail += " vendor: " + info.vendorRunReason;
        LOG_INFO("playerbots", "[rpg] {} L{} {}{}", bot->GetName(), bot->GetLevel(),
                 status < RPG_STATUS_END ? names[status] : "?", detail);
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
    // [deaths13] hurt, out of a fight, in a wanderer's reach: the rest is taken out of every reach
    if (RestEscape())
        return true;

    // [regress10] the recovery after a death comes first: before, only a far walk's MoveFarTo took it, so a quest spot
    // in reach of the corpse was fought at once (b345 Guinudrond L11 at the Silver Stream Mine)
    if (!bot->IsInCombat() && SoloSafety::HoldsPulls(bot) && SoloSafetyStep())
        return true;

    // [coop] a co-op party member goes with its party's leader; its own plan waits (NewRpgCoop.h)
    {
        NewRpgCoop::Order const coop = NewRpgCoop::Tick(botAI, bot);
        if (coop.kind == NewRpgCoop::Order::Kind::Follow)
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

    // [deaths14] Resurrection Sickness (spell 15007, Player::SpiritHealer... ResurrectPlayer: all stats and the damage it
    // deals at a quarter for (level - CONFIG_DEATH_SICKNESS_LEVEL + 1) minutes, ten from the tenth level above it): the
    // judge's numbers hold only for the bot without it - a wanderer or a camp it walks past meets a quarter of the bot.
    // b354: Laaral L14 (3 deaths, 83-151 s after its rise) and Rumanatol L13 (87 s) rose at the Blood Watch spirit
    // healer, walked out at 139 max health and 2.7 dps and died to single Elder Brown Bears L15-16. The bot stays where
    // it rose (the spirit healer's graveyard) until the aura is gone - a game mechanic's end, not a clock of its own
    if (!bot->IsInCombat() && !NewRpgCoop::IsMember(bot) && !bot->IsInFlight() && !bot->GetTransport())
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
            if (status != RPG_IDLE)
                info.ChangeToIdle();
            if (bot->isMoving())
                bot->StopMoving();
            return true;
        }
        std::lock_guard<std::mutex> guard(sickLock);
        sickLogged.erase(low);
    }

    switch (status)
    {
        case RPG_IDLE:
        {
            BindAtInnHere();  // [regress10] its home is the town it quests from
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
                uint32 const questId = ChooseBestQuest(giverPos);
                Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
                // [guild20] after the hand-ins: the planned profession's trainer or tool vendor
                // [tasks] single decider: a guild member works its profession only on the global leader's PROFESSION
                // call, never on its own choice (a bot outside the guild plan leads itself)
                if ((!quest || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE) &&
                    (!sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter()) ||
                     sGuildCoordinator.ProfessionCall(bot) != GuildProfessionKind::NONE) &&
                    NewRpgProfession::StartTask(botAI, bot))
                    return true;
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
                uint32 const questId = ChooseNextLevelQuest(giverPos, grindPos);
                Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
                // [stuck8] no grind spot of its level on this map and the giver under its feet: a GO_GRIND there
                // reaches it at once and WANDER_RANDOM finds no mob (b343 Orton on the Deeprun Tram, 160 alerts of
                // IDLE / GO_GRIND / WANDER_RANDOM); the reason is logged, it rests until the choice runs again
                if (quest && grindPos == WorldPosition() && giverPos.GetMapId() == bot->GetMapId() &&
                    bot->GetExactDist(giverPos) < 10.0f)
                {
                    LOG_INFO("playerbots", "[rpg] {} L{} STAY reason: q{} needs L{}, no grind spot of its level on "
                             "map {} and no quest it may take now", bot->GetName(), bot->GetLevel(), questId,
                             quest->GetMinLevel(), bot->GetMapId());
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
            LOG_INFO("playerbots", "[rpg] {} L{} STAY reason: no quest of any level on this map", bot->GetName(),
                     bot->GetLevel());
            info.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_GRIND -> WANDER_RANDOM: at the grind spot the bot fights the mobs there, no random walk
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                LOG_INFO("playerbots", "[rpg] {} L{} WANDER reason: grind spot reached", bot->GetName(),
                         bot->GetLevel());
                bool const forSkin = data.forSkin;
                bool const forCamp = data.forCamp;
                info.ChangeToWanderRandom();
                std::get<NewRpgInfo::WanderRandom>(info.data).forSkin = forSkin;  // [skin12]
                std::get<NewRpgInfo::WanderRandom>(info.data).forCamp = forCamp;  // [help13]
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC only to visit the vendor this walk came for; else the next purposeful target
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                // [guild30] a guild bank walk: deposit / withdraw at the vault
                if (uint8 const bank = sGuildCoordinator.OnCampReached(bot))
                {
                    if (bank == 1)
                        info.ChangeToIdle();
                    return true;
                }
                if (!info.vendorRunEntry)
                {
                    info.ChangeToIdle();
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
            auto& data = std::get<NewRpgInfo::WanderRandom>(info.data);
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
                    info.ChangeToIdle();
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
                info.ChangeToIdle();
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
            auto& data = std::get<NewRpgInfo::DoQuest>(info.data);

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
            auto& data = std::get<NewRpgInfo::TravelFlight>(info.data);
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival
                info.ChangeToIdle();
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
                info.ChangeToIdle();
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
            auto& data = std::get<NewRpgInfo::DoGather>(info.data);
            if (!data.level)
                data.level = bot->GetLevel();
            else if (data.level != bot->GetLevel() && !sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter()))
            {
                LOG_INFO("playerbots", "[rpg] {} L{} IDLE reason: level-up while gathering, the choice runs again",
                         bot->GetName(), bot->GetLevel());
                info.ChangeToIdle();
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

Creature* NewRpgWanderRandomAction::NextSkinMob()
{
    if (!bot->HasSkill(SKILL_SKINNING))
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
        if (!tmpl || !tmpl->SkinLootId || tmpl->rank > CREATURE_ELITE_NORMAL ||
            Acore::XP::GetColorCode(bot->GetLevel(), mob->GetLevel()) <= XP_ORANGE)
            continue;
        float const d = bot->GetExactDist(mob);
        if (d >= bestDist || InDangerZone(bot, mob->GetPositionX(), mob->GetPositionY()) ||
            WalkGivenUp(bot, mob->GetPositionX(), mob->GetPositionY()))
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
    if (auto const* wander = std::get_if<NewRpgInfo::WanderRandom>(&botAI->rpgInfo.data); wander && wander->forSkin)
        if (Creature* next = NextSkinMob())
        {
            LOG_INFO("playerbots", "[skin12] {} L{} skin grind: no mob in reach here, the nearest live {} L{} {:.0f} "
                     "yd off at ({:.0f},{:.0f})",
                     bot->GetName(), bot->GetLevel(), next->GetName(), next->GetLevel(), bot->GetExactDist(next),
                     next->GetPositionX(), next->GetPositionY());
            botAI->rpgInfo.ChangeToGoGrind(WorldPosition(next));
            std::get<NewRpgInfo::GoGrind>(botAI->rpgInfo.data).forSkin = true;
            return true;
        }
    LOG_INFO("playerbots", "[rpg] {} L{} grind spot cleared: no mob of its level in reach", bot->GetName(),
             bot->GetLevel());
    botAI->rpgInfo.ChangeToIdle();
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
            return true;
        }
        LOG_INFO("playerbots", "[New RPG] {} finds no vendor {} at its spawn", bot->GetName(), info.vendorRunEntry);
        info.vendorRunEntry = 0;
        info.vendorVisited = true;
        info.vendorVisitBagUsed = AI_VALUE(uint8, "bag space");
        info.vendorVisitMoney = bot->GetMoney();
    }
    if (!data.npcOrGo)
    {
        // [guild10] only a giver with something to accept or reward, never a random npc; none -> IDLE
        ObjectGuid npcOrGo = NewRpgHelpers::FindQuestGiver(botAI);
        if (npcOrGo.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
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
                        info.vendorRunEntry = 0;
                        info.vendorVisited = true;
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
        info.ChangeToIdle();
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
        if (object && !bot->isMoving() && object->GetMapId() == bot->GetMapId())
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
        if (!atWalkEnd && MoveWorldObjectTo(data.npcOrGo))
            return true;
        // [guild15] b319 GO_CAMP "sell junk" STUCK (Kylantino, Gnetlibozz, Rumanatol): no walk reaches the npc from
        // here - the visit ends (a vendor run counts as visited, as when the vendor is gone)
        if (object && (atWalkEnd || NpcOutOfWalk(object)))
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
                info.vendorVisitBagUsed = AI_VALUE(uint8, "bag space");
                info.vendorVisitMoney = bot->GetMoney();
            }
            info.ChangeToIdle();
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
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    // [questroute] the pickup is done (the giver search took its quests): the route, built again with them, says
    // which stop comes next - not this quest's objective by default
    if (data.giverPos != WorldPosition() && questStatus != QUEST_STATUS_NONE)
    {
        botAI->rpgInfo.ChangeToIdle();
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
    info.ChangeToIdle();
    return true;
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
        botAI->rpgInfo.ChangeToIdle();
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
            botAI->rpgInfo.ChangeToIdle();
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
                botAI->rpgInfo.ChangeToIdle();
                return true;
            }
            if (uint32 const wait = EarliestRespawn(bot, questId))
            {
                std::string const resolved = LastResolveStats();  // before OtherQuestSeconds resolves the others
                uint32 other = 0;
                float const otherSeconds = OtherQuestSeconds(questId, other);
                if (!other || float(wait) <= otherSeconds)
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
                    // the respawn is the world's clock: re-checked in short steps, the grind and loot strategies
                    // take what appears (and wanderers) meanwhile
                    return ForceToWait(std::min<uint32>(wait, 10) * IN_MILLISECONDS);
                }
                QuestStats::LogEvent(bot, "target-switch", other,
                                     "from=q" + std::to_string(questId) + " wait_s=" + std::to_string(wait) +
                                         " other_s=" + std::to_string(uint32(otherSeconds)) +
                                         " reason=all camps spent, the other quest is cheaper than the respawn");
                NoteResume(bot, questId);  // [flit] back to it at its respawn or once the other is done
            }
            // can't find a poi pos to go, stop doing quest for now
            // [questroute] the route leaves it out until its camps' respawn (or the next change of the log)
            InvalidateRoute(bot, questId, "no unspent objective spot");
            botAI->rpgInfo.ChangeToIdle();
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
            else if (!SafeFloorZ(bot, dx, dy, dz) && IsGridFallbackZ(bot, dx, dy, dz))
            {
                LOG_INFO("playerbots", "[rpg] {} L{} SKIP spot of q{} at ({:.0f},{:.0f},{:.0f}): no floor there (the "
                         "map grid's fallback height)",
                         bot->GetName(), bot->GetLevel(), questId, dx, dy, dz);
                continue;
            }
            if (FindBotPath(bot, dx, dy, dz).type & PATHFIND_NOPATH)
                continue;
            objectiveIdx = poi.objectiveIdx;
            reachable = true;
            break;
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
            botAI->rpgInfo.ChangeToIdle();
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
        return true;
    }
    // [timers] no stay time at the spot: it is left when its objective is done (the completed check above), when
    // nothing at it is left to do (the spent-spot check below: no mob it pulls, no quest object, no fight, no rest)
    // or when the quest is postponed (no walk, unsupported, all camps spent and another quest cheaper than the respawn)

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
    if (!bot->IsInCombat() && !AI_VALUE(Unit*, "grind target") && !FindNeededQuestObject(bot) &&
        !SoloSafety::WantsRest(bot, false) && !SoloSafety::WantsRest(bot, true) && !bot->IsSitState())
    {
        NoteSpentSpot(bot, questId, WorldPosition(bot));  // [guild90] the circle it scanned from where it stands
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        return true;
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
            botAI->rpgInfo.ChangeToIdle();
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
            botAI->rpgInfo.ChangeToIdle();
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

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        bool const walks = MoveFarTo(data.pos);
        // [quest19] the hand-in walk: a line when the bot is farther from the ender than the nearest it came by a
        // sight, or the walk returns none - the target, the walk's state, where the last move went. b363 Manue L12
        // q1715: 804 s in DO_QUEST for Gakin in Stormwind's Mage Quarter, its kills drifting through Elwynn, no line
        {
            static std::mutex handLock;
            static std::unordered_map<uint32, std::tuple<uint32, float, float>> handWalk;  // quest, nearest, logged
            float const d = bot->GetDistance(data.pos);
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
                         "[quest19] {} L{} q{} hand-in walk: ender at map {} ({:.0f},{:.0f},{:.0f}) {:.0f} yd, nearest "
                         "{:.0f} yd; walk {}; far route {} points; last move to ({:.0f},{:.0f}); status {}",
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
                botAI->rpgInfo.ChangeToIdle();
                return true;
            }
            LOG_INFO("playerbots", "[rpg] {} L{} QUEST_BUY q{}: {} from {} ({}c)", bot->GetName(), bot->GetLevel(),
                     questId, proto ? proto->Name1 : std::to_string(itemId), npc->GetName(), cost);
            NewRpgProfession::StartBuy(botAI, bot, itemId, npc->GetEntry(), WorldPosition(npc), cost);
            return true;
        }
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
        botAI->rpgInfo.ChangeToIdle();
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
            // a free slot first: what it may sell goes to a vendor now (its visit before was at the same fill)
            NewRpgInfo& info = botAI->rpgInfo;
            info.vendorVisited = false;
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
        info.ChangeToIdle();
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

    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes.empty() ? 0 : nodes.front(), nodes.empty() ? 0 : nodes.back());
        info.ChangeToIdle();
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
    // [wait14] a member beyond the group reward distance that comes to the leader (its follow, no plan of its own):
    // the leader walks to meet it on its own routed walk (MoveFarTo: the route search round the mobs it does not
    // pull), the two close the gap from both ends. b354: Orton L11 stood 928 s in Elwynn for Febineysh L7 4551 yd
    // away in Loch Modan - its follow stalled at the graveyard ("stuck watchdog: follow again"), it stood among the
    // Elder Black Bears L11 and died 3 times; Sycina L11 stood 1074 s for Nelaeleas 3849 yd off
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
    if (Player* member = purpose ? behind : nullptr;
        member && member->IsAlive() && member->IsInWorld() && member->GetMapId() == bot->GetMapId() &&
        !bot->IsInCombat())
    {
        WorldPosition const meet(member);
        if (_meetLogged != member->GetGUID())
        {
            _meetLogged = member->GetGUID();
            LOG_INFO("playerbots", "[wait14] {} L{} walks to meet {} L{} {:.0f} yd behind (its follow does not bring it "
                     "within the group reward distance)",
                     bot->GetName(), bot->GetLevel(), member->GetName(), member->GetLevel(), bot->GetExactDist(member));
        }
        // [wait19] the walk is a meet walk, no quest spot's: its give-up postpones no quest and asks no help
        info.meetWalk = true;
        bool const walks = MoveFarTo(meet);
        info.meetWalk = false;
        if (walks)
            return true;
    }
    else if (!behind)
        _meetLogged.Clear();

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
        if (ground > INVALID_HEIGHT && ground < z)
            player->NearTeleportTo(x, y, ground, player->GetOrientation());
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
