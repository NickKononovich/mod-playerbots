/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_INTERRUPTVALUES_H
#define PLAYERBOTS_INTERRUPTVALUES_H

#include "ObjectGuid.h"
#include "Value.h"

#include <string>

class Player;
class PlayerbotAI;
class SpellInfo;
class Unit;

// The enemy cast this bot is the group's interrupter for, and the spell to use on it.
struct InterruptAssignment
{
    ObjectGuid caster;
    std::string spell;
};

// One interrupter per enemy cast, shared by the group: the closest member whose interrupt is ready and in range
// takes the cast, the next cast goes to the next free member, and a cast the interrupter failed to stop is handed
// on. Claims live in a per-instance registry so every bot of the party reads the same answer.
namespace InterruptCoordination
{
    // Runs the assignment pass for the casts this bot can see; returns the cast assigned to this bot.
    InterruptAssignment Update(PlayerbotAI* botAI);

    // False when the current cast of `caster` belongs to another member (or to another of this bot's
    // spells): the per-class interrupt triggers stay quiet so two bots never spend a kick on one cast.
    bool Allows(PlayerbotAI* botAI, Unit* caster, std::string const& spell);

    // The interrupter has spent its spell; a cast still running shortly after this is handed on.
    void MarkFired(PlayerbotAI* botAI, Unit* caster);

    // A non-healer that is assigned a cast stops its own cast to interrupt, as a player stop-casts to kick.
    bool ShouldStopCastToInterrupt(PlayerbotAI* botAI);

    // The leader's "interrupt" order: the one group member of `bot`'s map with the best ready interrupt for the
    // cast `caster` is making now (same scoring as the automatic pass). False when no cast or nobody can.
    // Every bot hearing the order computes the same answer, so one member kicks.
    bool PickOrderedInterrupter(Player* bot, Unit* caster, ObjectGuid& interrupter, std::string& spell);

    // True when the kick, silence or stun `info` would stop a cast of `caster`. IsImmunedToSpell alone is not
    // enough: a boss immune to MECHANIC_INTERRUPT (creature_immunities) shrugs off Wind Shear's interrupt effect
    // while its threat effect still lands, so the spell as a whole does not count as immune. A spell without such
    // an effect of its own (Avenger's Shield silences through a talent proc) is judged by IsImmunedToSpell alone.
    bool CanStopCast(Unit* caster, SpellInfo const* info);
}

class InterruptAssignmentValue : public CalculatedValue<InterruptAssignment>
{
public:
    InterruptAssignmentValue(PlayerbotAI* botAI)
        : CalculatedValue<InterruptAssignment>(botAI, "interrupt assignment", 200)
    {
    }

    InterruptAssignment Calculate() override;
};

#endif
