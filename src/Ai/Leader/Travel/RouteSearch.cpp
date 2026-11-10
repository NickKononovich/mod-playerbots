/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBaseAction.h"  // [static39]
#include "Formulas.h"  // [zone38]
#include "GameTime.h"  // [route102]
#include "DBCStores.h"  // [zone38]
#include "Common.h"  // math defines first (Position.h)
#include "SlowScope.h"  // [perf26]
#include "RouteSearch.h"

#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNode.h"  // [perf210] the node pool's count
#include "LeaderOrders.h"
#include "SoloSafety.h"  // [guild13]
#include "Config.h"
#include "Log.h"
#include "Map.h"
#include "PerfMonitor.h"  // [guild15]
#include "PlayerbotAI.h"  // [deaths3] HasGroundSupport
#include "Player.h"  // [deaths] the pull spot walk
#include "MapDefines.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Unit.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Leader::Travel
{
    // The long search: an own dtNavMeshQuery on the map's navmesh (the harness runs on the world thread while no
    // map updates, as its PathGenerator calls do), PathGenerator's player filter flags (+ NAV_GROUND_STEEP when
    // steep), the corridor as corner points. False when start or end has no polygon; complete: the corridor
    // reaches the end polygon.
    // [hleft-passage-door-route] A filter that refuses every polygon whose bounds meet one of `boxes` (closed
    // doors that open only on a boss's death, DOOR_TYPE_PASSAGE): the navmesh holds no game objects, so a walk
    // "through" Talon King Ikiss's exit door 183398 (Sethekk Halls, the shortcut from the entrance hall up to
    // his room) is the shortest path while the door is shut. The long search with it finds the way a player
    // takes (the stairs round, seth-h 2026-10-25: "next boss behind a closed door" for Ikiss, the route fell back
    // to the full clear).
    class DoorAvoidFilter : public dtQueryFilter
    {
    public:
        std::vector<G3D::AABox> const* boxes = nullptr;
        // [b32_brs] discs (x, y, z, radius): a polygon whose centre lies within radius (2D, within 10 yd in
        // height) of one is refused - a hopeless pack's aggro reach (HopelessDetourLeg)
        std::vector<G3D::Vector4> const* discs = nullptr;
        std::vector<RoutePenalty> const* penalties = nullptr;  // [guild13]

        // [route102] the discs and penalties binned on a grid once per search: passFilter / getCost run for every
        // node the search expands and tested each against the whole list (b417: 200-430 ms searches with hundreds of
        // static discs); now only the entries of the cells the polygon / step touches.
        // [perf210] the grid is a flat array over the entries' bounds (was a hash map: a hash lookup per cell per
        // node), a polygon / step outside those bounds tests nothing, and each polygon's verdict is kept for the
        // search's three findPath calls (forward, back, big pool): the same answers, a fraction of the work per node
        struct Grid
        {
            static constexpr float CELL = 16.0f;               // yd, the cells [route102] binned on
            static constexpr uint64 DENSE_CELLS = 1ull << 18;  // a flat array up to ~1 MB of offsets, else sorted
            static constexpr float MARGIN = 0.05f;             // yd: float rounding of the exact tests stays inside
            float minX = 0.0f, minY = 0.0f, maxX = -1.0f, maxY = -1.0f;  // the entries' bounds; empty: maxX < minX
            int32 c0 = 0, r0 = 0, cols = 0, rows = 0;                    // the dense array's first cell and size
            bool dense = false;
            std::vector<uint32> start;                     // dense: cols * rows + 1 offsets into `items`
            std::vector<uint32> items;                     // dense: the entries of each cell, ascending
            std::vector<std::pair<uint64, uint32>> pairs;  // sorted (cell, entry): the cells of wide bounds

            static int32 CellOf(float v) { return int32(std::floor(v / CELL)); }
            static uint64 CellKey(int32 cx, int32 cy) { return (uint64(uint32(cx)) << 32) | uint32(cy); }
            bool Empty() const { return maxX < minX; }
            size_t Slot(int32 cx, int32 cy) const { return size_t(cx - c0) * size_t(rows) + size_t(cy - r0); }

            // `bounds(i, x0, y0, x1, y1)` of each of `count` entries; each is listed in every cell its bounds touch
            template <typename B>
            void Build(uint32 count, B const& bounds)
            {
                minX = minY = 0.0f;
                maxX = maxY = -1.0f;
                start.clear();
                items.clear();
                pairs.clear();
                if (!count)
                    return;
                minX = minY = 1e30f;
                maxX = maxY = -1e30f;
                for (uint32 i = 0; i < count; ++i)
                {
                    float x0, y0, x1, y1;
                    bounds(i, x0, y0, x1, y1);
                    minX = std::min(minX, x0);
                    minY = std::min(minY, y0);
                    maxX = std::max(maxX, x1);
                    maxY = std::max(maxY, y1);
                }
                c0 = CellOf(minX);
                r0 = CellOf(minY);
                cols = CellOf(maxX) - c0 + 1;
                rows = CellOf(maxY) - r0 + 1;
                minX -= MARGIN;
                minY -= MARGIN;
                maxX += MARGIN;
                maxY += MARGIN;
                dense = uint64(cols) * uint64(rows) <= DENSE_CELLS;
                if (!dense)
                {
                    for (uint32 i = 0; i < count; ++i)
                    {
                        float x0, y0, x1, y1;
                        bounds(i, x0, y0, x1, y1);
                        for (int32 cx = CellOf(x0); cx <= CellOf(x1); ++cx)
                            for (int32 cy = CellOf(y0); cy <= CellOf(y1); ++cy)
                                pairs.emplace_back(CellKey(cx, cy), i);
                    }
                    std::sort(pairs.begin(), pairs.end());
                    return;
                }
                start.assign(size_t(cols) * size_t(rows) + 1, 0);
                for (uint32 i = 0; i < count; ++i)
                {
                    float x0, y0, x1, y1;
                    bounds(i, x0, y0, x1, y1);
                    for (int32 cx = CellOf(x0); cx <= CellOf(x1); ++cx)
                        for (int32 cy = CellOf(y0); cy <= CellOf(y1); ++cy)
                            ++start[Slot(cx, cy) + 1];
                }
                for (size_t c = 1; c < start.size(); ++c)
                    start[c] += start[c - 1];
                items.assign(start.back(), 0);
                std::vector<uint32> fill(start.begin(), start.end() - 1);
                for (uint32 i = 0; i < count; ++i)
                {
                    float x0, y0, x1, y1;
                    bounds(i, x0, y0, x1, y1);
                    for (int32 cx = CellOf(x0); cx <= CellOf(x1); ++cx)
                        for (int32 cy = CellOf(y0); cy <= CellOf(y1); ++cy)
                            items[fill[Slot(cx, cy)]++] = i;
                }
            }

            // [route102]'s candidates of (x0, y0)-(x1, y1) in its order (cells x then y, a cell's entries ascending),
            // each once; none when the box misses every entry's bounds (no exact test can pass there)
            void Near(float x0, float y0, float x1, float y1, std::vector<uint32>& out, std::vector<uint32>& mark,
                      uint32& stamp) const
            {
                out.clear();
                if (Empty() || x1 < minX || x0 > maxX || y1 < minY || y0 > maxY)
                    return;
                if (++stamp == 0)
                {
                    std::fill(mark.begin(), mark.end(), 0);
                    stamp = 1;
                }
                int32 const cx0 = CellOf(x0), cx1 = CellOf(x1), cy0 = CellOf(y0), cy1 = CellOf(y1);
                bool const single = cx0 == cx1 && cy0 == cy1;  // a cell lists an entry once
                auto const take = [&](uint32 i)
                {
                    if (single)
                        out.push_back(i);
                    else if (mark[i] != stamp)
                    {
                        mark[i] = stamp;
                        out.push_back(i);
                    }
                };
                for (int32 cx = cx0; cx <= cx1; ++cx)
                    for (int32 cy = cy0; cy <= cy1; ++cy)
                    {
                        if (dense)
                        {
                            if (cx < c0 || cx >= c0 + cols || cy < r0 || cy >= r0 + rows)
                                continue;
                            size_t const c = Slot(cx, cy);
                            for (uint32 k = start[c]; k < start[c + 1]; ++k)
                                take(items[k]);
                            continue;
                        }
                        uint64 const key = CellKey(cx, cy);
                        auto it = std::lower_bound(pairs.begin(), pairs.end(), std::make_pair(key, uint32(0)));
                        for (; it != pairs.end() && it->first == key; ++it)
                            take(it->second);
                    }
            }
        };
        Grid discGrid;
        Grid penaltyGrid;
        // per-search scratch (a filter lives for one LongRoute on one thread)
        mutable std::vector<uint32> nearScratch;
        mutable std::vector<uint32> discMark;
        mutable std::vector<uint32> penaltyMark;
        mutable uint32 discStamp = 0;
        mutable uint32 penaltyStamp = 0;
        // [perf210] polygon -> refused by the discs / boxes (open addressing; 0 = an empty slot, refs are never 0)
        mutable std::vector<dtPolyRef> memoRef;
        mutable std::vector<uint8> memoRefused;
        mutable size_t memoCount = 0;
        mutable uint64 tests = 0;
        mutable uint64 memoHits = 0;

        void BuildGrids()
        {
            uint32 const discCount = discs ? uint32(discs->size()) : 0;
            discGrid.Build(discCount,
                           [&](uint32 i, float& x0, float& y0, float& x1, float& y1)
                           {
                               G3D::Vector4 const& d = (*discs)[i];
                               x0 = d.x - d.w;
                               y0 = d.y - d.w;
                               x1 = d.x + d.w;
                               y1 = d.y + d.w;
                           });
            uint32 const penaltyCount = penalties ? uint32(penalties->size()) : 0;
            penaltyGrid.Build(penaltyCount,
                              [&](uint32 i, float& x0, float& y0, float& x1, float& y1)
                              {
                                  RoutePenalty const& d = (*penalties)[i];
                                  x0 = d.x - d.r;
                                  y0 = d.y - d.r;
                                  x1 = d.x + d.r;
                                  y1 = d.y + d.r;
                              });
            discMark.assign(discCount, 0);
            penaltyMark.assign(penaltyCount, 0);
            discStamp = penaltyStamp = 0;
            memoRef.clear();
            memoRefused.clear();
            memoCount = 0;
            if (discCount || (boxes && !boxes->empty()))
            {
                memoRef.assign(4096, 0);
                memoRefused.assign(4096, 0);
            }
        }

        static size_t MemoSlot(dtPolyRef ref, size_t mask)
        {
            uint64 h = uint64(ref) * 0x9E3779B97F4A7C15ull;
            return size_t(h >> 20) & mask;
        }

        // the disc / box part of passFilter, uncached
        bool Refused(dtMeshTile const* tile, dtPoly const* poly) const;

        // [guild13] an edge from outside a penalty disc into it pays the disc's cost once (Detour: (y, z, x))
        float getCost(float const* pa, float const* pb, dtPolyRef const prevRef, dtMeshTile const* prevTile,
                      dtPoly const* prevPoly, dtPolyRef const curRef, dtMeshTile const* curTile, dtPoly const* curPoly,
                      dtPolyRef const nextRef, dtMeshTile const* nextTile, dtPoly const* nextPoly) const override
        {
            float cost = dtQueryFilter::getCost(pa, pb, prevRef, prevTile, prevPoly, curRef, curTile, curPoly, nextRef,
                                                nextTile, nextPoly);
            if (!penalties)
                return cost;
            // [route102] the penalties whose cells the step's bounds touch, each once ([perf210]: none when the step
            // misses all their bounds - most steps of a long search)
            penaltyGrid.Near(std::min(pa[2], pb[2]), std::min(pa[0], pb[0]), std::max(pa[2], pb[2]),
                             std::max(pa[0], pb[0]), nearScratch, penaltyMark, penaltyStamp);
            if (nearScratch.empty())
                return cost;
            // [guild13] the step pa -> pb (Detour's portal midpoints, a polygon apart: outdoors 20-50 yd) enters the
            // disc where its segment comes within the radius, not only where pb lies inside
            float const sx = pb[2] - pa[2];
            float const sy = pb[0] - pa[0];
            float const lengthSq = sx * sx + sy * sy;
            for (uint32 const penIndex : nearScratch)
            {
                RoutePenalty const& disc = (*penalties)[penIndex];
                if (std::fabs(pb[1] - disc.z) > 10.0f && std::fabs(pa[1] - disc.z) > 10.0f)
                    continue;
                float const ax = pa[2] - disc.x;
                float const ay = pa[0] - disc.y;
                if (ax * ax + ay * ay <= disc.r * disc.r)
                    continue;  // already inside: paid on the way in
                float t = lengthSq > 0.0f ? -(ax * sx + ay * sy) / lengthSq : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                float const ex = ax + sx * t;
                float const ey = ay + sy * t;
                if (ex * ex + ey * ey <= disc.r * disc.r)
                    cost += disc.cost;
            }
            return cost;
        }

        bool passFilter(dtPolyRef const ref, dtMeshTile const* tile, dtPoly const* poly) const override
        {
            if (!dtQueryFilter::passFilter(ref, tile, poly))
                return false;
            if (memoRef.empty() || !tile || !poly || poly->vertCount == 0)
                return true;
            // [perf210] the verdict depends on the polygon alone: each is judged once per search
            ++tests;
            size_t const mask = memoRef.size() - 1;
            size_t slot = MemoSlot(ref, mask);
            while (memoRef[slot] && memoRef[slot] != ref)
                slot = (slot + 1) & mask;
            if (memoRef[slot] == ref)
            {
                ++memoHits;
                return !memoRefused[slot];
            }
            bool const refused = Refused(tile, poly);
            memoRef[slot] = ref;
            memoRefused[slot] = refused ? 1 : 0;
            if (++memoCount * 2 > memoRef.size())  // half full: double, re-placed
            {
                std::vector<dtPolyRef> previousRef(memoRef.size() * 2, 0);
                std::vector<uint8> previousRefused(memoRef.size() * 2, 0);
                previousRef.swap(memoRef);
                previousRefused.swap(memoRefused);
                size_t const wide = memoRef.size() - 1;
                for (size_t i = 0; i < previousRef.size(); ++i)
                    if (previousRef[i])
                    {
                        size_t s = MemoSlot(previousRef[i], wide);
                        while (memoRef[s])
                            s = (s + 1) & wide;
                        memoRef[s] = previousRef[i];
                        memoRefused[s] = previousRefused[i];
                    }
            }
            return !refused;
        }
    };

    bool DoorAvoidFilter::Refused(dtMeshTile const* tile, dtPoly const* poly) const
    {
        if (discs && !discs->empty())
        {
            // [route102] the discs whose cells the polygon's bounds touch, each once
            float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
            for (uint8 index = 0; index < poly->vertCount; ++index)
            {
                float const* vertex = &tile->verts[size_t(poly->verts[index]) * 3];
                minX = std::min(minX, vertex[2]);
                maxX = std::max(maxX, vertex[2]);
                minY = std::min(minY, vertex[0]);
                maxY = std::max(maxY, vertex[0]);
            }
            discGrid.Near(minX, minY, maxX, maxY, nearScratch, discMark, discStamp);
            if (!nearScratch.empty())
            {
                G3D::Vector3 centre(0.0f, 0.0f, 0.0f);
                for (uint8 index = 0; index < poly->vertCount; ++index)
                {
                    float const* vertex = &tile->verts[size_t(poly->verts[index]) * 3];
                    centre += G3D::Vector3(vertex[2], vertex[0], vertex[1]);  // Detour stores (y, z, x)
                }
                centre /= float(poly->vertCount);
                for (uint32 const di : nearScratch)
                {
                    G3D::Vector4 const& disc = (*discs)[di];
                    if (std::fabs(centre.z - disc.z) > 10.0f)
                        continue;
                    if ((centre.x - disc.x) * (centre.x - disc.x) + (centre.y - disc.y) * (centre.y - disc.y) <=
                        disc.w * disc.w)
                        return true;
                    // [guild13] a large polygon that only overlaps the disc: one of its edges comes within the radius
                    // (a solo bot's search, the one that carries penalties; the harness's discs keep the centre test)
                    for (uint8 index = 0; penalties && index < poly->vertCount; ++index)
                    {
                        float const* va = &tile->verts[size_t(poly->verts[index]) * 3];
                        float const* vb = &tile->verts[size_t(poly->verts[(index + 1) % poly->vertCount]) * 3];
                        float const ax = va[2] - disc.x;
                        float const ay = va[0] - disc.y;
                        float const ex = vb[2] - va[2];
                        float const ey = vb[0] - va[0];
                        float const lengthSq = ex * ex + ey * ey;
                        float t = lengthSq > 0.0f ? -(ax * ex + ay * ey) / lengthSq : 0.0f;
                        t = std::clamp(t, 0.0f, 1.0f);
                        float const dx = ax + ex * t;
                        float const dy = ay + ey * t;
                        if (dx * dx + dy * dy <= disc.w * disc.w)
                            return true;
                    }
                }
            }
        }
        if (!boxes || boxes->empty())
            return false;
        // Detour stores (y, z, x).
        G3D::Vector3 low(1e9f, 1e9f, 1e9f);
        G3D::Vector3 high(-1e9f, -1e9f, -1e9f);
        for (uint8 index = 0; index < poly->vertCount; ++index)
        {
            float const* vertex = &tile->verts[size_t(poly->verts[index]) * 3];
            G3D::Vector3 const point(vertex[2], vertex[0], vertex[1]);
            low = low.min(point);
            high = high.max(point);
        }
        for (G3D::AABox const& box : *boxes)
            if (high.x >= box.low().x && low.x <= box.high().x && high.y >= box.low().y && low.y <= box.high().y &&
                high.z >= box.low().z && low.z <= box.high().z)
                return true;
        return false;
    }

    // [lead147] the floor of a tall column: of the polygons in it, the highest whose closest point lies at most the
    // tile's walkable climb above the point (the floor under a walker), the ones the point stands over first; with
    // none, the lowest above it (a walker sunk under its floor steps up onto it). Detour's own nearest polygon took
    // the nearest in 3D, a floor above as soon as one below
    class ColumnFloorQuery : public dtPolyQuery
    {
    public:
        ColumnFloorQuery(dtNavMeshQuery const* query, float const* centre) : _query(query), _centre(centre) {}

        void process(dtMeshTile const* tile, dtPoly** /*polys*/, dtPolyRef* refs, int count) override
        {
            for (int index = 0; index < count; ++index)
            {
                float closest[3];
                bool over = false;
                if (dtStatusFailed(_query->closestPointOnPoly(refs[index], _centre, closest, &over)))
                    continue;
                float const climb = tile && tile->header ? tile->header->walkableClimb : 0.0f;
                bool const under = closest[1] <= _centre[1] + climb;
                // a floor under it before one above, over it before beside it, then the nearest in height
                int const rank = (under ? 0 : 2) + (over ? 0 : 1);
                float const gap = std::fabs(closest[1] - _centre[1]);
                if (!_ref || rank < _rank || (rank == _rank && gap < _gap))
                {
                    _ref = refs[index];
                    _rank = rank;
                    _gap = gap;
                    dtVcopy(_nearest, closest);
                }
            }
        }

        dtPolyRef Ref() const { return _ref; }
        float const* Nearest() const { return _nearest; }

    private:
        dtNavMeshQuery const* _query;
        float const* _centre;
        dtPolyRef _ref = 0;
        int _rank = 0;
        float _gap = 0.0f;
        float _nearest[3] = {0.0f, 0.0f, 0.0f};
    };

    bool LongRoute(Unit* owner, G3D::Vector3 const& start, G3D::Vector3 const& dest, bool steep,
                   Movement::PointsArray& corners, bool& complete, std::vector<G3D::AABox> const* avoidBoxes,
                   std::vector<G3D::Vector4> const* avoidDiscs, float sampleStep, Movement::PointsArray* samples,
                   std::vector<RoutePenalty> const* penalties, LongRouteInfo* info)
    {
        complete = false;
        if (info)
            *info = LongRouteInfo{};
        dtNavMesh const* navMesh = owner->GetMap()->GetMapCollisionData().GetMMapData().GetNavMesh();
        if (!navMesh)
            return false;
        // One query per navmesh, kept: it only holds the navmesh pointer and its node pool, cleared by every
        // findPath, and is used with a navmesh taken from a live map only (instances share their parent's).
        // [lead99] per thread: the leader's walk checks (Leader::Route) run on the map threads too
        thread_local std::unordered_map<dtNavMesh const*, std::unique_ptr<dtNavMeshQuery, NavMeshQueryFree>>
            queries;
        auto& query = queries[navMesh];
        if (!query)
        {
            query.reset(dtAllocNavMeshQuery());
            if (!query || dtStatusFailed(query->init(navMesh, LONG_ROUTE_NODES)))
            {
                query.reset();
                return false;
            }
        }

        DoorAvoidFilter filter;
        filter.boxes = avoidBoxes;
        filter.discs = avoidDiscs;  // [b32_brs]
        filter.penalties = penalties && !penalties->empty() ? penalties : nullptr;  // [guild13]
        filter.BuildGrids();  // [route102]
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | (steep ? NAV_GROUND_STEEP : 0)));
        filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));

        // Detour works in (y, z, x).
        float const startPoint[3] = {start.y, start.z, start.x};
        float const endPoint[3] = {dest.y, dest.z, dest.x};
        auto nearestPoly = [&query, &filter](float const* point, float* nearest)
        {
            // PathGenerator::GetPolyByLocation: a low box first, then a tall one.
            float extents[3] = {3.0f, 5.0f, 3.0f};
            dtPolyRef ref = 0;
            if (dtStatusSucceed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) && ref)
                return ref;
            extents[1] = 50.0f;
            ColumnFloorQuery column(query.get(), point);  // [lead147]
            if (dtStatusFailed(query->queryPolygons(point, extents, &filter, &column)) || !column.Ref())
                return dtPolyRef(0);
            dtVcopy(nearest, column.Nearest());
            return column.Ref();
        };
        float startNearest[3];
        float endNearest[3];
        dtPolyRef const startRef = nearestPoly(startPoint, startNearest);
        dtPolyRef const endRef = nearestPoly(endPoint, endNearest);
        // [perf210] what the search did, for the route search's profile (and its pass-3 skip: `noComplete`)
        auto const timeNow = []() { return std::chrono::steady_clock::now(); };
        auto const micros = [](std::chrono::steady_clock::time_point since)
        {
            return uint64(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since)
                    .count());
        };
        auto const noteFilter = [&]()
        {
            if (info)
            {
                info->filterTests = filter.tests;
                info->filterMemo = filter.memoHits;
            }
        };
        if (!startRef || !endRef)
        {
            if (info)
                info->noComplete = true;  // the same polygons open: no start / end polygon whatever the costs
            noteFilter();
            return false;
        }

        // [perf210] the corridors in per-thread buffers (three 16 KB vectors per call before)
        thread_local std::vector<dtPolyRef> polys(LONG_ROUTE_MAX_POLYS);
        thread_local std::vector<dtPolyRef> back(LONG_ROUTE_MAX_POLYS);
        thread_local std::vector<dtPolyRef> wide(LONG_ROUTE_MAX_POLYS);
        thread_local std::vector<float> straight(size_t(LONG_ROUTE_MAX_CORNERS) * 3);
        int polyCount = 0;
        auto const searchStart = timeNow();
        dtStatus const status = query->findPath(startRef, endRef, startNearest, endNearest, &filter, polys.data(),
                                                &polyCount, LONG_ROUTE_MAX_POLYS);
        if (info)
        {
            ++info->findPaths;
            if (dtNodePool const* pool = query->getNodePool())
                info->nodes += uint64(pool->getNodeCount());
        }
        if (dtStatusFailed(status) || polyCount <= 0)
        {
            if (info)
                info->findPathUs += micros(searchStart);
            noteFilter();
            return false;
        }

        complete = polys[size_t(polyCount - 1)] == endRef;
        // [perf210] a search that visited every polygon it reaches without the nodes running out: the end is not
        // among them, so no search with these polygons open (any costs, any pool) completes this walk
        if (info && !complete && !dtStatusDetail(status, DT_OUT_OF_NODES) &&
            !dtStatusDetail(status, DT_BUFFER_TOO_SMALL))
            info->noComplete = true;
        // The search ran out of nodes: the same search from dest back to the start (SelfTravel::LongRoute's
        // reverse fallback; Wailing Caverns wave 2: the A* from the cliff top floods the plateau towards the goal
        // below and spends its pool, from the goal it leaves the basin at once). Only a complete corridor back to
        // the start's own polygon counts, none through an off-mesh link (they can be one-way). A search that
        // exhausted its part of the navmesh without the nodes running out has no way there in either direction.
        if (!complete && ROUTE_LONG_REVERSE && dtStatusDetail(status, DT_OUT_OF_NODES))
        {
            int backCount = 0;
            bool usable = dtStatusSucceed(query->findPath(endRef, startRef, endNearest, startNearest, &filter,
                                                          back.data(), &backCount, LONG_ROUTE_MAX_POLYS)) &&
                          backCount > 0 && back[size_t(backCount - 1)] == startRef;
            if (info)
            {
                ++info->reverse;
                if (dtNodePool const* pool = query->getNodePool())
                    info->nodes += uint64(pool->getNodeCount());
            }
            for (int index = 0; usable && index < backCount; ++index)
            {
                dtMeshTile const* tile = nullptr;
                dtPoly const* poly = nullptr;
                usable = dtStatusSucceed(navMesh->getTileAndPolyByRef(back[size_t(index)], &tile, &poly)) && poly &&
                         poly->getType() != DT_POLYTYPE_OFFMESH_CONNECTION;
            }
            if (usable)
            {
                std::reverse(back.begin(), back.begin() + backCount);
                polys.swap(back);
                polyCount = backCount;
                complete = true;
            }
        }
        if (!complete && ROUTE_LONG_BIG_POOL && dtStatusDetail(status, DT_OUT_OF_NODES))
        {
            thread_local std::unique_ptr<dtNavMeshQuery, NavMeshQueryFree> big(dtAllocNavMeshQuery());
            int wideCount = 0;
            dtStatus wideStatus = DT_FAILURE;
            if (big && dtStatusSucceed(big->init(navMesh, LONG_ROUTE_BIG_NODES)))
            {
                wideStatus = big->findPath(startRef, endRef, startNearest, endNearest, &filter, wide.data(), &wideCount,
                                           LONG_ROUTE_MAX_POLYS);
                if (info)
                {
                    ++info->big;
                    if (dtNodePool const* pool = big->getNodePool())
                        info->nodes += uint64(pool->getNodeCount());
                }
            }
            if (dtStatusSucceed(wideStatus) && wideCount > 0 && wide[size_t(wideCount - 1)] == endRef)
            {
                polys.swap(wide);
                polyCount = wideCount;
                complete = true;
            }
            // [perf210] the big pool visited every polygon it reaches without running out: no way there with these
            // polygons open (nor a ground walk back, Detour's ground links being two-way - the back search's rule)
            else if (info && dtStatusSucceed(wideStatus) && wideCount > 0 &&
                     !dtStatusDetail(wideStatus, DT_OUT_OF_NODES) && !dtStatusDetail(wideStatus, DT_BUFFER_TOO_SMALL))
                info->noComplete = true;
        }
        if (info)
            info->findPathUs += micros(searchStart);
        noteFilter();
        float target[3] = {endNearest[0], endNearest[1], endNearest[2]};
        if (!complete)
            query->closestPointOnPoly(polys[size_t(polyCount - 1)], endNearest, target, nullptr);

        int cornerCount = 0;
        if (dtStatusFailed(query->findStraightPath(startNearest, target, polys.data(), polyCount, straight.data(),
                                                   nullptr, nullptr, &cornerCount, LONG_ROUTE_MAX_CORNERS)) ||
            cornerCount < 1)
            return false;

        corners.clear();
        corners.reserve(size_t(cornerCount));
        for (int index = 0; index < cornerCount; ++index)
            corners.emplace_back(straight[size_t(index) * 3 + 2], straight[size_t(index) * 3],
                                 straight[size_t(index) * 3 + 1]);
        if (samples && sampleStep > 0.0f)
        {
            // [lead147] b168 20261101-140326 [height_gap]: the samples between two corners were a straight line in 3D
            // (RFC y -37: -23.9 -> -26.7 under the navmesh's -21.0 -> -17.3 hump, inside the rock, where the ground
            // snap finds no floor) and the walker's spline went through the rock. Each sample takes the detail height
            // of the corridor polygon under it (the corridor in order: a cursor), else of the nearer of the cursor's
            // polygon and the next (a point on their shared edge)
            auto const samplesStart = timeNow();
            samples->clear();
            // [perf210] the corridor's polygons looked up once, with their bounds: a sample outside a ground
            // polygon's bounds is not in it (dtPointInPolygon finds no crossing there), so its height is not asked.
            // A sample on no ground polygon from the cursor on (an off-mesh link's stretch) scanned the rest of the
            // corridor with a lookup, a vertex copy and a point-in-polygon test per polygon
            struct CorridorPoly
            {
                bool usable = false;
                bool offMesh = false;
                float maxX = 0.0f, minZ = 0.0f, maxZ = 0.0f;  // Detour (x, z): the game's y and x
            };
            constexpr float BOUND_MARGIN = 0.05f;  // yd past the bounds: float rounding of the crossing test
            thread_local std::vector<CorridorPoly> corridor;
            corridor.assign(size_t(polyCount), CorridorPoly{});
            for (int index = 0; index < polyCount; ++index)
            {
                dtMeshTile const* tile = nullptr;
                dtPoly const* poly = nullptr;
                if (dtStatusFailed(navMesh->getTileAndPolyByRef(polys[size_t(index)], &tile, &poly)) || !poly)
                    continue;
                CorridorPoly& c = corridor[size_t(index)];
                c.usable = true;
                c.offMesh = poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION;
                c.maxX = -1e30f;
                c.minZ = 1e30f;
                c.maxZ = -1e30f;
                for (uint8 v = 0; v < poly->vertCount; ++v)
                {
                    float const* vertex = &tile->verts[size_t(poly->verts[v]) * 3];
                    c.maxX = std::max(c.maxX, vertex[0]);
                    c.minZ = std::min(c.minZ, vertex[2]);
                    c.maxZ = std::max(c.maxZ, vertex[2]);
                }
            }
            size_t cursor = 0;
            auto const onSurface = [&](G3D::Vector3& point)
            {
                float const position[3] = {point.y, point.z, point.x};
                for (bool const offMesh : {false, true})
                    for (size_t index = cursor; index < size_t(polyCount); ++index)
                    {
                        CorridorPoly const& c = corridor[index];
                        if (!c.usable || c.offMesh != offMesh)
                            continue;
                        if (!offMesh &&
                            (position[2] < c.minZ || position[2] >= c.maxZ || position[0] >= c.maxX + BOUND_MARGIN))
                            continue;
                        float height = 0.0f;
                        if (dtStatusSucceed(query->getPolyHeight(polys[index], position, &height)))
                        {
                            cursor = index;
                            point.z = height;
                            return;
                        }
                    }
                float best = -1.0f;
                for (size_t index = cursor; index < size_t(polyCount) && index <= cursor + 1; ++index)
                {
                    float closest[3];
                    if (dtStatusFailed(query->closestPointOnPoly(polys[index], position, closest, nullptr)))
                        continue;
                    float const distance = dtVdist2DSqr(position, closest);
                    if (best < 0.0f || distance < best)
                    {
                        best = distance;
                        point.z = closest[1];
                    }
                }
            };
            G3D::Vector3 first = corners.front();
            onSurface(first);
            samples->push_back(first);
            for (size_t index = 1; index < corners.size(); ++index)
            {
                G3D::Vector3 const a = corners[index - 1];
                G3D::Vector3 const b = corners[index];
                float const length = (b - a).length();
                for (float at = sampleStep; at < length; at += sampleStep)
                {
                    G3D::Vector3 point = a + (b - a) * (at / length);
                    onSurface(point);
                    samples->push_back(point);
                }
                G3D::Vector3 corner = b;
                onSurface(corner);
                samples->push_back(corner);
            }
            if (info)
                info->samplesUs += micros(samplesStart);
        }
        return true;
    }

    G3D::Vector3 PointOnPath(Movement::PointsArray const& points, float walk)
    {
        float walked = 0.0f;
        for (size_t index = 1; index < points.size(); ++index)
        {
            G3D::Vector3 const segment = points[index] - points[index - 1];
            float const length = segment.length();
            if (length > 0.0f && walked + length >= walk)
                return points[index - 1] + segment * ((walk - walked) / length);
            walked += length;
        }
        // [b381] callers check for a path of 2+ points; an empty one is never back() (b380 crash)
        return points.empty() ? G3D::Vector3() : points.back();
    }

    bool HeightGapLogEnabled()
    {
        static bool const enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.HeightGapLog", false, false);
        return enabled;
    }

    void NoteHeightGap(Unit* walker, G3D::Vector3 const& point, float beforeSnap, char const* what)
    {
        constexpr int HEIGHT_GAP_NODES = 64;              // findNearestPoly needs no search nodes; init wants some
        constexpr float HEIGHT_GAP_COLUMN = 50.0f;        // the navmesh layers searched above / below the point
        constexpr size_t HEIGHT_GAP_MEMO_MAX = 65536;     // logged cells kept per thread
        constexpr float HEIGHT_GAP_VMAP_UP = 2.0f;        // the floor search starts this far above (Map::GetHeight)
        // [guild4] a diagnostic only (the walk never uses it), off unless AiPlayerbot.HeightGapLog = 1: it ran a navmesh
        // poly search, two vmap floor searches and a liquid query per route sample of every walking bot and flooded
        // the console (the playerbots logger runs at debug level here).
        if (!HeightGapLogEnabled())
            return;
        Map* map = walker ? walker->GetMap() : nullptr;
        dtNavMesh const* navMesh = map ? map->GetMapCollisionData().GetMMapData().GetNavMesh() : nullptr;
        if (!navMesh)
            return;
        thread_local std::unordered_map<dtNavMesh const*, std::unique_ptr<dtNavMeshQuery, NavMeshQueryFree>>
            queries;
        auto& query = queries[navMesh];
        if (!query)
        {
            query.reset(dtAllocNavMeshQuery());
            if (!query || dtStatusFailed(query->init(navMesh, HEIGHT_GAP_NODES)))
            {
                query.reset();
                return;
            }
        }
        dtQueryFilter filter;  // every polygon, liquids included: the navmesh's own floor at the point
        filter.setIncludeFlags(0xFFFF);
        filter.setExcludeFlags(0);
        float const centre[3] = {point.y, point.z, point.x};  // Detour works in (y, z, x)
        float const extents[3] = {1.0f, HEIGHT_GAP_COLUMN, 1.0f};
        float nearest[3];
        dtPolyRef ref = 0;
        if (dtStatusFailed(query->findNearestPoly(centre, extents, &filter, &ref, nearest)) || !ref)
            return;
        dtMeshTile const* tile = nullptr;
        dtPoly const* poly = nullptr;
        if (dtStatusFailed(navMesh->getTileAndPolyByRef(ref, &tile, &poly)) || !tile || !poly || !tile->header)
            return;
        float const meshZ = nearest[1];
        if (std::fabs(point.z - meshZ) <= tile->header->walkableClimb)
            return;
        thread_local std::unordered_set<uint64> logged;
        if (logged.size() >= HEIGHT_GAP_MEMO_MAX)
            logged.clear();
        uint64 const cell = (uint64(map->GetId()) << 40) ^ (uint64(uint32(int32(std::floor(point.x)))) << 20) ^
                            uint64(uint32(int32(std::floor(point.y))) & 0xFFFFF);
        if (!logged.insert(cell).second)
            return;
        auto& staticTree = map->GetMapCollisionData().GetStaticTree();
        float const vmapFromSample = staticTree.getHeight(point.x, point.y, point.z + HEIGHT_GAP_VMAP_UP,
                                                          HEIGHT_GAP_COLUMN);
        float const vmapFromMesh = staticTree.getHeight(point.x, point.y, meshZ + HEIGHT_GAP_VMAP_UP,
                                                        HEIGHT_GAP_COLUMN);
        LiquidData const liquid = map->GetLiquidData(walker->GetPhaseMask(), point.x, point.y,
                                                     std::max(point.z, meshZ), walker->GetCollisionHeight(), {});
        LOG_DEBUG("playerbots",
                 "[height_gap] {} {}: map {} ({:.1f},{:.1f}) sample z {:.1f} (before the ground snap {:.1f}), navmesh "
                 "z {:.1f} (poly flags {:#x}, climb {:.1f}), grid z {:.1f}, vmap z {:.1f} from the sample / {:.1f} "
                 "from the navmesh, liquid entry {} flags {:#x} level {:.1f}",
                 what, walker->GetName(), map->GetId(), point.x, point.y, point.z, beforeSnap, meshZ,
                 uint32(poly->flags), tile->header->walkableClimb, map->GetGridHeight(point.x, point.y),
                 vmapFromSample, vmapFromMesh, liquid.Entry, liquid.Flags, liquid.Level);
    }
}

