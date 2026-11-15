/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [jrn41] THE STANDING WATCH'S ANCHOR. The sequence below is a real one from the live base of b501: bot Lyleria
// stood at (-441.867, -3199.73, 201.09) on map 1 and was reported six times, 10 s to 60.199 s, each row with
// stood_yd = 0; then at 68.434 s it was teleported into Wailing Caverns and the seventh row said "stood for 68 s
// within 3355.5 yd" at map 43, position (-163.49, 132.9, -73.66). The distance is the straight line between a
// point on map 1 and a point on map 43, which is not a distance at all, and it was written into a column that
// means "the radius it stayed inside".

#include <gtest/gtest.h>

#include "StandingEpisode.h"

namespace
{
    Journal::Standing::Sample Outdoor(uint32_t nowMs)
    {
        Journal::Standing::Sample sample;
        sample.nowMs = nowMs;
        sample.mapId = 1;
        sample.x = -441.867f;
        sample.y = -3199.73f;
        sample.z = 201.09f;
        sample.radius = 1.5f;
        sample.cadenceMs = 10000;
        return sample;
    }

    Journal::Standing::Sample InsideWailingCaverns(uint32_t nowMs)
    {
        Journal::Standing::Sample sample = Outdoor(nowMs);
        sample.mapId = 43;
        sample.x = -163.49f;
        sample.y = 132.9f;
        sample.z = -73.66f;
        return sample;
    }

    // [jrn42] b502, bot Ganna L26: twelve samples at (-37.33, 2475.54, -4.16) on map 369, world coordinates
    // identical to the hundredth from t=1794735814271 to t=1794735924290 - 120 s - and then, fifteen seconds
    // after the last of them, "[tram28] Ganna L26 boards tram car 176084 at (-42,2473) on map 369". It stood on
    // the platform until the tram came: one wait of about one tram cycle (the module's own [ride133] note puts
    // one way at 63 s), reported as twelve problems with nothing in the row to say so.
    Journal::Standing::Sample OnTheTramPlatform(uint32_t nowMs)
    {
        Journal::Standing::Sample sample;
        sample.nowMs = nowMs;
        sample.mapId = 369;
        sample.x = -37.33f;
        sample.y = 2475.54f;
        sample.z = -4.16f;
        sample.radius = 1.5f;
        sample.cadenceMs = 10000;
        sample.holds.waitsForTransport = true;
        return sample;
    }
}  // namespace

// A jump between maps is not standing: there is no radius to stay inside across a teleport, and the stretch before
// it has already been reported once per cadence while it was open.
TEST(StandingEpisode, ATeleportToAnotherMapIsNotAStandingEpisode)
{
    Journal::Standing::Anchor anchor;
    Journal::Standing::Observe(anchor, Outdoor(0));  // the episode opens
    for (uint32_t nowMs : {10000u, 20069u, 30069u, 40199u, 50199u, 60199u})
        Journal::Standing::Observe(anchor, Outdoor(nowMs));

    Journal::Standing::Look const look = Journal::Standing::Observe(anchor, InsideWailingCaverns(68434));
    EXPECT_FALSE(look.report) << "the teleport into map 43 was reported as a standing episode of " << look.stoodSec
                              << " s within " << look.stoodYd << " yd";
    EXPECT_EQ(anchor.mapId, 43u) << "the anchor kept a position on the map the bot has left";
}

// The ending row's radius is the one the bot stayed inside, never the step that took it out of it. Same map here,
// so the distance is a real one - it is still not a containment radius.
TEST(StandingEpisode, TheRadiusIsTheOneItStayedInsideNotTheStepOut)
{
    Journal::Standing::Anchor anchor;
    Journal::Standing::Observe(anchor, Outdoor(0));
    Journal::Standing::Observe(anchor, Outdoor(10000));

    Journal::Standing::Sample walkedOff = Outdoor(20000);
    walkedOff.x += 50.f;  // one step out of the radius ends the episode
    Journal::Standing::Look const look = Journal::Standing::Observe(anchor, walkedOff);
    EXPECT_TRUE(look.report);
    EXPECT_TRUE(look.ended);
    EXPECT_LE(look.stoodYd, 1.5f) << "the episode ended with a radius of " << look.stoodYd
                                  << " yd, which is the step that broke it and not the radius it held";
    EXPECT_NEAR(look.stoodSec, 20.f, 0.01f);
}

// The cadence is the instrument's one floor and it must stay one row per window: this is the contract the issue
// row's growing `occurrences` rests on.
TEST(StandingEpisode, AnOpenEpisodeIsReportedOncePerWindow)
{
    Journal::Standing::Anchor anchor;
    Journal::Standing::Observe(anchor, Outdoor(0));
    EXPECT_FALSE(Journal::Standing::Observe(anchor, Outdoor(5000)).report);
    EXPECT_TRUE(Journal::Standing::Observe(anchor, Outdoor(10000)).report);
    EXPECT_FALSE(Journal::Standing::Observe(anchor, Outdoor(15000)).report);
    Journal::Standing::Look const second = Journal::Standing::Observe(anchor, Outdoor(20000));
    EXPECT_TRUE(second.report);
    EXPECT_FALSE(second.ended);
    EXPECT_NEAR(second.stoodSec, 20.f, 0.01f);
    EXPECT_NEAR(second.stoodYd, 0.f, 0.01f);
}

