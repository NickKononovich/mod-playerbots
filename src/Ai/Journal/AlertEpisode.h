/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AI_JOURNAL_ALERTEPISODE_H
#define PLAYERBOTS_AI_JOURNAL_ALERTEPISODE_H

#include <string>

// [alert20] ONE STANDING EPISODE IS ONE EVENT, AS A SEAM. Header-only, pure: no world, no clock, no database.
//
// WHAT WAS MEASURED. The STUCK watch's repeat gate was `stuckAlerted && stuckKey == status + "|" + task`, so the
// gate re-opened whenever the status or the task CHANGED - and a bot that stands still changes status. Feette
// (bot_low 1787) stood in ONE place for 132 s and the journal holds THIRTEEN STUCK rows of it, at 62, 72, 77, 82,
// 87, 97, 102, 107, 112, 117, 122, 127 and 132 s, with the status flipping GO_GRIND <-> WANDER_RANDOM every five
// seconds and the task `GATHER (tasks 5)` unchanged throughout. Those thirteen rows also fell under TWO different
// issue keys (`alert|STUCK|WANDER_RANDOM` and `alert|STUCK|GO_GRIND N yd`), so one event was counted thirteen times
// and filed twice. Over the whole corpus with an identity, 16 of 62 STUCK rows (25.8 %) repeat an episode that was
// already reported.
//
// This is trap #1 of the project ("count events, not rows", inflations up to 7.3x) in the form trap #11 names: the
// repeat key was tied to the STATUS instead of to the episode.
//
// THE REPLACEMENT IS A FACT, NOT A SHORTER KEY (rule 2): the episode. The standing watch
// (Journal::StoodSecOf) owns "it has not moved" and answers 0 when no episode is open - the watch already clears
// its reported flag on that 0 - so the only thing the gate has to ask is whether the OPEN episode has been
// reported. The status the episode happens to wear is recorded in its own column and no longer splits it.
namespace Journal::AlertEpisode
{
    struct Repeat
    {
        // the standing watch has an open episode for this bot (its duration is above zero). 0 s is "no open
        // episode" - it moved, or it is not standing at all - and never "it stood for 0 s"
        bool episodeOpen = false;
        // this watch has already written a row for the episode now open
        bool reportedAlready = false;
        // the two facts the OLD gate also compared, kept here so the stand can assert that they no longer split
        // one episode. They are recorded; they do not decide
        std::string status;
        std::string task;
        std::string reportedStatus;
        std::string reportedTask;
    };

    // Is this sample an event worth a row of its own?
    inline bool Reports(Repeat const& r) { return r.episodeOpen && !r.reportedAlready; }
}

#endif
