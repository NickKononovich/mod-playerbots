/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_ROUTEMEET_H
#define PLAYERBOTS_LEADER_ROUTEMEET_H

// [lead107] user: production code does not live in the test harness. The leader's meet (MeetMember: the walk back to
// a member left behind, the regroup after a revive, the stragglers) and the facts it reads, moved out of
// TestHarnessRunner.cpp unchanged; the harness calls it through the group view.

#include "Creature.h"
#include "G3D/Vector3.h"
#include "G3D/Vector4.h"
#include "GameObject.h"
#include "MoveSplineInitArgs.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Position.h"
#include "LeaderKnowledge.h"
#include "LeaderGroup.h"
#include "LeaderKnowledge.h"
#include "RouteEvents.h"
#include "RouteLegs.h"
#include "RoutePatrol.h"
#include "RouteSearch.h"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "RouteTargets.h"
#include "RouteSteer.h"

namespace Leader::Travel
{
    // Regroup after a revive (ROUTE_REGROUP, 2026-10-24, *.bak_routebackbone). RFC 18:34-18:35: the tank died,
    // came back in through the portal and stood at the entrance; MeetMember walked it back towards the group
    // 83-85 yd deeper, the four others followed it (follow) into a cave wall, wait_timeout after 60 s, a new
    // target. User rule: the group stands. The returning side (who stands at the entrance, REGROUP_ENTRANCE_RANGE)
    // runs to the group when the way is free - no idle hostile whose route reach (RouteReach) covers the walk
    // where it sees it (BlocksRoute), and no killed mob with its spawn point that close respawning before the walk
    // is over (its length at run speed plus REGROUP_RESPAWN_MARGIN_SEC: "close to the respawn the group comes
    // anyway") - and the group holds there ("stay"); else the group walks out through the exit and in through
    // the entrance (the portal goal, its own "instance portal", as a wing change: TickWingWalk watches) and is
    // at the entrance with them. The decision is the returning side's signal, taken once when the situation
    // starts and again when its walk makes no progress for REGROUP_STUCK_MS (meet_stuck).
    constexpr bool ROUTE_REGROUP = true;

    constexpr size_t MEET_PASS_PACK_MAX = 2;  // the walk back goes on into at most this many idle mobs

    // MeetMember: a living member this far behind the leader out of combat (the far-wait threshold is 50 yd).
    constexpr float MEET_RANGE = 45.0f;

    // [meet_hysteresis] STN92_EFFICIENCY 2026-10-25 (lagging hunter, "[regroup] ping-pong meet <-> route",
    // meet_stuck x2, "gives way ... far 276 s"): a meet started past MEET_RANGE ended the moment the member was
    // back inside it - at the edge, so the route's next step put it past the far line again and the leader walked
    // back again. The member being met stays met until it is within MEET_END_RANGE (one gather: the leader walks
    // toward it, its own follow toward the leader); TickParty's far wait uses the same line
    // (REGROUP_MEET_END_RANGE, TestHarnessMgr.cpp).
    // [meet_end_follow_trail] 30 yd (was 20): a following bot trails a walking leader 13-31 yd on ramps, so 20 yd
    // flipped the far wait on every step (wc-h wave 23: "hold meet" 15 flips in 109 s, 19 in 159 s, 22 in 292 s).
    // [b31_far_threshold] 15 yd (was 30): the meet ends with the member close, the group goes on together; the far
    // wait counts it far down to the same line (FAR_RESUME_RANGE, TestHarnessMgr.cpp)
    constexpr float MEET_END_RANGE = 15.0f;

    constexpr float WING_ARRIVE_RANGE = 40.0f;

    constexpr float REGROUP_GROUP_RADIUS = 30.0f;

    constexpr uint32 REGROUP_STUCK_MS = 10 * IN_MILLISECONDS;

    constexpr float REGROUP_PROGRESS = 3.0f;

    constexpr float REGROUP_RESPAWN_MARGIN_SEC = 30.0f;

    constexpr uint32 REGROUP_MAX_REPLANS = 3;  // then the group comes to the entrance

    constexpr uint32 REGROUP_COOLDOWN_MS = 3 * MINUTE * IN_MILLISECONDS;  // after giving up: the old meeting walk

