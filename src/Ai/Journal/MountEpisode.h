/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_JOURNAL_MOUNTEPISODE_H
#define PLAYERBOTS_JOURNAL_MOUNTEPISODE_H

// [mount9] THE MOUNT STATE'S OWN EPISODE, AS A SEAM: the samples go in as facts and numbers, the verdict "this bot
// has been getting on and off its mount in one place, this many times, and under THESE unchanged facts" comes out,
// and nothing here reads the world, the clock or the database. Journal::NoteMount (JournalMount.cpp) reads the
// bot's state from the map as the standing watch beside it does and calls Observe with it.
//
// THE USER ASKED FOR THIS ONE BY NAME: "they look stuck, they just stand there, propped up against something, and
// they keep getting off the mount and back on. That is a plain bug. I need such cases caught in issues."
//
// WHY IT WAS NOT IN THE PROBLEMS TABLE, AND WHY THAT IS A TRAP OF ITS OWN RATHER THAN AN OVERSIGHT. Getting on a
// mount is a CAST. The standing watch's criterion for a real problem (Journal::Standing) answers honestly that a
// casting bot is not idle, so every one of these windows was held as legitimate under the name `casting` - and
// worse than held: a cast ENDS the standing episode, so the duration restarts with every mount. Measured on b504:
// Emdyl stood at (-8777.3105, 665.5, 103.1) in Stormwind, `stood_yd` 0.000 in every row, across 95.4 MINUTES,
// and the longest episode the standing watch ever reported there was 69.9 s; 154 of its 462 rows restart at
// 10.0 s. Averaged over the whole table, standing rows whose engine trace shows the mount action executing run
// 17.8 s against 72.6 s for the rest - four times shorter, which is the cast talking and not the bot.
// THE GENERAL FORM: LEGITIMACY ESTABLISHED INSIDE ONE WINDOW DOES NOT CARRY OVER TO A SEQUENCE OF WINDOWS. One
// mount is legitimate; the same mount in a circle is not, and a criterion that looks at one window alone cannot
// tell them apart. So this detector's subject is the SEQUENCE, and that is the only reason it exists beside the
// standing watch instead of inside it.
//
// THE BOUNDARY IS DERIVED AND NOT CHOSEN, which the user's rules require twice over (no rule by elapsed time, and
// no threshold without his word). It is NOT "N changes in M seconds". A change of the mount state is justified by
// a FACT - a fight began, the bot went indoors, it entered water, its walk ended, its mount leader got off. So:
//
//   A SECOND CHANGE IN THE SAME DIRECTION, UNDER A JUSTIFICATION IDENTICAL TO THE FIRST, WITH THE BOT STILL
//   INSIDE ITS OWN ANCHOR RADIUS ON THE SAME MAP, CANNOT BE RIGHT.
//
// Nothing that justified the first change had changed, so one of the two was taken against unchanged inputs -
// which is this project's rule 5 (one quantity, one judge) seen from the outside. ONE such repetition is already
// the proof, so the count that makes the row real is 1, and it is arithmetic rather than taste.
//
// THE ANCHOR RADIUS IS WHAT KEEPS THE LEGITIMATE CASE OUT, and it is not a new number either: it is
// max(combat reach, 1.5 yd), the same expression the standing watch and GuildCoordinator's loop watch already use
// for "the same place". A bot walking 1 500 yd gets off for each fight on the way and back on afterwards under
// the very same words ("a fight: its target in reach") - measured on b504, Maronic re-mounted five times for one
// walk to (-5081,-2338) with 1 906, 1 825, 1 476, 1 432 and 1 381 yd left - and every one of those is right,
// because the bot had covered 525 yd between the first and the last. The radius is therefore the test of "the
// inputs did not change" for position, and it is why that sequence scores zero flips while Emdyl's, at one point
// to four decimals, scores every one.
//
// WHAT IS NOT IN HERE, deliberately: no clock of its own (`nowMs` and the cadence are inputs, the wrap-around
// arithmetic is the core's getMSTimeDiff character for character), no reading of the bot, and no decision - this
// file writes to nothing and no bot ever asks it anything.

#include <cstdint>
#include <string>

