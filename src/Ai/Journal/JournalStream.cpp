/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [jrn20] The raw stream into pb_journal_line: every line the server prints about itself, with a millisecond stamp.
//
// This is the insurance table. The narrow tables (decisions, their options, loot, metrics) answer the questions we
// know to ask; this one holds whatever they did not foresee, so a question nobody indexed is still answerable.
//
// How a line gets here. The core's AppenderJournal hands every line of a logger it is attached to to a sink; this
// file installs that sink. The interception has to happen there and not here: every line goes through
// LOG_INFO("playerbots", ...) at thousands of call sites, appenders are built inside sLog->Initialize() before any
// module exists, and the log FILE carries no timestamp at all - so loading the file afterwards could not recover
// when a line happened. See azerothcore/src/common/Logging/AppenderJournal.h for the core side and how to remove it.
//
// OFF BY DEFAULT (AiPlayerbot.Journal.Stream = 0). At 75 bots the stream is about 191 000 lines and 38 MB an hour,
// so a day is 4.6 million rows and roughly 1.5-2 GB in MySQL once row overhead and the three indexes are counted.
// That is affordable at the retention the user chose; it is not something to turn on without meaning to.
//
// WHAT IS NOT GUESSED HERE. The row's `bot_low` / `bot_name` stay empty for appender lines. Most lines do begin with
// a bot's name after the tag, but a name is not a unit: picking the bot by matching a name against the world is
// exactly the mistake that produced "the mob blocks itself" - the name matched, the unit did not. A caller that
// knows which bot it is talking about passes it through Journal::RecordLine directly; the appender cannot know, so
// it says nothing rather than something plausible. The text keeps the name either way.
//
// To turn the stream on, the appender also has to exist and be attached in worldserver.conf:
//     Appender.Journal=100,5,0
//     Logger.playerbots=5,Console Playerbots Journal
// Attach it to the `playerbots` logger only. On Logger.root it would carry the whole server's logging into the
// table, which is a different (and much larger) decision than this one.

#include "Journal.h"

#include "AppenderJournal.h"

#include <string>

namespace
{
    // the leading "[tag]" of a line, "" when it has none. Only a tag at the START of the line is the line's own
    // author: tags also appear mid-line as references to other code ("... + 0 heals [dun235], healer reaches it"),
    // and counting those as emitters is what inflates a grep by hundreds of lines.
    // [jrn40] THE BOUND IS 31, NOT 24, AND THAT NUMBER WAS LOSING TAGS. The tag column was VARCHAR(24) and this
    // test allowed a tag of up to 25 characters, so the two disagreed and both were too small for tags the code
    // actually prints. Measured on the live table:
    //   * "[pull_estimate_vs_actual]" (1 096 rows) and "[queue-stale-idle-victim]" (333 rows) are 25 characters -
    //     they passed this test and were then CLIPPED by the column to 24, losing the closing bracket, so a query
    //     for the real tag matched neither of them;
    //   * "[conjure_outside_instances]" (26) and "[route_blocker_required_only]" (29) failed this test outright and
    //     reached the table with an EMPTY tag, 19 rows that plainly begin with a tag.
    // A label that is wrong is worse than one that is missing, because a count by it answers. 31 here and
    // VARCHAR(32) there are now the same bound: substr(0, end + 1) is at most 32 characters.
    //
    // WHAT IS STILL EMPTY ON PURPOSE, and it is not this function's doing: 4 373 rows have no leading tag because
    // the LINE has none - they begin with a bot's name ("Relara careful pull: ..."), or with "Test harness:", or
    // with "Bot GUID Full:". Two of those matter and are named here so nobody looks for a parser bug again: the
    // harness's post-mortem verdict ("Test harness: ... finished: stalled (no progress for 4 minutes ...)") and the
    // movement watchdog ("stuck watchdog: reset movement (no progress 6s, still 3s ...)"). Both are detectors, and
    // neither can be found by tag at all - which is why the list of detectors has to be made by reading the code
    // and never by selecting tags out of this table.
    std::string LeadingTag(std::string const& text)
    {
        if (text.empty() || text.front() != '[')
            return {};
        size_t const end = text.find(']');
        if (end == std::string::npos || end > 31)
            return {};
        return text.substr(0, end + 1);
    }

    void OnLogLine(uint64 atMs, std::string const& /*logger*/, std::string const& text)
    {
        Journal::Line line;
        line.atMs = atMs;
        line.tag = LeadingTag(text);
        line.text = text;
        Journal::RecordLine(line);
    }
}

namespace Journal
{
    // Installed when the journal attaches its readers. Everything logged before that point - the core's own startup -
    // is dropped, which is why the journal's history starts at the boot row and not at the process's first line.
    //
    // This was a file-scope object whose constructor installed the sink, and that is why b495 wrote nothing at all
    // and AiPlayerbot.Journal.Stream = 1 did nothing: the module is a static library, no symbol of this file was
    // asked for, so the linker left the object file out - and a discarded object file's constructor never runs.
    // Journal::Attach() calls this now.
    // The sink goes in unconditionally and NO setting is read here: after the b497 crash, attaching asks the config
    // nothing at all (see Journal::Attach). AiPlayerbot.Journal.Stream is honoured where it was already honoured, in
    // RecordLine, which runs only when a line is actually being written - that is, at runtime, where asking is safe.
    // The cost of an unconditional sink while the stream is off is one call that returns on its first test.
    void AttachLogStream() { AppenderJournal::SetSink(&OnLogLine); }
}
