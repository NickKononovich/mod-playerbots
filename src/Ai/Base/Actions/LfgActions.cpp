/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LfgActions.h"
#include "LeaderDungeonRun.h"  // [lfg26]
#include "AiFactory.h"
#include "Group.h"  // [diag250]
#include "ItemVisitors.h"
#include "LFGMgr.h"
#include "Opcodes.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Transport.h"
#include "World.h"
#include "WorldPacket.h"

using namespace lfg;

namespace
{
    constexpr uint8 CLASSIC_MAX_LEVEL = 60;
    constexpr uint8 TBC_MAX_LEVEL = 70;

    bool IsExpansionOutleveled(LFGDungeonEntry const* dungeon, uint8 level)
    {
        // Seasonal bosses keep their original expansion but are level 78-82.
        if (dungeon->Flags & LFG_FLAG_SEASONAL)
            return false;

        switch (dungeon->ExpansionLevel)
        {
            case EXPANSION_CLASSIC:
                return level > CLASSIC_MAX_LEVEL;
            case EXPANSION_THE_BURNING_CRUSADE:
                return level > TBC_MAX_LEVEL;
            default:
                return false;
        }
    }
}

bool LfgJoinAction::Execute(Event /*event*/) { return JoinLFG(); }

uint32 LfgJoinAction::GetRoles()
{
    if (!RandomPlayerbotMgr::instance().IsRandomBot(bot))
    {
        if (PlayerbotAI::IsTank(bot))
            return PLAYER_ROLE_TANK;
        if (PlayerbotAI::IsHeal(bot))
            return PLAYER_ROLE_HEALER;
        else
            return PLAYER_ROLE_DAMAGE;
    }

    uint8 spec = AiFactory::GetPlayerSpecTab(bot);
    switch (bot->getClass())
    {
        case CLASS_DRUID:
            if (spec == 2)
                return PLAYER_ROLE_HEALER;
            else if (spec == 1 && (bot->HasAura(16929) || bot->HasAura(16930) || bot->HasAura(16931)))  // Thick Hide, any rank
                return PLAYER_ROLE_TANK;
            else
                return PLAYER_ROLE_DAMAGE;
            break;
        case CLASS_PALADIN:
            if (spec == 1)
                return PLAYER_ROLE_TANK;
            else if (!spec)
                return PLAYER_ROLE_HEALER;
            else
                return PLAYER_ROLE_DAMAGE;
            break;
        case CLASS_PRIEST:
            if (spec != 2)
                return PLAYER_ROLE_HEALER;
            else
                return PLAYER_ROLE_DAMAGE;
            break;
        case CLASS_SHAMAN:
            if (spec == 2)
                return PLAYER_ROLE_HEALER;
            else
                return PLAYER_ROLE_DAMAGE;
            break;
        case CLASS_WARRIOR:
            if (spec == 2)
                return PLAYER_ROLE_TANK;
            else
                return PLAYER_ROLE_DAMAGE;
            break;
        case CLASS_DEATH_KNIGHT:
            if (spec == 0)
                return PLAYER_ROLE_TANK;
            else
                return PLAYER_ROLE_DAMAGE;
            break;

        default:
            return PLAYER_ROLE_DAMAGE;
            break;
    }

    return PLAYER_ROLE_DAMAGE;
}

