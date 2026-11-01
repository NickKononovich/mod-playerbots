/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "RouteMeet.h"

#include "LeaderReports.h"
#include "LeaderEvents.h"
#include "AggressiveTargetValue.h"
#include "AiObjectContext.h"
#include "AKTriggers.h"
#include "ANAddTactics.h"
#include "Bag.h"
#include "BotMoveEvents.h"
#include "CellImpl.h"
#include "Chat.h"
#include "CoSTriggers.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "DBCStores.h"
#include "Engine.h"
#include "EscortProtect.h"  // [b36_ohb] EscortGossipHeld
#include "EstimatedLifetimeValue.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GameTime.h"
#include "GDTriggers.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "RaidLeaderState.h"
#include "HoRTriggers.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MechShared.h"
#include "MgTShared.h"
#include "ClassicShared.h"  // [b32_smpull] Mograine's linked chapel trash
#include "MotionMaster.h"
#include "OCTriggers.h"
#include "MoveSpline.h"
#include "AttackerCountValues.h"
#include "MovementActions.h"
#include "LeaderOrders.h"  // [lead1] the tank's pull orders
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "LeaderKnowledge.h"
#include "RouteSearch.h"  // [lead99] the leader's route search
#include "PullFacts.h"    // [lead104] the leader's pull / fight facts
#include "LeaderFightChecks.h"  // [lead107] the leader's checks
#include "LeaderPlan.h"         // [lead107] the leader's planner tick
#include "Pet.h"  // [lead38] the pet redirect
#include <source_location>  // [lead76]
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "PullActions.h"  // [b39_strat] FightHold: the opening fight held on the use side of a key gate
#include "ChooseTargetActions.h"  // [b47_door] PullReadiness::GroupPreparing
#include "SmartAI.h"
#include "SmartScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StuckWatchdog.h"
#include "DamageTaken.h"
#include "DungeonHazard.h"   // [lead16] the leader's hazard view
#include "LeaderActions.h"   // [lead19] the member guard model (LeaderMoveOrderAction::WalkRefusal)
#include "RtiValue.h"        // [lead16] the leader's CC assignments
#include "DungeonTactics.h"  // [lead15] the leader's cleave dodge (InFrontalCleave)
#include "InstanceScript.h"
#include "InstanceSaveMgr.h"
#include "SelfTravelActions.h"
#include "TemporarySummon.h"
#include "TerrainHazard.h"
#include "Transport.h"
#include "UKTriggers.h"
#include "UPTriggers.h"
#include "Timer.h"
#include <chrono>  // [lead15] the leader cadence cost
#include <functional>
#include <optional>  // [lead18]
#include "WaypointMgr.h"
#include "WorldPacket.h"
#include "World.h"
#include "WorldSession.h"
#include "G3D/Vector4.h"  // [b32_brs] HopelessDetourLeg discs
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "RouteTargets.h"
#include "RouteSteer.h"

namespace Leader::Travel
{
    using namespace Leader::Fight;
    using namespace Leader::Knowledge;

    std::unordered_map<uint32, RegroupState> regroups;  // per instance, world thread only

    std::unordered_map<uint32, uint32> regroupCooldownUntilMs;  // per instance, world thread only

    std::unordered_map<uint32, MeetProgress> meetProgress;  // per instance, world thread only

    std::unordered_map<uint32, StragglerEpisode> stragglers;  // per instance, world thread only

