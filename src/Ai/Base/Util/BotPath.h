/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTPATH_H
#define PLAYERBOTS_BOTPATH_H

// [lead109] user: one path search for every player bot (solo or grouped, in or out of a test): its path decisions
// and its walks take the leader's route (Leader::Route, the long search) - the leader and its members cannot judge
// a walk differently any more (b142 rfc-h 381-451 s: the leader's route up to the Earthborer ledge was complete, the
// tank's PathGenerator path a straight line through the air). Creatures (pets, mobs) keep the core's PathGenerator.

#include "Define.h"
#include "MoveSplineInitArgs.h"
#include "PathGenerator.h"

class Player;
class Unit;

#include <memory>

struct BotPath
{
    Movement::PointsArray points;
    float length = 0.0f;
    PathType type = PATHFIND_BLANK;  // the leader's route: NORMAL (complete), INCOMPLETE (partial), NOPATH (none)
    G3D::Vector3 actualEnd;          // where the walk ends
    G3D::Vector3 end;                // where it was asked to end
    bool leaderRoute = false;
    bool valid = false;              // PathGenerator::CalculatePath's answer: both ends are valid map coordinates
    Unit* owner = nullptr;                       // the walker, for this call only
    std::shared_ptr<PathGenerator> generator;  // a creature's path (PathGenerator's own)

    // PathGenerator::ShortenPathUntilDist: the walk ends `dist` from `target` (the last point still in sight of it)
    void ShortenPathUntilDist(G3D::Vector3 const& target, float dist);
};

// a player bot walking on the ground (no flight, swim, vehicle or transport): its paths are the leader's route
bool UsesLeaderRoute(Unit* owner);
// the path `owner` walks to (x, y, z) (from where it stands, or from `from`): the leader's route for a player bot
// (UsesLeaderRoute), PathGenerator's path for anything else
BotPath FindBotPath(Unit* owner, float x, float y, float z);
BotPath FindBotPath(Unit* owner, float fromX, float fromY, float fromZ, float x, float y, float z);

// the bot walks `points` (a leader's route, its first point where it stands) as one spline path; false: fewer than
// two points (no walk)
bool WalkBotPoints(Player* bot, Movement::PointsArray points);
// a player bot (UsesLeaderRoute) walks the leader's route to (x, y, z); false: no route walker or no route found
// (the caller's own core move then)
bool WalkBotRoute(Player* bot, float x, float y, float z);

#endif