bool LfgJoinAction::JoinLFG()
{
    // check if already in lfg
    LfgState state = sLFGMgr->GetState(bot->GetGUID());
    if (state != LFG_STATE_NONE)
        return false;

    /*ItemCountByQuality visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_EQUIP);
    bool random = urand(0, 100) < 20;
    bool heroic = urand(0, 100) < 50 &&
                  (visitor.count[ITEM_QUALITY_EPIC] >= 3 || visitor.count[ITEM_QUALITY_RARE] >= 10) &&
                  bot->GetLevel() >= 70;
    bool rbotAId = !heroic && (urand(0, 100) < 50 && visitor.count[ITEM_QUALITY_EPIC] >= 5 &&
                               (bot->GetLevel() == 60 || bot->GetLevel() == 70 || bot->GetLevel() == 80));*/

    LfgDungeonSet list;
    std::vector<uint32> selected;

    std::vector<uint32> dungeons = RandomPlayerbotMgr::instance().LfgDungeons[bot->GetTeamId()];
    if (!dungeons.size())
        return false;

    for (std::vector<uint32>::iterator i = dungeons.begin(); i != dungeons.end(); ++i)
    {
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(*i);
        if (!dungeon || (dungeon->TypeID != LFG_TYPE_RANDOM && dungeon->TypeID != LFG_TYPE_DUNGEON &&
                         dungeon->TypeID != LFG_TYPE_HEROIC && dungeon->TypeID != LFG_TYPE_RAID))
            continue;

        auto const& botLevel = bot->GetLevel();

        /*LFG_TYPE_RANDOM on classic is 15-58 so bot over level 25 will never queue*/
        if ((dungeon->MinLevel && (botLevel < dungeon->MinLevel || botLevel > dungeon->MaxLevel)) ||
            (botLevel > dungeon->MinLevel + 10 && dungeon->TypeID == LFG_TYPE_DUNGEON))
            continue;

        // LFG level ranges run past the expansion cap: Stratholme is 55-65.
        if (!sPlayerbotAIConfig.RandomBotJoinLfgOutleveledExpansion && IsExpansionOutleveled(dungeon, botLevel))
            continue;

        selected.push_back(dungeon->ID);
        list.insert(dungeon->ID);
    }

    if (!selected.size())
        return false;

    if (list.empty())
        return false;

    bool many = list.size() > 1;
    LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(*list.begin());

    // check role for console msg
    std::string _roles = "multiple roles";
    uint32 roleMask = GetRoles();
    if (roleMask & PLAYER_ROLE_TANK)
        _roles = "TANK";

    if (roleMask & PLAYER_ROLE_HEALER)
        _roles = "HEAL";

    if (roleMask & PLAYER_ROLE_DAMAGE)
        _roles = "DPS";

    LOG_INFO("playerbots", "Bot {} {}:{} <{}>: queues LFG, Dungeon as {} ({})", bot->GetGUID().ToString().c_str(),
             bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str(), _roles,
             many ? "several dungeons" : dungeon->Name[0]);

    // Set RbotAId Browser comment
    std::string const _gs = std::to_string(botAI->GetEquipGearScore(bot/*, false, false*/));

    // JoinLfg is not threadsafe, so make packet and queue into session
    // sLFGMgr->JoinLfg(bot, roleMask, list, _gs);

    WorldPacket* data = new WorldPacket(CMSG_LFG_JOIN);
    *data << (uint32)roleMask;
    *data << (bool)false;
    *data << (bool)false;
    // Slots
    *data << (uint8)(list.size());
    for (uint32 dungeon : list)
        *data << (uint32)dungeon;
    // Needs
    *data << (uint8)3 << (uint8)0 << (uint8)0 << (uint8)0;
    *data << _gs;
    bot->GetSession()->QueuePacket(data);

    return true;
}

bool LfgRoleCheckAction::Execute(Event /*event*/)
{
    if (bot->GetGroup())
    {
        // [lfg26] a member of a guild five's queued run answers with the role the five planned for it (the run set it
        // with UpdateRoleCheck); its own role guess sent a different set and the check failed (b375 five 1, Ragefire
        // Chasm: "the group was not moved (role check / proposal)" 1 s after the queue)
        if (uint8 const planned = Leader::Dungeon::DungeonRuns::Instance().PlannedLfgRoles(bot->GetGUID()))
        {
            LOG_INFO("playerbots", "[lfg26] {} answers the role check with its five's planned roles {}",
                     bot->GetName(), uint32(planned));
            WorldPacket* packet = new WorldPacket(CMSG_LFG_SET_ROLES);
            *packet << planned;
            bot->GetSession()->QueuePacket(packet);
            return true;
        }
        uint32 newRoles = GetRoles();
        // if (currentRoles == newRoles)
        //     return false;

        WorldPacket* packet = new WorldPacket(CMSG_LFG_SET_ROLES);
        *packet << (uint8)newRoles;
        bot->GetSession()->QueuePacket(packet);
        // sLFGMgr->SetRoles(bot->GetGUID(), newRoles);
        // sLFGMgr->UpdateRoleCheck(group->GetGUID(), bot->GetGUID(), newRoles);

        LOG_INFO("playerbots", "Bot {} {}:{} <{}>: LFG roles checked", bot->GetGUID().ToString().c_str(),
                 bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());

        return true;
    }

    return false;
}