// [lead99] the leader's one route service (Leader::Route, LeaderOrders.h)
namespace Leader
{
    namespace
    {
        // [spot70] the extra reaches one RouteAvoiding call refuses (this thread, for that call only)
        thread_local std::vector<G3D::Vector4> const* extraAvoid = nullptr;

        // [perf210] the route search's profile: one per top-level search (the [static39] search round the unseen
        // spawns runs inside it and is counted with it; a search a judge asks inside RouteCosts is that judge's
        // cost), summed per minute into the [perf210] line. A log, no decision reads it
        struct SearchProfile
        {
            uint32 depth = 0;
            uint64 pullUs = 0, costsUs = 0, findPathUs = 0, samplesUs = 0, openAirUs = 0, staticUs = 0, zoneUs = 0;
            uint64 longRoutes = 0, findPaths = 0, reverse = 0, big = 0, nodes = 0;
            uint64 filterTests = 0, filterMemo = 0;
            uint64 pass3Skipped = 0, passesReused = 0, costsReused = 0, staticSearches = 0;

            void Add(SearchProfile const& o)
            {
                pullUs += o.pullUs;
                costsUs += o.costsUs;
                findPathUs += o.findPathUs;
                samplesUs += o.samplesUs;
                openAirUs += o.openAirUs;
                staticUs += o.staticUs;
                zoneUs += o.zoneUs;
                longRoutes += o.longRoutes;
                findPaths += o.findPaths;
                reverse += o.reverse;
                big += o.big;
                nodes += o.nodes;
                filterTests += o.filterTests;
                filterMemo += o.filterMemo;
                pass3Skipped += o.pass3Skipped;
                passesReused += o.passesReused;
                costsReused += o.costsReused;
                staticSearches += o.staticSearches;
            }
        };
        thread_local SearchProfile routeProfile;