    RegroupPathCheck CheckRegroupPath(Player* runner, Movement::PointsArray const& points)
    {
        RegroupPathCheck result;
        if (points.size() < 2)
            return result;
        RouteBackbone const path{points};
        time_t const nowSec = GameTime::GetGameTime().count();
        for (auto const& [spawnId, creature] : runner->GetMap()->GetCreatureBySpawnIdStore())
        {
            if (!creature || !creature->IsInWorld() || creature->IsCritter() || creature->IsCivilian() ||
                creature->IsTotem() || creature->GetCreatureTemplate()->type == CREATURE_TYPE_TOTEM ||
                creature->IsTrigger() || !creature->IsHostileTo(runner))
                continue;
            float const reach = RouteReach(creature, runner);
            if (creature->IsAlive())
            {
                RouteCheck check;
                if (!creature->IsInCombat() && IdleAggroCheck{runner}(creature) &&
                    BlocksRoute(runner, creature, reach, path, check))
                {
                    if (!result.aliveMobs++)
                        result.firstMob = creature->GetName();
                }
                continue;
            }
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            creature->GetRespawnPosition(x, y, z);
            G3D::Vector3 const spawn(x, y, z);
            bool spawnClose = false;
            for (size_t index = 1; index < points.size() && !spawnClose; ++index)
                spawnClose = (ClosestOnSegment(spawn, points[index - 1], points[index]) - spawn).length() < reach;
            if (!spawnClose)
                continue;
            float const in = float(creature->GetRespawnTimeEx() - nowSec);
            if (in > REGROUP_RESPAWN_NEVER_SEC)
                continue;
            if (result.nextRespawnSec < 0.0f || in < result.nextRespawnSec)
            {
                result.nextRespawnSec = in;
                result.nextRespawnMob = creature->GetName();
            }
        }
        return result;
    }

    bool BlackMorassHoldsAtMedivh(Player* member)
    {
        // the_black_morass.h: DATA_SHIELD_PERCENT 13, DATA_RIFT_NUMBER 14; Medivh 15608 stands at (-2025.3, 7119.6).
        constexpr uint32 BM_MAP_ID = 269;
        constexpr uint32 BM_DATA_SHIELD_PERCENT = 13;
        constexpr uint32 BM_DATA_RIFT_NUMBER = 14;
        constexpr float BM_MEDIVH_X = -2025.3f;
        constexpr float BM_MEDIVH_Y = 7119.6f;
        constexpr float BM_MEDIVH_Z = 22.75f;
        constexpr float BM_MEDIVH_HOLD_RANGE = 30.0f;
        if (!member || !member->IsAlive() || member->GetMapId() != BM_MAP_ID)
            return false;
        InstanceScript const* script = member->GetInstanceScript();
        return script && script->GetData(BM_DATA_RIFT_NUMBER) >= 1 && script->GetData(BM_DATA_SHIELD_PERCENT) > 0 &&
               member->GetExactDist(BM_MEDIVH_X, BM_MEDIVH_Y, BM_MEDIVH_Z) <= BM_MEDIVH_HOLD_RANGE;
    }

