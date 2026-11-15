/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "StandingEpisode.h"

#include <cmath>

namespace
{
    // the core's getMSTimeDiff, character for character: the seam must not import the core to ask the time
    uint32_t Diff(uint32_t oldMs, uint32_t newMs)
    {
        return oldMs > newMs ? (0xFFFFFFFF - oldMs) + newMs : newMs - oldMs;
    }

    float Dist(Journal::Standing::Anchor const& anchor, Journal::Standing::Sample const& sample)
    {
        float const dx = anchor.x - sample.x;
        float const dy = anchor.y - sample.y;
        float const dz = anchor.z - sample.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    void Restart(Journal::Standing::Anchor& anchor, Journal::Standing::Sample const& sample)
    {
        anchor.open = true;
        anchor.sinceMs = sample.nowMs;
        anchor.reportedMs = sample.nowMs;
        anchor.mapId = sample.mapId;
        anchor.x = sample.x;
        anchor.y = sample.y;
        anchor.z = sample.z;
        anchor.maxYd = 0.f;
    }
}  // namespace

namespace Journal::Standing
{
    Look Observe(Anchor& anchor, Sample const& sample)
    {
        Look look;
        if (!anchor.open)
        {
            Restart(anchor, sample);
            return look;
        }
        // THE ANCHOR IS A POINT ON A MAP. A sample on another map is not a distance away from it, so the episode
        // cannot be measured and is not reported: the bot was teleported, which is one of the holds this detector
        // already refuses to call standing, and the stretch before the jump has been reported once per cadence
        // while it was open. Without this the two live rows "stood for 68 s within 3355.5 yd" and "19.8 s within
        // 4451.1 yd" were the straight line from a position on map 1 to a position inside Wailing Caverns.
        if (anchor.mapId != sample.mapId)
        {
            Restart(anchor, sample);
            return look;
        }
        float const movedYd = Dist(anchor, sample);
        uint32_t const stoodMs = Diff(anchor.sinceMs, sample.nowMs);
        if (sample.held || movedYd > sample.radius)
        {
            look.ended = true;
            look.report = stoodMs >= sample.cadenceMs;
            look.stoodSec = stoodMs / 1000.0f;
            // the radius it STAYED INSIDE, never the step that took it out: `movedYd` here is by definition
            // outside the radius, and the column means the opposite of that
            look.stoodYd = anchor.maxYd;
            Restart(anchor, sample);
            return look;
        }
        if (movedYd > anchor.maxYd)
            anchor.maxYd = movedYd;
        if (Diff(anchor.reportedMs, sample.nowMs) >= sample.cadenceMs)
        {
            anchor.reportedMs = sample.nowMs;
            look.report = true;
            look.stoodSec = stoodMs / 1000.0f;
            look.stoodYd = anchor.maxYd;  // the same quantity in the open row as in the ending one
        }
        return look;
    }
}
