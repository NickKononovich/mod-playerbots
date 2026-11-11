/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SelfTravelActions.h"
#include "BotPath.h"  // [lead109]

#include "DBCStores.h"
#include "Event.h"
#include "LeaderOrders.h"  // [lead1050] Leader::Route
#include "Map.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SoloSafety.h"  // [deaths146]
#include "SpellMgr.h"
#include "TerrainHazard.h"  // [b44_strat] PathThroughClosedDoor for the walk inside to an exact goal
#include "Timer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>  // [b44_st]
#include <vector>

namespace
{
    // A goal with exact = true counts as reached this close (2D) to its arrival point.
    constexpr float GOAL_REACHED_RANGE = 40.0f;
    // One walk of the portal route: the PathGenerator smooth path stops at MAX_POINT_PATH_LENGTH points, so a long
    // corpse run is walked leg by leg along the corner path.
    constexpr float PORTAL_LEG_STEP = 60.0f;
    // Where the navmesh ends short of the portal (a ramp above it, a portal floating over the floor): the last yards
    // straight, over ground for the living.
    constexpr float PORTAL_STRAIGHT_RANGE = 40.0f;
    // A point inside the trigger reached by a complete walk (FindEntryPoint): searched within this of the portal, on
    // rings of these shares of its radius, again after ENTRY_RECHECK_MS.
    constexpr float ENTRY_SEARCH_RANGE = 80.0f;
    constexpr std::array<float, 2> ENTRY_RING_SHARES = {0.6f, 0.9f};
    constexpr uint32 ENTRY_DIRECTIONS = 8;
    constexpr uint32 ENTRY_RECHECK_MS = 15 * IN_MILLISECONDS;
    // A step off a navmesh island (HopOut): these lengths, these directions, at most this height, and its walk must end
    // this much nearer the portal than the walk from here (or be complete).
    constexpr std::array<float, 2> HOP_DISTANCES = {5.0f, 8.0f};
    constexpr uint32 HOP_DIRECTIONS = 8;
    constexpr float HOP_MAX_DZ = 3.0f;
    constexpr float HOP_LOS_UP = 1.0f;
    constexpr float HOP_MIN_GAIN = 10.0f;
    constexpr float ROUTE_GAIN = 2.0f;  // a partial route must end at least this much nearer to count
    constexpr float FLOOR_SNAP_UP = 3.0f;
    constexpr float FLOOR_SNAP_RANGE = 10.0f;
    // Swim legs (SwimTowards): just under the surface while crossing, straight down within this 2D distance.
    constexpr float SWIM_UNDER_SURFACE = 1.5f;
    constexpr float SWIM_DIVE_RANGE = 6.0f;
    constexpr float SWIM_LEG_STEP = 40.0f;
    // The straight walk into a portal aims a little above its centre (a centre on the floor is out of sight).
    constexpr float PORTAL_AIM_UP = 1.5f;
    constexpr uint32 PORTAL_REENTER_MS = 5 * IN_MILLISECONDS;
    // No progress of 3 yd for this long on a walk: no walk (a ghost takes the spirit healer, the living fly).
    constexpr float PROGRESS_RANGE = 3.0f;
    constexpr uint32 WALK_STUCK_MS = 60 * IN_MILLISECONDS;
    // No gain of 3 yd on the distance to the portal for this long is no walk either (moving in circles: Razorfen
    // Kraul / Deadmines wave 13, ghosts paced 15-40 yd under the entrance for 20 minutes, each step "progress").
    constexpr uint32 WALK_NO_GAIN_MS = 3 * MINUTE * IN_MILLISECONDS;
    // Flight to a portal (Tempest Keep's float above Netherstorm): cruise above the higher of start and portal,
    // descend at a point beside it - Mechanar has a walkable roof right above its portal.
    constexpr float FLY_CRUISE_HEIGHT = 30.0f;
    constexpr float FLY_ARRIVE_DISTANCE = 6.0f;
    constexpr float FLY_SIDE_DISTANCE = 25.0f;
    constexpr float FLY_APPROACH_ABOVE = 2.0f;
    // A portal in a cave under the terrain (Drak'Tharon Keep: its trigger at z 235 under a cliff at 264; a flyer
    // cannot sink below the ground there and hung above it): the approach is a navmesh floor near the portal's
    // height, out in the open within these rings, seen from cruise height and seeing the portal.
    constexpr std::array<float, 4> FLY_OPEN_RINGS = {12.0f, 20.0f, 30.0f, 40.0f};
    constexpr uint32 FLY_OPEN_DIRECTIONS = 16;
    constexpr float FLY_OPEN_FLOOR_HALF_HEIGHT = 10.0f;
    constexpr float FLY_OPEN_MAX_DZ = 10.0f;
    constexpr uint32 MOUNT_CAST_WAIT_MS = 4 * IN_MILLISECONDS;
    constexpr uint32 MOUNT_RETRY_MS = 30 * IN_MILLISECONDS;
    // Breath (Player::HandleDrowning): up at 40 %, down again from 90 %, refilled ten times as fast above water.
    constexpr int32 BREATH_SURFACE_PCT = 40;
    constexpr int32 BREATH_RESUME_PCT = 90;
    constexpr int32 BREATH_REFILL_RATE = 10;
    constexpr uint32 BREATH_SAMPLE_MAX_MS = 5 * IN_MILLISECONDS;
    constexpr float SURFACE_ABOVE_LEVEL = 0.5f;
    constexpr float DIVE_DEPTH = 2.0f;
    // The long route (SelfTravel::LongRoute). PathGenerator's corridor stops at MAX_PATH_LENGTH (148) polygons and its
    // search at MMapMgr's 1024 nodes, and then returns a partial path to the explored node nearest the portal as the
    // crow flies: in a pit or round an island a dead end. Old Kingdom wave 16: five ghosts circled 70-120 yd above the
    // Ahn'kahet portal for 20 minutes (graveyard -> portal is 576 yd and 155 polygons round the pit); Scholomance wave
    // 17: the Caer Darrow graveyard is 146 yd from the keep's gate in a straight line, 707 yd and 149 polygons round
    // the island (tools/navmesh_probe.py: both connected). An own query: a larger node pool and corridor, corners.
    // Blackrock Mountain (lbrs-n / ubrs-n wave 26): graveyard 1469 -> Blackrock Spire portal is ~1230 yd and 311
    // polygons up the ramps inside the mountain; the polygons under the portal are a 12-polygon island of their own
    // (2.9 yd from the ramp's last one), so the search never ends at the goal and runs until its pool is spent.
    // A Detour-like replay of the search over the mmtiles reaches the ramp's end with 16384 nodes from the graveyard,
    // not from the ghosts' dead end under the portal (-7542,-1397,190; 65535 do): the pool is Detour's maximum
    // (dtNodeIndex is 16 bit). Only a ghost or bot without a PathGenerator walk searches, and a found route is kept.
    constexpr int LONG_ROUTE_NODES = 65535;
    constexpr int LONG_ROUTE_MAX_POLYS = 1024;
    constexpr int LONG_ROUTE_MAX_CORNERS = 256;
    constexpr uint32 LONG_ROUTE_RECHECK_MS = 20 * IN_MILLISECONDS;
    constexpr float LONG_ROUTE_OFF_RANGE = 15.0f;  // farther than this from the route: searched again
    // A partial route whose end lies this much beyond the trigger's own reach still counts (the last yards are the
    // straight walk of WalkTowards).
    constexpr float LONG_ROUTE_REACH_MARGIN = 2.0f;
    // PathGenerator::CreateFilter for a headless bot: water and the 50-60 degree slopes cost 20.
    constexpr float LONG_ROUTE_AREA_COST = 20.0f;
    // A RouteWalk goal this close to the last one is the same goal (NextRouteLeg).
    constexpr float ROUTE_SAME_GOAL_RANGE = 2.0f;
    // A route leg's floor: the navmesh polygon this far above or below the interpolated point, this far aside.
    constexpr float ROUTE_FLOOR_HALF_HEIGHT = 12.0f;
    constexpr float NAVMESH_FLOOR_HALF_WIDTH = 2.0f;
    // [b32_bfd] A gap between the bot's walk and the portal's navmesh pocket (FindBridge). Blackfathom Deeps: the
    // polygons round the underwater portal at Zoram Strand (radius 12, z -23) belong to a cave region of ~2600
    // polygons (z -30..-1.5) whose lake surface (z -17.3) lies 9.2 yd from the shore region's lake surface
    // (tools/navmesh_probe.py, map 1 tile 18,31); PathGenerator's partial walk took the ghost to the cliff top 111 yd
    // above the portal (4202.7,625.9,88.2), where it stood "no walk to the portal" for good. The pocket: what a walk
    // reaches within BRIDGE_POCKET_RADIUS from the polygons in the trigger's reach (a region larger than
    // BRIDGE_MAX_POCKET_POLYS is the open world, no pocket). A gap: at most BRIDGE_MAX_GAP across and BRIDGE_MAX_DZ
    // up or down, in sight over it, or under water at one of BRIDGE_DIVE_DEPTHS below both ends (a wall above the
    // water); a living bot crosses over water only. The shore end must be on a complete walk from the bot.
    constexpr float BRIDGE_POCKET_RADIUS = 300.0f;
    constexpr int BRIDGE_MAX_POCKET_POLYS = 8192;
    constexpr int BRIDGE_START_POLYS = 64;
    constexpr int BRIDGE_NEAR_POLYS = 128;
    constexpr float BRIDGE_MAX_GAP = 15.0f;
    constexpr float BRIDGE_MAX_DZ = 4.0f;
    constexpr std::array<float, 3> BRIDGE_DIVE_DEPTHS = {2.5f, 5.0f, 8.0f};
    constexpr float BRIDGE_LOS_UP = 1.0f;
    constexpr size_t BRIDGE_MAX_TRIES = 6;       // gaps whose crossing is checked, narrowest first
    constexpr size_t BRIDGE_MAX_ROUTES = 3;      // of those, shore walks searched (LongRoute)
    constexpr float BRIDGE_SAME_SHORE = 4.0f;    // a gap whose shore end lies this close to a tried one is the same
    constexpr float BRIDGE_SHORE_REACH = 2.0f;   // the walk to the shore may end this short of it
    constexpr float BRIDGE_SHORE_ARRIVE = 3.0f;  // at the shore: the crossing starts
    constexpr float BRIDGE_LEG_ARRIVE = 1.5f;
    constexpr uint32 BRIDGE_RECHECK_MS = 60 * IN_MILLISECONDS;  // searched again this long after none was found
    // [b40_brd] A ghost's drop into the pocket: a polygon of the walk at most GHOST_DROP_MAX_ACROSS aside of a pocket
    // polygon and up to GHOST_DROP_MAX_DZ above it, open air in between (a ghost takes no fall damage). Blackrock
    // Mountain, brd-h 20261026-060218-brd-h-5F-5: the healer died at Gerstahn (492 s), its ghost walked from the
    // graveyard and stood on a 29-polygon island at (-7541.3,-1397.2,190.9), "no walk to the portal (trigger 1466,
    // 586 yd)", until the run ended. tools/navmesh_probe.py (map 0): the BRD entrance floor round the portal (2002
    // polygons, z 164-184) is joined to nothing; the walk from the graveyard (80118 polygons) reaches the chain level
    // right above it, e.g. the edge (-7251.6,-940.5,247.2) over the floor at (-7249.6,-938.3,169.1), 73 yd from the
    // portal (78 yd down); no level gap within 15 yd (the nearest on the same level is 20 yd, steep-only).
    constexpr float GHOST_DROP_MAX_DZ = 100.0f;
    constexpr float GHOST_DROP_MAX_ACROSS = 6.0f;
    constexpr float GHOST_DROP_MIN_ACROSS = 1.0f;  // nearer is a floor right over the pocket, no drop past its edge
    // [b44_st] A gap or drop whose shore polygon lies on a closed navmesh island (its whole region is at most this many
    // polygons and holds not the bot) leads nowhere: skipped before it spends a try. Sunken Temple, st-h
    // 20261026-075135-st-h-5F-7: the druid's ghost swam over the temple (-10162,-4015,18.7), 128 yd above portal 446,
    // "no walk to the portal, no flight from here"; its pocket (3222 polygons, z -113..-38) had 31126 gaps and 26815
    // drops, the narrowest all onto the 2-83 polygon stair islands at z -55..-110 (tools/navmesh_probe.py --regions),
    // and the 6 tries were spent there. The way a player takes - down through the Pool of Tears onto the temple's top
    // at (-10250,-3868,-38.2), 56 yd under the lake surface - is a ghost drop from a water polygon, whose surface
    // lies right over the pocket floor (no edge): a water shore counts below GHOST_DROP_MIN_ACROSS too.
    constexpr int BRIDGE_ISLAND_MAX_POLYS = 1024;
    constexpr float BRIDGE_ISLAND_RADIUS = 5000.0f;
    // A query of its own whose node pool runs out soon after BRIDGE_ISLAND_MAX_POLYS: an open region costs no more
    // than that to tell apart (the main query's 65535-node pool would flood the open world).
    constexpr int BRIDGE_ISLAND_NODES = 4096;

