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
    bool EndsTheEpisode(Holds const& holds)
    {
        // Only the holds that mean the bot is not standing at all. The three labelling ones are deliberately
        // absent: see the comment on Holds in the header.
        return holds.dead || holds.sitting || holds.casting || holds.inFlight || holds.beingTeleported;
    }

    char const* HoldName(Holds const& holds)
    {
        // One name per sample, in a fixed order, so that the key is stable for one state of the world. The order
        // is "what the bot is" before "what it was told": a dead bot under its leader's Hold is dead.
        if (holds.dead)
            return "dead";
        if (holds.sitting)
            return "sitting";
        if (holds.casting)
            return "casting";
        if (holds.inFlight)
            return "in flight";
        if (holds.beingTeleported)
            return "being teleported";
        if (holds.orderedToStand)
            return "its leader ordered it to stand";
        if (holds.waitsForTransport)
            return "waits for a transport to dock";
        if (holds.waitsForGroup)
            return "waits for its group";
        return "";
    }

    std::string TrailTail(std::string const& trail, std::size_t room)
    {
        if (room == 0)
            return std::string();
        if (trail.size() <= room)
            return trail;
        std::string tail = trail.substr(trail.size() - room);
        // Start the tail at an action boundary when one is inside it, so the row never begins mid-name. If the
        // whole tail is one entry there is no boundary to find and the raw tail is kept: losing the newest
        // characters to tidiness would be the very defect this function exists to end.
        std::size_t const bar = tail.find('|');
        if (bar != std::string::npos && bar + 1 < tail.size())
            tail = tail.substr(bar);
        return tail;
    }

    Look Observe(Anchor& anchor, Sample const& sample)
    {
        Look look;
        // [jrn42] the hold travels with every row this call writes: the row that says "it stood for 120 s under
        // its leader's order" and the row that says "nothing held it still" are the same row, and the one field
        // below is the whole difference between a problem and a bot doing as it was told
        look.heldBy = HoldName(sample.holds);
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
        if (EndsTheEpisode(sample.holds) || movedYd > sample.radius)
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