    bool MeetMember(LeaderGroup& group, std::vector<Player*> const& players, Player* leader)
    {
        uint32 const now = getMSTime();
        // [hleft-bm-tank-fetch] the group holds at Medivh while the tank fetches a keeper: nobody to meet.
        {
            bool fetch = leader && leader->GetMapId() == BLACK_MORASS_MAP;
            for (Player* bot : players)
                if (fetch && bot != leader && bot->IsAlive())
                    fetch = BlackMorassHoldsAtMedivh(bot);
            if (fetch)
                return false;
        }
        // The end of a regroup: the members on "stay" follow again.
        auto const releaseRegroup = [&](char const* why)
        {
            auto const found = regroups.find(group.InstanceId());
            if (found == regroups.end())
                return;
            for (ObjectGuid const guid : found->second.held)
                if (Player* bot = ObjectAccessor::FindPlayer(guid))
                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                        botAI->ChangeStrategy("-stay,+follow", BOT_STATE_NON_COMBAT);
            group.LogEvent("regroup_end", JsonObject().Str("why", why).Num("held", int64(found->second.held.size()))
                                               .Num("replans", found->second.replans));
            regroups.erase(found);
        };

        // Steamvault wave 9 (peel agent): the healer, resurrected and walking back alone, followed the leader
        // straight through the idle packs' aggro radius and died 7 times in the Bog Overlord loop. A group waits for
        // it - here the leader (the others follow it) walks back to meet it; the member keeps following the leader,
        // so the two close in and nobody waits for the other.
        if (!leader || !leader->IsAlive() || leader->IsInCombat() || group.Route().wiped ||
            group.Route().linkIdx >= 0 || !group.Route().escort.IsEmpty())
        {
            group.Route().meeting.Clear();
            meetProgress.erase(group.InstanceId());
            // A dead leader: the group keeps standing where it waits.
            if (leader && leader->IsAlive())
                releaseRegroup(leader->IsInCombat() ? "combat" : "no meeting");
            return false;
        }

        Player* member = nullptr;
        float best = 0.0f;
        for (Player* bot : players)
        {
            if (bot == leader || !bot->IsAlive() || bot->IsInCombat() || bot->GetMap() != leader->GetMap() ||
                bot->IsBeingTeleported() || group.MemberStranded(
bot, leader))
                continue;
            float const distance = leader->GetExactDist(bot);
            // [meet_hysteresis] the member being met stays met down to MEET_END_RANGE
            float const meetFrom = group.Route().meeting == bot->GetGUID() ? MEET_END_RANGE : MEET_RANGE;
            if (distance > meetFrom && (!member || distance > best))
            {
                member = bot;
                best = distance;
            }
        }
        if (!member)
        {
            group.Route().meeting.Clear();
            meetProgress.erase(group.InstanceId());
            stragglers.erase(group.InstanceId());
            releaseRegroup("together");
            return false;
        }

        // Regroup: one side at the entrance (a member back in through the portal), the other farther than
        // MEET_RANGE from all of it.
        std::vector<Player*> atEntrance;
        std::vector<Player*> groupSide;
        Position const* entrance = group.Route().entranceIdx < group.Route().entrances.size()
                                       ? &group.Route().entrances[group.Route().entranceIdx]
                                       : nullptr;
        // A walk out and in (a wing change, a regroup's exit) is under way: nobody walks to meet anybody.
        if (group.Route().wingWalk)
        {
            group.ClearWaypoint(group.LeaderGuid());
            return true;
        }
        // A regroup whose returning side runs already keeps its sides until the two are together (the runner leaves
        // the entrance on its walk).
        auto const running = regroups.find(group.InstanceId());
        Player* const runningMember = running != regroups.end() && running->second.mode == RegroupMode::Run
                                          ? ObjectAccessor::FindPlayer(running->second.runner)
                                          : nullptr;
        Player* const runningAnchor =
            runningMember ? ObjectAccessor::FindPlayer(running->second.anchor) : nullptr;
        if (runningMember && runningAnchor && runningMember->IsAlive() && runningAnchor->IsAlive() &&
            runningMember->GetMap() == leader->GetMap() && runningAnchor->GetMap() == leader->GetMap() &&
            runningMember->GetExactDist(runningAnchor) > MEET_RANGE)
        {
            atEntrance.push_back(runningMember);
            for (Player* bot : players)
                if (bot != runningMember && bot->IsAlive() && bot->GetMap() == leader->GetMap() &&
                    (bot == runningAnchor || bot->GetExactDist(runningMember) > MEET_RANGE))
                    groupSide.push_back(bot);
        }
        else if (ROUTE_REGROUP && !HARNESS_MOVES_BOTS && entrance &&
                 (!regroupCooldownUntilMs.count(group.InstanceId()) ||
                  int32(now - regroupCooldownUntilMs[group.InstanceId()]) >= 0))
        {
            std::vector<Player*> standing;
            for (Player* bot : players)
                if (bot->IsAlive() && !bot->IsInCombat() && bot->GetMap() == leader->GetMap() &&
                    !bot->IsBeingTeleported() && !group.MemberStranded(
bot, leader))
                    standing.push_back(bot);
            // The returning side is a member back in through the portal (REGROUP_BACK_IN_MS), not whoever stands near
            // it: Gruul 25A 2026-10-24, the first pack fights 45-70 yd from the entrance, two mages casting from
            // 30 yd off it were "at the entrance", the off-tank and a warlock 45 yd ahead "the group" - an exit for two
            // with nobody dead or outside, and 22 members put on "stay" for the second regroup.
            auto const backIn = [&group, now](Player* bot)
            {
                for (LeaderGroup::MemberInfo const& candidate : group.Members())
                    if (candidate.guid == bot->GetGUID())
                        return candidate.backInMs && getMSTimeDiff(candidate.backInMs, now) <= REGROUP_BACK_IN_MS;
                return false;
            };
            for (Player* bot : standing)
                if (bot->GetExactDist2d(entrance) <= WING_ARRIVE_RANGE && (!REGROUP_BACK_IN_ONLY || backIn(bot)))
                    atEntrance.push_back(bot);
            for (Player* bot : standing)
            {
                if (std::find(atEntrance.begin(), atEntrance.end(), bot) != atEntrance.end())
                    continue;
                bool away = true;
                for (Player* back : atEntrance)
                    away &= back->GetExactDist(bot) > MEET_RANGE;
                if (away)
                    groupSide.push_back(bot);
            }
        }
        if (atEntrance.empty() || groupSide.empty())
            releaseRegroup("no regroup");
        else
        {
            RegroupState& state = regroups[group.InstanceId()];
            bool const leaderAtEntrance =
                std::find(atEntrance.begin(), atEntrance.end(), leader) != atEntrance.end();
            Player* const runner = leaderAtEntrance ? leader : atEntrance.front();
            // The group's anchor: the member with the most of the group around it.
            Player* anchor = nullptr;
            size_t anchorNear = 0;
            for (Player* bot : groupSide)
            {
                size_t around = 0;
                for (Player* other : groupSide)
                    around += bot->GetExactDist(other) <= REGROUP_GROUP_RADIUS ? 1 : 0;
                if (!anchor || around > anchorNear)
                {
                    anchor = bot;
                    anchorNear = around;
                }
            }

            // The walk of the returning side makes no progress: decided again (meet_stuck).
            if (state.mode == RegroupMode::Run)
            {
                Player* watched = ObjectAccessor::FindPlayer(state.runner);
                Player* target = ObjectAccessor::FindPlayer(state.anchor);
                if (!watched || !target || watched->GetMap() != leader->GetMap() ||
                    target->GetMap() != leader->GetMap())
                    state.mode = RegroupMode::None;
                else
                {
                    float const distance = watched->GetExactDist(target);
                    if (distance + REGROUP_PROGRESS < state.bestDistance)
                    {
                        state.bestDistance = distance;
                        state.progressMs = now;
                    }
                    else if (getMSTimeDiff(state.progressMs, now) > REGROUP_STUCK_MS)
                    {
                        ++state.replans;
                        LOG_INFO("playerbots", "[regroup] {}: meet_stuck: {} {:.1f} yd from {} for {} s, replan {}",
                                 group.Run().runId, watched->GetName(), distance, target->GetName(),
                                 REGROUP_STUCK_MS / IN_MILLISECONDS, state.replans);
                        group.LogEvent("meet_stuck",
                                 JsonObject().Str("name", watched->GetName()).Str("to", target->GetName())
                                     .Real("distance", distance).Num("replan", state.replans)
                                     .Real("x", watched->GetPositionX()).Real("y", watched->GetPositionY())
                                     .Real("z", watched->GetPositionZ()));
                        longRouteCache.erase(watched->GetGUID());
                        state.mode = RegroupMode::None;
                    }
                }
            }

            // The walk out and in is over (TickWingWalk) and the sides are still apart: decided anew, at most
            // REGROUP_MAX_REPLANS times with the stuck walks, then the meeting walk as before.
            if (state.mode == RegroupMode::Exit)
            {
                state.mode = RegroupMode::None;
                ++state.replans;
            }
            if (state.replans > REGROUP_MAX_REPLANS)
            {
                regroupCooldownUntilMs[group.InstanceId()] = now + REGROUP_COOLDOWN_MS;
                releaseRegroup("gave up");
            }
            else
            {
                if (state.mode == RegroupMode::None)
                {
                    // The signal: may the returning side run, or does the group come to the entrance?
                    G3D::Vector3 const goal(anchor->GetPositionX(), anchor->GetPositionY(), anchor->GetPositionZ());
                    PathResult const route = RoutePath(runner, goal.x, goal.y, goal.z);
                    RegroupPathCheck const check = CheckRegroupPath(runner, route.points);
                    float const speed = std::max(1.0f, runner->GetSpeed(MOVE_RUN));
                    float const runSec = route.length / speed + REGROUP_RESPAWN_MARGIN_SEC;
                    bool const respawnSoon = check.nextRespawnSec >= 0.0f && check.nextRespawnSec <= runSec;
                    bool const clear = route.reachable && !check.aliveMobs && !respawnSoon &&
                                       state.replans < REGROUP_MAX_REPLANS;
                    std::string const respawn =
                        check.nextRespawnSec < 0.0f
                            ? std::string("none")
                            : std::to_string(int32(check.nextRespawnSec)) + " s (" + check.nextRespawnMob + ")";
                    char const* const why = !route.reachable                        ? ", no walk"
                                            : state.replans >= REGROUP_MAX_REPLANS ? ", walk stuck"
                                                                                   : "";
                    std::string const decision =
                        clear ? Acore::StringFormat("regroup: {} can run (path clear, next respawn in {})",
                                                    runner->GetName(), respawn)
                              : Acore::StringFormat("regroup: group comes to the entrance ({} mobs alive{}{} / "
                                                    "respawn in {}{})",
                                                    check.aliveMobs, check.aliveMobs ? ", first " : "",
                                                    check.firstMob, respawn, why);
                    LOG_INFO("playerbots",
                             "[regroup] {}: {} (walk {:.0f} yd, {:.0f} s with margin; {} at the entrance, {} in the "
                             "group)",
                             group.Run().runId, decision, route.length, runSec, atEntrance.size(), groupSide.size());
                    JsonArray backNames;
                    for (Player* bot : atEntrance)
                        backNames.Str(bot->GetName());
                    JsonArray groupNames;
                    for (Player* bot : groupSide)
                        groupNames.Str(bot->GetName());
                    group.LogEvent("regroup",
                             JsonObject().Str("decision", clear ? "run" : "exit").Str("signal", decision)
                                 .Str("runner", runner->GetName()).Str("anchor", anchor->GetName())
                                 .Raw("atEntrance", backNames.Build()).Raw("group", groupNames.Build())
                                 .Bool("reachable", route.reachable).Real("pathLength", route.length)
                                 .Real("runSec", runSec).Num("aliveMobs", check.aliveMobs)
                                 .Str("firstMob", check.firstMob).Real("nextRespawnSec", check.nextRespawnSec)
                                 .Str("nextRespawnMob", check.nextRespawnMob)
                                 .Num("replans", state.replans));
                    group.Route().lastEvent = decision;
                    state.runner = runner->GetGUID();
                    state.anchor = anchor->GetGUID();
                    state.bestDistance = runner->GetExactDist(anchor);
                    state.progressMs = now;
                    // REGROUP_EXIT_NEEDS_WALK: the exit only for a group that can walk to the entrance. Wave 21 mech-h:
                    // the group stood on the Mechanar's upper level (joined to the entrance only by the lift link), the
                    // exit gave all four the portal goal, none had a walk ("no walk out for 2 minutes: left out of the
                    // wing walk"), the wing walk held the route 412 s. Refused, the regroup ends and the route (its
                    // route links, the stranded-member rule) brings the sides together.
                    std::string noWalkOut;
                    if (!clear && REGROUP_EXIT_NEEDS_WALK)
                        for (Player* bot : groupSide)
                            if (!RoutePath(bot, entrance->GetPositionX(), entrance->GetPositionY(),
                                           entrance->GetPositionZ())
                                     .complete)
                                noWalkOut += (noWalkOut.empty() ? "" : ",") + bot->GetName();
                    if (clear)
                        state.mode = RegroupMode::Run;
                    else if (!noWalkOut.empty())
                    {
                        LOG_INFO("playerbots",
                                 "[regroup] {}: regroup_exit_refused: no walk to the entrance for {} - no exit, the "
                                 "route goes on",
                                 group.Run().runId, noWalkOut);
                        group.LogEvent("regroup_exit_refused",
                                 JsonObject().Str("noWalkOut", noWalkOut).Str("runner", runner->GetName())
                                     .Str("anchor", anchor->GetName()).Num("replans", state.replans));
                        regroupCooldownUntilMs[group.InstanceId()] = now + REGROUP_COOLDOWN_MS;
                        releaseRegroup("exit refused: no walk to the entrance");
                        return false;
                    }
                    else
                    {
                        // Out through the exit and in through the entrance the returning side stands at, each on its
                        // own ("instance portal" with the exact goal, as a wing change; TickWingWalk ends it there).
                        state.mode = RegroupMode::Exit;
                        for (Player* bot : groupSide)
                            for (LeaderGroup::MemberInfo const& candidate : group.Members())
                                if (candidate.guid == bot->GetGUID())
                                {
                                    SelfTravel::PortalGoal goal;
                                    goal.mapId = group.MapId();
                                    goal.x = entrance->GetPositionX();
                                    goal.y = entrance->GetPositionY();
                                    goal.z = entrance->GetPositionZ();
                                    goal.exact = true;
                                    SelfTravel::SetPortalGoal(candidate.guid, goal);
                                    group.SetMemberHasPortalGoal(candidate.guid, true);
                                }
                        group.Route().wingWalk = true;
                        group.Route().wingWalkSinceMs = now;
                    }
                }

                if (state.mode == RegroupMode::Exit)
                {
                    group.ClearWaypoint(group.LeaderGuid());
                    return true;
                }

                // Run: the group holds where it stands (drinking is above "stay"), the returning side walks to it -
                // the leader by its waypoint, the others at the entrance by following the leader.
                for (Player* bot : groupSide)
                {
                    if (bot == leader)
                        continue;
                    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                    if (!botAI)
                        continue;
                    if (botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) ||
                        !botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
                        botAI->ChangeStrategy("-follow,+stay", BOT_STATE_NON_COMBAT);
                    if (std::find(state.held.begin(), state.held.end(), bot->GetGUID()) == state.held.end())
                        state.held.push_back(bot->GetGUID());
                }
                if (!leaderAtEntrance)
                {
                    group.ClearWaypoint(group.LeaderGuid());
                    return true;
                }
                member = anchor;
                best = leader->GetExactDist(anchor);
            }
        }

        bool const regrouping = regroups.find(group.InstanceId()) != regroups.end();
        StragglerEpisode* episode = nullptr;
        if (!regrouping)
        {
            episode = &stragglers[group.InstanceId()];
            if (episode->member != member->GetGUID() || getMSTimeDiff(episode->lastSeenMs, now) > MEET_EPISODE_GAP_MS)
            {
                *episode = StragglerEpisode();
                episode->member = member->GetGUID();
                episode->sinceMs = now;
                episode->holdSinceMs = now;
                episode->bestDistance = best;
            }
            episode->lastSeenMs = now;
            // The far-member wait counts it far until it is within MEET_RANGE (TickParty), not 50 yd: no flip-flop.
            group.Route().meeting = member->GetGUID();
            if (episode->gaveWay)
            {
                // [meet_end_follow_trail] gave way: the far wait uses its 50 yd line again, not the meet's
                group.Route().meeting.Clear();
                group.Route().lastEvent = "meet: gave way to the follow of " + member->GetName();
                return false;
            }
            if (episode->holdSinceMs)
            {
                if (best + REGROUP_PROGRESS < episode->bestDistance)
                {
                    episode->bestDistance = best;
                    episode->holdSinceMs = now;
                }
                if (getMSTimeDiff(episode->holdSinceMs, now) < MEET_GRACE_MS)
                {
                    group.ClearWaypoint(group.LeaderGuid());
                    group.Route().lastEvent = "meet hold: " + member->GetName() + " follows first";
                    return true;
                }
            }
        }

        PathResult leg;
        G3D::Vector3 const goal(member->GetPositionX(), member->GetPositionY(), member->GetPositionZ());
        SteerLeg(leader, goal, leg);
        if (leg.type & PATHFIND_NOPATH)
            return false;
        G3D::Vector3 const waypoint =
            FloorWaypoint(leader, leg, RouteWaypoint(leg, goal, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP));

        if (episode && episode->holdSinceMs)
        {
            // The grace is over and the member did not come: the leader walks back to it.
            episode->holdSinceMs = 0;
            meetProgress[group.InstanceId()] = {member->GetGUID(), best, now};
            group.LogEvent("meet_member", JsonObject().Str("name", member->GetName()).Real("distance", best)
                                               .Num("farSec", getMSTimeDiff(episode->sinceMs, now) / IN_MILLISECONDS)
                                               .Num("stuck", episode->stuck)
                                               .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                               .Real("z", leader->GetPositionZ()));
        }
        else if (regrouping && group.Route().meeting != member->GetGUID())
        {
            group.Route().meeting = member->GetGUID();
            meetProgress[group.InstanceId()] = {member->GetGUID(), best, now};
            group.LogEvent("meet_member", JsonObject().Str("name", member->GetName()).Real("distance", best)
                                               .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                               .Real("z", leader->GetPositionZ()));
        }
        else if (episode)
        {
            // The leader's walk back to the member makes no progress: its route is searched anew (meet_stuck) after
            // another grace for the member's own follow; MEET_MAX_STUCK times, then the leader gives way.
            MeetProgress& progress = meetProgress[group.InstanceId()];
            if (progress.member != member->GetGUID())
                progress = {member->GetGUID(), best, now};
            else if (best + REGROUP_PROGRESS < progress.bestDistance)
            {
                progress.bestDistance = best;
                progress.progressMs = now;
            }
            else if (getMSTimeDiff(progress.progressMs, now) > REGROUP_STUCK_MS)
            {
                ++episode->stuck;
                episode->gaveWay = episode->stuck >= MEET_MAX_STUCK;
                group.LogEvent("meet_stuck", JsonObject().Str("name", leader->GetName()).Str("to", member->GetName())
                                                  .Real("distance", best).Num("stuck", episode->stuck)
                                                  .Str("next", episode->gaveWay ? "give way" : "hold, then walk back")
                                                  .Real("x", leader->GetPositionX())
                                                  .Real("y", leader->GetPositionY())
                                                  .Real("z", leader->GetPositionZ()));
                longRouteCache.erase(leader->GetGUID());
                progress.progressMs = now;
                progress.bestDistance = best;
                group.ClearWaypoint(group.LeaderGuid());
                if (episode->gaveWay)
                {
                    LOG_INFO("playerbots",
                             "[regroup] {}: {} gives way to {} ({:.1f} yd, far {} s, {} meet_stuck): no more walks "
                             "back, its own follow comes (the guard's deadlock breaker), the far wait times out",
                             group.Run().runId, leader->GetName(), member->GetName(), best,
                             getMSTimeDiff(episode->sinceMs, now) / IN_MILLISECONDS, episode->stuck);
                    group.LogEvent("meet_give_way",
                             JsonObject().Str("name", member->GetName()).Real("distance", best)
                                 .Num("farSec", getMSTimeDiff(episode->sinceMs, now) / IN_MILLISECONDS)
                                 .Num("stuck", episode->stuck));
                    return false;
                }
                episode->holdSinceMs = now;
                episode->bestDistance = best;
                return true;
            }
        }

        // ROUTE_HARNESS_WALK_GUARD: the walk back never wakes an idle pack (lbrs-n wave 7 01:40:38: the route up to
        // Rlhunas ran through the refused Firebrand pack, 24 mobs, wipe). At the edge the leader holds; that is no
        // progress, so meet_stuck and then the give-way end it.
        G3D::Vector3 walkTo = waypoint;
        Creature* blocker = nullptr;
        bool held = false;
        // Only a pack the tank's planner refused holds the walk back; with no verdict (or an accepted one) the walk
        // goes on and the tank fights what it meets (strat-h wave 11: the meet stopped at a lone Mangled Cadaver,
        // "none, 0 mobs", for 100 s until the give-way, the members 60 yd behind it never came).
        bool refusedBlocker = false;
        if (ROUTE_HARNESS_WALK_GUARD &&
            HarnessWalkStop(leader, leg.points, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP, nullptr, walkTo,
                            blocker, held, &group))
        {
            bool accepted = true;
            uint32 mobs = 0;
            std::list<Creature*> pack;
            PackMobCheck packCheck{leader, blocker};
            Acore::CreatureListSearcher<PackMobCheck> packSearcher(blocker, pack, packCheck);
            Cell::VisitObjects(blocker, packSearcher, PACK_RADIUS);
            // Refused now, refused or skipped before (the verdict ages out in seconds), or a real pack: held.
            refusedBlocker = (PartyVerdict(group, blocker->GetGUID(), accepted, mobs) &&
                              !accepted) ||
                             group.Route().carefulRefused.count(blocker->GetGUID()) ||
                             group.Route().skipUntil.count(blocker->GetGUID()) ||
                             pack.size() > MEET_PASS_PACK_MAX;
            if (!refusedBlocker)
            {
                walkTo = waypoint;
                held = false;
            }
            else
                LogHarnessHold(group, leader, blocker, "meet", walkTo.x, walkTo.y, walkTo.z);
            if (held)
            {
                group.ClearWaypoint(group.LeaderGuid());
                return true;
            }
        }
        group.SetLeaderWaypoint(leader, walkTo.x, walkTo.y, walkTo.z);
        return true;
    }
}
