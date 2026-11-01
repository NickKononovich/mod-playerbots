/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestRunnerStrategy.h"

#include "LeaderKnowledge.h"  // [lead107 final] the door-cut registry

#include "Map.h"
#include "MoveConflict.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SelfTravelActions.h"
#include "Timer.h"

#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr float WAYPOINT_REACHED_DISTANCE = 2.5f;
    // A waypoint the own path does not bring the bot nearer to for this long: the straight walk instead.
    constexpr uint32 NO_PROGRESS_MS = 3 * IN_MILLISECONDS;
    constexpr float PROGRESS_GAIN = 1.0f;
    // The harness sets the leader's waypoint anew every tick (30 yd ahead on its route): the same one unless moved.
    constexpr float WAYPOINT_SAME_RANGE = 2.0f;
    // Straight walks only over a short gap (the harness gap walk took up to 45 yd).
    constexpr float STRAIGHT_MAX_DISTANCE = 45.0f;
    // [gd-link-straight] A route link's line (Waypoint::straight) is walked straight over its whole length: Gundrak's
    // bridge to Gal'darah is 52.5 yd, from its start the far end was beyond STRAIGHT_MAX_DISTANCE, the own path went
    // round on the navmesh and the leader fell into the moat at 1764.6,754.9,108.8 (gd-h 2026-10-25 062726).
    constexpr float LINK_STRAIGHT_MAX_DISTANCE = 80.0f;
    // A waypoint with no floor this close under it (a height interpolated between two stair corners, Sunken Temple
    // normal 2026-10-25: -370.3,53.7 at z -137.2 under the steps at -129): the navmesh polygon nearest to it, this far
    // up or down, gives its height.
    constexpr float WAYPOINT_FLOOR_SEARCH_UP = 3.0f;
    constexpr float WAYPOINT_FLOOR_MAX_DZ = 2.0f;
    constexpr float WAYPOINT_SNAP_HALF_HEIGHT = 12.0f;

    std::mutex waypointLock;
    std::unordered_map<ObjectGuid, TestRunner::Waypoint> waypoints;
}

namespace TestRunner
{
    void SetWaypoint(ObjectGuid bot, Waypoint const& waypoint)
    {
        std::lock_guard<std::mutex> lock(waypointLock);
        waypoints[bot] = waypoint;
    }

    void ClearWaypoint(ObjectGuid bot)
    {
        std::lock_guard<std::mutex> lock(waypointLock);
        waypoints.erase(bot);
    }

    bool GetWaypoint(ObjectGuid bot, Waypoint& waypoint)
    {
        std::lock_guard<std::mutex> lock(waypointLock);
        auto itr = waypoints.find(bot);
        if (itr == waypoints.end())
            return false;

        waypoint = itr->second;
        return true;
    }

    void ClearAll()
    {
        std::lock_guard<std::mutex> lock(waypointLock);
        waypoints.clear();
        Leader::Knowledge::ClearDoorCuts();  // [b32_strat_gate] [lead107 final] the leader's registry
    }
}

void TestRunnerStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Below eating, drinking and buffing (the harness clears the waypoint while the group recovers), above the
    // "aggressive target" pull at 4: the harness drops the waypoint once the target is within pull range.
    triggers.push_back(new TriggerNode("test runner waypoint", { NextAction("test runner move", ACTION_NORMAL + 1) }));
}

bool TestRunnerWaypointTrigger::IsActive()
{
    if (bot->IsInCombat() || !bot->IsAlive())
        return false;

    TestRunner::Waypoint waypoint;
    if (!TestRunner::GetWaypoint(bot->GetGUID(), waypoint) || waypoint.mapId != bot->GetMapId())
        return false;

    // [escort_anchor] an escort under way leads; the route waits (MoveConflict::EscortYieldsWaypoint).
    if (MoveConflict::EscortYieldsWaypoint(botAI, waypoint.x, waypoint.y, waypoint.z))
        return false;

    // [b33_harn] a dungeon event hold (MoveConflict::NoteEventHold): the walk off its spot waits.
    if (MoveConflict::EventHoldYieldsWaypoint(bot, waypoint.x, waypoint.y, waypoint.z))
        return false;

    return bot->GetExactDist(waypoint.x, waypoint.y, waypoint.z) > WAYPOINT_REACHED_DISTANCE;
}