    struct NavMeshQueryFree
    {
        void operator()(dtNavMeshQuery* query) const { dtFreeNavMeshQuery(query); }
    };

    float PolylineLength(Movement::PointsArray const& points)
    {
        float length = 0.0f;
        for (size_t index = 1; index < points.size(); ++index)
            length += (points[index] - points[index - 1]).length();
        return length;
    }

    // The point of a polyline nearest to from, and its segment. 3D: a route up the ramps inside a mountain passes
    // over itself (Blackrock Mountain: the ramp at z 251 runs under the one at z 273), the 2D nearest point lies on
    // the other level and the walk went back down a level.
    G3D::Vector3 NearestOnPolyline(Movement::PointsArray const& points, G3D::Vector3 const& from, size_t& segment,
                                   float& distance)
    {
        G3D::Vector3 best = points.front();
        segment = 0;
        distance = FLT_MAX;
        for (size_t index = 1; index < points.size(); ++index)
        {
            G3D::Vector3 const a = points[index - 1];
            G3D::Vector3 const ab = points[index] - a;
            float const squared = ab.squaredLength();
            float share = squared > 0.0f ? (from - a).dot(ab) / squared : 0.0f;
            share = std::clamp(share, 0.0f, 1.0f);
            G3D::Vector3 const point = a + ab * share;
            float const gap = (point - from).length();
            if (gap < distance)
            {
                distance = gap;
                best = point;
                segment = index - 1;
            }
        }
        return best;
    }

    std::mutex selfTravelLock;
    std::unordered_map<ObjectGuid, SelfTravel::PortalGoal> portalGoals;
    std::unordered_map<ObjectGuid, SelfTravel::MoveReport> moveReports;

    bool IsDungeonMap(uint32 mapId)
    {
        MapEntry const* entry = sMapStore.LookupEntry(mapId);
        return entry && entry->IsDungeon();
    }

    int32 BreathMaxMs(Player* bot)
    {
        if (bot->HasWaterBreathingAura() ||
            bot->GetSession()->GetSecurity() >= AccountTypes(sWorld->getIntConfig(CONFIG_DISABLE_BREATHING)))
            return 0;
        float const multiplier = bot->GetTotalAuraMultiplier(SPELL_AURA_MOD_WATER_BREATHING);
        return static_cast<int32>(static_cast<float>(sWorld->getIntConfig(CONFIG_WATER_BREATH_TIMER)) * multiplier);
    }

    // The point step yd along a polyline (its last point when shorter).
    G3D::Vector3 PointAlong(Movement::PointsArray const& points, float step)
    {
        float walked = 0.0f;
        for (size_t index = 1; index < points.size(); ++index)
        {
            G3D::Vector3 const segment = points[index] - points[index - 1];
            float const length = segment.length();
            if (length > 0.0f && walked + length >= step)
                return points[index - 1] + segment * ((step - walked) / length);
            walked += length;
        }
        return points.back();
    }

    // How far from its centre a trigger takes the bot: its radius, half the diagonal of a box.
    float PortalReach(AreaTrigger const* portal)
    {
        if (portal->radius > 0.0f)
            return portal->radius;
        return 0.5f * std::hypot(portal->length, portal->width);
    }
}

namespace SelfTravel
{
    void SetPortalGoal(ObjectGuid bot, PortalGoal const& goal)
    {
        std::lock_guard<std::mutex> lock(selfTravelLock);
        portalGoals[bot] = goal;
    }

    void ClearPortalGoal(ObjectGuid bot)
    {
        std::lock_guard<std::mutex> lock(selfTravelLock);
        portalGoals.erase(bot);
    }

    bool GetPortalGoal(ObjectGuid bot, PortalGoal& goal)
    {
        std::lock_guard<std::mutex> lock(selfTravelLock);
        auto const itr = portalGoals.find(bot);
        if (itr == portalGoals.end())
            return false;
        goal = itr->second;
        return true;
    }

    void ReportCannotMove(Player* bot, char const* action, char const* reason, float toX, float toY, float toZ)
    {
        if (!bot)
            return;
        MoveReport report;
        report.action = action;
        report.reason = reason;
        report.mapId = bot->GetMapId();
        report.x = bot->GetPositionX();
        report.y = bot->GetPositionY();
        report.z = bot->GetPositionZ();
        report.toX = toX;
        report.toY = toY;
        report.toZ = toZ;
        std::lock_guard<std::mutex> lock(selfTravelLock);
        moveReports[bot->GetGUID()] = report;
    }

    bool TakeReport(ObjectGuid bot, MoveReport& report)
    {
        std::lock_guard<std::mutex> lock(selfTravelLock);
        auto const itr = moveReports.find(bot);
        if (itr == moveReports.end())
            return false;
        report = itr->second;
        moveReports.erase(itr);
        return true;
    }

    void ClearAll()
    {
        std::lock_guard<std::mutex> lock(selfTravelLock);
        portalGoals.clear();
        moveReports.clear();
    }

