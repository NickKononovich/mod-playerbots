/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [dpszero3] THE THIRD READER OF THE ZONE RULE: a bot that STANDS inside the aggro disc of a fight it loses.
//
// The inputs are real rows of the closed b504 corpus, named at each assertion. The redness was proved by
// EXECUTION twice, in both directions, by swapping the seam's body for a rule and running the suite:
//   - the rule as it was before ("only the one mob the bot's own last verdict refused counts, so the ground never
//     asks") - in seam terms `Answer::Stays` always, which is exactly what b504 recorded: 0 `[reach66]` lines in
//     the whole hour. Three assertions fail.
//   - the rule "no bot stands in any reach at all" - `lostReaches` ignored. The INVERSE assertions fail, and they
//     are why they are here: 318 of the 371 unjudged fights of b504 came out WON on the bot's own projection, so a
//     rule that emptied every disc would move bots off most of the world's ground for no measured reason.
// Without both sides, "forbid everything" passes green.

#include <gtest/gtest.h>

#include "StandGround.h"

using StandGround::Answer;
using StandGround::Candidate;
using StandGround::Stand;

namespace
{
    Answer AnswerOf(Stand const& stand, std::vector<Candidate> const& spots, uint32_t& index)
    {
        return StandGround::Of(stand, spots, index);
    }
}

// b504 Rollothan L20, Shadowhide camp in Redridge (zone 44): its own last route search read the five mobs that then
// hit it as "Shadowhide Brute refused, Shadowhide Gnoll priced, Rabid Shadowhide Gnoll priced, Shadowhide Gnoll
// refused, Shadowhide Gnoll refused" - the judge had called three of those fights lost. It stood on its quest spot
// at hp 94 % and died 42 s later: 803 damage over 12.7 s against 556 max health.
TEST(StandGroundRule, AStandAReachWhoseFightIsLostCoversIsLeft)
{
    Stand stand;
    stand.reachesHere = 5;
    stand.lostReaches = 3;
    stand.clearSpot = true;
    uint32_t index = 0;
    EXPECT_NE(AnswerOf(stand, {}, index), Answer::Stays)
        << "the judge had already refused 3 of the 5 reaches over this ground and the bot went on standing in it: "
           "Rollothan L20 died there 42 s after that reading (b504, 803 dmg over 12.7 s at 556 max health)";
    EXPECT_TRUE(StandGround::Leaves(stand));
}

// THE INVERSE, and the whole reason the rule counts LOST reaches instead of reaches. b504: of the 371 fights that
// opened with no judge, the bot was inside the first mob's own aggro disc in 311 and its own projection came out
// WON in 318 of the 371 (85.7 %), LOST in 2. Standing in the disc of a mob it beats is not a defect.
TEST(StandGroundRule, AStandCoveredOnlyByFightsItWinsIsNotLeft)
{
    Stand stand;
    stand.reachesHere = 2;
    stand.lostReaches = 0;
    uint32_t index = 0;
    EXPECT_EQ(AnswerOf(stand, {{1}, {0}}, index), Answer::Stays)
        << "two reaches cover the ground and the judge takes both fights: 318 of the 371 unjudged fights of b504 "
           "were won on the bot's own projection, so emptying every disc would move bots off ground that costs "
           "them nothing";
}

// the other inverse: no reach at all over the ground is not the same value as "every reach over it is winnable"
// (rule 7, absence is not zero), and neither is a reason to move
TEST(StandGroundRule, GroundNoReachCoversIsNotLeftEither)
{
    Stand stand;
    stand.reachesHere = 0;
    stand.lostReaches = 0;
    uint32_t index = 0;
    EXPECT_EQ(AnswerOf(stand, {}, index), Answer::Stays);
}

// b504 Baridrol L27 at the Duskwood crypt: 5 reaches cover where it stands (Skeletal Warder L29 x3, Warder L28,
// Skeletal Healer L26), 9 points out of every reach exist but not one of their walks is taken, and the least
// covered ray point carries 4. That is the order this seam fixes: a clear spot would come first, and with none
// walkable the ground covered by fewer reaches than the stand is next.
TEST(StandGroundRule, TheClearGroundComesBeforeTheLessCoveredGround)
{
    Stand stand;
    stand.reachesHere = 5;
    stand.lostReaches = 1;
    stand.clearSpot = true;
    uint32_t index = 0;
    EXPECT_EQ(AnswerOf(stand, {{4}, {3}}, index), Answer::ClearSpot);
}

TEST(StandGroundRule, WithNoWalkableClearSpotTheLeastCoveredGroundIsTaken)
{
    Stand stand;
    stand.reachesHere = 5;  // b504 Baridrol L27: "covered by 4 reach(es) against 5 where it stands"
    stand.lostReaches = 1;
    stand.clearSpot = false;
    uint32_t index = 99;
    EXPECT_EQ(AnswerOf(stand, {{4}, {3}}, index), Answer::LessCovered);
    EXPECT_EQ(index, 0u) << "the list arrives fewest reaches first and nearest among equals (CoveredGround); the "
                            "reader takes its front, it does not sort again";
}

// b504 Baridrol L27 again, the same hour: `[deaths140]` recorded "14 spot(s) out of every reach within its sight,
// none with a clean walk". Ground covered by as many reaches as the stand is NOT an improvement, and taking it
// would be a move that buys nothing - the measured difference is the whole rule.
TEST(StandGroundRule, GroundNoBetterCoveredThanTheStandIsNotTakenForIt)
{
    Stand stand;
    stand.reachesHere = 2;
    stand.lostReaches = 2;
    stand.clearSpot = false;
    uint32_t index = 0;
    EXPECT_EQ(AnswerOf(stand, {{2}, {3}}, index), Answer::Nowhere)
        << "a point covered by 2 of the same 2 reaches is the same ground by the only measure there is";
}

// b503 Duskwood, the case `[death472]` was built on and the one this reader inherits: Eraelor L26 stood where no
// direction left the union of the reaches at all, so the ray list came back EMPTY. Sitting (or standing) where you
// are because nothing was better is how that bot died twice to one Mor'Ladim L30 patrol. Nowhere to go is a row.
TEST(StandGroundRule, NothingAtAllToMoveToIsTheRecordedCaseAndNotAReasonToStay)
{
    Stand stand;
    stand.reachesHere = 1;
    stand.lostReaches = 1;
    stand.clearSpot = false;
    uint32_t index = 0;
    EXPECT_EQ(AnswerOf(stand, {}, index), Answer::Nowhere);
    EXPECT_TRUE(StandGround::Leaves(stand))
        << "the stand is still left as far as the rule is concerned - what is missing is somewhere to leave to, and "
           "that difference is what the problem row carries";
}

// no clock anywhere: the same inputs answer the same whatever has elapsed, because there is nothing to elapse. The
// struct has no duration, no age and no timestamp, so no assertion here can be satisfied by waiting (rule 2).
TEST(StandGroundRule, TheRuleIsACountAndHasNoClockToRead)
{
    Stand stand;
    stand.reachesHere = 3;
    stand.lostReaches = 1;
    stand.clearSpot = false;
    std::vector<Candidate> const spots{{2}};
    uint32_t first = 0;
    uint32_t second = 0;
    EXPECT_EQ(AnswerOf(stand, spots, first), AnswerOf(stand, spots, second));
    EXPECT_EQ(first, second);
}
