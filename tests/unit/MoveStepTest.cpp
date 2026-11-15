/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [move51] THE DEFECT, AS TWO REFUTABLE CLAIMS:
//   1. "a movement that did not start the step does not report success" - before the fix `MoveTo` returned true
//      after all three silent exits of `DoMovePoint` and set `LastMovement` as if a walk were on;
//   2. "a leader that was refused sees it as a refusal" - before the fix the leader was acked "walks to the spot"
//      and held the order, while the member's next tick refused the re-ask of that same walk (IsDuplicateMove /
//      IsWaitingForLastMove) and it stood still.
//
// Both directions are asserted, because the easy half-fix is to make everything a refusal: then a walk that is
// genuinely under way (the group's water drop in the air) would be reported as one too, and the leader would pick
// another spot in the middle of a jump. So the test also pins the exit that MAY report success.
//
// Measured cost of the defect in the live journal (the largest key of pb_journal_issue): 73 rows of
// `A:leader move order - OK` while the member stood, up to 51 s in one episode.

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "MoveStep.h"

namespace
{
    // every exit that put no order into the motion master: the step did not start
    std::initializer_list<Move::StepStart> const NO_STEP = {
        Move::StepStart::NoMover, Move::StepStart::NoCompletePath, Move::StepStart::DungeonPathRefused};
}  // namespace

// CLAIM 1. The three exits that ordered nothing are not success, and the one that ordered a walk is.
TEST(MoveStep, AStepThatDidNotStartIsNoSuccess)
{
    for (Move::StepStart const step : NO_STEP)
    {
        EXPECT_FALSE(Move::StepStarted(step)) << "exit " << uint32_t(step) << " started no step";
        EXPECT_FALSE(Move::StepMoves(step)) << "exit " << uint32_t(step) << " reported success without a step";
    }

    EXPECT_TRUE(Move::StepStarted(Move::StepStart::Started));
    EXPECT_TRUE(Move::StepMoves(Move::StepStart::Started));
}

// CLAIM 1, the other direction: the jump already in the air started no step HERE, yet the bot does move - it may be
// reported as success, and it is not a refusal. (JudgeWaterDrop == Under: the effect motion generator runs, or the
// bot carries MOVEMENTFLAG_FALLING.)
TEST(MoveStep, AJumpAlreadyInTheAirMovesButStartedNoStepHere)
{
    EXPECT_FALSE(Move::StepStarted(Move::StepStart::AlreadyUnderWay));
    EXPECT_TRUE(Move::StepMoves(Move::StepStart::AlreadyUnderWay));
    EXPECT_STREQ("", Move::StepRefusal(Move::StepStart::AlreadyUnderWay));
    EXPECT_FALSE(Move::LeaderDecidesAgain(Move::StepStart::AlreadyUnderWay));
}

// CLAIM 2. Every refusal reaches the leader AS a refusal: it has words, and those words are not one of the
// transient ones the leader walks the plain path after ([lead372]).
TEST(MoveStep, TheLeaderSeesEveryRefusalAsARefusal)
{
    for (Move::StepStart const step : NO_STEP)
    {
        char const* const refusal = Move::StepRefusal(step);
        ASSERT_NE(nullptr, refusal);
        EXPECT_STRNE("", refusal) << "exit " << uint32_t(step) << " refused without words";
        EXPECT_FALSE(Move::TransientRefusal(refusal))
            << "exit " << uint32_t(step) << " refusal read as transient: \"" << refusal << "\"";
        EXPECT_TRUE(Move::LeaderDecidesAgain(step)) << "exit " << uint32_t(step) << " never reached the leader";
    }

    EXPECT_STREQ("", Move::StepRefusal(Move::StepStart::Started));
    EXPECT_FALSE(Move::LeaderDecidesAgain(Move::StepStart::Started));
}

// Each exit says WHICH one it was, in its own words: [sfk243] (a dungeon path WalkBotPoints refused) left no line
// anywhere at all and [nav24] left two an hour, so one shared "no move" would keep them indistinguishable in the
// journal.
TEST(MoveStep, EachRefusalHasItsOwnWords)
{
    std::set<std::string> words;
    for (Move::StepStart const step : NO_STEP)
        words.insert(Move::StepRefusal(step));

    EXPECT_EQ(size_t(3), words.size()) << "two exits share one refusal: the journal cannot tell them apart";
}

// The classifier of "a refusal that refuses no X" keeps its answers: it moved here from LeaderActions.cpp so that
// one place decides whether the leader walks the plain path, and the test can hold it to it.
TEST(MoveStep, TransientRefusalKnowsTheWalkThatIsAlreadyOn)
{
    EXPECT_TRUE(Move::TransientRefusal(""));
    EXPECT_TRUE(Move::TransientRefusal("IsDuplicateMove (the same walk is on)"));
    EXPECT_TRUE(Move::TransientRefusal("IsWaitingForLastMove (the last walk is not done)"));
    EXPECT_TRUE(Move::TransientRefusal("IsMovingAllowed (a hold, root, cast or dead state)"));
    EXPECT_TRUE(Move::TransientRefusal("the walk ends where the bot stands"));

    EXPECT_FALSE(Move::TransientRefusal("ClampToFightZone (the leader's safe zone)"));
    EXPECT_FALSE(Move::TransientRefusal("JudgeWaterDrop (hold)"));
}