        uint64 MicrosSince(std::chrono::steady_clock::time_point since)
        {
            return uint64(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since)
                    .count());
        }

        // the searches of the last minute, all map threads: each one's time, and the stages summed
        void NoteSearch(uint64 totalUs, SearchProfile const& one)
        {
            constexpr uint32 WINDOW_MS = MINUTE * IN_MILLISECONDS;  // a log cadence
            static std::mutex lock;
            static std::vector<uint32> times;
            static SearchProfile sum;
            static uint64 sumUs = 0;
            static uint32 windowStart = 0;
            std::lock_guard<std::mutex> guard(lock);
            uint32 const now = getMSTime();
            if (!windowStart)
                windowStart = now;
            times.push_back(uint32(std::min<uint64>(totalUs, std::numeric_limits<uint32>::max())));
            sum.Add(one);
            sumUs += totalUs;
            uint32 const elapsed = getMSTimeDiff(windowStart, now);
            if (elapsed < WINDOW_MS)
                return;
            std::sort(times.begin(), times.end());
            auto const at = [&](float share)
            { return float(times[size_t(share * float(times.size() - 1))]) / 1000.0f; };
            double const total = std::max<double>(double(sumUs), 1.0);
            auto const pct = [&](uint64 us) { return 100.0 * double(us) / total; };
            uint64 const staged =
                sum.pullUs + sum.costsUs + sum.findPathUs + sum.samplesUs + sum.openAirUs + sum.staticUs + sum.zoneUs;
            LOG_INFO(
                "playerbots",
                "[perf210] route searches in {} s: {} (ms p50 {:.1f} / p95 {:.1f} / max {:.1f}, {:.0f} ms in all); "
                "time: route costs {:.0f} %, pull walk {:.0f} %, findPath {:.0f} % ({} long routes: {} forward / "
                "{} back / {} big pool, {:.0f} nodes per long route), samples {:.0f} %, open air {:.0f} %, unseen "
                "spawns {:.0f} %, areas {:.0f} %, rest {:.0f} %; saved: {} pass-3 searches, {} passes and {} "
                "route costs of the {} unseen-spawn searches reused; filter memo {:.0f} % of {} polygon tests",
                elapsed / IN_MILLISECONDS, times.size(), at(0.5f), at(0.95f), float(times.back()) / 1000.0f,
                double(sumUs) / 1000.0, pct(sum.costsUs), pct(sum.pullUs), pct(sum.findPathUs), sum.longRoutes,
                sum.findPaths, sum.reverse, sum.big, sum.longRoutes ? double(sum.nodes) / double(sum.longRoutes) : 0.0,
                pct(sum.samplesUs), pct(sum.openAirUs), pct(sum.staticUs), pct(sum.zoneUs),
                staged < sumUs ? pct(sumUs - staged) : 0.0, sum.pass3Skipped, sum.passesReused, sum.costsReused,
                sum.staticSearches, sum.filterTests ? 100.0 * double(sum.filterMemo) / double(sum.filterTests) : 0.0,
                sum.filterTests);
            times.clear();
            sum = SearchProfile{};
            sumUs = 0;
            windowStart = now;
        }

