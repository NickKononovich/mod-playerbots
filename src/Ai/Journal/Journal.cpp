/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Journal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "PlayerbotsDatabase.h"
#include "StringFormat.h"

namespace Journal
{
    namespace
    {
        // ---------------------------------------------------------------------------------------------------------
        // [jrn20] settings, read once (the retention is a setting, not a constant: the user picks the window)
        // ---------------------------------------------------------------------------------------------------------
        struct Settings
        {
            bool enabled = true;
            bool stream = false;
            // [jrn20] the build this server is, stamped on every row: "what changed between build N and N+1" then
            // becomes a WHERE clause instead of a guess about which window belonged to which binary. The server has
            // no build number of its own (the number comes from the "bNNN:" commit subjects), so the build agent
            // sets it when it installs a build; an empty value means "the build was not stated", which is itself
            // worth seeing in a query rather than silently filled in with something plausible.
            std::string build;
            uint32 retentionHours = 48;
            uint32 batchRows = 500;
            uint32 flushMs = 1000;
        };

        Settings const& Config()
        {
            static Settings const settings = []
            {
                Settings s;
                s.enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Journal.Enable", true, false);
                s.stream = sConfigMgr->GetOption<bool>("AiPlayerbot.Journal.Stream", false, false);
                s.build = sConfigMgr->GetOption<std::string>("AiPlayerbot.Journal.Build", "", false);
                s.retentionHours =
                    uint32(std::max(1, sConfigMgr->GetOption<int32>("AiPlayerbot.Journal.RetentionHours", 48, false)));
                s.batchRows =
                    uint32(std::max(1, sConfigMgr->GetOption<int32>("AiPlayerbot.Journal.BatchRows", 500, false)));
                s.flushMs =
                    uint32(std::max(50, sConfigMgr->GetOption<int32>("AiPlayerbot.Journal.FlushMs", 1000, false)));
                return s;
            }();
            return settings;
        }

        constexpr uint64 MS_PER_HOUR = 3600ull * 1000ull;

        // [jrn20] the window counters, raised by Record() itself - the single store of cases is also the single
        // source of the counts over it
        std::mutex g_countLock;
        std::map<std::string, Count> g_counts;
        uint64 g_countsSinceMs = 0;

        // The wall clock in milliseconds. Every row is stamped with this, not with a tick or a second: the night's
        // arguments were about the order of things inside one second.
        uint64 NowMs()
        {
            return uint64(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count());
        }

        // a text value for a quoted SQL string (backslash and quote), as elsewhere in the module
        std::string Quoted(std::string const& text, size_t max)
        {
            std::string out;
            out.reserve(text.size() + 2);
            out += '\'';
            size_t taken = 0;
            for (char c : text)
            {
                if (taken++ >= max)
                    break;
                if (c == '\\' || c == '\'')
                    out += '\\';
                if (static_cast<unsigned char>(c) < 0x20)
                    c = ' ';
                out += c;
            }
            out += '\'';
            return out;
        }

        std::string Real(double value)
        {
            if (!(value > -1e15 && value < 1e15))  // NaN and the infinities included
                return "0";
            return Acore::StringFormat("{:.4f}", value);
        }

        // ---------------------------------------------------------------------------------------------------------
        // [jrn20] the writer: the world thread only pushes a finished row, a thread of its own sends the batches
        // ---------------------------------------------------------------------------------------------------------
        struct Row
        {
            char const* table = nullptr;  // a literal, never freed
            std::string values;           // "(...)" ready for a multi-row INSERT
        };

