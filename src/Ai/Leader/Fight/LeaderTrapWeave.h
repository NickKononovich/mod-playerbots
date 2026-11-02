/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERTRAPWEAVE_H
#define PLAYERBOTS_LEADER_LEADERTRAPWEAVE_H

// [lead271] The hunter's Explosive Trap weave (wowsims hunter__bm / mm / sv: Explosive Trap whenever its DoT is off,
// ~2.6 a minute on one target; TrapWeaveSpell: the walk in, the trap, the walk out, the shots lost meanwhile). In
// 3.3.5 a trap lies at the hunter's feet, so the leader decides it as a computed choice per hunter and fight target:
// the trap's damage on the fight mobs it covers over the time they still live (the trap's own spell data, the
// hunter's damage bonuses) against the hunter's measured damage per second over the walk there and back and the
// trap's global cooldown. It pays: the hunter walks to the trap spot (the trap's trigger radius off the target, on
// its own side, the farthest the trap still goes off from: out of the target's melee where the radius allows),
// drops the trap (the leader's External), and its combat spot takes it back.

#include "Common.h"
#include <functional>
#include <string>
#include <vector>

#include "LeaderGroup.h"
#include "LeaderLog.h"

class Player;
class Unit;

namespace Leader::TrapWeave
{
// In the leader's fight moves for one hunter (the member loop, before its combat spot). `move` issues the walk
// to the spot (the leader's checked move; false: refused). True: the trap weave owns the hunter's move this tick.
bool TickMove(LeaderGroup& group, Player* hunter, Player* tank, Unit* target, std::vector<Player*> const& seats,
              std::function<bool(Position const&, std::string const&)> const& move);

// After the fight's externals: the drop order of a hunter on its trap spot (until the trap's cooldown runs).
void TickDrop(LeaderGroup& group, std::vector<Player*> const& seats, bool fighting,
              std::function<void(Player*, char const*, JsonObject)> const& logEvent);
}  // namespace Leader::TrapWeave

#endif
