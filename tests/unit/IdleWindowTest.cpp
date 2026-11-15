/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [jrn51] THE IDLE WINDOW'S VERDICT, over windows COPIED OUT OF THE LIVE TABLE. Every fixture below is one row of
// pb_journal_idle from build b503, counters and all, named after the bot that wrote it - these are real fights and
// real levelling, not invented numbers, and that is why they can disagree with the criterion.
//
// WHAT THIS ASSERTS, and what was red before the fix:
//   * a window may contradict ITSELF - a global cooldown was running, so a spell had been cast, yet the same
//     window recorded no cast, no executed action and no cast in flight. Measured: 829 of the 2 453 world cases
//     (33.8 %) and 0 of the 2 301 dungeon windows of the same build. The old criterion had no such question and
//     called all of them real problems.
//   * an idle tick on the global cooldown or under the leader's command is not a defect: the first is the game's
//     own mechanic, the second an order nobody argues with. Measured: 13 614 of 64 112 dungeon idle ticks (21.2 %).

#include <gtest/gtest.h>

#include "IdleWindow.h"

using Journal::IdleWindow::Counts;

namespace
{
    // pb_journal_idle, b503, in the world, in combat: Dandolar (dps, 28 ticks) and Thelengyl (tank, 30 ticks).
    // Twenty ticks of global cooldown each and NOTHING recorded as done - the shape the blind recorder produced.
    Counts DandolarInTheWorld()
    {
        Counts c;
        c.ticks = 28;
        c.rangeLos = 2;
        c.gcd = 20;
        c.other = 6;
        return c;
    }

    Counts ThelengylInTheWorld()
    {
        Counts c;
        c.ticks = 30;
        c.gcd = 20;
        c.other = 10;
        return c;
    }

    // The same build, the same instrument, inside a dungeon: Bhogagnuth (dps, 75 ticks) spent 53 of them under its
    // leader's command and 17 on the global cooldown, and cast 3 spells. Nothing here is a defect.
    Counts BhogagnuthUnderItsLeader()
    {
        Counts c;
        c.ticks = 75;
        c.casts = 3;
        c.nonCast = 2;
        c.gcd = 17;
        c.leaderHold = 53;
        return c;
    }

    Counts KylanorUnderItsLeader()
    {
        Counts c;
        c.ticks = 77;
        c.casts = 9;
        c.castWait = 56;
        c.nonCast = 2;
        c.gcd = 7;
        c.leaderHold = 3;
        return c;
    }

    // Relara (tank, 647 ticks) held no target for 587 of them with two casts in the whole window: a real defect,
    // and the one this table exists for.
    Counts RelaraWithNoTarget()
    {
        Counts c;
        c.ticks = 647;
        c.casts = 2;
        c.nonCast = 20;
        c.noTarget = 587;
        c.rangeLos = 34;
        c.moving = 2;
        c.other = 2;
        return c;
    }

    Counts KylanorWithNoTarget()
    {
        Counts c;
        c.ticks = 95;
        c.casts = 6;
        c.castWait = 22;
        c.nonCast = 8;
        c.noTarget = 29;
        c.rangeLos = 3;
        c.moving = 15;
        c.gcd = 9;
        c.leaderHold = 2;
        c.other = 1;
        return c;
    }

    // Rumanatol (healer, 26 ticks, zone 406) wrote the largest key of the 522 - 307 cases of one bot. It had a cast
    // in flight, so its global cooldown is explained and the window does NOT contradict itself: the contradiction
    // is a proof when it fires and says nothing when it does not, which is why this fixture is here.
    Counts RumanatolInTheWorld()
    {
        Counts c;
        c.ticks = 26;
        c.castWait = 2;
        c.gcd = 9;
        c.other = 15;
        return c;
    }

    // The window the instrument prints every 15 s when nothing idled at all: 406 of b503's 3 900 were this.
    Counts NothingIdled()
    {
        Counts c;
        c.ticks = 40;
        c.casts = 12;
        c.nonCast = 28;
        return c;
    }
}  // namespace

TEST(IdleWindow, AGlobalCooldownWithNothingDoneContradictsTheWindowItself)
{
    EXPECT_TRUE(Journal::IdleWindow::ContradictsItself(DandolarInTheWorld()));
    EXPECT_TRUE(Journal::IdleWindow::ContradictsItself(ThelengylInTheWorld()));
}

