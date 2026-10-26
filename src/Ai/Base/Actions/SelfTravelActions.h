/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFTRAVELACTIONS_H
#define PLAYERBOTS_SELFTRAVELACTIONS_H

#include "MovementActions.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "Trigger.h"

#include <string>
#include <unordered_set>
#include <vector>

class Player;
class PlayerbotAI;
struct AreaTrigger;

// A bot's own way into an instance (strategy "instance portal") and its own breathing (strategy "breathe"): the
// bot walks, flies or takes the spirit healer like a player, nothing moves it from outside.
namespace SelfTravel
{
    // An instance the group goes into, as a leader says "we go in through that portal". exact: through the entrance
    // whose arrival point is nearest (x, y, z) - another wing - also when the bot already stands in that instance.
    struct PortalGoal
    {
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool exact = false;
    };

    void SetPortalGoal(ObjectGuid bot, PortalGoal const& goal);
    void ClearPortalGoal(ObjectGuid bot);
    bool GetPortalGoal(ObjectGuid bot, PortalGoal& goal);

    // What a bot's own movement could not do, for an observer (the test harness logs it as bot_cannot_move).
    struct MoveReport
    {
        std::string action;
        std::string reason;
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float toX = 0.0f;
        float toY = 0.0f;
        float toZ = 0.0f;
    };

    void ReportCannotMove(Player* bot, char const* action, char const* reason, float toX, float toY, float toZ);
    bool TakeReport(ObjectGuid bot, MoveReport& report);
    void ClearAll();

    // The instance map the bot is on its way into, 0 for none: its corpse's (a ghost), a goal not reached yet, or
    // the group leader's dungeon while the bot is alive outside it. O(1).
    uint32 PortalDestination(PlayerbotAI* botAI, PortalGoal* goal = nullptr);

    // A complete walk from the bot to dest on the navmesh, as corner points, searched past PathGenerator's limits
    // (its corridor stops at MAX_PATH_LENGTH polygons, its search at 1024 nodes). False: no complete route. reach > 0:
    // a partial route ending within reach (3D) of dest counts too (dest on a navmesh island of its own).
    bool LongRoute(Player* bot, G3D::Vector3 const& dest, Movement::PointsArray& corners, float reach = 0.0f);

    // A LongRoute walk kept by one action between ticks: searched again for another goal, when the bot left the
    // route, and at most every LONG_ROUTE_RECHECK_MS while none is found.
    struct RouteWalk
    {
        Movement::PointsArray corners;
        G3D::Vector3 goal;
        uint32 mapId = 0;
        uint32 searchedMs = 0;
        bool found = false;
    };

    // The next leg of a complete walk to goal (legStep yd along the route, on its floor) for a MoveTo. False: no
    // route (see LongRoute for reach).
    bool NextRouteLeg(Player* bot, RouteWalk& walk, G3D::Vector3 const& goal, float reach, float legStep,
                      G3D::Vector3& leg);

    // The height of the navmesh polygon nearest to point (Detour findNearestPoly, halfHeight up or down); for a
    // point whose height was interpolated between two corners of a stair or slope. Map thread only.
    bool NavmeshFloor(Player* bot, G3D::Vector3 const& point, float halfHeight, float& floorZ);

    // [b34_mech3] A bot standing on a navmesh island (a part of fewer than 48 polygons that nothing joins: a pipe, a
    // rail, a stair rim the straight walk climbed onto) gets the nearest point of a larger part within 8 yd across,
    // at most 13 yd below (no fall damage) or 1 yd above it, in sight: the step down a player takes back to the
    // floor. mech-n 20261026-014521-mech-n-5D-17: the tank stood 230 s on a 19-polygon pipe (268.3,-5.1,29.6), the
    // upper level's floor 4 yd below (271.4,-5.7,26.4). False: not on an island, or no such floor. Map thread.
    bool IslandStepDown(Player* bot, G3D::Vector3& landing);
}

class InstancePortalTrigger : public Trigger
{
public:
    InstancePortalTrigger(PlayerbotAI* botAI) : Trigger(botAI, "instance portal") {}

    bool IsActive() override;
};