bool LfgAcceptAction::Execute(Event event)
{
    // [diag250] the Dungeon Finder proposal is answered "yes" at once, in a fight, dead or on a boat too: the leader
    // queued the five and its decision is final. A member that cannot be ported when the last "yes" comes (the core's
    // TeleportPlayer: in combat, dead, falling, on a transport) stays outside the five's dungeon: it asks for the port
    // itself (CMSG_LFG_TELEPORT, the client's "teleport into the dungeon") once it can. b428 fives 11 / 17 / 22 / 40 /
    // 49 never started (members silent for up to 5.9 h): their members held the answer in their questing fights
    // ([lfg74]: Feette, a hunter, 25 s "in a fight" with 0 attackers), the last "yes" ported the members out of
    // combat (five 22: Geron and Thamesh stood in Ragefire Chasm) and left the others outside for good - nobody asked
    // for the port again, the run waits for all five on the map.
    uint32 id = AI_VALUE(uint32, "lfg proposal");
    if (!event.getPacket().empty())
    {
        WorldPacket p(event.getPacket());
        uint32 dungeonId = 0;
        uint8 state = 0;
        uint32 proposalId = 0;
        p >> dungeonId >> state >> proposalId;
        if (proposalId)
            id = proposalId;
    }
    if (!id)
        return false;

    auto const why = [this]() -> char const*
    {
        if (!bot->IsAlive())
            return "dead";
        if (bot->IsInCombat())
            return "in a fight";
        if (Transport* transport = bot->GetTransport(); transport && transport->IsMotionTransport())
            return "on a moving transport";
        if (bot->IsBeingTeleported())
            return "being teleported";
        if (bot->IsFalling() || bot->GetVehicle())
            return "falling / on a vehicle";
        return nullptr;
    };

    if (id != _answered)
    {
        char const* const blocked = why();
        _answered = id;
        _portAsked = false;
        LOG_INFO("playerbots", "[diag250] {} accepts the Dungeon Finder proposal {}{}", bot->GetName(), id,
                 blocked ? std::string(" (port later: ") + blocked + ")" : std::string());
        if (!blocked)
        {
            bot->ClearUnitState(UNIT_STATE_ALL_STATE);
            if (RandomPlayerbotMgr::instance().IsRandomBot(bot) && !bot->GetGroup())
            {
                RandomPlayerbotMgr::instance().Refresh(bot);
                botAI->ResetStrategies();
            }
            botAI->Reset();
        }
        WorldPacket* packet = new WorldPacket(CMSG_LFG_PROPOSAL_RESULT);
        *packet << id << true;
        bot->GetSession()->QueuePacket(packet);
        // kept: the follow-up below sees the proposal through (the late port, or its end)
        botAI->GetAiObjectContext()->GetValue<uint32>("lfg proposal")->Set(id);
        return true;
    }

    // the answer is given: the core's later updates of the same proposal are not answered again; the proposal stays
    // stored (a Reset() in between cleared it, the update packet brings it back) until the port or its end
    botAI->GetAiObjectContext()->GetValue<uint32>("lfg proposal")->Set(id);
    LfgState const state = sLFGMgr->GetState(bot->GetGUID());
    if (state == LFG_STATE_PROPOSAL || state == LFG_STATE_ROLECHECK)
        return false;

    Group* group = bot->GetGroup();
    if (state == LFG_STATE_DUNGEON && group && group->isLFGGroup())
    {
        uint32 const mapId = sLFGMgr->GetDungeonMapId(group->GetGUID());
        if (!mapId || bot->GetMapId() == mapId)
        {
            botAI->GetAiObjectContext()->GetValue<uint32>("lfg proposal")->Set(0);
            return false;
        }
        if (char const* const blocked = why())
        {
            if (_portWaitLogged != id)
            {
                _portWaitLogged = id;
                LOG_INFO("playerbots", "[diag250] {} waits to be ported into its Dungeon Finder dungeon (map {}): {}",
                         bot->GetName(), mapId, blocked);
            }
            return false;
        }
        if (!_portAsked)
            LOG_INFO("playerbots", "[diag250] {} was left outside by the Dungeon Finder port: asks for the port into "
                     "map {} itself", bot->GetName(), mapId);
        _portAsked = true;
        WorldPacket* packet = new WorldPacket(CMSG_LFG_TELEPORT);
        *packet << false;
        bot->GetSession()->QueuePacket(packet);
        return true;
    }

    // the proposal ended without a port for this bot (another member declined, the time ran out, the queue left)
    LOG_INFO("playerbots", "[diag250] {} accepted the Dungeon Finder proposal {}, but it ended without the port "
             "(lfg state {})", bot->GetName(), id, uint32(state));
    botAI->GetAiObjectContext()->GetValue<uint32>("lfg proposal")->Set(0);
    return false;
}

