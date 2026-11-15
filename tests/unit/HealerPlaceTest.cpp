/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [spot91] CAN A HEALER'S FIGHT PLACE EXIST AT ALL. The inputs are the real ones of
// pb_journal_issue 7708442440541159566 (b504): healer Aestymro L24, target Deviate Ravager 3636/177, 107 candidates
// refused (59 "heal reach", 46 "out of the healer's band"), 0 reached - and the tank Bierya 51.4443 yd from that very
// target when the pull was taken (pb_journal_actor.tank_yd of the `look target` row of decision 1794761871466, a
// column and not a sentence).
//
// THE PRIOR RULE THIS WAS RED AGAINST is the project's own written belief - "combat_spot_none is a search bug; a
// ranged spot always exists" - put into the same API: CanExist always true, MinGapToTank 0, ShortBy 0, OneBand true.
//
// THE OPPOSITE RULE MUST ALSO FAIL IT. "Forbid every healer place" would pass any test that only asserted the
// impossible case, so the cases where a place MUST exist stand next to it.

#include <gtest/gtest.h>

#include "../../src/Ai/Leader/Fight/HealerPlace.h"

namespace
{
    using namespace Leader::Fight;

    // the band as the code writes it: rings LEADER_HEALER_YD .. LEADER_HEALER_YD - 6 round the target
    // (LeaderCombatSpot), gaps LEADER_HEALER_YD - 6 .. LEADER_HEALER_YD + 10 accepted (CombatSpotValidWhy)
    HealerPlace::Band LiveBand()
    {
        HealerPlace::Band band;
        band.ringInner = 14.0f;    // LEADER_HEALER_YD - 6
        band.ringOuter = 20.0f;    // LEADER_HEALER_YD: the generator never offers further out
        band.acceptInner = 14.0f;  // LEADER_HEALER_YD - 6
        band.acceptOuter = 30.0f;  // LEADER_HEALER_YD + 10
        return band;
    }

    HealerPlace::Tank Aestymros()
    {
        HealerPlace::Tank tank;
        tank.toTargetYd = 51.4443f;  // pb_journal_actor.tank_yd, decision 1794761871466, note 'look target'
        tank.keepsYd = 30.0f;        // LEADER_HEALER_TANK_YD
        return tank;
    }
}  // namespace

TEST(HealerPlace, TheSetTheSearchWasAskedToPickFromWasEmptyByArithmetic)
{
    EXPECT_FALSE(HealerPlace::CanExist(LiveBand(), Aestymros()))
        << "the row blames the search for finding none of 107 candidates good, while no point of any ring it offers "
           "is within the tank's 30 yd of a target 51.4443 yd away";
    EXPECT_NEAR(31.4443f, HealerPlace::MinGapToTank(LiveBand(), Aestymros()), 0.001f);
    EXPECT_NEAR(1.4443f, HealerPlace::ShortBy(LiveBand(), Aestymros()), 0.001f)
        << "how far the geometry was from mattering at all";
}

TEST(HealerPlace, ThePlaceExistsAgainstTheBandTheCheckAlreadyAccepts)
{
    HealerPlace::Band wide = LiveBand();
    wide.ringOuter = wide.acceptOuter;  // the generator offering what CombatSpotValidWhy already lets through
    EXPECT_TRUE(HealerPlace::CanExist(wide, Aestymros()))
        << "the same geometry, judged against one band instead of two, has a place";
    EXPECT_NEAR(0.0f, HealerPlace::ShortBy(wide, Aestymros()), 0.001f);
}

TEST(HealerPlace, TheHealersBandIsWrittenAsTwoNumbersInTwoPlaces)
{
    EXPECT_FALSE(HealerPlace::OneBand(LiveBand()))
        << "the generator stops at 20 yd while the validity check accepts out to 30: one quantity, two judges";
    HealerPlace::Band one = LiveBand();
    one.ringOuter = one.acceptOuter;
    EXPECT_TRUE(HealerPlace::OneBand(one));
}

// the opposite side: a rule that forbade every healer place would pass the cases above and fail these
TEST(HealerPlace, APlaceExistsWhenTheTankFightsOnTheTarget)
{
    HealerPlace::Tank tank;
    tank.toTargetYd = 0.0f;  // the tank in melee, the usual case of every pull that worked
    tank.keepsYd = 30.0f;
    EXPECT_TRUE(HealerPlace::CanExist(LiveBand(), tank));
    EXPECT_NEAR(0.0f, HealerPlace::ShortBy(LiveBand(), tank), 0.001f);
    EXPECT_NEAR(14.0f, HealerPlace::MinGapToTank(LiveBand(), tank), 0.001f)
        << "the band has a hole: standing on the target, the nearest place the generator offers is the inner ring, "
           "and that is 14 yd of the tank's 30 - comfortably inside it";
}

TEST(HealerPlace, APlaceStillExistsAtTheFarEdgeOfWhatTheGeometryAllows)
{
    HealerPlace::Tank tank;
    tank.toTargetYd = 50.0f;  // ringOuter 20 + keeps 30: the last distance at which a place can exist
    tank.keepsYd = 30.0f;
    EXPECT_TRUE(HealerPlace::CanExist(LiveBand(), tank));
    tank.toTargetYd = 50.01f;
    EXPECT_FALSE(HealerPlace::CanExist(LiveBand(), tank))
        << "the boundary is the sum of the two radii and nothing else";
}

TEST(HealerPlace, AGeometryNoWideningReachesIsNotBlamedOnTheTwoNumbers)
{
    HealerPlace::Tank tank;
    tank.toTargetYd = 70.0f;
    tank.keepsYd = 30.0f;
    HealerPlace::Band wide = LiveBand();
    wide.ringOuter = wide.acceptOuter;
    EXPECT_FALSE(HealerPlace::CanExist(LiveBand(), tank));
    EXPECT_FALSE(HealerPlace::CanExist(wide, tank))
        << "at 70 yd the band's second number is not the cause, and the record must not say it is";
}

TEST(HealerPlace, TheTankInsideTheRingsHoleIsNotTreatedAsBeyondThem)
{
    HealerPlace::Tank tank;
    tank.toTargetYd = 2.0f;  // the tank hugging the target: the hole of the annulus, not past its outer edge
    tank.keepsYd = 30.0f;
    EXPECT_NEAR(12.0f, HealerPlace::MinGapToTank(LiveBand(), tank), 0.001f)
        << "the nearest ring point is ringInner - d away, and signing that wrong would read as 'beyond the band'";
    EXPECT_TRUE(HealerPlace::CanExist(LiveBand(), tank));
}
