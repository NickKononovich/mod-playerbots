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
#include <string>
#include <unordered_map>
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
        bool memoHit = false;    // [perf213] answered from the per-thread memo of exact inputs (no search ran)
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

    // [nav165] the deep search: one A* over the navmesh's polygons with its own node table (Detour's pool is 16-bit:
    // 65535 nodes at most, LONG_ROUTE_BIG_NODES), resumable - Step() expands `expansions` polygons and returns, the
    // next call goes on. For a walker that the route search cannot plan out of where it stands (b438 Sycina L19 on a
    // Stonetalon ridge: the only way down 4084 yd, 862 polygons, ~115k polygons expanded). The player filter flags with
    // NAV_GROUND_STEEP, no off-mesh link (they can be one-way); the corridor as corner points (findStraightPath)
    class DeepSearch
    {
    public:
        enum class State : uint8
        {
            Idle,
            Running,
            Found,
            Failed
        };
        // false (with why) when the walker has no navmesh or an end has no polygon
        bool Start(Unit* walker, G3D::Vector3 const& from, G3D::Vector3 const& to, uint32 maxNodes, std::string& why);
        State Step(Unit* walker, uint32 expansions);
        State GetState() const { return _state; }
        Movement::PointsArray const& Corners() const { return _corners; }
        std::string const& Why() const { return _why; }
        size_t Nodes() const { return _nodes.size(); }
        uint32 Steps() const { return _steps; }
        float Length() const { return _length; }

    private:
        struct Node
        {
            float g = 0.0f;
            float c[3] = {0.0f, 0.0f, 0.0f};  // the polygon's centre (Detour's y, z, x)
            dtPolyRef parent = 0;
            bool closed = false;
        };
        bool Centre(dtPolyRef ref, float* out) const;
        void Finish();

        State _state = State::Idle;
        dtNavMesh const* _mesh = nullptr;
        dtQueryFilter _filter;
        dtPolyRef _startRef = 0, _endRef = 0;
        float _startPos[3] = {0.0f, 0.0f, 0.0f};
        float _endPos[3] = {0.0f, 0.0f, 0.0f};
        uint32 _maxNodes = 0;
        uint32 _steps = 0;
        float _length = 0.0f;
        std::unordered_map<dtPolyRef, Node> _nodes;
        std::vector<std::pair<float, dtPolyRef>> _open;  // a min-heap on f (std::push_heap with std::greater)
        Movement::PointsArray _corners;
        std::string _why;
    };
}

namespace Leader
{
    // [perf214] the route searches this thread has started (Leader::Route's top searches, a tick cache's answer not
    // counted): a caller paces a sweep of searches by them
    uint64 RouteSearchesOnThread();
}

#endif
