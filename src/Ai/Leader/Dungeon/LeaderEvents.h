/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADEREVENTS_H
#define PLAYERBOTS_LEADER_LEADEREVENTS_H

// [lead107] user: production code does not live in the test harness. The dungeon leader's event tick (the events to
// every member: rest, ready, prepare, spots, follow, cleave / hazard exits, heal reach, attack / kill order, AoE,
// interrupts, externals, the route walk, the lingering combat), moved out of TestHarnessRunner.cpp unchanged; it
// reads the group through its view (LeaderGroup) and logs through the leader's sinks.

#include "LeaderGroup.h"

#include <string>

class Player;

namespace Leader::Dungeon
{
    // [lead22] a member at 99 % health and mana never rests and a full sitter holds nothing
    bool MemberFull(Player* bot);

    // [lead15] the leader's events to every member of `group`, `tank` its leader (world thread)
    void TickLeaderEvents(LeaderGroup& group, Player* tank);

    // [lead15] heal_reach: a member out of its healer's reach, logged per member every LEADER_HEAL_REACH_LOG_MS
    void LogHealReach(LeaderGroup& group, Player* member, Player* healer, std::string const& why,
                      std::string const& fix);
}

#endif