    bool LongRoute(Player* bot, G3D::Vector3 const& dest, Movement::PointsArray& corners, float reach)
    {
        corners.clear();
        if (!bot || !bot->IsInWorld())
            return false;
        dtNavMesh const* navMesh = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMesh();
        if (!navMesh)
            return false;
        // An own query per search (the map's query belongs to its PathGenerators): it holds only the navmesh pointer
        // and its node pool, the search runs on the bot's map thread like PathGenerator's.
        std::unique_ptr<dtNavMeshQuery, NavMeshQueryFree> query(dtAllocNavMeshQuery());
        if (!query || dtStatusFailed(query->init(navMesh, LONG_ROUTE_NODES)))
            return false;

        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
        filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));
        filter.setAreaCost(NAV_WATER, LONG_ROUTE_AREA_COST);
        filter.setAreaCost(NAV_GROUND_STEEP, LONG_ROUTE_AREA_COST);

        // Detour works in (y, z, x).
        float const startPoint[3] = {bot->GetPositionY(), bot->GetPositionZ(), bot->GetPositionX()};
        float const endPoint[3] = {dest.y, dest.z, dest.x};
        auto nearestPoly = [&query, &filter](float const* point, float* nearest)
        {
            // PathGenerator::GetPolyByLocation: a low box first, then a tall one.
            float extents[3] = {3.0f, 5.0f, 3.0f};
            dtPolyRef ref = 0;
            if (dtStatusSucceed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) && ref)
                return ref;
            extents[1] = 50.0f;
            ref = 0;
            if (dtStatusSucceed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) && ref)
                return ref;
            return dtPolyRef(0);
        };
        float startNearest[3];
        float endNearest[3];
        dtPolyRef const startRef = nearestPoly(startPoint, startNearest);
        dtPolyRef const endRef = nearestPoly(endPoint, endNearest);
        if (!startRef || !endRef)
            return false;

        std::vector<dtPolyRef> polys(LONG_ROUTE_MAX_POLYS);
        int polyCount = 0;
        bool found = dtStatusSucceed(query->findPath(startRef, endRef, startNearest, endNearest, &filter,
                                                     polys.data(), &polyCount, LONG_ROUTE_MAX_POLYS)) &&
                     polyCount > 0;
        if (found && polys[size_t(polyCount - 1)] != endRef)
        {
            // Only a complete route is worth the walk, a partial one is the same dead end - unless it ends within
            // reach of dest: the goal polygon is an island of its own (Blackrock Spire's portal) and Detour's partial
            // result is the explored polygon nearest to it, the ramp's last one inside the trigger.
            float closest[3];
            if (reach > 0.0f &&
                dtStatusSucceed(query->closestPointOnPoly(polys[size_t(polyCount - 1)], endPoint, closest, nullptr)) &&
                (G3D::Vector3(closest[2], closest[0], closest[1]) - dest).length() <= reach)
                std::copy(closest, closest + 3, endNearest);
            else
                found = false;
        }
        if (!found)
        {
            // The same search from dest back to the bot. Wailing Caverns wave 2: the portal lies in the oasis 70 yd
            // below the graveyard's cliff, the walk is ~290 polygons round the rim and down the ravine; the A* from
            // the cliff top floods the Barrens plateau towards the portal below and spends the whole node pool
            // (~80000 nodes needed), its partial result is the rim right above the portal, where the ghosts stood.
            // From the portal the search leaves the basin by the ravine at once (~9000 nodes). Only a complete
            // corridor back to the bot's own polygon counts, and none through an off-mesh link (they can be one-way).
            int backCount = 0;
            if (dtStatusFailed(query->findPath(endRef, startRef, endNearest, startNearest, &filter, polys.data(),
                                               &backCount, LONG_ROUTE_MAX_POLYS)) ||
                backCount <= 0 || polys[size_t(backCount - 1)] != startRef)
                return false;
            std::reverse(polys.begin(), polys.begin() + backCount);
            for (int index = 0; index < backCount; ++index)
            {
                dtMeshTile const* tile = nullptr;
                dtPoly const* poly = nullptr;
                if (dtStatusFailed(navMesh->getTileAndPolyByRef(polys[size_t(index)], &tile, &poly)) || !poly ||
                    poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                    return false;
            }
            polyCount = backCount;
        }

        std::vector<float> straight(size_t(LONG_ROUTE_MAX_CORNERS) * 3);
        int cornerCount = 0;
        if (dtStatusFailed(query->findStraightPath(startNearest, endNearest, polys.data(), polyCount, straight.data(),
                                                   nullptr, nullptr, &cornerCount, LONG_ROUTE_MAX_CORNERS)) ||
            cornerCount < 2)
            return false;

        corners.reserve(size_t(cornerCount));
        for (int index = 0; index < cornerCount; ++index)
            corners.emplace_back(straight[size_t(index) * 3 + 2], straight[size_t(index) * 3],
                                 straight[size_t(index) * 3 + 1]);
        return true;
    }

    bool NextRouteLeg(Player* bot, RouteWalk& walk, G3D::Vector3 const& goal, float reach, float legStep,
                      G3D::Vector3& leg)
    {
        if (!bot || !bot->IsInWorld())
            return false;

        uint32 const now = getMSTime();
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        bool const sameGoal = walk.searchedMs && walk.mapId == bot->GetMapId() &&
                              (walk.goal - goal).length() <= ROUTE_SAME_GOAL_RANGE;
        size_t segment = 0;
        float off = FLT_MAX;
        G3D::Vector3 onRoute = here;
        if (sameGoal && walk.found && walk.corners.size() >= 2)
            onRoute = NearestOnPolyline(walk.corners, here, segment, off);

        // A found route is kept while the bot is on it (a search may spend the whole node pool).
        bool const onIt = sameGoal && walk.found && off <= LONG_ROUTE_OFF_RANGE;
        bool const recent = sameGoal && getMSTimeDiff(walk.searchedMs, now) < LONG_ROUTE_RECHECK_MS;
        if (!onIt && !recent)
        {
            walk.goal = goal;
            walk.mapId = bot->GetMapId();
            walk.searchedMs = now ? now : 1;
            walk.found = LongRoute(bot, goal, walk.corners, reach);
            off = FLT_MAX;
            segment = 0;
            if (walk.found && walk.corners.size() >= 2)
                onRoute = NearestOnPolyline(walk.corners, here, segment, off);
        }
        if (!walk.found || walk.corners.size() < 2 || off > LONG_ROUTE_OFF_RANGE)
            return false;

        Movement::PointsArray rest;
        rest.reserve(walk.corners.size() - segment);
        rest.push_back(onRoute);
        for (size_t index = segment + 1; index < walk.corners.size(); ++index)
            rest.push_back(walk.corners[index]);

        leg = PointAlong(rest, legStep);
        // Between two corners the height is interpolated: on a stair it lies under the steps (Sunken Temple), the
        // floor search from above it finds the floor below them, the navmesh polygon there gives the right one.
        if (!bot->Unit::IsInWater())
        {
            float floor = 0.0f;
            if (NavmeshFloor(bot, leg, ROUTE_FLOOR_HALF_HEIGHT, floor))
                leg.z = floor;
            else
            {
                floor = bot->GetMapHeight(leg.x, leg.y, leg.z + FLOOR_SNAP_UP);
                if (floor > INVALID_HEIGHT && std::fabs(floor - leg.z) <= FLOOR_SNAP_RANGE)
                    leg.z = floor;
            }
        }
        return true;
    }

    bool NavmeshFloor(Player* bot, G3D::Vector3 const& point, float halfHeight, float& floorZ)
    {
        if (!bot || !bot->GetMap())
            return false;

        // The map's own query: findNearestPoly needs no node pool, and the bot runs on its map's thread.
        dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
        if (!query)
            return false;

        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
        filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));
        // Detour works in (y, z, x).
        float const center[3] = {point.y, point.z, point.x};
        float const extents[3] = {NAVMESH_FLOOR_HALF_WIDTH, halfHeight, NAVMESH_FLOOR_HALF_WIDTH};
        float nearest[3];
        dtPolyRef ref = 0;
        if (dtStatusFailed(query->findNearestPoly(center, extents, &filter, &ref, nearest)) || !ref)
            return false;

        floorZ = nearest[1];
        return true;
    }

    // [b34_mech3] the step off a navmesh part (IslandStepDown, [lead1050] PartStepToward)
    constexpr int ISLAND_QUERY_NODES = 2048;
    constexpr float STEP_DOWN_REACH = 8.0f;      // across (2D) from the bot to the landing
    constexpr float STEP_DOWN_MAX_DROP = 13.0f;  // under LEDGE_FALL_SAFE (13.4): no fall damage
    constexpr float STEP_DOWN_MAX_RISE = 1.0f;   // a gap at about the same height counts too
    constexpr float STAND_ON_POLY_DZ = 2.0f;     // the bot's own polygon: this close under / over it
    constexpr int STEP_DOWN_NEAR_POLYS = 64;
    constexpr size_t STEP_DOWN_TRIES = 6;        // candidates (nearest first) whose part is measured

    // [b34_mech3] See the declaration. The navmesh's own connectivity (bot filter: ground and water, no steep).
    bool IslandStepDown(Player* bot, G3D::Vector3& landing)
    {
        constexpr int ISLAND_MAX_POLYS = 48;  // a part this small (all of it) is an island
        constexpr float ISLAND_SEARCH_RADIUS = 200.0f;

        Map* map = bot ? bot->GetMap() : nullptr;
        dtNavMesh const* navMesh = map ? map->GetMapCollisionData().GetMMapData().GetNavMesh() : nullptr;
        if (!navMesh || bot->IsFlying() || bot->Unit::IsInWater() || bot->GetTransport())
            return false;
        // One query per navmesh and thread (the node pool is the query's own; map threads run bots in parallel).
        thread_local std::unordered_map<dtNavMesh const*, std::unique_ptr<dtNavMeshQuery, NavMeshQueryFree>> queries;
        auto& query = queries[navMesh];
        if (!query)
        {
            query.reset(dtAllocNavMeshQuery());
            if (!query || dtStatusFailed(query->init(navMesh, ISLAND_QUERY_NODES)))
            {
                query.reset();
                return false;
            }
        }
        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER));
        filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));

        // Detour works in (y, z, x).
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        float const hereDetour[3] = {here.y, here.z, here.x};
        float const standExtents[3] = {1.0f, STAND_ON_POLY_DZ, 1.0f};
        float onPoly[3];
        dtPolyRef hereRef = 0;
        if (dtStatusFailed(query->findNearestPoly(hereDetour, standExtents, &filter, &hereRef, onPoly)) || !hereRef)
            return false;  // off the navmesh: JudgeLedgeDrop's "stranded" perch case

        // The size of a navmesh part: every polygon a walk reaches from ref, up to ISLAND_MAX_POLYS.
        std::array<dtPolyRef, ISLAND_MAX_POLYS> part{};
        auto const partSize = [&](dtPolyRef ref, float const* at, bool keep) -> int
        {
            std::array<dtPolyRef, ISLAND_MAX_POLYS> found{};
            int count = 0;
            dtStatus const status = query->findPolysAroundCircle(ref, at, ISLAND_SEARCH_RADIUS, &filter, found.data(),
                                                                 nullptr, nullptr, &count, ISLAND_MAX_POLYS);
            if (dtStatusFailed(status))
                return -1;
            if (keep)
                part = found;
            if (dtStatusDetail(status, DT_BUFFER_TOO_SMALL) || dtStatusDetail(status, DT_OUT_OF_NODES))
                return ISLAND_MAX_POLYS;
            return count;
        };
        int const ownSize = partSize(hereRef, onPoly, true);
        if (ownSize < 0 || ownSize >= ISLAND_MAX_POLYS)
            return false;  // not on an island
        auto const inOwnPart = [&](dtPolyRef ref)
        { return std::find(part.begin(), part.begin() + ownSize, ref) != part.begin() + ownSize; };

        // Polygons of another part below (or level with) the bot, within STEP_DOWN_REACH across.
        float const boxMiddleZ = here.z - (STEP_DOWN_MAX_DROP - STEP_DOWN_MAX_RISE) * 0.5f;
        float const boxCenter[3] = {here.y, boxMiddleZ, here.x};
        float const boxExtents[3] = {STEP_DOWN_REACH, (STEP_DOWN_MAX_DROP + STEP_DOWN_MAX_RISE) * 0.5f,
                                     STEP_DOWN_REACH};
        std::array<dtPolyRef, STEP_DOWN_NEAR_POLYS> nearPolys{};
        int nearCount = 0;
        if (dtStatusFailed(query->queryPolygons(boxCenter, boxExtents, &filter, nearPolys.data(), &nearCount,
                                                STEP_DOWN_NEAR_POLYS)))
            return false;
        struct Candidate
        {
            float across;
            dtPolyRef ref;
            G3D::Vector3 at;
        };
        std::vector<Candidate> candidates;
        for (int index = 0; index < nearCount; ++index)
        {
            dtPolyRef const ref = nearPolys[size_t(index)];
            if (inOwnPart(ref))
                continue;
            float closest[3];
            if (dtStatusFailed(query->closestPointOnPoly(ref, hereDetour, closest, nullptr)))
                continue;
            G3D::Vector3 const at(closest[2], closest[0], closest[1]);
            float const across = std::hypot(at.x - here.x, at.y - here.y);
            float const drop = here.z - at.z;
            if (across > STEP_DOWN_REACH || drop > STEP_DOWN_MAX_DROP || drop < -STEP_DOWN_MAX_RISE)
                continue;
            candidates.push_back({across, ref, at});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](Candidate const& a, Candidate const& b) { return a.across < b.across; });

        uint32 const phase = bot->GetPhaseMask();
        for (size_t index = 0; index < candidates.size() && index < STEP_DOWN_TRIES; ++index)
        {
            Candidate const& candidate = candidates[index];
            // A larger part than the island (the floor the island stands over), in sight over the step.
            float const atDetour[3] = {candidate.at.y, candidate.at.z, candidate.at.x};
            if (partSize(candidate.ref, atDetour, false) < ISLAND_MAX_POLYS)
                continue;
            if (!map->isInLineOfSight(here.x, here.y, here.z + 2.0f, candidate.at.x, candidate.at.y,
                                      candidate.at.z + 2.0f, phase, LINEOFSIGHT_ALL_CHECKS,
                                      VMAP::ModelIgnoreFlags::Nothing))
                continue;
            landing = candidate.at;
            LOG_INFO("playerbots",
                     "[b34_mech3] {}: island_step_down: on a navmesh island of {} polygons at ({:.1f},{:.1f},{:.1f}) "
                     "-> ({:.1f},{:.1f},{:.1f}), {:.1f} yd across, {:.1f} yd down",
                     bot->GetName(), ownSize, here.x, here.y, here.z, landing.x, landing.y, landing.z,
                     candidate.across, here.z - landing.z);
            return true;
        }
        return false;
    }

    // [lead1050] See the declaration. IslandStepDown's step (reach, drop, rise, sight), judged by the goal: a landing
    // counts when the leader's route from it reaches the goal (its own part does not, by the caller's partial route)
    bool PartStepToward(Player* bot, G3D::Vector3 const& goal, G3D::Vector3& landing)
    {
        Map* map = bot ? bot->GetMap() : nullptr;
        dtNavMesh const* navMesh = map ? map->GetMapCollisionData().GetMMapData().GetNavMesh() : nullptr;
        if (!navMesh || bot->IsFlying() || bot->Unit::IsInWater() || bot->GetTransport())
            return false;
        thread_local std::unordered_map<dtNavMesh const*, std::unique_ptr<dtNavMeshQuery, NavMeshQueryFree>> queries;
        auto& query = queries[navMesh];
        if (!query)
        {
            query.reset(dtAllocNavMeshQuery());
            if (!query || dtStatusFailed(query->init(navMesh, ISLAND_QUERY_NODES)))
            {
                query.reset();
                return false;
            }
        }
        // the route's polygons (Leader::Route: ground, water, steep as its fallback): the bot's own part counts whole
        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
        filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));

        // Detour works in (y, z, x).
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        float const hereDetour[3] = {here.y, here.z, here.x};
        float const standExtents[3] = {1.0f, STAND_ON_POLY_DZ, 1.0f};
        float onPoly[3];
        dtPolyRef hereRef = 0;
        if (dtStatusFailed(query->findNearestPoly(hereDetour, standExtents, &filter, &hereRef, onPoly)) || !hereRef)
            return false;

        // the polygons the bot walks to inside the step's reach: its own part there, no step
        std::array<dtPolyRef, STEP_DOWN_NEAR_POLYS> ownPolys{};
        int ownCount = 0;
        if (dtStatusFailed(query->findPolysAroundCircle(hereRef, onPoly, STEP_DOWN_REACH, &filter, ownPolys.data(),
                                                        nullptr, nullptr, &ownCount, STEP_DOWN_NEAR_POLYS)))
            return false;
        auto const own = [&](dtPolyRef ref)
        { return std::find(ownPolys.begin(), ownPolys.begin() + ownCount, ref) != ownPolys.begin() + ownCount; };

        float const boxMiddleZ = here.z - (STEP_DOWN_MAX_DROP - STEP_DOWN_MAX_RISE) * 0.5f;
        float const boxCenter[3] = {here.y, boxMiddleZ, here.x};
        float const boxExtents[3] = {STEP_DOWN_REACH, (STEP_DOWN_MAX_DROP + STEP_DOWN_MAX_RISE) * 0.5f,
                                     STEP_DOWN_REACH};
        std::array<dtPolyRef, STEP_DOWN_NEAR_POLYS> nearPolys{};
        int nearCount = 0;
        if (dtStatusFailed(query->queryPolygons(boxCenter, boxExtents, &filter, nearPolys.data(), &nearCount,
                                                STEP_DOWN_NEAR_POLYS)))
            return false;
        struct Candidate
        {
            float across;
            G3D::Vector3 at;
        };
        std::vector<Candidate> candidates;
        for (int index = 0; index < nearCount; ++index)
        {
            dtPolyRef const ref = nearPolys[size_t(index)];
            if (ref == hereRef || own(ref))
                continue;
            float closest[3];
            if (dtStatusFailed(query->closestPointOnPoly(ref, hereDetour, closest, nullptr)))
                continue;
            G3D::Vector3 const at(closest[2], closest[0], closest[1]);
            float const across = std::hypot(at.x - here.x, at.y - here.y);
            float const drop = here.z - at.z;
            if (across > STEP_DOWN_REACH || drop > STEP_DOWN_MAX_DROP || drop < -STEP_DOWN_MAX_RISE)
                continue;
            candidates.push_back({across, at});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](Candidate const& a, Candidate const& b) { return a.across < b.across; });

        uint32 const phase = bot->GetPhaseMask();
        size_t tried = 0;
        for (Candidate const& candidate : candidates)
        {
            if (tried >= STEP_DOWN_TRIES)
                break;
            if (!map->isInLineOfSight(here.x, here.y, here.z + 2.0f, candidate.at.x, candidate.at.y,
                                      candidate.at.z + 2.0f, phase, LINEOFSIGHT_ALL_CHECKS,
                                      VMAP::ModelIgnoreFlags::Nothing))
                continue;
            ++tried;
            // a polygon of the bot's own part routes no further than the bot's own walk did: only another part's
            // complete route to the goal is a way there
            if (!Leader::Route(bot, candidate.at, goal).complete)
                continue;
            landing = candidate.at;
            LOG_INFO("playerbots",
                     "[lead1050] {}: part_step: its navmesh part at ({:.1f},{:.1f},{:.1f}) does not reach "
                     "({:.1f},{:.1f},{:.1f}); the step to ({:.1f},{:.1f},{:.1f}), {:.1f} yd across, {:.1f} yd down, "
                     "routes there",
                     bot->GetName(), here.x, here.y, here.z, goal.x, goal.y, goal.z, landing.x, landing.y, landing.z,
                     candidate.across, here.z - landing.z);
            return true;
        }
        return false;
    }

    uint32 PortalDestination(PlayerbotAI* botAI, PortalGoal* goalOut)
    {
        Player* bot = botAI ? botAI->GetBot() : nullptr;
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || bot->InBattleground() || bot->InArena())
            return 0;

        uint32 const here = bot->GetMapId();
        // The anchor (goalOut): where the bot wants to be inside, to pick the entrance whose arrival is nearest.
        if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        {
            WorldLocation const corpse = bot->GetCorpseLocation();
            if (corpse.GetMapId() == here || !IsDungeonMap(corpse.GetMapId()))
                return 0;
            if (goalOut)
                *goalOut = {corpse.GetMapId(), corpse.GetPositionX(), corpse.GetPositionY(), corpse.GetPositionZ(),
                            false};
            return corpse.GetMapId();
        }
        if (!bot->IsAlive())
            return 0;

        PortalGoal goal;
        if (GetPortalGoal(bot->GetGUID(), goal) && goal.mapId)
        {
            bool const reached =
                here == goal.mapId && (!goal.exact || bot->GetExactDist2d(goal.x, goal.y) <= GOAL_REACHED_RANGE);
            if (reached)
                return 0;
            if (goalOut)
                *goalOut = goal;
            return goal.mapId;
        }

        Player* leader = botAI->GetGroupLeader();
        if (!leader || leader == bot || !leader->IsInWorld() || leader->GetMapId() == here ||
            !IsDungeonMap(leader->GetMapId()))
            return 0;
        if (goalOut)
            *goalOut = {leader->GetMapId(), leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                        false};
        return leader->GetMapId();
    }
}

