/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERREPORTS_H
#define PLAYERBOTS_LEADER_LEADERREPORTS_H

// [lead107] user: production code does not live in the test harness. The tank's order reports, the members'
// acks and own decisions drained into the leader's decision log, and the tank's pull state they name; moved out of
// TestHarnessRunner.cpp unchanged.

#include "LeaderGroup.h"
#include "ObjectGuid.h"
#include <string>

class Player;

namespace Leader::Dungeon
{
    constexpr uint32 PULL_ABORT_HOLD_MS = 10 * IN_MILLISECONDS;  // CarefulPlanVerdict::RecentlyAborted window

    // What holds the tank's pull, for the pull_hold / pull_hold_stall lines.
    std::string TankPullState(Player* tank);

    // [lead1] [lead15] the tank's order reports and the members' acks / own decisions into the decision log, the
    // order's fight / done / orphan states and its end facts (the leader's cleared spots)
    void DrainOrderReports(LeaderGroup& group, Player* leader);
}

#endif
