/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MountEpisode.h"

#include <cmath>

namespace
{
    // the core's getMSTimeDiff, character for character: the seam must not import the core to ask the time
    uint32_t Diff(uint32_t oldMs, uint32_t newMs)
    {
        return oldMs > newMs ? (0xFFFFFFFF - oldMs) + newMs : newMs - oldMs;
    }

    float Dist(Journal::MountState::Episode const& episode, Journal::MountState::Sample const& sample)
    {
        float const dx = episode.x - sample.x;
        float const dy = episode.y - sample.y;
        float const dz = episode.z - sample.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // A new episode: a new place, or a new map. Everything counted so far belonged to the old place, so the
    // counters go with it - otherwise a bot that flapped once in town and then walked across a continent would
    // carry that flip into every later report, and the row's value would stop being about one episode.
    void Restart(Journal::MountState::Episode& episode, Journal::MountState::Sample const& sample)
    {
        episode.open = true;
        episode.sinceMs = sample.nowMs;
        episode.reportedMs = sample.nowMs;
        episode.mapId = sample.mapId;
        episode.x = sample.x;
        episode.y = sample.y;
        episode.z = sample.z;
        episode.maxYd = 0.f;
        episode.mounts = 0;
        episode.dismounts = 0;
        episode.flips = 0;
        episode.lastOn.clear();
        episode.lastOff.clear();
        episode.flipUnder.clear();
        // `known` and `mounted` deliberately survive: the mount state is a property of the bot and not of the
        // place, so a bot that walks out of its radius while mounted has not changed state. Forgetting it here
        // would invent a change at the first sample of every new place.
    }
}  // namespace

namespace Journal::MountState
{
    std::string Justification(Facts const& facts)
    {
        // A fixed order, so one state of the world always gives one string: the key depends on it, and a set that
        // reorders itself would fan one cause into a staircase of rows (this project's trap #2).
        std::string out;
        auto add = [&out](char const* name)
        {
            if (!out.empty())
                out += '+';
            out += name;
        };
        if (facts.inCombat)
            add("in combat");
        if (facts.targetInReach)
            add("target in reach");
        if (facts.indoors)
            add("indoors");
        if (facts.inWater)
            add("in water");
        if (facts.onTransport)
            add("on a transport");
        if (facts.moving)
            add("walking");
        if (facts.hasWalk)
            add("a walk is on");
        if (facts.hasLeader)
            add(facts.leaderRides ? "its leader rides" : "its leader is on foot");
        // The walk's target belongs to the justification and not to the place: two mounts for two different
        // walks are two decisions. It is appended rather than named so that "a walk is on" stays readable.
        if (facts.hasWalk)
            out += " to (" + std::to_string(facts.walkToX) + "," + std::to_string(facts.walkToY) + ")";
        // "nothing" is not an empty value here: a change that no fact justifies is the WORST case, because it
        // cannot possibly have a second one that differs, and it is exactly Emdyl's case in Stormwind.
        return out.empty() ? "nothing" : out;
    }

    bool IsReal(Episode const& episode)
    {
        // One flip is the whole proof: a second change in the same direction under an identical justification,
        // inside one radius on one map, means nothing that justified the first had changed. See the header for
        // the derivation; the number is 1 because the argument gives 1, not because 1 was picked.
        return episode.flips >= 1;
    }

    Look Observe(Episode& episode, Sample const& sample)
    {
        Look look;

        if (!episode.open || episode.mapId != sample.mapId)
        {
            // A map change is not a distance. The standing watch was caught reporting a teleport into Wailing
            // Caverns as an episode "within 3355.5 yd" because its anchor carried no map; the anchor here carries
            // one for that reason, and the lesson is repeated rather than re-learned.
            bool const firstEver = !episode.known;
            Restart(episode, sample);
            episode.known = true;
            episode.mounted = sample.mounted;
            look.mountedNow = sample.mounted;
            (void)firstEver;
            return look;
        }

        float const moved = Dist(episode, sample);
        if (moved > sample.radius)
        {
            // It left the place. This is the test of "the inputs did not change" for position, and it is what
            // keeps the legitimate case out: a bot re-mounting for each leg of a long walk has covered ground
            // between the changes, so its changes are never counted against each other.
            episode.known = true;
            bool const wasMounted = episode.mounted;
            Restart(episode, sample);
            episode.mounted = sample.mounted;
            look.mountedNow = sample.mounted;
            // The state may also have changed in this very sample; it belongs to the NEW episode, where it is
            // the first change and has nothing to repeat.
            if (wasMounted != sample.mounted)
            {
                look.changed = true;
                look.justification = Justification(sample.facts);
                if (sample.mounted)
                {
                    episode.mounts = 1;
                    episode.lastOn = look.justification;
                }
                else
                {
                    episode.dismounts = 1;
                    episode.lastOff = look.justification;
                }
            }
            look.mounts = episode.mounts;
            look.dismounts = episode.dismounts;
            return look;
        }

        if (moved > episode.maxYd)
            episode.maxYd = moved;  // the radius it stayed INSIDE, never the step that took it out

        if (episode.mounted != sample.mounted)
        {
            look.changed = true;
            std::string const why = Justification(sample.facts);
            look.justification = why;
            std::string& sameDirection = sample.mounted ? episode.lastOn : episode.lastOff;
            if (!sameDirection.empty() && sameDirection == why)
            {
                ++episode.flips;
                episode.flipUnder = why;
            }
            sameDirection = why;
            if (sample.mounted)
                ++episode.mounts;
            else
                ++episode.dismounts;
            episode.mounted = sample.mounted;
        }

        look.mountedNow = episode.mounted;
        look.mounts = episode.mounts;
        look.dismounts = episode.dismounts;
        look.flips = episode.flips;
        look.flipUnder = episode.flipUnder;
        look.heldSec = Diff(episode.sinceMs, sample.nowMs) / 1000.f;
        look.heldYd = episode.maxYd;

        // Reported once per instrument window while the episode holds at least one change, and at once on the
        // sample that flips: the cadence is the sampler's and is reused, not invented. An episode with no change
        // at all writes nothing - a bot that simply stays mounted is not this detector's subject, and reporting
        // it would be the clean-result poisoning the standing watch already paid for (406 of 3 900 rows).
        bool const anyChange = episode.mounts + episode.dismounts > 0;
        if (anyChange && (look.changed || Diff(episode.reportedMs, sample.nowMs) >= sample.cadenceMs))
        {
            look.report = true;
            episode.reportedMs = sample.nowMs;
        }
        return look;
    }
}  // namespace Journal::MountState