bool LfgLeaveAction::Execute(Event /*event*/)
{
    // Don't leave if lfg strategy enabled
    // if (botAI->HasStrategy("lfg", BOT_STATE_NON_COMBAT))
    //    return false;

    // Don't leave if already invited / in dungeon
    if (sLFGMgr->GetState(bot->GetGUID()) > LFG_STATE_QUEUED)
        return false;

    // Don't drop a queue we deliberately joined. The "seldom" tick (RandomTrigger, ~300s)
    // otherwise pulls random bots straight back out, so LFGQueue::CheckCompatibility never
    // sees a role-complete pool that holds still long enough to form a group. Turning
    // RandomBotJoinLfg off still lets whoever is mid-queue fall through and leave.
    // Config bool is tested first so the O(currentBots) IsRandomBot() scan is skipped
    // whenever the feature is disabled.
    if (sPlayerbotAIConfig.RandomBotJoinLfg && RandomPlayerbotMgr::instance().IsRandomBot(bot))
        return false;

    WorldPacket* packet = new WorldPacket(CMSG_LFG_LEAVE);
    bot->GetSession()->QueuePacket(packet);
    // sLFGMgr->LeaveLfg(bot->GetGUID());
    return true;
}

bool LfgLeaveAction::isUseful() { return true; }

bool LfgTeleportAction::Execute(Event event)
{
    bool out = false;

    WorldPacket p(event.getPacket());
    if (!p.empty())
    {
        p.rpos(0);
        p >> out;
    }

    bot->ClearUnitState(UNIT_STATE_ALL_STATE);

    WorldPacket* packet = new WorldPacket(CMSG_LFG_TELEPORT);
    *packet << out;
    bot->GetSession()->QueuePacket(packet);
    // sLFGMgr->TeleportPlayer(bot, out);

    return true;
}

bool LfgJoinAction::isUseful()
{
    if (!sPlayerbotAIConfig.RandomBotJoinLfg)
    {
        // botAI->ChangeStrategy("-lfg", BOT_STATE_NON_COMBAT);
        return false;
    }

    if (bot->GetLevel() < 15)
        return false;

    if (bot->GetGroup() && bot->GetGroup()->GetLeaderGUID() != bot->GetGUID())
    {
        // botAI->ChangeStrategy("-lfg", BOT_STATE_NON_COMBAT);
        return false;
    }

    if (bot->IsBeingTeleported())
        return false;

    if (bot->InBattleground())
        return false;

    if (bot->InBattlegroundQueue())
        return false;

    if (bot->isDead())
        return false;

    // A selfbot only gets the lfg strategy when its owner adds it, so that is its opt-in.
    if (!RandomPlayerbotMgr::instance().IsRandomBot(bot) && !IsSelfBot(bot))
        return false;

    Map* map = bot->GetMap();
    if (map && map->Instanceable())
        return false;

    LfgState state = sLFGMgr->GetState(bot->GetGUID());
    if (state != LFG_STATE_NONE)
        return false;

    return true;
}
