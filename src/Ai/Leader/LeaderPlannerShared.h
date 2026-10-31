/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LEADERPLANNERSHARED_H
#define _PLAYERBOT_LEADERPLANNERSHARED_H

// [lead31a] The pull planner's pieces the tank's execution (Ai/Base/Actions/ChooseTargetActions.cpp) uses. The
// planner itself is the leader's (Ai/Leader/LeaderPlanner.cpp; its internals in namespace PullPlanning).

#include "LeaderOrders.h"
#include "Position.h"

#include <string>

class Player;
class Unit;

namespace PullPlanning
{
    // the values the execution shares (the planner's own definitions, LeaderPlanner.cpp)
    constexpr float CAREFUL_AGGRO_MARGIN = 3.0f;
    constexpr float APPROACH_DISTANCE = 30.0f;
    constexpr float SIDE_PULL_ARRIVED = 2.0f;

    // set by Execute: the decision it runs was issued by the leader (the commit reports it as the leader's)
    extern thread_local bool executingLeaderDecision;

    void NoteLosCorner(Player* tank, Position const& spot, Position const& gather);
    bool NoteLosPullStart(Player* bot, Unit* target);
}

// the tank's commit of a pull, posted to the leader's order registry
bool PostCommit(Player* bot, Unit* target, bool ok, Leader::PullMethod method, std::string const& why, uint32 mobs,
                float cost, bool single);

#endif