namespace Journal::MountState
{
    // [mount9] THE FACTS THAT CAN JUSTIFY A CHANGE OF THE MOUNT STATE, each read by the emitter at the instant of
    // the change and passed in. They are the facts the code that changes the state actually names: the dismount
    // reasons recorded by GuildRiding::NoteDismount and the conditions MountForWalk and CheckMountStateAction
    // test. A fact missing from this list is a justification this detector cannot see, which would make it call a
    // legitimate change a flip - so the list grows by adding a fact, never by loosening the rule.
    struct Facts
    {
        bool indoors = false;        // the core takes the mount away indoors, and MountForWalk refuses there
        bool inCombat = false;       // a fight, by the bot's own combat state
        bool targetInReach = false;  // a target inside the dismount band (CheckMountStateAction's own quantity)
        bool inWater = false;
        bool onTransport = false;  // a boat, a zeppelin, the tram, a vehicle, a taxi
        bool moving = false;       // it is walking: the only state in which a ride has anything to do
        bool hasLeader = false;    // it has a mount leader whose state it copies
        bool leaderRides = false;  // and that leader is mounted or casting its mount
        bool hasWalk = false;      // a walk is in play (NewRpgInfo::mountWalkT)
        // The walk's own target in WHOLE YARDS. A new walk is a new justification, so the target is part of the
        // justification and not of the position: two mounts for two different walks are two decisions, however
        // close together the bot was standing. Whole yards because a float that differs in its last bit would
        // make every change its own justification, which is this project's trap #2 in another dress.
        int32_t walkToX = 0;
        int32_t walkToY = 0;
    };

    // The justification, as a stable string: the facts that could justify a change, WITHOUT the state itself.
    // "nothing" when no fact speaks at all - which is Emdyl's case in town and the worst one, because a change
    // with no justification cannot have a second one that differs.
    std::string Justification(Facts const& facts);

    // One look at the bot, taken by the emitter on the bot's own tick.
    struct Sample
    {
        uint32_t nowMs = 0;
        uint32_t mapId = 0;
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        bool mounted = false;  // the state NOW, read once, whoever caused it
        Facts facts;
        float radius = 1.5f;        // max(combat reach, 1.5 yd), passed in, never computed here
        uint32_t cadenceMs = 10000;  // the journal's one instrument window
    };

    // The caller's state, one per bot. `known` is the distinction this project spells '' against '?': until the
    // first sample there is no previous state, and the first sample is NOT a change.
    struct Episode
    {
        bool open = false;
        bool known = false;
        bool mounted = false;
        uint32_t sinceMs = 0;     // when this episode began (the bot last left its radius, or the map changed)
        uint32_t reportedMs = 0;  // when it was last written
        uint32_t mapId = 0;       // the map the anchor is a point ON: a position on another map is not a distance
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        float maxYd = 0.f;     // the widest distance from the anchor seen WHILE INSIDE the radius
        uint32_t mounts = 0;   // changes onto the mount within this episode
        uint32_t dismounts = 0;
        uint32_t flips = 0;    // changes that repeated a same-direction justification: see the rule above
        std::string lastOn;    // the justification of the last change ONTO the mount ("" none yet)
        std::string lastOff;   // ... and off it
        std::string flipUnder;  // the justification a flip repeated under, for the row and for the key
    };

    // What the emitter should write, if anything.
    struct Look
    {
        bool report = false;   // write a row now
        bool changed = false;  // this very sample was a change of state
        bool mountedNow = false;
        uint32_t mounts = 0;
        uint32_t dismounts = 0;
        uint32_t flips = 0;
        float heldSec = 0.f;  // how long this episode has held one place
        float heldYd = 0.f;   // the radius it stayed inside, never the step that took it out
        std::string justification;  // of this sample's change, "" when this sample was not a change
        std::string flipUnder;      // the justification the flips repeated under, "" when there were none
    };

    // A flip is the proof, and one is enough: see the derivation above. Kept as a function so that the number is
    // in one place and a test can state it.
    bool IsReal(Episode const& episode);

    // Advances the episode by one sample and says what to write.
    Look Observe(Episode& episode, Sample const& sample);
}

#endif
