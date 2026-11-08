/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAction.h"
#include "BotPath.h"  // [guild12]
#include "QuestStats.h"  // [guild90]
#include "SmartScriptMgr.h"  // [guild15] talk-to objectives
#include <set>
#include <mutex>
#include <unordered_map>

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

bool NewRpgBaseAction::StartVendorRun()
{
    NewRpgInfo& info = botAI->rpgInfo;
    // [guild15] a vendor run another state took over (a death's recovery, a quest, a guild order) is over: it kept
    // vendorRunEntry set and no run started again (b308: Farret L10 with 6 of 7 items broken logged "vendor: repair"
    // 750 times without walking there; 12 of 50 bots ended the run with broken gear)
    if (info.vendorRunEntry && info.GetStatus() != RPG_GO_CAMP && info.GetStatus() != RPG_WANDER_NPC)
    {
        LOG_INFO("playerbots", "[New RPG] {} vendor run ({}) dropped by another state: planned again after its next "
                 "loot or money gain", bot->GetName(), info.vendorRunReason);
        info.vendorRunEntry = 0;
        // counted as a visit: tried again once the bags or the purse grow (b311: Hobliir's unreachable Exodar vendor
        // 29 yd off was walked to, given up and chosen again every few seconds)
        info.vendorVisited = true;
        info.vendorVisitBagUsed = AI_VALUE(uint8, "bag space");
        info.vendorVisitMoney = bot->GetMoney();
    }
    if (info.vendorRunEntry || bot->IsInCombat() || !bot->IsAlive())
        return false;

    uint8 const bagUsed = AI_VALUE(uint8, "bag space");
    bool const sell = AI_VALUE(bool, "should sell") && AI_VALUE(bool, "can sell") &&
                      (!info.vendorVisited || bagUsed > info.vendorVisitBagUsed);

    uint32 ammoSubClass = 0;
    if (bot->getClass() == CLASS_HUNTER)
    {
        uint32 const subClass = RequiredAmmoSubClass(bot);
        if (subClass && AI_VALUE2(uint32, "item count", "ammo") < VENDOR_LOW_AMMO && bot->GetMoney() > 0 &&
            (!info.vendorVisited || bot->GetMoney() > info.vendorVisitMoney))
            ammoSubClass = subClass;
    }
    // [guild13] a broken item (0 durability: a hunter's bow shoots no more) is repaired, as a player does - at a vendor
    // that repairs, paid from its own money; one try per money gain
    // [guild15] ... and before the next death breaks it: an item with no more durability left than one death takes
    // (the core's RATE_DURABILITY_LOSS_ON_DEATH, plus RATE_DURABILITY_LOSS_ON_SPIRIT_RESURRECT for a bot that rises
    // at the spirit healer) - b308: hunters fought on with broken bows at 4-5 dps and died to mobs below their level
    float const deathLoss = (sWorld->getRate(RATE_DURABILITY_LOSS_ON_DEATH) +
                             (int32(bot->GetLevel()) < sWorld->getIntConfig(CONFIG_DEATH_SICKNESS_LEVEL)
                                  ? sWorld->getRate(RATE_DURABILITY_LOSS_ON_SPIRIT_RESURRECT)
                                  : 0.0f)) /
                            100.0f;
    bool repair = false;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END && !repair; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            uint32 const maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            repair = item->IsBroken() ||
                     (maxDurability &&
                      float(item->GetUInt32Value(ITEM_FIELD_DURABILITY)) <= deathLoss * float(maxDurability));
        }
    repair = repair && bot->GetMoney() > 0 && (!info.vendorVisited || bot->GetMoney() > info.vendorVisitMoney);
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
        QuestStats::OnRpgTick(bot, quest ? quest->questId : 0);
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

    // [guild3] full bags or low ammo: to a vendor first, as a player does (not out of a flight, rest or pvp)
    if ((status == RPG_IDLE || status == RPG_GO_GRIND || status == RPG_WANDER_RANDOM || status == RPG_DO_QUEST ||
         status == RPG_DO_GATHER || ((status == RPG_GO_CAMP || status == RPG_WANDER_NPC) && !info.vendorRunEntry)) &&
        StartVendorRun())
        return true;

    switch (status)
    {
        case RPG_IDLE:
        {
            // A level-up makes quests that were given up on worth another try.
            if (info.lastQuestLevel != bot->GetLevel())
            {
                botAI->lowPriorityQuest.clear();
                info.lastQuestLevel = bot->GetLevel();
                LogDungeonCandidates();  // [guild11] hook for the dungeon step, once per level
            }

            // [guild30] an order of the guild coordinator first; QUEST (the default) = the choice below
            {
                GuildRunStep const step = sGuildCoordinator.NextStep(bot, botAI);
                if (step.kind == GuildRunStep::BUSY)
                    return true;
                if (step.kind == GuildRunStep::WALK)
                {
                    LOG_INFO("playerbots", "[rpg] {} L{} GUILD reason: {}", bot->GetName(), bot->GetLevel(),
                             step.reason);
                    info.ChangeToGoCamp(step.pos);
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
                    sGuildCoordinator.SetOrderStatus(bot, "нет узлов для навыка в этой зоне - квестует");
                }
            }
            // [guild10] a purposeful target only, never an aimless walk: a quest (hand in, objective, then a
            // starter of its level - ChooseBestQuest keeps that order), else mobs of its level, else stay put.
            {
                WorldPosition giverPos;
                uint32 const questId = ChooseBestQuest(giverPos);
                Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
                // [guild20] after the hand-ins: the planned profession's trainer or tool vendor
                if ((!quest || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE) &&
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
                info.ChangeToWanderRandom();
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
            // WANDER_RANDOM -> IDLE
            if (info.HasStatusPersisted(statusWanderRandomDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (info.HasStatusPersisted(statusWanderNpcDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // Progress on the quest keeps it going past the status duration.
            auto& data = std::get<NewRpgInfo::DoQuest>(info.data);
            uint32 const progress = GetQuestProgress(data.questId);
            if (progress > data.progress)
            {
                data.progress = progress;
                info.startT = getMSTime();
            }

            // DO_QUEST -> IDLE
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
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
            // REST -> IDLE
            if (info.HasStatusPersisted(statusRestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_GATHER:
        {
            // DO_GATHER -> IDLE
            if (info.HasStatusPersisted(statusDoGatherDuration))
            {
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

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        // Small nudge so the next tick's MoveFarTo starts from a
        // slightly different position. Kept small so it doesn't look
        // like the bot is abandoning its destination.
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    // [guild10] no random steps: the grind strategy takes the bot to the next mob of its level; none left here
    // -> IDLE picks the next purposeful target
    if (AI_VALUE(Unit*, "grind target"))
        return false;

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
                    botAI->DoSpecificAction("repair", Event("repair"), true);
                    // [guild15] its damage and the fights it refused were measured with the worn gear
                    SoloSafety::OnRepaired(bot);
                    NewRpgBaseAction::ForgetRefusals(bot);
                }
            }
        }

        // [guild10] the job at this npc is done: on to the next purposeful target, no next random npc
        info.ChangeToIdle();
        return true;
    }
    else
    {
        if (MoveWorldObjectTo(data.npcOrGo))
            return true;
        // [guild15] b319 GO_CAMP "sell junk" STUCK (Kylantino, Gnetlibozz, Rumanatol): no walk reaches the npc from
        // here - the visit ends (a vendor run counts as visited, as when the vendor is gone)
        if (object && NpcOutOfWalk(object))
        {
            if (info.vendorRunEntry)
            {
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

        // reached the giver but it did not offer the quest (despawned, phased, wrong spawn)
        botAI->lowPriorityQuest.insert(questId);
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
            }
            // can't find a poi pos to go, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // [guild12] the nearest objective spot the bot can walk to (the leader's route search finds a walk)
        std::sort(poiInfo.begin(), poiInfo.end(), [this](POIInfo const& a, POIInfo const& b)
                  { return bot->GetDistance2d(a.pos.x, a.pos.y) < bot->GetDistance2d(b.pos.x, b.pos.y); });
        float dx = 0.0f, dy = 0.0f, dz = INVALID_HEIGHT;
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
            if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
                dz = bot->GetPositionZ();
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
        // [guild90] the sweep: next unspent camp nearest the bot, claimed (other bots skip it)
        ClaimSpot(bot, questId, pos);
        QuestStats::LogEvent(bot, "sweep", questId,
                             "obj=" + std::to_string(objectiveIdx) + " to=(" + std::to_string(int32(dx)) + "," +
                                 std::to_string(int32(dy)) + ") " + std::to_string(uint32(bot->GetDistance2d(dx, dy))) +
                                 "yd " + LastResolveStats());
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        // [guild12] a needed quest object passed on the way is looted on the way
        if (UseQuestGameObject(questId))
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
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        bool hasProgression = false;
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has progression
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] != 0 && quest->RequiredNpcOrGoCount[currentObjective])
                hasProgression = true;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] != 0 &&
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                hasProgression = true;
        }
        if (!hasProgression)
        {
            // we has reach the poi for more than 5 mins but no progession
            // may not be able to complete this quest, marked as abandoned
            /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
            botAI->lowPriorityQuest.insert(questId);
            NotePostpone(bot, questId, "no progress at its spot");  // [guild90]
            botAI->RecordQuestAbandoned(questId, QuestAbandonReason::NoProgress);
            LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // clear and select another poi later
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        return true;
    }

    // objects to use for the quest (#2725)
    if (UseQuestGameObject(questId))
        return true;

    if (TalkToQuestNpc(questId))  // [guild15]
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
        // if quest is completed, back to poi with -1 idx to reward
        BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
        botAI->RecordQuestCompleted(questId);
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            // can't find a poi pos to reward, stop doing quest for now
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
        if (MoveFarTo(data.pos))
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
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        // e.g. Can not reward quest to gameobjects
        /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
        botAI->lowPriorityQuest.insert(questId);
        botAI->RecordQuestAbandoned(questId, QuestAbandonReason::CannotTurnIn);
        LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    return false;
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
        if (!info.taxiStillSince || !player->IsInFlight() ||
            GetMSTimeDiffToNow(info.taxiStillSince) < TaxiStallWatch::STALL_MS)
            return;

        recovered = true;
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
