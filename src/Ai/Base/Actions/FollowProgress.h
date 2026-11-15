/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AI_BASE_ACTIONS_FOLLOWPROGRESS_H
#define PLAYERBOTS_AI_BASE_ACTIONS_FOLLOWPROGRESS_H

// [stuck260] IS THE FOLLOW MAKING PROGRESS, AS A SEAM: the facts of one sample go in, the answer comes out, and
// nothing here reads the world, the clock or the database. Header-only, so the unit stand compiles it alone.
//
// WHAT THE CRITERION SAID AND WHAT WAS TRUE. The watchdog's steps are measured in seconds "with no progress", and
// of the 7 recorded `watchdog` rows 5 carry `still for 0 s` - the bot was walking the whole time the watch called
// it stuck. Peny following Bami is the plain case: `reset movement: no progress for 6 s` at 116 yd, then
// `follow again: no progress for 12 s` at 81 yd - 35 yards CLOSED, 5.8 yd/s, while the line said no progress.
//
// THE CAUSE WAS THE REFERENCE THE GAP WAS COMPARED WITH, not the tolerance and not the stages. The watch keeps one
// `_bestDistance`, written only by Restart - and Restart runs on every sample the bot is WITH the group, where the
// gap is a dozen yards. When the bot then falls behind, that number stays: the test `gap + 2 < reference` is being
// asked whether 81 yd is nearer than the 20 yd the bot stood at while it was still with its leader. It never can
// be, so the "no progress" seconds ran on through a sprint. The fix is one statement: THE REFERENCE BELONGS TO
// THIS EPISODE - the gap at the moment the watch armed (the first sample the bot was behind by more than the
// formation allows), improved by every yard actually closed since.
//
// THE STAGES AND THEIR SECONDS ARE NOT TOUCHED (6 / 12 / 20 / 45 / 90): those are behaviour the user set, and this
// file decides nothing about them. It answers one question - whether the gap is coming down - and that answer is
// what the seconds are counted from.
//
// NO CLOCK IS IN HERE: every field is a distance. A sample cannot become progress by being waited for (rule 2).

namespace Bot::Follow
{
    struct ProgressFacts
    {
        // how far the bot is from the one it follows, now
        float gapYd = 0.0f;
        // the gap this episode has to beat: the gap when the watch armed, lowered by every real improvement since.
        // It is the episode's own number - never one carried over from a sample where the bot was with the group
        float gapToBeatYd = 0.0f;
        // the yards that count as a move rather than as the noise of standing (the watch's PROGRESS_DISTANCE)
        float toleranceYd = 0.0f;
        // the leader itself has walked off the spot this episode's anchor was taken at (LEADER_MOVED_DISTANCE):
        // keeping pace with a leader on the move is progress too, even while the gap does not shrink
        bool leaderMovedOff = false;
        // the ground the bot covered since that anchor
        float botMovedYd = 0.0f;
    };

    // The yards of the gap actually closed since this episode's reference (negative: the gap grew)
    inline float ClosedYd(ProgressFacts const& facts) { return facts.gapToBeatYd - facts.gapYd; }

    // The gap is coming down by a measurable amount
    inline bool Closing(ProgressFacts const& facts) { return ClosedYd(facts) >= facts.toleranceYd; }

    // It is keeping up with a leader that is on the move
    inline bool KeepingPace(ProgressFacts const& facts)
    {
        return facts.leaderMovedOff && facts.botMovedYd >= facts.toleranceYd;
    }

    inline bool MakesProgress(ProgressFacts const& facts) { return Closing(facts) || KeepingPace(facts); }
}

#endif
