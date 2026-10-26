/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TBCHAZARD_H
#define PLAYERBOTS_TBCHAZARD_H

#include "Common.h"
#include "MovementActions.h"
#include "Multiplier.h"
#include "Position.h"
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

class Action;
class Player;
class PlayerbotAI;
class Unit;

// Small helpers shared by the TBC 5-man encounter strategies (Slave Pens, Blood Furnace, Mana-Tombs,
// Shadow Labyrinth, ...). Every hazard here is a circle around a point (a boss pulsing a nova, a totem, a bomb).
namespace TbcHazard
{
// Extra yards kept beyond a spell's own radius when leaving it, and the band in which a bot that has just left
// a hazard is kept from walking straight back in by the hold multipliers.
inline constexpr float EXIT_MARGIN = 3.0f;
inline constexpr float HOLD_BUFFER = 2.0f;

// Finds a reachable spot on the circle of `radius` around `center`, straight away from the center through the
// bot first, then rotating left/right until a collision-free line is found. False when the bot is already out
// or no direction is free.
bool FindExitPoint(Player* bot, Position const& center, float radius, float& x, float& y, float& z);

// Same as FindExitPoint but for a bot that should stand at exactly `radius` from `center` (closer or farther).
bool FindRingPoint(Player* bot, Position const& center, float radius, float& x, float& y, float& z);

// True when any alive group member (the bot included) stands within `radius` of `center`.
bool AnyGroupMemberWithin(Player* bot, Position const& center, float radius);

// Actions that reposition the bot (plain movement, reach-target casts, blink/disengage). The hold multipliers
// drop these near a hazard; TbcHazardMoveAction and its subclasses are exempt.
bool IsRepositionAction(Action* action);

// Danger test shared by the hazard triggers: the bot stands within `dangerRadius` (+ a yard of safety).
bool IsInside(Player* bot, Position const& center, float dangerRadius);

// Hold test shared by the hazard multipliers: the bot is inside or just outside the hazard, and what its
// movement would chase (the current target) is inside too, so any reposition would carry it back in.
bool ShouldHoldOutside(PlayerbotAI* botAI, Position const& center, float dangerRadius);

// The first alive in-combat attacker with one of the given entries (cached "attackers" list, no grid search).
Unit* FindAttackerByEntry(PlayerbotAI* botAI, std::initializer_list<uint32> entries);

// Whether the bot's group is engaged with the named boss ("find target" reads the bot's threat links only).
Unit* FindBoss(PlayerbotAI* botAI, std::string const& lowerCaseName);
}

// A damage dealer whose current target is one of `entries` (a totem, a beacon, ...) keeps it: the generic
// target pickers ("dps assist", "dps aoe") would otherwise switch straight back to the boss the next tick.
class TbcKeepFocusMultiplier : public Multiplier
{
public:
    TbcKeepFocusMultiplier(PlayerbotAI* botAI, std::string const name, std::vector<uint32> entries)
        : Multiplier(botAI, name), _entries(std::move(entries))
    {
    }
    float GetValue(Action* action) override;

private:
    std::vector<uint32> _entries;
};

// Base for every hazard-escape action of the TBC dungeon strategies, so the hold multipliers can tell them apart
// from ordinary movement.
class TbcHazardMoveAction : public MovementAction
{
public:
    TbcHazardMoveAction(PlayerbotAI* botAI, std::string const name) : MovementAction(botAI, name) {}

protected:
    // Leaves the circle (center, radius + EXIT_MARGIN). False when already out or no free direction.
    bool LeaveCircle(Position const& center, float radius, bool stopCasting);
    // Walks to the ring of `radius` around `center` (used to hold a band, e.g. max range from a boss).
    bool MoveToRing(Position const& center, float radius, MovementPriority priority);
};

#endif
