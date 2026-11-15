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
#include <string>

namespace Journal::Standing
{
    // [jrn42] THE LEGITIMATE STILLNESSES, AS FACTS WITH NAMES. Until now the emitter folded them into one
    // anonymous bool, so the row could say "nothing legitimately held it still" and be wrong without anyone
    // being able to ask which hold it was. They fall into two kinds, and the kinds behave differently on purpose:
    //
    //   * THE BOT IS NOT STANDING AT ALL (dead, sitting, casting, in flight, being teleported). These end the
    //     episode, exactly as before: a duration joined across a sit-down would be a number about nothing.
    //
    //   * THE BOT IS STANDING, AND IT IS RIGHT TO (its leader ordered it to stand, it waits on the pier for a
    //     transport, its walk waits for the group). These LABEL the episode and never end it, because the bot
    //     really does hold one position and the duration is the very number worth having. Measured on b502:
    //     Ganna stood 120 s at (-37.33, 2475.54) on map 369 and boarded tram car 176084 fifteen seconds after
    //     the last row - one wait of one tram cycle (the module's own note puts one way at 63 s), reported as
    //     twelve problems. Ending the episode on such a hold would instead write one 10 s row per sample, which
    //     is the cadence talking and not the bot.
    //
    // The name is also the cause field the deduplication key has been missing (IssueKey.h says so in as many
    // words): without it the largest key covered 17 bots over 4 maps because this detector held nothing that
    // told one cause of standing from another.
    struct Holds
    {
        bool dead = false;
        bool sitting = false;
        bool casting = false;
        bool inFlight = false;
        bool beingTeleported = false;
        bool orderedToStand = false;     // its leader's Hold / StandAt, or it is already on the spot it was sent to
        bool waitsForTransport = false;  // on the pier / platform until the boat, zeppelin or tram docks
        bool waitsForGroup = false;      // its far walk waits with the group (NewRpgInfo::groupWaiting)
    };

    // True when the bot is not standing at all and the episode is over. The three labelling holds are NOT here.
    bool EndsTheEpisode(Holds const& holds);

    // The hold's name for the row and for the key, "" when nothing held it still. One hold is named per sample:
    // the first in this order, so the name is stable for one state of the world rather than a set that grows.
    char const* HoldName(Holds const& holds);

    // Engine::GetLastAction() is a ring buffer kept to its NEWEST 512 characters, so the end of the trail is the
    // newest end. A column narrower than the trail must therefore keep the TAIL: keeping the head answers "what
    // did it push first", while the column is read as "its engine's last action". Measured on b502: 170 of 1029
    // standing rows had filled `why` to its 512 characters, so the newest ~110 characters of the trail fell off
    // the end of it, and `doing` (96 characters) held the OLDEST 96 in 837 of 1032 rows. The cut is made at a
    // '|' boundary when one lies within the room, so a row never starts in the middle of an action's name.
    std::string TrailTail(std::string const& trail, std::size_t room);

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

    // One look at the bot. `holds` is the detector's own list of legitimate reasons to be still, each read in the
    // emitter at this instant and passed in as a fact.
    struct Sample
    {
        uint32_t nowMs = 0;
        uint32_t mapId = 0;
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        Holds holds;
        float radius = 1.5f;
        uint32_t cadenceMs = 10000;
    };

    // What the emitter should write, if anything.
    struct Look
    {
        bool report = false;  // write a row for this episode now
        bool ended = false;   // the episode is over ("stood for"), as against still open ("has been standing")
        float stoodSec = 0.f;
        float stoodYd = 0.f;        // the radius the bot stayed inside, never the step that took it out
        char const* heldBy = "";    // [jrn42] the legitimate hold this episode stands under, "" when none
    };

    // Advances the anchor by one sample and says what to write. The anchor is the caller's state, one per bot.
    Look Observe(Anchor& anchor, Sample const& sample);
}

#endif
