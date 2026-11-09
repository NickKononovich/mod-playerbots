/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"  // math defines first (Position.h)
#include "SlowScope.h"  // [perf26]
#include "RouteSearch.h"

#include "DetourCommon.h"
#include "DetourNavMesh.h"
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
#include <cmath>
#include <memory>
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

        // [guild13] an edge from outside a penalty disc into it pays the disc's cost once (Detour: (y, z, x))
        float getCost(float const* pa, float const* pb, dtPolyRef const prevRef, dtMeshTile const* prevTile,
                      dtPoly const* prevPoly, dtPolyRef const curRef, dtMeshTile const* curTile, dtPoly const* curPoly,
                      dtPolyRef const nextRef, dtMeshTile const* nextTile, dtPoly const* nextPoly) const override
        {
            float cost = dtQueryFilter::getCost(pa, pb, prevRef, prevTile, prevPoly, curRef, curTile, curPoly, nextRef,
                                                nextTile, nextPoly);
            if (!penalties)
                return cost;
            // [guild13] the step pa -> pb (Detour's portal midpoints, a polygon apart: outdoors 20-50 yd) enters the
            // disc where its segment comes within the radius, not only where pb lies inside
            float const sx = pb[2] - pa[2];
            float const sy = pb[0] - pa[0];
            float const lengthSq = sx * sx + sy * sy;
            for (RoutePenalty const& disc : *penalties)
            {
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
            if (discs && !discs->empty() && tile && poly && poly->vertCount > 0)
            {
                G3D::Vector3 centre(0.0f, 0.0f, 0.0f);
                for (uint8 index = 0; index < poly->vertCount; ++index)
                {
                    float const* vertex = &tile->verts[size_t(poly->verts[index]) * 3];
                    centre += G3D::Vector3(vertex[2], vertex[0], vertex[1]);  // Detour stores (y, z, x)
                }
                centre /= float(poly->vertCount);
                for (G3D::Vector4 const& disc : *discs)
                {
                    if (std::fabs(centre.z - disc.z) > 10.0f)
                        continue;
                    if ((centre.x - disc.x) * (centre.x - disc.x) + (centre.y - disc.y) * (centre.y - disc.y) <=
                        disc.w * disc.w)
                        return false;
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
                            return false;
                    }
                }
            }
            if (!boxes || boxes->empty() || !tile || !poly || poly->vertCount == 0)
                return true;
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
                if (high.x >= box.low().x && low.x <= box.high().x && high.y >= box.low().y &&
                    low.y <= box.high().y && high.z >= box.low().z && low.z <= box.high().z)
                    return false;
            return true;
        }
    };

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
                   std::vector<RoutePenalty> const* penalties)
    {
        complete = false;
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
        if (!startRef || !endRef)
            return false;

        std::vector<dtPolyRef> polys(LONG_ROUTE_MAX_POLYS);
        int polyCount = 0;
        dtStatus const status = query->findPath(startRef, endRef, startNearest, endNearest, &filter, polys.data(),
                                                &polyCount, LONG_ROUTE_MAX_POLYS);
        if (dtStatusFailed(status) || polyCount <= 0)
            return false;

        complete = polys[size_t(polyCount - 1)] == endRef;
        // The search ran out of nodes: the same search from dest back to the start (SelfTravel::LongRoute's
        // reverse fallback; Wailing Caverns wave 2: the A* from the cliff top floods the plateau towards the goal
        // below and spends its pool, from the goal it leaves the basin at once). Only a complete corridor back to
        // the start's own polygon counts, none through an off-mesh link (they can be one-way). A search that
        // exhausted its part of the navmesh without the nodes running out has no way there in either direction.
        if (!complete && ROUTE_LONG_REVERSE && dtStatusDetail(status, DT_OUT_OF_NODES))
        {
            std::vector<dtPolyRef> back(LONG_ROUTE_MAX_POLYS);
            int backCount = 0;
            bool usable = dtStatusSucceed(query->findPath(endRef, startRef, endNearest, startNearest, &filter,
                                                          back.data(), &backCount, LONG_ROUTE_MAX_POLYS)) &&
                          backCount > 0 && back[size_t(backCount - 1)] == startRef;
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
            std::vector<dtPolyRef> wide(LONG_ROUTE_MAX_POLYS);
            int wideCount = 0;
            if (big && dtStatusSucceed(big->init(navMesh, LONG_ROUTE_BIG_NODES)) &&
                dtStatusSucceed(big->findPath(startRef, endRef, startNearest, endNearest, &filter, wide.data(),
                                              &wideCount, LONG_ROUTE_MAX_POLYS)) &&
                wideCount > 0 && wide[size_t(wideCount - 1)] == endRef)
            {
                polys.swap(wide);
                polyCount = wideCount;
                complete = true;
            }
        }
        float target[3] = {endNearest[0], endNearest[1], endNearest[2]};
        if (!complete)
            query->closestPointOnPoly(polys[size_t(polyCount - 1)], endNearest, target, nullptr);

        std::vector<float> straight(size_t(LONG_ROUTE_MAX_CORNERS) * 3);
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
            samples->clear();
            size_t cursor = 0;
            auto const onSurface = [&](G3D::Vector3& point)
            {
                float const position[3] = {point.y, point.z, point.x};
                for (bool const offMesh : {false, true})
                    for (size_t index = cursor; index < size_t(polyCount); ++index)
                    {
                        dtMeshTile const* tile = nullptr;
                        dtPoly const* poly = nullptr;
                        if (dtStatusFailed(navMesh->getTileAndPolyByRef(polys[index], &tile, &poly)) || !poly ||
                            (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) != offMesh)
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

    RouteResult Route(Unit* walker, G3D::Vector3 const& from, G3D::Vector3 const& toAsked)
    {
        SlowScope slow("route search");  // [perf26]
        PMON_SCOPE(routeScope, PERF_MON_VALUE, "leader route search");  // [guild15]
        constexpr float ROUTE_SAMPLE_YD = 4.0f;  // PathGenerator's smooth step: the checks keep their spacing
        // [deaths] user 2026-11-08: a solo bot's walk from where it stands to the target it engages from a pull spot
        // (SoloSafety::Verdict::pullSpot) ends at that spot, round the reaches of the idle mobs the pull must not wake
        G3D::Vector3 to = toAsked;
        std::vector<G3D::Vector4> pullAvoid;
        if (Player* bot = walker ? walker->ToPlayer() : nullptr)
            if (bot->GetExactDist(from.x, from.y, from.z) <= bot->GetCombatReach())
            {
                G3D::Vector3 spot;
                if (SoloSafety::PullWalk(bot, toAsked, spot, pullAvoid))
                    to = spot;
                else
                    pullAvoid.clear();
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
        std::vector<G3D::Vector4> refuse;
        std::vector<Travel::RoutePenalty> penalties;
        SoloSafety::RouteCosts(walker, from, to, refuse, penalties);
        refuse.insert(refuse.end(), pullAvoid.begin(), pullAvoid.end());  // [deaths] the pull spot's walk
        // [guild15] a refused reach the walker stands in already lies behind the walk: it walks out of it
        refuse.erase(std::remove_if(refuse.begin(), refuse.end(),
                                    [&from](G3D::Vector4 const& c)
                                    { return (c.x - from.x) * (c.x - from.x) + (c.y - from.y) * (c.y - from.y) <=
                                             c.w * c.w; }),
                     refuse.end());
        // [route28] the search's own inputs are its cache key: the map and instance, the walker's kind, both ends (to
        // half a yard) and every refused reach and priced penalty RouteCosts gave (the mobs alive where they stand
        // now, the danger zones): the navmesh is static, so equal inputs give the equal walk. A changed input (a mob
        // moved or died, the walker stepped) is a new key - no clock. b377: 24255 searches, 3.3 ms each, 72.7 % of
        // all AI time, most of them the same walk asked again (IDLE looks, pull spots, rest spots, every leg)
        uint64 cacheKey = 0;
        {
            auto const mix = [&cacheKey](int64 v)
            { cacheKey = (cacheKey ^ uint64(v)) * 1099511628211ull + 0x9E3779B97F4A7C15ull; };
            auto const q = [](float v) { return int64(std::floor(v * 2.0f)); };
            mix(map->GetId());
            mix(map->GetInstanceId());
            mix(walker->IsPlayer() ? 1 : 2);
            for (float v : {from.x, from.y, from.z, to.x, to.y, to.z})
                mix(q(v));
            mix(int64(refuse.size()));
            for (G3D::Vector4 const& c : refuse)
                for (float v : {c.x, c.y, c.z, c.w})
                    mix(q(v));
            mix(int64(penalties.size()));
            for (Travel::RoutePenalty const& pen : penalties)
                for (float v : {pen.x, pen.y, pen.z, pen.r, pen.cost})
                    mix(q(v));
        }
        thread_local std::unordered_map<uint64, RouteResult> routeCache;
        thread_local uint64 routeCacheHits = 0;
        constexpr size_t ROUTE_CACHE_ENTRIES = 4096;  // a memory bound per thread (no age rule)
        if (auto const hit = routeCache.find(cacheKey); hit != routeCache.end())
        {
            ++routeCacheHits;
            SoloSafety::NoteRoute(walker, hit->second.complete, hit->second.found && hit->second.throughRefused);
            return hit->second;
        }
        struct StoreOnExit
        {
            std::unordered_map<uint64, RouteResult>& cache;
            uint64 key;
            RouteResult const& value;
            ~StoreOnExit()
            {
                if (cache.size() >= ROUTE_CACHE_ENTRIES)
                {
                    LOG_INFO("playerbots", "[route28] route cache full ({} walks): cleared, {} searches answered "
                             "from it since its last clear", cache.size(), routeCacheHits);
                    cache.clear();
                    routeCacheHits = 0;
                }
                cache[key] = value;
            }
        } storeOnExit{routeCache, cacheKey, result};
        auto const search = [&](std::vector<G3D::Vector4> const* refused,
                                std::vector<Travel::RoutePenalty> const* priced)
        {
            for (bool const steep : {false, true})
            {
                Movement::PointsArray tried;
                Movement::PointsArray triedSurface;
                bool complete = false;
                if (!Travel::LongRoute(walker, from, to, steep, tried, complete, nullptr, refused, ROUTE_SAMPLE_YD,
                                       &triedSurface, priced) ||
                    tried.empty())
                    continue;
                // [deaths3] a walk with a stretch over open air is no walk (b341 Alnia L8 at Teldrassil: "route
                // complete (558 yd): its leg drops through open air", then WALK_FAIL loops and STUCK)
                if (OverOpenAir(walker, triedSurface.empty() ? tried : triedSurface))
                    continue;
                if (result.found && !complete)
                    continue;
                corners = tried;
                surface = triedSurface;
                result.found = true;
                result.complete = complete;
                if (complete)
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
        if (!result.complete)
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