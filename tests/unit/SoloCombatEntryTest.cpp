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

// [stuck259] AND THE MEMO THAT HOLDS THAT RELEASE: by WHAT does the mob stop being at the stand it was judged at.
//
// The fixtures are live rows. `pb_journal_stuck`, kind `stuck_target`: 199 cases, 25 of them the same bot and the
// same mob again inside a minute, and in 20 of those 25 the bot had not moved a yard - only the mob had. The mobs
// are `acore_world.creature` rows: Giant Moss Creeper 15453 / 15468 and Vicious Gray Bear 15469 (MovementType 1,
// wander_distance 10), Syndicate Shadow Mage 15584 (wander_distance 3), Rot Hide Savage 18283 (MovementType 0,
// wander_distance 0). The old measure was Leader::LEADER_COMMIT_MOVED_YD = 12 yd - larger than either whole area.
#include "../../src/Ai/World/Rpg/MobStand.h"

namespace
{
    using Rpg::MobStand::Facts;
    using Rpg::MobStand::LeftItsArea;

    // Giant Moss Creeper 15453: it roams its 10 yd area, body 1.5 yd, and stands 9 yd from its spawn point
    Facts CreeperAcrossItsOwnArea()
    {
        Facts facts;
        facts.wanders = true;
        facts.areaRadius = 10.0f;
        facts.fromCenterYd = 9.0f;
        facts.bodyYd = 1.5f;
        return facts;
    }
}

TEST(MobStand, AWandererInsideItsOwnAreaIsStillAtTheStandItWasJudgedAt)
{
    EXPECT_FALSE(LeftItsArea(CreeperAcrossItsOwnArea()));
    // the far side of the same area: 18 yd of walking from the near side, and still the same arrangement
    Facts far = CreeperAcrossItsOwnArea();
    far.fromCenterYd = 10.0f;
    EXPECT_FALSE(LeftItsArea(far));
}

TEST(MobStand, AWandererOutOfItsAreaHasLeftTheStandEvenShortOfTwelveYards)
{
    // THE RED ONE on the old rule: the mob stands 11.6 yd from its center, 1.6 yd outside its own 10 yd area, and
    // it has moved less than the leader's commitment measure from where the verdict was given - so the memo held
    // and the same bot picked the same mob again. It is out of its area: the walk to it is another question.
    Facts out = CreeperAcrossItsOwnArea();
    out.fromCenterYd = 11.6f;
    EXPECT_TRUE(LeftItsArea(out));
    EXPECT_NEAR(Rpg::MobStand::OutsideItsAreaYd(out), 1.6f, 0.01f);
}

TEST(MobStand, AMobThatHoldsOneSpotLeavesItByItsOwnBodyAndNotByTwelveYards)
{
    // Rot Hide Savage 18283: MovementType 0, wander_distance 0 - its area is the spot it holds
    Facts stander;
    stander.wanders = false;
    stander.areaRadius = 0.0f;
    stander.fromCenterYd = 1.0f;
    stander.bodyYd = 1.5f;
    EXPECT_FALSE(LeftItsArea(stander));  // within its own body of its spot: it is standing there
    stander.fromCenterYd = 4.0f;
    EXPECT_TRUE(LeftItsArea(stander));   // it walked off: 4 yd, and nothing waits for 12
}

TEST(MobStand, AWanderDistanceOfItsOwnIsWhatCounted)
{
    // Syndicate Shadow Mage 15584 roams 3 yd, so 5 yd from its center is already out - the same 5 yd leaves the
    // Creeper's 10 yd area untouched. The area is the mob's, not a number shared by every mob.
    Facts mage;
    mage.wanders = true;
    mage.areaRadius = 3.0f;
    mage.fromCenterYd = 5.0f;
    mage.bodyYd = 1.0f;
    EXPECT_TRUE(LeftItsArea(mage));
    Facts creeper = CreeperAcrossItsOwnArea();
    creeper.fromCenterYd = 5.0f;
    EXPECT_FALSE(LeftItsArea(creeper));
}
