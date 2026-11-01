/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_PULLFACTS_H
#define PLAYERBOTS_LEADER_PULLFACTS_H

// [lead104] user: production logic does not live in the test harness. The leader's pull and fight facts: what the
// leader judges before it issues an engage and in a fight; the harness only calls them and logs.

#include "ObjectGuid.h"
#include <vector>

class Creature;
class Player;
class Unit;

namespace Leader::Fight
{
    // [lead101] [lead103] the idle mob the tank's walk in to `target` wakes (the leader's route search against every
    // idle pack's reach), the plan's own mobs and the target's formation not judged; null: the walk is clear
    Unit* WalkInWakes(Player* tank, Unit* target, std::vector<ObjectGuid> const& planMobs);
    // [lead101] an order's own mob that stays the leader's next pick: alive and in no fight
    bool OrderMobStaysPick(Creature* own);
    // [lead104] a fight mob the leader has stopped from fleeing: at its SmartAI flee-for-assist health or running,
    // not held (stunned, rooted, crowd-controlled), not stopped within the group's stop spread
    bool NeedsFleeStop(Unit* mob);
    // [lead104] the member (not a healer) with a stop ready that takes `mob` now; null: none
    Player* FleeStopper(std::vector<Player*> const& members, Unit* mob);
}

#endif
