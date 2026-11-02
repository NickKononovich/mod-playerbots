/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_ROUTEREENTER_H
#define PLAYERBOTS_LEADER_ROUTEREENTER_H

// [lead341] user: «Стенд никого не двигает»: the re-entries (WotLK's LFG teleport out / in) are the leader's
// decisions and the leader's acts, moved out of TestHarnessRunner.cpp unchanged - the stranded member's re-entry
// (a death) and the shortcut re-entry (a route choice).

#include "LeaderGroup.h"
#include "MoveSplineInitArgs.h"

#include <string>
#include <vector>

class Player;

namespace Leader::Travel
{
    // MemberStranded: the navmesh check per far member at most this often; a member within this range of where its
    // no-walk watch started counts as not moving
    constexpr uint32 STRANDED_RECHECK_MS = 10 * IN_MILLISECONDS;
    constexpr float STRANDED_STILL_RANGE = 20.0f;
    // [b30_topped_up] StrandedReenter (user rule 2026-10-25: «если нет пути обратно, то можно перезайти в данж. Но это
    // будет считаться как смерть»): no walk back to the group for STRANDED_REENTER_MS within the still range, the
    // follow watchdog stuck at least STRANDED_REENTER_WATCHDOG_S meanwhile; at most STRANDED_REENTER_MAX per member
    // and run (each one is a death).
    constexpr uint32 STRANDED_REENTER_MS = 75 * IN_MILLISECONDS;
    constexpr uint32 STRANDED_REENTER_WATCHDOG_S = 20;
    constexpr uint32 STRANDED_REENTER_MAX = 3;
    // [b30_reenter] User rule 2026-10-25: «еще можно перезайти в данж, когда это реально ближе идти будет» - a
    // re-entry as a route choice: the walk to the goal here longer than SHORTCUT_REENTER_RATIO x the walk from an
    // entrance and by more than SHORTCUT_REENTER_MIN_GAIN (RouteTargets.h).
    constexpr float SHORTCUT_REENTER_RATIO = 2.0f;
    constexpr uint32 SHORTCUT_REENTER_CHECK_MS = 20 * IN_MILLISECONDS;
    constexpr uint32 SHORTCUT_REENTER_COOLDOWN_MS = 3 * MINUTE * IN_MILLISECONDS;
    constexpr float SHORTCUT_REENTER_MEMBER_FAR = 100.0f;  // a member this far from the leader re-enters alone
    constexpr float RESPAWN_ZONE_RADIUS = 30.0f;            // a walk this close to a zone point crosses the zone
    constexpr float RESPAWN_ZONE_WALK_COST = 150.0f;        // yd added to an entrance walk through a zone

    // The walk `points` passes within RESPAWN_ZONE_RADIUS of a respawn zone point of the instance.
    bool RespawnZoneOnWalk(uint32 instanceId, Movement::PointsArray const& points);

    // [b30_topped_up] A member with no walk back to the group leaves the instance and comes back in at its entrance,
    // counted as a death; true when it re-entered now.
    bool StrandedReenter(LeaderGroup& group, ObjectGuid memberGuid, Player* bot, Player* leader);

    // [b30_reenter] The movers out of the instance and in again at `entrance` (WotLK's LFG teleport out / in): not a
    // death ([b31_shortcut_not_death]). The goal stays the route's; only the way there changes.
    void ShortcutReenter(LeaderGroup& group, std::vector<Player*> const& movers, size_t entrance, char const* why,
                         float walk, float entranceWalk, std::string const& goal);

    // [b30_reenter] The group's route (everyone with the leader, the goal its target) or a single member far behind
    // is walked from an entrance when that is the much shorter walk; true when the group re-entered now.
    bool TickShortcutReenter(LeaderGroup& group, std::vector<Player*> const& players, Player* leader);

    // [lead342] a living member outside the instance is told to come back in (its own "instance portal" walks it):
    // the portal goal is the leader inside, else the run's entrance; the goal ends once it is inside again
    void ReturnInside(LeaderGroup& group, Player* bot);

    // [lead342] a walked wing change runs (the members walk out through the exit and in through the next entrance):
    // over once every member stands within WING_ARRIVE_RANGE of the entrance; a member inside near one spot for
    // WING_MEMBER_STILL_MS is left out of it (no walk out); the run fails after WING_WALK_MAX_MS. True while it runs.
    constexpr uint32 WING_WALK_MAX_MS = 15 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 WING_MEMBER_STILL_MS = 2 * MINUTE * IN_MILLISECONDS;
    constexpr float WING_MEMBER_STILL_RANGE = 25.0f;
    bool TickWingWalk(LeaderGroup& group, std::vector<Player*> const& players, Player* leader);
}

#endif
