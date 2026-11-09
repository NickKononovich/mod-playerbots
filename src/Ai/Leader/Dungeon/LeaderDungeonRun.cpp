/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderDungeonRun.h"
#include "AiFactory.h"  // [spec71]

#include "DBCStores.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Item.h"
#include "LFGMgr.h"
#include "LeaderKnowledge.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "SelfTravelActions.h"
#include "Timer.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace Leader::Dungeon
{
    namespace
    {
        // the run driver's look at its runs (a tick, not a rule: every decision below waits on a game fact)
        constexpr uint32 RUN_TICK_MS = 500;

        // what a five does while the five-leader runs the dungeon: no own quests, travel or grind (its own walk,
        // its corpse run and the portal walk stay its own actions)
        constexpr char const* RUN_STRATEGIES_OFF = "-rpg,-new rpg,-grind,-travel,-move random,-quest";

        uint8 LfgRoles(std::string const& role, bool leader)
        {
            uint8 roles = role == "tank"     ? uint8(lfg::PLAYER_ROLE_TANK)
                          : role == "healer" ? uint8(lfg::PLAYER_ROLE_HEALER)
                                             : uint8(lfg::PLAYER_ROLE_DAMAGE);
            if (leader)
                roles |= uint8(lfg::PLAYER_ROLE_LEADER);
            return roles;
        }

        std::string Stamp()
        {
            tm const local = Acore::Time::TimeBreakdown(time(nullptr));
            char buffer[32];
            strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
            return buffer;
        }

        std::string JsonText(std::string const& text)
        {
            std::string out;
            for (char c : text)
            {
                if (c == '"' || c == '\\')
                    out += '\\';
                if (c == '\n')
                {
                    out += "\\n";
                    continue;
                }
                out += c;
            }
            return out;
        }

        // the arrival point inside of the outside portal nearest the five's entrance
        bool ArrivalInside(FiveRunRequest const& request, Position& at)
        {
            float best = FLT_MAX;
            for (auto const& [triggerId, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
            {
                if (teleport.target_mapId != request.mapId)
                    continue;
                AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
                if (!trigger || trigger->map != request.entrance.GetMapId())
                    continue;
                float const distance = request.entrance.GetExactDist(trigger->x, trigger->y, trigger->z);
                if (distance < best)
                {
                    best = distance;
                    at.Relocate(teleport.target_X, teleport.target_Y, teleport.target_Z, teleport.target_Orientation);
                }
            }
            return best < FLT_MAX;
        }
    }

    std::vector<MissingItem> MissingCarriedItems(uint32 mapId, uint8 difficulty, std::vector<Player*> const& members)
    {
        std::vector<MissingItem> missing;
        for (Knowledge::EventItemDef const* def : Knowledge::EventItems(mapId))
        {
            // a drop of this dungeon is looted inside (the five loots its kills): only what a group carries in
            if (!def->dropFrom.empty() || (def->difficulty >= 0 && uint8(def->difficulty) != difficulty))
                continue;
            bool held = false;
            for (Player* member : members)
                if (member && member->HasItemCount(def->itemId, 1, false))
                    held = true;
            if (held)
                continue;
            MissingItem item;
            item.itemId = def->itemId;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(def->itemId);
            item.name = proto ? proto->Name1 : std::to_string(def->itemId);
            item.source = def->source ? def->source : "";
            missing.push_back(item);
        }
        return missing;
    }

    DungeonRuns& DungeonRuns::Instance()
    {
        static DungeonRuns instance;
        return instance;
    }

    char const* DungeonRuns::StageName(Stage stage)
    {
        switch (stage)
        {
            case Stage::Group:
                return "group";
            case Stage::Queue:
                return "dungeon finder";
            case Stage::WalkIn:
                return "enters through the portal";
            case Stage::Inside:
                return "in the dungeon";
            default:
                return "report";
        }
    }

    // [lfg26] the planned roles of the queued members, read by their map threads (the role check packet): a leaf
    // lock of its own, written by the world thread's run updates
    namespace
    {
        std::mutex plannedRolesLock;
        std::unordered_map<ObjectGuid, uint8> plannedRoles;
    }

    uint8 DungeonRuns::PlannedLfgRoles(ObjectGuid member) const
    {
        std::lock_guard<std::mutex> guard(plannedRolesLock);
        auto const found = plannedRoles.find(member);
        return found == plannedRoles.end() ? 0 : found->second;
    }

    bool DungeonRuns::CanQueue(FiveRunRequest const& request, std::string& why) const
    {
        if (!sLFGMgr->isOptionEnabled(lfg::LFG_OPTION_ENABLE_DUNGEON_FINDER))
        {
            why = "the dungeon finder is off on the server";
            return false;
        }
        LFGDungeonEntry const* dungeon = request.lfgId ? sLFGDungeonStore.LookupEntry(request.lfgId) : nullptr;
        if (!dungeon || dungeon->MapID != request.mapId)
        {
            why = "no dungeon finder entry for " + request.contentKey;
            return false;
        }
        if (request.members.size() != 5)
        {
            why = "the five is not full";
            return false;
        }
        if (lfgRefused_.count(request.fiveId))
        {
            why = "the dungeon finder refused this five already";
            return false;
        }
        for (FiveRunMember const& member : request.members)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(member.guid);
            if (!player || !player->IsInWorld())
            {
                why = member.guid.ToString() + " offline";
                return false;
            }
            if (player->InBattleground() || player->InBattlegroundQueue() || player->isDead())
            {
                why = player->GetName() + " in an arena / battleground or dead";
                return false;
            }
            if (player->GetLevel() < dungeon->MinLevel || player->GetLevel() > dungeon->MaxLevel)
            {
                why = player->GetName() + " L" + std::to_string(player->GetLevel()) + " outside the finder's level range " +
                      std::to_string(dungeon->MinLevel) + "-" + std::to_string(dungeon->MaxLevel);
                return false;
            }
            lfg::LfgLockMap const& locks = sLFGMgr->GetLockedDungeons(member.guid);
            if (locks.find(dungeon->Entry()) != locks.end())
            {
                why = player->GetName() + ": the dungeon is locked for it in the finder";
                return false;
            }
            if (sLFGMgr->GetState(member.guid) != lfg::LFG_STATE_NONE)
            {
                why = player->GetName() + " already in the dungeon finder";
                return false;
            }
        }
        return true;
    }

    std::string DungeonRuns::Start(FiveRunRequest const& request, std::string& error)
    {
        if (runs_.count(request.fiveId))
        {
            error = "five " + std::to_string(request.fiveId) + " runs its own dungeon already";
            return "";
        }
        if (!ObjectAccessor::FindConnectedPlayer(request.leader))
        {
            error = "the five's leader is offline";
            return "";
        }
        Run run;
        run.request = request;
        run.id = Stamp() + "-" + request.contentKey + "-five" + std::to_string(request.fiveId) + "-" +
                 std::to_string(++seq_);
        run.startMs = getMSTime();
        run.entry = request.viaLfg ? "lfg" : "walk";
        LOG_INFO("playerbots", "[guild70] five {} run {}: {} ({}), the five-leader {} leads, in by {}", request.fiveId,
                 run.id, request.name, request.contentKey, request.leader.ToString(), run.entry);
        runs_[request.fiveId] = run;
        return run.id;
    }

    void DungeonRuns::Stop(uint32 fiveId, std::string const& reason)
    {
        auto const itr = runs_.find(fiveId);
        if (itr == runs_.end())
            return;
        Run& run = itr->second;
        if (run.stage == Stage::Inside && engine_ && !engine_->Phase(fiveId).empty())
        {
            engine_->Stop(fiveId, reason);  // its end comes back through OnCleared
            return;
        }
        End(run, "stopped", reason);
    }

    std::string DungeonRuns::Phase(uint32 fiveId) const
    {
        auto const itr = runs_.find(fiveId);
        if (itr == runs_.end())
            return "";
        Run const& run = itr->second;
        std::string phase = StageName(run.stage);
        if (run.stage == Stage::Inside && engine_)
            if (std::string const inside = engine_->Phase(fiveId); !inside.empty())
                phase += ": " + inside;
        return phase;
    }

    void DungeonRuns::Update(uint32 diff)
    {
        timer_ += diff;
        if (timer_ < RUN_TICK_MS || runs_.empty())
            return;
        timer_ = 0;
        std::vector<uint32> ids;
        for (auto const& [id, run] : runs_)
            ids.push_back(id);
        for (uint32 id : ids)
        {
            auto const itr = runs_.find(id);
            if (itr == runs_.end())
                continue;
            Run& run = itr->second;
            // a member gone: the run cannot go on (the global leader decides again)
            if (run.stage != Stage::Inside)
            {
                std::string offline;
                for (FiveRunMember const& member : run.request.members)
                    if (!ObjectAccessor::FindConnectedPlayer(member.guid))
                        offline = member.guid.ToString();
                if (!offline.empty())
                {
                    End(run, "failed", offline + " offline before entering");
                    continue;
                }
            }
            switch (run.stage)
            {
                case Stage::Group:
                    if (TickGroup(run))
                    {
                        if (run.request.viaLfg)
                        {
                            run.stage = Stage::Queue;
                            TickQueue(run);
                        }
                        else
                            run.stage = Stage::WalkIn;
                    }
                    break;
                case Stage::Queue:
                    TickQueue(run);
                    break;
                case Stage::WalkIn:
                    TickWalkIn(run);
                    break;
                default:
                    break;
            }
        }
    }

    bool DungeonRuns::TickGroup(Run& run)
    {
        FiveRunRequest const& request = run.request;
        Player* leader = ObjectAccessor::FindConnectedPlayer(request.leader);
        if (!leader || !leader->IsInWorld() || leader->IsBeingTeleported())
            return false;

        // the five-leader's group: its own when it leads exactly this five already, else a new one (it invites, the
        // members accept: bots accept their leader's invite, the five was agreed with the human)
        Group* group = leader->GetGroup();
        bool exact = group && group->GetLeaderGUID() == leader->GetGUID() &&
                     group->GetMembersCount() == request.members.size() && !group->isRaidGroup() &&
                     !group->isBGGroup() && !group->isBFGroup();
        if (exact)
            for (FiveRunMember const& member : request.members)
                if (!group->IsMember(member.guid))
                    exact = false;
        if (!exact)
        {
            if (group)
                group->RemoveMember(leader->GetGUID());
            group = new Group();
            if (!group->Create(leader))
            {
                delete group;
                End(run, "failed", "the leader cannot form the group");
                return false;
            }
            sGroupMgr->AddGroup(group);
            for (FiveRunMember const& member : request.members)
            {
                if (member.guid == request.leader)
                    continue;
                Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid);
                if (!bot)
                    continue;
                if (Group* old = bot->GetGroup())
                    old->RemoveMember(bot->GetGUID());
                if (!group->AddMember(bot))
                {
                    End(run, "failed", bot->GetName() + " did not join the group");
                    return false;
                }
            }
        }
        group->SetGroupMemberFlag(request.leader, true, MEMBER_FLAG_MAINTANK);
        group->SetDungeonDifficulty(Difficulty(request.difficulty));
        run.groupGuid = group->GetGUID();

        // the members follow the five-leader's run: no own errands; the portal walk and the corpse run are theirs
        for (FiveRunMember const& member : request.members)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid);
            PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
            if (!botAI || member.human)
                continue;
            bot->SetDungeonDifficulty(Difficulty(request.difficulty));
            // [spec71] its strategies play its talents: a member whose live strategies are another role than its
            // talents (a respec, a talent change since its login) is reset to its talents' strategies before the run.
            // b402 rfc-n five 1: Irrua L15, resto by talents (6 points) and the five's healer by them, healed 0 in
            // all 30 pulls - Lightning Bolt, Flame Shock, Earth Shock ("[trigger flame shock / ele]"): its strategies
            // were the elemental ones; 5 deaths, 1/4 bosses
            {
                bool const healSpec = PlayerbotAI::IsHeal(bot, true), healLive = PlayerbotAI::IsHeal(bot, false);
                bool const tankSpec = PlayerbotAI::IsTank(bot, true), tankLive = PlayerbotAI::IsTank(bot, false);
                std::map<uint8, uint32> tabs = AiFactory::GetPlayerSpecTabs(bot);
                LOG_INFO("playerbots", "[spec71] five {} {} L{} ({} of the five): talents {}/{}/{} (tree {}), spec heal {} "
                         "tank {}, live strategies heal {} tank {}{}",
                         request.fiveId, bot->GetName(), bot->GetLevel(), member.role, tabs[0], tabs[1], tabs[2],
                         AiFactory::GetPlayerSpecTab(bot), healSpec, tankSpec, healLive, tankLive,
                         healSpec != healLive || (tankSpec && !tankLive) ? ": reset to its talents" : "");
                if (healSpec != healLive || (tankSpec && !tankLive))
                    botAI->ResetStrategies();
            }
            botAI->ChangeStrategy(RUN_STRATEGIES_OFF, BOT_STATE_NON_COMBAT);
            botAI->ChangeStrategy("+instance portal,+breathe", BOT_STATE_NON_COMBAT);
            botAI->ChangeStrategy("+ghost portal", BOT_STATE_DEAD);
        }
        // [log28] the members by name (the triage's DUNGEON time per member)
        std::string memberNames;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                memberNames += (memberNames.empty() ? "" : ",") + member->GetName();
        LOG_INFO("playerbots", "[guild70] five {} run {}: group {} formed by {} ({} members: {})", request.fiveId,
                 run.id, run.groupGuid.ToString(), leader->GetName(), group->GetMembersCount(), memberNames);
        return true;
    }

    void DungeonRuns::TickQueue(Run& run)
    {
        FiveRunRequest const& request = run.request;
        Player* leader = ObjectAccessor::FindConnectedPlayer(request.leader);
        if (!leader)
            return;
        lfg::LfgState const state = sLFGMgr->GetState(run.groupGuid);

        // inside: every member on the dungeon's map (the core ported them in), one instance
        bool all = true;
        uint32 instanceId = 0;
        bool mixed = false;
        for (FiveRunMember const& member : request.members)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(member.guid);
            if (!p || !p->IsInWorld() || p->IsBeingTeleported() || p->GetMapId() != request.mapId)
            {
                all = false;
                continue;
            }
            uint32 const id = p->GetMap()->GetInstanceId();
            mixed |= instanceId && id != instanceId;
            instanceId = id;
        }
        if (all && !mixed)
        {
            LOG_INFO("playerbots", "[guild70] five {} run {}: the Dungeon Finder ported the five in (instance {})",
                     request.fiveId, run.id, instanceId);
            GoInside(run);
            return;
        }

        if (!run.queued)
        {
            if (state == lfg::LFG_STATE_NONE)
            {
                // the five-leader joins the Dungeon Finder for this dungeon (as the client's CMSG_LFG_JOIN does)
                lfg::LfgDungeonSet dungeons;
                dungeons.insert(request.lfgId);
                std::string role = "tank";
                for (FiveRunMember const& member : request.members)
                    if (member.guid == request.leader)
                        role = member.role;
                {
                    std::lock_guard<std::mutex> guard(plannedRolesLock);  // [lfg26] before the check is sent
                    for (FiveRunMember const& member : request.members)
                        plannedRoles[member.guid] = LfgRoles(member.role, member.guid == request.leader);
                }
                sLFGMgr->JoinLfg(leader, LfgRoles(role, true), dungeons, "");
                if (sLFGMgr->GetState(run.groupGuid) == lfg::LFG_STATE_NONE)
                {
                    End(run, "not_started", "the dungeon finder did not take the five", true);
                    return;
                }
                run.queued = true;
                // each bot member answers the role check with the role the five planned for it (its CMSG_LFG_SET_ROLES)
                for (FiveRunMember const& member : request.members)
                    if (member.guid != request.leader && !member.human)
                        if (Player* p = ObjectAccessor::FindConnectedPlayer(member.guid); p && GET_PLAYERBOT_AI(p))
                            sLFGMgr->UpdateRoleCheck(run.groupGuid, member.guid, LfgRoles(member.role, false));
                LOG_INFO("playerbots", "[guild70] five {} run {}: the five-leader {} queued the Dungeon Finder for {} "
                         "(dungeon {}), state {}",
                         request.fiveId, run.id, leader->GetName(), request.name, request.lfgId,
                         uint32(sLFGMgr->GetState(run.groupGuid)));
            }
            return;
        }

        // the queue ended without the port (role check failed, proposal declined or expired - the core's rules)
        if (state == lfg::LFG_STATE_NONE)
            End(run, "not_started", "dungeon finder: the group was not moved (role check / proposal)", true);
        else if (mixed)
            End(run, "failed", "the five's members are in different instances " + request.name);
    }

    void DungeonRuns::TickWalkIn(Run& run)
    {
        FiveRunRequest const& request = run.request;
        Position arrival;
        if (!ArrivalInside(request, arrival))
        {
            End(run, "failed", "no portal to " + request.name + " at the entrance");
            return;
        }
        bool all = true;
        uint32 instanceId = 0;
        bool mixed = false;
        for (FiveRunMember const& member : request.members)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(member.guid);
            if (!p)
                return;
            // what the member's own walk in could not do (a fact: the core refused it, or no walk there exists)
            SelfTravel::MoveReport report;
            while (SelfTravel::TakeReport(member.guid, report))
                if (report.action == "move to instance portal" && p->IsAlive())
                {
                    End(run, "failed", p->GetName() + " cannot enter: " + report.reason);
                    return;
                }
            if (p->IsBeingTeleported() || !p->IsInWorld() || p->GetMapId() != request.mapId)
            {
                all = false;
                SelfTravel::PortalGoal goal;
                if (!member.human && !SelfTravel::GetPortalGoal(member.guid, goal))
                {
                    goal.mapId = request.mapId;
                    goal.x = arrival.GetPositionX();
                    goal.y = arrival.GetPositionY();
                    goal.z = arrival.GetPositionZ();
                    SelfTravel::SetPortalGoal(member.guid, goal);
                }
                continue;
            }
            if (!p->m_InstanceValid && p->GetGroup() && p->GetGroup()->GetGUID() == run.groupGuid)
                p->m_InstanceValid = true;  // in the group that owns the instance (Group::_cancelHomebindIfInstance)
            uint32 const id = p->GetMap()->GetInstanceId();
            mixed |= instanceId && id != instanceId;
            instanceId = id;
        }
        if (mixed)
        {
            End(run, "failed", "the five's members are in different instances " + request.name);
            return;
        }
        if (!all)
            return;
        for (FiveRunMember const& member : request.members)
            SelfTravel::ClearPortalGoal(member.guid);
        LOG_INFO("playerbots", "[guild70] five {} run {}: all five walked in (instance {})", request.fiveId, run.id,
                 instanceId);
        GoInside(run);
    }

    void DungeonRuns::GoInside(Run& run)
    {
        FiveRunRequest const& request = run.request;
        run.insideMs = getMSTime();

        // the carried-in key / event items: the five-leader looks into the members' bags. Nobody holds one: the part
        // behind it is skipped honestly (the route skips a starter whose item the leader does not carry) and the need
        // goes to the global leader - never created
        std::vector<Player*> players;
        for (FiveRunMember const& member : request.members)
            if (Player* p = ObjectAccessor::FindConnectedPlayer(member.guid))
                players.push_back(p);
        for (MissingItem const& item : MissingCarriedItems(request.mapId, request.difficulty, players))
        {
            run.needs.push_back(item.name + " (" + std::to_string(item.itemId) + "): " + item.source);
            LOG_INFO("playerbots", "[guild70] five {} run {}: nobody carries {} ({}) - that part is skipped; source: {}",
                     request.fiveId, run.id, item.name, item.itemId, item.source);
        }
        // a key a member holds but not the leader: the leader uses only its own bags (a trade is not built yet)
        Player* leader = ObjectAccessor::FindConnectedPlayer(request.leader);
        for (Knowledge::EventItemDef const* def : Knowledge::EventItems(request.mapId))
        {
            if (!def->dropFrom.empty() || !leader || leader->HasItemCount(def->itemId, 1, false))
                continue;
            for (Player* p : players)
                if (p != leader && p->HasItemCount(def->itemId, 1, false))
                {
                    run.needs.push_back(std::to_string(def->itemId) + " at " + p->GetName() + ", not at the leader");
                    LOG_INFO("playerbots", "[guild70] five {} run {}: {} carries item {}, the leader does not (no "
                             "hand-over yet)",
                             request.fiveId, run.id, p->GetName(), def->itemId);
                    break;
                }
        }

        run.stage = Stage::Inside;
        if (!engine_)
        {
            End(run, "failed", "no clearing engine");
            return;
        }
        std::string error;
        if (!engine_->Begin(request, run.id, error))
            End(run, "failed", "clearing not started: " + error);
    }

    void DungeonRuns::OnCleared(uint32 fiveId, ClearResult const& result)
    {
        auto const itr = runs_.find(fiveId);
        if (itr == runs_.end())
            return;
        Run& run = itr->second;
        FiveRunReport report;
        report.result = result.result;
        report.reason = result.reason;
        report.bossesKilled = result.bossesKilled;
        report.bossesTotal = result.bossesTotal;
        report.wipes = result.wipes;
        report.clearSec = result.durationSec;
        report.deaths = result.deaths;
        LOG_INFO("playerbots", "[guild70] five {} run {}: clearing {} ended {} ({})", fiveId, run.id,
                 result.engineRunId, result.result, result.reason);
        Release(run, false);  // the engine released its group already
        Report(run, report);
    }

    void DungeonRuns::End(Run& run, std::string const& result, std::string const& reason, bool lfgRefused)
    {
        {
            std::lock_guard<std::mutex> guard(plannedRolesLock);  // [lfg26]
            for (FiveRunMember const& member : run.request.members)
                plannedRoles.erase(member.guid);
        }
        FiveRunReport report;
        report.result = result;
        report.reason = reason;
        report.lfgRefused = lfgRefused;
        if (lfgRefused)
            lfgRefused_.insert(run.request.fiveId);
        Release(run, true);
        Report(run, report);
    }

    void DungeonRuns::Release(Run& run, bool disband)
    {
        FiveRunRequest const& request = run.request;
        if (run.queued && sLFGMgr->GetState(run.groupGuid) != lfg::LFG_STATE_NONE &&
            sLFGMgr->GetState(run.groupGuid) != lfg::LFG_STATE_DUNGEON &&
            sLFGMgr->GetState(run.groupGuid) != lfg::LFG_STATE_FINISHED_DUNGEON)
            sLFGMgr->LeaveLfg(run.groupGuid);
        for (FiveRunMember const& member : request.members)
            SelfTravel::ClearPortalGoal(member.guid);
        if (disband && !run.groupGuid.IsEmpty())
            if (Group* group = sGroupMgr->GetGroupByGUID(run.groupGuid.GetCounter()))
                group->Disband();
        // each member is its own leader again (its own strategies, New RPG)
        for (FiveRunMember const& member : request.members)
            if (Player* p = ObjectAccessor::FindConnectedPlayer(member.guid); p && !member.human)
                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(p))
                    botAI->ResetStrategies(false);
    }

    void DungeonRuns::Report(Run& run, FiveRunReport& report)
    {
        FiveRunRequest const request = run.request;
        uint32 const now = getMSTime();
        report.runId = run.id;
        report.contentKey = request.contentKey;
        report.entry = run.entry;
        report.durationSec = getMSTimeDiff(run.startMs, now) / IN_MILLISECONDS;
        report.needs = run.needs;

        // the five-leader's run statistics: the run log (Playerbots.log) and its own run line (guild_runs.jsonl)
        std::string killed;
        for (std::string const& boss : report.bossesKilled)
            killed += (killed.empty() ? "" : ", ") + boss;
        std::string deaths;
        uint32 deathCount = 0;
        for (auto const& [name, count] : report.deaths)
        {
            deaths += (deaths.empty() ? "" : ", ") + name + " x" + std::to_string(count);
            deathCount += count;
        }
        std::string needs;
        for (std::string const& need : report.needs)
            needs += (needs.empty() ? "" : "; ") + need;
        LOG_INFO("playerbots",
                 "[guild70] five {} run {} report: {} {} ({}), in by {}, bosses {}/{} [{}], wipes {}, deaths {} [{}], "
                 "{} s ({} s inside), needs [{}]",
                 request.fiveId, run.id, request.contentKey, report.result, report.reason, report.entry,
                 report.bossesKilled.size(), report.bossesTotal, killed, report.wipes, deathCount, deaths,
                 report.durationSec, report.clearSec, needs);
        {
            std::ofstream out(sLog->GetLogsDir() + "guild_runs.jsonl", std::ios::app);
            if (out)
            {
                std::ostringstream line;
                line << "{\"run\":\"" << JsonText(run.id) << "\",\"five\":" << request.fiveId << ",\"content\":\""
                     << JsonText(request.contentKey) << "\",\"entry\":\"" << report.entry << "\",\"result\":\""
                     << JsonText(report.result) << "\",\"reason\":\"" << JsonText(report.reason)
                     << "\",\"bossesKilled\":" << report.bossesKilled.size() << ",\"bossesTotal\":"
                     << report.bossesTotal << ",\"bosses\":\"" << JsonText(killed) << "\",\"wipes\":" << report.wipes
                     << ",\"deaths\":" << deathCount << ",\"deathList\":\"" << JsonText(deaths)
                     << "\",\"durationSec\":" << report.durationSec << ",\"clearSec\":" << report.clearSec
                     << ",\"needs\":\"" << JsonText(needs) << "\",\"at\":" << uint64(time(nullptr)) << "}";
                out << line.str() << "\n";
            }
        }

        runs_.erase(request.fiveId);  // `run` is gone from here on
        // authority back to the global leader with the report
        if (endHook_)
            endHook_(request.fiveId, report);
    }
}

// the five-leaders' runs tick on the world thread (after the maps updated)
class LeaderDungeonRunWorldScript : public WorldScript
{
public:
    LeaderDungeonRunWorldScript() : WorldScript("LeaderDungeonRunWorldScript", {WORLDHOOK_ON_UPDATE}) {}

    void OnUpdate(uint32 diff) override { Leader::Dungeon::DungeonRuns::Instance().Update(diff); }
};

void AddLeaderDungeonRunScripts() { new LeaderDungeonRunWorldScript(); }
