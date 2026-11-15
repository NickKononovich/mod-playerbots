/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [lastway1] A LOST FIGHT WITH NO CLEAN WAY OUT STILL LEAVES - on the seven live "survival none" lines of the
// closed b504 corpus, every one of which carries BOTH "no way out clear of other mobs" AND "0 castable", and
// three of which belong to a bot that died there (Baridrol, Kianey, Kylantino).
//
// The inverse assertions are not decoration: without them a rule of "always run" passes green, and that rule
// would make the bot flee fights it wins and throw away a Vanish that ends the fight outright.

#include <gtest/gtest.h>

#include "../../src/Ai/World/Rpg/LastWay.h"

namespace
{
    using LastWay::Answer;
    using LastWay::Exits;

    // the ground as `SoloSafety::StandsOnLostGround` reports it
    StandGround::Stand Ground(uint32_t reachesHere, uint32_t lostReaches, bool clearSpot = false)
    {
        StandGround::Stand stand;
        stand.reachesHere = reachesHere;
        stand.lostReaches = lostReaches;
        stand.clearSpot = clearSpot;
        return stand;
    }

    std::vector<StandGround::Candidate> Rays(std::vector<uint32_t> const& counts)
    {
        std::vector<StandGround::Candidate> spots;
        for (uint32_t c : counts)
            spots.push_back({c});
        return spots;
    }

