/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "RouteReenter.h"

#include "AiObjectContext.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "RouteLegs.h"
#include "RouteMeet.h"
#include "RouteTargets.h"
#include "SelfTravelActions.h"
#include "StuckWatchdog.h"

#include <algorithm>

namespace Leader::Travel
{
    bool RespawnZoneOnWalk(uint32 instanceId, Movement::PointsArray const& points)
    {
        auto const found = respawnZones.find(instanceId);
        if (!ROUTE_RESPAWN_IGNORE || found == respawnZones.end() || points.empty())
            return false;
        for (G3D::Vector3 const& point : found->second)
            for (size_t index = 0; index < points.size(); ++index)
            {
                G3D::Vector3 closest = points[index];
                if (index + 1 < points.size())
                {
                    G3D::Vector3 const segment = points[index + 1] - points[index];
                    float const lengthSq = segment.squaredLength();
                    float const t = lengthSq > 0.0f
                                        ? std::clamp((point - points[index]).dot(segment) / lengthSq, 0.0f, 1.0f)
                                        : 0.0f;
                    closest = points[index] + segment * t;
                }
                if ((closest - point).length() < RESPAWN_ZONE_RADIUS)
                    return true;
            }
        return false;
    }

    bool StrandedReenter(LeaderGroup& group, ObjectGuid memberGuid, Player* bot, Player* leader)
    {
        GroupRouteState& route = group.Route();
        MemberReenter& reenter = route.reenter[memberGuid];
        auto const reset = [&reenter]()
        {
            reenter.sinceMs = 0;
            reenter.watchdogSec = 0;
        };

        Player* reference = bot != leader ? leader : nullptr;
        if (bot == leader)
            for (LeaderGroup::MemberInfo const& other : group.Members())
                if (Player* candidate = ObjectAccessor::FindPlayer(other.guid);
                    candidate && candidate != bot && candidate->IsAlive() && candidate->GetMap() == bot->GetMap() &&
                    (!reference || bot->GetExactDist(candidate) < bot->GetExactDist(reference)))
                    reference = candidate;

        Map* map = bot->GetMap();
        if (!reference || !leader || !bot->IsAlive() || !reference->IsAlive() || !map || !map->IsDungeon() ||
            reference->GetMap() != map || bot->IsInCombat() || reference->IsInCombat() || bot->IsBeingTeleported() ||
            bot->GetTransport() || bot->GetVehicle() || bot->IsFlying() || reference->GetTransport() ||
            reference->GetVehicle() || reference->IsFlying() ||  // a lift, a drake (Oculus): no walk is the plan
            route.linkIdx >= 0 || !route.escort.IsEmpty() || route.wingWalk || group.MemberHasPortalGoal(memberGuid) ||
            route.entrances.empty() || route.entranceIdx >= route.entrances.size() ||
            bot->GetExactDist(reference) <= MEET_RANGE || reenter.reentries >= STRANDED_REENTER_MAX)
        {
            reset();
            return false;
        }

        uint32 now = getMSTime();
        now = now ? now : 1;
        if (reenter.checkMs && getMSTimeDiff(reenter.checkMs, now) < STRANDED_RECHECK_MS)
            return false;
        reenter.checkMs = now;

        PathResult const out =
            RoutePath(bot, reference->GetPositionX(), reference->GetPositionY(), reference->GetPositionZ());
        if (out.complete)
        {
            reset();
            return false;
        }

        if (!reenter.sinceMs || bot->GetExactDist(&reenter.pos) > STRANDED_STILL_RANGE)
        {
            reset();
            reenter.sinceMs = now;
            reenter.pos = bot->GetPosition();
            return false;
        }

        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            if (auto* watchdog = dynamic_cast<StuckWatchdogValue*>(
                    botAI->GetAiObjectContext()->GetUntypedValue("stuck watchdog")))
                reenter.watchdogSec = std::max(reenter.watchdogSec, watchdog->GetStuckSeconds());

        uint32 const noWalkMs = getMSTimeDiff(reenter.sinceMs, now);
        if (noWalkMs < STRANDED_REENTER_MS || reenter.watchdogSec < STRANDED_REENTER_WATCHDOG_S)
            return false;

        Position const& entrance = route.entrances[route.entranceIdx];
        G3D::Vector3 const entranceAt(entrance.GetPositionX(), entrance.GetPositionY(), entrance.GetPositionZ());
        PathResult const back = RoutePath(bot, reference->GetPositionX(), reference->GetPositionY(),
                                          reference->GetPositionZ(), &entranceAt);
        if (!back.complete)
        {
            // re-entering would not help: no walk from the entrance either (kept as stranded, logged once)
            if (reenter.watchdogSec != UINT32_MAX)
                group.LogEvent("stranded_reenter",
                               JsonObject().Str("name", bot->GetName()).Str("state", "refused")
                                   .Str("why", "no walk from the entrance to the group either")
                                   .Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                                   .Real("z", bot->GetPositionZ()));
            reenter.watchdogSec = UINT32_MAX;  // logged; the watch restarts when the member moves
            return false;
        }

        G3D::Vector3 const from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        float const distance = bot->GetExactDist(reference);
        group.RecordReentry(bot, "stranded_reenter: no path back, re-entered the instance",
                            JsonObject().Str("kind", "stranded").Str("goal", reference->GetName())
                                .Real("distance", distance).Num("noWalkSec", noWalkMs / IN_MILLISECONDS)
                                .Real("walk", -1.0f).Real("entranceWalk", back.length)
                                .Num("entrance", int64(route.entranceIdx)).Real("toX", entrance.GetPositionX())
                                .Real("toY", entrance.GetPositionY()).Real("toZ", entrance.GetPositionZ()));
        // [guild70] a real group (no test teleports): the member walks out through the exit and back in at the
        // entrance itself (its own "instance portal" with an exact goal); ReturnInside ends the goal back inside
        if (!group.TestTeleports())
        {
            SelfTravel::PortalGoal goal;
            goal.mapId = map->GetId();
            goal.x = entrance.GetPositionX();
            goal.y = entrance.GetPositionY();
            goal.z = entrance.GetPositionZ();
            goal.exact = true;
            SelfTravel::SetPortalGoal(memberGuid, goal);
            group.SetMemberHasPortalGoal(memberGuid, true);
            reenter.walking = true;
            reenter.seenOutside = false;
            ++reenter.reentries;
            reset();
            LOG_INFO("playerbots",
                     "[guild70] {}: {} had no walk back to {} for {} s: walks out through the exit and back in at "
                     "entrance {} (counted as a death, {} of {})",
                     group.Run().runId, bot->GetName(), reference->GetName(), noWalkMs / IN_MILLISECONDS,
                     route.entranceIdx, reenter.reentries, STRANDED_REENTER_MAX);
            group.LogEvent("stranded_reenter",
                           JsonObject().Str("name", bot->GetName()).Str("state", "walks out and in")
                               .Str("to", reference->GetName()).Num("noWalkSec", noWalkMs / IN_MILLISECONDS)
                               .Real("distance", distance).Real("x", from.x).Real("y", from.y).Real("z", from.z)
                               .Num("entrance", int64(route.entranceIdx)).Num("count", reenter.reentries)
                               .Bool("countedAsDeath", true).Bool("walked", true));
            return true;
        }
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        if (!bot->TeleportTo(map->GetId(), entrance.GetPositionX(), entrance.GetPositionY(), entrance.GetPositionZ(),
                             entrance.GetOrientation()))
        {
            group.LogEvent("stranded_reenter",
                           JsonObject().Str("name", bot->GetName()).Str("state", "teleport refused (death recorded)"));
            reset();
            return false;
        }

        ++reenter.reentries;
        // [b32_sh_ooze] The walk from the entrance crosses a respawn zone: the member counts as back in through the
        // portal (MemberInfo::backInMs), so MeetMember's regroup decides - it runs only on a clear walk, else the group
        // comes to the entrance and walks back with it - instead of a lone walk through the refilled mobs.
        bool const respawnZone = RespawnZoneOnWalk(group.InstanceId(), back.points);
        group.NoteReentered(memberGuid, respawnZone, now);
        reset();
        LOG_INFO("playerbots",
                 "[stranded_reenter] {}: {} had no walk back to {} for {} s (watchdog stuck {} s, {:.0f} yd, at "
                 "{:.1f} {:.1f} {:.1f}): re-enters the instance at entrance {} - counted as a death ({} of {})",
                 group.Run().runId, bot->GetName(), reference->GetName(), noWalkMs / IN_MILLISECONDS,
                 reenter.watchdogSec, distance, from.x, from.y, from.z, route.entranceIdx, reenter.reentries,
                 STRANDED_REENTER_MAX);
        group.LogEvent("stranded_reenter",
                       JsonObject().Str("name", bot->GetName()).Str("state", "re-entered")
                           .Str("to", reference->GetName()).Num("noWalkSec", noWalkMs / IN_MILLISECONDS)
                           .Real("distance", distance)
                           .Real("x", from.x).Real("y", from.y).Real("z", from.z)
                           .Num("entrance", int64(route.entranceIdx)).Num("count", reenter.reentries)
                           .Bool("countedAsDeath", true).Bool("respawnZone", respawnZone));
        return true;
    }

