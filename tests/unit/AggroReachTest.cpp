/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [death470] THE GROUND ONE MOB'S AGGRO COVERS, over REAL rows of b503.
//
// The defect, in the numbers of the recorded window (see AggroReach.h for the full count): of the 31 mobs
// `[death462]` named as "reaches over the spot left out" of a judged engage, 26 were left out because they were in a
// fight of their own (18) or evading home (8) - states the judge's surroundings drop outright. Of 4 920 priced
// fights 118 had more mobs come than were judged, and that 2.4 % holds ALL FIVE lost fights.
//
// BOTH DIRECTIONS ARE ASSERTED, because each one alone passes with a wrong rule:
// - a busy mob's home ground must still be covered (the old rule never looked at it at all);
// - the yards it is being HELD in must not be covered (a rule that simply widened the disc by "how far it has gone",
//   which is the rule free wanderers use, walls off 160 yd of Duskwood).

#include <gtest/gtest.h>

#include "AggroReach.h"

namespace
{
    using AggroReach::Disc;
    using AggroReach::Mob;

    // pb_journal_line [deaths14] at 1794752313244 / [death462] of the same engage, b503, map 0 (Wetlands):
    //   "Orton L26 engages Dragonmaw Raider L23: judged 2 mobs ... fought at its reach (-3380,-2228) ...
    //    [death462] reaches over the spot left out: Dragonmaw Raider L24 35 yd (in a fight (victim Selalleas))"
    // The Raider is a wanderer (wander_distance 5 in `creature`), its home 20 yd from that fight spot, and it was in
    // Selalleas' fight 35 yd away on the other side. Orton's fight ran, the Raider came, Orton died.
    // detection_range 20 at one level above the bot -> GetAggroRange 21; Orton's combat reach 1.5.
    Mob RaiderInSelalleasFight()
    {
        Mob mob;
        mob.homeX = -3399.0f;  // 20 yd from the fight spot at (-3380,-2228)
        mob.homeY = -2232.0f;
        mob.x = -3353.0f;  // where Selalleas was holding it: 35 yd from the fight spot, 47 yd from its home
        mob.y = -2214.0f;
        mob.aggroYd = 21.0f;
        mob.botReachYd = 1.5f;
        mob.wanderYd = 5.0f;
        mob.wanders = true;
        mob.busy = true;  // IsInCombat: the state that dropped it from the count
        return mob;
    }

    // the same spawn once Selalleas is dead and it has walked home: free, where it belongs
    Mob RaiderBackHome()
    {
        Mob mob = RaiderInSelalleasFight();
        mob.busy = false;
        mob.x = mob.homeX;
        mob.y = mob.homeY;
        return mob;
    }
}  // namespace

// THE DEFECT ITSELF: 26 of the 31 named left-outs are this shape. A mob held in a fight of its own still owns the
// ground round its home, and the fight spot 20 yd from that home is on it.
TEST(AggroReachSeam, AMobBusyElsewhereStillCoversTheGroundRoundItsHome)
{
    Disc const disc = AggroReach::Of(RaiderInSelalleasFight());
    EXPECT_TRUE(AggroReach::Covers(disc, -3380.0f, -2228.0f))
        << "the fight spot 20 yd from the Raider's home is not on its ground while it is in Selalleas' fight: "
           "Orton judged 2 mobs at (-3380,-2228), the Raider came, Orton died (b503, 1794752313244)";
}

// THE OTHER DIRECTION: the yards it is being held in are not its ground. Widening the disc by "how far it has gone"
// (47 yd here) is the rule for a FREE wanderer and it is wrong for a held one - in Duskwood the same widening put
// Mor'Ladim L30's refusal 103 to 162 yd from the walker and walled the graveyard off entirely.
TEST(AggroReachSeam, TheYardsAMobIsHeldInAreNotItsGround)
{
    Disc const disc = AggroReach::Of(RaiderInSelalleasFight());
    EXPECT_FALSE(AggroReach::Covers(disc, -3353.0f, -2214.0f))
        << "the spot where another bot is holding the Raider counts as its own aggro ground: a disc widened by the "
           "47 yd it was dragged walls off ground it is walking away from";
}

// ... and nothing of the free wanderer's own answer changes: back home and free, its wander area plus its aggro is
// its ground, exactly as before.
TEST(AggroReachSeam, AFreeWandererKeepsItsWanderAreaAndAggro)
{
    Disc const disc = AggroReach::Of(RaiderBackHome());
    EXPECT_FLOAT_EQ(disc.radius, 21.0f + 1.5f + 5.0f);
    EXPECT_TRUE(AggroReach::Covers(disc, -3380.0f, -2228.0f));
    EXPECT_FALSE(AggroReach::Covers(disc, -3353.0f, -2214.0f));
}