bool InstancePortalTrigger::IsActive()
{
    if (bot->IsInCombat() || bot->GetVehicle() || bot->IsInFlight())
        return false;
    return SelfTravel::PortalDestination(botAI) != 0;
}

bool MoveToInstancePortalAction::isUseful()
{
    return !bot->IsInCombat() && !bot->GetVehicle() && SelfTravel::PortalDestination(botAI) != 0;
}

AreaTrigger const* MoveToInstancePortalAction::FindPortal(uint32 destMapId, SelfTravel::PortalGoal const* goal) const
{
    uint32 const here = bot->GetMapId();
    AreaTrigger const* best = nullptr;
    float bestScore = 0.0f;
    for (auto const& [triggerId, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* at = sObjectMgr->GetAreaTrigger(triggerId);
        if (!at || at->map != here)
            continue;

        bool const leadsIn = teleport.target_mapId == destMapId;
        float const arrival =
            goal ? std::hypot(teleport.target_X - goal->x, teleport.target_Y - goal->y) : 0.0f;
        float const distance = bot->GetExactDist(at->x, at->y, at->z);
        float score = 0.0f;
        if (here == destMapId)
        {
            // Inside already, bound for another wing: a portal inside straight to it, else out into the world.
            if (leadsIn)
            {
                if (!goal || arrival > GOAL_REACHED_RANGE)
                    continue;
                score = distance;
            }
            else
            {
                if (IsDungeonMap(teleport.target_mapId))
                    continue;
                // [b30_wings] Out into the world for another wing: the exit that leads back in, not the nearest one.
                // Dire Maul heroic (dme-h 20261025-184229-dme-h-5E-15 and build 29's -163739-dme-h-5A-23): after
                // Alzzin the nearest exit was 3197 "Alzzin the Wildshaper's broken wall [1-Way]", whose arrival
                // (-3577.7,841.9,134.6) is a walled pocket of 323 polygons joined to nothing (tools/navmesh_probe.py
                // 1 -3577.7 841.9 134.6 -3742.0 1249.2 160.2: NOT CONNECTED); the group stood at its rim
                // -3629.6,925.3 "no walk to the portal" until stopped. Ranked: a walk inside to it (the long route's
                // length, not the straight line - an exit of another wing is no walk), an entrance back in at its
                // arrival (a two-way exit), then the leg outside from its arrival to the entrance that arrives
                // nearest the goal.
                constexpr float EXIT_PAIR_RANGE = 40.0f;  // an entrance this near the arrival: the exit is two-way
                constexpr float EXIT_ONE_WAY_PENALTY = 200000.0f;
                constexpr float EXIT_NO_WALK_PENALTY = 400000.0f;
                float nearestEntrance = FLT_MAX;
                float bestArrival = FLT_MAX;
                float outsideLeg = 0.0f;
                for (auto const& [entryId, entryTeleport] : sObjectMgr->GetAllAreaTriggerTeleports())
                {
                    if (entryTeleport.target_mapId != destMapId)
                        continue;
                    AreaTrigger const* in = sObjectMgr->GetAreaTrigger(entryId);
                    if (!in || in->map != teleport.target_mapId)
                        continue;
                    float const fromArrival = std::hypot(in->x - teleport.target_X, in->y - teleport.target_Y);
                    nearestEntrance = std::min(nearestEntrance, fromArrival);
                    if (goal)
                    {
                        float const toGoal =
                            std::hypot(entryTeleport.target_X - goal->x, entryTeleport.target_Y - goal->y);
                        if (toGoal < bestArrival)
                        {
                            bestArrival = toGoal;
                            outsideLeg = fromArrival;
                        }
                    }
                }
                Movement::PointsArray corners;
                bool const walk = SelfTravel::LongRoute(bot, G3D::Vector3(at->x, at->y, at->z), corners,
                                                        PortalReach(at) + LONG_ROUTE_REACH_MARGIN) &&
                                  corners.size() >= 2;
                score = (walk ? PolylineLength(corners) : distance + EXIT_NO_WALK_PENALTY) + outsideLeg +
                        (nearestEntrance <= EXIT_PAIR_RANGE ? 0.0f : EXIT_ONE_WAY_PENALTY) + 100000.0f;
            }
        }
        else
        {
            if (!leadsIn)
                continue;
            // The entrance whose arrival is nearest where the bot is going (the wing of its corpse, its group, the
            // goal), the nearest one of equals.
            score = arrival * 100.0f + distance;
        }

        if (!best || score < bestScore)
        {
            best = at;
            bestScore = score;
        }
    }
    return best;
}

bool MoveToInstancePortalAction::EnterPortal(AreaTrigger const* portal)
{
    uint32 const now = getMSTime();
    if (_lastEnterMs && getMSTimeDiff(_lastEnterMs, now) < PORTAL_REENTER_MS)
        return true;  // the port is under way
    _lastEnterMs = now;

    // As the client sends it on entering the trigger; the core makes every check (instance, group, lock).
    WorldPacket packet(CMSG_AREATRIGGER);
    packet << uint32(portal->entry);
    bot->GetSession()->HandleAreaTriggerOpcode(packet);
    if (!bot->IsBeingTeleported())
    {
        SelfTravel::ReportCannotMove(bot, "move to instance portal", "the portal did not take the bot", portal->x,
                                     portal->y, portal->z);
        return false;
    }
    return true;
}

bool MoveToInstancePortalAction::FindEntryPoint(AreaTrigger const* portal, G3D::Vector3& entry)
{
    // A complete walk to a point inside the trigger when the centre itself lies on a navmesh part of its own
    // (Deadmines wave 13: the polygons under the portal are a 78-polygon island 2.2 yd from the tunnel floor, the
    // tunnel's last polygon is 4.8 yd from the centre, inside the 7 yd trigger; the ghost paced on the floor above).
    uint32 const now = getMSTime();
    if (_entryPortal == portal->entry && _entryCheckedMs && getMSTimeDiff(_entryCheckedMs, now) < ENTRY_RECHECK_MS &&
        bot->GetMapId() == _entryMapId)
    {
        entry = _entryPoint;
        return _entryFound;
    }
    _entryPortal = portal->entry;
    _entryMapId = bot->GetMapId();
    _entryCheckedMs = now;
    _entryFound = false;

    float const radius = portal->radius > 0.0f ? portal->radius : std::min(portal->length, portal->width) * 0.5f;
    for (float const share : ENTRY_RING_SHARES)
    {
        float const ring = std::max(0.5f, radius * share);
        for (uint32 i = 0; i < ENTRY_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / ENTRY_DIRECTIONS;
            float const x = portal->x + std::cos(angle) * ring;
            float const y = portal->y + std::sin(angle) * ring;
            float const z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, portal->z + FLOOR_SNAP_UP, true,
                                                     FLOOR_SNAP_RANGE);
            if (z <= INVALID_HEIGHT || std::fabs(z - portal->z) > radius + FLOOR_SNAP_UP)
                continue;

            BotPath const path = FindBotPath(bot, x, y, z);  // [lead109]
            if (path.type != PATHFIND_NORMAL || path.points.size() < 2)
                continue;
            if ((path.actualEnd - G3D::Vector3(portal->x, portal->y, portal->z)).length() > radius)
                continue;
            _entryPoint = G3D::Vector3(x, y, z);
            _entryFound = true;
            entry = _entryPoint;
            return true;
        }
    }
    return false;
}

bool MoveToInstancePortalAction::HopOut(AreaTrigger const* portal, bool ghost)
{
    // Stuck on a navmesh island (Razorfen Kraul wave 13: five ghosts on a 14-polygon ledge under the Great Lift, 4.4
    // yd of crack from the floor that leads up to the entrance): a short straight step to a spot whose walk gets
    // nearer, as a player steps over the crack. A ghost needs no ground under the step, the living do.
    G3D::Vector3 const portalPos(portal->x, portal->y, portal->z);
    // How near a walk from a spot gets to the portal. A shortcut or no path reports the portal itself as its end,
    // which made every hop look no better (dtk-h 2026-10-25: a ghost on a 49-polygon pond in a pit, 5 yd from the
    // river region that leads up under the entrance, stood there "no walk to the portal" for good).
    auto const walkGap = [&portalPos](BotPath const& path, G3D::Vector3 const& start)  // [lead109]
    {
        if (path.type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH))
            return (start - portalPos).length();
        return (path.actualEnd - portalPos).length();
    };
    BotPath const here = FindBotPath(bot, portal->x, portal->y, portal->z);  // [lead109]
    float const hereGap =
        walkGap(here, G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));

    for (float const step : HOP_DISTANCES)
        for (uint32 i = 0; i < HOP_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / HOP_DIRECTIONS;
            float const x = bot->GetPositionX() + std::cos(angle) * step;
            float const y = bot->GetPositionY() + std::sin(angle) * step;
            float z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, bot->GetPositionZ() + FLOOR_SNAP_UP, true,
                                               FLOOR_SNAP_RANGE);
            // In a pond the step is to its surface, not to its bottom.
            float const surface = bot->GetMapWaterOrGroundLevel(x, y, bot->GetPositionZ() + FLOOR_SNAP_UP);
            if (surface > z)
                z = surface;
            if (z <= INVALID_HEIGHT || std::fabs(z - bot->GetPositionZ()) > HOP_MAX_DZ ||
                !bot->IsWithinLOS(x, y, z + HOP_LOS_UP))
                continue;

            BotPath const from = FindBotPath(bot, x, y, z, portal->x, portal->y, portal->z);  // [lead109]
            float const gap = walkGap(from, G3D::Vector3(x, y, z));
            if (from.type != PATHFIND_NORMAL && gap + HOP_MIN_GAIN > hereGap)
                continue;
            if (MoveStraight(bot->GetMapId(), x, y, z, !ghost))
                return true;
        }
    return false;
}