    void ShortcutReenter(LeaderGroup& group, std::vector<Player*> const& movers, size_t entrance, char const* why,
                         float walk, float entranceWalk, std::string const& goal)
    {
        GroupRouteState& route = group.Route();
        Position const& at = route.entrances[entrance];
        std::string names;
        for (Player* bot : movers)
        {
            // [b31_shortcut_not_death] user 2026-10-25: a shortcut re-entry is a route choice, NOT a death (only the
            // stranded no-path re-entry is); it stays visible in the shortcut_reenter event below.
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            if (!bot->TeleportTo(bot->GetMapId(), at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(),
                                 at.GetOrientation()))
                continue;
            names += (names.empty() ? "" : ",") + bot->GetName();
            if (bot->GetGUID() == group.LeaderGuid())
                group.ClearWaypoint(group.LeaderGuid());
        }

        uint32 const now = getMSTime();
        ++route.shortcutReentries;
        route.shortcutCooldownUntilMs = now + SHORTCUT_REENTER_COOLDOWN_MS;
        group.NoteProgress(now);
        LOG_INFO("playerbots",
                 "[shortcut_reenter] {}: {} re-enter at entrance {} ({}): walk to {} {:.0f} yd from here, {:.0f} yd "
                 "from the entrance - a route choice, not a death",
                 group.Run().runId, names, entrance, why, goal, walk, entranceWalk);
        group.LogEvent("shortcut_reenter",
                       JsonObject().Str("members", names).Str("why", why).Str("goal", goal).Real("walk", walk)
                           .Real("entranceWalk", entranceWalk).Num("entrance", int64(entrance))
                           .Real("x", at.GetPositionX()).Real("y", at.GetPositionY()).Real("z", at.GetPositionZ())
                           .Bool("countedAsDeath", false).Bool("harnessMove", false));
    }