        char const* const COLUMNS_DECISION =
            "(id, boot_id, at_ms, hour_bucket, topic, choice, verdict, why, judge, judge_name, judge_low, subject, "
            "subject_kind, subject_entry, subject_low, cost, cost_unit, tag, src, read_field, read_by, run_id, "
            "run_ms, options_total, options_rejected, fields, text, build)";
        char const* const COLUMNS_LOOT =
            "(id, boot_id, at_ms, hour_bucket, item_entry, item_name, count, quality, from_kind, from_name, "
            "from_entry, to_name, to_low, verdict, why, worth, worth_unit, run_id, decision_id, build)";
        char const* const COLUMNS_OPTION =
            "(decision_id, boot_id, at_ms, hour_bucket, idx, name, kind, reason, source_kind, cost, value, taken, "
            "fields, build)";
        char const* const COLUMNS_READ = "(decision_id, boot_id, at_ms, hour_bucket, reader)";
        char const* const COLUMNS_LINE =
            "(id, boot_id, at_ms, hour_bucket, tag, bot_low, bot_name, map_id, zone_id, pos_x, pos_y, pos_z, text, "
            "build)";
        char const* const COLUMNS_METRIC =
            "(id, boot_id, at_ms, hour_bucket, name, value, unit, from_ms, to_ms, scope, build)";

        char const* ColumnsFor(char const* table)
        {
            if (!std::strcmp(table, "pb_journal_decision"))
                return COLUMNS_DECISION;
            if (!std::strcmp(table, "pb_journal_option"))
                return COLUMNS_OPTION;
            if (!std::strcmp(table, "pb_journal_read"))
                return COLUMNS_READ;
            if (!std::strcmp(table, "pb_journal_loot"))
                return COLUMNS_LOOT;
            if (!std::strcmp(table, "pb_journal_metric"))
                return COLUMNS_METRIC;
            return COLUMNS_LINE;
        }

        // defined below; the writer thread calls it as its first act (see Writer::Loop)
        void CreateTables(uint32 bootId, uint64 nowMs);

        class Writer
        {
        public:
            static Writer& Instance()
            {
                static Writer writer;
                return writer;
            }

            void Push(char const* table, std::string&& values)
            {
                {
                    std::lock_guard<std::mutex> lock(_lock);
                    if (_queue.size() >= QUEUE_MAX)
                    {
                        ++_dropped;  // the journal never stalls the world thread: it drops and says so
                        return;
                    }
                    _queue.push_back(Row{table, std::move(values)});
                }
                _wake.notify_one();
            }

            uint64 NextId() { return (uint64(BootId()) << 32) | uint64(_seq.fetch_add(1) + 1); }

            void Flush()
            {
                std::vector<Row> batch;
                {
                    std::lock_guard<std::mutex> lock(_lock);
                    batch.swap(_queue);
                }
                Send(batch);
            }

            void Start()
            {
                std::call_once(_started, [this] { _thread = std::thread([this] { Loop(); }); });
            }

        private:
            static constexpr size_t QUEUE_MAX = 200000;

            Writer() = default;
            ~Writer()
            {
                _stop = true;
                _wake.notify_all();
                if (_thread.joinable())
                    _thread.join();
            }

            void Loop()
            {
                // The tables are made HERE, on the journal's own thread, and never on the thread that happened to
                // record the first row. CreateTables uses DirectExecute, which is synchronous, and bot AI runs on
                // map threads: a synchronous query there blocks every bot on that map. Rows recorded before this
                // returns wait in the queue, because the first Send cannot happen before the first wait below.
                CreateTables(BootId(), NowMs());
                LOG_INFO("playerbots", "[jrn20] journal on: boot {}, retention {} h, batch {} rows / {} ms", BootId(),
                         Config().retentionHours, Config().batchRows, Config().flushMs);

                uint64 lastSweepMs = 0;
                while (!_stop)
                {
                    std::vector<Row> batch;
                    {
                        std::unique_lock<std::mutex> lock(_lock);
                        _wake.wait_for(lock, std::chrono::milliseconds(Config().flushMs),
                                       [this] { return _stop || _queue.size() >= Config().batchRows; });
                        batch.swap(_queue);
                    }
                    Send(batch);

                    uint64 const now = NowMs();
                    if (now - lastSweepMs >= 10ull * 60ull * 1000ull)
                    {
                        lastSweepMs = now;
                        Sweep(now);
                    }
                }
                Flush();
            }