        // [perf210] one search pass's walk (no reach refused: [guild13]'s priced pass or the bare one): the same
        // inputs give the same walk, so the [static39] search round the unseen spawns reuses the top search's
        struct PassMemo
        {
            bool valid = false;
            bool found = false;  // LongRoute's answer with a walk
            bool complete = false;
            bool openAir = false;
            bool noComplete = false;
            Movement::PointsArray tried;
            Movement::PointsArray triedSurface;
        };

        // [perf210] what a top search hands the [static39] search it starts (same walker, from, destination, tick):
        // its pull walk, RouteCosts' answer and its unrefused passes - the inner search computed them again
        struct SearchPrep
        {
            Unit* walker = nullptr;
            G3D::Vector3 from;
            G3D::Vector3 toAsked;
            G3D::Vector3 to;
            std::vector<G3D::Vector4> pullAvoid;
            std::vector<G3D::Vector4> costRefuse;
            std::vector<Travel::RoutePenalty> penalties;
            PassMemo passes[2][2];  // [priced][steep]
        };
        thread_local SearchPrep const* handedPrep = nullptr;
    }

    std::string RouteResult::Describe() const
    {
        return source + (found ? (complete ? ", complete" : ", partial") : ", none");
    }

    // [deaths3] a stretch of the walk that descends more than PlayerbotAI's GROUND_SUPPORT_DROP with no ground or
    // liquid under a point of it (PlayerbotAI::HasGroundSupport every GROUND_SAMPLE_STEP, the spline guard's sampling)
    static bool OverOpenAir(Unit* walker, Movement::PointsArray const& points)
    {
        constexpr float SUPPORT_DROP = 6.0f;  // PlayerbotAI.cpp GROUND_SUPPORT_DROP
        constexpr float SAMPLE_STEP = 2.0f;   // PlayerbotAI.cpp GROUND_SAMPLE_STEP
        for (size_t i = 1; i < points.size(); ++i)
        {
            G3D::Vector3 const& from = points[i - 1];
            G3D::Vector3 const& to = points[i];
            float const gap = (to - from).length();
            if (from.z - to.z <= SUPPORT_DROP || gap <= 0.0f)
                continue;
            for (float at = SAMPLE_STEP; at < gap; at += SAMPLE_STEP)
            {
                G3D::Vector3 const point = from + (to - from) * (at / gap);
                if (!PlayerbotAI::HasGroundSupport(walker, point.x, point.y, point.z))
                    return true;
            }
        }
        return false;
    }

    // [cap65] the area (or its zone) is the other faction's capital (AREA_FLAG_CAPITAL, the other team's)
    bool EnemyCapitalArea(Player* bot, AreaTableEntry const* entry)
    {
        AreaTableEntry const* zone = entry;
        if (zone && zone->zone)
            if (AreaTableEntry const* parent = sAreaTableStore.LookupEntry(zone->zone))
                zone = parent;
        if (!zone || !(zone->flags & AREA_FLAG_CAPITAL) || !zone->team || zone->team == AREATEAM_ANY)
            return false;
        uint32 const own = bot->GetTeamId() == TEAM_ALLIANCE ? AREATEAM_ALLY : AREATEAM_HORDE;
        return zone->team != own;
    }

    RouteResult RouteSearchUncached(Unit* walker, G3D::Vector3 const& from, G3D::Vector3 const& toAsked);

    // [route102] the same walk asked again within one world tick (one decision asks the route of a trip, its walk,
    // its pricing in turn: TripRefused, MoveFarTo, the planner's leg) is answered from the first search of that
    // tick: the facts it was searched on (the mobs, the walker's spot) are the same tick's. Per thread, a few
    // entries, cleared at each new tick - no answer outlives its tick (route31: a longer-lived cache was a loss)
    RouteResult Route(Unit* walker, G3D::Vector3 const& from, G3D::Vector3 const& toAsked)
    {
        if (!walker || extraAvoid)
            return RouteSearchUncached(walker, from, toAsked);
        struct Entry
        {
            uint64 walker;
            int32 fx, fy, fz, tx, ty, tz;
            RouteResult result;
        };
        thread_local std::vector<Entry> entries;
        thread_local uint64 entriesTick = 0;
        uint64 const tick = uint64(GameTime::GetGameTimeMS().count());
        if (tick != entriesTick)
        {
            entries.clear();
            entriesTick = tick;
        }
        Entry key{walker->GetGUID().GetRawValue(), int32(std::floor(from.x)), int32(std::floor(from.y)),
                  int32(std::floor(from.z)), int32(std::floor(toAsked.x)), int32(std::floor(toAsked.y)),
                  int32(std::floor(toAsked.z)), {}};
        for (Entry const& e : entries)
            if (e.walker == key.walker && e.fx == key.fx && e.fy == key.fy && e.fz == key.fz && e.tx == key.tx &&
                e.ty == key.ty && e.tz == key.tz)
                return e.result;
        key.result = RouteSearchUncached(walker, from, toAsked);
        constexpr size_t TICK_ENTRIES = 16;  // a decision's walks; the cost bound of the lookup
        if (entries.size() >= TICK_ENTRIES)
            entries.erase(entries.begin());
        entries.push_back(key);
        return key.result;
    }

    RouteResult RouteSearchUncached(Unit* walker, G3D::Vector3 const& from, G3D::Vector3 const& toAsked)
    {
        // [perf210] the [static39] search this one's caller started, handed what it computed already (taken here, so
        // a search a judge asks further down does not see it)
        SearchPrep const* prep = handedPrep;
        handedPrep = nullptr;
        if (prep && (prep->walker != walker || prep->from != from || prep->toAsked != toAsked))
            prep = nullptr;
        // [perf210] the profile: the top search and its [static39] search are counted (one line per minute)
        bool const top = routeProfile.depth == 0;
        bool const record = top || prep;
        struct DepthScope
        {
            bool top;
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            DepthScope(bool isTop) : top(isTop) { ++routeProfile.depth; }
            ~DepthScope()
            {
                --routeProfile.depth;
                if (!top)
                    return;
                NoteSearch(MicrosSince(start), routeProfile);
                routeProfile = SearchProfile{};
            }
        } depthScope(top);
        // [perf26] [patrol101] the walker named: a slow search is its bot's (b417's 5.4 s tick had 100 unnamed
        // 200-430 ms searches in one health window). [perf210] The top search only: the searches inside it are in its
        // time (the [static39] one doubled the lines and their sum)
        std::string const walkerName = walker ? walker->GetName() : std::string("-");
        std::optional<SlowScope> slow;
        if (top)
            slow.emplace("route search", &walkerName);
        PMON_SCOPE(routeScope, PERF_MON_VALUE, "leader route search");  // [guild15]
        constexpr float ROUTE_SAMPLE_YD = 4.0f;  // PathGenerator's smooth step: the checks keep their spacing
        // [deaths] user 2026-11-08: a solo bot's walk from where it stands to the target it engages from a pull spot
        // (SoloSafety::Verdict::pullSpot) ends at that spot, round the reaches of the idle mobs the pull must not wake
        G3D::Vector3 to = toAsked;
        std::vector<G3D::Vector4> pullAvoid;
        if (prep)
        {
            to = prep->to;
            pullAvoid = prep->pullAvoid;
        }
        else if (Player* bot = walker ? walker->ToPlayer() : nullptr)
            if (bot->GetExactDist(from.x, from.y, from.z) <= bot->GetCombatReach())
            {
                auto const pullStart = std::chrono::steady_clock::now();
                G3D::Vector3 spot;
                if (SoloSafety::PullWalk(bot, toAsked, spot, pullAvoid))
                    to = spot;
                else
                    pullAvoid.clear();
                if (record)
                    routeProfile.pullUs += MicrosSince(pullStart);
            }
        RouteResult result;
        result.source = "long route";
        Map* map = walker ? walker->GetMap() : nullptr;
        if (!map)
            return result;
        if (!map->GetMapCollisionData().GetMMapData().GetNavMesh())
        {
            // a map without its navmesh: the walk is straight on (as PathGenerator's shortcut was)
            result.source = "no navmesh (straight)";
            result.found = result.complete = true;
            result.points = {from, to};
            result.length = (to - from).length();
            return result;
        }
        Movement::PointsArray corners;
        Movement::PointsArray surface;  // [lead147] the walk on the navmesh's detail height
        // [guild13] a solo questing bot walks round the idle mobs: their reach costs the fight it would start
        // (penalties), a mob it must not fight (orange / red, elite) is refused while a complete walk round exists
        // [perf210] the [static39] search takes the top search's RouteCosts (same walker, walk and tick: the same
        // mobs priced the same; its state was written by that call and is noted again by this search's NoteRoute)
        std::vector<G3D::Vector4> costRefuse;
        std::vector<Travel::RoutePenalty> penalties;
        if (prep)
        {
            costRefuse = prep->costRefuse;
            penalties = prep->penalties;
            ++routeProfile.costsReused;
        }
        else
        {
            auto const costsStart = std::chrono::steady_clock::now();
            SoloSafety::RouteCosts(walker, from, to, costRefuse, penalties);
            if (record)
                routeProfile.costsUs += MicrosSince(costsStart);
        }
        std::vector<G3D::Vector4> refuse = costRefuse;
        refuse.insert(refuse.end(), pullAvoid.begin(), pullAvoid.end());  // [deaths] the pull spot's walk
        // [static39] the walk's stretch beyond the walker's sight priced from the spawn index (a search before found
        // it through them): their reaches are refused as RouteCosts refuses the live ones it sees
        thread_local std::vector<G3D::Vector4> staticRefuse;
        refuse.insert(refuse.end(), staticRefuse.begin(), staticRefuse.end());
        if (extraAvoid)  // [spot70] the caller's reaches (a led member's walk round idle mobs)
            refuse.insert(refuse.end(), extraAvoid->begin(), extraAvoid->end());
        // [guild15] a refused reach the walker stands in already lies behind the walk: it walks out of it.
        // [leave33] user 2026-11-09: out by the nearest way, not on through the camp - the reach is kept, shrunk to
        // the walker's own distance from its centre (less its combat reach, its own polygon stays open): the walk
        // goes no deeper into it than where it stands. Dropped whole only when the destination lies in it too (the
        // camp is the goal) or the walker stands at its centre. b385 Aestymro L13 rose in the Longshore murlocs'
        // reach, its RECOVER walk to its hub (952 yd) went on through the camp, died again 20 yd off
        float const reachOwn = walker->GetCombatReach();
        uint32 shrunk = 0;
        uint64 shrunkKey = 0;  // [leave34] the reaches it stands in, for the log line's dedup
        refuse.erase(std::remove_if(refuse.begin(), refuse.end(),
                                    [&](G3D::Vector4& c)
                                    {
                                        float const inFrom = std::hypot(c.x - from.x, c.y - from.y);
                                        if (inFrom > c.w)
                                            return false;
                                        if (std::hypot(c.x - to.x, c.y - to.y) <= c.w || inFrom - reachOwn <= 1.0f)
                                            return true;
                                        c.w = inFrom - reachOwn;
                                        ++shrunk;
                                        shrunkKey = shrunkKey * 1000003ull + uint64(int64(c.x) * 7919 + int64(c.y));
                                        return false;
                                    }),
                     refuse.end());
        if (shrunk)
        {
            // [leave34] logged once per walker and set of reaches it stands in (b386: 505 lines of Maronic's waits,
            // its spot alternating between a few yards): a log cadence, no decision reads it
            thread_local std::unordered_map<uint64, uint64> leaveLogged;
            uint64 const spot = shrunkKey;
            uint64& last = leaveLogged[walker->GetGUID().GetRawValue()];
            if (last != spot)
            {
                last = spot;
                LOG_INFO("playerbots", "[leave33] {} starts a walk inside {} refused reach(es) at ({:.0f},{:.0f}): "
                         "no deeper into them than it stands",
                         walker->GetName(), shrunk, from.x, from.y);
            }
        }
        // [route31] no walk cache: route28-30 held ~12 % hits (the key holds both ends to half a yard, a walker that
        // moved is a new key) and its stored walks (MBs of points per map thread) slowed every other call: b382 /
        // b384 AI 1.57-2.07 ms per tick against b377's 0.37, trivial values (invalid target, inventory items) 2x
        // [perf210] the unrefused passes' walks, for a [static39] search this one starts
        PassMemo passMemo[2][2];          // [priced][steep]
        bool proven[2] = {false, false};  // the last pass, per steep: no walk of it can be complete ([perf210])
        auto const search =
            [&](std::vector<G3D::Vector4> const* refused, std::vector<Travel::RoutePenalty> const* priced)
        {
            proven[0] = proven[1] = false;
            for (bool const steep : {false, true})
            {
                PassMemo own;
                PassMemo const* reused = !refused && prep ? &prep->passes[priced ? 1 : 0][steep ? 1 : 0] : nullptr;
                if (reused && reused->valid)
                {
                    own = *reused;
                    ++routeProfile.passesReused;
                }
                else
                {
                    Travel::LongRouteInfo info;
                    bool const found = Travel::LongRoute(walker, from, to, steep, own.tried, own.complete, nullptr,
                                                         refused, ROUTE_SAMPLE_YD, &own.triedSurface, priced, &info);
                    own.valid = true;
                    own.found = found && !own.tried.empty();
                    own.noComplete = info.noComplete;
                    if (record)
                    {
                        ++routeProfile.longRoutes;
                        routeProfile.findPaths += info.findPaths;
                        routeProfile.reverse += info.reverse;
                        routeProfile.big += info.big;
                        routeProfile.nodes += info.nodes;
                        routeProfile.findPathUs += info.findPathUs;
                        routeProfile.samplesUs += info.samplesUs;
                        routeProfile.filterTests += info.filterTests;
                        routeProfile.filterMemo += info.filterMemo;
                    }
                    // [deaths3] a walk with a stretch over open air is no walk (b341 Alnia L8 at Teldrassil: "route
                    // complete (558 yd): its leg drops through open air", then WALK_FAIL loops and STUCK)
                    if (own.found)
                    {
                        auto const airStart = std::chrono::steady_clock::now();
                        own.openAir = OverOpenAir(walker, own.triedSurface.empty() ? own.tried : own.triedSurface);
                        if (record)
                            routeProfile.openAirUs += MicrosSince(airStart);
                    }
                    if (!refused)
                        passMemo[priced ? 1 : 0][steep ? 1 : 0] = own;
                }
                proven[steep ? 1 : 0] = own.noComplete;
                if (!own.found || own.openAir)
                    continue;
                if (result.found && !own.complete)
                    continue;
                corners = own.tried;
                surface = own.triedSurface;
                result.found = true;
                result.complete = own.complete;
                if (own.complete)
                    break;
            }
        };
        if (!refuse.empty())
            search(&refuse, &penalties);
        bool roundRefused = refuse.empty() || result.complete;
        // [deaths] a walk the search cannot complete at all (the destination past its node / corridor budget, on
        // another landmass): the priced walk is as complete as the unpriced one, so the priced one is walked. Before,
        // every search came back partial and the last one - no mob priced, none refused - was walked: b337 GO_CAMP
        // walks of 4700-18900 yd went through Dragonmaw Scouts L19 at Dun Algaz (Mutil L12 twice, Keenonk L11), the
        // same Infected Nightstalker Runt L8 twice (Feeli L2, a danger zone remembered), wolves and bears in Dun Morogh
        // (L1-L2 bots). A fallback is taken only when it is complete: then no walk goes round (throughRefused)
        RouteResult pricedResult = result;
        Movement::PointsArray pricedCorners = corners;
        Movement::PointsArray pricedSurface = surface;
        if (!result.complete && !penalties.empty())
        {
            result.found = false;
            search(nullptr, &penalties);
            if (!pricedResult.found && result.found)
            {
                pricedResult = result;
                pricedCorners = corners;
                pricedSurface = surface;
            }
        }
        // [perf210] the priced pass opens the same polygons as the bare one (penalties only cost): when it proved, for
        // both steep filters, that no walk there is complete, the bare pass's walk is partial too and is dropped for
        // the priced one below - not searched (an unreachable destination searched both passes to the big pool)
        bool const bareIsPartial =
            !result.complete && !penalties.empty() && proven[0] && proven[1] && pricedResult.found;
        if (bareIsPartial)
        {
            result.found = false;
            if (record)
                ++routeProfile.pass3Skipped;
        }
        else if (!result.complete)
        {
            result.found = false;
            search(nullptr, nullptr);
        }
        if (!result.complete && pricedResult.found)
        {
            result = pricedResult;
            corners = pricedCorners;
            surface = pricedSurface;
            roundRefused = true;
        }
        SoloSafety::NoteRoute(walker, result.complete, result.found && !roundRefused);
        if (!result.found)
            return result;
        result.throughRefused = !roundRefused;
        if (corners.size() == 1)
        {
            corners.insert(corners.begin(), from);
            surface.insert(surface.begin(), from);
        }
        result.corners.assign(corners.begin(), corners.end());
        for (size_t i = 1; i < corners.size(); ++i)
            result.length += (corners[i] - corners[i - 1]).length();
        // [lead147] the samples stand on the navmesh's detail height (LongRoute); the ground snap is only the
        // [height_gap] cross-check now
        bool const heightGapLog = Travel::HeightGapLogEnabled();
        for (G3D::Vector3 const& point : surface)
        {
            // [lead161] no repeated point (a zero-length segment has no direction for the walk's shortening)
            if (!result.points.empty() && (result.points.back() - point).squaredLength() <= 1e-4f)
                continue;
            if (heightGapLog)  // [guild4] the ground snap feeds only the diagnostic
            {
                G3D::Vector3 ground = point;
                walker->UpdateAllowedPositionZ(ground.x, ground.y, ground.z);
                Travel::NoteHeightGap(walker, ground, point.z, "route sample ground");  // [lead144]
            }
            result.points.push_back(point);
        }
        // [static39] user 2026-11-09: RouteCosts prices only the mobs in sight at the search; the stretch beyond it
        // is priced from the static spawns (each spawn's level, elite rank, aggro reach): when the walk passes such
        // reaches out there, it is searched once more with them refused (b388 Ganna L14: "route complete, Crag Coyote
        // unseen" - the Loch Modan walk round the Stonesplinter walls went through the Badlands)
        if (walker->IsPlayer() && SoloSafety::Applies(walker->ToPlayer()) && result.found && staticRefuse.empty() &&
            result.points.size() >= 2)
        {
            Player* bot = walker->ToPlayer();
            size_t beyond = 0;
            while (beyond < result.points.size() &&
                   (result.points[beyond] - from).length() <= sPlayerbotAIConfig.SightDistance)
                ++beyond;
            std::vector<G3D::Vector3> const walk(result.points.begin(), result.points.end());
            auto const staticStart = std::chrono::steady_clock::now();
            std::vector<G3D::Vector4> const discs = NewRpgBaseAction::StaticDangerDiscs(bot, walk, beyond);
            if (record)
                routeProfile.staticUs += MicrosSince(staticStart);
            // the destination's own reaches are its goal's (the walk ends there), not refused
            std::vector<G3D::Vector4> kept;
            for (G3D::Vector4 const& d : discs)
                if (std::hypot(d.x - to.x, d.y - to.y) > d.w)
                    kept.push_back(d);
            if (!kept.empty())
            {
                staticRefuse = kept;
                // [perf210] the search round them gets what this one computed (the same walker, walk and tick)
                SearchPrep handed;
                handed.walker = walker;
                handed.from = from;
                handed.toAsked = toAsked;
                handed.to = to;
                handed.pullAvoid = std::move(pullAvoid);
                handed.costRefuse = std::move(costRefuse);
                handed.penalties = std::move(penalties);
                for (int priced = 0; priced < 2; ++priced)
                    for (int steep = 0; steep < 2; ++steep)
                        handed.passes[priced][steep] = std::move(passMemo[priced][steep]);
                handedPrep = &handed;
                if (record)
                    ++routeProfile.staticSearches;
                RouteResult again = Route(walker, from, toAsked);
                handedPrep = nullptr;
                staticRefuse.clear();
                if (!again.found)
                    again.refusedArea = true;  // [loop70] no way round the spawns it must not fight
                thread_local std::unordered_map<uint64, size_t> staticLogged;  // a log cadence, no decision reads it
                size_t& last = staticLogged[bot->GetGUID().GetRawValue()];
                if (last != kept.size())
                {
                    last = kept.size();
                    LOG_INFO("playerbots", "[static39] {} L{} walk to ({:.0f},{:.0f}) passes {} reach(es) of unseen "
                             "spawns it must not fight: searched round them ({})",
                             bot->GetName(), bot->GetLevel(), to.x, to.y, kept.size(),
                             again.found ? (again.throughRefused ? "no way round" : "a way round") : "none");
                }
                return again;
            }
        }
        // [zone38] a solo walker's walk through an area far above its level (the area's own level red for it: the
        // Badlands' 35-45 for a level 14) is no walk: its mobs are out of the search's sight (RouteCosts prices the
        // mobs it sees), the detour round a refused reach could lead there. b388 Ganna L14's q2038 sweep walk in Loch
        // Modan went round the Stonesplinter walls through the Badlands ("route complete, Crag Coyote unseen"): 7
        // deaths to L35 coyotes and buzzards. The start's and the destination's own areas are not judged here
        if (walker->IsPlayer() && SoloSafety::Applies(walker->ToPlayer()) && result.points.size() >= 2)
        {
            Player* bot = walker->ToPlayer();
            auto const zoneStart = std::chrono::steady_clock::now();
            struct ZoneTime
            {
                bool record;
                std::chrono::steady_clock::time_point start;
                ~ZoneTime()
                {
                    if (record)
                        routeProfile.zoneUs += MicrosSince(start);
                }
            } zoneTime{record, zoneStart};
            uint32 const phase = bot->GetPhaseMask();
            uint32 const fromArea = map->GetAreaId(phase, from.x, from.y, from.z);
            uint32 const toArea = map->GetAreaId(phase, to.x, to.y, to.z);
            constexpr size_t AREA_SAMPLE_EVERY = 25;  // ~100 yd of 4 yd samples: a look's cost, no rule
            for (size_t i = AREA_SAMPLE_EVERY; i < result.points.size(); i += AREA_SAMPLE_EVERY)
            {
                G3D::Vector3 const& p = result.points[i];
                uint32 const area = map->GetAreaId(phase, p.x, p.y, p.z);
                if (!area || area == fromArea || area == toArea)
                    continue;
                AreaTableEntry const* entry = sAreaTableStore.LookupEntry(area);
                if (!entry)
                    continue;
                bool const red = entry->area_level > 0 &&
                                 Acore::XP::GetColorCode(bot->GetLevel(), uint8(std::min(entry->area_level, 255))) ==
                                     XP_RED;
                // [cap65] ... or an area of the other faction's capital (its guards L75 everywhere, out of the
                // search's sight until met): b400 Feette L19 (Alliance) walked from Durotar into Orgrimmar's north on
                // its way to Ashenvale and stood there walled by Orgrimmar Grunts L75 - 31 dead-end postpones
                if (!red && !EnemyCapitalArea(bot, entry))
                    continue;
                thread_local std::unordered_map<uint64, uint32> zoneLogged;  // a log cadence, no decision reads it
                uint32& last = zoneLogged[bot->GetGUID().GetRawValue()];
                if (last != area)
                {
                    last = area;
                    LOG_INFO("playerbots", "[zone38] {} L{} refuses a walk to ({:.0f},{:.0f}): it passes {} (area level "
                             "{}) at ({:.0f},{:.0f})",
                             bot->GetName(), bot->GetLevel(), to.x, to.y, entry->area_name[0], entry->area_level, p.x,
                             p.y);
                }
                result.found = false;
                result.refusedArea = true;  // [loop70]
                result.complete = false;
                result.points.clear();
                result.corners.clear();
                result.length = 0.0f;
                break;
            }
        }
        return result;
    }

    RouteResult RouteAvoiding(Unit* walker, Position const& to, std::vector<G3D::Vector4> const& avoid)
    {
        std::vector<G3D::Vector4> const* const before = extraAvoid;
        extraAvoid = &avoid;
        RouteResult result = Route(walker, to);
        extraAvoid = before;
        return result;
    }

    RouteResult Route(Unit* walker, Position const& to)
    {
        return Route(walker, G3D::Vector3(walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ()),
                     G3D::Vector3(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ()));
    }

    // [lead804] b221 rfc-h 5A / 5D / 5H: the Ragefire Trogg ramp engage was priced as a 44 yd straight walk, the route
    // up the ramp was 86-130 yd - every walk in the leader's estimates (CornerVsStand, the engage and pull-back walks,
    // GroupWalkTo, the seconds to a boss) read GetExactDist. They read the navmesh walk now
    float WalkYd(Unit* walker, Position const& from, Position const& to)
    {
        float const straight = from.GetExactDist(to);
        Map* map = walker ? walker->GetMap() : nullptr;
        if (!map || straight <= 0.5f || !map->GetMapCollisionData().GetMMapData().GetNavMesh())
            return straight;
        struct Key
        {
            uint32 map;
            uint8 kind;
            int32 bin;
            int32 c[6];
            bool operator==(Key const& o) const
            {
                return map == o.map && kind == o.kind && bin == o.bin && std::equal(c, c + 6, o.c);
            }
        };
        struct KeyHash
        {
            size_t operator()(Key const& k) const
            {
                size_t h = std::hash<uint32>()(k.map) ^ (size_t(k.kind) << 1) ^ (size_t(k.bin) << 3);
                for (int32 v : k.c)
                    h = h * 1000003u ^ std::hash<int32>()(v);
                return h;
            }
        };
        struct Stats
        {
            uint32 routes = 0;
            uint32 longer = 0;
            float worstRatio = 0.0f;
            std::string worst;
            uint32 loggedMs = 0;
        };
        thread_local std::unordered_map<Key, float, KeyHash> cache;
        thread_local Stats stats;
        if (cache.size() > 8192)
            cache.clear();
        float const binYd = std::max(2.0f, straight * 0.05f);
        int32 const bin = int32(std::ceil(std::log2(binYd)));  // the bin size class (a pair keeps one class)
        float const size = std::exp2(float(bin));
        auto const q = [&](float v) { return int32(std::floor(v / size)); };
        Key const key{map->GetId(),
                      uint8(walker->IsPlayer() ? 0 : 1),
                      bin,
                      {q(from.GetPositionX()), q(from.GetPositionY()), q(from.GetPositionZ()), q(to.GetPositionX()),
                       q(to.GetPositionY()), q(to.GetPositionZ())}};
        if (auto const found = cache.find(key); found != cache.end())
            return std::max(found->second, straight);
        RouteResult const route =
            Route(walker, G3D::Vector3(from.GetPositionX(), from.GetPositionY(), from.GetPositionZ()),
                  G3D::Vector3(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ()));
        float length = straight;
        if (route.found && !route.corners.empty())
        {
            G3D::Vector3 const& last = route.corners.back();
            length = route.length +
                     (route.complete ? 0.0f : (G3D::Vector3(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ()) -
                                               last).length());
        }
        length = std::max(length, straight);
        cache.emplace(key, length);
        ++stats.routes;
        if (length > straight + size)
        {
            ++stats.longer;
            if (length / straight > stats.worstRatio)
            {
                stats.worstRatio = length / straight;
                stats.worst = Acore::StringFormat("{} {:.0f} yd for {:.0f} straight ({:.1f},{:.1f},{:.1f} -> "
                                                  "{:.1f},{:.1f},{:.1f})",
                                                  walker->GetName(), length, straight, from.GetPositionX(),
                                                  from.GetPositionY(), from.GetPositionZ(), to.GetPositionX(),
                                                  to.GetPositionY(), to.GetPositionZ());
            }
        }
        uint32 const now = getMSTime();
        if (!stats.loggedMs)
            stats.loggedMs = now;
        else if (getMSTimeDiff(stats.loggedMs, now) >= MINUTE * IN_MILLISECONDS && stats.routes)
        {
            LOG_INFO("playerbots", "[lead804] walk estimates this minute: {} routes, {} longer than straight; the "
                     "most: {}", stats.routes, stats.longer, stats.longer ? stats.worst : std::string("-"));
            stats = Stats{};
            stats.loggedMs = now;
        }
        return length;
    }

    float WalkYd(Unit* walker, Position const& to)
    {
        return walker ? WalkYd(walker, walker->GetPosition(), to) : 0.0f;
    }
}