TEST(IdleWindow, ACastInFlightExplainsTheGlobalCooldownAndIsNoContradiction)
{
    EXPECT_FALSE(Journal::IdleWindow::ContradictsItself(RumanatolInTheWorld()));
}

TEST(IdleWindow, NoDungeonWindowOfTheMeasuredBuildContradictsItself)
{
    EXPECT_FALSE(Journal::IdleWindow::ContradictsItself(BhogagnuthUnderItsLeader()));
    EXPECT_FALSE(Journal::IdleWindow::ContradictsItself(KylanorUnderItsLeader()));
    EXPECT_FALSE(Journal::IdleWindow::ContradictsItself(RelaraWithNoTarget()));
    EXPECT_FALSE(Journal::IdleWindow::ContradictsItself(KylanorWithNoTarget()));
}

TEST(IdleWindow, ACastInFlightCountsAsActivityAndSoDoesANonCastAction)
{
    EXPECT_TRUE(Journal::IdleWindow::ActivityRecorded(RumanatolInTheWorld()));
    EXPECT_TRUE(Journal::IdleWindow::ActivityRecorded(BhogagnuthUnderItsLeader()));
    EXPECT_FALSE(Journal::IdleWindow::ActivityRecorded(DandolarInTheWorld()));
}

TEST(IdleWindow, TheGlobalCooldownAndTheLeadersOrderAreNotDefects)
{
    EXPECT_EQ(70u, Journal::IdleWindow::IdleTicks(BhogagnuthUnderItsLeader()));
    EXPECT_EQ(70u, Journal::IdleWindow::MechanicTicks(BhogagnuthUnderItsLeader()));
    EXPECT_STREQ("no", Journal::IdleWindow::Real(BhogagnuthUnderItsLeader()));
    EXPECT_STREQ("no", Journal::IdleWindow::Real(KylanorUnderItsLeader()));
}

TEST(IdleWindow, AContradictoryWindowIsNotARealProblemUntilTheInstrumentIsBelievable)
{
    EXPECT_STREQ("no", Journal::IdleWindow::Real(DandolarInTheWorld()));
    EXPECT_STREQ("no", Journal::IdleWindow::Real(ThelengylInTheWorld()));
}

TEST(IdleWindow, ATankWithNoTargetForMostOfItsWindowStaysAProblem)
{
    EXPECT_STREQ("yes", Journal::IdleWindow::Real(RelaraWithNoTarget()));
    EXPECT_STREQ("yes", Journal::IdleWindow::Real(KylanorWithNoTarget()));
    EXPECT_STREQ("yes", Journal::IdleWindow::Real(RumanatolInTheWorld()));
}

TEST(IdleWindow, AWindowInWhichNothingIdledIsNoProblemAtAll)
{
    EXPECT_EQ(0u, Journal::IdleWindow::IdleTicks(NothingIdled()));
    EXPECT_STREQ("no", Journal::IdleWindow::Real(NothingIdled()));
}

TEST(IdleWindow, TheLegitimatePartIsNeverMoreThanTheIdlePart)
{
    Counts const all[] = {DandolarInTheWorld(),      ThelengylInTheWorld(),  BhogagnuthUnderItsLeader(),
                          KylanorUnderItsLeader(),   RelaraWithNoTarget(),   KylanorWithNoTarget(),
                          RumanatolInTheWorld(),     NothingIdled()};
    for (Counts const& c : all)
        EXPECT_LE(Journal::IdleWindow::MechanicTicks(c), Journal::IdleWindow::IdleTicks(c));
}

TEST(IdleWindow, TheVerdictInWordsNamesTheNumbersOfThatVeryWindow)
{
    std::string const contradictory = Journal::IdleWindow::RealWhy(ThelengylInTheWorld());
    EXPECT_NE(std::string::npos, contradictory.find("20"));          // the global cooldown ticks
    EXPECT_NE(std::string::npos, contradictory.find("cooldown"));
    std::string const held = Journal::IdleWindow::RealWhy(BhogagnuthUnderItsLeader());
    EXPECT_NE(std::string::npos, held.find("70"));                   // the idle ticks, all of them legitimate
    EXPECT_NE(std::string::npos, held.find("leader"));
}

