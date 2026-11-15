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