namespace Leader::Travel
{
    namespace
    {
        // the walk over `order` (+inf when a leg has none)
        double TourCost(std::vector<size_t> const& order, std::vector<std::vector<float>> const& cost)
        {
            double total = 0.0;
            for (size_t i = 1; i < order.size(); ++i)
                total += cost[order[i - 1]][order[i]];
            return total;
        }

        // 2-opt (a reversed stretch) and or-opt (a stretch of 1-3 stops moved, either way round) until neither finds
        // a shorter walk; the start (and with `fixedEnd` the end) stay where they are
        void ImproveTour(std::vector<size_t>& order, std::vector<std::vector<float>> const& cost, bool fixedEnd)
        {
            if (order.size() < 3)
                return;
            size_t const last = fixedEnd ? order.size() - 2 : order.size() - 1;  // the last stop that may move
            if (last < 1)
                return;

            double best = TourCost(order, cost);
            for (bool better = true; better;)
            {
                better = false;
                for (size_t i = 1; i < last; ++i)
                {
                    for (size_t j = i + 1; j <= last; ++j)
                    {
                        std::reverse(order.begin() + i, order.begin() + j + 1);
                        double const changed = TourCost(order, cost);
                        if (changed + 1e-3 < best)
                        {
                            best = changed;
                            better = true;
                        }
                        else
                            std::reverse(order.begin() + i, order.begin() + j + 1);
                    }
                }

                for (size_t span = 1; span <= 3; ++span)
                {
                    for (size_t i = 1; i + span - 1 <= last; ++i)
                    {
                        std::vector<size_t> const stretch(order.begin() + i, order.begin() + i + span);
                        std::vector<size_t> rest(order.begin(), order.begin() + i);
                        rest.insert(rest.end(), order.begin() + i + span, order.end());
                        size_t const restLast = last - span;  // the last movable stop once the stretch is out
                        std::vector<size_t> found;
                        for (size_t at = 1; at <= restLast + 1; ++at)
                        {
                            for (bool const reversed : {false, true})
                            {
                                std::vector<size_t> candidate(rest.begin(), rest.begin() + at);
                                if (reversed)
                                    candidate.insert(candidate.end(), stretch.rbegin(), stretch.rend());
                                else
                                    candidate.insert(candidate.end(), stretch.begin(), stretch.end());
                                candidate.insert(candidate.end(), rest.begin() + at, rest.end());
                                double const changed = TourCost(candidate, cost);
                                if (changed + 1e-3 < best)
                                {
                                    best = changed;
                                    found = std::move(candidate);
                                }
                            }
                        }

                        if (!found.empty())
                        {
                            order = std::move(found);
                            better = true;
                        }
                    }
                }
            }
        }
    }

