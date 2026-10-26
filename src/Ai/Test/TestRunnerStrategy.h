/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTRUNNERSTRATEGY_H
#define PLAYERBOTS_TESTRUNNERSTRATEGY_H

#include "MovementActions.h"
#include "ObjectGuid.h"
#include "Strategy.h"
#include "Trigger.h"

// Headless test harness: the leader of a test group walks the instance on its own. The route (next target,
// next waypoint on the path) is computed by the harness on the world thread; the bot only reads the waypoint.
namespace TestRunner
{
    struct Waypoint
    {
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32 serial = 0;  // bumped by the harness on every new waypoint
        // A jump down (RouteLinkDef::jump, Shattered Halls' sewer): the bot stands at the link's start and jumps to the
        // waypoint as a player steps off the ledge (MovementAction::JumpTo, a controlled move the airborne guard
        // lets through); every other waypoint is walked.
        bool jump = false;
        // The far end of a walked route link (RouteLinkDef, the harness checked ground under every 2 yd of the line):
        // the bot walks the line straight first (MovementAction::MoveStraight). Its own path goes round on the
        // navmesh: the Underbog root (wave 21, ub-n / ub-h) took both groups along the floor under the ramp to the
        // polygon below the far end, 23 yd under it, and they stood there 8 minutes ("no straight walk over ground").
        bool straight = false;
        // [b32_strat_gate] A walk through an auto-closing key gate the moment it opens (the harness's TickKeyDoorPass):
        // the idle-mob path guard does not shorten it (TestRunnerMoveAction::SkipsIdleGuard) - it cut the healer's
        // pass to 1.9 yd before Stratholme's Service Entrance Gate and the gate shut on it 3 s later.
        bool gatePass = false;
    };

    void SetWaypoint(ObjectGuid bot, Waypoint const& waypoint);
    void ClearWaypoint(ObjectGuid bot);
    bool GetWaypoint(ObjectGuid bot, Waypoint& waypoint);
    void ClearAll();
    // [b32_strat_gate] A member the harness found cut off behind a closed door (TickDoorCut): the tank's pull gate
    // holds every pull while one of its group is (ChooseTargetActions, "behind a closed door").
    void SetDoorCut(ObjectGuid bot, bool cut);
    bool IsDoorCut(ObjectGuid bot);
}

class TestRunnerStrategy : public Strategy
{
public:
    TestRunnerStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "test runner"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

class TestRunnerWaypointTrigger : public Trigger
{
public:
    TestRunnerWaypointTrigger(PlayerbotAI* botAI) : Trigger(botAI, "test runner waypoint") {}

    bool IsActive() override;
};

// The bot walks to the waypoint with its own MoveTo; where the navmesh does not carry it (a ramp, a bridge or a gap
// without polygons: MoveTo refuses, or the walk stops short), it walks straight on over ground like a player
// (MovementAction::MoveStraight). What it cannot do is reported (SelfTravel::ReportCannotMove), never covered.
class TestRunnerMoveAction : public MovementAction
{
public:
    TestRunnerMoveAction(PlayerbotAI* botAI) : MovementAction(botAI, "test runner move") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    // [b32_strat_gate] a gate pass waypoint (Waypoint::gatePass) walks through as given
    bool SkipsIdleGuard() override;

private:
    // Progress towards the waypoint; a new waypoint (moved by more than WAYPOINT_SAME_RANGE) starts anew.
    float _waypointX = 0.0f;
    float _waypointY = 0.0f;
    float _waypointZ = 0.0f;
    uint32 _progressMs = 0;
    float _bestDistance = 0.0f;
    uint32 _jumpSerial = 0;  // the jump waypoint already jumped to (once per waypoint)
    uint32 _straightLogMs = 0;  // last "[test runner] link line" log line
};

#endif
