/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"  // math defines first (Position.h)
#include "RouteSearch.h"

#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "LeaderOrders.h"
#include "Log.h"
#include "Map.h"
#include "MapDefines.h"
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
                    if (std::fabs(centre.z - disc.z) <= 10.0f &&
                        (centre.x - disc.x) * (centre.x - disc.x) + (centre.y - disc.y) * (centre.y - disc.y) <=
                            disc.w * disc.w)
                        return false;
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
                   std::vector<G3D::Vector4> const* avoidDiscs, float sampleStep, Movement::PointsArray* samples)
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
        return points.back();
    }

    void NoteHeightGap(Unit* walker, G3D::Vector3 const& point, float beforeSnap, char const* what)
    {
        constexpr int HEIGHT_GAP_NODES = 64;              // findNearestPoly needs no search nodes; init wants some
        constexpr float HEIGHT_GAP_COLUMN = 50.0f;        // the navmesh layers searched above / below the point
        constexpr size_t HEIGHT_GAP_MEMO_MAX = 65536;     // logged cells kept per thread
        constexpr float HEIGHT_GAP_VMAP_UP = 2.0f;        // the floor search starts this far above (Map::GetHeight)
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
        LOG_INFO("playerbots",
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

    RouteResult Route(Unit* walker, G3D::Vector3 const& from, G3D::Vector3 const& to)
    {
        constexpr float ROUTE_SAMPLE_YD = 4.0f;  // PathGenerator's smooth step: the checks keep their spacing
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
        for (bool const steep : {false, true})
        {
            Movement::PointsArray tried;
            Movement::PointsArray triedSurface;
            bool complete = false;
            if (!Travel::LongRoute(walker, from, to, steep, tried, complete, nullptr, nullptr, ROUTE_SAMPLE_YD,
                                   &triedSurface) ||
                tried.empty())
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
        if (!result.found)
            return result;
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
        for (G3D::Vector3 const& point : surface)
        {
            G3D::Vector3 ground = point;
            walker->UpdateAllowedPositionZ(ground.x, ground.y, ground.z);
            Travel::NoteHeightGap(walker, ground, point.z, "route sample ground");  // [lead144]
            result.points.push_back(point);
        }
        return result;
    }

    RouteResult Route(Unit* walker, Position const& to)
    {
        return Route(walker, G3D::Vector3(walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ()),
                     G3D::Vector3(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ()));
    }
}
