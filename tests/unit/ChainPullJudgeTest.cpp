/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [test1] The chain pull judge's seam, on inputs written out by hand: the gates themselves, their order, and the
// two properties the whole rig rests on - the judge reads no clock of its own, and an unrecorded fact is not a
// passed gate. The fixture-driven tests live in ChainPullCorpusTest.cpp.

#include "ChainPullJudge.h"

#include <gtest/gtest.h>

using namespace Leader::Judge;

namespace
{
    // a pack the judge would take: every gate passed, the numbers comfortable
    ChainPullFacts Taken()
    {
        ChainPullFacts facts;
        facts.nowMs = 1000;
        facts.timing = {0.5f, 0.0f, 3.0f, 0.5f};
        facts.survival = {1.0f, 0.2f};
        facts.packMobs = 2;
        facts.fightMobs = 1;
        facts.unionMobs = 3;
        facts.bossInPack = Tri::No;
        facts.walkerStop = Tri::No;
        facts.openerReaches = Tri::Yes;
        facts.packReached = Tri::Yes;
        facts.groupBuffed = Tri::Yes;
        facts.everyoneAlive = Tri::Yes;
        facts.tankHolds = Tri::Yes;
        facts.overloadFits = Tri::Yes;
        facts.healerKeeps = Tri::Yes;
        facts.healerHasMana = Tri::Yes;
        facts.wayInQuiet = Tri::Yes;
        return facts;
    }
}

TEST(ChainPullJudge, DueWhenTheFightHasLessLeftThanTheTakingCosts)
{
    EXPECT_TRUE(DueNow({1.0f, 0.0f, 3.0f, 0.5f}));
    EXPECT_FALSE(DueNow({9.0f, 0.0f, 3.0f, 0.5f}));
}

TEST(ChainPullJudge, EqualTimesAreNotDue)
{
    // the boundary is strict: a fight with exactly as much left as the taking costs is not yet due. A recorded
    // situation that lands on the boundary cannot be replayed from the journal's one-decimal numbers, which is why
    // the corpus test skips its rounding band instead of guessing.
    EXPECT_FALSE(DueNow({3.5f, 0.0f, 3.0f, 0.5f}));
}

TEST(ChainPullJudge, TheTankMustStandAboveItsOwnModelledDrop)
{
    EXPECT_TRUE(TankOutlastsDrop({1.0f, 0.2f}));
    // the margin is the point: equal is not above. A fight taken with no margin is a calculation error, and a death
    // out of it is that error showing, not bad luck.
    EXPECT_FALSE(TankOutlastsDrop({0.2f, 0.2f}));
    EXPECT_FALSE(TankOutlastsDrop({0.1f, 0.2f}));
}

TEST(ChainPullJudge, ATakenPackPassesEveryGate)
{
    ChainPullReplay const replay = ReplayChainPull(Taken());
    EXPECT_EQ(replay.gate, Gate::Taken);
    EXPECT_TRUE(replay.taken);
    EXPECT_TRUE(replay.due);
}

TEST(ChainPullJudge, TheHealthGateTripsOnANegativeMargin)
{
    ChainPullFacts facts = Taken();
    facts.survival = {0.15f, 0.40f};
    ChainPullReplay const replay = ReplayChainPull(facts);
    EXPECT_EQ(replay.gate, Gate::Health);
    EXPECT_FALSE(replay.taken);
}

TEST(ChainPullJudge, TheOpenerGateIsReadBeforeTheTrigger)
{
    // the recorded order: 142 of the journal's 159 chain pull judgements are "opener", and some of them are not due -
    // so the opener is read first. A replay that tested the trigger first would answer "waits" for them and every
    // comparison against the record would be a false disagreement.
    ChainPullFacts facts = Taken();
    facts.openerReaches = Tri::No;
    facts.timing = {9.0f, 0.0f, 3.0f, 0.5f};  // not due either
    ChainPullReplay const replay = ReplayChainPull(facts);
    EXPECT_EQ(replay.gate, Gate::Opener);
    EXPECT_FALSE(replay.due);
}

TEST(ChainPullJudge, AnUnrecordedFactLeavesTheReplayUndecided)
{
    // absence is not zero and not "passed": a fixture that never carried the healer's verdict must not be replayed
    // into a taken pull.
    ChainPullFacts facts = Taken();
    facts.healerKeeps = Tri::Unknown;
    ChainPullReplay const replay = ReplayChainPull(facts);
    EXPECT_EQ(replay.gate, Gate::Undecided);
    EXPECT_FALSE(replay.taken);
}

TEST(ChainPullJudge, AnUnrecordedSurvivalNumberLeavesTheReplayUndecided)
{
    ChainPullFacts facts = Taken();
    facts.survival = {-1.0f, -1.0f};  // the emitter did not write the tank's health or the drop
    EXPECT_EQ(ReplayChainPull(facts).gate, Gate::Undecided);
}

TEST(ChainPullJudge, TheClockIsAnInputSoTheVerdictDoesNotMoveWithTime)
{
    // the whole stand rests on this: run the same facts at two different clocks and the verdict is the same. If any
    // gate read the machine's clock, this would be the test that caught it.
    ChainPullFacts early = Taken();
    ChainPullFacts late = Taken();
    early.nowMs = 1;
    late.nowMs = 7 * 24 * 60 * 60 * 1000u;
    EXPECT_EQ(ReplayChainPull(early).gate, ReplayChainPull(late).gate);
    EXPECT_EQ(ReplayChainPull(early).taken, ReplayChainPull(late).taken);
}

TEST(ChainPullJudge, EveryGateHasAName)
{
    // the names are what a replayed verdict and a recorded one compare as; a nameless gate would compare equal to
    // everything that is also nameless.
    EXPECT_STREQ(GateName(Gate::Undecided), "undecided");
    EXPECT_STREQ(GateName(Gate::Taken), "taken");
    EXPECT_STREQ(GateName(Gate::PackReach), "pack_reach");
    EXPECT_STREQ(GateName(Gate::Health), "health");
}
