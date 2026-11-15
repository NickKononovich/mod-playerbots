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
    EXPECT_FALSE(HealerPlace::CanExistOnTargetRings(LiveBand(), Aestymros()))
        << "the row blames the search for finding none of 107 candidates good, while no point of any ring it offers "
           "is within the tank's 30 yd of a target 51.4443 yd away";
    EXPECT_NEAR(31.4443f, HealerPlace::MinGapToTank(LiveBand(), Aestymros()), 0.001f);
    EXPECT_NEAR(1.4443f, HealerPlace::ShortByOnTargetRings(LiveBand(), Aestymros()), 0.001f)
        << "how far the geometry was from mattering at all";
}

TEST(HealerPlace, ThePlaceExistsAgainstTheBandTheCheckAlreadyAccepts)
{
    HealerPlace::Band wide = LiveBand();
    wide.ringOuter = wide.acceptOuter;  // the generator offering what CombatSpotValidWhy already lets through
    EXPECT_TRUE(HealerPlace::CanExistOnTargetRings(wide, Aestymros()))
        << "the same geometry, judged against one band instead of two, has a place";
    EXPECT_NEAR(0.0f, HealerPlace::ShortByOnTargetRings(wide, Aestymros()), 0.001f);
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
    EXPECT_TRUE(HealerPlace::CanExistOnTargetRings(LiveBand(), tank));
    EXPECT_NEAR(0.0f, HealerPlace::ShortByOnTargetRings(LiveBand(), tank), 0.001f);
    EXPECT_NEAR(14.0f, HealerPlace::MinGapToTank(LiveBand(), tank), 0.001f)
        << "the band has a hole: standing on the target, the nearest place the generator offers is the inner ring, "
           "and that is 14 yd of the tank's 30 - comfortably inside it";
}

TEST(HealerPlace, APlaceStillExistsAtTheFarEdgeOfWhatTheGeometryAllows)
{
    HealerPlace::Tank tank;
    tank.toTargetYd = 50.0f;  // ringOuter 20 + keeps 30: the last distance at which a place can exist
    tank.keepsYd = 30.0f;
    EXPECT_TRUE(HealerPlace::CanExistOnTargetRings(LiveBand(), tank));
    tank.toTargetYd = 50.01f;
    EXPECT_FALSE(HealerPlace::CanExistOnTargetRings(LiveBand(), tank))
        << "the boundary is the sum of the two radii and nothing else";
}

TEST(HealerPlace, AGeometryNoWideningReachesIsNotBlamedOnTheTwoNumbers)
{
    HealerPlace::Tank tank;
    tank.toTargetYd = 70.0f;
    tank.keepsYd = 30.0f;
    HealerPlace::Band wide = LiveBand();
    wide.ringOuter = wide.acceptOuter;
    EXPECT_FALSE(HealerPlace::CanExistOnTargetRings(LiveBand(), tank));
    EXPECT_FALSE(HealerPlace::CanExistOnTargetRings(wide, tank))
        << "at 70 yd the band's second number is not the cause, and the record must not say it is";
}

TEST(HealerPlace, TheTankInsideTheRingsHoleIsNotTreatedAsBeyondThem)
{
    HealerPlace::Tank tank;
    tank.toTargetYd = 2.0f;  // the tank hugging the target: the hole of the annulus, not past its outer edge
    tank.keepsYd = 30.0f;
    EXPECT_NEAR(12.0f, HealerPlace::MinGapToTank(LiveBand(), tank), 0.001f)
        << "the nearest ring point is ringInner - d away, and signing that wrong would read as 'beyond the band'";
    EXPECT_TRUE(HealerPlace::CanExistOnTargetRings(LiveBand(), tank));
}

// ---------------------------------------------------------------------------------------------------------------
// [spot91b] USER 2026-11-15: «Место для хила ищется кольцами вокруг моба. Я не понимаю, а почему место для хила
// ищется не возле танка, а возле моба? Какая-то херня. Это нужно пофиксить.»
//
// ONE CENTRE, AND IT IS THE TANK, because the acceptance already measures from the tank: a candidate generated on a
// ring round the tank's fight place is inside `keepsYd` of it BY CONSTRUCTION, so the tank half of the acceptance can
// no longer be the thing that empties the set before a candidate is looked at. The band to the target stays a check.
//
// MEASURED ON THE CLOSED b504 CORPUS, and it is why the rings round the mob are the defect: in ALL 20 healer
// `combat_spot_none` events of b504 every one of the 480 ring candidates round the target (16 directions x 3 rings)
// was refused - 480 of 480, in both the 10 events where the healer could not even cast from where it stood and the
// 10 where it could. Of 2 329 candidates over those events 1 283 (55.1 %) were refused "heal reach", the test that
// `keeps` owns, and 820 of the 980 `near` candidates (83.7 %) - the stage centred on the HEALER - went that way.
// No stage of the search was centred on the tank, and the acceptance is the tank's.
//
// THE PRIOR RULE THESE ARE RED AGAINST is the generator as it stood: rings round the target only, so the boundary is
// ringOuter + keepsYd (50) and no ring round the tank is walked at all.
//
// THE OPPOSITE RULE MUST ALSO FAIL. "Round the tank a place always exists" would pass the cases above and fail the
// two below it: a target beyond the two radii together has no place whatever the centre, and a tank swallowed by the
// band's own hole has none either.