// [jrn52] THE CHAIN'S ORDER, which is where the largest of b503's ten dungeon causes came from. `!target` and
// "out of reach or sight" were asked BEFORE `isMoving()`, so a bot WALKING to the next pack was counted as "it
// had no target" and a bot RUNNING AT its target as "out of range" - and the `moving` bucket could only catch the
// residue where the target was already in reach and in sight. Measured over b503's dungeon combat windows:
// `moving` 6 002 ticks of 96 070 (6.2 %), against `noTarget` 46 119 and `rangeLos` 14 851; and of the 548
// noTarget-dominant windows, 373 (68.1 %) belong to a bot that covered 20 yd or more during the window, 9 to one
// its leader had ordered to stand, 118 to one that moved 5-20 yd, and 48 to one that stood inside 5 yd.

namespace
{
    Journal::IdleWindow::TickFacts WalkingWithNoTarget()
    {
        Journal::IdleWindow::TickFacts f;
        f.moving = true;
        return f;
    }

    Journal::IdleWindow::TickFacts StandingWithNoTarget() { return Journal::IdleWindow::TickFacts{}; }

    Journal::IdleWindow::TickFacts RunningAtATargetOutOfReach()
    {
        Journal::IdleWindow::TickFacts f;
        f.hasLiveTarget = true;
        f.outOfReachOrLos = true;
        f.moving = true;
        return f;
    }

    Journal::IdleWindow::TickFacts StandingShortOfATargetOutOfReach()
    {
        Journal::IdleWindow::TickFacts f;
        f.hasLiveTarget = true;
        f.outOfReachOrLos = true;
        return f;
    }
}  // namespace

TEST(IdleWindowChain, WalkingWithNoTargetIsNotTheSameTickAsStandingWithNoTarget)
{
    EXPECT_EQ(Journal::IdleWindow::Tick::NoTargetMoving, Journal::IdleWindow::Classify(WalkingWithNoTarget()));
    EXPECT_EQ(Journal::IdleWindow::Tick::NoTargetStill, Journal::IdleWindow::Classify(StandingWithNoTarget()));
}

TEST(IdleWindowChain, RunningAtATargetIsNotTheSameTickAsStandingShortOfIt)
{
    EXPECT_EQ(Journal::IdleWindow::Tick::RangeLosMoving,
              Journal::IdleWindow::Classify(RunningAtATargetOutOfReach()));
    EXPECT_EQ(Journal::IdleWindow::Tick::RangeLosStill,
              Journal::IdleWindow::Classify(StandingShortOfATargetOutOfReach()));
}

TEST(IdleWindowChain, WhatTheBotDidBeatsEveryIdleAnswerAndACastInFlightBeatsThat)
{
    Journal::IdleWindow::TickFacts f = WalkingWithNoTarget();
    f.actionRan = true;
    EXPECT_EQ(Journal::IdleWindow::Tick::Action, Journal::IdleWindow::Classify(f));
    f.cast = true;
    EXPECT_EQ(Journal::IdleWindow::Tick::Cast, Journal::IdleWindow::Classify(f));
    f.casting = true;
    EXPECT_EQ(Journal::IdleWindow::Tick::CastWait, Journal::IdleWindow::Classify(f));
}

TEST(IdleWindowChain, TheMovingBucketIsOnlyForATargetAlreadyInReachAndInSight)
{
    Journal::IdleWindow::TickFacts f;
    f.hasLiveTarget = true;
    f.moving = true;
    EXPECT_EQ(Journal::IdleWindow::Tick::Moving, Journal::IdleWindow::Classify(f));
    f.moving = false;
    EXPECT_EQ(Journal::IdleWindow::Tick::AskGcd, Journal::IdleWindow::Classify(f));
}

TEST(IdleWindowChain, TheWorldReadsStayWithTheCallerAndTheirOrderIsStillAsserted)
{
    EXPECT_EQ(Journal::IdleWindow::Tick::Gcd, Journal::IdleWindow::ClassifyAfterGcd(true, true));
    EXPECT_EQ(Journal::IdleWindow::Tick::LeaderHold, Journal::IdleWindow::ClassifyAfterGcd(false, true));
    EXPECT_EQ(Journal::IdleWindow::Tick::AskThreat, Journal::IdleWindow::ClassifyAfterGcd(false, false));
}

TEST(IdleWindowChain, TheTravelPartIsCountedAndIsNOTCalledLegitimate)
{
    Counts c;
    c.ticks = 62;
    c.casts = 4;
    c.noTarget = 28;
    c.noTargetMoving = 25;
    c.rangeLos = 6;
    c.rangeLosMoving = 4;
    c.gcd = 3;
    EXPECT_EQ(29u, Journal::IdleWindow::TravelTicks(c));
    // the walk is named by number, and the window is still a problem: a purposeful walk and a circle look alike
    // here, and which it was belongs to the route domain
    EXPECT_EQ(3u, Journal::IdleWindow::MechanicTicks(c));
    EXPECT_STREQ("yes", Journal::IdleWindow::Real(c));
    EXPECT_NE(std::string::npos, Journal::IdleWindow::RealWhy(c).find("29"));
}

