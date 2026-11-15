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
#include "Config.h"

#include <string>

namespace
{
    // the leading "[tag]" of a line, "" when it has none. Only a tag at the START of the line is the line's own
    // author: tags also appear mid-line as references to other code ("... + 0 heals [dun235], healer reaches it"),
    // and counting those as emitters is what inflates a grep by hundreds of lines.
    std::string LeadingTag(std::string const& text)
    {
        if (text.empty() || text.front() != '[')
            return {};
        size_t const end = text.find(']');
        if (end == std::string::npos || end > 24)
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
    void AttachLogStream()
    {
        if (sConfigMgr->GetOption<bool>("AiPlayerbot.Journal.Stream", false, false))
            AppenderJournal::SetSink(&OnLogLine);
    }
}
