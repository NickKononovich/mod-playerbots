/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_JOURNAL_ISSUEKEY_H
#define PLAYERBOTS_JOURNAL_ISSUEKEY_H

// [jrn41] THE DEDUPLICATION KEYS OF pb_journal_issue, AS A SEAM: the causal parts go in, the key comes out, and
// nothing here reads the world, the clock or the database. The emitters (JournalIdle.cpp, PlayerbotAI.cpp) gather
// the facts from the map as they always did and call these functions where they used to concatenate the key by hand.
//
// WHY A SEAM AND NOT A STRING BUILT ON THE SPOT: the key decides what counts as ONE problem, so a wrong key is
// measured in the problems table and nowhere else - and it has now been wrong twice in the same way. The key may
// therefore be asserted by a test over recorded inputs, with no server, and both directions have to be asserted:
// too narrow fans one cause into a staircase of rows, too wide merges two causes into one row and then a fix for
// the first one turns the test green while the second lives on under it.
//
// THE ENGINE'S ACTION TRACE IS AN INPUT TO THE ROW AND NEVER TO THE KEY. Engine::GetLastAction() returns the
// trace of what the engine pushed, ran and refused in that instant - "|A:create soulstone - PREREQ|A:create
// soulstone - FAILED|..." - and it changes on every sample of one standing episode. Measured on the live table:
// one bot (Kylantino, map 369, the same position to four decimals) standing from 10 s to 60 s wrote SIX samples
// that became FIVE issue rows, because the trace is in the key; and the project had already measured the same
// trap on the idle diagnostics, where keying on the whole line gave 757 "causes" over 3 900 rows and the 757 were
// a trailing list of action names. The trace stays in `why` and in the domain row (pb_journal_idle.doing,
// pb_journal_stuck.action_running), where it is read by eye in a row already found.

#include <cstdint>
#include <string>

namespace Journal::Key
{
    // `standing`: the key of the standing watch (JournalIdle.cpp). The cause as this detector can state it is the
    // bot's role in its five, whether it stood through a fight, and which state its AI was in - the same three
    // kinds of part the sibling kinds use (`idle|<role>|<buckets>`, `stuck_target|<role>|...`). `actionTrace` is
    // passed in so that the one place that must NOT use it is the one place that holds it.
    std::string Standing(std::string const& role, bool inCombat, uint32_t engineState,
                         std::string const& actionTrace);

    // `stuck_target`: the combat engine holds an idle mob as its target and nothing of it runs (PlayerbotAI.cpp).
    // The cause is the role plus whether the engine had anything at all to run - an empty trace is the point of
    // this problem - and never the trace's text.
    std::string StuckTarget(std::string const& role, std::string const& actionRunning);
}

#endif