// [jrn55] THE TICKS THE CHAIN ANSWERED BEFORE THE ENGINE WAS EVER ASKED.
//
// Every fixture here is one row of pb_journal_idle, at_ms <= 1794764400000, named after the bot that wrote it.
// The engine records on EVERY tick what it would have done and why it did not (NoteIdleDiagRefusal, the first
// refusal of the relevance-ordered queue); that pair is COUNTED only on a tick that reaches `other`, and
// `noTarget`, `rangeLos` and `moving` are decided earlier. So on those ticks the answer existed and was dropped.
//
// MEASURED, and it is a whole role rather than an edge: a healer in a dungeon in combat wrote 1 028 windows over
// three builds (b502 595, b503 310, b504 123) holding 45 234 ticks, of which 38 090 idled - and ALL 38 090 are in
// `noTarget`, with `rangeLos`, `moving`, `gcd`, `threat`, `leaderHold` and `other` exactly zero in every window,
// and ref_top_action NULL in 1 028 of 1 028. A healer in a dungeon has no hostile "current target" (the leader
// gives the pull to the tank), so the chain's first question answers the window. `idle|healer|noTarget` - 923
// cases, the ledger's largest key - names the only bucket such a healer can reach.
//
// THE REVERSE DIRECTION IS ASSERTED TOO, because a rule saying "every window names no cause" would pass the
// first two assertions: the same healers OUTSIDE a dungeon reach every bucket (217 windows with rangeLos, 560
// with moving, 1 357 with gcd, 1 146 with other), and a window that did reach the engine's answer must not be
// described as nameless. And the verdict must stay "yes": the idle is real, only its reason is unrecorded.
namespace
{
    // pb_journal_idle, b504, Aestymro (healer, Wailing Caverns, in combat): 75 ticks, every single one idle with
    // no live target, and nothing else counted anywhere.
    Counts AestymroHealingInADungeon()
    {
        Counts c;
        c.ticks = 75;
        c.noTarget = 75;
        return c;
    }

    // pb_journal_idle, b502/b503, Pherua (healer, in combat, in a dungeon): the same shape with the healer's own
    // work visible beside it - one cast and one executed action, 72 of 74 ticks idle, all of them `noTarget`.
    Counts PheruaWithItsOwnWorkBesideIt()
    {
        Counts c;
        c.ticks = 74;
        c.casts = 1;
        c.nonCast = 1;
        c.noTarget = 72;
        return c;
    }
}  // namespace

TEST(IdleWindowUnasked, TheTicksAnsweredBeforeTheEngineWasAskedAreCounted)
{
    Counts const aestymro = AestymroHealingInADungeon();
    EXPECT_EQ(75u, Journal::IdleWindow::IdleTicks(aestymro));
    EXPECT_EQ(75u, Journal::IdleWindow::UnaskedTicks(aestymro));
    EXPECT_TRUE(Journal::IdleWindow::NamesNoCause(aestymro));

    Counts const pherua = PheruaWithItsOwnWorkBesideIt();
    EXPECT_EQ(72u, Journal::IdleWindow::UnaskedTicks(pherua));
    EXPECT_TRUE(Journal::IdleWindow::NamesNoCause(pherua));
}

TEST(IdleWindowUnasked, AWindowWhoseWholeIdleWasAnsweredFirstSaysItNamesNoCause)
{
    std::string const why = Journal::IdleWindow::RealWhy(AestymroHealingInADungeon());
    EXPECT_NE(std::string::npos, why.find("names a bucket and no cause"))
        << "the window says the engine 'says in which bucket' as if the bucket were the cause, while all 75 of "
           "its idle ticks were decided before the engine's own answer was ever read: 38 090 of 38 090 idle "
           "ticks of a healer in a dungeon, 1 028 windows, ref_top_action NULL in all of them";
    EXPECT_NE(std::string::npos, why.find("75"));
}

