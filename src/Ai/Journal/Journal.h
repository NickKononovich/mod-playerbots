/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_JOURNAL_JOURNAL_H
#define PLAYERBOTS_JOURNAL_JOURNAL_H

// [jrn20] The MySQL journal: the server's own history, in `acore_playerbots`, with a millisecond stamp on every row.
//
// Why a table and not the log file. Playerbots.log is truncated at every server start, so the baseline a comparison
// needs disappears with the thing it compares; the window a measurement covers is whatever the run happened to last,
// so raw counts are not comparable and have to be normalised by hand; and the count itself is a grep pattern, so two
// readers measuring "the same" thing can disagree because their patterns match different lines - one of them scoring
// zero by construction, looking for a cause in a line that never carries it. In a table none of that happens: the
// history outlives the restart (`boot_id`), the window is a WHERE clause on `at_ms`, and the count is one query
// everybody runs.
//
// What the journal must make queryable, and why it is built this way: four diagnoses in one night were wrong the same
// way - a log line was taken for the verdict without asking who reads it and what it actually counts. So a journal row
// carries, besides the choice: the judge (`judge`), the code site that printed it (`tag`, `src`), the field the judge
// read (`read_field`), and who reads its verdict (`read_by` - the empty string means "nobody known", which has twice
// turned out to be the truth, the line standing on the losing side of an argument between two judges). The options the
// judge did NOT take are rows of their own in `pb_journal_option`, each with its own reason, cost and source kind - so
// "why did the bot stand" and "where did that rate come from, and was a mob drop counted as a source at all" are
// queries, not investigations.
//
// Cost. Nothing here runs a query on the world thread: Record() formats the row's values and pushes it onto a vector
// under a short lock; a writer thread of its own swaps that vector, builds one multi-row INSERT per batch and hands it
// to PlayerbotsDatabase.Execute(), which is itself asynchronous. Retention drops whole hour partitions
// (ALTER TABLE ... DROP PARTITION), never DELETE, so cleaning a day out of a five-million-row table is a metadata
// operation and holds no lock on the hot partition.
//
// Settings (playerbots.conf, read once at the first row - the retention is a setting, not a constant in the code):
//   AiPlayerbot.Journal.Enable          (bool, default true)   write at all
//   AiPlayerbot.Journal.RetentionHours  (int,  default 48)     how far back the journal keeps rows
//   AiPlayerbot.Journal.BatchRows       (int,  default 500)    rows per INSERT
//   AiPlayerbot.Journal.FlushMs         (int,  default 1000)   how long a partial batch waits
//   AiPlayerbot.Journal.Stream          (bool, default false)  also mirror raw log lines into pb_journal_line
//
// Tables: pb_journal_boot (one row per server start), pb_journal_decision (a choice, parsed into columns),
// pb_journal_option (one row per option the judge weighed, taken or not), pb_journal_read (a reader that actually
// consumed a verdict), pb_journal_line (the raw stream, when enabled or filled by tools/journal_load.py).
// The DDL is also kept as a file for review: data/sql/playerbots/updates/2026_11_15_01_pb_journal_jrn20.sql.

#include <string>
#include <vector>

#include "Define.h"
#include "ObjectGuid.h"

namespace Journal
{
    // Who the judge is. The same decision taken by a different judge is a different decision, and "one judge per
    // decision" is only checkable when the row says which one spoke.
    enum class Judge : uint8
    {
        Bot = 0,          // the bot deciding about itself
        FiveLeader = 1,   // the leader of a five, inside a run
        GuildLeader = 2,  // the global leader of the 25
        Harness = 3,      // the test harness / a tool
        Unknown = 4
    };

    char const* JudgeName(Judge judge);

    // One option the judge weighed. `taken` marks the one it chose; every other row is what it rejected and why.
    // `sourceKind` exists for the priced options (where a number came from): "node", "drop", "vendor", "craft",
    // "quest", "bank", "mail", "" - a plan refused on a rate of 0.01/min is only answerable when the row says whether
    // a drop source was among the options at all, or was never considered.
    struct Option
    {
        std::string name;        // the option ("Mageweave Cloth x4784", "Deviate Ravager (low 167)", "pull now")
        std::string kind;        // the option's class ("mob", "pack", "item", "spot", "plan", "member", ...)
        std::string reason;      // why it was taken or rejected, in the judge's own words
        std::string sourceKind;  // where its number comes from, when it has one
        double cost = 0.0;       // the price the judge put on it (its own unit, named by `costUnit` on the decision)
        double value = 0.0;      // the gain the judge put on it
        bool taken = false;
        std::string fields;  // the numbers the judge weighed it on, as one JSON object ("" for none)
    };

