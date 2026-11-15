/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [jrn20] The route row's instrument, tested on the numbers a real plan produced. pb_journal_route and
// pb_journal_route_stop were empty, so the first thing to prove is not that the server writes them - that needs a
// run - but that what WOULD be written says the right thing about a plan whose numbers are already known:
//
//   b181 rfc-h measured the same tour two ways: 1 886 yd walked on the navmesh by the planned order against
//   2 327-2 803 yd by the greedy nearest pick. A single number cannot show that, which is why the row carries the
//   walk and the straight line as a pair and the detour is their ratio.
//
// The three facts asserted here are the three that were got wrong before: the ratio, the three-way leg verdict, and
// "absent is not zero" for a plan the navmesh refused a walk for.

#include "RouteFacts.h"

#include <gtest/gtest.h>

namespace
{
    using namespace Leader::Travel::RouteFacts;

    StopFact Stop(char const* what, uint32_t entry, uint32_t low, float x, float y, char const* kind)
    {
        StopFact stop;
        stop.what = what;
        stop.entry = entry;
        stop.low = low;
        stop.x = x;
        stop.y = y;
        stop.z = 0.0f;
        stop.kind = kind;
        return stop;
    }
}  // namespace

TEST(RouteFacts, ADetourIsTheRatioAndNotEitherLength)
{
    // the measured tour: 1 886 yd of walking over a 900 yd straight tour is a detour of 2.1
    EXPECT_NEAR(Detour(1886.0f, 900.0f), 2.0956f, 0.001f);
    // and the greedy order's 2 803 yd over the same straight tour is a worse one, which is the whole comparison
    EXPECT_GT(Detour(2803.0f, 900.0f), Detour(1886.0f, 900.0f));
}

TEST(RouteFacts, AnUnmeasuredLengthIsNotADetourOfZero)
{
    // the navmesh found no walk for the tour: the planner's own marker is a negative length, and the ratio must
    // refuse to answer rather than say "no detour" (1.0) or "no distance" (0.0)
    EXPECT_LT(Detour(-1.0f, 900.0f), 0.0f);
    // a straight line of zero: the tour has one point, so there is nothing to divide by
    EXPECT_LT(Detour(1886.0f, 0.0f), 0.0f);
}

TEST(RouteFacts, ALegEndsInOneOfThreeWaysAndTheyAreNotTheSame)
{
    EXPECT_STREQ(LegVerdict(true, false), "arrived");
    EXPECT_STREQ(LegVerdict(false, true), "refused");   // the route put it aside, with a reason of its own
    EXPECT_STREQ(LegVerdict(false, false), "abandoned");  // dropped for another target: no refusal was recorded
    // a target that was reached is "arrived" even though it was skipped afterwards: the leg's own outcome is the
    // arrival, and the skip belongs to the next leg's reason
    EXPECT_STREQ(LegVerdict(true, true), "arrived");
}

TEST(RouteFacts, AStopKeepsItsKindAndABossIsNamedFirst)
{
    EXPECT_STREQ(StopKind(false, false), "pack");
    EXPECT_STREQ(StopKind(true, false), "patrol");
    EXPECT_STREQ(StopKind(true, true), "boss");
}

TEST(RouteFacts, TheRowCarriesTheRejectedCandidatesAsStopsOfTheirOwn)
{
    std::vector<StopFact> planned{Stop("Ragefire Shaman +2", 11319, 157, 0.0f, 0.0f, "pack"),
                                  Stop("Earthborer", 11320, 221, 30.0f, 40.0f, "pack"),
                                  Stop("Taragaman the Hungerer", 11319, 310, 30.0f, 140.0f, "boss")};
    StopFact refused = Stop("Searing Blade Enforcer +1", 11324, 404, -50.0f, 0.0f, "pack");
    refused.why = "behind the group (101 yd from the cleared walk, 130 yd from the walk ahead)";

    // the plan's walk: the leader's place first, then each stop's spot
    std::vector<SpotFact> const spots{{0.0f, -50.0f, 0.0f},
                                      {0.0f, 0.0f, 0.0f},
                                      {30.0f, 40.0f, 0.0f},
                                      {30.0f, 140.0f, 0.0f}};

    PlanFact const plan = BuildPlan(planned, {refused}, spots, 310.0f);

    // the dropped candidate is in the row, with its reason: a candidate that returned zero is a row, not a
    // disappearance
    ASSERT_EQ(plan.stops.size(), 4u);
    EXPECT_EQ(plan.stopCount, 3u) << "a dropped candidate is not a stop of the tour";
    EXPECT_EQ(plan.stops[3].verdict, "refused");
    EXPECT_FALSE(plan.stops[3].why.empty()) << "a refusal without its reason is the gap this table was made for";
    EXPECT_EQ(plan.stops[0].verdict, "planned");
    // the identity travels as fields, never as the name
    EXPECT_EQ(plan.stops[1].entry, 11320u);
    EXPECT_EQ(plan.stops[1].low, 221u);

    // no two stops of one route may share a seq: it is part of the stop table's primary key
    for (size_t i = 0; i < plan.stops.size(); ++i)
        EXPECT_EQ(plan.stops[i].seq, uint32_t(i));

    // 50 + 50 + 100 = 200 yd of straight tour against 310 yd walked: a detour of 1.55
    EXPECT_NEAR(plan.straightYd, 200.0f, 0.01f);
    EXPECT_NEAR(Detour(plan.pathYd, plan.straightYd), 1.55f, 0.01f);
}

TEST(RouteFacts, APlanWithNoWalkKeepsItsLengthNegative)
{
    PlanFact const plan = BuildPlan({}, {}, {}, -1.0f);
    EXPECT_LT(plan.pathYd, 0.0f) << "the navmesh refused a walk: that is not a walk of 0 yd";
    EXPECT_LT(plan.straightYd, 0.0f) << "one point is no tour: that is not a tour of 0 yd";
    EXPECT_EQ(plan.stopCount, 0u);
}
