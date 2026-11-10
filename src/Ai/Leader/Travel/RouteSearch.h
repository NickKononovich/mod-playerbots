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

#include "Define.h"
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

    // [guild13] a route search penalty: an edge that enters the disc (x, y, z within 10 yd, radius r) costs `cost`
    // yards more - an idle mob's aggro reach priced by the fight it starts (SoloSafety::RouteCosts)
    struct RoutePenalty
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float r = 0.0f;
        float cost = 0.0f;
    };

    // [perf210] what one LongRoute did: the route search's profile, and `noComplete` - no search with the same
    // polygons open completes this walk whatever its costs (no start / end polygon, or a search that visited every
    // polygon it reaches without the nodes running out)
    struct LongRouteInfo
    {
        bool noComplete = false;
        uint32 findPaths = 0;  // forward searches (8192 nodes)
        uint32 reverse = 0;    // back searches after the forward one ran out of nodes
        uint32 big = 0;        // big-pool searches (65535 nodes)
        uint64 nodes = 0;      // nodes the searches used, summed
        uint64 findPathUs = 0;
        uint64 samplesUs = 0;
        uint64 filterTests = 0;  // polygon tests against the refused discs / boxes
        uint64 filterMemo = 0;   // ... answered from the search's memo
    };

    // The long search: an own dtNavMeshQuery on the walker's map navmesh (per thread), the player filter flags
    // (+ NAV_GROUND_STEEP when steep), the corridor as corner points. False when start or end has no polygon;
    // complete: the corridor reaches the end polygon. `avoidBoxes` / `avoidDiscs`: polygons refused (closed passage
    // doors, a hopeless pack's reach). [lead147] `samples` (with `sampleStep` > 0): the corridor walked every
    // `sampleStep` yd, each point at the navmesh's detail height of its corridor polygon.
    bool LongRoute(Unit* owner, G3D::Vector3 const& start, G3D::Vector3 const& dest, bool steep,
                   Movement::PointsArray& corners, bool& complete, std::vector<G3D::AABox> const* avoidBoxes = nullptr,
                   std::vector<G3D::Vector4> const* avoidDiscs = nullptr, float sampleStep = 0.0f,
                   Movement::PointsArray* samples = nullptr, std::vector<RoutePenalty> const* penalties = nullptr,
                   LongRouteInfo* info = nullptr);

    // [lead107] the point `walk` yd along `points` (its last point when the path is shorter)
    G3D::Vector3 PointOnPath(Movement::PointsArray const& points, float walk);

    // [lead144] diagnostic only: logs [height_gap] once per 1 yd cell when `point` (a route sample after its ground
    // snap, or a walker's own position) lies farther than the tile's walkable climb from the navmesh's detail height
    // there: the sample z, the z before the snap, the navmesh z, the grid z, the vmap floor from both, the liquid
    bool HeightGapLogEnabled();  // [guild4] AiPlayerbot.HeightGapLog (default 0)
    void NoteHeightGap(Unit* walker, G3D::Vector3 const& point, float beforeSnap, char const* what);

    // [lead204] The shortest open walk over the stops of `cost` (cost[a][b]: the walk from stop a to stop b, +inf
    // when there is none): stop 0 is the start, with `fixedEnd` the last stop is the end (the next boss), else the
    // walk ends at whichever stop comes last. Nearest neighbour and cheapest insertion, each improved by 2-opt and
    // or-opt (segments of 1-3 stops), the shorter kept - the order tools/ideal_route.py computes. The order of the
    // stops (0 first) and its `length`.
    std::vector<size_t> ShortestTour(std::vector<std::vector<float>> const& cost, bool fixedEnd, float& length);
}

#endif
