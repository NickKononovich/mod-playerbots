/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_JOURNAL_STANDINGEPISODE_H
#define PLAYERBOTS_JOURNAL_STANDINGEPISODE_H

// [jrn41] THE STANDING EPISODE'S ARITHMETIC, AS A SEAM: the samples go in as numbers, the verdict "report this
// episode, and these are its duration and its radius" comes out, and nothing here reads the world, the clock or
// the database. Journal::NoteStanding (JournalIdle.cpp) reads the bot's position, map and holds from the map as it
// always did and calls Observe with them.
//
// THE CLOCK IS AN INPUT. `nowMs` and the cadence come in as numbers; the wrap-around arithmetic is the core's
// getMSTimeDiff, character for character, so a replay is repeatable and no test depends on when it runs.
//
// WHAT THIS SEAM EXISTS TO HOLD, measured on the live table: two rows of 79 claimed a bot "stood for 68 s within
// 3355.5 yd" and "19.8 s within 4451.1 yd" - both at map 43, the position just inside Wailing Caverns, while the
// anchor they were measured against was the outdoor position on map 1. The anchor carried no map, so the distance
// was taken between two points in different worlds, and a teleport into an instance was reported as a standing
// episode with a radius of three and a half thousand yards. Two defects in one row:
//   * an anchor without a map compares positions across maps, which is not a distance at all;
//   * the episode's `stoodYd` was the distance that BROKE the episode - the step out of the radius - while the
//     column means the radius the bot stayed INSIDE. A number that means its own opposite in the ending row is
//     worse than a missing one, because a query by it answers.
// So the anchor carries its map, a sample on another map ends the episode without reporting it (across a teleport
// there is no "stood still" to assert, and the stretch before it has already been reported once per cadence), and
// `stoodYd` is the widest distance from the anchor seen WHILE INSIDE the radius.

#include <cstdint>

namespace Journal::Standing
{
    // The open episode: where the bot was last seen to be making progress through space, and when.
    struct Anchor
    {
        bool open = false;
        uint32_t sinceMs = 0;     // when the bot last moved out of its radius
        uint32_t reportedMs = 0;  // when this episode was last written
        uint32_t mapId = 0;       // the map the anchor is a point ON: a position on another map is not a distance away
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        float maxYd = 0.f;  // the widest distance from the anchor seen while inside the radius
    };

    // One look at the bot. `held` is the detector's own list of legitimate reasons to be still (dead, sitting,
    // casting, in flight, being teleported), read in the emitter at this instant and passed in as a fact.
    struct Sample
    {
        uint32_t nowMs = 0;
        uint32_t mapId = 0;
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        bool held = false;
        float radius = 1.5f;
        uint32_t cadenceMs = 10000;
    };

    // What the emitter should write, if anything.
    struct Look
    {
        bool report = false;  // write a row for this episode now
        bool ended = false;   // the episode is over ("stood for"), as against still open ("has been standing")
        float stoodSec = 0.f;
        float stoodYd = 0.f;  // the radius the bot stayed inside, never the step that took it out
    };

    // Advances the anchor by one sample and says what to write. The anchor is the caller's state, one per bot.
    Look Observe(Anchor& anchor, Sample const& sample);
}

#endif
