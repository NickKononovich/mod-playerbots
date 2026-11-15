/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_WALKGRAPH_H
#define PLAYERBOTS_LEADER_WALKGRAPH_H

// [nav180] the continents' crossing graph: every map grid (a 533 yd mmtile) is a cluster, its nodes are the places
// the navmesh leaves it for a neighbour grid (a run of border polygons linked across, or an off-mesh link's end),
// its edges the walks between them over the grid's own polygons (a Dijkstra on the navmesh, Detour's portal-midpoint
// metric) and the crossings themselves (Detour's links, so a one-way off-mesh link stays one-way). Built once per
// start on a background thread from the mmtile files (its own navmesh, three grid rows loaded at a time), never from
// the live one: the live navmesh holds only the grids loaded so far. A long walk plans on it in microseconds; the
// fine route search (Leader::Route) only walks the bot to the plan's next node.
// The stock TravelNodeMap (playerbots_travelnode*) is not used: it is never loaded in this build, its nodes are
// NPCs / triggers / zone means (no guarantee that a navmesh walk has a node chain), its links were computed with
// PathGenerator on older mmaps (not the 5x5 sub-tile navmesh), and its objects are not safe on parallel map threads.

#include "Define.h"
#include "G3D/Vector3.h"
#include "ObjectGuid.h"
#include <vector>

class Map;

namespace Leader::Travel::WalkGraph
{
    // the world thread at startup: the background build of every continent's graph; StopBuild at shutdown
    void StartBuild();
    void StopBuild();

    struct Plan
    {
        bool ready = false;       // the map has a built graph
        bool sameRegion = false;  // from and to lie in one grid's connected polygons: no graph walk
        bool found = false;
        std::vector<uint32> nodes;           // the plan's nodes, the first one in the walker's grid
        std::vector<G3D::Vector3> points;    // their positions
        std::vector<float> walked;           // the graph yd from `from` to each node
        float yd = 0.0f;                     // the plan's whole length, the last node's straight line to `to` added
        uint64 us = 0;
        // [nav230] what this answer was, for the caller's log: every return of PlanWalk names itself, and every one
        // of them fills `us`. A "no plan" used to come back with us = 0 (the timer was taken on the A* path alone),
        // which read as an instant search and said nothing about which of the four lookups before the A* failed
        char const* why = "";
    };
    // the graph walk from `from` to `to` on `map` (a continent), the nodes in `banned` left out
    Plan PlanWalk(Map* map, G3D::Vector3 const& from, G3D::Vector3 const& to, std::vector<uint32> const& banned);
    // the live navmesh has the tile at `at` (the fine route search reaches only loaded grids)
    bool LiveTileLoaded(Map* map, G3D::Vector3 const& at);

    // [maze21] THE GRAPH'S OWN NODES AROUND A POINT - the one place that answers "which places near here could a
    // walk go through". The maze search (NewRpgBaseAction::WalkAround) asked the stock TravelNodeMap for this, and
    // that store is never loaded in this build (TravelMgr::LoadQuestTravelTable has no caller in the module or the
    // core; only the `load node` debug command reaches loadNodeStore), so the whole way-round cycle was dead code:
    // 76 of 76 recorded give-ups of b504 carry "0 vias in reach, 0 searched" and not one child row, while 75 of
    // those 76 stands have 1 to 8 walkable places within 400 yd. The user's word 2026-11-15: switch it over.
    // `out` gets the nodes within `reach` of `at`, unordered (the caller ranks them: Leader::Travel::Maze::Of).
    // THE RETURN IS THE SOURCE'S OWN ANSWER AND NOT A COUNT: false = this map has no built graph, i.e. nobody
    // looked, which is NOT "there are none" (rule 7). An empty `out` with true is a measurement.
    struct NodeAt
    {
        uint32 node = 0;
        G3D::Vector3 pos;
    };
    bool NodesInReach(Map* map, G3D::Vector3 const& at, float reach, std::vector<NodeAt>& out);

    // [nav183] the far walk's search time per kind, for the [perf] line (fine vs graph, per bot and hour)
    enum class SearchKind : uint8
    {
        Full,   // the whole walk to the destination (the route search before the graph)
        Leg,    // a far route's next leg, two sights ahead ([guild15])
        Stage,  // the fine search to the graph plan's next node
        Graph,  // the graph plan itself
        Count
    };
    void NoteSearch(SearchKind kind, uint64 us, ObjectGuid walker);
}

#endif
