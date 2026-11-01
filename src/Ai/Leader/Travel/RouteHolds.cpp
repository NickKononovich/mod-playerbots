/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "RouteHolds.h"

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
#include "LeaderRest.h"  // FAR_* / ApartMetric

namespace Leader::Travel
{
    using Leader::Dungeon::FAR_RESUME_RANGE;
    using Leader::Dungeon::FAR_SLOPE_DZ_ALLOWANCE;
    using Leader::Dungeon::FAR_WAIT_RANGE;

    bool PostKillWaveHold(LeaderGroup& group, Player* leader, uint32 now)
    {
        GroupRouteState& route = group.Route();
        // [b32_sfk_door] A boss whose death starts a scripted wave that opens the next door: the group
        // stays at the kill spot until the wave came. Shadowfang Keep: Fenrus the Devourer 4274's death
        // sets data on Archmage Arugal 4275 (smart_scripts 4274 id 2), whose script 427500 summons four
        // Arugal's Voidwalkers 4627 at Fenrus' spot ~13 s later (creature_summon_groups 4275 group 1,
        // TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT 60 s; "On Reset - Attack Start" the nearest player
        // within 40 yd); a Voidwalker's death opens the Sorcerer's Gate 18972 (smart_scripts 4627 id 1).
        // sfk-h 20261025-223844-sfk-h-5B-83: Fenrus died at 1453 s, shortcut_reenter at 1454 s walked
        // the group out to the entrance, the Voidwalkers met nobody within 40 yd and despawned, the gate
        // never opened: Nandos / Arugal "behind a closed door", stalled at 1891 s. The four passing runs
        // of the day fought the Voidwalkers 13-14 s after the kill.
        bool waveHold = false;
        {
            struct PostKillWaveDef
            {
                uint32 mapId;
                uint32 bossEntry;
                uint32 waveEntry;
                uint32 doorEntry;
                uint32 waitMs;  // the wave comes this long after the kill at the latest
            };
            static constexpr PostKillWaveDef POST_KILL_WAVES[] = {
                {33, 4274, 4627, 18972, 30 * IN_MILLISECONDS},
            };
            // Party::instanceId -> since when the group waits for the wave (world thread only)
            static std::unordered_map<uint32, uint32> waveWaitSince;
            static std::unordered_map<uint32, uint32> waveWaitLogged;  // 1 started, 2 ended
            Map* waveMap = leader->GetMap();
            for (PostKillWaveDef const& wave : POST_KILL_WAVES)
            {
                if (!waveMap || waveMap->GetId() != wave.mapId || !route.deadEntries.count(wave.bossEntry))
                    continue;
                bool doorClosed = false;
                for (auto const& [spawnId, go] : waveMap->GetGameObjectBySpawnIdStore())
                    doorClosed |= go && go->isSpawned() && go->GetEntry() == wave.doorEntry &&
                                  go->GetGoState() == GO_STATE_READY;
                uint32& since = waveWaitSince[group.InstanceId()];
                uint32& logged = waveWaitLogged[group.InstanceId()];
                if (!doorClosed)
                    continue;
                // A wave creature alive: the group fights it (the normal targeting), no hold.
                if (leader->FindNearestCreature(wave.waveEntry, 150.0f, true))
                    continue;
                if (!since)
                    since = now ? now : 1;
                if (getMSTimeDiff(since, now) < wave.waitMs)
                {
                    waveHold = true;
                    if (!logged)
                    {
                        logged = 1;
                        group.LogEvent("post_kill_wave_hold",
                                 JsonObject().Str("state", "start").Num("boss", wave.bossEntry)
                                     .Num("wave", wave.waveEntry).Num("door", wave.doorEntry)
                                     .Num("waitSec", wave.waitMs / IN_MILLISECONDS));
                    }
                }
                else if (logged == 1)
                {
                    logged = 2;
                    group.LogEvent("post_kill_wave_hold",
                             JsonObject().Str("state", "no wave, door still closed")
                                 .Num("boss", wave.bossEntry).Num("wave", wave.waveEntry)
                                 .Num("door", wave.doorEntry));
                }
            }
        }
        if (waveHold)
            group.ClearWaypoint(group.LeaderGuid());
        return waveHold;
    }