            // One INSERT per table per batch. PlayerbotsDatabase.Execute enqueues onto the pool's own worker, so
            // nothing here waits on MySQL either.
            void Send(std::vector<Row> const& batch)
            {
                if (batch.empty())
                    return;
                if (uint64 const dropped = _dropped.exchange(0))
                    LOG_ERROR("playerbots", "[jrn20] journal dropped {} row(s): the writer fell behind", dropped);

                char const* table = nullptr;
                std::string sql;
                size_t rows = 0;
                auto send = [&]
                {
                    if (!table || sql.empty())
                        return;
                    PlayerbotsDatabase.Execute(sql.c_str());
                    sql.clear();
                    rows = 0;
                };
                for (Row const& row : batch)
                {
                    if (table != row.table || rows >= Config().batchRows)
                    {
                        send();
                        table = row.table;
                        sql = Acore::StringFormat("INSERT INTO {} {} VALUES ", table, ColumnsFor(table));
                    }
                    else
                        sql += ',';
                    sql += row.values;
                    ++rows;
                }
                send();
            }

            // Retention: whole hour partitions are dropped. A DELETE of five million rows holds a lock on the table
            // everything is being written to; DROP PARTITION is a metadata change and touches no live partition.
            void Sweep(uint64 nowMs)
            {
                uint64 const nowHour = nowMs / MS_PER_HOUR;
                uint64 const keepFrom = nowHour > Config().retentionHours ? nowHour - Config().retentionHours : 0;
                for (char const* table : {"pb_journal_decision", "pb_journal_option", "pb_journal_read",
                                          "pb_journal_loot", "pb_journal_metric", "pb_journal_line"})
                {
                    // Which hours this table already has - asked, not assumed. After a restart the writer has no
                    // memory of the partitions it made last time, and dropping one that is not there is an error on
                    // every sweep. This is a query, but on the journal's own thread: never a map or world thread.
                    uint64 highest = 0;
                    std::vector<uint64> old;
                    if (QueryResult result = PlayerbotsDatabase.Query(
                            Acore::StringFormat("SELECT PARTITION_NAME FROM information_schema.PARTITIONS WHERE "
                                                "TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}' AND "
                                                "PARTITION_NAME IS NOT NULL AND PARTITION_NAME <> 'pfuture'",
                                                table)
                                .c_str()))
                    {
                        do
                        {
                            std::string const name = result->Fetch()[0].Get<std::string>();
                            uint64 const hour = std::strtoull(name.c_str() + 1, nullptr, 10);
                            highest = std::max(highest, hour);
                            if (hour < keepFrom)
                                old.push_back(hour);
                        } while (result->NextRow());
                    }

                    // extend forward: reorganising the empty trailing partition is cheap, and a row past the last
                    // partition would otherwise be refused (ER_NO_PARTITION_FOR_GIVEN_VALUE)
                    for (uint64 hour = std::max(highest + 1, nowHour); hour <= nowHour + 6; ++hour)
                        PlayerbotsDatabase.Execute(
                            Acore::StringFormat("ALTER TABLE {} REORGANIZE PARTITION pfuture INTO (PARTITION p{} "
                                                "VALUES LESS THAN ({}), PARTITION pfuture VALUES LESS THAN MAXVALUE)",
                                                table, hour, hour + 1)
                                .c_str());

                    // the retention itself: whole partitions go in one statement, so clearing a day of five million
                    // rows is a metadata change and not a DELETE across the table being written to
                    if (!old.empty())
                    {
                        std::string names;
                        for (uint64 hour : old)
                            names += (names.empty() ? "p" : ",p") + std::to_string(hour);
                        PlayerbotsDatabase.Execute(
                            Acore::StringFormat("ALTER TABLE {} DROP PARTITION {}", table, names).c_str());
                    }
                }
            }

            std::mutex _lock;
            std::condition_variable _wake;
            std::vector<Row> _queue;
            std::thread _thread;
            std::once_flag _started;
            std::atomic<bool> _stop{false};
            std::atomic<uint64> _dropped{0};
            std::atomic<uint32> _seq{0};
        };