int32 MoveToInstancePortalAction::WalkTowards(AreaTrigger const* portal, bool ghost)
{
    G3D::Vector3 const target(portal->x, portal->y, portal->z);
    float const left = bot->GetExactDist(portal->x, portal->y, portal->z);

    // [deaths146] alive and walking alone in the open world (its group in the instance or far off): the walk is the
    // priced route a solo walk takes (SoloSafety::RouteCosts: round the packs it loses, never through orange / red
    // reaches), not the core's path through every camp. A route that only passes through refused reaches is no
    // walk: the bot stays (its group's leader decides), as a solo walk does. No route at all: the walks below
    if (!ghost && SoloSafety::WalksAlone(bot))
    {
        // the route kept per bot while it walks on it (searched again off it, for another goal, or at the long
        // route's recheck cadence), as NextRouteLeg keeps its own: a route search is a cost
        struct PricedWalk
        {
            SelfTravel::RouteWalk walk;
            bool refused = false;
        };
        static thread_local std::unordered_map<ObjectGuid, PricedWalk> pricedWalks;
        PricedWalk& priced = pricedWalks[bot->GetGUID()];
        uint32 const now = getMSTime();
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        bool const sameGoal = priced.walk.searchedMs && priced.walk.mapId == bot->GetMapId() &&
                              (priced.walk.goal - target).length() <= ROUTE_SAME_GOAL_RANGE;
        size_t segment = 0;
        float off = FLT_MAX;
        G3D::Vector3 onRoute = here;
        if (sameGoal && priced.walk.found && priced.walk.corners.size() >= 2)
            onRoute = NearestOnPolyline(priced.walk.corners, here, segment, off);
        bool const onIt = sameGoal && priced.walk.found && off <= LONG_ROUTE_OFF_RANGE;
        bool const recent = sameGoal && getMSTimeDiff(priced.walk.searchedMs, now) < LONG_ROUTE_RECHECK_MS;
        if (!onIt && !recent)
        {
            Leader::RouteResult const route = Leader::Route(bot, Position(target.x, target.y, target.z));
            priced.walk.goal = target;
            priced.walk.mapId = bot->GetMapId();
            priced.walk.searchedMs = now ? now : 1;
            priced.walk.found = route.found && !route.throughRefused && route.points.size() >= 2;
            priced.walk.corners.assign(route.points.begin(), route.points.end());
            priced.refused = (route.found && route.throughRefused) || route.refusedArea;
            off = FLT_MAX;
            segment = 0;
            if (priced.walk.found)
                onRoute = NearestOnPolyline(priced.walk.corners, here, segment, off);
            if (priced.refused)
                LOG_INFO("playerbots", "[deaths146] {} L{} walks alone to portal {} ({:.0f} yd): every way passes "
                         "mobs or an area it must not walk through - it stays",
                         bot->GetName(), bot->GetLevel(), portal->entry, left);
        }
        if (priced.walk.found && priced.walk.corners.size() >= 2 && off <= LONG_ROUTE_OFF_RANGE)
        {
            Movement::PointsArray rest;
            rest.push_back(onRoute);
            for (size_t index = segment + 1; index < priced.walk.corners.size(); ++index)
                rest.push_back(priced.walk.corners[index]);
            G3D::Vector3 const next = PointAlong(rest, PORTAL_LEG_STEP);
            if (bot->GetExactDist(next.x, next.y, next.z) > 1.0f && MoveTo(bot->GetMapId(), next.x, next.y, next.z))
                return 1;
            if (bot->isMoving())
                return -1;
        }
        else if (priced.refused)
            return 0;
    }

    PathGenerator path(bot);
    path.SetUseStraightPath(true);
    path.CalculatePath(target.x, target.y, target.z, false);
    uint32 const type = path.GetPathType();
    Movement::PointsArray const& points = path.GetPath();
    bool const usable =
        !(type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH)) && points.size() >= 2;

    // Near the portal, no complete walk to its centre: a point inside the trigger that a walk does reach.
    G3D::Vector3 entry;
    if (type != PATHFIND_NORMAL && left <= ENTRY_SEARCH_RANGE && FindEntryPoint(portal, entry))
    {
        PathGenerator entryPath(bot);
        entryPath.SetUseStraightPath(true);
        entryPath.CalculatePath(entry.x, entry.y, entry.z, false);
        if (entryPath.GetPathType() == PATHFIND_NORMAL && entryPath.GetPath().size() >= 2)
        {
            G3D::Vector3 const next = PointAlong(entryPath.GetPath(), PORTAL_LEG_STEP);
            if (bot->GetExactDist(next.x, next.y, next.z) > 1.0f && MoveTo(bot->GetMapId(), next.x, next.y, next.z))
                return 1;
            if (bot->isMoving())
                return -1;
        }
    }
    // No complete walk from PathGenerator: the long route, when it reaches the portal.
    if (type != PATHFIND_NORMAL)
    {
        int32 const routed = WalkLongRoute(portal);
        if (routed != 0)
            return routed;
        // [b32_bfd] No route at all: the portal's pocket across a gap (Blackfathom Deeps), before the partial walk
        // that ends at the navmesh point nearest the portal as the crow flies (a cliff top above it).
        int32 const bridged = WalkViaBridge(portal, ghost);
        if (bridged != 0)
            return bridged;
    }
    if (usable && (path.GetActualEndPosition() - target).length() + ROUTE_GAIN < left)
    {
        G3D::Vector3 next = PointAlong(points, PORTAL_LEG_STEP);
        // Between two corners the height is interpolated: the floor under the point instead.
        float const floor = bot->GetMapHeight(next.x, next.y, next.z + FLOOR_SNAP_UP);
        if (floor > INVALID_HEIGHT && std::fabs(floor - next.z) <= FLOOR_SNAP_RANGE && !bot->Unit::IsInWater())
            next.z = floor;
        if (bot->GetExactDist(next.x, next.y, next.z) > 1.0f && MoveTo(bot->GetMapId(), next.x, next.y, next.z))
            return 1;
        if (bot->isMoving())
            return -1;
    }

    // The navmesh ends short of the portal: the last yards straight (a ghost takes no fall damage). Its centre often
    // lies on the floor or behind a door frame (Magisters' Terrace wave 11: a ghost 24 yd away tried the centre for
    // minutes): a point above it, then the points of its radius on the bot's side.
    if (left <= PORTAL_STRAIGHT_RANGE)
    {
        if (MoveStraight(bot->GetMapId(), portal->x, portal->y, portal->z + PORTAL_AIM_UP, !ghost))
            return 1;
        float const reach = std::max(1.0f, portal->radius > 0.0f ? portal->radius * 0.5f : 1.0f);
        float const angle = std::atan2(bot->GetPositionY() - portal->y, bot->GetPositionX() - portal->x);
        for (float const offset : {0.0f, 0.6f, -0.6f})
        {
            float const x = portal->x + std::cos(angle + offset) * reach;
            float const y = portal->y + std::sin(angle + offset) * reach;
            if (MoveStraight(bot->GetMapId(), x, y, portal->z + PORTAL_AIM_UP, !ghost))
                return 1;
        }
    }
    return bot->isMoving() ? -1 : 0;
}

int32 MoveToInstancePortalAction::WalkLongRoute(AreaTrigger const* portal)
{
    uint32 const now = getMSTime();
    G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    bool const samePortal = _routePortal == portal->entry && _routeMapId == bot->GetMapId() && _routeMs;
    bool const recent = samePortal && getMSTimeDiff(_routeMs, now) < LONG_ROUTE_RECHECK_MS;
    bool fresh = recent;
    size_t segment = 0;
    float off = 0.0f;
    G3D::Vector3 onRoute = here;
    if (samePortal && _routeFound && _routeCorners.size() >= 2)
    {
        // A found route is kept while the bot is on it (a search spends up to the whole node pool); searched again
        // when the bot left it, or at its end short of the portal (the corner buffer cut it), at most every
        // LONG_ROUTE_RECHECK_MS.
        onRoute = NearestOnPolyline(_routeCorners, here, segment, off);
        G3D::Vector3 const& last = _routeCorners.back();
        bool const atEnd = segment + 2 >= _routeCorners.size() &&
                           (onRoute - last).length() <= PROGRESS_RANGE;
        bool const endShort = (last - G3D::Vector3(portal->x, portal->y, portal->z)).length() >
                              PortalReach(portal) + LONG_ROUTE_REACH_MARGIN;
        fresh = off <= LONG_ROUTE_OFF_RANGE && (!(atEnd && endShort) || recent);
    }
    if (!fresh)
    {
        // Searched at most every LONG_ROUTE_RECHECK_MS while no route is found, or when the bot left the route.
        _routePortal = portal->entry;
        _routeMapId = bot->GetMapId();
        _routeMs = now;
        _routeBestLeft = FLT_MAX;
        _routeFound = SelfTravel::LongRoute(bot, G3D::Vector3(portal->x, portal->y, portal->z), _routeCorners,
                                            PortalReach(portal) + LONG_ROUTE_REACH_MARGIN);
        if (_routeFound)
            onRoute = NearestOnPolyline(_routeCorners, here, segment, off);
    }
    if (!_routeFound || _routeCorners.size() < 2)
        return 0;

    Movement::PointsArray rest;
    rest.reserve(_routeCorners.size() - segment);
    rest.push_back(onRoute);
    for (size_t index = segment + 1; index < _routeCorners.size(); ++index)
        rest.push_back(_routeCorners[index]);

    // Progress is the walk left along the route: it leads away from the portal first (round the pit, the island),
    // the straight-line gain of Execute would call that circling.
    float const left = PolylineLength(rest);
    if (left + PROGRESS_RANGE < _routeBestLeft)
    {
        _routeBestLeft = left;
        _gainMs = now;
        _gainDistance = bot->GetExactDist(portal->x, portal->y, portal->z);
    }

    G3D::Vector3 next = PointAlong(rest, PORTAL_LEG_STEP);
    // Between two corners the height is interpolated: the floor under the point instead.
    float const floor = bot->GetMapHeight(next.x, next.y, next.z + FLOOR_SNAP_UP);
    if (floor > INVALID_HEIGHT && std::fabs(floor - next.z) <= FLOOR_SNAP_RANGE && !bot->Unit::IsInWater())
        next.z = floor;
    if (bot->GetExactDist(next.x, next.y, next.z) > 1.0f && MoveTo(bot->GetMapId(), next.x, next.y, next.z))
        return 1;
    return bot->isMoving() ? -1 : 0;
}

