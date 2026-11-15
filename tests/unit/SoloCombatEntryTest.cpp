/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [solofight1] THE TWO CLAIMS, over real b502 rows, and both of them refutable:
//
//   * a bot alone that the core holds in combat and that has attackers does NOT stay on its non-combat set;
//   * a target that no longer attacks the bot and for which no action of it passes IS let go.
//
// The fixtures are the two cases the user named. Thelengyl L27 (pb_journal_alert STUCK_COMBAT #10): a tank alone in
// Ashenvale, 60 s in combat with one Horde Scout L26 at 23 yd "on it", hp 23 %, mana 0 %, `engine non-combat`, its
// trail `A:attack anything - USELESS|A:new rpg do quest - OK`; the fight lasted 113.9 s, the bot was active 9.6 s of
// it, and the foe ended on 100 % health. Selalleas (pb_journal_stuck, kind `stuck_target`, role healer, 52 rows over
// 36 mobs): targets 23.8-101.3 yd away, `in_combat = 0`, trail `cancel tree form - USELESS|moonfire - IMPOSSIBLE|
// wrath ...` - not one action of the engine passed on the mob it was holding.
//
// NO CLOCK APPEARS IN ANY CASE BELOW, and that is asserted by construction: neither struct has a duration, an age or
// a timestamp in it, so no assertion here can be satisfied by waiting (rule 2).

#include <gtest/gtest.h>

#include "SoloCombatEntry.h"

namespace
{
    using Bot::Combat::EntryFacts;
    using Bot::Combat::ReleaseFacts;

    // Thelengyl at the instant of its STUCK_COMBAT alert.
    EntryFacts ThelengylAloneUnderFire()
    {
        EntryFacts facts;
        facts.alive = true;
        facts.inGroup = false;       // alone in the open world
        facts.coreInCombat = true;   // "60 s in combat" - the core's own answer
        facts.hasAttackers = true;   // "Horde Scout L26 100 % hp 23 yd on it"
        facts.engineOnNonCombatSet = true;  // "engine non-combat"
        return facts;
    }

    // Selalleas holding a mob it cannot act on, at the instant the engine produced nothing.
    ReleaseFacts SelalleasHoldingAnUnreachableMob()
    {
        ReleaseFacts facts;
        facts.engineIsCombat = true;
        facts.alive = true;
        facts.actedThisTick = false;  // "no action that runs" - the engine's own answer
        facts.inInstance = false;     // all 178 rows are open world
        facts.led = false;
        facts.movingToIt = false;
        facts.castingOnIt = false;
        facts.targetPresent = true;
        facts.targetAlive = true;
        facts.targetAttacksMe = false;         // it is not on the bot
        facts.targetWithinMeleeRange = false;  // 23.8-101.3 yd
        return facts;
    }
}  // namespace

// CLAIM 1. This is the shape Thelengyl died in.
TEST(SoloCombatEntry, ABotAloneTheCoreHoldsInCombatDoesNotStayOnItsNonCombatSet)
{
    EXPECT_TRUE(Bot::Combat::TakesTheCombatEngine(ThelengylAloneUnderFire()))
        << "a bot alone, alive, held in combat by the core and with an attacker on it stays on its non-combat set: "
           "that is Thelengyl L27, 113.9 s of fight, 9.6 s of activity, its foe on 100 % health";
}

// THE OTHER DIRECTION, four ways, because each of them is a rule and not a detail.
TEST(SoloCombatEntry, TheEntryStaysOutOfEveryCaseThatIsNotItsOwn)
{
    // a group means a leader, and the leader does every check before it issues (rule 4)
    EntryFacts inAGroup = ThelengylAloneUnderFire();
    inAGroup.inGroup = true;
    EXPECT_FALSE(Bot::Combat::TakesTheCombatEngine(inAGroup));

    // the core does not hold it in combat: there is no contradiction to repair, and starting a fight is the grind
    // picker's business and priced by it
    EntryFacts outOfCombat = ThelengylAloneUnderFire();
    outOfCombat.coreInCombat = false;
    EXPECT_FALSE(Bot::Combat::TakesTheCombatEngine(outOfCombat));

    // in combat with nothing attacking it (a mob ran off, a lingering reference): there is no attacker to name, and
    // this rule names an ATTACKER and never a mob of its own choosing - that is what keeps it off the leader's plan
    EntryFacts noAttacker = ThelengylAloneUnderFire();
    noAttacker.hasAttackers = false;
    EXPECT_FALSE(Bot::Combat::TakesTheCombatEngine(noAttacker));

    // already on the combat set: nothing to do, and re-entering would re-pick the target every tick. The dead set
    // answers false the same way, which is what keeps a resurrected rooted bot out of this branch.
    EntryFacts already = ThelengylAloneUnderFire();
    already.engineOnNonCombatSet = false;
    EXPECT_FALSE(Bot::Combat::TakesTheCombatEngine(already));

    EntryFacts dead = ThelengylAloneUnderFire();
    dead.alive = false;
    EXPECT_FALSE(Bot::Combat::TakesTheCombatEngine(dead));
}

