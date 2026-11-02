/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERPREPULL_H
#define PLAYERBOTS_LEADER_LEADERPREPULL_H

// [lead270] The prepull Army of the Dead (wowsims tank_deathknight__blood_aggro -6 s, deathknight__blood_dps -8.5 s,
// deathknight__frost_bl_pesti -8 s; unholy_dw_ss has none). The leader times it like a prepull consumable: while the
// group stands in the boss's preparation (the buff pass at the boss's buff distance; the pull walk follows it) it
// computes when that preparation ends and tells the death knight to channel once the preparation left is no longer
// than the channel, so the channel ends with it. The pass's end waits only for the channel's remainder (a pass too
// short to time it into starts it at its end). All numbers are the game's: the channel's duration, the rune cost,
// the members' regen, the food's Well Fed timer.

#include "Common.h"
#include <functional>
#include <string>
#include <vector>

#include "LeaderGroup.h"
#include "LeaderLog.h"

class Player;
class Unit;

namespace Leader::Prepull
{
// Every leader tick (TickLeaderEvents, between BeginTick and EndTick): tells a ready death knight to channel when
// the computed preparation left fits the channel, asserts the order until the channel runs, and logs its course.
void TickArmy(LeaderGroup& group, Player* tank, std::vector<Player*> const& seats, bool fighting,
              std::function<void(Player*, char const*, JsonObject)> const& logEvent);

// The boss preparation is over but for the Army: `orderNow` true at the buff pass's end (an Army not told yet is told
// now), false at the planner's pull gates (they only wait for a channel under way or told). Empty: nothing holds.
std::string ArmyHolds(LeaderGroup& group, Unit* boss, bool orderNow);
}  // namespace Leader::Prepull

#endif