// [jrn42] A LEGITIMATE WAIT IS STILL ONE WAIT, AND THE ROW SAYS WHICH WAIT. The hold travels on every row this
// episode writes, so the duration stays true (120 s, not twelve tens) and the reader never has to guess whether
// "nothing legitimately held it still" was measured or merely unexamined.
TEST(StandingEpisode, AWaitForATransportIsNamedOnEveryRowOfTheEpisode)
{
    Journal::Standing::Anchor anchor;
    Journal::Standing::Observe(anchor, OnTheTramPlatform(1794735814271u));  // the episode opens
    Journal::Standing::Look last;
    for (uint32_t step = 1; step <= 11; ++step)
    {
        Journal::Standing::Look const look =
            Journal::Standing::Observe(anchor, OnTheTramPlatform(1794735814271u + step * 10000u));
        ASSERT_TRUE(look.report) << "sample " << step << " of one wait wrote no row";
        EXPECT_STREQ(look.heldBy, "waits for a transport to dock")
            << "the row claims nothing held it still while the bot was on the platform waiting for the tram";
        last = look;
    }
    EXPECT_NEAR(last.stoodSec, 110.f, 0.01f) << "the wait was cut into pieces instead of being named";
}

// The hold NAMES the episode, it does not end it: the three waits are the bot holding one position on purpose,
// and ending the episode on them would write one row per sample - the cadence talking, not the bot.
TEST(StandingEpisode, AWaitDoesNotEndTheEpisodeWhileASitDownDoes)
{
    Journal::Standing::Holds waits;
    waits.waitsForTransport = true;
    EXPECT_FALSE(Journal::Standing::EndsTheEpisode(waits));
    waits.orderedToStand = true;
    waits.waitsForGroup = true;
    EXPECT_FALSE(Journal::Standing::EndsTheEpisode(waits));

    Journal::Standing::Holds sits;
    sits.sitting = true;
    EXPECT_TRUE(Journal::Standing::EndsTheEpisode(sits));
    Journal::Standing::Holds dead;
    dead.dead = true;
    EXPECT_TRUE(Journal::Standing::EndsTheEpisode(dead));
    Journal::Standing::Holds casting;
    casting.casting = true;
    EXPECT_TRUE(Journal::Standing::EndsTheEpisode(casting));
    Journal::Standing::Holds flying;
    flying.inFlight = true;
    EXPECT_TRUE(Journal::Standing::EndsTheEpisode(flying));
    Journal::Standing::Holds teleported;
    teleported.beingTeleported = true;
    EXPECT_TRUE(Journal::Standing::EndsTheEpisode(teleported));
}

// "" means "checked, nothing held it", exactly as it does in read_by: it is a finding, not a blank.
TEST(StandingHoldName, NothingHoldsItWhenNothingDoes)
{
    EXPECT_STREQ(Journal::Standing::HoldName(Journal::Standing::Holds{}), "");
    Journal::Standing::Holds ordered;
    ordered.orderedToStand = true;
    EXPECT_STREQ(Journal::Standing::HoldName(ordered), "its leader ordered it to stand");
    // what the bot IS comes before what it was TOLD: a dead bot under its leader's Hold is dead
    ordered.dead = true;
    EXPECT_STREQ(Journal::Standing::HoldName(ordered), "dead");
}

// [jrn42] THE TRAIL'S NEWEST END IS THE ANSWER THE COLUMN PROMISES. Engine::GetLastAction keeps the newest 512
// characters, the row calls it "its engine's last action", and the two columns that carry it were keeping the
// OLDEST characters: measured on b502, `doing` (96) held the oldest 96 in 837 of 1032 rows and `why` (512) had
// been filled to the brim in 170 of 1029, dropping the newest ~110 characters of the trail off its end.
TEST(StandingTrailTail, TheColumnKeepsTheNewestEndOfTheTrail)
{
    // a real b502 trail of a 134 s episode (bot Laaral, map 369), whose newest entry is the one that matters
    std::string const trail =
        "|PUSH:new rpg status update - 11.000000 (default)|A:new rpg status update - PREREQ|A:new rpg status "
        "update - FAILED|A:add gathering loot - PREREQ|A:add gathering loot - FAILED|A:drink - USELESS|A:food - "
        "USELESS|A:attack anything - USELESS|A:new rpg go camp - PREREQ|A:new rpg go camp - OK";
    std::string const kept = Journal::Standing::TrailTail(trail, 96);
    EXPECT_LE(kept.size(), 96u);
    EXPECT_NE(kept.find("new rpg go camp - OK"), std::string::npos)
        << "the column named \"its engine's last action\" kept " << kept << ", which is not its last action";
    EXPECT_EQ(kept.front(), '|') << "the row begins in the middle of an action's name: " << kept;
    // a trail that fits is kept whole, and no room keeps nothing
    EXPECT_EQ(Journal::Standing::TrailTail(trail, trail.size()), trail);
    EXPECT_EQ(Journal::Standing::TrailTail(trail, 0u), "");
}
