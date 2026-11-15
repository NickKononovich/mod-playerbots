/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteLegs.h"

#include "AggressiveTargetValue.h"
#include "AiObjectContext.h"
#include "AKTriggers.h"
#include "ANAddTactics.h"
#include "Bag.h"
#include "BotMoveEvents.h"
#include "CellImpl.h"
#include "Chat.h"
#include "CoSTriggers.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "DBCStores.h"
#include "Engine.h"
#include "EscortProtect.h"  // [b36_ohb] EscortGossipHeld
#include "EstimatedLifetimeValue.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GameTime.h"
#include "GDTriggers.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "RaidLeaderState.h"
#include "HoRTriggers.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MechShared.h"
#include "MgTShared.h"
#include "ClassicShared.h"  // [b32_smpull] Mograine's linked chapel trash
#include "MotionMaster.h"
#include "OCTriggers.h"
#include "MoveSpline.h"
#include "AttackerCountValues.h"
#include "MovementActions.h"
#include "LeaderOrders.h"  // [lead1] the tank's pull orders
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "LeaderKnowledge.h"
#include "RouteSearch.h"  // [lead99] the leader's route search
#include "PullFacts.h"    // [lead104] the leader's pull / fight facts
#include "LeaderFightChecks.h"  // [lead107] the leader's checks
#include "LeaderPlan.h"         // [lead107] the leader's planner tick
#include "Pet.h"  // [lead38] the pet redirect
#include <source_location>  // [lead76]
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "PullActions.h"  // [b39_strat] FightHold: the opening fight held on the use side of a key gate
#include "ChooseTargetActions.h"  // [b47_door] PullReadiness::GroupPreparing
#include "SmartAI.h"
#include "SmartScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StuckWatchdog.h"
#include "DamageTaken.h"
#include "DungeonHazard.h"   // [lead16] the leader's hazard view
#include "LeaderActions.h"   // [lead19] the member guard model (LeaderMoveOrderAction::WalkRefusal)
#include "RtiValue.h"        // [lead16] the leader's CC assignments
#include "DungeonTactics.h"  // [lead15] the leader's cleave dodge (InFrontalCleave)
#include "InstanceScript.h"
#include "InstanceSaveMgr.h"
#include "SelfTravelActions.h"
#include "TemporarySummon.h"
#include "TerrainHazard.h"
#include "Transport.h"
#include "UKTriggers.h"
#include "UPTriggers.h"
#include "Timer.h"
#include <chrono>  // [lead15] the leader cadence cost
#include <functional>
#include <optional>  // [lead18]
#include "WaypointMgr.h"
#include "WorldPacket.h"
#include "World.h"
#include "WorldSession.h"
#include "G3D/Vector4.h"  // [b32_brs] HopelessDetourLeg discs
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Leader::Travel
{
    using namespace Leader::Knowledge;

    // One path with one filter: the smooth path, the corner path when the smooth one ran out of points.
    uint32 BuildFilteredLeg(Player* owner, G3D::Vector3 const& start, G3D::Vector3 const& dest, bool fromOwner,
                            bool steep, Movement::PointsArray& points, G3D::Vector3& end, bool& straightPath)
    {
        for (bool const straight : {false, true})
        {
            PathGenerator path(owner);
            path.SetUseStraightPath(straight);
            if (steep)
            {
                // A real player walks the 50-60 degree slopes (PathGenerator::CreateFilter keeps them off the
                // headless filter only); lava and slime stay excluded.
                path.SetIncludeFlags(uint16(path.GetIncludeFlags() | NAV_GROUND_STEEP));
                path.SetExcludeFlags(uint16(path.GetExcludeFlags() & ~NAV_GROUND_STEEP));
            }
            bool const built = fromOwner ? path.CalculatePath(dest.x, dest.y, dest.z, false)
                                         : path.CalculatePath(start.x, start.y, start.z, dest.x, dest.y, dest.z,
                                                              false);
            uint32 const type = path.GetPathType();
            if (!straight && built && type == POINT_PATH_FAILED && ROUTE_CORNER_PATH)
                continue;  // the polygon corridor was found, only the 4 yd point walk ran out: corners instead
            straightPath = straight;
            points = path.GetPath();
            end = path.GetActualEndPosition();
            return built ? type : uint32(PATHFIND_NOPATH);
        }
        return PATHFIND_NOPATH;
    }

    // One PathGenerator path: the bot filter first; when it does not get there, the same with the steep slopes a
    // player walks (Steamvault wave 6: Kalithresh's hall joins the rest only over NAV_GROUND_STEEP polygons, so
    // Steamrigger was "unreachable" from it: tools/navmesh_probe.py 545 -91.5 -440.4 3.6 -330.1 -121.5 -8 --both).
    uint32 BuildLeg(Player* owner, G3D::Vector3 const& start, G3D::Vector3 const& dest, bool fromOwner,
                    Movement::PointsArray& points, G3D::Vector3& end, bool& straightPath, bool* steepUsed)
    {
        if (steepUsed)
            *steepUsed = false;
        uint32 const type = BuildFilteredLeg(owner, start, dest, fromOwner, false, points, end, straightPath);
        if (!(type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)))
            return type;

        Movement::PointsArray steepPoints;
        G3D::Vector3 steepEnd;
        bool steepStraight = false;
        uint32 const steepType =
            BuildFilteredLeg(owner, start, dest, fromOwner, true, steepPoints, steepEnd, steepStraight);
        bool const better = !(steepType & PATHFIND_NOPATH) &&
                            ((type & PATHFIND_NOPATH) || !(steepType & PATHFIND_INCOMPLETE) ||
                             (dest - steepEnd).length() + ROUTE_PROGRESS < (dest - end).length());
        if (!better)
            return type;
        points = steepPoints;
        end = steepEnd;
        straightPath = steepStraight;
        if (steepUsed)
            *steepUsed = true;
        return steepType;
    }

    // The liquid surface at a point that lies in or under water, INVALID_HEIGHT anywhere else.
    float WaterSurfaceAt(Player* walker, G3D::Vector3 const& point)
    {
        LiquidData const liquid = walker->GetMap()->GetLiquidData(walker->GetPhaseMask(), point.x, point.y,
                                                                  point.z, walker->GetCollisionHeight(), {});
        if (!(liquid.Status & (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER)) || liquid.Level <= INVALID_HEIGHT)
            return INVALID_HEIGHT;
        return liquid.Level;
    }

    float PolylineLength(Movement::PointsArray const& points)
    {
        float length = 0.0f;
        for (size_t index = 1; index < points.size(); ++index)
            length += (points[index] - points[index - 1]).length();
        return length;
    }

    // [kara1] An instance loads all its grids, and with them every navmesh tile its mmaps have (Map::OnCreateMap ->
    // LoadAllGrids -> GridTerrainLoader::LoadMMap). A point of an instance with a navmesh but no tile under it is a
    // hole in the mmaps data: the generator wrote no file for that tile (mmaps_generator "Too many vertices! N out
    // of 65535" skips the whole tile - Karazhan 5325235 / 5325236, the entrance hall to Moroes and the Opera,
    // data/log_mmaps.txt). PathGenerator then answers PATHFIND_NOT_USING_PATH (CalculatePath: !HaveTile) - a straight
    // line, through walls and floors.
    bool NavTileMissing(Map const* map, G3D::Vector3 const& point)
    {
        if (!map || !map->Instanceable())
            return false;
        dtNavMesh const* navMesh = map->GetMapCollisionData().GetMMapData().GetNavMesh();
        if (!navMesh)
            return false;  // a map without mmaps at all: the straight walk is the core's own movement
        // [nav26] a point outside the instance's own grids is a caller's error (another map's point), not a hole:
        // logged once per map and grid, never read as a missing tile (b375: Orgrimmar points on map 389)
        if (!map->IsGridLoaded(point.x, point.y))
        {
            static std::mutex lock;
            static std::set<std::tuple<uint32, int32, int32>> logged;
            int32 const gridX = int32(32.0f - point.x / SIZE_OF_GRIDS);
            int32 const gridY = int32(32.0f - point.y / SIZE_OF_GRIDS);
            bool fresh = false;
            {
                std::lock_guard<std::mutex> guard(lock);
                fresh = logged.insert({map->GetId(), gridX, gridY}).second;
            }
            // [jrn47] the level the journal takes: a point of another map paired with this one is a caller's
            // mistake inside the game's own data, read and refused here - the file is for system failures, and
            // this is counted by a query over pb_journal_line (tag [nav26]) like every other game state
            if (fresh)
                LOG_INFO("playerbots", "[nav26] a point ({:.1f} {:.1f} {:.1f}) outside map {}'s own grids (grid {} {}) "
                         "was asked for its navmesh tile: a caller paired another map's point with this map - no hole",
                         point.x, point.y, point.z, map->GetId(), gridX, gridY);
            return false;
        }
        float const detourPoint[3] = {point.y, point.z, point.x};
        int tileX = -1;
        int tileY = -1;
        navMesh->calcTileLoc(detourPoint, &tileX, &tileY);
        if (tileX < 0 || tileY < 0)
            return false;
        if (navMesh->getTileAt(tileX, tileY, 0))
            return false;
        // [nav-v2] MMAP_VERSION 21 splits a grid into N x N Detour tiles (tileWidth = SIZE_OF_GRIDS / N) and writes
        // none for an empty sub-tile. As PathGenerator::HaveTile (MMapMgr::HasGridTileAt), a point counts as on a
        // tile when any sub-tile of its 533 yd grid is loaded; with N = 1 this is the check above.
        int const perGrid = std::max(1, int(std::lround(SIZE_OF_GRIDS / navMesh->getParams()->tileWidth)));
        int const gridX = tileX - tileX % perGrid;
        int const gridY = tileY - tileY % perGrid;
        for (int y = gridY; y < gridY + perGrid; ++y)
            for (int x = gridX; x < gridX + perGrid; ++x)
                if (navMesh->getTileAt(x, y, 0))
                    return false;
        return true;
    }

    namespace
    {
        // [kara1] once per map tile and process: which mmtile file is missing
        void LogNavTileMissing(Player* from, G3D::Vector3 const& point, G3D::Vector3 const& start,
                               G3D::Vector3 const& dest)
        {
            static std::mutex lock;
            static std::set<std::tuple<uint32, int32, int32>> logged;
            int32 const gridX = int32(32.0f - point.x / SIZE_OF_GRIDS);
            int32 const gridY = int32(32.0f - point.y / SIZE_OF_GRIDS);
            {
                std::lock_guard<std::mutex> guard(lock);
                if (!logged.insert({from->GetMapId(), gridX, gridY}).second)
                    return;
            }
            LOG_INFO("playerbots", "[kara1] navmesh hole: map {} has mmaps but no tile at ({:.1f} {:.1f} {:.1f}): "
                     "mmaps/{:03}{:02}{:02}.mmtile missing (generator skipped it, see log_mmaps.txt 'Too many "
                     "vertices'); a straight route ({:.1f} {:.1f} {:.1f}) -> ({:.1f} {:.1f} {:.1f}) counts only in "
                     "line of sight ({})", from->GetMapId(), point.x, point.y, point.z, from->GetMapId(), gridX, gridY,
                     start.x, start.y, start.z, dest.x, dest.y, dest.z, from->GetName());
        }
    }

    // startAt: the route from that point instead of the owner's position (the far end of a RouteLinkDef).
    PathResult RoutePath(Player* from, float x, float y, float z, G3D::Vector3 const* startAt)
    {
        PathResult result;
        G3D::Vector3 const dest(x, y, z);
        G3D::Vector3 start(from->GetPositionX(), from->GetPositionY(), from->GetPositionZ());
        if (startAt)
            start = *startAt;
        float const straight = (dest - start).length();

        // [nav26] a destination outside an instance's own grids is another map's point (a caller's error, logged by
        // NavTileMissing): no walk, never the straight line toward it
        if (Map const* map = from->GetMap(); map && map->Instanceable() &&
                                             map->GetMapCollisionData().GetMMapData().GetNavMesh() &&
                                             !map->IsGridLoaded(dest.x, dest.y))
        {
            NavTileMissing(map, dest);  // its [nav26] line
            result.reachable = false;
            result.complete = false;
            result.points = {start};
            result.length = straight;
            result.end = start;
            return result;
        }

        Movement::PointsArray points;
        G3D::Vector3 end;
        result.type = BuildLeg(from, start, dest, !startAt, points, end, result.straightPath, &result.steep);
        if ((result.type & (PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH)) && !(result.type & PATHFIND_NOPATH))
        {
            // [kara1] A hole in an instance's mmaps (NavTileMissing): the straight line is a walk only where nothing
            // stands between (a wall, a floor). kara-n 20261108-193702-kara-n-10A-2: from the entrance (z 49.9) the
            // Ghostly Baker (z 77.4, the kitchen floor above) "reachable" 37 yd straight on, routeType none; the tank
            // walked under it and stood 27.5 yd below, 13 engage orders orphaned, the run stalled at the first trash.
            Map* const map = from->GetMap();
            bool const startHole = NavTileMissing(map, start);
            if (startHole || NavTileMissing(map, dest))
            {
                float const eye = from->GetCollisionHeight();
                if (!map->isInLineOfSight(start.x, start.y, start.z + eye, dest.x, dest.y, dest.z + eye,
                                          from->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                          VMAP::ModelIgnoreFlags::Nothing))
                {
                    LogNavTileMissing(from, startHole ? start : dest, start, dest);
                    result.navHole = true;
                    result.reachable = false;
                    result.complete = false;
                    result.points = {start};
                    result.length = straight;
                    result.end = start;
                    return result;
                }
            }
            // No navmesh here (map without mmaps, a tile not loaded) or flying / swimming: straight on.
            result.reachable = true;
            result.complete = true;
            result.points = points;
            result.length = straight;
            result.end = dest;
            return result;
        }
        bool const pathOk = !(result.type & PATHFIND_NOPATH);
        if (pathOk)
        {
            result.points = points;
            result.length = PolylineLength(points);
            result.end = end;
            if (!(result.type & PATHFIND_INCOMPLETE))
            {
                result.reachable = true;
                result.complete = true;
                return result;
            }
        }

        // Partial or none: the long search, bot filter first, then with the steep slopes.
        for (bool const steep : {false, true})
        {
            if (!ROUTE_LONG_SEARCH)
                break;
            Movement::PointsArray corners;
            bool complete = false;
            if (!LongRoute(from, start, dest, steep, corners, complete))
                continue;
            G3D::Vector3 const cornerEnd = corners.back();
            bool const nearer =
                !pathOk || (dest - cornerEnd).length() + ROUTE_PROGRESS < (dest - result.end).length();
            if (!complete && !nearer)
                continue;
            result.points = corners;
            result.length = PolylineLength(corners);
            result.end = cornerEnd;
            result.steep = steep;
            result.longRoute = true;
            result.straightPath = true;
            if (complete)
            {
                result.reachable = true;
                result.complete = true;
                return result;
            }
        }

        // [incomplete_route_deadend] A walker on a lake floor: the floor polygons can be a navmesh part of their
        // own, joined to the rest only through the water-surface polygons above. Maraudon heroic 2026-10-25
        // (20261025-163739-mara-h-5A-17): after Rotgrip, from the Earth Song Falls lake floor (23,-210,-205.6) the
        // floor part (786 polygons, tools/navmesh_probe.py 349 23 -209.8 -205.6 21 42.2 -125.7) ended under the
        // waterfall 124 yd short and 79 yd below Princess Theradras; the group swam there (low_breath 39 %) and
        // stood "own path refused". From the surface (z -198) the same search is complete (the ~1970 yd way back
        // up the south shore and the upper ledges, the one the group took once it had surfaced). The long search
        // once more from the surface above the walker; the walker rises first.
        if (ROUTE_SURFACE_START && ROUTE_LONG_SEARCH)
        {
            float const startSurface = WaterSurfaceAt(from, start);
            if (startSurface > INVALID_HEIGHT && startSurface > start.z + SURFACE_START_MIN_DEPTH)
            {
                G3D::Vector3 const up(start.x, start.y, startSurface - 0.5f);
                for (bool const steep : {false, true})
                {
                    Movement::PointsArray corners;
                    bool complete = false;
                    if (!LongRoute(from, up, dest, steep, corners, complete) || !complete || corners.empty())
                        continue;
                    result.points.clear();
                    result.points.push_back(start);
                    result.points.insert(result.points.end(), corners.begin(), corners.end());
                    result.length = PolylineLength(result.points);
                    result.end = corners.back();
                    result.steep = steep;
                    result.longRoute = true;
                    result.straightPath = true;
                    result.fromSurface = true;
                    result.reachable = true;
                    result.complete = true;
                    return result;
                }
            }
        }

        if (!pathOk && !result.longRoute)
            return result;

        // Still partial: a route that ends nearer to the target than the leader stands counts as the way there (the
        // next retarget walks on from closer). Several instance navmeshes are cut into parts the bots still walk
        // between (ramps and ledges without polygons: Ahn'kahet's entrance, Nadox's, Taldaram's, Jedoga's and
        // Volazj's halls are five parts 20-98 yd apart; Utgarde Pinnacle 451769 polygons in pieces), so a stricter
        // rule would strand them. Areas closed until an event (Gundrak: Gal'darah's wing before the bridge) are
        // kept out by AreaGateDef instead.
        float const left = (dest - result.end).length();
        // Or it ends next to the target, a ramp or stairs away (a gap without polygons the leader walks over,
        // Steer: gap_cross / the target as the waypoint / steer_stuck). Deadmines wave 8: from the ship's middle
        // deck (a 144-polygon part) VanCleef was 21.7 yd away, the route ended 16.9 yd from him - "unreachable".
        bool const besideGap = left <= GAP_REACHABLE_LEFT && std::fabs(dest.z - result.end.z) <= GAP_REACHABLE_DZ;
        // Or it ends in water below the target (the navmesh's water polygons follow the lake floor): a swimmer
        // rises to it. Slave Pens wave 9/10: the route to Rokmar the Crackler ended on the lake floor at z -14.2,
        // 14.9 yd from him at the surface (dz 16.4) - "unreachable", the run stalled at 1/3 (wave 8: done).
        float const surface = ROUTE_SWIM_UP ? WaterSurfaceAt(from, result.end) : INVALID_HEIGHT;
        bool const swimUp = surface > INVALID_HEIGHT && left <= GAP_REACHABLE_LEFT && dest.z >= result.end.z &&
                            dest.z <= surface + GAP_SWIM_ABOVE;
        result.reachable = left + 5.0f < straight || besideGap || swimUp;
        result.length += left;
        return result;
    }

    PathResult EvaluatePath(Player* from, WorldObject* to)
    {
        // [nav26] the target's point and the walker's map from the same frame: a target on another map (b375 the
        // five's dead leader Brelir released to Orgrimmar's graveyard, map 1 (1177.8,-4464.2), while its members
        // stood in Ragefire Chasm, map 389) is no walk at all - before, its map-1 point was searched on map 389's
        // navmesh, read as three "missing" tiles (3892940 / 3892840 / 3892839) and walked straight, +826 yd
        if (!from || !to || to->GetMapId() != from->GetMapId() || to->GetInstanceId() != from->GetInstanceId())
        {
            if (from && to)
            {
                static std::mutex lock;
                static std::set<std::pair<ObjectGuid, ObjectGuid>> logged;
                bool fresh = false;
                {
                    std::lock_guard<std::mutex> guard(lock);
                    fresh = logged.insert({from->GetGUID(), to->GetGUID()}).second;
                }
                if (fresh)
                    LOG_INFO("playerbots", "[nav26] {} (map {}) asked for a walk to {} on map {} (instance {} / {}): no "
                             "walk across maps",
                             from->GetName(), from->GetMapId(), to->GetName(), to->GetMapId(), from->GetInstanceId(),
                             to->GetInstanceId());
            }
            PathResult none;
            none.reachable = false;
            none.complete = false;
            if (from)
            {
                G3D::Vector3 const here(from->GetPositionX(), from->GetPositionY(), from->GetPositionZ());
                none.points = {here};
                none.end = here;
            }
            return none;
        }
        return RoutePath(from, to->GetPositionX(), to->GetPositionY(), to->GetPositionZ());
    }

    PathResult DoorFreeRoute(Player* from, G3D::Vector3 const& dest, std::vector<GameObject*> const& doors)
    {
        PathResult result;
        if (!ROUTE_LONG_SEARCH || doors.empty())
            return result;
        std::vector<G3D::AABox> boxes;
        for (GameObject* door : doors)
            if (door && door->m_model)
            {
                G3D::AABox const& bounds = door->m_model->GetBounds();
                G3D::Vector3 const margin(PASSAGE_DOOR_MARGIN, PASSAGE_DOOR_MARGIN, PASSAGE_DOOR_MARGIN);
                boxes.emplace_back(bounds.low() - margin, bounds.high() + margin);
            }
        if (boxes.empty())
            return result;
        G3D::Vector3 const start(from->GetPositionX(), from->GetPositionY(), from->GetPositionZ());
        for (bool const steep : {false, true})
        {
            Movement::PointsArray corners;
            bool complete = false;
            if (!LongRoute(from, start, dest, steep, corners, complete, &boxes) || !complete || corners.size() < 2)
                continue;
            result.points = corners;
            result.length = PolylineLength(corners);
            result.end = corners.back();
            result.steep = steep;
            result.longRoute = true;
            result.straightPath = true;
            result.reachable = true;
            result.complete = true;
            result.type = PATHFIND_NORMAL;
            return result;
        }
        return result;
    }

    std::unordered_map<ObjectGuid, CachedLongRoute> longRouteCache;  // world thread only

    // The route's points from where the walker stands now: from the segment nearest to it on.
    Movement::PointsArray TrimRoute(Movement::PointsArray const& points, G3D::Vector3 const& here)
    {
        if (points.size() < 2)
            return points;
        size_t best = 0;
        float bestDist = -1.0f;
        for (size_t index = 0; index + 1 < points.size(); ++index)
        {
            G3D::Vector3 const segment = points[index + 1] - points[index];
            float const lengthSq = segment.squaredLength();
            float t = lengthSq > 0.0f ? (here - points[index]).dot(segment) / lengthSq : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            float const dist = (points[index] + segment * t - here).length();
            if (bestDist < 0.0f || dist < bestDist)
            {
                bestDist = dist;
                best = index;
            }
        }
        Movement::PointsArray trimmed;
        trimmed.push_back(here);
        trimmed.insert(trimmed.end(), points.begin() + std::ptrdiff_t(best + 1), points.end());
        return trimmed;
    }

    std::unordered_map<ObjectGuid, PassageLegMemo> passageLegMemos;  // world thread only

    bool PassageDetourLeg(Player* owner, G3D::Vector3 const& here, G3D::Vector3 const& goal, PathResult& leg)
    {
        Map* map = owner->GetMap();
        InstanceMap* instance = map ? map->ToInstanceMap() : nullptr;
        InstanceScript const* script = instance ? instance->GetInstanceScript() : nullptr;
        if (!script)
            return false;
        uint32 const now = getMSTime();
        PassageLegMemo& memo = passageLegMemos[owner->GetGUID()];
        if (!memo.ms || getMSTimeDiff(memo.ms, now) > PASSAGE_LEG_REUSE_MS ||
            (memo.goal - goal).length() > LONG_ROUTE_SAME_GOAL)
        {
            memo.goal = goal;
            memo.ms = now ? now : 1;
            memo.round = false;
            std::vector<GameObject*> const closed = ClosedDoors(map, owner);
            std::vector<GameObject*> passage;
            for (GameObject* door : closed)
                if (PassageDoorBoss(script, door) >= 0)
                    passage.push_back(door);
            if (!passage.empty() && RouteThroughClosedDoor(leg.points, passage, owner->GetPhaseMask()))
            {
                memo.route = DoorFreeRoute(owner, goal, closed);
                memo.round = memo.route.reachable;
            }
        }
        if (!memo.round || memo.route.points.size() < 2)
            return false;
        leg.points = TrimRoute(memo.route.points, here);
        leg.end = memo.route.end;
        leg.steep = memo.route.steep;
        leg.straightPath = true;
        leg.type = uint32(PATHFIND_NORMAL);
        return true;
    }

    uint32 SteerLeg(Player* owner, G3D::Vector3 const& goal, PathResult& leg)
    {
        G3D::Vector3 const here(owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ());
        leg.type = BuildLeg(owner, here, goal, true, leg.points, leg.end, leg.straightPath, &leg.steep);
        leg.partial = (leg.type & PATHFIND_INCOMPLETE) != 0;  // [incomplete_route_deadend]
        leg.fromSurface = false;
        // [b32_brs] the way round a pack the planner refused as hopeless, when there is one
        if (!(leg.type & PATHFIND_NOPATH) && leg.points.size() >= 2 && HopelessDetourLeg(owner, here, goal, leg))
        {
            leg.partial = false;
            return leg.type;
        }
        if (ROUTE_PASSAGE_DOOR_DETOUR_LEG && !(leg.type & PATHFIND_NOPATH) && leg.points.size() >= 2 &&
            PassageDetourLeg(owner, here, goal, leg))
        {
            leg.partial = false;  // [incomplete_route_deadend] DoorFreeRoute is complete only
            return leg.type;
        }
        if (!(leg.type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) || !ROUTE_LONG_SEARCH)
            return leg.type;

        uint32 const now = getMSTime();
        CachedLongRoute& cached = longRouteCache[owner->GetGUID()];
        if (!cached.builtMs || getMSTimeDiff(cached.builtMs, now) > LONG_ROUTE_REUSE_MS ||
            (cached.goal - goal).length() > LONG_ROUTE_SAME_GOAL)
        {
            cached.goal = goal;
            cached.route = RoutePath(owner, goal.x, goal.y, goal.z);
            cached.builtMs = now;
        }
        PathResult const& route = cached.route;
        if (route.longRoute && route.points.size() >= 2)
        {
            leg.points = TrimRoute(route.points, here);
            leg.end = route.end;
            leg.steep = route.steep;
            leg.straightPath = true;
            leg.type = route.reachable ? uint32(PATHFIND_NORMAL) : uint32(PATHFIND_INCOMPLETE);
            leg.partial = !route.complete;           // [incomplete_route_deadend] a reachable partial too
            leg.fromSurface = route.fromSurface;
        }
        return leg.type;
    }

    // The leader's next waypoint on a route: step yd along its first leg. A corner path has points far apart
    // (one per corner); the line between two corners is on the navmesh, so the waypoint is taken on it.
    G3D::Vector3 RouteWaypoint(PathResult const& route, G3D::Vector3 const& fallback, float step)
    {
        G3D::Vector3 waypoint = fallback;
        float walked = 0.0f;
        for (size_t index = 1; index < route.points.size(); ++index)
        {
            G3D::Vector3 const segment = route.points[index] - route.points[index - 1];
            float const length = segment.length();
            if (walked + length >= step && length > 0.0f)
                return route.points[index - 1] + segment * ((step - walked) / length);
            walked += length;
            waypoint = route.points[index];
        }
        return waypoint;
    }

    bool NavmeshFloor(Player* walker, G3D::Vector3 const& point, float& floorZ)
    {
        dtNavMesh const* navMesh = walker->GetMap()->GetMapCollisionData().GetMMapData().GetNavMesh();
        if (!navMesh)
            return false;
        // One small query per navmesh (findNearestPoly needs no node pool), world thread only like LongRoute's.
        static std::unordered_map<dtNavMesh const*, std::unique_ptr<dtNavMeshQuery, NavMeshQueryFree>> snaps;
        auto& query = snaps[navMesh];
        if (!query)
        {
            query.reset(dtAllocNavMeshQuery());
            if (!query || dtStatusFailed(query->init(navMesh, 64)))
            {
                query.reset();
                return false;
            }
        }
        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
        filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));
        float const center[3] = {point.y, point.z, point.x};
        float const extents[3] = {FLOOR_SNAP_HALF_WIDTH, FLOOR_SNAP_HALF_HEIGHT, FLOOR_SNAP_HALF_WIDTH};
        float nearest[3];
        dtPolyRef ref = 0;
        if (dtStatusFailed(query->findNearestPoly(center, extents, &filter, &ref, nearest)) || !ref)
            return false;
        floorZ = nearest[1];
        return true;
    }

    G3D::Vector3 FloorWaypoint(Player* walker, PathResult const& leg, G3D::Vector3 waypoint)
    {
        if (!leg.straightPath)
            return waypoint;
        float const floor = walker->GetMap()->GetHeight(walker->GetPhaseMask(), waypoint.x, waypoint.y,
                                                        waypoint.z + 3.0f, true, 10.0f);
        // Without a vmap floor within the search GetHeight answers the ADT ground at any depth (Utgarde Pinnacle
        // wave 11: a waypoint on the ledge became the terrain 30 yd below it): only a floor near the route counts.
        if (floor <= INVALID_HEIGHT || std::fabs(floor - waypoint.z) > FLOOR_WAYPOINT_MAX_DZ)
        {
            float snapped = 0.0f;
            if (NavmeshFloor(walker, waypoint, snapped))
                waypoint.z = snapped;
            return waypoint;
        }
        float const surface =
            ROUTE_SWIM_UP ? WaterSurfaceAt(walker, G3D::Vector3(waypoint.x, waypoint.y, floor)) : INVALID_HEIGHT;
        if (surface > INVALID_HEIGHT && surface > floor + 1.0f)
            waypoint.z = std::clamp(waypoint.z, floor, surface);
        else
            waypoint.z = floor;
        return waypoint;
    }

    std::unordered_map<ObjectGuid, std::vector<HopelessMob>> hopelessPacks;  // leader -> mobs, world thread

    std::unordered_map<ObjectGuid, HopelessLegMemo> hopelessLegMemos;  // world thread only

    void NoteHopelessPack(Player* leader, Creature* mob)
    {
        if (!leader || !mob || mob->IsDungeonBoss())
            return;
        uint32 const now = getMSTime();
        std::list<Creature*> pack;
        PackMobCheck check{leader, mob};
        Acore::CreatureListSearcher<PackMobCheck> searcher(mob, pack, check);
        Cell::VisitObjects(mob, searcher, PACK_RADIUS);
        if (std::find(pack.begin(), pack.end(), mob) == pack.end())
            pack.push_back(mob);
        std::vector<HopelessMob>& list = hopelessPacks[leader->GetGUID()];
        for (Creature* member : pack)
        {
            if (member->IsDungeonBoss())
                continue;
            auto const found = std::find_if(list.begin(), list.end(), [member](HopelessMob const& known)
                                            { return known.guid == member->GetGUID(); });
            if (found != list.end())
                found->sinceMs = now ? now : 1;
            else
                list.push_back({member->GetGUID(), now ? now : 1});
        }
    }

    bool InDisc(G3D::Vector3 const& point, G3D::Vector4 const& disc)
    {
        return (point.x - disc.x) * (point.x - disc.x) + (point.y - disc.y) * (point.y - disc.y) <=
                   disc.w * disc.w &&
               std::fabs(point.z - disc.z) <= HOPELESS_AVOID_DZ;
    }

    bool HopelessDetourLeg(Player* owner, G3D::Vector3 const& here, G3D::Vector3 const& goal, PathResult& leg)
    {
        if (!ROUTE_LONG_SEARCH || owner->IsInCombat())
            return false;
        // [b32_smpull] An idle Mograine with his chapel is walked round at his keep-away ring (the trash first,
        // LinkedChapelTrash; a walk past him engages him and his whole chapel): keep_away_detour.
        std::vector<G3D::Vector4> discs;
        bool keepAway = false;
        if (Unit* mograine = ScarletMonastery::IdleMograineWithChapel(owner))
        {
            discs.emplace_back(mograine->GetPositionX(), mograine->GetPositionY(), mograine->GetPositionZ(),
                               ScarletMonastery::MOGRAINE_KEEP_AWAY + HOPELESS_AVOID_MARGIN);
            keepAway = true;
        }
        auto const listed = hopelessPacks.find(owner->GetGUID());
        if ((listed == hopelessPacks.end() || listed->second.empty()) && discs.empty())
            return false;
        uint32 const now = getMSTime();
        Map* map = owner->GetMap();
        std::vector<HopelessMob> noList;
        std::vector<HopelessMob>& list = listed != hopelessPacks.end() ? listed->second : noList;
        for (auto it = list.begin(); it != list.end();)
        {
            Creature* mob = map ? map->GetCreature(it->guid) : nullptr;
            if (!mob || !mob->IsAlive() || getMSTimeDiff(it->sinceMs, now) > HOPELESS_AVOID_MS)
            {
                it = list.erase(it);
                continue;
            }
            if (!mob->IsInCombat())
                discs.emplace_back(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ(),
                                   mob->GetAggroRange(owner) + HOPELESS_AVOID_MARGIN);
            ++it;
        }
        if (discs.empty())
            return false;
        // the walk goes to that pack, or starts inside its reach: nothing to go round
        for (G3D::Vector4 const& disc : discs)
            if (InDisc(goal, disc) || InDisc(here, disc))
                return false;
        // only a leg that enters one of them
        bool enters = false;
        for (size_t index = 1; index < leg.points.size() && !enters; ++index)
        {
            G3D::Vector3 const& from = leg.points[index - 1];
            G3D::Vector3 const segment = leg.points[index] - from;
            float const lengthSq = segment.x * segment.x + segment.y * segment.y;
            for (G3D::Vector4 const& disc : discs)
            {
                float t = lengthSq > 0.0f
                              ? ((disc.x - from.x) * segment.x + (disc.y - from.y) * segment.y) / lengthSq
                              : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                if (InDisc(from + segment * t, disc))
                {
                    enters = true;
                    break;
                }
            }
        }
        if (!enters)
            return false;
        HopelessLegMemo& memo = hopelessLegMemos[owner->GetGUID()];
        if (!memo.ms || getMSTimeDiff(memo.ms, now) > LONG_ROUTE_REUSE_MS ||
            (memo.goal - goal).length() > LONG_ROUTE_SAME_GOAL || memo.mobs != discs.size())
        {
            memo.goal = goal;
            memo.ms = now ? now : 1;
            memo.mobs = discs.size();
            memo.round = false;
            memo.route = PathResult();
            float const legWalk = PolylineLength(leg.points);
            // [b46_strat] the way round keeps out of closed doors too (as DoorFreeRoute does): strat-n
            // 20261026-093620-strat-n-5A-3 1382-1788 s, the round of the hopeless Market Row chain ran south
            // through the shut King's Square Gates while PassageDetourLeg sent the leader north round the gates
            // into that chain - the leader turned back and forth at (3710,-3465) for 6 minutes.
            std::vector<G3D::AABox> doorBoxes;
            for (GameObject* door : ClosedDoors(map, owner))
                if (door && door->m_model)
                {
                    G3D::AABox const& bounds = door->m_model->GetBounds();
                    G3D::Vector3 const margin(PASSAGE_DOOR_MARGIN, PASSAGE_DOOR_MARGIN, PASSAGE_DOOR_MARGIN);
                    doorBoxes.emplace_back(bounds.low() - margin, bounds.high() + margin);
                }
            for (bool const steep : {false, true})
            {
                Movement::PointsArray corners;
                bool complete = false;
                if (!LongRoute(owner, here, goal, steep, corners, complete,
                               doorBoxes.empty() ? nullptr : &doorBoxes, &discs) ||
                    !complete || corners.size() < 2)
                    continue;
                float const walk = PolylineLength(corners);
                if (walk > legWalk * HOPELESS_DETOUR_MAX_RATIO + 60.0f)
                    break;
                memo.route.points = corners;
                memo.route.length = walk;
                memo.route.end = corners.back();
                memo.route.steep = steep;
                memo.route.longRoute = true;
                memo.route.straightPath = true;
                memo.route.reachable = true;
                memo.route.complete = true;
                memo.route.type = PATHFIND_NORMAL;
                memo.round = true;
                // [b32_smpull] the planner calls a pack hopeless only after its split search failed
                // (ChooseTargetActions SPLIT_FAILS_HOPELESS): this detour is the last resort
                LOG_INFO("playerbots", "[b32_brs] {}: {}: {:.0f} yd round {} refused mob(s){} instead of {:.0f} "
                         "yd through them, to ({:.1f},{:.1f},{:.1f})", owner->GetName(),
                         keepAway && discs.size() == 1 ? "keep_away_detour" : "hopeless_detour", walk,
                         discs.size(), keepAway ? " (Mograine's keep-away ring)" : "", legWalk, goal.x, goal.y,
                         goal.z);
                break;
            }
        }
        if (!memo.round || memo.route.points.size() < 2)
            return false;
        leg.points = TrimRoute(memo.route.points, here);
        leg.end = memo.route.end;
        leg.steep = memo.route.steep;
        leg.straightPath = true;
        leg.type = uint32(PATHFIND_NORMAL);
        return true;
    }

    std::vector<GameObject*> ClosedDoors(Map* map, Player* leader)
    {
        std::vector<GameObject*> doors;
        if (!ROUTE_CLOSED_DOORS)
            return doors;
        for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
            if (go && go->IsInWorld() && go->isSpawned() && go->GetGoType() == GAMEOBJECT_TYPE_DOOR &&
                go->GetGoState() == GO_STATE_READY && go->m_model && go->m_model->isEnabled() &&
                go->InSamePhase(leader))
                doors.push_back(go);
        return doors;
    }

    int32 ShutForGoodDoorSlot(InstanceScript const* script, GameObject* door)
    {
        constexpr uint32 SHUT_FOR_GOOD_LOG_MS = 30 * IN_MILLISECONDS;
        static uint32 loggedMs = 0;  // world thread only
        for (ShutForGoodDoorDef const& def : SHUT_FOR_GOOD_DOORS)
            if (door->GetMapId() == def.mapId && door->GetEntry() == def.doorEntry &&
                script->IsBossDone(def.doneSlot))
            {
                uint32 const now = getMSTime();
                if (!loggedMs || getMSTimeDiff(loggedMs, now) > SHUT_FOR_GOOD_LOG_MS)
                {
                    loggedMs = now ? now : 1;
                    LOG_INFO("playerbots", "[w24c-shut-door-detour] map {}: {} {} stays shut after boss slot {} "
                                           "(DONE): the route goes round it",
                             def.mapId, door->GetName(), door->GetEntry(), def.doneSlot);
                }
                return int32(def.doneSlot);
            }
        return -1;
    }

    int32 PassageDoorBoss(InstanceScript const* script, GameObject* door)
    {
        if (!ROUTE_PASSAGE_DOOR_DETOUR || !script || !door)
            return -1;
        for (uint32 id = 0; id < script->GetEncounterCount(); ++id)
            if (BossInfo const* info = script->GetBossInfo(id);
                info && info->state != DONE && info->door[DOOR_TYPE_PASSAGE].count(door))
                return int32(id);
        // [brd_ring_gate_round] a gate an event's DONE opens (no DoorData row): until then the way round it.
        // Blackrock Depths' ArenaGate03 161524 (GO_ARENA3, the Ring of Law's north exit; npc_grimstone opens it
        // at the end, the entry ArenaGate04 161523 stands open): a walk to the arena from the north ran into it
        // (2026-10-25 brd-h 3657-3759 s door_hold at 588.3,-144.4 until the stop) - the way in is the west
        // corridor and ArenaGate04.
        if (door->GetMapId() == BRD_MAP_ID && door->GetEntry() == BRD_GO_ARENA_EXIT_GATE &&
            script->GetData(BRD_TYPE_RING_OF_LAW) != DONE)
            return int32(BRD_TYPE_RING_OF_LAW);
        // [w24c-shut-door-detour] the way round, too (the callers only ask whether there is a slot).
        return ShutForGoodDoorSlot(script, door);
    }

    // The first closed door the route's polyline passes through (a ray along each segment, DOOR_RAY_HEIGHT above
    // it, against the door's own collision model), nullptr: none.
    GameObject* RouteThroughClosedDoor(Movement::PointsArray const& points, std::vector<GameObject*> const& doors,
                                       uint32 phaseMask)
    {
        for (GameObject* door : doors)
        {
            // [b41_gnomer] a route that stays in front of a tunnel plug (Gnomeregan's Cave Ins) only clips its
            // edge (TerrainHazard DoorEdgeClipOnly, the same rule as the bots' own walk guard).
            if (points.size() >= 2 && DoorEdgeClipOnly(door, points.front(), points.back()))
                continue;
            G3D::AABox const& bounds = door->m_model->GetBounds();
            for (size_t index = 1; index < points.size(); ++index)
            {
                G3D::Vector3 const from = points[index - 1] + G3D::Vector3(0.0f, 0.0f, DOOR_RAY_HEIGHT);
                G3D::Vector3 const to = points[index] + G3D::Vector3(0.0f, 0.0f, DOOR_RAY_HEIGHT);
                G3D::Vector3 const low = from.min(to) - G3D::Vector3(DOOR_SEGMENT_MARGIN, DOOR_SEGMENT_MARGIN,
                                                                     DOOR_SEGMENT_MARGIN);
                G3D::Vector3 const high = from.max(to) + G3D::Vector3(DOOR_SEGMENT_MARGIN, DOOR_SEGMENT_MARGIN,
                                                                      DOOR_SEGMENT_MARGIN);
                if (high.x < bounds.low().x || low.x > bounds.high().x || high.y < bounds.low().y ||
                    low.y > bounds.high().y || high.z < bounds.low().z || low.z > bounds.high().z)
                    continue;
                G3D::Vector3 const segment = to - from;
                float distance = segment.length();
                if (distance <= 0.0f)
                    continue;
                G3D::Ray const ray(from, segment / distance);
                if (door->m_model->intersectRay(ray, distance, true, phaseMask, VMAP::ModelIgnoreFlags::Nothing))
                    return door;
            }
        }
        return nullptr;
    }

    DoorEventDef const* DoorEventOf(GameObject const* door)
    {
        for (DoorEventDef const* def : DoorEvents(door->GetMapId()))
            if (def->doorEntry == door->GetEntry())
                return def;
        return nullptr;
    }

    std::unordered_map<ObjectGuid, DoorStopMemo> doorStopMemos;  // world thread only

    // true: the waypoint ran through a closed door, `point` is now the hold before it (the leader's own spot when
    // the door is closer than DOOR_HOLD_BACK + DOOR_HOLD_MIN_WALK); door / walked say which and how far on.
    bool LeaderDoorStop(Player* leader, G3D::Vector3& point, ObjectGuid& door, float& walked,
                        std::function<bool(GameObject*)> const& beforeOpen)
    {
        if (!ROUTE_CLOSED_DOORS_ALL)
            return false;
        Map* map = leader->GetMap();
        if (!map || !map->IsDungeon() || leader->GetVehicle() || leader->GetTransport() || leader->IsFlying())
            return false;

        uint32 const now = getMSTime();
        DoorStopMemo& memo = doorStopMemos[leader->GetGUID()];
        if (memo.ms && getMSTimeDiff(memo.ms, now) < DOOR_HOLD_MEMO_MS &&
            (memo.asked - point).length() < DOOR_HOLD_MEMO_DISTANCE)
        {
            if (memo.door.IsEmpty())
                return false;
            point = memo.held;
            door = memo.door;
            walked = memo.walked;
            return true;
        }
        memo.asked = point;
        memo.ms = now ? now : 1;
        memo.door.Clear();

        // The cheap gate (TerrainHazard: the map's door list, bounds only): no closed door near the line.
        if (!ClosedDoorNear(leader, leader->GetPositionX(), leader->GetPositionY(), point.x, point.y,
                            CLOSED_DOOR_SEARCH))
            return false;

        G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        Movement::PointsArray points;
        {
            PathGenerator path(leader);
            path.CalculatePath(point.x, point.y, point.z, false);
            if (!(path.GetPathType() & PATHFIND_NOPATH))
                points = path.GetPath();
        }
        if (points.size() < 2)
            points = {here, point};  // the own walk's straight fallback
        ClosedDoorHit const hit = PathThroughClosedDoor(leader, points);
        if (!hit.Found())
            return false;

        // A door that opens by use (DoorOpensFor): the leader clicks it once it stands in reach, out of combat
        // ("[door] <leader>: opens <name> <entry>"), and the waypoint goes on; until then the hold is closer, in
        // reach. The followers' own walks stop before it (MovementAction::GuardClosedDoor) until it is open.
        bool const openable = !DoorEventOf(hit.door) && DoorOpensByUse(hit.door);
        // [kara2] a door that closes again by itself is clicked with the group gathered at the leader (DoorGatherHolds):
        // kara-n b355 20261108-231538-10A-1, the leader clicked the Gatehouse Door 183450 (closes after 3 s) at 6 s
        // while the warlock cast its soulstone and the druid stood 11 yd back; both were shut out, the group held 240 s.
        if (openable && beforeOpen && hit.door->IsWithinDistInMap(leader, hit.door->GetInteractionDistance()) &&
            beforeOpen(hit.door))
        {
            memo.ms = 0;  // judged again next time: the gather may be over
            point = here;
            door = hit.door->GetGUID();
            walked = hit.walked;
            return true;
        }
        if (openable && OpenDoorByUse(leader, hit.door))
        {
            memo.ms = 0;  // judged again next time: the walk may run through another door further on
            return false;
        }

        float const stop = hit.walked - (openable ? DOOR_OPEN_BACK : DOOR_HOLD_BACK);
        memo.held = stop >= DOOR_HOLD_MIN_WALK ? PointOnPath(points, stop) : here;
        memo.door = hit.door->GetGUID();
        memo.walked = hit.walked;
        point = memo.held;
        door = memo.door;
        walked = memo.walked;
        return true;
    }
}
