/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDKITEHELPER_H
#define PLAYERBOTS_RAIDKITEHELPER_H

#include "Define.h"
#include "Position.h"
#include <cstddef>
#include <functional>
#include <vector>

class Player;

// Kiting primitive for raid scripts. It only computes the next step; the calling MovementAction
// issues MoveTo() with it and re-plans every tick, so a chasing threat is handled step by step.
namespace RaidKiteHelper
{

// Returns true when (x, y) must not be used as a kite step (fire, void zone, volcano, spores...).
using HazardCheck = std::function<bool(float x, float y)>;

struct KiteParams
{
    float keepDistance = 20.0f;  // wanted 2D distance (centre to centre) from every threat
    float stepDistance = 5.0f;   // length of one step
    Position leashCenter;        // the step must stay within leashRadius of this point...
    float leashRadius = 0.0f;    // ...unless the radius is 0 (no leash)
    float preferredAngle = 0.0f; // tie-breaker for open-field steps, used when usePreferredAngle
    bool usePreferredAngle = false;
    HazardCheck isHazard;        // optional
};

// Smallest 2D distance from (x, y) to any threat, or a very large value when there is none.
float MinDistanceToThreats(float x, float y, std::vector<Position> const& threats);

bool IsKiteNeeded(Player* bot, std::vector<Position> const& threats, float keepDistance);

// Samples directions around the bot and returns the collision-checked step that ends farthest from
// the threats, inside the leash and outside hazards. Returns false when no step gains distance.
bool FindOpenKiteStep(
    Player* bot, std::vector<Position> const& threats, KiteParams const& params, Position& step);

// The route is an ordered loop of waypoints taken from the encounter tactics. The bot walks toward
// the neighbouring waypoint (forward or backward along the loop) that leaves it farther from the
// threats without passing next to them, skipping hazardous waypoints. Falls back to
// FindOpenKiteStep when no waypoint is usable.
bool FindRouteKiteStep(
    Player* bot, std::vector<Position> const& threats, std::vector<Position> const& route,
    KiteParams const& params, Position& step);

// Index of the next waypoint after 'current' in 'direction' (+1 or -1) that is not a hazard.
// Returns 'current' when every other waypoint is hazardous.
size_t NextSafeRouteIndex(
    std::vector<Position> const& route, size_t current, int direction, HazardCheck const& isHazard);

}

#endif