    // One decision. Everything here is a column, not text to grep.
    struct Decision
    {
        std::string topic;    // what was being decided ("pull", "rest", "town", "profession_plan", "five_roster")
        std::string choice;   // the option chosen ("engage Deviate Ravager", "no pull", "Tailoring 20->580")
        std::string verdict;  // the choice's class: "taken" | "held" | "refused" | "none" - one word, queryable
        std::string why;      // the judge's own sentence
        Judge judge = Judge::Unknown;
        std::string judgeName;  // the deciding bot / leader by name ("" when the judge has no character)
        uint32 judgeLow = 0;    // its GUID counter (0: none)

        std::string subject;      // what the decision is about (a mob, an item, a member)
        uint32 subjectEntry = 0;  // its entry (creature / item), 0 when it has none
        uint32 subjectLow = 0;    // its GUID counter, 0 when it has none
        std::string subjectKind;  // "creature" | "item" | "member" | "spot" | "" ...

        double cost = 0.0;     // the price of the choice
        std::string costUnit;  // what `cost` is measured in ("tank hp", "sec", "copper", "min", ...)

        std::string tag;        // the code tag that owns the line ("[pull81]"), so the row names its own author
        std::string src;        // file:line of the judge, so "who prints this" needs no grep
        std::string readField;  // the field the judge actually read to decide ("fightSecLeft", "rate_per_min")
        std::string readBy;     // who consumes this verdict; "" means nobody known - the point of the column

        std::string runId;   // the run a decision inside a run belongs to (guild_project_runs / harness id)
        uint32 runMs = 0;    // ms since that run started
        std::string fields;  // the judge's remaining inputs as one JSON object ("" for none)

        // [jrn20] ONE count, not two. The line the server printed about this decision is kept in the row itself, so
        // the table is not a second, independently produced tally of what the file says: there is one event, written
        // once, and the printed line carries the row's id ("[jrn20 #<id>]"). A grep of the file and a COUNT over the
        // table therefore cannot disagree - if they did, the ids would show which rows are missing, and that is a
        // defect with an address rather than an argument between two patterns. Nothing recomputes a narrow table
        // from the stream table; the narrow row is the original.
        std::string text;

        std::vector<Option> options;  // every option weighed, the taken one included
    };

    // Writes one decision (and its options). World thread safe, no query runs here. Returns the row's journal id
    // (0 when the journal is off), for NoteRead() and for referring to it from another row.
    uint64 Record(Decision const& decision);

    // One loot item and the decision behind it. The run is referred to by `runId` only: `guild_project_runs` stays
    // the one table of runs, and the journal never keeps a second copy of a run (its `at` column is the run's START,
    // so a window over runs is ORDER BY id DESC LIMIT n, not a range over `at`).
    struct Loot
    {
        uint32 itemEntry = 0;
        std::string itemName;
        uint32 count = 1;
        uint32 quality = 0;
        std::string fromKind;  // "mob" | "boss" | "chest" | "quest" | "craft" | "vendor"
        std::string fromName;  // the creature / object it came from
        uint32 fromEntry = 0;
        std::string toName;  // the member it went to ("" when nobody took it)
        uint32 toLow = 0;
        std::string verdict;    // "equipped" | "bank" | "vendor" | "disenchant" | "nobody" | "held"
        std::string why;        // the leader's own sentence
        double worth = 0.0;     // copper, or the weight the leader priced it in
        std::string worthUnit;  // "copper" | "weight"
        std::string runId;
        uint64 decisionId = 0;  // the pb_journal_decision row that chose this, when there is one
    };

    void RecordLoot(Loot const& loot);