TEST(IdleWindowUnasked, TheVerdictOfSuchAWindowStaysYesBecauseTheIdleIsReal)
{
    // the inverse of the obvious "shrug and call it the instrument": nothing in this window contradicts itself,
    // the bot really idled those ticks, and 941 of the 1 028 windows show it working in the same breath (1 272
    // casts, 3 493 ticks with a cast in flight). Calling it "no" would hide a real idle behind a complaint.
    EXPECT_STREQ("yes", Journal::IdleWindow::Real(AestymroHealingInADungeon()));
    EXPECT_STREQ("yes", Journal::IdleWindow::Real(PheruaWithItsOwnWorkBesideIt()));
}

TEST(IdleWindowUnasked, AWindowThatDidReachTheEnginesAnswerIsNotCalledNameless)
{
    // Zharet, b504, dps in Wailing Caverns: 74 ticks, 2 casts, 11 with a cast in flight, 1 executed action, and
    // of its 60 idle ticks 54 DID reach `other`, where the engine named "fear counter tremor totem - IMPOSSIBLE".
    // This fixture is deliberately one that gets as far as the main sentence - it neither contradicts itself nor
    // is all mechanic - so the assertion below can actually come out false.
    Counts zharet;
    zharet.ticks = 74;
    zharet.casts = 2;
    zharet.castWait = 11;
    zharet.nonCast = 1;
    zharet.noTarget = 5;
    zharet.noTargetMoving = 5;
    zharet.moving = 1;
    zharet.other = 54;
    ASSERT_FALSE(Journal::IdleWindow::ContradictsItself(zharet));
    ASSERT_NE(Journal::IdleWindow::IdleTicks(zharet), Journal::IdleWindow::MechanicTicks(zharet));
    EXPECT_EQ(6u, Journal::IdleWindow::UnaskedTicks(zharet));
    EXPECT_FALSE(Journal::IdleWindow::NamesNoCause(zharet));
    EXPECT_EQ(std::string::npos, Journal::IdleWindow::RealWhy(zharet).find("names a bucket and no cause"));

    // and the leader's own hold is asked AFTER the chain's first three too: Bhogagnuth's 53 leaderHold + 17 gcd
    // ticks are all named, so this window is not nameless either
    Counts const bhogagnuth = BhogagnuthUnderItsLeader();
    EXPECT_EQ(0u, Journal::IdleWindow::UnaskedTicks(bhogagnuth));
    EXPECT_FALSE(Journal::IdleWindow::NamesNoCause(bhogagnuth));
}

TEST(IdleWindowUnasked, AWindowWithNoIdleTickAtAllIsNotCalledNameless)
{
    // the guard against "zero equals zero": a clean window (every bucket zero) must not be swept in by an
    // equality that would hold trivially, and it already has its own verdict and its own sentence
    Counts c;
    c.ticks = 40;
    c.casts = 12;
    c.nonCast = 28;
    EXPECT_EQ(0u, Journal::IdleWindow::IdleTicks(c));
    EXPECT_FALSE(Journal::IdleWindow::NamesNoCause(c));
    EXPECT_STREQ("no", Journal::IdleWindow::Real(c));
    EXPECT_EQ(std::string::npos, Journal::IdleWindow::RealWhy(c).find("names a bucket and no cause"));
}

// [jrn56] WHICH TARGET THE CHAIN IS TALKING ABOUT. The question, not the order, was wrong for a whole role:
// `hasLiveTarget` came from "current target", the HOSTILE target, and in a dungeon the leader's plan gives the
// pull to the tank, so a healer has none. Measured with the corpus closed at at_ms <= 1794764479861: a healer in
// a dungeon in combat wrote 1 028 windows holding 45 234 ticks, of which 38 090 idled - all 38 090 in `noTarget`,
// with every other bucket exactly zero in every window, over three builds. Those two keys hold 1 021 of the
// ledger's 3 606 unexplained idle cases (28.3 %) and name no cause at all.
//
// WHAT THE REVERSE ASSERTIONS GUARD, because "a healer always has a target" would pass the first one:
//   * a dps or a tank must be untouched to the byte - its job target IS the hostile one, and reading a hurt
//     member for it would move its ticks out of `noTarget` on a fact that is not its job;
//   * a healer that IS fighting something keeps that mob as its target, which is why the same healers outside a
//     dungeon already reach every bucket (217 windows with rangeLos, 560 moving, 1 357 gcd, 1 146 other);
//   * a healer with neither target must still come out "no live target" - the honest version of the same answer.
TEST(IdleWindowTarget, AHealerWithNothingHostileIsJudgedByTheTargetItHeals)
{
    Journal::IdleWindow::TargetLook look;
    look.heals = true;
    look.hostileAlive = false;  // in a dungeon the pull is the tank's, by the leader's plan
    look.healAlive = true;
    look.healOutOfReachOrLos = false;

    Journal::IdleWindow::Target const t = Journal::IdleWindow::Looked(look);
    EXPECT_TRUE(t.live) << "the healer's own target is the one it heals, and asking only about a hostile target "
                           "put all 38 090 idle ticks of 1 028 dungeon windows into noTarget with every other "
                           "bucket at exactly zero";
    EXPECT_FALSE(t.outOfReachOrLos);

    // and when the one it heals is too far or out of sight, that is the bucket it belongs in - a tick that used
    // to be called "no target" is now a tick that says the healer could not reach its man
    look.healOutOfReachOrLos = true;
    Journal::IdleWindow::Target const far = Journal::IdleWindow::Looked(look);
    EXPECT_TRUE(far.live);
    EXPECT_TRUE(far.outOfReachOrLos);
}

