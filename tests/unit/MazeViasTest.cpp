/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [maze21] The ways round a refused walk, on the inputs of b504.
//
// THE INPUTS ARE REAL. The give-up is `pb_journal_route` id 7708442440541176511 (event 'leg_end', verdict
// 'abandoned', bot Idunk, map 0, zone 40 Westfall): the bot stood at (-11085.3, 1457.55, 44.65) heading for
// (-11327.6, 1539.12, 28.88), path 769.7 yd against 256.1 straight, and the row says `candidates = 0` -
// "there was nowhere to go round". The eight spots below are every row of `playerbots_travelnode` within 400 yd
// of that stand, with their own names, read out of the live database. The world was not empty there; the SOURCE
// was, and the row turned that into a statement about the world.
//
// Measured over the whole closed b504 corpus: 76 rows of the maze search (the ones that carry a non-NULL
// `candidates`), ALL of them `candidates = 0`, `tries_skipped = 0`, and ZERO child rows of kind 'via'; yet 75 of
// those 76 stands have 1 to 8 such spots inside 400 yd (max 8, mean 3.72), and 69 of them inside 240 yd.

#include "MazeVias.h"

#include <gtest/gtest.h>

namespace
{
    using Leader::Travel::Maze::Spot;

    constexpr float REACH = 400.0f;      // the search's own reach at that look (2 x 2 x SightDistance)
    constexpr float SAME_SPOT = 5.0f;    // NewRpgBaseAction::MOVE_FAR_PROGRESS, the caller's existing number

    Spot IdunkStand() { return {-11085.3f, 1457.55f, 44.65f}; }
    Spot IdunkGoal() { return {-11327.6f, 1539.12f, 28.88f}; }

    // every playerbots_travelnode row of map 0 within 400 yd of Idunk's stand, nearest way through first
    std::vector<Spot> SpotsRoundIdunk()
    {
        // deliberately NOT in the order the seam must produce: the source lists what it holds, the seam ranks it.
        // The yards are the straight 3-D ones (the walk itself is measured by the route search, not here)
        return {
            {-11339.9f, 1572.45f, 94.39f},   // Deadmines exit, 283.7 yd off, 358.3 through
            {-11052.1f, 1495.50f, 44.36f},   // Westfall Moonbrook, 50.4 off, 329.8 through
            {-11257.5f, 1502.36f, 38.01f},   // Westfall The Dagger Hills, 178.1 off, 257.7 through
            {-11208.7f, 1675.90f, 24.57f},   // Deadmines exit, 251.6 off, 432.9 through
            {-11208.5f, 1685.34f, 21.76f},   // Deadmines entrance, 260.0 off, 448.7 through
            {-11219.3f, 1705.41f, 38.97f},   // The Dagger Hills spirithealer, 281.8 off, 480.5 through
            {-11076.1f, 1784.68f, 38.49f},   // Westfall Demonts Place, 327.3 off, 679.0 through
            {-10820.9f, 1328.40f, 34.28f}};  // Westfall Stendels Pond, 294.4 off, 843.2 through
    }
}

// The lie the instrument told, and the only assertion that catches it: an answer nobody looked for must not come
// back as a measured zero. The old step 2 asked a source that is never loaded and wrote its silence down as 0.
TEST(MazeVias, ASourceThatNeverAnsweredIsNotAMeasuredZero)
{
    auto const vias = Leader::Travel::Maze::Of(false, {}, IdunkStand(), IdunkGoal(), REACH, SAME_SPOT);
    EXPECT_FALSE(vias.measured) << "the stock node store is never loaded in this build, and its silence was "
                                   "written down as `candidates = 0` on 76 of 76 maze rows of b504 - while 75 of "
                                   "those stands have walkable places within 400 yd";
    EXPECT_TRUE(vias.order.empty());
}

// The switch itself: handed the places that are really there, the search has ways round where it had none.
TEST(MazeVias, TheWaysRoundIdunksGiveUpAreNotNone)
{
    auto const vias =
        Leader::Travel::Maze::Of(true, SpotsRoundIdunk(), IdunkStand(), IdunkGoal(), REACH, SAME_SPOT);
    ASSERT_TRUE(vias.measured);
    EXPECT_EQ(vias.candidates, 8u) << "pb_journal_route 7708442440541176511 recorded 0 ways round at this stand";
    ASSERT_EQ(vias.order.size(), 8u);
}

// The inverse, without which "hand back everything" would pass green: a source that answered and found nothing
// must report nothing, and it must be a MEASURED nothing.
TEST(MazeVias, AStandWithNoPlaceRoundItReportsAMeasuredNone)
{
    // pb_journal_route 7708442440541321052, Nurlian, map 1 zone 406 at (1690.4, 0, 127.33): the one stand of the
    // 76 that really has no such place within 400 yd
    auto const vias = Leader::Travel::Maze::Of(true, {}, {1690.4f, 0.0f, 127.33f}, {1608.37f, 184.17f, 104.73f},
                                               REACH, SAME_SPOT);
    EXPECT_TRUE(vias.measured) << "a look that happened is a measurement even when it found nothing";
    EXPECT_EQ(vias.candidates, 0u);
}

// The order is the answer's own, and the bound cuts into THE SHORTEST WAYS THROUGH, not into whatever the source
// listed first. Without this the 8 tries would be spent on the eight spots the source happened to emit first.
TEST(MazeVias, TheShortestWayThroughComesFirst)
{
    auto const vias =
        Leader::Travel::Maze::Of(true, SpotsRoundIdunk(), IdunkStand(), IdunkGoal(), REACH, SAME_SPOT);
    ASSERT_EQ(vias.order.size(), 8u);
    for (size_t i = 1; i < vias.order.size(); ++i)
        EXPECT_LE(vias.order[i - 1].yards, vias.order[i].yards) << "way " << i << " is shorter than way " << i - 1;
    EXPECT_NEAR(vias.order.front().yards, 257.74f, 0.1f);  // Westfall The Dagger Hills, third in the source's list
    EXPECT_NEAR(vias.order.back().yards, 843.23f, 0.1f);   // Westfall Stendels Pond
}

// The reach and "where it already stands" are the caller's existing numbers, and both still cut: a place out of
// reach is not a candidate, and the spot under the bot's feet is not a way round itself.
TEST(MazeVias, OutOfReachAndUnderTheFeetAreNoWaysRound)
{
    std::vector<Spot> spots = SpotsRoundIdunk();
    spots.push_back(IdunkStand());                           // the bot's own stand
    spots.push_back({-11085.3f + 3.0f, 1457.55f, 44.65f});   // 3 yd off it: the same spot
    spots.push_back({-9000.0f, 1457.55f, 44.65f});           // 2085 yd off: out of reach
    auto const vias = Leader::Travel::Maze::Of(true, spots, IdunkStand(), IdunkGoal(), REACH, SAME_SPOT);
    ASSERT_TRUE(vias.measured);
    EXPECT_EQ(vias.candidates, 8u) << "the stand itself, a spot 3 yd from it and one 2085 yd away were counted";
}
