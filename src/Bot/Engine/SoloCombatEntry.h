/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOT_ENGINE_SOLOCOMBATENTRY_H
#define PLAYERBOTS_BOT_ENGINE_SOLOCOMBATENTRY_H

// [solofight1] TWO DECISIONS OF THE ENGINE STATE MACHINE, AS A SEAM: the facts of the tick go in, the answer comes
// out, and nothing here reads the world, the clock or the database. PlayerbotAI gathers the facts from the map where
// it already holds them (DoNextAction, ReleaseIdleTargetNobodyReaches) and calls these instead of deciding inline.
//
// WHAT WAS BROKEN, measured on b502.
//
// 1. A BOT THE CORE HELD IN COMBAT STAYED ON ITS NON-COMBAT SET OF STRATEGIES UNTIL IT BLED OUT. Thelengyl L27, a
//    tank alone in Ashenvale, was in a fight for 113.9 s, was active for 9.6 s of it, took 985 damage out of a pool
//    of 1 034, and its foe ended on 100 % health. The alert that caught it says `engine non-combat` while the core
//    held the bot in combat, and the engine's own trail says why: `A:attack anything - USELESS|A:new rpg do quest -
//    OK`. Nothing "returned not in combat" - the engine state was never a function of the combat state at all.
//    `currentState` is moved to BOT_STATE_COMBAT by an ACTION (AttackAction and the other target pickers) plus two
//    fact-driven exceptions in DoNextAction: a healer whose group is under attack, and a tank IN A GROUP with a mob
//    it can take back. For a bot alone that is not a healer there was no fact-driven path at all, and the one target
//    picker of the non-combat set that needs neither a group nor a tank - "attack anything" - disqualifies itself
//    with `if (bot->IsInCombat()) return false`. In the same hour: 91 `[solo] NO_ATTACK` fights (one row per fight),
//    28 of them with the engine on its non-combat set and no target, and 4 `STUCK_COMBAT` alerts.
//
//    This is the SAME mechanism already approved for a healer alone ([death383]) widened to every role, with the
//    same target: THE NEAREST ATTACKER. Not a grind target, so no pack the leader did not plan is pulled, and it
//    runs only where there is no group at all, so the leader is not touched (rule 4).
//
// 2. A TARGET WAS ONLY EVER LET GO WHEN IT DIED. DropTargetAction tests `target->isDead()`, and the one fact-driven
//    release (ReleaseIdleTargetNobodyReaches) refused to fire whenever the bot or the mob was in combat with anybody
//    at all. So a mob that no longer attacks this bot, that the bot cannot reach and has no action for, was held.
//    Measured: 178 `stuck_target` rows, 116 of them healers (Selalleas 52 rows over 36 mobs, Ellosera 32,
//    Rollothan 20), targets 5.1-102.1 yd away, trail `moonfire - IMPOSSIBLE | wrath ... | reach spell - USELESS`.
//    The release is now by the FACT the user named - "this mob no longer attacks me and I have nothing that passes
//    on it" - and NOT by a clock: there is no timer anywhere in this file (rule 2).
//
// WHY THE TWO CANNOT CHASE EACH OTHER, which is the first thing to check when two rules touch one state: the entry
// takes an ATTACKER as its target, and the release only lets go of a target that does NOT attack the bot. No set of
// facts satisfies both for the same unit. When something else is attacking the bot, the release hands the stale
// target back and the entry re-takes the engine on the attacker in the same tick - which is the intended sequence,
// not a loop.

namespace Bot::Combat
{
    // The facts of one tick of PlayerbotAI::DoNextAction, for the question "does this bot belong on its combat set
    // of strategies right now".
    struct EntryFacts
    {
        bool alive = false;
        bool inGroup = false;  // ANY group: with one, the leader decides and this rule stays out of its way (rule 4)
        bool coreInCombat = false;    // the core holds the bot in combat - the fact the engine used to contradict
        bool hasAttackers = false;    // somebody is actually attacking it, so there is a target to name
        // the bot is running its NON-COMBAT set right now. Stated this way round, and not as "already in combat",
        // because the dead set is a third state: a bot alive but still on the dead engine (resurrected and rooted)
        // must not be pushed into the combat set from here - DoNextAction's own dead-engine branch owns that step.
        bool engineOnNonCombatSet = false;
    };

    bool TakesTheCombatEngine(EntryFacts const& facts);

    // The facts for "is this target still this bot's business". `actedThisTick` is the engine's own answer, which is
    // why the caller passes it: the engine knows whether anything ran, and this must never guess it.
    struct ReleaseFacts
    {
        bool engineIsCombat = false;
        bool alive = false;
        bool actedThisTick = false;  // the engine ran an action this tick: it is working, nothing is stuck
        bool inInstance = false;     // an instance fight belongs to the leader and to the encounter, never here
        bool led = false;            // a led member holds what its leader named (rule 4)
        bool movingToIt = false;     // walking: the reach actions are doing their job
        bool castingOnIt = false;    // a cast in flight, or the last cast was on this target: it IS being acted on
        bool targetPresent = false;
        bool targetAlive = false;
        bool targetAttacksMe = false;         // the decisive fact: it is on the bot, so it is the bot's fight
        bool targetWithinMeleeRange = false;  // reachable by definition - the rotation's problem, not this one's
    };

    bool ReleasesTheTarget(ReleaseFacts const& facts);
}

#endif
