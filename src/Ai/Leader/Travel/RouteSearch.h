/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_ROUTESEARCH_H
#define PLAYERBOTS_LEADER_ROUTESEARCH_H

// [lead99] user: production code does not live in the test harness - the leader's one route search (the long Detour
// search, moved here from TestHarnessRunner.cpp unchanged). The harness calls it; Leader::Route (LeaderOrders.h) is
// the leader's walk service on top of it.

#include "DetourNavMeshQuery.h"
#include "G3D/AABox.h"
#include "G3D/Vector3.h"
#include "G3D/Vector4.h"
#include "MoveSplineInitArgs.h"
#include <vector>

class Unit;

namespace Leader::Travel
{
    constexpr int LONG_ROUTE_NODES = 8192;  // 8x MMapMgr's 1024; one query per navmesh (and thread), ~350 KB
    constexpr int LONG_ROUTE_BIG_NODES = 65535;
    constexpr bool ROUTE_LONG_BIG_POOL = true;
    constexpr int LONG_ROUTE_MAX_POLYS = 2048;
    constexpr int LONG_ROUTE_MAX_CORNERS = 256;
    constexpr bool ROUTE_LONG_REVERSE = true;

    struct NavMeshQueryFree
    {
        void operator()(dtNavMeshQuery* query) const { dtFreeNavMeshQuery(query); }
    };

    // The long search: an own dtNavMeshQuery on the walker's map navmesh (per thread), the player filter flags
    // (+ NAV_GROUND_STEEP when steep), the corridor as corner points. False when start or end has no polygon;
    // complete: the corridor reaches the end polygon. `avoidBoxes` / `avoidDiscs`: polygons refused (closed passage
    // doors, a hopeless pack's reach).
    bool LongRoute(Unit* owner, G3D::Vector3 const& start, G3D::Vector3 const& dest, bool steep,
                   Movement::PointsArray& corners, bool& complete, std::vector<G3D::AABox> const* avoidBoxes = nullptr,
                   std::vector<G3D::Vector4> const* avoidDiscs = nullptr);

    // [lead107] the point `walk` yd along `points` (its last point when the path is shorter)
    G3D::Vector3 PointOnPath(Movement::PointsArray const& points, float walk);
}

#endif
