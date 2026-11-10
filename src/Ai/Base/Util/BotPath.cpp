/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotPath.h"

#include <cmath>

#include "GridDefines.h"
#include "LeaderOrders.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Player.h"
#include "Playerbots.h"
#include "RouteSearch.h"  // [lead144] NoteHeightGap

bool UsesLeaderRoute(Unit* owner)
{
    Player* bot = owner ? owner->ToPlayer() : nullptr;
    return bot && GET_PLAYERBOT_AI(bot) && bot->IsInWorld() && !bot->IsFlying() && !bot->isSwimming() &&
           !bot->GetVehicle() && !bot->GetTransport();
}

namespace
{
    BotPath FromRoute(Unit* owner, Leader::RouteResult const& route, G3D::Vector3 const& end)
    {
        BotPath out;
        out.owner = owner;
        out.end = end;
        out.leaderRoute = true;
        out.throughRefused = route.found && route.throughRefused;  // [deaths]
        out.refusedArea = !route.found && route.refusedArea;       // [loop70]
        out.type = !route.found ? PATHFIND_NOPATH : route.complete ? PATHFIND_NORMAL : PATHFIND_INCOMPLETE;
        out.points.assign(route.points.begin(), route.points.end());
        out.length = route.length;
        out.valid = true;
        if (!out.points.empty())
            out.actualEnd = out.points.back();
        return out;
    }

    BotPath FromGenerator(Unit* owner, std::shared_ptr<PathGenerator> path, bool valid)
    {
        BotPath out;
        out.valid = valid;
        out.owner = owner;
        out.type = path->GetPathType();
        out.points = path->GetPath();
        out.length = path->getPathLength();
        out.actualEnd = path->GetActualEndPosition();
        out.end = path->GetEndPosition();
        out.generator = std::move(path);
        return out;
    }
}

BotPath FindBotPath(Unit* owner, float x, float y, float z)
{
    return FindBotPath(owner, owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ(), x, y, z);
}

BotPath FindBotPath(Unit* owner, float fromX, float fromY, float fromZ, float x, float y, float z)
{
    bool const route = UsesLeaderRoute(owner);
    Leader::RouteResult found;
    if (route && Acore::IsValidMapCoord(fromX, fromY, fromZ) && Acore::IsValidMapCoord(x, y, z))
    {
        found = Leader::Route(owner, G3D::Vector3(fromX, fromY, fromZ), G3D::Vector3(x, y, z));
        if (found.found)
            return FromRoute(owner, found, G3D::Vector3(x, y, z));
    }
    auto path = std::make_shared<PathGenerator>(owner);
    bool const valid = path->CalculatePath(fromX, fromY, fromZ, x, y, z, false);
    // no route: as the route's own "no navmesh: straight" rule, a stretch without navmesh tiles goes by
    // PathGenerator's shortcut; anything else is the route's "no walk"
    if (!route || (path->GetPathType() & PATHFIND_NOT_USING_PATH) || !valid)
        return FromGenerator(owner, std::move(path), valid);
    // [nav142] the route's own "no walk" keeps why it found none: its refusal of an area red for the walker / an
    // enemy capital / static spawns it must not fight ([zone38], [static39] -> refusedArea). A default result here
    // dropped it, so [loop70]'s refused walk never fired and the caller took the core's unpriced path toward the
    // refused area: b428 Relara L14 and Salleas L14 on q866's 6400 yd giver walk to Ratchet, "[zone38] refuses ...
    // Maestra's Post (area level 19)", then "no route out from here: walks the core's path" leg after leg down the
    // Ashenvale coast into Bathran's Haunt (Forsaken L18-20), "dead end of its route", dead there 5 times
    Leader::RouteResult refused;
    refused.refusedArea = found.refusedArea;
    BotPath none = FromRoute(owner, refused, G3D::Vector3(x, y, z));
    return none;
}

void BotPath::ShortenPathUntilDist(G3D::Vector3 const& target, float dist)
{
    if (generator)
    {
        generator->ShortenPathUntilDist(target, dist);
        points = generator->GetPath();
        return;
    }
    // the leader's route: PathGenerator's rule (the last point still in sight of the target, no slope check: the
    // route's own search judged the slopes)
    if (type == PATHFIND_BLANK || points.size() < 2 || !owner)
        return;
    float const distSq = dist * dist;
    if ((points[0] - target).squaredLength() < distSq || (points.back() - target).squaredLength() >= distSq)
        return;
    std::size_t i = points.size() - 1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float const collisionHeight = owner->GetCollisionHeight();
    while (true)
    {
        if ((points[i - 1] - target).squaredLength() >= distSq)
            break;
        owner->GetHitSpherePointFor({points[i - 1].x, points[i - 1].y, points[i - 1].z + collisionHeight}, x, y, z);
        if (!owner->GetMap()->isInLineOfSight(x, y, z, points[i - 1].x, points[i - 1].y, points[i - 1].z + collisionHeight,
                                              owner->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                              VMAP::ModelIgnoreFlags::Nothing))
        {
            points.resize(i + 1);
            return;
        }
        if (!--i)
        {
            points[0] = points[1];
            points.resize(2);
            return;
        }
    }
    // [lead161] a zero-length last segment (two equal route points) has no direction: G3D's direction() of it is
    // NaN, the shortened end became (nan, ...) - the end stays where it is then
    G3D::Vector3 const back = points[i - 1] - points[i];
    if (back.squaredLength() > 1e-6f)
        points[i] += back.direction() * (dist - (points[i] - target).length());
    points.resize(i + 1);
}

bool WalkBotPoints(Player* bot, Movement::PointsArray points)
{
    MotionMaster* mm = bot ? bot->GetMotionMaster() : nullptr;
    if (!mm || points.size() < 2)
        return false;
    // [lead161] no spline through a non-finite point
    for (G3D::Vector3 const& point : points)
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
            return false;
    points[0] = G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    Leader::Travel::NoteHeightGap(bot, points[0], points[0].z, "walker position");  // [lead144]
    mm->Clear();
    mm->MoveSplinePath(&points);
    return true;
}

bool WalkBotRoute(Player* bot, float x, float y, float z)
{
    if (!UsesLeaderRoute(bot))
        return false;
    BotPath const route = FindBotPath(bot, x, y, z);
    return !(route.type & PATHFIND_NOPATH) && WalkBotPoints(bot, route.points);
}
