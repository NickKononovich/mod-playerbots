/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_ROUTEFACTS_H
#define PLAYERBOTS_LEADER_ROUTEFACTS_H

// [jrn20] What a route row SAYS, as a seam: the facts go in as plain numbers, the row's shape comes out, and nothing
// here reads the world, the clock or the database. The emitters in RouteTargets.cpp gather the plan from the map as
// they always did and call these to shape the row, so no bot decision changes and the shaping is testable with no
// server (tests/unit/RouteFactsTest.cpp).
//
// Why this is a seam and not three expressions inline. Two of the three answers here are exactly the kind that was
// got wrong before:
//   * THE DETOUR IS A RATIO. Neither the walk nor the straight line shows a detour on its own (1 886 yd means
//     nothing until the straight line beside it says 900), so the two lengths are written as a pair and the ratio is
//     computed in one place instead of by every reader of the table.
//   * ABSENT IS NOT ZERO. A plan whose walk the navmesh refused has NO length; writing 0 for it would say "no
//     walking at all", which is a different and false statement. Unmeasured comes out negative here and the column
//     keeps it negative, so a query can tell "no walk" from "no distance".
//   * A LEG'S VERDICT IS ONE OF THREE and they are not interchangeable: reached, refused (the route put the target
//     aside and said why), or dropped for another target. One boolean cannot carry that, and a wrong word here
//     answers a query wrongly for ever.

#include <cstdint>
#include <string>
#include <vector>

namespace Leader::Travel::RouteFacts
{
    // One stop of a route row. A REJECTED candidate is a stop too, with its own verdict and reason: a stop that was
    // weighed and dropped is the thing the table exists to show, and it used to vanish without a trace.
    struct StopFact
    {
        uint32_t seq = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        std::string kind;     // "pack" | "patrol" | "boss"
        std::string what;     // the name at that stop, for the eyes
        uint32_t entry = 0;   // the identity: entry ...
        uint32_t low = 0;     // ... and the GUID counter. Never the name.
        std::string verdict;  // "planned" | "meets" | "refused"
        std::string why;
    };

    // A point of the plan's walk, in the order the plan visits it (the leader's own place first).
    struct SpotFact
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // The lengths and the stop list of one "planned" / "changed" row.
    struct PlanFact
    {
        std::vector<StopFact> stops;  // the planned stops first, then the candidates that were dropped
        uint32_t stopCount = 0;       // the PLANNED ones only: a dropped candidate is no stop of the tour
        float pathYd = -1.0f;         // along the navmesh, as the planner measured it (< 0: it found no walk)
        float straightYd = -1.0f;     // the same tour in straight lines (< 0: fewer than two points, so unmeasured)
    };

    // < 0 when the ratio cannot be formed at all (either length unmeasured, or a straight line of zero). NEVER 0 for
    // "unknown": 0 would read as "no detour", which is the opposite answer.
    float Detour(float pathYd, float straightYd);

    char const* StopKind(bool patrol, bool boss);

    // `arrived`: the group actually reached the target (a fact of state, not a time). `skipped`: the route put it
    // aside with a reason. Anything else is a target the route gave up on for another one.
    char const* LegVerdict(bool arrived, bool skipped);

    // The row's shape out of the plan: the stops renumbered in one series (the dropped candidates after the planned
    // ones, so no two rows of one route can collide on `seq`), the planned count, and the tour measured both ways.
    PlanFact BuildPlan(std::vector<StopFact> planned, std::vector<StopFact> rejected,
                       std::vector<SpotFact> const& spots, float pathYd);
}  // namespace Leader::Travel::RouteFacts

#endif
