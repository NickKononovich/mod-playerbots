/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_ROUTEWATER_H
#define PLAYERBOTS_LEADER_ROUTEWATER_H

// [lead107] user: production code does not live in the test harness. The leader's water rule (LeaveWater: the
// breath estimate, the surfacing, the rest on dry ground) and the facts it reads, moved out of TestHarnessRunner.cpp
// unchanged; the harness calls it through the group view.

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
#include "RouteMeet.h"

namespace Leader::Travel
{
    constexpr int32 BREATH_MAX_MS = 3 * MINUTE * IN_MILLISECONDS;  // Player::getMaxTimer(BREATH_TIMER)

    constexpr int32 BREATH_REFILL_RATE = 10;                         // Player::HandleDrowning above water

    constexpr int32 BREATH_SURFACE_MS = BREATH_MAX_MS * 40 / 100;    // surface below 40 %

    constexpr int32 BREATH_RESUME_MS = BREATH_MAX_MS * 90 / 100;     // and dive again from 90 %

    constexpr uint32 SURFACE_HOLD_MS = 2 * IN_MILLISECONDS;          // bot AI paused while it swims up

    void LeaveWater(LeaderGroup& group, std::vector<Player*> const& players, bool resting);
}

#endif