    std::vector<size_t> ShortestTour(std::vector<std::vector<float>> const& walks, bool fixedEnd, float& length)
    {
        size_t const count = walks.size();
        length = 0.0f;
        if (count < 2)
            return std::vector<size_t>(count, 0);

        // a leg without a walk costs more than all the walks there are together (no inf arithmetic in the sums)
        double finite = 0.0;
        for (auto const& row : walks)
            for (float walk : row)
                if (std::isfinite(walk))
                    finite += walk;
        float const none = float((finite + 1.0) * double(count));
        std::vector<std::vector<float>> cost(walks);
        for (auto& row : cost)
            for (float& walk : row)
                if (!std::isfinite(walk))
                    walk = none;

        size_t const end = count - 1;
        std::vector<size_t> free;
        for (size_t stop = 1; stop < count; ++stop)
            if (!fixedEnd || stop != end)
                free.push_back(stop);

        // nearest neighbour: the next stop is the nearest one left
        std::vector<size_t> nearest{0};
        std::vector<bool> taken(count, false);
        for (size_t step = 0; step < free.size(); ++step)
        {
            size_t next = count;
            for (size_t stop : free)
                if (!taken[stop] && (next == count || cost[nearest.back()][stop] < cost[nearest.back()][next]))
                    next = stop;
            taken[next] = true;
            nearest.push_back(next);
        }
        if (fixedEnd)
            nearest.push_back(end);

        // cheapest insertion into start -> end, the stop farthest from both first
        std::vector<size_t> inserted{0};
        if (fixedEnd)
            inserted.push_back(end);
        auto const nearestEnd = [&](size_t stop)
        { return fixedEnd ? std::min(cost[0][stop], cost[stop][end]) : cost[0][stop]; };
        std::vector<size_t> byFar(free);
        std::sort(byFar.begin(), byFar.end(),
                  [&](size_t left, size_t right) { return nearestEnd(left) > nearestEnd(right); });
        for (size_t stop : byFar)
        {
            size_t const limit = fixedEnd ? inserted.size() - 1 : inserted.size();  // never after the end
            bool placed = false;
            double bestAdd = 0.0;
            size_t bestAt = limit;
            for (size_t at = 1; at <= limit; ++at)
            {
                size_t const before = inserted[at - 1];
                double add = cost[before][stop];
                if (at < inserted.size())
                    add += double(cost[stop][inserted[at]]) - double(cost[before][inserted[at]]);
                if (!placed || add < bestAdd)
                {
                    placed = true;
                    bestAdd = add;
                    bestAt = at;
                }
            }
            inserted.insert(inserted.begin() + bestAt, stop);
        }

        ImproveTour(nearest, cost, fixedEnd);
        ImproveTour(inserted, cost, fixedEnd);
        std::vector<size_t> const& best = TourCost(inserted, cost) < TourCost(nearest, cost) ? inserted : nearest;
        length = float(TourCost(best, walks));  // +inf when even the best walk has a leg without one
        return best;
    }
}