    bool TickShortcutReenter(LeaderGroup& group, std::vector<Player*> const& players, Player* leader)
    {
        GroupRouteState& route = group.Route();
        // [guild70] a shortcut re-entry is a teleport out and in (the test's LFG port emulated): a real group walks
        if (!group.TestTeleports())
            return false;
        uint32 now = getMSTime();
        now = now ? now : 1;
        Map* map = leader ? leader->GetMap() : nullptr;
        if (!map || !map->IsDungeon() || route.entrances.empty() || group.IsRaid() || route.linkIdx >= 0 ||
            !route.escort.IsEmpty() || route.wingWalk || route.approach || route.guaranteeHolding ||
            int32(now - route.shortcutCooldownUntilMs) < 0 ||
            (route.shortcutCheckMs && getMSTimeDiff(route.shortcutCheckMs, now) < SHORTCUT_REENTER_CHECK_MS))
            return false;
        // [b35_up] never out of the instance while Skadi's gauntlet runs: leaving the hall resets it
        if (SkadiGauntletRunning(leader))
            return false;
        // [b49_hos] never out of the instance while an encounter runs (a boss slot IN_PROGRESS): an event fought by
        // an NPC goes on without the group (hos-h build 45 / 47: Brann's Tribunal of Ages went on while the group
        // re-entered 535 yd away; both "Tribunal wipes" were this, not the add DPS).
        if (InstanceScript* script = leader->GetInstanceScript(); script && script->IsEncounterInProgress())
        {
            if (!route.shortcutEncounterLogMs ||
                getMSTimeDiff(route.shortcutEncounterLogMs, now) >= 30 * IN_MILLISECONDS)
            {
                route.shortcutEncounterLogMs = now;
                LOG_INFO("playerbots", "[b49_hos] {}: no shortcut re-entry, an encounter is in progress",
                         leader->GetName());
            }
            return false;
        }
        route.shortcutCheckMs = now;

        auto const canReenter = [](Player* bot)
        {
            return bot->IsAlive() && !bot->IsInCombat() && !bot->IsBeingTeleported() && !bot->GetTransport() &&
                   !bot->GetVehicle() && !bot->IsFlying();
        };
        for (Player* bot : players)
            if (bot->GetMap() == map && (bot->IsInCombat() || bot->GetTransport() || bot->GetVehicle()))
                return false;  // nobody fights, rides a lift or a vehicle
        if (!canReenter(leader))
            return false;

        // The entrance with the shortest complete walk from it to (x, y, z), as `walker` walks; -1 for none.
        auto const bestEntrance = [&](Player* walker, float x, float y, float z, float& length) -> int32
        {
            int32 best = -1;
            for (size_t i = 0; i < route.entrances.size(); ++i)
            {
                Position const& entrance = route.entrances[i];
                if (entrance.GetExactDist(x, y, z) >= length)
                    continue;  // even the straight line is no shortcut
                G3D::Vector3 const from(entrance.GetPositionX(), entrance.GetPositionY(), entrance.GetPositionZ());
                PathResult const walk = RoutePath(walker, x, y, z, &from);
                // [b32_sh_ooze] a walk from the entrance through a respawn zone (refilled behind the group) costs
                // RESPAWN_ZONE_WALK_COST more: the re-entered side meets its mobs again
                float const walkCost = walk.length + (RespawnZoneOnWalk(group.InstanceId(), walk.points)
                                                          ? RESPAWN_ZONE_WALK_COST
                                                          : 0.0f);
                if (walk.complete && (best < 0 || walkCost < length))
                {
                    best = int32(i);
                    length = walkCost;
                }
            }
            return best;
        };
        auto const worth = [](float walk, float entranceWalk)
        {
            return walk > SHORTCUT_REENTER_RATIO * entranceWalk && walk - entranceWalk > SHORTCUT_REENTER_MIN_GAIN;
        };

        // The group's route: everyone in here is with the leader, the goal its target.
        WorldObject* goal = route.target.IsEmpty() ? nullptr : ObjectAccessor::GetWorldObject(*leader, route.target);
        std::vector<Player*> movers;
        bool together = true;
        for (Player* bot : players)
        {
            if (bot->GetMap() != map || !bot->IsAlive())
                continue;
            if (bot != leader && bot->GetExactDist(leader) > MEET_RANGE)
                together = false;
            movers.push_back(bot);
        }
        if (goal && goal->GetMap() == map && together)
        {
            PathResult const here = RoutePath(leader, goal->GetPositionX(), goal->GetPositionY(), goal->GetPositionZ());
            if (here.complete && here.length > SHORTCUT_REENTER_MIN_GAIN)
            {
                float entranceWalk = here.length / SHORTCUT_REENTER_RATIO;
                int32 const entrance = bestEntrance(leader, goal->GetPositionX(), goal->GetPositionY(),
                                                    goal->GetPositionZ(), entranceWalk);
                if (entrance >= 0 && worth(here.length, entranceWalk) &&
                    std::all_of(movers.begin(), movers.end(), canReenter))
                {
                    ShortcutReenter(group, movers, size_t(entrance), "the group's route", here.length, entranceWalk,
                                    goal->GetName());
                    return true;
                }
            }
            return false;
        }

        // A single member far behind the group: its walk to the leader against the walk from an entrance.
        for (Player* bot : players)
        {
            if (bot == leader || bot->GetMap() != map || !canReenter(bot) ||
                bot->GetExactDist(leader) <= SHORTCUT_REENTER_MEMBER_FAR)
                continue;
            PathResult const here =
                RoutePath(bot, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
            if (!here.complete || here.length <= SHORTCUT_REENTER_MIN_GAIN)
                continue;  // no walk at all is StrandedReenter's (a death)
            float entranceWalk = here.length / SHORTCUT_REENTER_RATIO;
            int32 const entrance =
                bestEntrance(bot, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(), entranceWalk);
            if (entrance >= 0 && worth(here.length, entranceWalk))
            {
                ShortcutReenter(group, {bot}, size_t(entrance), "a member far behind", here.length, entranceWalk,
                                leader->GetName());
                return true;
            }
        }
        return false;
    }

    void ReturnInside(LeaderGroup& group, Player* bot)
    {
        GroupRouteState& route = group.Route();
        ObjectGuid const guid = bot->GetGUID();
        bool const inside = bot->GetMapId() == group.MapId();
        bool const ghost = !bot->IsAlive() && bot->HasPlayerFlag(PLAYER_FLAGS_GHOST);
        bool const hasGoal = group.MemberHasPortalGoal(guid);
        // [guild70] a walked re-entry (StrandedReenter in a real group): its goal holds while it walks out, and ends
        // once it is back inside after being outside
        if (auto walk = route.reenter.find(guid); walk != route.reenter.end() && walk->second.walking)
        {
            if (bot->IsBeingTeleported())
                return;
            if (!inside)
            {
                walk->second.seenOutside = true;
                return;
            }
            if (!walk->second.seenOutside && bot->IsAlive())
                return;  // still walking out through the exit
            walk->second.walking = false;
            walk->second.seenOutside = false;
            SelfTravel::ClearPortalGoal(guid);
            group.SetMemberHasPortalGoal(guid, false);
            uint32 const now = getMSTime();
            group.NoteReentered(guid, false, now ? now : 1);
            group.LogEvent("stranded_reenter",
                           JsonObject().Str("name", bot->GetName()).Str("state", "walked back in"));
            return;
        }
        if (inside || bot->IsBeingTeleported() || bot->IsInCombat() || (!ghost && !bot->IsAlive()))
        {
            if (inside && hasGoal && !route.wingWalk && !bot->IsBeingTeleported())
            {
                SelfTravel::ClearPortalGoal(guid);
                group.SetMemberHasPortalGoal(guid, false);
            }
            return;
        }
        if (ghost || hasGoal || route.wingWalk || route.entranceIdx >= route.entrances.size())
            return;
        // Back in next to the group (Gundrak heroic 2026-10-25: a revived shaman went in through the run's south
        // entrance, 1002 yd of walk from the group at the north one, and fell off its ledge): the entrance whose
        // arrival is nearest the leader inside (FindPortal: the arrival nearest the goal).
        Player* const inLeader = ObjectAccessor::FindPlayer(group.LeaderGuid());
        bool const leaderIn =
            inLeader && inLeader != bot && inLeader->IsAlive() && inLeader->GetMapId() == group.MapId();
        Position const at = leaderIn ? inLeader->GetPosition() : route.entrances[route.entranceIdx];
        SelfTravel::PortalGoal goal;
        goal.mapId = group.MapId();
        goal.x = at.GetPositionX();
        goal.y = at.GetPositionY();
        goal.z = at.GetPositionZ();
        SelfTravel::SetPortalGoal(guid, goal);
        group.SetMemberHasPortalGoal(guid, true);
    }

    bool TickWingWalk(LeaderGroup& group, std::vector<Player*> const& players, Player* leader)
    {
        GroupRouteState& route = group.Route();
        if (!route.wingWalk || route.entranceIdx >= route.entrances.size())
        {
            route.wingWalk = false;
            return false;
        }

        uint32 const now = getMSTime();
        Position const& entrance = route.entrances[route.entranceIdx];
        bool everybody = true;
        for (Player* bot : players)
        {
            ObjectGuid const memberGuid = bot->GetGUID();
            if (!group.MemberHasPortalGoal(memberGuid))
                continue;
            MemberWingWalk& wing = route.wingWalkers[memberGuid];
            if (bot->IsAlive() && bot->GetMapId() == group.MapId() && !bot->IsBeingTeleported() &&
                bot->GetExactDist2d(&entrance) <= WING_ARRIVE_RANGE)
            {
                SelfTravel::ClearPortalGoal(memberGuid);
                group.SetMemberHasPortalGoal(memberGuid, false);
                continue;
            }
            // Inside and near one spot for WING_MEMBER_STILL_MS: no walk out (Gundrak heroic 2026-10-25, Rtshabe on a
            // 19-polygon island of the moat floor, 387 s "no walk to the portal"): left out of the walk, logged.
            if (bot->IsAlive() && bot->GetMapId() == group.MapId())
            {
                if (!wing.stillMs || int32(wing.stillMs - route.wingWalkSinceMs) < 0 ||
                    bot->GetExactDist(&wing.stillPos) > WING_MEMBER_STILL_RANGE)
                {
                    wing.stillMs = now ? now : 1;
                    wing.stillPos = bot->GetPosition();
                }
                else if (getMSTimeDiff(wing.stillMs, now) > WING_MEMBER_STILL_MS)
                {
                    SelfTravel::ClearPortalGoal(memberGuid);
                    group.SetMemberHasPortalGoal(memberGuid, false);
                    wing.stillMs = 0;
                    group.LogCannotMove(bot, "wing change", "no walk out for 2 minutes: left out of the wing walk",
                                        entrance.GetPositionX(), entrance.GetPositionY(), entrance.GetPositionZ());
                    continue;
                }
            }
            everybody = false;
            if (bot->isMoving())
                group.NoteProgress(now);
        }

        if (everybody)
        {
            route.wingWalk = false;
            route.nextRetargetMs = now;
            group.LogEvent("wing_walk_done",
                           JsonObject().Num("entrance", int64(route.entranceIdx))
                               .Num("durationSec", getMSTimeDiff(route.wingWalkSinceMs, now) / IN_MILLISECONDS));
            return false;
        }

        if (getMSTimeDiff(route.wingWalkSinceMs, now) > WING_WALK_MAX_MS)
        {
            for (Player* bot : players)
                if (group.MemberHasPortalGoal(bot->GetGUID()))
                    group.LogCannotMove(bot, "wing change", "did not reach the next entrance in 15 minutes",
                                        entrance.GetPositionX(), entrance.GetPositionY(), entrance.GetPositionZ());
            group.FinishRun("failed", "wing change walk did not finish in 15 minutes");
            return true;
        }

        // The members walk on their own (out through the exit, in through the entrance); no route meanwhile.
        if (leader)
            group.ClearWaypoint(group.LeaderGuid());
        return true;
    }
}
