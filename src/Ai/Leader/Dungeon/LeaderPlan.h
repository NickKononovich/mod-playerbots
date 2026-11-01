/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERPLAN_H
#define PLAYERBOTS_LEADER_LEADERPLAN_H

// [lead107] user: production code does not live in the test harness. The dungeon leader's planner tick (its pick,
// its inputs to the planner, the commitment and the order it issues, the pull spots, the readiness verdict, the
// whole-pack / boss-pull requests, planner_no_option), moved out of TestHarnessRunner.cpp unchanged; it reads the
// group through its view (LeaderGroup) and logs through the leader's sinks.

#include "LeaderGroup.h"
#include "LeaderLog.h"

#include <string>
#include <vector>

class Creature;
class Player;
class Unit;

namespace PullPlanner
{
    struct Decision;  // ChooseTargetActions.h
}

namespace Leader::Dungeon
{
    constexpr uint32 DOOR_WALK_MS = 15 * IN_MILLISECONDS;  // the walk to the door to open, at most
    constexpr uint32 CAREFUL_REFUSED_SKIP_MS = 3 * MINUTE * IN_MILLISECONDS;

    // [lead7] the leader plans the tank's pull (PullPlanner::Decide on the world thread, every world update, when the
    // tank asked) and issues the decision; a hopeless refusal is the planner_no_option bug event.
    void TickLeaderPlanner(LeaderGroup& group);
    // [lead6] the leader's one readiness verdict ("" = ready): the group here and topped up, nobody buffing,
    // sitting or preparing (PullReadiness::GroupPreparing)
    std::string LeaderReadyWhy(LeaderGroup& group, Player* leader);
    // [lead3] the pack the leader must clear: asked of its planner whole (walk-in priced by the planner); the
    // leader holds where it stands meanwhile. `required`: no option for it is planner_no_option with a stop.
    void LeaderWholePack(LeaderGroup& group, Player* leader, Creature* pack, std::string const& why, bool walkIn,
                         bool required);
    // [lead3] a boss pull: a BossPull order at the tank's next ask (PullPlanner::BossPullDecision)
    void LeaderBossPull(LeaderGroup& group, Player* leader, Creature* boss, std::string const& why);
    // [lead3] user decision 1: planner_no_option, once per pack; `stops` ends the run (LEADER_STOP_ON_NO_OPTION)
    // true: reported now (a pack reported before is not again)
    bool LeaderNoOption(LeaderGroup& group, Player* leader, Unit* mob, std::vector<ObjectGuid> const& pack,
                        std::string const& why, bool stops, JsonObject fields);
    // [lead15] the spots of a pull (false: no gate for it) and what the pull still waits for ("" = everyone there)
    bool LeaderAssignSpots(LeaderGroup& group, Player* tank, PullPlanner::Decision const& decision);
    std::string LeaderSpotsWait(LeaderGroup& group, Player* tank);
    // [lead24] a former forcing timer's diagnostic: `kind` waited `ms` (once per kind per 30 s)
    void WaitedLong(LeaderGroup& group, std::string const& kind, uint32 ms, std::string const& why);
}

#endif