TEST(IdleWindowTarget, AHealerThatIsFightingSomethingKeepsThatMobAsItsTarget)
{
    // the reverse side: a healer grinding in the world HAS a hostile target, and the bucket has always said so.
    // If the heal target won instead, a healer fighting a mob out of reach would be reported as in reach of a
    // party member standing next to it.
    Journal::IdleWindow::TargetLook look;
    look.heals = true;
    look.hostileAlive = true;
    look.hostileOutOfReachOrLos = true;
    look.healAlive = true;
    look.healOutOfReachOrLos = false;

    Journal::IdleWindow::Target const t = Journal::IdleWindow::Looked(look);
    EXPECT_TRUE(t.live);
    EXPECT_TRUE(t.outOfReachOrLos) << "the mob it is actually fighting decides the reach question, not a member "
                                      "standing beside it";
}

TEST(IdleWindowTarget, ADpsOrTankIsUnchangedToTheByte)
{
    // nothing about a tank or a dps may move: its job target is the hostile one, and a hurt member beside it is
    // not its business. This is the assertion a rule of "anybody near a hurt member has a target" would fail.
    for (bool outOfReach : {false, true})
    {
        Journal::IdleWindow::TargetLook look;
        look.heals = false;
        look.hostileAlive = true;
        look.hostileOutOfReachOrLos = outOfReach;
        look.healAlive = true;  // there IS a hurt member; it must not count for this bot
        look.healOutOfReachOrLos = !outOfReach;
        Journal::IdleWindow::Target const t = Journal::IdleWindow::Looked(look);
        EXPECT_TRUE(t.live);
        EXPECT_EQ(outOfReach, t.outOfReachOrLos);
    }

    Journal::IdleWindow::TargetLook none;
    none.heals = false;
    none.healAlive = true;  // again: a hurt member in sight of a dps is not a target of the dps
    Journal::IdleWindow::Target const t = Journal::IdleWindow::Looked(none);
    EXPECT_FALSE(t.live) << "a dps with nothing hostile has no target, whatever the healer's value says";
}

TEST(IdleWindowTarget, AHealerWithNeitherTargetStillHasNone)
{
    // the honest version of the old answer, and the assertion that a blanket "a healer always has a target"
    // would fail: with nothing hostile and nobody to heal, the window really is "nothing to act on".
    Journal::IdleWindow::TargetLook look;
    look.heals = true;
    Journal::IdleWindow::Target const t = Journal::IdleWindow::Looked(look);
    EXPECT_FALSE(t.live);
    EXPECT_FALSE(t.outOfReachOrLos);
}

TEST(IdleWindowTarget, TheChainItselfIsUnchangedAndStillAsksMovementInsideEachBranch)
{
    // the target question feeds the SAME chain: a healer whose man is out of reach while it walks to him lands in
    // the walking part of rangeLos, which is the [jrn52] order still holding after the question changed.
    Journal::IdleWindow::TargetLook look;
    look.heals = true;
    look.healAlive = true;
    look.healOutOfReachOrLos = true;
    Journal::IdleWindow::Target const t = Journal::IdleWindow::Looked(look);

    Journal::IdleWindow::TickFacts facts;
    facts.hasLiveTarget = t.live;
    facts.outOfReachOrLos = t.outOfReachOrLos;
    facts.moving = true;
    EXPECT_EQ(Journal::IdleWindow::Tick::RangeLosMoving, Journal::IdleWindow::Classify(facts));
    facts.moving = false;
    EXPECT_EQ(Journal::IdleWindow::Tick::RangeLosStill, Journal::IdleWindow::Classify(facts));
}
