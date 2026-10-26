/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTRUNNERCONTEXT_H
#define PLAYERBOTS_TESTRUNNERCONTEXT_H

#include "NamedObjectContext.h"
#include "TestRunnerStrategy.h"

class TestRunnerStrategyContext : public NamedObjectContext<Strategy>
{
public:
    TestRunnerStrategyContext() : NamedObjectContext<Strategy>(false, true)
    {
        creators["test runner"] = &TestRunnerStrategyContext::test_runner;
    }

private:
    static Strategy* test_runner(PlayerbotAI* botAI) { return new TestRunnerStrategy(botAI); }
};

class TestRunnerTriggerContext : public NamedObjectContext<Trigger>
{
public:
    TestRunnerTriggerContext()
    {
        creators["test runner waypoint"] = &TestRunnerTriggerContext::test_runner_waypoint;
    }

private:
    static Trigger* test_runner_waypoint(PlayerbotAI* botAI) { return new TestRunnerWaypointTrigger(botAI); }
};

class TestRunnerActionContext : public NamedObjectContext<Action>
{
public:
    TestRunnerActionContext()
    {
        creators["test runner move"] = &TestRunnerActionContext::test_runner_move;
    }

private:
    static Action* test_runner_move(PlayerbotAI* botAI) { return new TestRunnerMoveAction(botAI); }
};

#endif
