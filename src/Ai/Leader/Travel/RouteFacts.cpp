/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteFacts.h"

#include <cmath>
#include <utility>

namespace Leader::Travel::RouteFacts
{
    float Detour(float pathYd, float straightYd)
    {
        // an unmeasured length and a zero-length straight line both make the ratio meaningless, and they come out as
        // "no answer" rather than as 1.0 (no detour) or 0.0 (no distance)
        if (!std::isfinite(pathYd) || !std::isfinite(straightYd) || pathYd < 0.0f || straightYd <= 0.0f)
            return -1.0f;
        return pathYd / straightYd;
    }

    char const* StopKind(bool patrol, bool boss)
    {
        // a boss is named first: a patrolling boss is still the stop the tour ends at
        if (boss)
            return "boss";
        return patrol ? "patrol" : "pack";
    }

    char const* LegVerdict(bool arrived, bool skipped)
    {
        if (arrived)
            return "arrived";
        return skipped ? "refused" : "abandoned";
    }

    PlanFact BuildPlan(std::vector<StopFact> planned, std::vector<StopFact> rejected,
                       std::vector<SpotFact> const& spots, float pathYd)
    {
        PlanFact out;
        out.stopCount = uint32_t(planned.size());
        out.pathYd = std::isfinite(pathYd) ? pathYd : -1.0f;

        if (spots.size() >= 2)
        {
            float straight = 0.0f;
            for (size_t i = 1; i < spots.size(); ++i)
            {
                float const dx = spots[i].x - spots[i - 1].x;
                float const dy = spots[i].y - spots[i - 1].y;
                float const dz = spots[i].z - spots[i - 1].z;
                straight += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            out.straightYd = straight;
        }
        // fewer than two points: the tour has no straight line to measure. It stays at -1 and is not called 0.

        uint32_t seq = 0;
        out.stops.reserve(planned.size() + rejected.size());
        for (StopFact& stop : planned)
        {
            stop.seq = seq++;
            if (stop.verdict.empty())
                stop.verdict = "planned";
            out.stops.push_back(std::move(stop));
        }
        // the dropped candidates continue the same series: seq is part of the stop table's key, so a candidate may
        // not reuse a planned stop's number
        for (StopFact& stop : rejected)
        {
            stop.seq = seq++;
            if (stop.verdict.empty())
                stop.verdict = "refused";
            out.stops.push_back(std::move(stop));
        }
        return out;
    }
}  // namespace Leader::Travel::RouteFacts