// [b32_strat_gate] the walk through an auto-closing key gate in its 3 s: not shortened by the idle-mob guard.
bool TestRunnerMoveAction::SkipsIdleGuard()
{
    TestRunner::Waypoint waypoint;
    return TestRunner::GetWaypoint(bot->GetGUID(), waypoint) && waypoint.gatePass;
}

bool TestRunnerMoveAction::isUseful()
{
    if (bot->IsInCombat() || !bot->IsAlive() || bot->IsBeingTeleported())
        return false;

    TestRunner::Waypoint waypoint;
    return TestRunner::GetWaypoint(bot->GetGUID(), waypoint) && waypoint.mapId == bot->GetMapId() &&
           !MoveConflict::EscortYieldsWaypoint(botAI, waypoint.x, waypoint.y, waypoint.z) &&
           !MoveConflict::EventHoldYieldsWaypoint(bot, waypoint.x, waypoint.y, waypoint.z);  // [b33_harn]
}

bool TestRunnerMoveAction::Execute(Event /*event*/)
{
    TestRunner::Waypoint waypoint;
    if (!TestRunner::GetWaypoint(bot->GetGUID(), waypoint) || waypoint.mapId != bot->GetMapId())
        return false;

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    // A jump link: once, from where the harness gave it (the link's start); the jump itself is the move.
    if (waypoint.jump)
    {
        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE ||
            bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING))
            return true;
        if (_jumpSerial == waypoint.serial)
            return false;
        // Retried until it starts (the walk to the start may still be ending: IsWaitingForLastMove).
        if (!JumpTo(waypoint.mapId, waypoint.x, waypoint.y, waypoint.z, MovementPriority::MOVEMENT_NORMAL))
            return false;
        _jumpSerial = waypoint.serial;
        return true;
    }

    // The progress check keeps the waypoint as given; the walk goes to its floor.
    float const givenZ = waypoint.z;
    Map* map = bot->GetMap();
    if (map && !map->IsInWater(bot->GetPhaseMask(), waypoint.x, waypoint.y, waypoint.z, bot->GetCollisionHeight()))
    {
        float const floor = map->GetHeight(bot->GetPhaseMask(), waypoint.x, waypoint.y,
                                           waypoint.z + WAYPOINT_FLOOR_SEARCH_UP, true,
                                           2.0f * WAYPOINT_FLOOR_SEARCH_UP);
        float snapped = 0.0f;
        if ((floor <= INVALID_HEIGHT || std::fabs(floor - waypoint.z) > WAYPOINT_FLOOR_MAX_DZ) &&
            SelfTravel::NavmeshFloor(bot, G3D::Vector3(waypoint.x, waypoint.y, waypoint.z),
                                     WAYPOINT_SNAP_HALF_HEIGHT, snapped))
            waypoint.z = snapped;
    }

    uint32 const now = getMSTime();
    float const distance = bot->GetExactDist(waypoint.x, waypoint.y, waypoint.z);
    float const moved = std::hypot(waypoint.x - _waypointX, waypoint.y - _waypointY);
    if (!_progressMs || moved > WAYPOINT_SAME_RANGE || std::fabs(givenZ - _waypointZ) > WAYPOINT_SAME_RANGE ||
        distance + PROGRESS_GAIN < _bestDistance)
    {
        _waypointX = waypoint.x;
        _waypointY = waypoint.y;
        _waypointZ = givenZ;
        _progressMs = now;
        _bestDistance = distance;
    }
    // [b34_ss] A gate pass (Waypoint::gatePass) waits at the shut gate by design: its "no progress" is the gate, and
    // skipping the own path for it threw away the 3 s the gate stands open (strat-n 20261026-014521-5C-2: "own path
    // makes no progress" at 2099 s, the healer 0.7 yd from King's Square Gate 175353 never went through).
    bool const noProgress =
        !waypoint.gatePass && getMSTimeDiff(_progressMs, now) > NO_PROGRESS_MS && !bot->isMoving();

    // A route link's far end (Waypoint::straight): the line itself first, as a player walks the ramp; the own path is
    // only the fallback (it goes round on the navmesh, to the polygon below the far end).
    if (waypoint.straight && distance <= LINK_STRAIGHT_MAX_DISTANCE)
    {
        // [w24b-exact-line] the line as given: a MovePoint is pathed again and ended in Gundrak's moat.
        if (MoveStraight(waypoint.mapId, waypoint.x, waypoint.y, waypoint.z, true, MovementPriority::MOVEMENT_NORMAL,
                         true))
        {
            if (!_straightLogMs || getMSTimeDiff(_straightLogMs, now) > 10 * IN_MILLISECONDS)
            {
                _straightLogMs = now ? now : 1;
                LOG_INFO("playerbots",
                         "[test runner] [w24b-exact-line] {}: link_line_walk: exact line over the link to "
                         "({:.1f},{:.1f},{:.1f}), {:.1f} yd",
                         bot->GetName(), waypoint.x, waypoint.y, waypoint.z, distance);
            }
            _progressMs = now;
            return true;
        }
        // Walking that line already: no own path meanwhile.
        if (bot->isMoving() && IsDuplicateMove(waypoint.x, waypoint.y, waypoint.z))
            return false;
    }

    // [obstacle_sidestep] a solid game object on the path (the navmesh has none: cos-h's Hallow's End pumpkins): a
    // step round it, then the own path again.
    if (SidestepObstacle(waypoint.mapId, waypoint.x, waypoint.y, waypoint.z))
    {
        _progressMs = now;
        return true;
    }

    if (!noProgress && MoveTo(waypoint.mapId, waypoint.x, waypoint.y, waypoint.z, false, false, false, false,
                              MovementPriority::MOVEMENT_NORMAL))
        return true;
    // Still on the way, or the last move is not over yet.
    if (bot->isMoving() || !IsMovingAllowed() || IsDuplicateMove(waypoint.x, waypoint.y, waypoint.z) ||
        IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return false;

    // The own path refuses the waypoint or ends short of it: straight on, as a player walks a ramp or a bridge.
    if (distance <= STRAIGHT_MAX_DISTANCE && MoveStraight(waypoint.mapId, waypoint.x, waypoint.y, waypoint.z))
    {
        _progressMs = now;
        return true;
    }

    // [b34_mech3] Stranded on a navmesh island (SelfTravel::IslandStepDown: a pipe or rail the straight walk climbed
    // onto, nothing joins it): the step down to the floor beside it, as a player takes it, then the own path again.
    // mech-n 20261026-014521-mech-n-5D-17: 230 s "no straight walk over ground" on a pipe 4 yd over the floor.
    {
        constexpr uint32 STEP_DOWN_RETRY_MS = 5 * IN_MILLISECONDS;
        thread_local std::unordered_map<ObjectGuid, uint32> stepDownTriedMs;  // per bot, its map thread
        uint32& triedMs = stepDownTriedMs[bot->GetGUID()];
        if (!triedMs || getMSTimeDiff(triedMs, now) >= STEP_DOWN_RETRY_MS)
        {
            triedMs = now ? now : 1;
            G3D::Vector3 landing;
            if (SelfTravel::IslandStepDown(bot, landing) &&
                JumpTo(waypoint.mapId, landing.x, landing.y, landing.z, MovementPriority::MOVEMENT_NORMAL))
            {
                _progressMs = now;
                return true;
            }
        }
    }

    SelfTravel::ReportCannotMove(bot, "test runner move",
                                 distance > STRAIGHT_MAX_DISTANCE
                                     ? (noProgress ? "own path makes no progress, too far for a straight walk"
                                                   : "own path refused, too far for a straight walk")
                                     : (noProgress ? "own path makes no progress, no straight walk over ground"
                                                   : "own path refused, no straight walk over ground"),
                                 waypoint.x, waypoint.y, waypoint.z);
    _progressMs = now;  // the own path is tried again before the next report
    return false;
}