    // The exit decision needs a complete walk to the entrance for every member of the group (regroup_exit_refused).
    constexpr bool REGROUP_EXIT_NEEDS_WALK = true;

    // Gruul 2026-10-24, false restores the old side test (anybody within WING_ARRIVE_RANGE of the entrance): the
    // returning side is a member that came back in through the portal (Member::backInMs) at most this long ago.
    constexpr bool REGROUP_BACK_IN_ONLY = true;

    constexpr uint32 REGROUP_BACK_IN_MS = 3 * MINUTE * IN_MILLISECONDS;

    // A killed mob respawning later than this (Map::SaveCreatureRespawnTime: a respawn past the instance reset
    // is stored as now + YEAR, raid trash "in 31535988 s") does not respawn during the run.
    constexpr float REGROUP_RESPAWN_NEVER_SEC = float(DAY);

    enum class RegroupMode : uint8
    {
        None,
        Run,   // the way is free: the group holds, the returning side runs to it
        Exit,  // the group walks out and in through the entrance
    };

    struct RegroupState
    {
        RegroupMode mode = RegroupMode::None;
        std::vector<ObjectGuid> held;  // members put on "stay"
        ObjectGuid runner;             // whose walk is watched (Run)
        ObjectGuid anchor;             // the group member it walks to
        float bestDistance = 0.0f;
        uint32 progressMs = 0;
        uint32 replans = 0;
    };

    extern std::unordered_map<uint32, RegroupState> regroups;  // per instance, world thread only

    extern std::unordered_map<uint32, uint32> regroupCooldownUntilMs;  // per instance, world thread only

    // MeetMember's own walk (the leader back to a straggler): its progress, for meet_stuck.
    struct MeetProgress
    {
        ObjectGuid member;
        float bestDistance = 0.0f;
        uint32 progressMs = 0;
    };

    extern std::unordered_map<uint32, MeetProgress> meetProgress;  // per instance, world thread only

    // A straggler episode of MeetMember (not a regroup). The member's own follow comes first: the follow guard's
    // deadlock breaker lets a vetoed follower go after 5 s with the tank out of combat and over 30 yd away
    // (MovementActions), so the leader stands MEET_GRACE_MS before it walks back, and again after each
    // meet_stuck; after MEET_MAX_STUCK of them it walks back no more ("give way": the breaker and the far wait's
    // timeout decide). SFK 5B-5 20:29-20:38: walk back / route on for 8 minutes, 30 meet_stuck, run stalled.
    constexpr uint32 MEET_GRACE_MS = 12 * IN_MILLISECONDS;

    constexpr uint32 MEET_MAX_STUCK = 2;

    constexpr uint32 MEET_EPISODE_GAP_MS = 90 * IN_MILLISECONDS;  // not seen this long: a new episode

    struct StragglerEpisode
    {
        ObjectGuid member;
        uint32 sinceMs = 0;
        uint32 lastSeenMs = 0;
        uint32 holdSinceMs = 0;     // the leader stands (grace) since; 0: it walks back
        float bestDistance = 0.0f;  // while it stands: the member closing in by itself restarts the grace
        uint32 stuck = 0;
        bool gaveWay = false;
    };

    extern std::unordered_map<uint32, StragglerEpisode> stragglers;  // per instance, world thread only

    // What lies on the walk: idle hostiles that would aggro on it, and the soonest respawn (s, -1: none) of a
    // killed hostile whose spawn point is within its route reach of the walk.
    struct RegroupPathCheck
    {
        uint32 aliveMobs = 0;
        std::string firstMob;
        float nextRespawnSec = -1.0f;
        std::string nextRespawnMob;
    };

    RegroupPathCheck CheckRegroupPath(Player* runner, Movement::PointsArray const& points);
    // [hleft-bm-tank-fetch] Black Morass while the event runs (rift >= 1, shield > 0): `member` holds at Medivh (within
    // BM_MEDIVH_HOLD_RANGE) while the tank fetches the rift keepers alone (tbc-bm strategy) - no far wait, no regroup
    // and no meet for him.
    bool BlackMorassHoldsAtMedivh(Player* member);

    bool MeetMember(LeaderGroup& group, std::vector<Player*> const& players, Player* leader);
}

#endif