// To the portal on foot (a corner path in legs, the last yards straight), into it as the client does
// (CMSG_AREATRIGGER); a ghost without a walk takes the spirit healer, a living bot without one flies on its own
// flying mount and descends beside the portal.
class MoveToInstancePortalAction : public MovementAction
{
public:
    MoveToInstancePortalAction(PlayerbotAI* botAI) : MovementAction(botAI, "move to instance portal") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    AreaTrigger const* FindPortal(uint32 destMapId, SelfTravel::PortalGoal const* goal) const;
    bool EnterPortal(AreaTrigger const* portal);
    // 1: walking, 0: no walk there, -1: waiting for the last move
    int32 WalkTowards(AreaTrigger const* portal, bool ghost);
    // A point inside the trigger that a complete walk reaches (the centre on a navmesh part of its own), cached.
    bool FindEntryPoint(AreaTrigger const* portal, G3D::Vector3& entry);
    // Stuck: a short straight step to a spot whose walk to the portal is better (off a navmesh island).
    bool HopOut(AreaTrigger const* portal, bool ghost);
    // The same for a portal under water or a bot in water without a navmesh route: a straight swim.
    int32 SwimTowards(AreaTrigger const* portal);
    bool FlyTo(AreaTrigger const* portal);
    uint32 FlyingMountSpell() const;
    bool CanFlyFromHere() const;
    int32 WalkLongRoute(AreaTrigger const* portal);
    // [b32_bfd] The portal's navmesh pocket is cut off from the bot's walk (Blackfathom Deeps: the Moonshrine Ruins
    // cave is a region of its own, 9 yd of lake surface from the Zoram Strand shore): the walk to the shore point of
    // the narrowest gap, then straight across it - over the water, or a short dive under a wall into the pocket.
    // 1: walking, 0: no such gap or the bot already is in the pocket, -1: waiting for the last move.
    int32 WalkViaBridge(AreaTrigger const* portal, bool ghost);
    bool FindBridge(AreaTrigger const* portal, bool ghost);

    uint32 _lastEnterMs = 0;
    Position _progressPos;
    uint32 _progressMapId = 0;
    uint32 _progressMs = 0;
    // The nearest the bot came to the portal and since when (WALK_NO_GAIN_MS).
    float _gainDistance = 0.0f;
    uint32 _gainMapId = 0;
    uint32 _gainPortal = 0;
    uint32 _gainMs = 0;
    uint32 _entryPortal = 0;
    uint32 _entryMapId = 0;
    uint32 _entryCheckedMs = 0;
    bool _entryFound = false;
    G3D::Vector3 _entryPoint;
    uint32 _flyPortal = 0;
    uint32 _flyCastMs = 0;
    float _flyCruiseZ = 0.0f;
    Position _flyApproach;
    uint8 _flyStage = 0;
    bool _flyOpenApproach = false;  // the approach point found is out in the open (FlyTo)
    // A ghost whose walk to the portal failed goes to the spirit healer and stays with that choice until it is alive.
    bool _spiritHealerChosen = false;
    // The long route (WalkLongRoute): searched for this portal on this map at _routeMs, found or not.
    uint32 _routePortal = 0;
    uint32 _routeMapId = 0;
    uint32 _routeMs = 0;
    bool _routeFound = false;
    float _routeBestLeft = 0.0f;
    Movement::PointsArray _routeCorners;
    // [b32_bfd] The gap into the portal's pocket (FindBridge): searched for this portal on this map at _bridgeMs.
    uint32 _bridgePortal = 0;
    uint32 _bridgeMapId = 0;
    uint32 _bridgeMs = 0;
    bool _bridgeFound = false;
    G3D::Vector3 _bridgeShore;                 // the end of the walk, on the bot's side
    std::vector<G3D::Vector3> _bridgeCrossing;  // the straight legs from there, the last one in the pocket
    size_t _bridgeLeg = 0;
    float _bridgeBestLeft = 0.0f;
    std::unordered_set<uint64> _bridgePocket;  // the pocket's polygons (dtPolyRef)
    SelfTravel::RouteWalk _bridgeWalk;
    // The portal this walk goes to (Execute): kept for this map, destination and goal until the walk gets stuck.
    uint32 _lockedPortal = 0;
    uint32 _lockedMapId = 0;
    uint32 _lockedDest = 0;
    bool _lockedGoalInside = false;
    float _lockedGoalX = 0.0f;
    float _lockedGoalY = 0.0f;
};

// Breath as Player::HandleDrowning counts it (the core's mirror timer is private): under water it runs out over
// CONFIG_WATER_BREATH_TIMER (times the water breathing multiplier), above it refills ten times as fast. Active from
// BREATH_SURFACE_PCT until BREATH_RESUME_PCT.
class LowBreathTrigger : public Trigger
{
public:
    LowBreathTrigger(PlayerbotAI* botAI) : Trigger(botAI, "low breath") {}

    bool IsActive() override;

private:
    int32 _breathMs = -1;
    uint32 _sampleMs = 0;
    bool _surfacing = false;
};

// Straight up to the surface; at the surface it only stops a dive back down until the breath is back.
class SurfaceForAirAction : public MovementAction
{
public:
    SurfaceForAirAction(PlayerbotAI* botAI) : MovementAction(botAI, "surface for air") {}

    bool Execute(Event event) override;
};

#endif