    Answer AnswerOf(Exits const& exits, StandGround::Stand const& stand,
                    std::vector<StandGround::Candidate> const& spots, uint32_t& index)
    {
        return LastWay::Of(exits, stand, spots, index);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// THE DEFECT. Red under the rule b504 actually ran: no clean way out -> "survival none" -> the bot stands still.
// ---------------------------------------------------------------------------------------------------------------

TEST(LastWay, ALostFightWithNoCleanWayOutStillLeaves)
{
    // Kianey L26, b504: "survival none: no reset, no piece due (0 castable), no run: no way out clear of other
    // mobs, spot 1 back skipped: Redridge Mystic L19 (idle, assists a chaser) on it; 4 attackers 65.3 dps, 29 s
    // at its 55.1 dps: 769 dmg to come at hp 686". 769 against 686 is 1.12x of the body: one stretch of distance
    // decides it, and standing still was the one choice with no upside.
    Exits exits;
    exits.fightLost = true;
    exits.resetDue = false;
    exits.pieceDue = false;
    exits.clearWay = false;

    uint32_t index = 99;
    // two reaches cover the stand; a ray leaves one of them behind
    Answer const answer = AnswerOf(exits, Ground(2, 2), Rays({2, 1, 3}), index);

    EXPECT_EQ(answer, Answer::LeastCovered)
        << "no way out was clear of other mobs and the bot went on standing in a fight it loses: Kianey L26 took "
           "769 dmg into a 686 hp body (b504), and three of the seven deaths of that hour carry this very line";
    EXPECT_TRUE(LastWay::Leaves(answer)) << "the answer has to move the bot, not only name a spot";
    EXPECT_EQ(index, 1u) << "the direction is the ray point FEWER reaches cover, as StandGround orders them";
}

TEST(LastWay, TheWayOutIsTheLeastCoveredGroundAndNotTheFirstRay)
{
    // Baridrol L27, b504: "15 attackers 134.7 dps ... 9275 dmg to come at hp 734". Hopeless by the price, which
    // is exactly why the direction must still be the best one available and not whichever ray came first.
    Exits exits;
    exits.fightLost = true;

    uint32_t index = 0;
    EXPECT_EQ(AnswerOf(exits, Ground(4, 3), Rays({4, 4, 1, 2}), index), Answer::LeastCovered);
    EXPECT_EQ(index, 2u) << "the ray with one reach over it is the way out, not the two that match the stand";
}

TEST(LastWay, GroundOutOfEveryReachIsTheLeastCoveredThereIs)
{
    Exits exits;
    exits.fightLost = true;

    uint32_t index = 0;
    EXPECT_EQ(AnswerOf(exits, Ground(3, 1, /*clearSpot*/ true), Rays({3, 3}), index), Answer::LeastCovered)
        << "a spot out of every reach is a way out even when no ray beats the stand";
}

// ---------------------------------------------------------------------------------------------------------------
// THE INVERSES. Each of these is red under "always run", and that is the only thing that stops this seam from
// becoming a rule that empties the world of bots.
// ---------------------------------------------------------------------------------------------------------------

TEST(LastWay, AFightItIsNotLosingIsNotRunFrom)
{
    // 318 of the 371 unjudged fights of b504 (85.7 %) came out WON on the bot's own projection. A door that
    // opened on anything less than the judge's refusal would run from all of them.
    Exits exits;
    exits.fightLost = false;
    exits.clearWay = false;

    uint32_t index = 0;
    Answer const answer = AnswerOf(exits, Ground(3, 0), Rays({1, 0}), index);
    EXPECT_EQ(answer, Answer::Fights)
        << "the fight is not lost: 318 of 371 unjudged fights of b504 were won on the bot's own projection";
    EXPECT_FALSE(LastWay::Leaves(answer));
}

TEST(LastWay, AResetThatEndsTheFightBeatsAnyRun)
{
    // b504 took 4 resets (Ginde, Baridrol, Kianey each Vanish). A reset ends the fight outright and the bot rests
    // to full after it: a run with no clean way out is strictly worse, and spending the one on the other is a loss.
    Exits exits;
    exits.fightLost = true;
    exits.resetDue = true;
    exits.clearWay = false;

    uint32_t index = 0;
    EXPECT_EQ(AnswerOf(exits, Ground(2, 2), Rays({0}), index), Answer::ResetFirst)
        << "Vanish ends the fight; running through other mobs instead throws it away";
}

TEST(LastWay, APricedKitPieceIsTriedBeforeARunWithNoCleanWay)
{
    // [guild80] prices every piece against the shortfall. b504: 21 heals, 3 damage-taken buffs taken.
    Exits exits;
    exits.fightLost = true;
    exits.pieceDue = true;
    exits.clearWay = false;

    uint32_t index = 0;
    EXPECT_EQ(AnswerOf(exits, Ground(2, 2), Rays({0}), index), Answer::PieceFirst);
}

TEST(LastWay, APricedCleanWayIsStillTheWayItTakes)
{
    // 8 flees were taken in b504, each priced over every chaser's leash, speed, daze and casts. This seam must
    // not replace them: a clean way has a survival price, the last resort has none.
    Exits exits;
    exits.fightLost = true;
    exits.clearWay = true;

    uint32_t index = 0;
    Answer const answer = AnswerOf(exits, Ground(4, 4), Rays({0}), index);
    EXPECT_EQ(answer, Answer::ClearWay)
        << "the three priced searches own the decision whenever one of them found a clean way";
    EXPECT_TRUE(LastWay::Leaves(answer));
}

TEST(LastWay, NowhereToGoIsSaidAndNotInvented)
{
    // `stand_no_ground` / `rest_no_ground` have ZERO rows in the whole issue table: both detectors shipped after
    // b504 closed, so "nowhere to go" is NOT MEASURED, not "does not happen". The seam must therefore be able to
    // say it, and must not fabricate a direction to avoid saying it (rule 7).
    Exits exits;
    exits.fightLost = true;
    exits.clearWay = false;

    uint32_t index = 7;
    Answer const answer = AnswerOf(exits, Ground(2, 2), Rays({2, 3, 2}), index);
    EXPECT_EQ(answer, Answer::Nowhere)
        << "no ray beats the stand and there is no clear spot: that is a recorded problem, not a direction";
    EXPECT_FALSE(LastWay::Leaves(answer));
    EXPECT_EQ(index, 0u) << "no direction was named, so none may be left behind in the out-parameter";
}

TEST(LastWay, NoRayAtAllIsNowhereAndNotAWayOut)
{
    // the caller that asks before the ground has been looked at must not be handed a run
    Exits exits;
    exits.fightLost = true;

    uint32_t index = 0;
    EXPECT_EQ(AnswerOf(exits, Ground(1, 1), {}, index), Answer::Nowhere);
}

TEST(LastWay, TheOrderIsStrictAndTheBetterOptionAlwaysWins)
{
    // every option at once: the answer is the best of them, and the order never depends on the ground
    Exits exits;
    exits.fightLost = true;
    exits.resetDue = true;
    exits.pieceDue = true;
    exits.clearWay = true;

    uint32_t index = 0;
    EXPECT_EQ(AnswerOf(exits, Ground(5, 5), Rays({0}), index), Answer::ResetFirst);

    exits.resetDue = false;
    EXPECT_EQ(AnswerOf(exits, Ground(5, 5), Rays({0}), index), Answer::PieceFirst);

    exits.pieceDue = false;
    EXPECT_EQ(AnswerOf(exits, Ground(5, 5), Rays({0}), index), Answer::ClearWay);

    exits.clearWay = false;
    EXPECT_EQ(AnswerOf(exits, Ground(5, 5), Rays({0}), index), Answer::LeastCovered);
}

TEST(LastWay, NothingHereReadsAClock)
{
    // rule 2: there is no duration, no age and no timestamp in `Exits` or in `StandGround::Stand`, so no assertion
    // in this file can be satisfied by waiting. The check is that the same states give the same answer, always.
    Exits exits;
    exits.fightLost = true;

    uint32_t first = 0;
    uint32_t second = 0;
    Answer const a = AnswerOf(exits, Ground(3, 2), Rays({3, 1}), first);
    Answer const b = AnswerOf(exits, Ground(3, 2), Rays({3, 1}), second);
    EXPECT_EQ(a, b);
    EXPECT_EQ(first, second);
}