    bool LevelSplitRejoin(LeaderGroup& group, Player* leader, uint32 now)
    {
        GroupRouteState& route = group.Route();
        // [b35_brd] level-split rejoin. brd-h 20261026-024524-brd-h-5G-5 (1934-2173 s, stalled): after
        // the Ring of Law the tank stopped on the arena stands (618.7,-125.9,-34.1) 24 yd from a
        // refused 2-Medic pack; the members' follow guard ("would wake Anvilrage Medic -> alt shorter
        // (619.6,-124.6,-43.2)" / "-> stay") dropped three of them to the floor 12 yd below, the healer
        // out of the tank's sight. ApartMetric read them 4-5 yd "apart" (no far wait, no meet), the
        // healer's lost sight held GroupFullyReady false (no pull_after_hold, no body pull, no skip),
        // the compact pause cycled 30 s on / 20 s off, "arrived (the tank's pull decides)" with no tank
        // target: 4 minutes. A leader standing LEVEL_SPLIT_AFTER_MS without a walk while a member is on
        // another level close below / above (|dz| over FAR_SLOPE_DZ_ALLOWANCE, within FAR_WAIT_RANGE,
        // no line of sight) walks to that member - the healer first - for LEVEL_SPLIT_WALK_MS at most,
        // LEVEL_SPLIT_MAX times per instance, then the stop is decided anew from there.
        bool levelRejoin = false;
        {
            constexpr uint32 LEVEL_SPLIT_AFTER_MS = 60 * IN_MILLISECONDS;
            constexpr uint32 LEVEL_SPLIT_WALK_MS = 25 * IN_MILLISECONDS;
            constexpr uint32 LEVEL_SPLIT_COOLDOWN_MS = 90 * IN_MILLISECONDS;
            constexpr uint32 LEVEL_SPLIT_MAX = 3;
            struct LevelSplit
            {
                ObjectGuid member;
                uint32 untilMs = 0;
                uint32 lastMs = 0;
                uint32 count = 0;
            };
            static std::unordered_map<uint32, LevelSplit> levelSplits;  // instance id (world)
            LevelSplit& split = levelSplits[group.InstanceId()];
            if (split.untilMs)
            {
                Player* bot = ObjectAccessor::FindPlayer(split.member);
                bool const gone = !bot || !bot->IsAlive() || bot->GetMap() != leader->GetMap();
                bool const together =
                    !gone &&
                    std::fabs(bot->GetPositionZ() - leader->GetPositionZ()) <= FAR_SLOPE_DZ_ALLOWANCE &&
                    bot->GetExactDist(leader) <= FAR_RESUME_RANGE && bot->IsWithinLOSInMap(leader);
                bool const timedOut = int32(now - split.untilMs) >= 0;
                if (gone || together || timedOut || leader->IsInCombat())
                {
                    group.LogEvent("level_split_rejoin",
                             JsonObject().Str("state", "end")
                                 .Str("why", gone        ? "member gone"
                                             : together  ? "together"
                                             : timedOut  ? "timed out"
                                                         : "combat")
                                 .Num("count", split.count)
                                 .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                 .Real("z", leader->GetPositionZ()));
                    split.untilMs = 0;
                    group.ClearWaypoint(group.LeaderGuid());
                    // the old stop is no stop any more: Steer decides from here
                    route.arrivedMs = 0;
                    route.stopSinceMs = 0;
                    route.guaranteeHolding = false;
                    route.approachBlocker.Clear();
                    route.nextRetargetMs = now;
                }
                else
                {
                    levelRejoin = true;
                    group.SetLeaderWaypoint(leader, bot->GetPositionX(), bot->GetPositionY(),
                                      bot->GetPositionZ());
                }
            }
            else if (route.noWalkSinceMs &&
                     getMSTimeDiff(route.noWalkSinceMs, now) >= LEVEL_SPLIT_AFTER_MS &&
                     split.count < LEVEL_SPLIT_MAX &&
                     (!split.lastMs || getMSTimeDiff(split.lastMs, now) >= LEVEL_SPLIT_COOLDOWN_MS) &&
                     !leader->IsInCombat() && !route.approach && route.escort.IsEmpty() &&
                     route.linkIdx < 0 && !group.IsRaid())
            {
                Player* splitBot = nullptr;
                bool splitHealer = false;
                for (LeaderGroup::MemberInfo const& member : group.Members())
                {
                    Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    if (!bot || bot == leader || !bot->IsAlive() || bot->GetMap() != leader->GetMap() ||
                        bot->IsInCombat() || group.MemberStranded(bot, leader) ||
                        std::fabs(bot->GetPositionZ() - leader->GetPositionZ()) <=
                            FAR_SLOPE_DZ_ALLOWANCE ||
                        bot->GetExactDist2d(leader) > FAR_WAIT_RANGE || bot->IsWithinLOSInMap(leader))
                        continue;
                    bool const healer = PlayerbotAI::IsHeal(bot);
                    if (!splitBot || (healer && !splitHealer))
                    {
                        splitBot = bot;
                        splitHealer = healer;
                    }
                }
                if (splitBot)
                {
                    split.member = splitBot->GetGUID();
                    split.untilMs = now + LEVEL_SPLIT_WALK_MS;
                    split.lastMs = now ? now : 1;
                    ++split.count;
                    levelRejoin = true;
                    LOG_INFO("playerbots",
                             "[b35_brd] level_split_rejoin {}: {} stood {} s without a walk, {} is "
                             "{:.1f} yd below / above out of sight ({:.1f} yd away): the leader walks "
                             "to it ({} of {})",
                             group.Run().runId, leader->GetName(),
                             getMSTimeDiff(route.noWalkSinceMs, now) / IN_MILLISECONDS,
                             splitBot->GetName(), splitBot->GetPositionZ() - leader->GetPositionZ(),
                             splitBot->GetExactDist2d(leader), split.count, LEVEL_SPLIT_MAX);
                    group.LogEvent("level_split_rejoin",
                             JsonObject().Str("state", "start").Str("member", splitBot->GetName())
                                 .Bool("healer", splitHealer)
                                 .Real("dz", splitBot->GetPositionZ() - leader->GetPositionZ())
                                 .Real("horizontal", splitBot->GetExactDist2d(leader))
                                 .Num("noWalkSec", getMSTimeDiff(route.noWalkSinceMs, now) /
                                                       IN_MILLISECONDS)
                                 .Num("count", split.count)
                                 .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                 .Real("z", leader->GetPositionZ())
                                 .Real("toX", splitBot->GetPositionX())
                                 .Real("toY", splitBot->GetPositionY())
                                 .Real("toZ", splitBot->GetPositionZ()));
                    group.SetLeaderWaypoint(leader, splitBot->GetPositionX(), splitBot->GetPositionY(),
                                      splitBot->GetPositionZ());
                }
            }
        }
        return levelRejoin;
    }