TEST(HealerPlace, TheOneCentreIsTheTankSoTheGeometryAestymroHadNoPlaceForHasOne)
{
    EXPECT_TRUE(HealerPlace::CanExist(LiveBand(), Aestymros()))
        << "with the candidates generated round the tank, 51.4443 yd between the two centres leaves a place: the "
           "tank's own 30 yd reach the band's 30 yd round the target with 8.5556 yd to spare";
    EXPECT_NEAR(0.0f, HealerPlace::ShortBy(LiveBand(), Aestymros()), 0.001f);
    EXPECT_FALSE(HealerPlace::CanExistOnTargetRings(LiveBand(), Aestymros()))
        << "and the record must still be able to say that the generator it had could not have found one";
}

TEST(HealerPlace, TheBoundaryIsTheTanksOwnReachPlusTheBandAndNothingElse)
{
    EXPECT_NEAR(60.0f, HealerPlace::MaxTankToTargetYd(LiveBand(), Aestymros()), 0.001f)
        << "LEADER_HEALER_TANK_YD (30) plus the band the check already accepts (30) - no number that is not already "
           "in the code";
    HealerPlace::Tank tank = Aestymros();
    tank.toTargetYd = 60.0f;
    EXPECT_TRUE(HealerPlace::CanExist(LiveBand(), tank));
    tank.toTargetYd = 60.01f;
    EXPECT_FALSE(HealerPlace::CanExist(LiveBand(), tank));
}

TEST(HealerPlace, OnlyTheRingsRoundTheTankThatCanReachTheBandAreWalked)
{
    HealerPlace::Band const band = LiveBand();
    EXPECT_TRUE(HealerPlace::RingReachesBand(band, 51.4443f, 30.0f))
        << "at 51.4443 yd between the centres the tank's outermost ring comes 21.4443 yd from the target, inside the "
           "band - this is the ring that holds Aestymro's missing place";
    EXPECT_FALSE(HealerPlace::RingReachesBand(band, 51.4443f, 18.0f))
        << "a ring 18 yd round the tank stays 33.4443 yd from the target, outside the band's 30: walking it would be "
           "candidates that cannot pass the check that generated them";
    EXPECT_TRUE(HealerPlace::RingReachesBand(band, 0.0f, 30.0f))
        << "the tank in melee of the target: the ring at 30 is the band's own outer edge";
    EXPECT_FALSE(HealerPlace::RingReachesBand(band, 0.0f, 6.0f))
        << "and the ring at 6 is inside the band's hole (14), where the healer may not stand";
}

// the opposite side: a rule that let a place exist round the tank unconditionally would pass every case above
TEST(HealerPlace, ATargetBeyondBothRadiiTogetherHasNoPlaceFromEitherCentre)
{
    HealerPlace::Tank tank = Aestymros();
    tank.toTargetYd = 63.97f;  // pb_journal_actor.tank_yd, Aestymro on Druid of the Fang 3840/108, b504
    EXPECT_FALSE(HealerPlace::CanExist(LiveBand(), tank));
    EXPECT_NEAR(3.97f, HealerPlace::ShortBy(LiveBand(), tank), 0.001f);
    tank.toTargetYd = 70.0f;
    EXPECT_FALSE(HealerPlace::CanExist(LiveBand(), tank));
    EXPECT_FALSE(HealerPlace::CanExistOnTargetRings(LiveBand(), tank))
        << "re-centring is not a universal answer, and the record must not say it is";
}

TEST(HealerPlace, ATankSwallowedByTheBandsOwnHoleHasNoPlaceEither)
{
    HealerPlace::Tank tank;
    tank.toTargetYd = 2.0f;
    tank.keepsYd = 5.0f;  // a reach so short that every point it allows is inside the band's hole
    EXPECT_FALSE(HealerPlace::CanExist(LiveBand(), tank))
        << "7 yd is the farthest from the target the tank's reach allows, and the band starts at 14: checking only "
           "the outer bound would call this a place";
}
