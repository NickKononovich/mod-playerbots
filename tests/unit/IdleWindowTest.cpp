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