// CLAIM 2. 116 of 178 `stuck_target` rows are this, and they are healers.
TEST(SoloCombatEntry, ATargetThatNoLongerAttacksAndThatNothingPassesOnIsLetGo)
{
    EXPECT_TRUE(Bot::Combat::ReleasesTheTarget(SelalleasHoldingAnUnreachableMob()))
        << "the bot goes on holding a mob that does not attack it, that it cannot reach and that no action of its "
           "engine passes on: that is Selalleas, 52 rows over 36 mobs, 23.8-101.3 yd";
}

// IT IS LET GO WHETHER OR NOT THE MOB IS BUSY ELSEWHERE, which is the half the old guard refused: it returned at
// once if the bot or the mob was in combat with ANYBODY, and neither of those facts says whether this mob is this
// bot's business. Those two inputs are not in ReleaseFacts at all, deliberately - the fact that decides is
// `targetAttacksMe`.
TEST(SoloCombatEntry, AMobBusyWithSomebodyElseIsStillLetGo)
{
    // the same facts: nothing about who else the mob or the bot is fighting can change the answer, because nothing
    // about it is an input. The assertion is that the seam's inputs are the causal ones.
    ReleaseFacts const facts = SelalleasHoldingAnUnreachableMob();
    EXPECT_TRUE(Bot::Combat::ReleasesTheTarget(facts));
}

// THE OTHER DIRECTION, and this one protects the fight rather than the instrument.
TEST(SoloCombatEntry, TheReleaseKeepsEveryTargetThatIsStillTheBotsBusiness)
{
    // it is ON the bot: this is the bot's fight whatever else is true
    ReleaseFacts onMe = SelalleasHoldingAnUnreachableMob();
    onMe.targetAttacksMe = true;
    EXPECT_FALSE(Bot::Combat::ReleasesTheTarget(onMe));

    // the engine ran something this tick: it is working
    ReleaseFacts acted = SelalleasHoldingAnUnreachableMob();
    acted.actedThisTick = true;
    EXPECT_FALSE(Bot::Combat::ReleasesTheTarget(acted));

    // walking to it, or casting on it, or the last cast was on it: it IS being acted on
    ReleaseFacts walking = SelalleasHoldingAnUnreachableMob();
    walking.movingToIt = true;
    EXPECT_FALSE(Bot::Combat::ReleasesTheTarget(walking));
    ReleaseFacts casting = SelalleasHoldingAnUnreachableMob();
    casting.castingOnIt = true;
    EXPECT_FALSE(Bot::Combat::ReleasesTheTarget(casting));

    // in melee range it is reachable by definition; a rotation that produces nothing there is another defect, and
    // dropping the target would hide it
    ReleaseFacts inMelee = SelalleasHoldingAnUnreachableMob();
    inMelee.targetWithinMeleeRange = true;
    EXPECT_FALSE(Bot::Combat::ReleasesTheTarget(inMelee));

    // an instance fight belongs to the leader and to the encounter; a led member holds what its leader named (rule 4)
    ReleaseFacts instance = SelalleasHoldingAnUnreachableMob();
    instance.inInstance = true;
    EXPECT_FALSE(Bot::Combat::ReleasesTheTarget(instance));
    ReleaseFacts led = SelalleasHoldingAnUnreachableMob();
    led.led = true;
    EXPECT_FALSE(Bot::Combat::ReleasesTheTarget(led));

    // nothing to release, and a dead target is DropTargetAction's (it has loot to register)
    ReleaseFacts none = SelalleasHoldingAnUnreachableMob();
    none.targetPresent = false;
    EXPECT_FALSE(Bot::Combat::ReleasesTheTarget(none));
    ReleaseFacts deadTarget = SelalleasHoldingAnUnreachableMob();
    deadTarget.targetAlive = false;
    EXPECT_FALSE(Bot::Combat::ReleasesTheTarget(deadTarget));

    // not on the combat engine: there is no held target to speak of
    ReleaseFacts nonCombat = SelalleasHoldingAnUnreachableMob();
    nonCombat.engineIsCombat = false;
    EXPECT_FALSE(Bot::Combat::ReleasesTheTarget(nonCombat));
}

// THE TWO RULES CANNOT CHASE EACH OTHER, asserted rather than argued: the entry names an ATTACKER, and the release
// only lets go of a target that does NOT attack the bot, so no unit can be both at one instant.
TEST(SoloCombatEntry, TheEntryAndTheReleaseCannotFightOverOneUnit)
{
    ReleaseFacts release = SelalleasHoldingAnUnreachableMob();
    release.targetAttacksMe = true;  // the unit the entry would have named
    EXPECT_FALSE(Bot::Combat::ReleasesTheTarget(release));

    // and once the engine is taken, the entry stops firing, so the target is picked once and not every tick
    EntryFacts entry = ThelengylAloneUnderFire();
    entry.engineOnNonCombatSet = false;
    EXPECT_FALSE(Bot::Combat::TakesTheCombatEngine(entry));
}