bool MoveToInstancePortalAction::FindBridge(AreaTrigger const* portal, bool ghost)
{
    // [b32_bfd] Searched once per portal and map (kept while found, again after BRIDGE_RECHECK_MS when not).
    uint32 const now = getMSTime();
    if (_bridgePortal == portal->entry && _bridgeMapId == bot->GetMapId() && _bridgeMs &&
        (_bridgeFound || getMSTimeDiff(_bridgeMs, now) < BRIDGE_RECHECK_MS))
        return _bridgeFound;
    _bridgePortal = portal->entry;
    _bridgeMapId = bot->GetMapId();
    _bridgeMs = now ? now : 1;
    _bridgeFound = false;
    _bridgeCrossing.clear();
    _bridgeLeg = 0;
    _bridgeBestLeft = FLT_MAX;
    _bridgePocket.clear();
    _bridgeWalk = SelfTravel::RouteWalk();

    Map* map = bot->GetMap();
    dtNavMesh const* navMesh = map ? map->GetMapCollisionData().GetMMapData().GetNavMesh() : nullptr;
    if (!navMesh)
        return false;
    std::unique_ptr<dtNavMeshQuery, NavMeshQueryFree> query(dtAllocNavMeshQuery());
    if (!query || dtStatusFailed(query->init(navMesh, LONG_ROUTE_NODES)))
        return false;
    dtQueryFilter filter;
    filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
    filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));
    // Detour works in (y, z, x).
    auto const fromDetour = [](float const* point) { return G3D::Vector3(point[2], point[0], point[1]); };

    // The pocket: every polygon a walk reaches from the polygons within the trigger's reach.
    float const reach = PortalReach(portal);
    float const center[3] = {portal->y, portal->z, portal->x};
    float const startExtents[3] = {reach, reach, reach};
    std::array<dtPolyRef, BRIDGE_START_POLYS> starts{};
    int startCount = 0;
    if (dtStatusFailed(query->queryPolygons(center, startExtents, &filter, starts.data(), &startCount,
                                            BRIDGE_START_POLYS)) ||
        startCount <= 0)
        return false;
    std::vector<dtPolyRef> reached(static_cast<size_t>(BRIDGE_MAX_POCKET_POLYS));
    for (int index = 0; index < startCount; ++index)
    {
        if (_bridgePocket.count(uint64(starts[size_t(index)])))
            continue;
        int count = 0;
        dtStatus const status = query->findPolysAroundCircle(starts[size_t(index)], center, BRIDGE_POCKET_RADIUS,
                                                             &filter, reached.data(), nullptr, nullptr, &count,
                                                             BRIDGE_MAX_POCKET_POLYS);
        if (dtStatusFailed(status) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL) ||
            dtStatusDetail(status, DT_OUT_OF_NODES))
        {
            _bridgePocket.clear();
            return false;  // the open world, no pocket
        }
        for (int k = 0; k < count; ++k)
            _bridgePocket.insert(uint64(reached[size_t(k)]));
        if (_bridgePocket.size() >= size_t(BRIDGE_MAX_POCKET_POLYS))
        {
            _bridgePocket.clear();
            return false;
        }
    }

    // The gaps: a polygon outside the pocket within BRIDGE_MAX_GAP of a pocket polygon.
    struct Gap
    {
        float length;
        G3D::Vector3 shore;
        G3D::Vector3 inside;
        bool drop = false;  // [b40_brd] a ghost's drop from above
        dtPolyRef shoreRef = 0;  // [b44_st] the shore polygon (the island test)
    };
    std::vector<Gap> gaps;
    std::vector<Gap> drops;  // [b40_brd]
    std::array<dtPolyRef, BRIDGE_NEAR_POLYS> nearPolys{};
    for (uint64 const raw : _bridgePocket)
    {
        dtPolyRef const ref = dtPolyRef(raw);
        dtMeshTile const* tile = nullptr;
        dtPoly const* poly = nullptr;
        if (dtStatusFailed(navMesh->getTileAndPolyByRef(ref, &tile, &poly)) || !tile || !poly ||
            poly->getType() != DT_POLYTYPE_GROUND || !poly->vertCount)
            continue;
        float low[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float high[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (uint8 vertex = 0; vertex < poly->vertCount; ++vertex)
        {
            float const* point = &tile->verts[size_t(poly->verts[vertex]) * 3];
            for (int axis = 0; axis < 3; ++axis)
            {
                low[axis] = std::min(low[axis], point[axis]);
                high[axis] = std::max(high[axis], point[axis]);
            }
        }
        float const middle[3] = {(low[0] + high[0]) * 0.5f, (low[1] + high[1]) * 0.5f, (low[2] + high[2]) * 0.5f};
        float const extents[3] = {(high[0] - low[0]) * 0.5f + BRIDGE_MAX_GAP,
                                  (high[1] - low[1]) * 0.5f + BRIDGE_MAX_DZ,
                                  (high[2] - low[2]) * 0.5f + BRIDGE_MAX_GAP};
        int nearCount = 0;
        if (dtStatusFailed(query->queryPolygons(middle, extents, &filter, nearPolys.data(), &nearCount,
                                                BRIDGE_NEAR_POLYS)))
            continue;
        for (int index = 0; index < nearCount; ++index)
        {
            dtPolyRef const other = nearPolys[size_t(index)];
            if (_bridgePocket.count(uint64(other)))
                continue;
            float shore[3];
            float inside[3];
            if (dtStatusFailed(query->closestPointOnPoly(other, middle, shore, nullptr)) ||
                dtStatusFailed(query->closestPointOnPoly(ref, shore, inside, nullptr)) ||
                dtStatusFailed(query->closestPointOnPoly(other, inside, shore, nullptr)))
                continue;
            G3D::Vector3 const a = fromDetour(shore);
            G3D::Vector3 const b = fromDetour(inside);
            float const length = (a - b).length();
            if (length <= BRIDGE_MAX_GAP && std::fabs(a.z - b.z) <= BRIDGE_MAX_DZ)
                gaps.push_back({length, a, b, false, other});
        }

        // [b40_brd] a ghost's drop: a walk polygon above this one, its nearest point beside it (an edge over open
        // air, not a floor right over it), the shortest falls first and after every level gap
        if (!ghost)
            continue;
        float const above[3] = {middle[0], middle[1] + GHOST_DROP_MAX_DZ * 0.5f, middle[2]};
        float const aboveExtents[3] = {(high[0] - low[0]) * 0.5f + GHOST_DROP_MAX_ACROSS, GHOST_DROP_MAX_DZ * 0.5f,
                                       (high[2] - low[2]) * 0.5f + GHOST_DROP_MAX_ACROSS};
        nearCount = 0;
        if (dtStatusFailed(query->queryPolygons(above, aboveExtents, &filter, nearPolys.data(), &nearCount,
                                                BRIDGE_NEAR_POLYS)))
            continue;
        for (int index = 0; index < nearCount; ++index)
        {
            dtPolyRef const other = nearPolys[size_t(index)];
            if (_bridgePocket.count(uint64(other)))
                continue;
            float shore[3];
            float inside[3];
            if (dtStatusFailed(query->closestPointOnPoly(other, middle, shore, nullptr)) ||
                dtStatusFailed(query->closestPointOnPoly(ref, shore, inside, nullptr)) ||
                dtStatusFailed(query->closestPointOnPoly(other, inside, shore, nullptr)))
                continue;
            G3D::Vector3 const a = fromDetour(shore);
            G3D::Vector3 const b = fromDetour(inside);
            float const across = std::hypot(a.x - b.x, a.y - b.y);
            float const fall = a.z - b.z;
            // [b44_st] a water surface right over the pocket is a dive, not a floor above it
            dtMeshTile const* otherTile = nullptr;
            dtPoly const* otherPoly = nullptr;
            bool const waterShore = dtStatusSucceed(navMesh->getTileAndPolyByRef(other, &otherTile, &otherPoly)) &&
                                    otherPoly && (otherPoly->flags & NAV_WATER);
            if (fall > BRIDGE_MAX_DZ && fall <= GHOST_DROP_MAX_DZ &&
                (across >= GHOST_DROP_MIN_ACROSS || waterShore) && across <= GHOST_DROP_MAX_ACROSS)
                drops.push_back({fall, a, b, true, other});
        }
    }
    std::sort(gaps.begin(), gaps.end(), [](Gap const& a, Gap const& b) { return a.length < b.length; });
    // [b40_brd] the drops after the level gaps, with a try budget of their own
    std::sort(drops.begin(), drops.end(), [](Gap const& a, Gap const& b) { return a.length < b.length; });

    // The crossing of a gap: straight over it in sight, or a dive under a wall (both ends under water).
    uint32 const phase = bot->GetPhaseMask();
    float const height = bot->GetCollisionHeight();
    auto const sight = [map, phase](G3D::Vector3 const& a, G3D::Vector3 const& b)
    {
        return map->isInLineOfSight(a.x, a.y, a.z, b.x, b.y, b.z, phase, LINEOFSIGHT_ALL_CHECKS,
                                    VMAP::ModelIgnoreFlags::Nothing);
    };
    auto const crossing = [&](Gap const& gap, std::vector<G3D::Vector3>& legs)
    {
        G3D::Vector3 const up(0.0f, 0.0f, BRIDGE_LOS_UP);
        // [b40_brd] a drop: out over the edge at the shore's height, then straight down, open air on both legs
        if (gap.drop)
        {
            G3D::Vector3 const over(gap.inside.x, gap.inside.y, gap.shore.z);
            if (!sight(gap.shore + up, over + up) || !sight(over + up, gap.inside + up))
                return false;
            legs = {over, gap.inside};
            return true;
        }
        G3D::Vector3 const middle = (gap.shore + gap.inside) * 0.5f;
        bool const wet = map->IsInWater(phase, middle.x, middle.y, middle.z, height);
        if ((ghost || wet) && sight(gap.shore + up, gap.inside + up))
        {
            legs = {gap.inside};
            return true;
        }
        for (float const depth : BRIDGE_DIVE_DEPTHS)
        {
            G3D::Vector3 const down(0.0f, 0.0f, depth);
            G3D::Vector3 const a = gap.shore - down;
            G3D::Vector3 const b = gap.inside - down;
            if (!map->IsUnderWater(phase, a.x, a.y, a.z, height) || !map->IsUnderWater(phase, b.x, b.y, b.z, height))
                continue;
            if (sight(gap.shore + up, a) && sight(a, b) && sight(b, gap.inside + up))
            {
                legs = {a, b, gap.inside};
                return true;
            }
        }
        return false;
    };

    // [b44_st] closed islands (BRIDGE_ISLAND_MAX_POLYS): their polygons, and the bot's own polygon (not one of them)
    std::unordered_set<uint64> islandPolys;
    std::unordered_set<uint64> openPolys;
    size_t islandsSkipped = 0;
    dtPolyRef botRef = 0;
    {
        float const here[3] = {bot->GetPositionY(), bot->GetPositionZ(), bot->GetPositionX()};
        float const extents[3] = {NAVMESH_FLOOR_HALF_WIDTH * 2.0f, FLOOR_SNAP_RANGE, NAVMESH_FLOOR_HALF_WIDTH * 2.0f};
        float nearest[3];
        if (dtStatusFailed(query->findNearestPoly(here, extents, &filter, &botRef, nearest)))
            botRef = 0;
    }
    std::vector<dtPolyRef> islandBuffer(static_cast<size_t>(BRIDGE_ISLAND_MAX_POLYS));
    std::unique_ptr<dtNavMeshQuery, NavMeshQueryFree> islandQuery(dtAllocNavMeshQuery());
    if (islandQuery && dtStatusFailed(islandQuery->init(navMesh, BRIDGE_ISLAND_NODES)))
        islandQuery.reset();
    auto const onIsland = [&](Gap const& gap)
    {
        if (!islandQuery || !gap.shoreRef || openPolys.count(uint64(gap.shoreRef)))
            return false;
        if (islandPolys.count(uint64(gap.shoreRef)))
            return true;
        float const from[3] = {gap.shore.y, gap.shore.z, gap.shore.x};
        int count = 0;
        dtStatus const status =
            islandQuery->findPolysAroundCircle(gap.shoreRef, from, BRIDGE_ISLAND_RADIUS, &filter,
                                               islandBuffer.data(), nullptr, nullptr, &count,
                                               BRIDGE_ISLAND_MAX_POLYS);
        bool const closed = dtStatusSucceed(status) && !dtStatusDetail(status, DT_BUFFER_TOO_SMALL) &&
                            !dtStatusDetail(status, DT_OUT_OF_NODES) && count < BRIDGE_ISLAND_MAX_POLYS;
        bool const holdsBot = closed && botRef &&
                              std::find(islandBuffer.begin(), islandBuffer.begin() + count, botRef) !=
                                  islandBuffer.begin() + count;
        if (!closed || holdsBot)
        {
            openPolys.insert(uint64(gap.shoreRef));
            return false;
        }
        for (int k = 0; k < count; ++k)
            islandPolys.insert(uint64(islandBuffer[size_t(k)]));
        return true;
    };

    std::vector<G3D::Vector3> tried;
    size_t routes = 0;
    // [b40_brd] the level gaps first, then the ghost's drops with a try budget of their own
    size_t triedGaps = 0;    // tried / routes when the drops began
    size_t routesGaps = 0;
    bool dropsBegan = false;
    std::vector<Gap const*> order;
    order.reserve(gaps.size() + drops.size());
    for (Gap const& gap : gaps)
        order.push_back(&gap);
    for (Gap const& gap : drops)
        order.push_back(&gap);
    for (Gap const* const next : order)
    {
        Gap const& gap = *next;
        if (gap.drop && !dropsBegan)
        {
            dropsBegan = true;
            triedGaps = tried.size();
            routesGaps = routes;
        }
        size_t const triedHere = tried.size() - (gap.drop ? triedGaps : 0);
        size_t const routesHere = routes - (gap.drop ? routesGaps : 0);
        if (triedHere >= BRIDGE_MAX_TRIES || routesHere >= BRIDGE_MAX_ROUTES)
        {
            if (gap.drop)
                break;
            continue;  // on to the drops
        }
        if (std::any_of(tried.begin(), tried.end(), [&gap](G3D::Vector3 const& shore)
                        { return (shore - gap.shore).length() <= BRIDGE_SAME_SHORE; }))
            continue;
        // [b44_st] a shore on a closed island spends no try
        if (onIsland(gap))
        {
            ++islandsSkipped;
            continue;
        }
        tried.push_back(gap.shore);
        std::vector<G3D::Vector3> legs;
        if (!crossing(gap, legs))
            continue;
        ++routes;
        Movement::PointsArray corners;
        if (!SelfTravel::LongRoute(bot, gap.shore, corners, BRIDGE_SHORE_REACH))
            continue;
        _bridgeFound = true;
        _bridgeShore = gap.shore;
        _bridgeCrossing = legs;
        LOG_INFO("playerbots", "[b32_bfd ghost bridge] {}: portal {} lies in a navmesh pocket of {} polygons; walk "
                 "to ({:.1f},{:.1f},{:.1f}), then {} straight leg(s) {} {:.1f} yd into it at "
                 "({:.1f},{:.1f},{:.1f})", bot->GetName(), portal->entry, _bridgePocket.size(), gap.shore.x,
                 gap.shore.y, gap.shore.z, legs.size(), gap.drop ? "down a [b40_brd] drop of" : "over a gap of",
                 gap.length, gap.inside.x, gap.inside.y, gap.inside.z);
        return true;
    }
    LOG_INFO("playerbots", "[b32_bfd ghost bridge] {}: portal {}: no gap into its navmesh pocket of {} polygons "
             "({} gaps within {:.0f} yd, {} ghost drops, {} crossings tried, {} shore walks searched, [b44_st] {} "
             "shores on closed islands skipped)", bot->GetName(), portal->entry, _bridgePocket.size(), gaps.size(),
             BRIDGE_MAX_GAP, drops.size(), tried.size(), routes, islandsSkipped);
    return false;
}

int32 MoveToInstancePortalAction::WalkViaBridge(AreaTrigger const* portal, bool ghost)
{
    if (!FindBridge(portal, ghost))
        return 0;

    // In the pocket: the walk inside leads on (WalkLongRoute's partial route ends within the trigger's reach).
    if (dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery())
    {
        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
        filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));
        float const here[3] = {bot->GetPositionY(), bot->GetPositionZ(), bot->GetPositionX()};
        float const extents[3] = {NAVMESH_FLOOR_HALF_WIDTH, FLOOR_SNAP_UP, NAVMESH_FLOOR_HALF_WIDTH};
        float nearest[3];
        dtPolyRef ref = 0;
        if (dtStatusSucceed(query->findNearestPoly(here, extents, &filter, &ref, nearest)) && ref &&
            _bridgePocket.count(uint64(ref)))
        {
            _bridgeLeg = 0;
            return 0;
        }
    }

    if (bot->isMoving() && IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return -1;

    // The walk to the shore end of the gap (_bridgeLeg 0), then leg _bridgeLeg - 1 of the crossing.
    float const shoreLeft = bot->GetExactDist(_bridgeShore.x, _bridgeShore.y, _bridgeShore.z);
    if (!_bridgeLeg && shoreLeft > BRIDGE_SHORE_ARRIVE)
    {
        // Progress is the walk to the shore: it may lead away from the portal first.
        if (shoreLeft + PROGRESS_RANGE < _bridgeBestLeft)
        {
            _bridgeBestLeft = shoreLeft;
            _gainMs = getMSTime();
            _gainDistance = bot->GetExactDist(portal->x, portal->y, portal->z);
        }
        G3D::Vector3 leg;
        if (!SelfTravel::NextRouteLeg(bot, _bridgeWalk, _bridgeShore, BRIDGE_SHORE_REACH, PORTAL_LEG_STEP, leg))
            return 0;
        if (bot->GetExactDist(leg.x, leg.y, leg.z) > 1.0f && MoveTo(bot->GetMapId(), leg.x, leg.y, leg.z))
            return 1;
        return bot->isMoving() ? -1 : 0;
    }

    if (!_bridgeLeg)
        _bridgeLeg = 1;
    while (_bridgeLeg <= _bridgeCrossing.size())
    {
        G3D::Vector3 const& point = _bridgeCrossing[_bridgeLeg - 1];
        if (bot->GetExactDist(point.x, point.y, point.z) > BRIDGE_LEG_ARRIVE)
            break;
        ++_bridgeLeg;
    }
    if (_bridgeLeg > _bridgeCrossing.size())
    {
        // Across but on no pocket polygon: searched again in the next window.
        _bridgeLeg = 0;
        _bridgeMs = 0;
        _bridgeFound = false;
        return 0;
    }

    // A straight leg as given: a MovePoint is pathed again (PointMovementGenerator) and that path ends at the gap's
    // own edge; a spline of three points is followed as it is (as MovementAction::SwimStraight does).
    G3D::Vector3 const next = _bridgeCrossing[_bridgeLeg - 1];
    G3D::Vector3 const start(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    Movement::PointsArray path = {start, (start + next) * 0.5f, next};
    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    MotionMaster* motion = bot->GetMotionMaster();
    motion->Clear();
    motion->MoveSplinePath(&path);
    float const delay = std::clamp(static_cast<float>(IN_MILLISECONDS) * MoveDelay((next - start).length()), 0.0f,
                                   static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove));
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), next.x, next.y, next.z, bot->GetOrientation(), delay, MovementPriority::MOVEMENT_NORMAL);
    return 1;
}

