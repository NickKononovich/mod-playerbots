/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SoloCombatEntry.h"

namespace Bot::Combat
{
    bool TakesTheCombatEngine(EntryFacts const& facts)
    {
        // A GROUP MEANS A LEADER, and the leader does every check before it issues (rule 4). This rule exists only
        // for a bot that has nobody deciding for it, which is also why it can carry no risk of pulling something
        // the leader did not plan.
        if (facts.inGroup)
            return false;
        // Nothing to repair: the core is not holding it in combat, so starting a fight is the grind picker's
        // business and is priced there.
        if (!facts.coreInCombat)
            return false;
        // THE TARGET IS AN ATTACKER OR THERE IS NO RULE HERE. With nothing on the bot this must not pick a mob of
        // its own choosing - that is the difference between repairing a contradiction and inventing a pull.
        if (!facts.hasAttackers)
            return false;
        return facts.alive && facts.engineOnNonCombatSet;
    }

    bool ReleasesTheTarget(ReleaseFacts const& facts)
    {
        // whose fight it is: an instance fight is the leader's and the encounter's, and a led member holds what its
        // leader named (rule 4)
        if (facts.inInstance || facts.led)
            return false;
        if (!facts.engineIsCombat || !facts.alive)
            return false;
        // nothing held, or a dead target - the dead one is DropTargetAction's, which has its loot to register
        if (!facts.targetPresent || !facts.targetAlive)
            return false;
        // THE DECISIVE FACT, and the only one about the target that matters: it is on the bot, so it is the bot's
        // fight whatever else is true of it. What the OLD guard read instead - whether the bot or the mob was in
        // combat with ANYBODY - says nothing about that, which is why neither is an input to this function.
        if (facts.targetAttacksMe)
            return false;
        // ... and it IS being acted on: the engine ran something, or the reach actions are walking it in, or a cast
        // is in flight on it. Each of these is the engine's own answer, passed in, never re-derived here.
        if (facts.actedThisTick || facts.movingToIt || facts.castingOnIt)
            return false;
        // in melee range it is reachable by definition. A rotation that produces nothing standing next to a mob is
        // a different defect, and letting the target go would hide it instead of reporting it.
        return !facts.targetWithinMeleeRange;
    }
}  // namespace Bot::Combat