// a free wanderer that has walked out of its own wander area keeps covering the ground round where it stands: this
// is the [surv10] widening and it must survive the repair (b484 Kylantino L22 met a Boulderfist Ogre L33 at 5 yd and
// the home disc read "outside it")
TEST(AggroReachSeam, AFreeWandererOutsideItsAreaStillCoversWhereItStands)
{
    Mob mob = RaiderBackHome();
    mob.x = mob.homeX + 40.0f;
    Disc const disc = AggroReach::Of(mob);
    EXPECT_TRUE(AggroReach::Covers(disc, mob.x, mob.y));
}

// a mob that does not wander and is not busy: the circle round where it stands, no home term at all
TEST(AggroReachSeam, AStandingMobIsJudgedWhereItStands)
{
    Mob mob;
    mob.homeX = 100.0f;
    mob.x = 0.0f;
    mob.aggroYd = 20.0f;
    mob.botReachYd = 1.5f;
    Disc const disc = AggroReach::Of(mob);
    EXPECT_FLOAT_EQ(disc.x, 0.0f);
    EXPECT_FLOAT_EQ(disc.radius, 21.5f);
}

// ---------------------------------------------------------------------------------------------------------------
// [death471] A REFUSAL LIVES EXACTLY AS LONG AS THE GROUND IT WAS MADE OF.
//
// The route search's wall list is read back long after the search that wrote it by things that ACT on it: ConWalls
// hands a walled quest to a party, NearestLostFightWall writes the problem row, RouteWalls names the wall in a death
// line. b503 Duskwood: Mor'Ladim L30 is a WAYPOINT patrol (creature 5086, map 0, wander_distance 0,
// detection_range 20, rank 1) and its wall was read back at 103, 107, 115, 117, 128, 132, 160 and 162 yd from the
// walker - "[wall17] ... q133 walled by Mor'Ladim L30 (elite): asks for help", "[death433] its only walk there
// passes a reach it refused (... Mor'Ladim L30 128 yd ...)". Baridrol's death row carries route_age_ms 24 826: the
// ground the refusal was computed over was 25 s old and the patrol had walked on.
//
// The replacement for that age is NOT a shorter age (rule 2) - it is the ground itself. The wall keeps the very disc
// that went into the search's refusal set, and a reader asks THIS function of it, so "the ground this mob's aggro
// covers" still has exactly one judge (rule 5).

namespace
{
    // the disc Mor'Ladim's refusal was computed over: a patrol, so it sits where the patrol stood
    Disc MorLadimRefusedAt()
    {
        Mob mob;
        mob.homeX = mob.x = -10374.0f;  // "[surv10] ... its aggro disc (-10374,364) r 26"
        mob.homeY = mob.y = 364.0f;
        mob.aggroYd = 24.0f;    // "[solo] Mor'Ladim L30 (patrol, home 23 yd off, wander 0, aggro 24)"
        mob.botReachYd = 1.5f;
        return AggroReach::Of(mob);
    }
}  // namespace

TEST(RefusalFact, APatrolThatWalkedOffItsDiscIsNoLongerTheWallItWasRefusedAs)
{
    Disc const made = MorLadimRefusedAt();
    EXPECT_FALSE(AggroReach::Covers(made, -10374.0f + 104.0f, 364.0f))
        << "Mor'Ladim's refusal still describes ground 104 yd from the disc it was computed over: [wall17] handed "
           "q133 to a party over a patrol that had walked away (b503, route_age_ms 24 826)";
}

TEST(RefusalFact, AMobStillOnTheGroundItWasRefusedOnKeepsItsWall)
{
    Disc const made = MorLadimRefusedAt();
    EXPECT_TRUE(AggroReach::Covers(made, -10374.0f + 20.0f, 364.0f))
        << "a mob that has not left the ground its refusal was made of dropped its wall: the refusal would flicker "
           "on every step it takes inside the reach it was refused for";
}

// ... and a free wanderer's wall never flickers, because its disc is its home's and it is inside it by
// construction: Dragonmaw Raider (entry 1034) roams creature.wander_distance 5
TEST(RefusalFact, AWandererInsideItsOwnAreaKeepsItsWall)
{
    Mob mob = RaiderBackHome();
    Disc const made = AggroReach::Of(mob);
    mob.x = mob.homeX + 5.0f;  // it crossed its whole area
    EXPECT_TRUE(AggroReach::Covers(made, mob.x, mob.y));
}