int32 MoveToInstancePortalAction::SwimTowards(AreaTrigger const* portal)
{
    // Coilfang wave 11: the portals lie ~100 yd under the Zangarmarsh lake on navmesh islands of their own (the
    // generator drops the lake floor), PathGenerator gives only a shortcut; a player (or its ghost) swims down.
    Map* map = bot->GetMap();
    LiquidData const portalLiquid =
        map->GetLiquidData(bot->GetPhaseMask(), portal->x, portal->y, portal->z, bot->GetCollisionHeight(), {});
    bool const portalWet = (portalLiquid.Status & MAP_LIQUID_STATUS_SWIMMING) != 0;
    bool const botWet = bot->Unit::IsInWater();
    if (!portalWet && !botWet)
        return 0;

    if (bot->isMoving() && IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return -1;

    G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    G3D::Vector3 const target(portal->x, portal->y, portal->z);
    auto legTowards = [&here](G3D::Vector3 const& goal)
    {
        G3D::Vector3 const line = goal - here;
        float const length = line.length();
        return length <= SWIM_LEG_STEP ? goal : here + line * (SWIM_LEG_STEP / length);
    };

    // In sight: straight there (the water carries the line, MoveStraight's ground check counts it).
    if (bot->IsWithinLOS(target.x, target.y, target.z + PORTAL_AIM_UP))
    {
        G3D::Vector3 const next = legTowards(G3D::Vector3(target.x, target.y, target.z + PORTAL_AIM_UP));
        if (MoveStraight(bot->GetMapId(), next.x, next.y, next.z, true))
            return 1;
    }

    // Else along the surface above the portal, then down. The surface is the portal's water, or the bot's own.
    float surface = portalWet ? portalLiquid.Level : INVALID_HEIGHT;
    if (surface <= INVALID_HEIGHT && botWet)
        surface = bot->GetLiquidData().Level;
    if (surface <= INVALID_HEIGHT)
        return 0;

    float const cruiseZ = surface - SWIM_UNDER_SURFACE;
    G3D::Vector3 goal;
    if (bot->GetExactDist2d(target.x, target.y) <= SWIM_DIVE_RANGE)
        goal = G3D::Vector3(target.x, target.y, target.z + PORTAL_AIM_UP);
    else if (here.z < cruiseZ - SWIM_UNDER_SURFACE)
        goal = G3D::Vector3(here.x, here.y, cruiseZ);  // up first: the way along the surface is open
    else
        goal = G3D::Vector3(target.x, target.y, cruiseZ);

    G3D::Vector3 const next = legTowards(goal);
    if (MoveStraight(bot->GetMapId(), next.x, next.y, next.z, true))
        return 1;
    return bot->isMoving() ? -1 : 0;
}

uint32 MoveToInstancePortalAction::FlyingMountSpell() const
{
    uint32 mount = 0;
    int32 bestSpeed = -1;
    for (auto const& [spellId, spell] : bot->GetSpellMap())
    {
        if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !info->HasAura(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED))
            continue;
        int32 speed = 0;
        for (uint8 effect = EFFECT_0; effect <= EFFECT_2; ++effect)
            if (info->Effects[effect].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED)
                speed = std::max(speed, info->Effects[effect].CalcValue());
        if (speed > bestSpeed)
        {
            bestSpeed = speed;
            mount = spellId;
        }
    }
    return mount;
}

// Where the revived bot could mount and fly from here (Tempest Keep's floating portals): a flying mount it knows and
// an area that allows flight - not Dalaran (AREA_FLAG_NO_FLY_ZONE), not the old continents, Northrend with Cold
// Weather Flying only.
bool MoveToInstancePortalAction::CanFlyFromHere() const
{
    uint32 const mount = FlyingMountSpell();
    SpellInfo const* info = mount ? sSpellMgr->GetSpellInfo(mount) : nullptr;
    if (!info)
        return false;
    uint32 const zoneId = bot->GetZoneId();
    uint32 const areaId = bot->GetAreaId();
    for (uint32 const id : {areaId, zoneId})
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(id);
        if (area && (!area->IsFlyable() || (area->flags & AREA_FLAG_NO_FLY_ZONE)))
            return false;
    }
    return bot->canFlyInZone(bot->GetMapId(), zoneId, info) &&
           info->CheckLocation(bot->GetMapId(), zoneId, areaId, bot) == SPELL_CAST_OK;
}

bool MoveToInstancePortalAction::FlyTo(AreaTrigger const* portal)
{
    uint32 const now = getMSTime();
    if (_flyPortal != portal->entry)
    {
        _flyPortal = portal->entry;
        _flyCruiseZ = 0.0f;
        _flyStage = 0;
    }

    if (!bot->HasIncreaseMountedFlightSpeedAura() && !bot->HasFlyAura())
    {
        _flyCruiseZ = 0.0f;
        _flyStage = 0;
        if (bot->IsNonMeleeSpellCast(false) || (_flyCastMs && getMSTimeDiff(_flyCastMs, now) < MOUNT_CAST_WAIT_MS))
            return true;  // mounting
        if (_flyCastMs && getMSTimeDiff(_flyCastMs, now) < MOUNT_RETRY_MS)
            return false;  // the last cast failed: not again right away

        uint32 const mount = FlyingMountSpell();
        if (!mount)
        {
            SelfTravel::ReportCannotMove(bot, "move to instance portal", "no walk to the portal and no flying mount",
                                         portal->x, portal->y, portal->z);
            return false;
        }
        if (bot->IsMounted())
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        if (bot->isMoving())
            bot->StopMoving();

        // A normal cast: cast time, outdoor and flyable-zone checks (Cold Weather Flying in Northrend).
        SpellCastResult const result = bot->CastSpell(bot, mount, false);
        _flyCastMs = now;
        if (result != SPELL_CAST_OK)
        {
            SelfTravel::ReportCannotMove(bot, "move to instance portal", "flying mount cast refused here", portal->x,
                                         portal->y, portal->z);
            return false;
        }
        return true;
    }
    _flyCastMs = 0;

    if (_flyCruiseZ <= 0.0f)
    {
        _flyCruiseZ = std::max(bot->GetPositionZ(), portal->z) + FLY_CRUISE_HEIGHT;
        _flyStage = 0;
        _flyOpenApproach = false;
        _flyApproach.Relocate(portal->x, portal->y, portal->z);
        // From the side the portal faces: a point out along the trigger's thin axis, either side, with a clear drop
        // from cruise height and a clear line into the trigger.
        Map* map = bot->GetMap();
        float const approachZ = portal->z + FLY_APPROACH_ABOVE;
        for (float const side : {1.0f, -1.0f})
        {
            float const ax = portal->x + std::cos(portal->orientation) * FLY_SIDE_DISTANCE * side;
            float const ay = portal->y + std::sin(portal->orientation) * FLY_SIDE_DISTANCE * side;
            LineOfSightChecks const checks = LINEOFSIGHT_ALL_CHECKS;
            if (map->isInLineOfSight(ax, ay, _flyCruiseZ, ax, ay, approachZ, bot->GetPhaseMask(), checks,
                                     VMAP::ModelIgnoreFlags::Nothing) &&
                map->isInLineOfSight(ax, ay, approachZ, portal->x, portal->y, approachZ, bot->GetPhaseMask(), checks,
                                     VMAP::ModelIgnoreFlags::Nothing))
            {
                _flyApproach.Relocate(ax, ay, approachZ);
                _flyOpenApproach = true;
                break;
            }
        }

        // No open side along the trigger's axis: any open floor round it at its height.
        if (!_flyOpenApproach)
        {
            LineOfSightChecks const checks = LINEOFSIGHT_ALL_CHECKS;
            for (float const ring : FLY_OPEN_RINGS)
            {
                for (uint32 i = 0; i < FLY_OPEN_DIRECTIONS && !_flyOpenApproach; ++i)
                {
                    float const angle = 2.0f * static_cast<float>(M_PI) * i / FLY_OPEN_DIRECTIONS;
                    G3D::Vector3 const probe(portal->x + std::cos(angle) * ring, portal->y + std::sin(angle) * ring,
                                             portal->z);
                    float floor = 0.0f;
                    if (!SelfTravel::NavmeshFloor(bot, probe, FLY_OPEN_FLOOR_HALF_HEIGHT, floor) ||
                        std::fabs(floor - portal->z) > FLY_OPEN_MAX_DZ)
                        continue;
                    float const z = floor + FLY_APPROACH_ABOVE;
                    if (map->isInLineOfSight(probe.x, probe.y, _flyCruiseZ, probe.x, probe.y, z, bot->GetPhaseMask(),
                                             checks, VMAP::ModelIgnoreFlags::Nothing) &&
                        map->isInLineOfSight(probe.x, probe.y, z, portal->x, portal->y, approachZ,
                                             bot->GetPhaseMask(), checks, VMAP::ModelIgnoreFlags::Nothing))
                    {
                        _flyApproach.Relocate(probe.x, probe.y, z);
                        _flyOpenApproach = true;
                    }
                }
                if (_flyOpenApproach)
                    break;
            }
        }
    }

    if (bot->isMoving())
        return true;

    uint32 const mapId = bot->GetMapId();
    float const approachFlat = bot->GetExactDist2d(&_flyApproach);
    if (_flyStage == 0 && approachFlat <= FLY_ARRIVE_DISTANCE &&
        bot->GetPositionZ() <= _flyApproach.GetPositionZ() + 3.0f)
        _flyStage = 1;
    if (_flyStage == 1)
        return MoveFlying(mapId, portal->x, portal->y, portal->z) || bot->isMoving();

    // Climb, cross at altitude, descend at the approach point: a straight line from the ground cuts through the
    // floating islands.
    if (approachFlat > FLY_ARRIVE_DISTANCE)
    {
        if (bot->GetPositionZ() < _flyCruiseZ - 3.0f)
            MoveFlying(mapId, bot->GetPositionX(), bot->GetPositionY(), _flyCruiseZ);
        else
            MoveFlying(mapId, _flyApproach.GetPositionX(), _flyApproach.GetPositionY(), _flyCruiseZ);
    }
    else
        MoveFlying(mapId, _flyApproach.GetPositionX(), _flyApproach.GetPositionY(), _flyApproach.GetPositionZ());
    return true;
}