    // A reader states that it consumed decision `decisionId`. This is what makes "nobody reads this verdict" a
    // query (LEFT JOIN pb_journal_read ... WHERE reader IS NULL) instead of a reading of the code.
    void NoteRead(uint64 decisionId, std::string const& reader);

    // One raw stream line, when AiPlayerbot.Journal.Stream is on. The insurance: whatever a narrow table forgot to
    // foresee is still here, with where the bot was when it was printed.
    struct Line
    {
        std::string tag;
        std::string text;
        std::string botName;
        uint32 botLow = 0;
        uint32 mapId = 0;
        uint32 zoneId = 0;
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        // the wall clock in ms when the line was WRITTEN, 0 for "now". The log appender passes its own stamp rather
        // than letting the row take one later: the order of two things inside one second is the point.
        uint64 atMs = 0;
    };

    void RecordLine(Line const& line);

    // One measured value. `fromMs`/`toMs` are the window it was measured over and are never implied: numbers taken
    // over different windows were compared as the same number three times in one day. `scope` names what it is about
    // ("bot:Bierya", "five:3", "dungeon:wc-n", "server"). With the build in the row, "what changed between build N
    // and N+1" is a WHERE clause.
    struct Metric
    {
        std::string name;  // "dps_vs_ideal_pct", "levels_per_hour", "clean_run_share"
        double value = 0.0;
        std::string unit;  // "%", "per_hour", "sec", "copper"
        uint64 fromMs = 0;
        uint64 toMs = 0;
        std::string scope;
    };

    void RecordMetric(Metric const& metric);

    // The server start this process is. Called once lazily; the boot row is what makes windows across restarts
    // comparable at all.
    uint32 BootId();

    // [jrn20] Attaches the journal's readers (the leader's decision lines, and the log stream when it is on). Called
    // once, from Leader::Decisions().
    //
    // WHY THIS FUNCTION EXISTS AT ALL, because it looks like something a constructor should do and it must not be.
    // b495 wrote nothing - not one row, not even the boot row that is due on every single start. The cause was not
    // the config and not the appender: the module is a STATIC library, and a linker pulls an object file out of one
    // only when some symbol in it is asked for. Nothing outside src/Ai/Journal referred to the journal
    // (`grep -rn "Journal::" src | grep -v "^src/Ai/Journal/"` = 0), so Journal.obj, JournalPull.obj and
    // JournalStream.obj were compiled, archived, and then left behind. Searching the binary showed it plainly:
    // `AppenderJournal` and an unrelated control string were in worldserver.exe, `pb_journal_boot`,
    // `pb_journal_line` and `AiPlayerbot.Journal.Stream` were not there at all.
    //
    // The readers used to attach themselves from file-scope objects, and the constructor of a discarded object file
    // never runs - so the registration could not fix the discarding that removed it. An explicit call from a
    // translation unit that is certainly linked is what makes the journal real: it writes because something asks it
    // to, not because the linker was told to keep it. Attaching from here, rather than from a startup hook, is also
    // what LeaderLog is for - it owns where the leader's decision lines go.
    //
    // Does nothing when AiPlayerbot.Journal.Enable is off, and is safe to call more than once.
    void Attach();

    // Defined in JournalPull.cpp and JournalStream.cpp; called only by Attach(), which is also what drags those two
    // object files into the link.
    void AttachPullObserver();
    void AttachLogStream();

    // [jrn20] The counter a window line prints, taken from the SAME call that wrote the row - never by reading the
    // journal back. The journal is the one store of cases; a count made by a second pass over those cases would be
    // a second judge of one quantity, which is the defect this whole table exists to prevent. So: one Record() call
    // raises one counter and writes one row, and a window line and a query over the window can only agree.
    // `topic` is the decision's topic; the empty topic is the total over all of them.
    struct Count
    {
        uint64 decisions = 0;
        uint64 refused = 0;  // verdict other than "taken"
        uint64 optionsRejected = 0;
        uint64 sinceMs = 0;  // when this counter last started: a count NEVER carries an implied window
    };

    Count Counted(std::string const& topic);

    // Starts a fresh window for the counters (the printer of a window line calls this when it has printed).
    void ResetCounts();

    // Flushes what is queued (shutdown, and the `.journal flush` path). Blocks until the batch is handed to the pool.
    void Flush();
}  // namespace Journal

#endif
