/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_ROUTELEGS_H
#define PLAYERBOTS_LEADER_ROUTELEGS_H

// [lead107] user: production code does not live in the test harness. The leader's route legs, moved out of
// TestHarnessRunner.cpp unchanged: the route's path (RoutePath / EvaluatePath, the long search and its cache, the
// smooth and filtered legs, the water surface start), the closed-door legs (DoorFreeRoute, the passage door detour,
// the door stop memo, LeaderDoorStop), the hopeless pack's detour leg, the route's waypoint along it (SteerLeg,
// RouteWaypoint, FloorWaypoint on the navmesh floor), and the facts they share (PackMobCheck, PACK_RADIUS, the
// shut-for-good doors).

#include "Creature.h"
#include "G3D/Vector3.h"
#include "G3D/Vector4.h"
#include "GameObject.h"
#include "MoveSplineInitArgs.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Position.h"
#include "LeaderKnowledge.h"
#include "RouteSearch.h"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace Leader::Travel
{
    using Knowledge::DoorEventDef;  // LeaderKnowledge.h

    // [brd_ring_*] Blackrock Depths' Ring of Law (blackrock_depths.h / .cpp at_ring_of_law,
    // instance_blackrock_depths.cpp).
    constexpr uint32 BRD_MAP_ID = 230;

    constexpr uint32 BRD_TYPE_RING_OF_LAW = 1;

    constexpr uint32 BRD_GO_ARENA_EXIT_GATE = 161524;  // GO_ARENA3 "ArenaGate03", opened at the Ring's DONE

    // RoutePath: a partial route ending this close to the target (and this little below / above it) counts.
    constexpr float GAP_REACHABLE_LEFT = 22.0f;

    constexpr float GAP_REACHABLE_DZ = 12.0f;

    // ... or it ends in water and the target stands at most this high above that water's surface (swims up).
    constexpr float GAP_SWIM_ABOVE = 5.0f;

    struct PathResult
    {
        bool reachable = false;
        float length = 0.0f;
        Movement::PointsArray points;  // what the leader walks now (PathGenerator path or long route corners)
        uint32 type = PATHFIND_BLANK;  // PathGenerator path type
        bool straightPath = false;     // the corner path was needed (smooth path over its point limit)
        bool steep = false;            // the steep slopes were needed (NAV_GROUND_STEEP)
        bool longRoute = false;        // only the long search (LongRoute) found it
        bool complete = false;         // the route reaches the target's polygon (not only "ends nearer")
        G3D::Vector3 end;              // where the route ends
        // [incomplete_route_deadend] the long search ran from the water surface above a lake floor (RoutePath),
        // and SteerLeg's leg does not reach the goal's polygon (PathGenerator INCOMPLETE or a partial long route).
        bool fromSurface = false;
        bool partial = false;
    };

    // Detour limits make a long walkable route look unreachable. Wave 7 boss_unreachable: Krystallus, Capacitus,
    // Dalliah, Soccothrates, Jandice and Anub'arak all had pathType 10 (SHORTCUT | NOPATH, 2 points). That is
    // BuildPointPath giving up: FindSmoothPath walks the corridor in 4 yd steps and fails at MAX_POINT_PATH_LENGTH
    // points ("most likely a loop"), although the polygon corridor exists (HoS: Krystallus 99 yd away, ~400 yd
    // round the hall). Longer still, findPath runs out of its 1024 search nodes (MMapMgr::CreateNavMeshQuery) or
    // its MAX_PATH_LENGTH corridor polygons and returns a partial path to the node nearest the target as the crow
    // flies - in a maze a dead end behind a wall (Razorfen Kraul: Roogug 45 yd away, ~760 yd to walk; Wailing
    // Caverns: Anacondra ~460 yd over 113-160 polygons). Such a route is searched again with an own query
    // (LongRoute): the same navmesh and filter flags, a larger node pool and corridor, corner points.
    constexpr float ROUTE_PROGRESS = 2.0f;

    constexpr uint32 POINT_PATH_FAILED = PATHFIND_SHORTCUT | PATHFIND_NOPATH;

    // Kill switches for the route pass 1 pieces (wave 9 freezes): false restores the wave 8 behaviour of that
    // piece - the corner path when the smooth path ran out of points, and the long search (LongRoute) with
    // SteerLeg walking its corners.
    constexpr bool ROUTE_CORNER_PATH = true;

    constexpr bool ROUTE_LONG_SEARCH = true;

    // - water: a route ending under water next to a target at the surface counts (a swimmer rises), the walker is
    //   not put on a lake floor, and the gap walk swims (Slave Pens wave 9/10: Rokmar "unreachable").
    constexpr bool ROUTE_SWIM_UP = true;

    // One path with one filter: the smooth path, the corner path when the smooth one ran out of points.
    uint32 BuildFilteredLeg(Player* owner, G3D::Vector3 const& start, G3D::Vector3 const& dest, bool fromOwner,
                            bool steep, Movement::PointsArray& points, G3D::Vector3& end, bool& straightPath);

    // One PathGenerator path: the bot filter first; when it does not get there, the same with the steep slopes a
    // player walks (Steamvault wave 6: Kalithresh's hall joins the rest only over NAV_GROUND_STEEP polygons, so
    // Steamrigger was "unreachable" from it: tools/navmesh_probe.py 545 -91.5 -440.4 3.6 -330.1 -121.5 -8 --both).
    uint32 BuildLeg(Player* owner, G3D::Vector3 const& start, G3D::Vector3 const& dest, bool fromOwner,
                    Movement::PointsArray& points, G3D::Vector3& end, bool& straightPath, bool* steepUsed = nullptr);

    // The liquid surface at a point that lies in or under water, INVALID_HEIGHT anywhere else.
    float WaterSurfaceAt(Player* walker, G3D::Vector3 const& point);

    float PolylineLength(Movement::PointsArray const& points);

    // [incomplete_route_deadend] (user, 2026-10-25: "if the path is incomplete, why walk it?") Kill switches, false
    // restores the behaviour before: the long search once more from the water surface above a walker on a lake
    // floor (RoutePath), and Steer's refusal of a partial leg whose end is no progress (JudgeIncompleteEnd).
    constexpr bool ROUTE_SURFACE_START = true;

    constexpr float SURFACE_START_MIN_DEPTH = 2.0f;

    // startAt: the route from that point instead of the owner's position (the far end of a RouteLinkDef).
    PathResult RoutePath(Player* from, float x, float y, float z, G3D::Vector3 const* startAt = nullptr);

    PathResult EvaluatePath(Player* from, WorldObject* to);

    // [hleft-passage-door-route] The walk to dest that keeps out of `doors` (their collision bounds widened by
    // PASSAGE_DOOR_MARGIN): the long search with DoorAvoidFilter, bot filter first, then the steep slopes. Only a
    // complete corridor counts (reachable false: there is no way round, the door is the way).
    constexpr float PASSAGE_DOOR_MARGIN = 1.0f;

    PathResult DoorFreeRoute(Player* from, G3D::Vector3 const& dest, std::vector<GameObject*> const& doors);

    // Long routes per walker, reused for LONG_ROUTE_REUSE_MS while the goal stays put (Steer runs every tick, a
    // long search can take milliseconds).
    struct CachedLongRoute
    {
        G3D::Vector3 goal;
        PathResult route;
        uint32 builtMs = 0;
    };

    constexpr uint32 LONG_ROUTE_REUSE_MS = 5 * IN_MILLISECONDS;

    constexpr bool ROUTE_PASSAGE_DOOR_DETOUR_LEG = true;  // [hleft-passage-door-route] SteerLeg's way round

    constexpr float LONG_ROUTE_SAME_GOAL = 3.0f;

    extern std::unordered_map<ObjectGuid, CachedLongRoute> longRouteCache;  // world thread only

    // The route's points from where the walker stands now: from the segment nearest to it on.
    Movement::PointsArray TrimRoute(Movement::PointsArray const& points, G3D::Vector3 const& here);

    // [hleft-passage-door-route] A leg through a closed door that opens on a boss's death (PassageDoorBoss) is
    // replaced by the way round it (DoorFreeRoute), when there is one. Per walker, reused PASSAGE_LEG_REUSE_MS
    // while the goal stays put (ClosedDoors walks the map's game objects).
    constexpr uint32 PASSAGE_LEG_REUSE_MS = 3 * IN_MILLISECONDS;

    struct PassageLegMemo
    {
        G3D::Vector3 goal;
        uint32 ms = 0;
        bool round = false;
        PathResult route;
    };

    extern std::unordered_map<ObjectGuid, PassageLegMemo> passageLegMemos;  // world thread only

    bool PassageDetourLeg(Player* owner, G3D::Vector3 const& here, G3D::Vector3 const& goal, PathResult& leg);

    uint32 SteerLeg(Player* owner, G3D::Vector3 const& goal, PathResult& leg);

    // The leader's next waypoint on a route: step yd along its first leg. A corner path has points far apart
    // (one per corner); the line between two corners is on the navmesh, so the waypoint is taken on it.
    G3D::Vector3 RouteWaypoint(PathResult const& route, G3D::Vector3 const& fallback, float step);

    // RouteWaypoint on a corner path interpolates the height between two corners; on stairs and slopes that
    // point floats or sinks and the walker's own MoveTo refuses it. The floor under it instead - but not a lake
    // floor under water (ROUTE_SWIM_UP): there the swimmer stays between the floor and the surface.
    constexpr float FLOOR_WAYPOINT_MAX_DZ = 10.0f;

    // No floor within FLOOR_WAYPOINT_MAX_DZ below it (a stair above, Sunken Temple normal 2026-10-25: the waypoint
    // at -370.3,53.7 lay at z -137.2 between two corners of the stair whose polygons are at z -129, the floor
    // search from above it found only the pit floor 13 yd below, the leader's own path refused it 3 minutes):
    // the navmesh polygon nearest to it (Detour findNearestPoly, this far up or down) gives the height.
    constexpr float FLOOR_SNAP_HALF_HEIGHT = 12.0f;

    constexpr float FLOOR_SNAP_HALF_WIDTH = 2.0f;

    bool NavmeshFloor(Player* walker, G3D::Vector3 const& point, float& floorZ);

    G3D::Vector3 FloorWaypoint(Player* walker, PathResult const& leg, G3D::Vector3 waypoint);

    // Approach rules (peel agent, wave 6): the leader stops outside the aggro radius of the pack's NEAREST
    // mob (Stratholme: walked to 14 yd of a refused 7-mob pack and pulled 14-17), and only on the target's
    // floor (Dire Maul West: stopped in the spiral stairwell 3.5 yd under the Highborne room, no LOS for 25 min).
    constexpr float PACK_RADIUS = 12.0f;

    struct PackMobCheck
    {
        Player* leader;
        Creature* target;
        bool operator()(Creature* creature) const
        {
            return creature->IsAlive() && !creature->IsInCombat() && !creature->IsCritter() &&
                   !creature->IsCivilian() && creature->GetExactDist(target) <= PACK_RADIUS &&
                   creature->IsHostileTo(leader);
        }
    };

    // [b32_brs] A pack the tank's planner refused as hopeless ("no safe pull": its smallest pull is over the
    // guarantee's ceiling; CarefulPlanVerdict: "no pull will come, route around it") is walked round when the
    // navmesh has a way round it: the leg avoids every polygon within each idle mob's aggro reach plus
    // HOPELESS_AVOID_MARGIN, for HOPELESS_AVOID_MS, a complete corridor only (else the leg stays as it was).
    // lbrs-n 20261025-221216-lbrs-n-5D-10 (2288-2513 s): the Firebrand Grunt pack (-29..-43,-411..-427; "brings
    // 13 mobs over the cap 9 at cost 14.45, over the guarantee's ceiling 9.00") stood on the shortest way from
    // Voone's floor to Smolderweb's ramp (navmesh_probe: via -71.5,-414 / -96,-423); every walk stopped at it, the
    // hall behind it went "behind skipped", the boss approach held ("refused by the chain count") until the
    // 4-minute stall, 3/9. The south way round (-2,-450 / 15,-480 / -23,-501 / -61,-475 / -67,-491 / -103,-497)
    // keeps 45+ yd from it; the packs on it are planned on their own.
    constexpr uint32 HOPELESS_AVOID_MS = 5 * MINUTE * IN_MILLISECONDS;

    constexpr float HOPELESS_AVOID_MARGIN = 4.0f;

    constexpr float HOPELESS_AVOID_DZ = 10.0f;

    constexpr float HOPELESS_DETOUR_MAX_RATIO = 4.0f;  // a way round at most this x the leg's own walk + 60 yd

    struct HopelessMob
    {
        ObjectGuid guid;
        uint32 sinceMs = 0;
    };

    extern std::unordered_map<ObjectGuid, std::vector<HopelessMob>> hopelessPacks;  // leader -> mobs, world thread

    struct HopelessLegMemo
    {
        G3D::Vector3 goal;
        uint32 ms = 0;
        size_t mobs = 0;
        bool round = false;
        PathResult route;
    };

    extern std::unordered_map<ObjectGuid, HopelessLegMemo> hopelessLegMemos;  // world thread only

    void NoteHopelessPack(Player* leader, Creature* mob);

    bool InDisc(G3D::Vector3 const& point, G3D::Vector4 const& disc);

    bool HopelessDetourLeg(Player* owner, G3D::Vector3 const& here, G3D::Vector3 const& goal, PathResult& leg);

    // Closed doors (2026-10-24, *.bak_approach2), false restores the old pick: idle trash whose route runs through
    // a closed door (GAMEOBJECT_TYPE_DOOR in GO_STATE_READY with its collision on) is no target until the door
    // is open - the navmesh is static and runs through every door, a player does not. Shadowfang Keep: the
    // courtyard (Moonwalkers, Servitors, Worgs at z 79.8, y 2150-2204) lies behind the Courtyard Door (GO 18895
    // at -242.6,2159.1,90.6), which instance_shadowfang_keep opens with TYPE_COURTYARD (the prisoner event after
    // Rethilgore); the route took them before it. No AreaGateDef for it: a gate opens on deaths (afterDead), this
    // door on an event. Bosses are not filtered (the boss order and its unreachable hold stay as they are).
    constexpr bool ROUTE_CLOSED_DOORS = true;

    constexpr float DOOR_RAY_HEIGHT = 1.0f;  // the route's segment tested at the height of a walking body

    constexpr float DOOR_SEGMENT_MARGIN = 2.0f;

    std::vector<GameObject*> ClosedDoors(Map* map, Player* leader);

    // [hleft-passage-door-route] A closed door the instance opens once a boss slot is DONE (its DoorData row is
    // DOOR_TYPE_PASSAGE) while that slot is not done yet: a boss's exit or his cell (Sethekk Halls 183398 behind
    // Talon King Ikiss, Blood Furnace 181819 Broggok's cell, which the lever event opens). The boss slot, -1: none.
    constexpr bool ROUTE_PASSAGE_DOOR_DETOUR = true;

    // [w24c-shut-door-detour] A door an encounter leaves shut for good once it is DONE: no way on, the walk goes
    // round it like round a passage door. UBRS (229): the stadium's wave portcullis 175185 (GO_GYTH_COMBAT_DOOR /
    // GO_DR_PORTCULLIS, boss_gyth / boss_rend_blackhand keep it as the wave door, no DoorData row) stays shut
    // after Rend 10 is DONE, the way on is the exit 175186 (DOOR_TYPE_PASSAGE on Rend). ubrs-n 20261025-115805
    // 1581-1791 s: The Beast "next boss behind a closed door (... 175185)", Drakkisath and all trash "route
    // through a closed door" while 175186 stood open - the navmesh path ignores doors and ran through 175185.
    struct ShutForGoodDoorDef
    {
        uint32 mapId;
        uint32 doorEntry;
        uint32 doneSlot;  // the boss slot whose DONE leaves the door shut for good
    };

    constexpr std::array<ShutForGoodDoorDef, 1> SHUT_FOR_GOOD_DOORS = {{
        {229, 175185, 10},  // UBRS stadium wave portcullis after Warchief Rend Blackhand (DATA 10)
    }};

    int32 ShutForGoodDoorSlot(InstanceScript const* script, GameObject* door);

    int32 PassageDoorBoss(InstanceScript const* script, GameObject* door);

    // The first closed door the route's polyline passes through (a ray along each segment, DOOR_RAY_HEIGHT above
    // it, against the door's own collision model), nullptr: none.
    GameObject* RouteThroughClosedDoor(Movement::PointsArray const& points, std::vector<GameObject*> const& doors,
                                       uint32 phaseMask);

    // Closed doors everywhere (2026-10-24, *.bak_doorgate), false restores ROUTE_CLOSED_DOORS alone (idle trash
    // only, the leader's waypoints unchecked). User, Utgarde Keep heroic: the group walked through the forge
    // room's walls of flame, a wall even a GM cannot pass (DoorEventDef). With it:
    // - every leader waypoint (SetLeaderWaypoint) is cut ~2 yd before the first closed door its path runs through
    //   ("[closed door] leader ...", door_hold), and Retarget drops a target that door does not open for;
    // - a route target behind a closed door is no target - bosses (the next boss: as unreachable, the order hold),
    //   fighting mobs and event starters too - unless the door opens for it (DoorOpensFor: the target is the door
    //   itself, a key used on it, or the key creature of a DoorEventDef);
    // - a DoorEventDef key creature whose afterData has not started is no target (it evades: Forge Master n).
    constexpr bool ROUTE_CLOSED_DOORS_ALL = true;

    constexpr float DOOR_HOLD_BACK = 2.0f;      // a leader waypoint a closed door cuts ends this far before it

    constexpr float DOOR_HOLD_MIN_WALK = 1.0f;  // a cut that walks less than this: the leader stays

    constexpr uint32 DOOR_HOLD_MEMO_MS = IN_MILLISECONDS;

    constexpr float DOOR_HOLD_MEMO_DISTANCE = 1.0f;

    DoorEventDef const* DoorEventOf(GameObject const* door);

    // The leader's waypoint cut before the first closed door its walk runs through (the navmesh is static and
    // runs through every door, and the leader's own walk falls back to a straight MoveStraight where its path is
    // refused). Per leader: the last answer, reused for DOOR_HOLD_MEMO_MS while the waypoint stays put.
    struct DoorStopMemo
    {
        G3D::Vector3 asked;
        G3D::Vector3 held;
        ObjectGuid door;
        float walked = 0.0f;
        uint32 ms = 0;
    };

    extern std::unordered_map<ObjectGuid, DoorStopMemo> doorStopMemos;  // world thread only

    // true: the waypoint ran through a closed door, `point` is now the hold before it (the leader's own spot when
    // the door is closer than DOOR_HOLD_BACK + DOOR_HOLD_MIN_WALK); door / walked say which and how far on.
    bool LeaderDoorStop(Player* leader, G3D::Vector3& point, ObjectGuid& door, float& walked);
}

#endif