bool MoveToInstancePortalAction::Execute(Event /*event*/)
{
    SelfTravel::PortalGoal anchor;
    uint32 const dest = SelfTravel::PortalDestination(botAI, &anchor);
    if (!dest)
        return false;

    bool const ghost = bot->HasPlayerFlag(PLAYER_FLAGS_GHOST);
    if (!ghost)
        _spiritHealerChosen = false;
    // The portal chosen for this walk stays chosen: a pick by straight-line distance flips between two exits as the
    // walk winds (Dire Maul East, Gnomeregan heroic: the bot turned back and forth between them mid-walk). Chosen
    // anew on another map, for another destination or goal, and after a walk that made no progress.
    AreaTrigger const* portal = nullptr;
    bool const goalInside = anchor.mapId == dest;
    // [b44_strat] An exact goal on the map the bot already stands in (a walked wing change whose entrance the group
    // reaches inside) is walked to directly while a complete walk through no closed door gets there: the way out
    // through an exit portal is only for a goal the walk inside does not reach. strat-n 20261026-075135-strat-n-5A-2
    // 2334-2452 s: the wing change to the main gate entrance (3395.1,-3380.2), the group stood 38 yd from it ("reached"
    // at 40 yd), the hunter Rlhunap at 40.4 yd took Stratholme's only exit 2221 at the Service Entrance, 206 yd back
    // through the closed King's Square / Service Entrance Gates: "no walk to the portal", and "move to instance portal"
    // <-> "stuck recovery" (its follow back to the leader) flipped every few seconds until the stop.
    if (!ghost && goalInside && anchor.exact && bot->GetMapId() == dest)
    {
        // Per bot, kept between ticks (NextRouteLeg searches again only off the route or for another goal); map
        // thread only, like the bot's AI.
        static thread_local std::unordered_map<ObjectGuid, SelfTravel::RouteWalk> insideWalks;
        SelfTravel::RouteWalk& walk = insideWalks[bot->GetGUID()];
        G3D::Vector3 leg;
        if (SelfTravel::NextRouteLeg(bot, walk, G3D::Vector3(anchor.x, anchor.y, anchor.z), 0.0f, PORTAL_LEG_STEP,
                                     leg) &&
            !PathThroughClosedDoor(bot, walk.corners).Found())
        {
            if (bot->GetExactDist(leg.x, leg.y, leg.z) > 1.0f && MoveTo(bot->GetMapId(), leg.x, leg.y, leg.z))
                return true;
            if (bot->isMoving())
                return true;
        }
    }
    if (_lockedPortal && _lockedMapId == bot->GetMapId() && _lockedDest == dest &&
        _lockedGoalInside == goalInside &&
        (!goalInside || std::hypot(anchor.x - _lockedGoalX, anchor.y - _lockedGoalY) <= GOAL_REACHED_RANGE))
    {
        portal = sObjectMgr->GetAreaTrigger(_lockedPortal);
        if (portal && portal->map != bot->GetMapId())
            portal = nullptr;
    }
    if (!portal)
    {
        portal = FindPortal(dest, goalInside ? &anchor : nullptr);
        _lockedPortal = portal ? portal->entry : 0;
        _lockedMapId = bot->GetMapId();
        _lockedDest = dest;
        _lockedGoalInside = goalInside;
        _lockedGoalX = anchor.x;
        _lockedGoalY = anchor.y;
    }
    if (!portal)
    {
        SelfTravel::ReportCannotMove(bot, "move to instance portal", "no portal on this map leads there", anchor.x,
                                     anchor.y, anchor.z);
        return false;
    }

    if (bot->IsInAreaTriggerRadius(portal))
        return EnterPortal(portal);

    // A walk that makes no progress for WALK_STUCK_MS is no walk.
    uint32 const now = getMSTime();
    if (!_progressMs || bot->GetMapId() != _progressMapId || bot->GetExactDist(&_progressPos) > PROGRESS_RANGE)
    {
        _progressPos = bot->GetPosition();
        _progressMapId = bot->GetMapId();
        _progressMs = now;
    }
    float const portalDistance = bot->GetExactDist(portal->x, portal->y, portal->z);
    if (!_gainMs || bot->GetMapId() != _gainMapId || _gainPortal != portal->entry ||
        portalDistance < _gainDistance - PROGRESS_RANGE)
    {
        _gainDistance = portalDistance;
        _gainMapId = bot->GetMapId();
        _gainPortal = portal->entry;
        _gainMs = now;
    }
    bool const circling = getMSTimeDiff(_gainMs, now) > WALK_NO_GAIN_MS;
    bool const stuck = getMSTimeDiff(_progressMs, now) > WALK_STUCK_MS || circling;

    // On the way to the spirit healer: no new try of the walk in between (SV-n wave 15: each window the walk to the
    // underwater portal started again, its spline kept the healer's MoveTo waiting, and every try reported "no
    // spirit healer" for 20 minutes).
    if (ghost && _spiritHealerChosen)
    {
        if (botAI->DoSpecificAction("spirit healer", Event(), true) || bot->isMoving())
            return true;
        _spiritHealerChosen = false;
        SelfTravel::ReportCannotMove(bot, "move to instance portal", "ghost: no walk to the spirit healer either",
                                     portal->x, portal->y, portal->z);
        return false;
    }

    bool const flying = bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura();
    if (!flying || ghost)
    {
        if (!stuck && WalkTowards(portal, ghost) != 0)
            return true;
        // No progress on this portal's walk: the next tick may choose another one.
        if (stuck)
            _lockedPortal = 0;
        if (stuck && HopOut(portal, ghost))
        {
            _progressPos = bot->GetPosition();
            _progressMs = now;
            _gainMs = 0;
            return true;
        }
        // A portal under water, or a swimmer the navmesh does not carry: it swims (stuck or not, a swim leg is
        // straight and needs no route).
        if (SwimTowards(portal) != 0)
            return true;
    }

    if (ghost)
    {
        // No walk to the portal (it floats above the world, or the ghost is stuck): the spirit healer at the
        // graveyard, as a player does - when the living bot then flies. The healer revives in place, with
        // Resurrection Sickness: without a flight from here the living bot has no walk either (Violet Hold wave 18:
        // the ghost took the Dalaran healer 30 s after its release, Dalaran is no-fly, and stood alive 332 yd from
        // the portal for 6 minutes; Scholomance wave 17: the group took the healer after 3 minutes of circling and
        // the next run started at 25 % stats). Then the ghost stays a ghost and the walk is tried again.
        bool const healerHelps = CanFlyFromHere();
        // Stuck for good (WALK_STUCK_MS without progress, or circling): the healer from now on, the walk stops.
        if (stuck && healerHelps)
        {
            _spiritHealerChosen = true;
            if (bot->isMoving())
                bot->StopMoving();
        }
        if (healerHelps && botAI->DoSpecificAction("spirit healer", Event(), true))
            return true;
        // Nothing worked this time: the walk is tried again in the next window instead of never.
        if (stuck)
        {
            _progressPos = bot->GetPosition();
            _progressMs = now;
            _gainMs = 0;
            _routeMs = 0;
        }
        char const* reason = nullptr;
        if (!healerHelps)
            reason = circling ? "ghost gets no nearer to the portal, no flight from here"
                     : stuck  ? "ghost stuck on the way, no flight from here"
                              : "ghost: no walk to the portal, no flight from here";
        else
            reason = circling ? "ghost gets no nearer to the portal, no spirit healer"
                     : stuck  ? "ghost stuck on the way, no spirit healer"
                              : "ghost: no walk to the portal, no spirit healer";
        SelfTravel::ReportCannotMove(bot, "move to instance portal", reason, portal->x, portal->y, portal->z);
        return false;
    }

    if (FlyTo(portal))
        return true;
    if (!flying)
        SelfTravel::ReportCannotMove(bot, "move to instance portal",
                                     circling ? "gets no nearer to the portal"
                                     : stuck  ? "stuck on the way to the portal"
                                              : "no walk to the portal",
                                     portal->x, portal->y, portal->z);
    // Tried again in the next window, not never.
    if (stuck)
    {
        _progressPos = bot->GetPosition();
        _progressMs = now;
        _gainMs = 0;
        _routeMs = 0;
    }
    return false;
}

bool LowBreathTrigger::IsActive()
{
    uint32 const now = getMSTime();
    uint32 const elapsed = _sampleMs ? std::min<uint32>(getMSTimeDiff(_sampleMs, now), BREATH_SAMPLE_MAX_MS) : 0;
    _sampleMs = now;

    int32 const maxMs = bot->IsAlive() ? BreathMaxMs(bot) : 0;
    if (maxMs <= 0)
    {
        _breathMs = -1;
        _surfacing = false;
        return false;
    }
    if (_breathMs < 0 || _breathMs > maxMs)
        _breathMs = maxMs;

    bool const under = bot->IsUnderWater();
    _breathMs = under ? std::max<int32>(0, _breathMs - static_cast<int32>(elapsed))
                      : std::min<int32>(maxMs, _breathMs + static_cast<int32>(elapsed) * BREATH_REFILL_RATE);

    if (under && _breathMs < maxMs / 100 * BREATH_SURFACE_PCT)
        _surfacing = true;
    else if (_surfacing && (!bot->Unit::IsInWater() || (!under && _breathMs >= maxMs / 100 * BREATH_RESUME_PCT)))
        _surfacing = false;
    return _surfacing;
}

bool SurfaceForAirAction::Execute(Event /*event*/)
{
    LiquidData const& liquid = bot->GetLiquidData();
    if (liquid.Level <= INVALID_HEIGHT)
        return false;

    if (bot->IsUnderWater())
        return MoveTo(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), liquid.Level + SURFACE_ABOVE_LEVEL,
                      false, false, false, true, MovementPriority::MOVEMENT_FORCED) ||
               bot->isMoving();

    // At the surface: only a dive back down waits (casting and fighting from the surface go on).
    if (bot->movespline->Initialized() && !bot->movespline->Finalized() &&
        bot->movespline->FinalDestination().z < liquid.Level - DIVE_DEPTH)
    {
        bot->StopMoving();
        return true;
    }
    return false;
}