    void NoWalkWatch(LeaderGroup& group, Player* leader, uint32 now, bool compactPause, bool traveling)
    {
        GroupRouteState& route = group.Route();
        // [b33_sh2] no silent hold: a target set, phase traveling, no harness waypoint and the leader
        // standing for NO_WALK_LOG_MS is logged (harness_hold "no walk") with the state that holds
        // it, every NO_WALK_LOG_MS (sh-n 5B-89 stood 140 s with no approach_stop / pull_hold /
        // body_pull / harness_hold at all).
        {
            constexpr uint32 NO_WALK_LOG_MS = 20 * IN_MILLISECONDS;
            LeaderGroup::WalkPoint noWalkWp;
            bool const noWalk = !route.target.IsEmpty() && traveling &&
                                !group.GetWaypoint(group.LeaderGuid(), noWalkWp) &&
                                !leader->isMoving();
            if (!noWalk)
                route.noWalkSinceMs = 0;
            else if (!route.noWalkSinceMs)
                route.noWalkSinceMs = now ? now : 1;
            else if (getMSTimeDiff(route.noWalkSinceMs, now) >= NO_WALK_LOG_MS &&
                     (!route.noWalkLogMs || getMSTimeDiff(route.noWalkLogMs, now) >= NO_WALK_LOG_MS))
            {
                route.noWalkLogMs = now ? now : 1;
                Unit const* held = leader->GetMap()->GetCreature(route.target);
                std::string state;
                auto const add = [&state](std::string const& part)
                { state += (state.empty() ? "" : "; ") + part; };
                if (compactPause)
                    add("compact pause");
                if (route.approach)
                    add("body pull walk");
                if (route.guaranteeHolding)
                    add("guarantee hold");
                // [b35_brd] "arrived" is the stop (an approach stop at a neighbour pack), not the
                // target: brd-h 5G-5 read "arrived ... (the tank's pull decides)" 147.8 yd from
                // Loregrain - the stop was 24 yd from an Anvilrage Medic the planner refused, and the
                // group never stood ready (healer below, out of sight): the stop and the readiness are
                // named.
                if (route.arrivedMs)
                {
                    Creature const* stopBy = route.approachBlocker.IsEmpty()
                                                 ? nullptr
                                                 : leader->GetMap()->GetCreature(route.approachBlocker);
                    std::string notReady;
                    for (LeaderGroup::MemberInfo const& member : group.Members())
                    {
                        Player* bot = ObjectAccessor::FindPlayer(member.guid);
                        if (!bot || bot == leader)
                            continue;
                        if (!bot->IsAlive() || bot->GetMap() != leader->GetMap())
                            notReady += (notReady.empty() ? "" : ", ") + member.name + " dead / away";
                        else if (bot->GetDistance(leader) > 30.0f)
                            notReady += (notReady.empty() ? "" : ", ") +
                                        Acore::StringFormat("{} {:.0f} yd", member.name,
                                                            bot->GetDistance(leader));
                        else if (PlayerbotAI::IsHeal(bot) && !bot->IsWithinLOSInMap(leader))
                            notReady += (notReady.empty() ? "" : ", ") +
                                        Acore::StringFormat("healer {} out of sight (dz {:.1f})",
                                                            member.name,
                                                            bot->GetPositionZ() -
                                                                leader->GetPositionZ());
                    }
                    // [b43_harn] Steer stamps arrivedMs with a getMSTime() later than this tick's
                    // `now`: getMSTimeDiff wrapped to "arrived 4294967 s ago". Ahead of now is 0 s.
                    uint32 arrivedSec = 0;
                    if (int32(now - route.arrivedMs) > 0)
                        arrivedSec = getMSTimeDiff(route.arrivedMs, now) / IN_MILLISECONDS;
                    // [lead13] the leader owns the pull: its planner decides, not the tank
                    add(Acore::StringFormat(
                        "arrived {} s ago at {} ({}){}", arrivedSec,
                        group.LeaderDecides() ? "the leader's planner decides"
                                             : "the tank's pull decides",
                        stopBy ? Acore::StringFormat("the stop by {} {:.1f} yd", stopBy->GetName(),
                                                     leader->GetExactDist(stopBy))
                               : std::string("the target's stop"),
                        notReady.empty() ? std::string() : "; group not ready: " + notReady));
                    // [lead37] the route is data (lead34): what holds is the leader's verdict
                    bool leaderReady = false;
                    std::string leaderWhy;
                    Group const* leaderGroup = leader->GetGroup();
                    if (group.LeaderDecides() && leaderGroup &&
                        Leader::Readiness::Get(leaderGroup->GetGUID(), leaderReady, &leaderWhy) &&
                        !leaderReady)
                        add("the leader holds: " + leaderWhy);
                }
                else
                    add("not arrived");  // [b43_harn] arrivedMs 0: no stop reached yet
                if (route.patrolWaitUntilMs && int32(route.patrolWaitUntilMs - now) > 0)
                    add("patrol wait");
                if (leader->IsInCombat())
                    add(Acore::StringFormat("leader flagged in combat, {} attackers",
                                            leader->getAttackers().size()));
                // [lead14] only while it still holds: b58 mgt-h read "last hold: leader in combat with
                // attackers" for 4 minutes out of combat
                if (!route.harnessHoldWhy.empty() && leader->IsInCombat())
                    add("last hold: " + route.harnessHoldWhy);
                if (state.empty())
                    add("unexplained: Steer set no waypoint");
                LOG_INFO("playerbots",
                         "[b33_sh2] harness_hold {}: no walk for {} s to {} ({:.1f} yd): {}",
                         leader->GetName(), getMSTimeDiff(route.noWalkSinceMs, now) / IN_MILLISECONDS,
                         held ? held->GetName() : "?", held ? leader->GetExactDist(held) : -1.0f,
                         state);
                group.LogEvent("harness_hold",
                         JsonObject().Str("why", "no walk").Str("state", state)
                             .Str("target", held ? held->GetName() : "")
                             .Real("distance", held ? leader->GetExactDist(held) : -1.0f)
                             .Num("sec", getMSTimeDiff(route.noWalkSinceMs, now) / IN_MILLISECONDS)
                             .Str("routeRule", route.targetRouteRule));
                // [lead14] the general breaker: the leader stood LEADER_NO_WALK_BREAK_MS out of combat
                // with no order open, no door gather and no escort - the hold that holds it is named
                // and, when it is a patrol (a wait or a pull that never ends), broken: the patrol is
                // banned from waits and pulls for 5 minutes, the group walks on (the old patrol
                // budget's end). Other holds are reported (leader_unstuck), their own rules end them.
                constexpr uint32 LEADER_NO_WALK_BREAK_MS = 60 * IN_MILLISECONDS;
                if (group.LeaderDecides() && !leader->IsInCombat() && !route.doorGatherHeld &&
                    route.escort.IsEmpty() && !Leader::Orders::IsOpen(group.LeaderGuid()) &&
                    getMSTimeDiff(route.noWalkSinceMs, now) >= LEADER_NO_WALK_BREAK_MS)
                {
                    auto& lead = group.State();
                    // [lead41] patrols are killed, never waited out (LEADER_PATROL_IS_TARGET): the
                    // patrol pick (intercept) is no hold, only a wait would be one
                    bool const waitOnly =
                        !lead.waitPatrol.IsEmpty() || Leader::LEADER_PATROL_IS_TARGET;
                    ObjectGuid const patrol = waitOnly ? lead.waitPatrol : lead.intercept;
                    std::string broke = "nothing to break: reported";
                    // [lead24] LEADER_NO_FORCING_TIMERS: no ban because time passed (waited_long)
                    if (!patrol.IsEmpty() && Leader::LEADER_NO_FORCING_TIMERS)
                        broke = "waited_long: no break (the patrol hold stays)";
                    else if (!patrol.IsEmpty())
                    {
                        lead.interceptBanned[patrol] = now + 5 * MINUTE * IN_MILLISECONDS;
                        lead.intercept.Clear();
                        lead.interceptMs = 0;
                        lead.interceptSinceMs = 0;
                        lead.waitPatrol.Clear();
                        broke = "the patrol hold: banned, the group walks on";
                    }
                    LOG_INFO("playerbots", "[lead14] leader_unstuck {}: {} s without a walk ({}): {}",
                             group.Run().runId, getMSTimeDiff(route.noWalkSinceMs, now) / IN_MILLISECONDS,
                             state, broke);
                    group.LogEvent("leader_unstuck",
                             JsonObject().Str("state", state).Str("action", broke)
                                 .Num("sec", getMSTimeDiff(route.noWalkSinceMs, now) /
                                                 IN_MILLISECONDS));
                }
            }
        }
    }
}