        // ---------------------------------------------------------------------------------------------------------
        // [jrn20] the tables. Created the way guild_project_runs is created - idempotently, at the first row - so a
        // build needs no manual migration step; the same DDL is kept as a file for review
        // (data/sql/playerbots/updates/2026_11_15_01_pb_journal_jrn20.sql).
        // ---------------------------------------------------------------------------------------------------------
        // One catch-all partition at creation; the writer's sweep carves the hours out of it (and drops the hours
        // past the retention). The statement is therefore identical to the one in the reviewed SQL file, so the
        // table a build creates and the table the file creates cannot drift apart.
        char const* const PARTS = " PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE)";

        void CreateTables(uint32 bootId, uint64 nowMs)
        {
            std::string const parts = PARTS;

            PlayerbotsDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS pb_journal_boot ("
                "boot_id INT UNSIGNED NOT NULL, started_ms BIGINT UNSIGNED NOT NULL, "
                "started_at DATETIME(3) NOT NULL, note VARCHAR(255) NOT NULL DEFAULT '', "
                "PRIMARY KEY (boot_id), KEY started_ms (started_ms)) "
                "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                "COMMENT='[jrn20] one row per server start: the window a comparison uses survives the restart'");

            // the decision: thousands of rows an hour, so the indexes are generous - the reads are offline
            PlayerbotsDatabase.DirectExecute(
                (std::string(
                     "CREATE TABLE IF NOT EXISTS pb_journal_decision ("
                     "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, at_ms BIGINT UNSIGNED NOT NULL, "
                     "hour_bucket INT UNSIGNED NOT NULL, topic VARCHAR(48) NOT NULL, choice VARCHAR(255) NOT NULL, "
                     "verdict VARCHAR(16) NOT NULL, why VARCHAR(1024) NOT NULL, judge VARCHAR(16) NOT NULL, "
                     "judge_name VARCHAR(32) NOT NULL, judge_low INT UNSIGNED NOT NULL DEFAULT 0, "
                     "subject VARCHAR(96) NOT NULL, subject_kind VARCHAR(16) NOT NULL, "
                     "subject_entry INT UNSIGNED NOT NULL DEFAULT 0, subject_low INT UNSIGNED NOT NULL DEFAULT 0, "
                     "cost DOUBLE NOT NULL DEFAULT 0, cost_unit VARCHAR(16) NOT NULL DEFAULT '', "
                     "tag VARCHAR(24) NOT NULL DEFAULT '', src VARCHAR(96) NOT NULL DEFAULT '', "
                     "read_field VARCHAR(48) NOT NULL DEFAULT '', read_by VARCHAR(96) NOT NULL DEFAULT '', "
                     "run_id VARCHAR(48) NOT NULL DEFAULT '', run_ms INT UNSIGNED NOT NULL DEFAULT 0, "
                     "options_total SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
                     "options_rejected SMALLINT UNSIGNED NOT NULL DEFAULT 0, fields TEXT, text TEXT, "
                     "build VARCHAR(16) NOT NULL DEFAULT '', "
                     "PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY topic_at (topic, at_ms), "
                     "KEY verdict_at (verdict, topic, at_ms), KEY judge_at (judge_low, at_ms), "
                     "KEY judge_name_at (judge_name, at_ms), KEY run_at (run_id, at_ms), "
                     "KEY tag_at (tag, at_ms), KEY boot_at (boot_id, at_ms), "
                     "KEY subject_at (subject_entry, subject_low, at_ms), KEY read_by_at (read_by, at_ms)) "
                     "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                     "COMMENT='[jrn20] one choice: what was chosen, who judged, what it read, who reads the verdict'") +
                 parts)
                    .c_str());

            // the options: what was rejected and why. This is the point of the whole journal.
            PlayerbotsDatabase.DirectExecute(
                (std::string(
                     "CREATE TABLE IF NOT EXISTS pb_journal_option ("
                     "decision_id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                     "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                     "idx SMALLINT UNSIGNED NOT NULL, name VARCHAR(255) NOT NULL, kind VARCHAR(24) NOT NULL, "
                     "reason VARCHAR(1024) NOT NULL, source_kind VARCHAR(16) NOT NULL DEFAULT '', "
                     "cost DOUBLE NOT NULL DEFAULT 0, value DOUBLE NOT NULL DEFAULT 0, "
                     "taken TINYINT UNSIGNED NOT NULL DEFAULT 0, fields TEXT, "
                     "build VARCHAR(16) NOT NULL DEFAULT '', "
                     "PRIMARY KEY (decision_id, idx, hour_bucket), KEY at_ms (at_ms), "
                     "KEY source_at (source_kind, at_ms), KEY kind_at (kind, at_ms), KEY taken_at (taken, at_ms), "
                     "KEY name_at (name(64), at_ms)) "
                     "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                     "COMMENT='[jrn20] every option a decision weighed, the rejected ones with their own reason'") +
                 parts)
                    .c_str());

            // who actually read a verdict. An empty join side is the answer "nobody", and it was the answer twice.
            PlayerbotsDatabase.DirectExecute(
                (std::string("CREATE TABLE IF NOT EXISTS pb_journal_read ("
                             "decision_id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                             "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                             "reader VARCHAR(96) NOT NULL, "
                             "PRIMARY KEY (decision_id, reader, hour_bucket), KEY at_ms (at_ms), "
                             "KEY reader_at (reader, at_ms)) "
                             "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                             "COMMENT='[jrn20] a reader that consumed a verdict - no row here means nobody did'") +
                 parts)
                    .c_str());

            // the loot: what dropped, from what, who got it, and on whose verdict. A narrow table because
            // "what dropped in that run and where did it go" must not read the whole stream; the run itself stays in
            // guild_project_runs and is referred to by run_id only.
            PlayerbotsDatabase.DirectExecute(
                (std::string(
                     "CREATE TABLE IF NOT EXISTS pb_journal_loot ("
                     "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                     "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                     "item_entry INT UNSIGNED NOT NULL, item_name VARCHAR(96) NOT NULL, "
                     "count INT UNSIGNED NOT NULL DEFAULT 1, quality TINYINT UNSIGNED NOT NULL DEFAULT 0, "
                     "from_kind VARCHAR(16) NOT NULL DEFAULT '', from_name VARCHAR(96) NOT NULL DEFAULT '', "
                     "from_entry INT UNSIGNED NOT NULL DEFAULT 0, to_name VARCHAR(32) NOT NULL DEFAULT '', "
                     "to_low INT UNSIGNED NOT NULL DEFAULT 0, verdict VARCHAR(16) NOT NULL DEFAULT '', "
                     "why VARCHAR(512) NOT NULL DEFAULT '', worth DOUBLE NOT NULL DEFAULT 0, "
                     "worth_unit VARCHAR(16) NOT NULL DEFAULT '', run_id VARCHAR(48) NOT NULL DEFAULT '', "
                     "decision_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
                     "build VARCHAR(16) NOT NULL DEFAULT '', "
                     "PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY run_at (run_id, at_ms), "
                     "KEY item_at (item_entry, at_ms), KEY to_at (to_low, at_ms), "
                     "KEY verdict_at (verdict, at_ms), KEY from_at (from_entry, at_ms), "
                     "KEY decision (decision_id)) "
                     "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                     "COMMENT='[jrn20] one dropped item, where it came from, who got it and on whose verdict'") +
                 parts)
                    .c_str());

            // the raw stream. Millions of rows a day, so it keeps three indexes, not eleven: an index costs on every
            // insert, and the insert is the side that is not allowed to be expensive.
            PlayerbotsDatabase.DirectExecute(
                (std::string("CREATE TABLE IF NOT EXISTS pb_journal_line ("
                             "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                             "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                             "tag VARCHAR(24) NOT NULL DEFAULT '', bot_low INT UNSIGNED NOT NULL DEFAULT 0, "
                             "bot_name VARCHAR(32) NOT NULL DEFAULT '', map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
                             "zone_id INT UNSIGNED NOT NULL DEFAULT 0, pos_x FLOAT NOT NULL DEFAULT 0, "
                             "pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0, text TEXT NOT NULL, "
                             "build VARCHAR(16) NOT NULL DEFAULT '', "
                             "PRIMARY KEY (id, hour_bucket), KEY tag_at (tag, at_ms), KEY bot_at (bot_low, at_ms)) "
                             "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                             "COMMENT='[jrn20] the raw stream - the file stays the live stream, this is the history'") +
                 parts)
                    .c_str());

            // metric snapshots. A metric NEVER carries an implied window: from_ms / to_ms are columns, because three
            // times in one day numbers taken over different windows were compared as if they were the same number.
            // `scope` says what the value is about ("bot:Bierya", "five:3", "dungeon:wc-n", "server").
            PlayerbotsDatabase.DirectExecute(
                (std::string(
                     "CREATE TABLE IF NOT EXISTS pb_journal_metric ("
                     "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                     "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                     "name VARCHAR(64) NOT NULL, value DOUBLE NOT NULL DEFAULT 0, "
                     "unit VARCHAR(16) NOT NULL DEFAULT '', from_ms BIGINT UNSIGNED NOT NULL DEFAULT 0, "
                     "to_ms BIGINT UNSIGNED NOT NULL DEFAULT 0, scope VARCHAR(64) NOT NULL DEFAULT '', "
                     "build VARCHAR(16) NOT NULL DEFAULT '', "
                     "PRIMARY KEY (id, hour_bucket), KEY name_at (name, at_ms), "
                     "KEY name_build (name, build, at_ms), KEY scope_at (scope, at_ms)) "
                     "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                     "COMMENT='[jrn20] one measured value with its own window and build, never an implied one'") +
                 parts)
                    .c_str());

            PlayerbotsDatabase.Execute(
                Acore::StringFormat("INSERT IGNORE INTO pb_journal_boot (boot_id, started_ms, started_at, note) "
                                    "VALUES ({}, {}, FROM_UNIXTIME({}/1000), '')",
                                    bootId, nowMs, nowMs)
                    .c_str());
        }

        bool Ready()
        {
            if (!Config().enabled)
                return false;
            static bool const ready = []
            {
                Writer::Instance().Start();  // the thread makes the tables; nothing synchronous happens here
                return true;
            }();
            return ready;
        }
    }  // namespace

    char const* JudgeName(Judge judge)
    {
        switch (judge)
        {
            case Judge::Bot:
                return "bot";
            case Judge::FiveLeader:
                return "five";
            case Judge::GuildLeader:
                return "guild";
            case Judge::Harness:
                return "harness";
            default:
                return "unknown";
        }
    }

    // The boot id is the process's start second: unique per start, and it needs no round-trip to the database, so a
    // row can be written (and linked to its options) without ever waiting on MySQL.
    uint32 BootId()
    {
        static uint32 const bootId = uint32(NowMs() / 1000ull);
        return bootId;
    }

    Count Counted(std::string const& topic)
    {
        std::lock_guard<std::mutex> lock(g_countLock);
        auto const found = g_counts.find(topic);
        return found == g_counts.end() ? Count{0, 0, 0, g_countsSinceMs} : found->second;
    }

    void ResetCounts()
    {
        std::lock_guard<std::mutex> lock(g_countLock);
        g_counts.clear();
        g_countsSinceMs = NowMs();
    }

    uint64 Record(Decision const& decision)
    {
        if (!Ready())
            return 0;

        uint64 const nowMs = NowMs();
        uint64 const hour = nowMs / MS_PER_HOUR;
        uint64 const id = Writer::Instance().NextId();

        uint16 rejected = 0;
        for (Option const& option : decision.options)
            if (!option.taken)
                ++rejected;

        // the window counter is raised HERE, in the one call that writes the row: see Counted() in the header
        {
            std::lock_guard<std::mutex> lock(g_countLock);
            if (!g_countsSinceMs)
                g_countsSinceMs = nowMs;
            // the total (the empty key) and the topic's own counter. A decision with no topic raises only the
            // total, never the same counter twice: a count that counts one event twice is the defect, not the row.
            for (std::string const& key : {std::string(), decision.topic})
            {
                Count& count = g_counts[key];
                ++count.decisions;
                count.refused += decision.verdict == "taken" ? 0 : 1;
                count.optionsRejected += rejected;
                count.sinceMs = g_countsSinceMs;
                if (decision.topic.empty())
                    break;
            }
        }

        Writer::Instance().Push(
            "pb_journal_decision",
            Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{})",
                                id, BootId(), nowMs, hour, Quoted(decision.topic, 48), Quoted(decision.choice, 255),
                                Quoted(decision.verdict, 16), Quoted(decision.why, 1024),
                                Quoted(JudgeName(decision.judge), 16), Quoted(decision.judgeName, 32),
                                decision.judgeLow, Quoted(decision.subject, 96), Quoted(decision.subjectKind, 16),
                                decision.subjectEntry, decision.subjectLow, Real(decision.cost),
                                Quoted(decision.costUnit, 16), Quoted(decision.tag, 24), Quoted(decision.src, 96),
                                Quoted(decision.readField, 48), Quoted(decision.readBy, 96), Quoted(decision.runId, 48),
                                decision.runMs, uint32(decision.options.size()), uint32(rejected),
                                decision.fields.empty() ? std::string("NULL") : Quoted(decision.fields, 16000),
                                decision.text.empty() ? std::string("NULL") : Quoted(decision.text, 16000),
                                Quoted(Config().build, 16)));

        uint16 idx = 0;
        for (Option const& option : decision.options)
            Writer::Instance().Push(
                "pb_journal_option",
                Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{})", id, BootId(), nowMs, hour, idx++,
                                    Quoted(option.name, 255), Quoted(option.kind, 24), Quoted(option.reason, 1024),
                                    Quoted(option.sourceKind, 16), Real(option.cost), Real(option.value),
                                    option.taken ? 1 : 0,
                                    option.fields.empty() ? std::string("NULL") : Quoted(option.fields, 16000),
                                    Quoted(Config().build, 16)));
        return id;
    }

    void RecordLoot(Loot const& loot)
    {
        if (!Ready())
            return;
        uint64 const nowMs = NowMs();
        Writer::Instance().Push(
            "pb_journal_loot",
            Acore::StringFormat(
                "({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{})", Writer::Instance().NextId(), BootId(),
                nowMs, nowMs / MS_PER_HOUR, loot.itemEntry, Quoted(loot.itemName, 96), loot.count, loot.quality,
                Quoted(loot.fromKind, 16), Quoted(loot.fromName, 96), loot.fromEntry, Quoted(loot.toName, 32),
                loot.toLow, Quoted(loot.verdict, 16), Quoted(loot.why, 512), Real(loot.worth),
                Quoted(loot.worthUnit, 16), Quoted(loot.runId, 48), loot.decisionId, Quoted(Config().build, 16)));
    }

    void NoteRead(uint64 decisionId, std::string const& reader)
    {
        if (!decisionId || !Ready())
            return;
        uint64 const nowMs = NowMs();
        Writer::Instance().Push("pb_journal_read", Acore::StringFormat("({},{},{},{},{})", decisionId, BootId(), nowMs,
                                                                       nowMs / MS_PER_HOUR, Quoted(reader, 96)));
    }

    void RecordLine(Line const& line)
    {
        if (!Config().stream || !Ready())
            return;
        uint64 const nowMs = line.atMs ? line.atMs : NowMs();
        Writer::Instance().Push(
            "pb_journal_line",
            Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{})", Writer::Instance().NextId(), BootId(),
                                nowMs, nowMs / MS_PER_HOUR, Quoted(line.tag, 24), line.botLow, Quoted(line.botName, 32),
                                line.mapId, line.zoneId, Real(line.x), Real(line.y), Real(line.z),
                                Quoted(line.text, 16000), Quoted(Config().build, 16)));
    }

    void RecordMetric(Metric const& metric)
    {
        if (!Ready())
            return;
        uint64 const nowMs = NowMs();
        Writer::Instance().Push("pb_journal_metric",
                                Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{})", Writer::Instance().NextId(),
                                                    BootId(), nowMs, nowMs / MS_PER_HOUR, Quoted(metric.name, 64),
                                                    Real(metric.value), Quoted(metric.unit, 16), metric.fromMs,
                                                    metric.toMs, Quoted(metric.scope, 64), Quoted(Config().build, 16)));
    }

    void Flush()
    {
        if (Config().enabled)
            Writer::Instance().Flush();
    }
}  // namespace Journal
