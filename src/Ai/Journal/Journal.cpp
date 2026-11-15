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
#include <set>
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
            // [jrn20] the stream's own window, measured: 403 bytes a row at 152 967 rows an hour is 1.5 GB a day at
            // 75 bots and about 20 GB a day at a thousand - more than every narrow table together. The user allows
            // the disk and forbids cutting detail, so RETENTION is the only lever: one day for the insurance table,
            // two for the narrow ones. Shorten this, never the record.
            uint32 streamRetentionHours = 24;
            // [jrn20] how much of a judge's raw JSON an OPTION row keeps. Measured: option rows came out at 3 358
            // bytes each, the fattest in the journal, because each kept the judge's whole JSON - and most of it is
            // now in columns of its own (the actor rows). It defaults to keeping EVERYTHING: the disk is allowed
            // and detail is not what gets cut. The setting exists only so an operator can trade detail for space
            // deliberately - and if space ever presses, the retention goes first.
            uint32 optionFieldsMax = 16000;
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
                s.streamRetentionHours = uint32(
                    std::max(1, sConfigMgr->GetOption<int32>("AiPlayerbot.Journal.StreamRetentionHours", 24, false)));
                s.optionFieldsMax = uint32(
                    std::max(0, sConfigMgr->GetOption<int32>("AiPlayerbot.Journal.OptionFieldsMax", 16000, false)));
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

        // [jrn20] An Optional reaches SQL as its value or as NULL - never as 0. A mob at 0 % health is a corpse; a
        // mob whose health was not recorded is a gap. Writing 0 for "not supplied" would merge the two, and a query
        // over them would answer anyway. This is the same rule as '' against '?' on read_by, in numbers.
        std::string OrNull(Optional<uint32> const& v) { return v ? std::to_string(*v) : std::string("NULL"); }
        std::string OrNull(Optional<float> const& v) { return v ? Real(*v) : std::string("NULL"); }
        // a separate overload and not a conversion: Optional<double> does not convert to Optional<float>, and the
        // metric rows and an issue's value range are doubles
        std::string OrNull(Optional<double> const& v) { return v ? Real(*v) : std::string("NULL"); }
        std::string OrNull(Optional<bool> const& v) { return v ? std::string(*v ? "1" : "0") : std::string("NULL"); }
        std::string OrNull(Optional<std::string> const& v, size_t max)
        {
            return v ? Quoted(*v, max) : std::string("NULL");
        }

        // defined further down, beside the other child-row writers; declared here because Record() calls it
        void WriteActors(uint64 id, uint64 nowMs, uint64 hour, std::vector<Actor> const& actors);

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
            "run_ms, options_total, options_rejected, fields, text, build, method, pack_size, careful, judge_x, "
            "judge_y, judge_z, kind, route_target_entry, route_target_low, map_id, zone_id)";
        char const* const COLUMNS_LOOT =
            "(id, boot_id, at_ms, hour_bucket, item_entry, item_name, count, quality, from_kind, from_name, "
            "from_entry, to_name, to_low, verdict, why, worth, worth_unit, run_id, decision_id, build)";
        char const* const COLUMNS_OPTION =
            "(decision_id, boot_id, at_ms, hour_bucket, idx, name, kind, reason, source_kind, cost, value, taken, "
            "fields, build, fight_sec_left, run_in_sec, open_sec, opener, distance, overload, drop_pct, tank_health, "
            "union_sec, group_dps, walk_sec, walk_yd, healer, healer_walks, healer_sec, due, stop, pack_mobs, "
            "fight_mobs, union_mobs, gate_boss, gate_walker, gate_buff, gate_dead, gate_holds, gate_wakes)";
        char const* const COLUMNS_READ = "(decision_id, boot_id, at_ms, hour_bucket, reader)";
        char const* const COLUMNS_LINE =
            "(id, boot_id, at_ms, hour_bucket, tag, bot_low, bot_name, map_id, zone_id, pos_x, pos_y, pos_z, text, "
            "run_id, build)";
        char const* const COLUMNS_METRIC =
            "(id, boot_id, at_ms, hour_bucket, name, value, unit, from_ms, to_ms, scope, build, value_min, "
            "value_max, value_p95, samples, bots, sessions)";
        char const* const COLUMNS_FIGHT =
            "(id, boot_id, at_ms, hour_bucket, event, bot_name, bot_low, bot_level, role, map_id, zone_id, area_id, "
            "pos_x, pos_y, pos_z, in_dungeon, enemy_name, enemy_entry, enemy_low, enemy_level, mobs, puller_name, "
            "puller_low, pull_kind, pull_method, pull_expected, pull_cost, pull_order_id, run_id, decision_id, "
            "build)";
        char const* const COLUMNS_AGGRO =
            "(id, boot_id, at_ms, hour_bucket, bot_name, bot_low, role, bot_health_pct, bot_in_combat, bot_moving, "
            "mob_name, mob_entry, mob_low, mob_level, map_id, zone_id, pos_x, pos_y, pos_z, reason, why, tank_name, "
            "tank_low, expected, order_target_entry, order_target_low, run_id, decision_id, build)";
        char const* const COLUMNS_DEATH =
            "(id, boot_id, at_ms, hour_bucket, bot_name, bot_low, bot_level, bot_class, role, spec, map_id, zone_id, "
            "area_id, pos_x, pos_y, pos_z, in_dungeon, killer_name, killer_entry, killer_low, killer_level, "
            "killer_health_pct, health_max, damage_from_killer, attackers, judged_pull, expected, pull_expected, "
            "pull_method, context, threat, run_id, decision_id, build, "
            // [runpast88] the fight, the chase and the route search as COLUMNS - the same numbers the context
            // sentence already carried as words. NULL means the emitter did not hold the value, not a zero.
            "fight_hit_by, fight_taken, fight_damage_from_killer, fight_sec, runpast_priced, runpast_priced_high, "
            "runpast_sec, runpast_hp, runpast_taken, runpast_of_its_chase, route_age_ms, route_complete, "
            "route_through_refused, route_priced, route_refused, route_unseen, route_other_fight)";
        // [jrn40] the problems table. `checked_state` is NOT here: it is a GENERATED column the database computes,
        // so it can neither be inserted nor contradicted.
        char const* const COLUMNS_ISSUE =
            "(id, boot_id, kind, severity, repro_class, status, dedup_key, dedup_hash, why, real_when, first_seen_ms, "
            "last_seen_ms, occurrences, value_min, value_max, value_unit, bot_name, bot_low, map_id, zone_id, pos_x, "
            "pos_y, pos_z, run_id, source_table, source_id, decision_id, build, first_build, checked, checked_at_ms, "
            "checked_by, test_name, cleared_in_run, fixed_in_build, fixed_ms, merged_into, fixture_path, "
            "last_progress_ms, is_real, real_why, real_source, fixture)";
        char const* const COLUMNS_IDLE =
            "(id, boot_id, at_ms, hour_bucket, event, bot_name, bot_low, bot_level, role, map_id, zone_id, pos_x, "
            "pos_y, pos_z, in_dungeon, in_combat, window_sec, ticks, casts, cast_wait, non_cast, no_target, "
            "range_los, moving, gcd, threat, leader_hold, other, ref_unsaid, ref_useless, ref_impossible, "
            "ref_prereq, ref_failed, ref_blocked, ref_no_candidate, gap_n, gap_avg_ms, gap_max_ms, gap_over_150, "
            "gap_over_400, top_actions, stood_sec, stood_yd, doing, held_by, why, run_id, build)";
        // [jrn43] target_zone_id: the target's own zone - for `wait_behind`, the member's
        char const* const COLUMNS_STUCK =
            "(id, boot_id, at_ms, hour_bucket, kind, bot_name, bot_low, bot_level, role, map_id, zone_id, pos_x, "
            "pos_y, pos_z, target_name, target_entry, target_low, target_yd, target_level, target_zone_id, "
            "in_combat, action_running, why, run_id, build)";
        char const* const COLUMNS_ALERT =
            "(id, boot_id, at_ms, hour_bucket, kind, num, bot_name, bot_low, bot_level, map_id, zone_id, pos_x, "
            "pos_y, pos_z, loop_n, activity, no_progress_xp, no_progress_quest, same_place, last_action, reason, "
            "text, run_id, build)";
        char const* const COLUMNS_ACTOR =
            "(decision_id, boot_id, at_ms, hour_bucket, idx, side, name, low, entry, spawn_id, level, spec, role, "
            "health, health_max, health_pct, mana, mana_max, mana_pct, pos_x, pos_y, pos_z, tank_yd, target_yd, "
            "in_combat, wanders, opener_ready, doing, checks, chk_reaches_spot, chk_pull_path, chk_walk, "
            "chk_walks_into_fight, note, build)";
        char const* const COLUMNS_ROUTE =
            "(id, boot_id, at_ms, hour_bucket, event, purpose, target, target_entry, target_low, why, verdict, "
            "bot_name, bot_low, map_id, zone_id, from_x, from_y, from_z, to_x, to_y, to_z, path_yd, straight_yd, "
            "stop_count, changed_stop, changed_why, run_id, decision_id, build)";
        char const* const COLUMNS_ROUTE_STOP =
            "(route_id, boot_id, at_ms, hour_bucket, seq, pos_x, pos_y, pos_z, kind, what, entry, low, verdict, why, "
            "build)";

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
            if (!std::strcmp(table, "pb_journal_fight"))
                return COLUMNS_FIGHT;
            if (!std::strcmp(table, "pb_journal_aggro"))
                return COLUMNS_AGGRO;
            if (!std::strcmp(table, "pb_journal_death"))
                return COLUMNS_DEATH;
            if (!std::strcmp(table, "pb_journal_issue"))
                return COLUMNS_ISSUE;
            if (!std::strcmp(table, "pb_journal_idle"))
                return COLUMNS_IDLE;
            if (!std::strcmp(table, "pb_journal_stuck"))
                return COLUMNS_STUCK;
            if (!std::strcmp(table, "pb_journal_alert"))
                return COLUMNS_ALERT;
            if (!std::strcmp(table, "pb_journal_actor"))
                return COLUMNS_ACTOR;
            if (!std::strcmp(table, "pb_journal_route"))
                return COLUMNS_ROUTE;
            if (!std::strcmp(table, "pb_journal_route_stop"))
                return COLUMNS_ROUTE_STOP;
            return COLUMNS_LINE;
        }

        // [jrn40] WHAT GOES AFTER THE VALUES LIST, and only one table needs it.
        //
        // pb_journal_issue is the one table where a row is a PROBLEM and not an occurrence, so a second occurrence
        // must UPDATE the row rather than add one - that is requirement 2, and without it the table could never
        // reach zero and its count would mean nothing. The identity is `dedup_hash` (UNIQUE), and this suffix is
        // the whole merge rule, in the database rather than in a reader's head:
        //
        //   * the window grows: last_seen_ms moves, occurrences rises by one, and first_seen_ms / first_build /
        //     id / fixture_path are LEFT ALONE - "since when" must not be rewritten by a later sighting;
        //   * the number's range widens with LEAST / GREATEST, never overwritten: a staircase of fifteen "causes"
        //     was one cause across a range, and the range is the fact;
        //   * `why` takes the latest sentence, because it is what is read with the eyes;
        //   * AND THE ROW REOPENS BY ITSELF. A sighting that arrives after the row was marked fixed, on a build not
        //     older than the one it was fixed in, flips status to "returned" and ZEROES `checked` in the same
        //     statement. The build is compared as a number (b499 -> 499), not as text: "b99" against "b100" sorts
        //     wrongly as text, and a comparison that is right only while the numbers are the same width is a defect
        //     waiting for build 1000.
        //
        // A row the writer has never seen before is inserted as given; everything above only runs on a repeat.
        char const* SuffixFor(char const* table)
        {
            if (std::strcmp(table, "pb_journal_issue"))
                return "";
            return " AS inc ON DUPLICATE KEY UPDATE "
                   "boot_id = inc.boot_id, "
                   "last_seen_ms = GREATEST(pb_journal_issue.last_seen_ms, inc.last_seen_ms), "
                   "occurrences = pb_journal_issue.occurrences + 1, "
                   "value_min = LEAST(IFNULL(pb_journal_issue.value_min, inc.value_min), "
                   "                  IFNULL(inc.value_min, pb_journal_issue.value_min)), "
                   "value_max = GREATEST(IFNULL(pb_journal_issue.value_max, inc.value_max), "
                   "                     IFNULL(inc.value_max, pb_journal_issue.value_max)), "
                   "why = inc.why, build = inc.build, "
                   // [jrn43] THE SEVERITY OF A PROBLEM IS THE WORST CASE SEEN, not the latest one. It used to be
                   // `severity = inc.severity`, so a row took the severity of whichever case happened to arrive
                   // last - measured on b502: 24 of the 103 `mid` idle rows hold at least one case the emitter
                   // called `high` (a whole window in a fight with no cast at all), and the row said `mid`. That
                   // is the same mistake value_min / value_max were built to avoid: the range is the fact, and for
                   // a label the fact is the worst end of it. FIELD gives 0 to a name that is not in the list, so
                   // an unknown severity never wins over a known one and never erases it.
                   "severity = IF(FIELD(inc.severity, 'low', 'mid', 'high') > "
                   "              FIELD(pb_journal_issue.severity, 'low', 'mid', 'high'), "
                   "           inc.severity, pb_journal_issue.severity), "
                   "bot_name = inc.bot_name, bot_low = inc.bot_low, map_id = inc.map_id, zone_id = inc.zone_id, "
                   "pos_x = inc.pos_x, pos_y = inc.pos_y, pos_z = inc.pos_z, run_id = inc.run_id, "
                   "source_table = inc.source_table, source_id = inc.source_id, "
                   "checked = IF(pb_journal_issue.status = 'fixed' "
                   "             AND inc.last_seen_ms > pb_journal_issue.fixed_ms "
                   "             AND (pb_journal_issue.fixed_in_build = '' "
                   "                  OR CAST(SUBSTRING(inc.build, 2) AS UNSIGNED) "
                   "                     >= CAST(SUBSTRING(pb_journal_issue.fixed_in_build, 2) AS UNSIGNED)), "
                   "           0, pb_journal_issue.checked), "
                   "status = IF(pb_journal_issue.status = 'fixed' "
                   "            AND inc.last_seen_ms > pb_journal_issue.fixed_ms "
                   "            AND (pb_journal_issue.fixed_in_build = '' "
                   "                 OR CAST(SUBSTRING(inc.build, 2) AS UNSIGNED) "
                   "                    >= CAST(SUBSTRING(pb_journal_issue.fixed_in_build, 2) AS UNSIGNED)), "
                   "          'returned', pb_journal_issue.status)";
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
                        // [jrn31] the journal never stalls the world thread: it drops and says so. The counter is
                        // MONOTONIC and nothing resets it - the writer's error line reports the delta it has not
                        // reported yet, and the stats sampler reads the total. Two readers each zeroing one counter
                        // is a second judge of one quantity, which is the defect this table exists to prevent.
                        ++_dropped;
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

            // [jrn31] the two numbers the stats sampler puts in a metric row. The queue depth is the honest sign of
            // whether the single push mutex is being fought over at all: while it stays small and `Dropped()` stays
            // 0 there is no contention and nothing in the writer is worth changing.
            uint64 Queued()
            {
                std::lock_guard<std::mutex> lock(_lock);
                return uint64(_queue.size());
            }

            uint64 Dropped() { return _dropped.load(); }

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
                if (uint64 const total = _dropped.load(); total > _droppedSaid)
                {
                    LOG_ERROR("playerbots", "[jrn20] journal dropped {} row(s) ({} in all): the writer fell behind",
                              total - _droppedSaid, total);
                    _droppedSaid = total;
                }

                char const* table = nullptr;
                std::string sql;
                size_t rows = 0;
                auto send = [&]
                {
                    if (!table || sql.empty())
                        return;
                    // [jrn40] the merge rule of pb_journal_issue rides on the end of its own INSERT and on no
                    // other table's: SuffixFor returns "" for every table whose row is an occurrence
                    sql += SuffixFor(table);
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
                // [jrn40] pb_journal_issue IS DELIBERATELY ABSENT from this list and must stay absent. Every table
                // named here keeps 24-48 h and is cleaned by dropping whole hour partitions; the list of problems
                // lives for ever, is not partitioned at all, and a sweep of it would silently turn "we have no
                // problems" into "we forgot them". That is also why an issue row must carry a fixture path: the
                // DOMAIN record it points at does get swept, so a week later source_id reaches nothing.
                for (char const* table : {"pb_journal_decision", "pb_journal_option", "pb_journal_read",
                                          "pb_journal_loot", "pb_journal_metric", "pb_journal_actor",
                                          "pb_journal_route", "pb_journal_route_stop", "pb_journal_fight",
                                          "pb_journal_aggro", "pb_journal_death", "pb_journal_idle",
                                          "pb_journal_stuck", "pb_journal_alert", "pb_journal_line"})
                {
                    // [jrn20] the raw stream keeps its OWN, shorter window, and the number says why: measured on the
                    // live table it is 403 bytes a row at 152 967 rows an hour - 1.5 GB a day at 75 bots, and about
                    // 20 GB a day at the thousand the project is aimed at, which is more than everything else put
                    // together. The narrow tables are the ones worth two days; the insurance table is worth hours.
                    uint64 const tableKeepFrom = std::strcmp(table, "pb_journal_line")
                                                     ? keepFrom
                                                     : (nowHour > Config().streamRetentionHours
                                                            ? nowHour - Config().streamRetentionHours
                                                            : 0);
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
                            if (hour < tableKeepFrom)
                                old.push_back(hour);
                        } while (result->NextRow());
                    }

                    // [jrn33] Extend forward. The reason stated here before was WRONG, and a comment about what
                    // the code does not do costs more than a wrong number: a row past the last hour is NOT refused
                    // with ER_NO_PARTITION_FOR_GIVEN_VALUE, because `pfuture` catches everything by construction
                    // (VALUES LESS THAN MAXVALUE) - checked by inserting hour_bucket 999999, which landed in
                    // pfuture silently.
                    //
                    // The real reason is retention, and it is worse than a refusal. The sweep below only ever drops
                    // partitions named p<hour>; it excludes pfuture explicitly, and it must, because pfuture holds
                    // every hour that does not exist yet. So a row that lands in pfuture is a row RETENTION WILL
                    // NEVER REACH. While the carving ahead works (six hours of slack, swept every ten minutes) that
                    // never happens; if it ever stops - the writer thread dead, one ALTER refused, the sweep itself
                    // broken - then every new row goes to pfuture, writes keep succeeding, queries keep answering,
                    // no error appears anywhere, and the table grows without a bound. That is why the count of rows
                    // in pfuture is reported as a metric below: it is zero in a healthy server, so any non-zero
                    // value is itself the alarm.
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

                // [jrn33] THE SILENT PATH TO UNBOUNDED GROWTH, reported as a number. Rows that land in `pfuture`
                // are outside retention for ever (see the comment above), and nothing about that state looks
                // broken: the writes succeed and the queries answer. In a healthy server this is 0 for every
                // table, so ANY non-zero value is the alarm and no threshold has to be invented for it.
                //
                // THE NUMBER IS AN ESTIMATE AND MUST NOT BE READ AS A COUNT. information_schema's TABLE_ROWS is
                // InnoDB's own guess and it lies in the number - it said 322 for pb_journal_decision while the
                // real count was 9 457. What it does NOT lie about is zero against non-zero, and that is the only
                // thing read here. A real count would mean COUNT(*) over the partition on every sweep, which is
                // the expensive scan this whole retention design exists to avoid.
                //
                // The row is written by the sweep because the sweep OWNS retention: one quantity, one owner, and a
                // second reader of it would be a second judge of the same number.
                if (QueryResult result = PlayerbotsDatabase.Query(
                        "SELECT TABLE_NAME, IFNULL(TABLE_ROWS, 0) FROM information_schema.PARTITIONS WHERE "
                        "TABLE_SCHEMA = DATABASE() AND PARTITION_NAME = 'pfuture' AND TABLE_NAME LIKE "
                        "'pb\\_journal\\_%'"))
                    do
                    {
                        Field* fields = result->Fetch();
                        Metric metric;
                        metric.name = "journal_pfuture_rows";
                        metric.scope = "table:" + fields[0].Get<std::string>();
                        metric.unit = "rows";
                        metric.value = double(fields[1].Get<uint64>());
                        metric.valueMax = metric.value;
                        metric.samples = 1u;
                        // the window is never implied, and it is never 0 either: on the first sweep there is no
                        // previous one behind it, so the window is this instant - a reading of the state now, which
                        // is what it is, rather than a range stretching back to the epoch
                        metric.fromMs = lastSweepEndedMs ? lastSweepEndedMs : nowMs;
                        metric.toMs = nowMs;
                        RecordMetric(metric);
                    } while (result->NextRow());
                lastSweepEndedMs = nowMs;
            }

            std::mutex _lock;
            std::condition_variable _wake;
            std::vector<Row> _queue;
            std::thread _thread;
            std::once_flag _started;
            std::atomic<bool> _stop{false};
            std::atomic<uint64> _dropped{0};
            uint64 _droppedSaid = 0;  // how much of _dropped the error line has already reported (writer thread only)
            // [jrn33] when the previous sweep finished: the window of the pfuture metric is never implied
            uint64 lastSweepEndedMs = 0;
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

        // -------------------------------------------------------------------------------------------------------
        // [jrn32] THE SCHEMA DRIFT TRAP, and the one thing that closes it.
        //
        // `CREATE TABLE IF NOT EXISTS` on a table that already exists does NOTHING. So the moment a build adds a
        // column to the code's INSERT and the live table was made by an older build, every insert of that table
        // fails with "Unknown column" - and because the write is asynchronous, the error goes to the database log
        // and the journal simply stays EMPTY while the code is entirely correct. That is the exact failure this
        // whole journal exists to prevent: an instrument reading zero, and the zero not being about the world.
        //
        // It had already happened: pb_journal_decision had 28 columns in the live database while Record() wrote a
        // 37-field tuple (method, pack_size, careful, judge_x/y/z, kind, route_target_entry/low added by the build
        // before this one). Nothing in the file migrated them - the only two ALTERs in here were about partitions.
        //
        // What closes it: the table is brought up to the schema the CODE declares, read out of the very CREATE
        // statement the code just issued. Nothing is kept twice, so nothing can drift: the column list that creates
        // a fresh table is literally the list that is compared against an old one. It runs on the journal's own
        // thread (where DirectExecute is allowed) and is idempotent - a second start finds nothing missing.
        //
        // How to check it rather than read it: for every table, the number of columns in the live database must
        // equal the number of fields in that table's tuple (the COLUMNS_* lists above).
        //   SELECT TABLE_NAME, COUNT(*) FROM information_schema.COLUMNS
        //    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME LIKE 'pb_journal%' GROUP BY TABLE_NAME;
        // -------------------------------------------------------------------------------------------------------

        // the pieces of a CREATE's column/key list, split at the commas that are not inside parentheses
        std::vector<std::string> SplitDefinition(std::string const& body)
        {
            std::vector<std::string> pieces;
            int depth = 0;
            size_t start = 0;
            for (size_t i = 0; i <= body.size(); ++i)
            {
                if (i == body.size() || (body[i] == ',' && depth == 0))
                {
                    size_t from = start;
                    while (from < i && body[from] == ' ')
                        ++from;
                    if (i > from)
                        pieces.push_back(body.substr(from, i - from));
                    start = i + 1;
                    continue;
                }
                if (body[i] == '(')
                    ++depth;
                else if (body[i] == ')')
                    --depth;
            }
            return pieces;
        }

        // the first word of a piece: a column's name, or "PRIMARY"/"KEY"/"UNIQUE" for a key. Leading spaces are
        // skipped rather than taken as an empty word - a key piece is read again past its "KEY " to get its name,
        // and that remainder starts with a space.
        std::string FirstWord(std::string const& piece)
        {
            size_t const from = piece.find_first_not_of(' ');
            if (from == std::string::npos)
                return {};
            size_t const end = piece.find_first_of(" (", from);
            return end == std::string::npos ? piece.substr(from) : piece.substr(from, end - from);
        }

        // Issues the CREATE, then brings a table an older build left behind up to the same column and index list.
        void CreateOrExtend(std::string const& statement)
        {
            PlayerbotsDatabase.DirectExecute(statement.c_str());

            static char const* const PREFIX = "CREATE TABLE IF NOT EXISTS ";
            size_t const nameAt = statement.find(PREFIX);
            if (nameAt == std::string::npos)
                return;
            size_t const open = statement.find('(', nameAt);
            if (open == std::string::npos)
                return;
            std::string table = statement.substr(nameAt + std::strlen(PREFIX), open - nameAt - std::strlen(PREFIX));
            while (!table.empty() && (table.back() == ' ' || table.back() == '('))
                table.pop_back();

            int depth = 0;
            size_t close = std::string::npos;
            for (size_t i = open; i < statement.size(); ++i)
            {
                if (statement[i] == '(')
                    ++depth;
                else if (statement[i] == ')' && --depth == 0)
                {
                    close = i;
                    break;
                }
            }
            if (close == std::string::npos)
                return;

            // what the live table has. Asked, never assumed: the whole point of this function is that an
            // assumption about the live schema is what left the decision table empty.
            std::set<std::string> columns;
            std::set<std::string> keys;
            if (QueryResult result = PlayerbotsDatabase.Query(
                    Acore::StringFormat("SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE "
                                        "TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'",
                                        table)
                        .c_str()))
                do
                    columns.insert(result->Fetch()[0].Get<std::string>());
                while (result->NextRow());
            if (columns.empty())
                return;  // the CREATE above made it: it is already the code's schema
            if (QueryResult result = PlayerbotsDatabase.Query(
                    Acore::StringFormat("SELECT DISTINCT INDEX_NAME FROM information_schema.STATISTICS WHERE "
                                        "TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'",
                                        table)
                        .c_str()))
                do
                    keys.insert(result->Fetch()[0].Get<std::string>());
                while (result->NextRow());

            // One ALTER for everything missing: an ADD COLUMN rebuilds the table, and doing that once per build is
            // the difference between a metadata-sized pause and one per column.
            std::string adds;
            for (std::string const& piece : SplitDefinition(statement.substr(open + 1, close - open - 1)))
            {
                std::string const first = FirstWord(piece);
                if (first == "PRIMARY" || first == "UNIQUE")
                    continue;  // a primary key is never added after the fact; nothing here declares a unique one
                if (first == "KEY")
                {
                    std::string const rest = piece.substr(4);
                    if (!keys.count(FirstWord(rest)))
                        adds += (adds.empty() ? "" : ", ") + ("ADD " + piece);
                    continue;
                }
                if (!columns.count(first))
                    adds += (adds.empty() ? "" : ", ") + ("ADD COLUMN " + piece);
            }
            if (adds.empty())
                return;
            LOG_INFO("playerbots", "[jrn32] {}: the live table is older than the code - adding {}", table, adds);
            PlayerbotsDatabase.DirectExecute(
                Acore::StringFormat("ALTER TABLE {} {}", table, adds).c_str());
        }

        void CreateTables(uint32 bootId, uint64 nowMs)
        {
            std::string const parts = PARTS;

            CreateOrExtend(
                "CREATE TABLE IF NOT EXISTS pb_journal_boot ("
                "boot_id INT UNSIGNED NOT NULL, started_ms BIGINT UNSIGNED NOT NULL, "
                "started_at DATETIME(3) NOT NULL, note VARCHAR(255) NOT NULL DEFAULT '', "
                "PRIMARY KEY (boot_id), KEY started_ms (started_ms)) "
                "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                "COMMENT='[jrn20] one row per server start: the window a comparison uses survives the restart'");

            // the decision: thousands of rows an hour, so the indexes are generous - the reads are offline
            CreateOrExtend(
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
                     "method VARCHAR(24) NOT NULL DEFAULT '', pack_size SMALLINT UNSIGNED NULL, "
                     "careful TINYINT UNSIGNED NULL, judge_x FLOAT NULL, judge_y FLOAT NULL, "
                     "judge_z FLOAT NULL, "
                     "kind VARCHAR(24) NOT NULL DEFAULT '', "
                     "route_target_entry INT UNSIGNED NOT NULL DEFAULT 0, "
                     "route_target_low INT UNSIGNED NOT NULL DEFAULT 0, "
                     "map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
                     "zone_id INT UNSIGNED NOT NULL DEFAULT 0, "
                     "PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY topic_at (topic, at_ms), "
                     "KEY where_at (map_id, zone_id, at_ms), "
                     "KEY verdict_at (verdict, topic, at_ms), KEY judge_at (judge_low, at_ms), "
                     "KEY judge_name_at (judge_name, at_ms), KEY run_at (run_id, at_ms), "
                     "KEY tag_at (tag, at_ms), KEY boot_at (boot_id, at_ms), "
                     "KEY subject_at (subject_entry, subject_low, at_ms), KEY read_by_at (read_by, at_ms), "
                     "KEY method_at (method, at_ms), KEY pack_at (pack_size, at_ms), "
                     "KEY careful_at (careful, at_ms), KEY choice_at (choice(64), at_ms), "
                     "KEY judge_spot (judge_x, judge_y), KEY kind_at (kind, at_ms), "
                     "KEY build_topic (build, topic, at_ms), "
                     "KEY route_target_at (route_target_entry, route_target_low, at_ms)) "
                     "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                     "COMMENT='[jrn20] one choice: what was chosen, who judged, what it read, who reads the verdict'") +
                 parts)
                    .c_str());

            // the options: what was rejected and why. This is the point of the whole journal.
            CreateOrExtend(
                (std::string(
                     "CREATE TABLE IF NOT EXISTS pb_journal_option ("
                     "decision_id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                     "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                     "idx SMALLINT UNSIGNED NOT NULL, name VARCHAR(255) NOT NULL, kind VARCHAR(24) NOT NULL, "
                     "reason VARCHAR(1024) NOT NULL, source_kind VARCHAR(16) NOT NULL DEFAULT '', "
                     "cost DOUBLE NOT NULL DEFAULT 0, value DOUBLE NOT NULL DEFAULT 0, "
                     "taken TINYINT UNSIGNED NOT NULL DEFAULT 0, fields TEXT, "
                     "build VARCHAR(16) NOT NULL DEFAULT '', "
                     // [jrn30] the chain-pull judge's numbers, out of `fields` and into columns: these are what the
                     // questions are about ("every refusal where the healer had to walk more than N yards"), and a
                     // LIKE over the JSON was measured at 384 ms over 411 819 rows. NULL is "the judge did not
                     // weigh this one", which is not 0.
                     "fight_sec_left FLOAT NULL, run_in_sec FLOAT NULL, open_sec FLOAT NULL, "
                     "opener VARCHAR(48) NOT NULL DEFAULT '', distance FLOAT NULL, overload FLOAT NULL, "
                     "drop_pct FLOAT NULL, tank_health FLOAT NULL, union_sec FLOAT NULL, group_dps FLOAT NULL, "
                     "walk_sec FLOAT NULL, walk_yd FLOAT NULL, healer VARCHAR(32) NOT NULL DEFAULT '', "
                     "healer_walks TINYINT UNSIGNED NULL, healer_sec FLOAT NULL, due TINYINT UNSIGNED NULL, "
                     "stop SMALLINT UNSIGNED NULL, pack_mobs SMALLINT UNSIGNED NULL, "
                     "fight_mobs SMALLINT UNSIGNED NULL, union_mobs SMALLINT UNSIGNED NULL, "
                     // [test1] the judge's world gates, a column each so a replay can be asked for by gate
                     // ("every refusal where the tank did not hold what it already had"). NULL is "the judge
                     // never reached this gate" - it returns at the first one that trips - and that is NOT false.
                     "gate_boss TINYINT UNSIGNED NULL, gate_walker TINYINT UNSIGNED NULL, "
                     "gate_buff TINYINT UNSIGNED NULL, gate_dead TINYINT UNSIGNED NULL, "
                     "gate_holds TINYINT UNSIGNED NULL, gate_wakes TINYINT UNSIGNED NULL, "
                     "PRIMARY KEY (decision_id, idx, hour_bucket), KEY at_ms (at_ms), "
                     "KEY source_at (source_kind, at_ms), KEY kind_at (kind, at_ms), KEY taken_at (taken, at_ms), "
                     "KEY name_at (name(64), at_ms), KEY build_at (build, at_ms), "
                     "KEY drop_at (drop_pct, at_ms), KEY overload_at (overload, at_ms), "
                     "KEY walk_at (walk_yd, at_ms), KEY healer_walk_at (healer_walks, healer_sec, at_ms), "
                     "KEY union_at (union_mobs, at_ms), KEY fight_left_at (fight_sec_left, at_ms), "
                     "KEY tank_health_at (tank_health, at_ms), KEY opener_at (opener, at_ms), "
                     "KEY due_at (due, at_ms), KEY stop_at (stop, at_ms), "
                     "KEY gate_boss_at (gate_boss, at_ms), KEY gate_walker_at (gate_walker, at_ms), "
                     "KEY gate_buff_at (gate_buff, at_ms), KEY gate_dead_at (gate_dead, at_ms), "
                     "KEY gate_holds_at (gate_holds, at_ms), KEY gate_wakes_at (gate_wakes, at_ms)) "
                     "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                     "COMMENT='[jrn20] every option a decision weighed, the rejected ones with their own reason'") +
                 parts)
                    .c_str());

            // who actually read a verdict. An empty join side is the answer "nobody", and it was the answer twice.
            CreateOrExtend(
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
            CreateOrExtend(
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
                             "KEY build_at (build, at_ms), "
                     "KEY decision (decision_id)) "
                     "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                     "COMMENT='[jrn20] one dropped item, where it came from, who got it and on whose verdict'") +
                 parts)
                    .c_str());

            // the participants of a decision: the five's bots and the mobs the judge weighed, one row each. This is
            // what makes a record replayable - the same conditions can be put back, the judge run again, and the
            // verdict watched for turning over; a hundred of these rows are unit-test fixtures out of real fights.
            // EVERY measured number IS NULLABLE and NULL means "the emitter did not supply it": a mob at 0 % health
            // is a corpse, a mob whose health was never recorded is a gap, and a query that cannot tell those apart
            // answers anyway. Shares always sit beside their denominator (health/health_max, mana/mana_max), because
            // "11 %" with no pool behind it is not checkable - and the tank drinking at 11 % mana was that argument.
            CreateOrExtend(
                (std::string("CREATE TABLE IF NOT EXISTS pb_journal_actor ("
                             "decision_id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                             "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                             "idx SMALLINT UNSIGNED NOT NULL, side TINYINT UNSIGNED NOT NULL, "
                             "name VARCHAR(48) NOT NULL DEFAULT '', low INT UNSIGNED NOT NULL DEFAULT 0, "
                             "entry INT UNSIGNED NOT NULL DEFAULT 0, spawn_id INT UNSIGNED NOT NULL DEFAULT 0, "
                             "level SMALLINT UNSIGNED NULL, spec VARCHAR(24) NULL, role VARCHAR(8) NULL, "
                             "health INT UNSIGNED NULL, health_max INT UNSIGNED NULL, health_pct FLOAT NULL, "
                             "mana INT UNSIGNED NULL, mana_max INT UNSIGNED NULL, mana_pct FLOAT NULL, "
                             "pos_x FLOAT NULL, pos_y FLOAT NULL, pos_z FLOAT NULL, tank_yd FLOAT NULL, "
                             "target_yd FLOAT NULL, in_combat TINYINT UNSIGNED NULL, wanders TINYINT UNSIGNED NULL, "
                             "opener_ready TINYINT UNSIGNED NULL, doing VARCHAR(96) NULL, checks VARCHAR(255) NULL, "
                             "chk_reaches_spot TINYINT UNSIGNED NULL, chk_pull_path TINYINT UNSIGNED NULL, "
                             "chk_walk TINYINT UNSIGNED NULL, chk_walks_into_fight TINYINT UNSIGNED NULL, "
                             "note VARCHAR(48) NOT NULL DEFAULT '', build VARCHAR(16) NOT NULL DEFAULT '', "
                             "PRIMARY KEY (decision_id, idx, hour_bucket), KEY at_ms (at_ms), "
                             "KEY who_at (low, at_ms), KEY entry_at (entry, at_ms), KEY spawn_at (spawn_id, at_ms), "
                             "KEY side_health (side, health_pct, at_ms), KEY side_mana (side, mana_pct, at_ms), "
                             "KEY role_at (role, at_ms), KEY level_at (level, at_ms), "
                             "KEY checks_at (chk_reaches_spot, chk_pull_path, chk_walk, at_ms), "
                             "KEY build_at (build, at_ms)) "
                             "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                             "COMMENT='[jrn20] every participant of a decision and its state - NULL means not given'")
                 + parts)
                    .c_str());

            // a route, recorded as EVENTS (planned / changed / leg_start / leg_end) and not as sampled positions: a
            // coordinate per tick is the stream again, only dearer, and it still cannot say why the bot went there.
            // Lengths are kept BOTH along the path and straight, because a detour is the ratio of the two and
            // neither number alone shows it.
            CreateOrExtend(
                (std::string("CREATE TABLE IF NOT EXISTS pb_journal_route ("
                             "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                             "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                             "event VARCHAR(12) NOT NULL, purpose VARCHAR(48) NOT NULL DEFAULT '', "
                             "target VARCHAR(96) NOT NULL DEFAULT '', target_entry INT UNSIGNED NOT NULL DEFAULT 0, "
                             "target_low INT UNSIGNED NOT NULL DEFAULT 0, why VARCHAR(512) NOT NULL DEFAULT '', "
                             "verdict VARCHAR(16) NOT NULL DEFAULT '', bot_name VARCHAR(32) NOT NULL DEFAULT '', "
                             "bot_low INT UNSIGNED NOT NULL DEFAULT 0, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
                             "zone_id INT UNSIGNED NOT NULL DEFAULT 0, from_x FLOAT NOT NULL DEFAULT 0, "
                             "from_y FLOAT NOT NULL DEFAULT 0, from_z FLOAT NOT NULL DEFAULT 0, "
                             "to_x FLOAT NOT NULL DEFAULT 0, to_y FLOAT NOT NULL DEFAULT 0, "
                             "to_z FLOAT NOT NULL DEFAULT 0, path_yd FLOAT NOT NULL DEFAULT 0, "
                             "straight_yd FLOAT NOT NULL DEFAULT 0, stop_count SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
                             "changed_stop SMALLINT UNSIGNED NULL, changed_why VARCHAR(255) NOT NULL DEFAULT '', "
                             "run_id VARCHAR(48) NOT NULL DEFAULT '', "
                             "decision_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
                             "build VARCHAR(16) NOT NULL DEFAULT '', "
                             "PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY bot_at (bot_low, at_ms), "
                             "KEY event_at (event, at_ms), KEY run_at (run_id, at_ms), "
                             "KEY purpose_at (purpose, at_ms), KEY verdict_at (verdict, at_ms), "
                             "KEY target_at (target_entry, target_low, at_ms), KEY decision (decision_id), "
                             "KEY build_at (build, at_ms)) "
                             "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                             "COMMENT='[jrn20] a route as events: planned, changed, leg started, leg ended'")
                 + parts)
                    .c_str());

            CreateOrExtend(
                (std::string("CREATE TABLE IF NOT EXISTS pb_journal_route_stop ("
                             "route_id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                             "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                             "seq SMALLINT UNSIGNED NOT NULL, pos_x FLOAT NOT NULL DEFAULT 0, "
                             "pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0, "
                             "kind VARCHAR(16) NOT NULL DEFAULT '', what VARCHAR(96) NOT NULL DEFAULT '', "
                             "entry INT UNSIGNED NOT NULL DEFAULT 0, low INT UNSIGNED NOT NULL DEFAULT 0, "
                             "verdict VARCHAR(16) NOT NULL DEFAULT '', why VARCHAR(512) NOT NULL DEFAULT '', "
                             "build VARCHAR(16) NOT NULL DEFAULT '', "
                             "PRIMARY KEY (route_id, seq, hour_bucket), KEY at_ms (at_ms), "
                             "KEY kind_at (kind, at_ms), KEY verdict_at (verdict, at_ms), "
                             "KEY what_at (entry, low, at_ms)) "
                             "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                             "COMMENT='[jrn20] one stop of a route: where, what is there, and its own verdict'")
                 + parts)
                    .c_str());

            // the raw stream. Millions of rows a day, so it keeps three indexes, not eleven: an index costs on every
            // insert, and the insert is the side that is not allowed to be expensive.
            CreateOrExtend(
                (std::string("CREATE TABLE IF NOT EXISTS pb_journal_line ("
                             "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                             "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                             "tag VARCHAR(32) NOT NULL DEFAULT '', bot_low INT UNSIGNED NOT NULL DEFAULT 0, "
                             "bot_name VARCHAR(32) NOT NULL DEFAULT '', map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
                             "zone_id INT UNSIGNED NOT NULL DEFAULT 0, pos_x FLOAT NOT NULL DEFAULT 0, "
                             "pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0, text TEXT NOT NULL, "
                             "run_id VARCHAR(48) NOT NULL DEFAULT '', "
                             "build VARCHAR(16) NOT NULL DEFAULT '', "
                             "PRIMARY KEY (id, hour_bucket), KEY tag_at (tag, at_ms), KEY bot_at (bot_low, at_ms), "
                             "KEY run_at (run_id, at_ms), "
                             "KEY build_at (build, at_ms)) "
                             "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                             "COMMENT='[jrn20] the raw stream - the file stays the live stream, this is the history'") +
                 parts)
                    .c_str());

            // metric snapshots. A metric NEVER carries an implied window: from_ms / to_ms are columns, because three
            // times in one day numbers taken over different windows were compared as if they were the same number.
            // `scope` says what the value is about ("bot:Bierya", "five:3", "dungeon:wc-n", "server").
            CreateOrExtend(
                (std::string(
                     "CREATE TABLE IF NOT EXISTS pb_journal_metric ("
                     "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                     "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                     "name VARCHAR(64) NOT NULL, value DOUBLE NOT NULL DEFAULT 0, "
                     "unit VARCHAR(16) NOT NULL DEFAULT '', from_ms BIGINT UNSIGNED NOT NULL DEFAULT 0, "
                     "to_ms BIGINT UNSIGNED NOT NULL DEFAULT 0, scope VARCHAR(64) NOT NULL DEFAULT '', "
                     "build VARCHAR(16) NOT NULL DEFAULT '', "
                     // [jrn31] THE MEAN HIDES THE SPIKE: a 50 ms average over a window holding one 900 ms tick is
                     // 50 ms, and 900 ms is what the bots stand on. The distribution is columns, with indexes, so
                     // "every window whose worst sample went over 200 ms" is a WHERE clause; and `bots` / `sessions`
                     // are in the row because a tick time at 75 bots and one at 300 are not the same number.
                     "value_min DOUBLE NULL, value_max DOUBLE NULL, value_p95 DOUBLE NULL, "
                     "samples INT UNSIGNED NULL, bots INT UNSIGNED NULL, sessions INT UNSIGNED NULL, "
                     "PRIMARY KEY (id, hour_bucket), KEY name_at (name, at_ms), "
                     "KEY name_build (name, build, at_ms), KEY scope_at (scope, at_ms), "
                     "KEY name_max (name, value_max, at_ms), KEY name_p95 (name, value_p95, at_ms), "
                     "KEY name_value (name, value, at_ms), "
                     "KEY name_bots (name, bots, at_ms), KEY scope_name (scope, name, at_ms)) "
                     "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                     "COMMENT='[jrn20] one measured value with its own window and build, never an implied one'") +
                 parts)
                    .c_str());

            // [jrn30] the start of a fight. 9 467 fights in one 2.69 h window had an end row in
            // playerbots_fight_history and NOT ONE beginning: the start was only visible through a sampler that ran
            // every 22 s and caught about half the bots. This row is written at the combat-state transition the core
            // announces (OnPlayerEnterCombat), so it is one row per fight per bot with no clock of its own.
            CreateOrExtend(
                (std::string("CREATE TABLE IF NOT EXISTS pb_journal_fight ("
                             "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                             "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                             "event VARCHAR(12) NOT NULL DEFAULT '', bot_name VARCHAR(32) NOT NULL DEFAULT '', "
                             "bot_low INT UNSIGNED NOT NULL DEFAULT 0, bot_level SMALLINT UNSIGNED NULL, "
                             "role VARCHAR(8) NULL, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
                             "zone_id INT UNSIGNED NOT NULL DEFAULT 0, area_id INT UNSIGNED NOT NULL DEFAULT 0, "
                             "pos_x FLOAT NOT NULL DEFAULT 0, pos_y FLOAT NOT NULL DEFAULT 0, "
                             "pos_z FLOAT NOT NULL DEFAULT 0, in_dungeon TINYINT UNSIGNED NOT NULL DEFAULT 0, "
                             "enemy_name VARCHAR(48) NOT NULL DEFAULT '', "
                             "enemy_entry INT UNSIGNED NOT NULL DEFAULT 0, "
                             "enemy_low INT UNSIGNED NOT NULL DEFAULT 0, enemy_level SMALLINT UNSIGNED NULL, "
                             "mobs SMALLINT UNSIGNED NULL, puller_name VARCHAR(32) NOT NULL DEFAULT '', "
                             "puller_low INT UNSIGNED NOT NULL DEFAULT 0, "
                             "pull_kind VARCHAR(12) NOT NULL DEFAULT '', "
                             "pull_method VARCHAR(16) NOT NULL DEFAULT '', pull_expected SMALLINT UNSIGNED NULL, "
                             "pull_cost DOUBLE NOT NULL DEFAULT 0, pull_order_id INT UNSIGNED NOT NULL DEFAULT 0, "
                             "run_id VARCHAR(48) NOT NULL DEFAULT '', "
                             "decision_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
                             "build VARCHAR(16) NOT NULL DEFAULT '', "
                             "PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY bot_at (bot_low, at_ms), "
                             "KEY enemy_at (enemy_entry, enemy_low, at_ms), KEY where_at (map_id, zone_id, at_ms), "
                             "KEY event_at (event, at_ms), KEY pull_kind_at (pull_kind, at_ms), "
                             "KEY puller_at (puller_low, at_ms), KEY mobs_at (mobs, at_ms), "
                             "KEY run_at (run_id, at_ms), KEY build_at (build, at_ms), "
                             "KEY decision (decision_id)) "
                             "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                             "COMMENT='[jrn30] a fight BEGINS: who, where, against whom, how many, pulled by whom'")
                 + parts)
                    .c_str());

            // [jrn30] aggro gained, and the reason code is the point. `reason` is a column with an index because
            // "an error of the estimate" is the one case the user asked for, and while the reason lived in a
            // sentence that case was not merely unrecorded but inexpressible.
            CreateOrExtend(
                (std::string("CREATE TABLE IF NOT EXISTS pb_journal_aggro ("
                             "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                             "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                             "bot_name VARCHAR(32) NOT NULL DEFAULT '', bot_low INT UNSIGNED NOT NULL DEFAULT 0, "
                             "role VARCHAR(8) NULL, bot_health_pct FLOAT NULL, bot_in_combat TINYINT UNSIGNED NULL, "
                             "bot_moving TINYINT UNSIGNED NULL, mob_name VARCHAR(48) NOT NULL DEFAULT '', "
                             "mob_entry INT UNSIGNED NOT NULL DEFAULT 0, mob_low INT UNSIGNED NOT NULL DEFAULT 0, "
                             "mob_level SMALLINT UNSIGNED NULL, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
                             "zone_id INT UNSIGNED NOT NULL DEFAULT 0, pos_x FLOAT NOT NULL DEFAULT 0, "
                             "pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0, "
                             "reason VARCHAR(16) NOT NULL DEFAULT '', why VARCHAR(512) NOT NULL DEFAULT '', "
                             "tank_name VARCHAR(32) NOT NULL DEFAULT '', tank_low INT UNSIGNED NOT NULL DEFAULT 0, "
                             "expected TINYINT UNSIGNED NULL, order_target_entry INT UNSIGNED NOT NULL DEFAULT 0, "
                             "order_target_low INT UNSIGNED NOT NULL DEFAULT 0, "
                             "run_id VARCHAR(48) NOT NULL DEFAULT '', "
                             "decision_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
                             "build VARCHAR(16) NOT NULL DEFAULT '', "
                             "PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), "
                             "KEY reason_at (reason, at_ms), KEY bot_at (bot_low, at_ms), "
                             "KEY mob_at (mob_entry, mob_low, at_ms), KEY where_at (map_id, zone_id, at_ms), "
                             "KEY expected_at (expected, at_ms), KEY tank_at (tank_low, at_ms), "
                             "KEY reason_bot (reason, bot_low, at_ms), KEY run_at (run_id, at_ms), "
                             "KEY build_at (build, at_ms), KEY decision (decision_id)) "
                             "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                             "COMMENT='[jrn30] aggro gained, with the REASON as a code: came/path/estimate/add'")
                 + parts)
                    .c_str());

            // [jrn30] a REAL death, in a table of its own so it cannot be counted together with a death prevented.
            // Measured: 20 502 stream lines in one window carried a "[death*" tag and 17 of them were deaths; the
            // rest are the prevention domain saying a death did NOT happen. A count by tag overstated by ~1 200x.
            CreateOrExtend(
                (std::string("CREATE TABLE IF NOT EXISTS pb_journal_death ("
                             "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                             "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                             "bot_name VARCHAR(32) NOT NULL DEFAULT '', bot_low INT UNSIGNED NOT NULL DEFAULT 0, "
                             "bot_level SMALLINT UNSIGNED NULL, bot_class SMALLINT UNSIGNED NULL, "
                             "role VARCHAR(8) NULL, spec VARCHAR(24) NULL, "
                             "map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, zone_id INT UNSIGNED NOT NULL DEFAULT 0, "
                             "area_id INT UNSIGNED NOT NULL DEFAULT 0, pos_x FLOAT NOT NULL DEFAULT 0, "
                             "pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0, "
                             "in_dungeon TINYINT UNSIGNED NOT NULL DEFAULT 0, "
                             "killer_name VARCHAR(48) NOT NULL DEFAULT '', "
                             "killer_entry INT UNSIGNED NOT NULL DEFAULT 0, "
                             "killer_low INT UNSIGNED NOT NULL DEFAULT 0, killer_level SMALLINT UNSIGNED NULL, "
                             "killer_health_pct FLOAT NULL, health_max INT UNSIGNED NULL, "
                             "damage_from_killer INT UNSIGNED NULL, attackers SMALLINT UNSIGNED NULL, "
                             // [runpast88] the fight, the chase and the route search behind the death, each
                             // quantity its own column. They stood in `context` as words, and nothing here is
                             // filtered by text. NULL = the emitter did not hold it, and that is not a zero.
                             "fight_hit_by SMALLINT UNSIGNED NULL, fight_taken INT UNSIGNED NULL, "
                             "fight_damage_from_killer INT UNSIGNED NULL, fight_sec FLOAT NULL, "
                             "runpast_priced FLOAT NULL, runpast_priced_high FLOAT NULL, "
                             "runpast_sec FLOAT NULL, runpast_hp FLOAT NULL, "
                             "runpast_taken INT UNSIGNED NULL, runpast_of_its_chase TINYINT UNSIGNED NULL, "
                             "route_age_ms INT UNSIGNED NULL, route_complete TINYINT UNSIGNED NULL, "
                             "route_through_refused TINYINT UNSIGNED NULL, route_priced SMALLINT UNSIGNED NULL, "
                             "route_refused SMALLINT UNSIGNED NULL, route_unseen SMALLINT UNSIGNED NULL, "
                             "route_other_fight SMALLINT UNSIGNED NULL, "
                             "judged_pull TINYINT UNSIGNED NULL, expected VARCHAR(512) NOT NULL DEFAULT '', "
                             "pull_expected SMALLINT UNSIGNED NULL, pull_method VARCHAR(16) NOT NULL DEFAULT '', "
                             "context VARCHAR(1024) NOT NULL DEFAULT '', threat VARCHAR(1024) NOT NULL DEFAULT '', "
                             "run_id VARCHAR(48) NOT NULL DEFAULT '', "
                             "decision_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
                             "build VARCHAR(16) NOT NULL DEFAULT '', "
                             "PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY bot_at (bot_low, at_ms), "
                             "KEY killer_at (killer_entry, killer_low, at_ms), "
                             "KEY where_at (map_id, zone_id, at_ms), KEY judged_at (judged_pull, at_ms), "
                             "KEY level_at (bot_level, at_ms), KEY dungeon_at (in_dungeon, at_ms), "
                             "KEY run_at (run_id, at_ms), KEY build_at (build, at_ms), "
                             // [runpast88] one index per question that gets asked of a death: more hitters than
                             // the search had priced, a chase priced below what arrived, a mob never seen
                             "KEY fight_hit_by_at (fight_hit_by, at_ms), "
                             "KEY runpast_priced_high_at (runpast_priced_high, at_ms), "
                             "KEY runpast_taken_at (runpast_taken, at_ms), "
                             "KEY route_unseen_at (route_unseen, at_ms), "
                             "KEY route_priced_at (route_priced, at_ms), "
                             "KEY route_age_ms_at (route_age_ms, at_ms), "
                             "KEY decision (decision_id)) "
                             "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                             "COMMENT='[jrn30] a real death with the judges inputs - prevention is never written here'")
                 + parts)
                    .c_str());

            // [jrn40] THE LIST OF PROBLEMS, and the ONE table of the journal that is NOT PARTITIONED and that the
            // sweep does not name. Every other table here lives 24-48 h and is cleaned by dropping hour partitions;
            // a list of problems that evaporates every two days is not a list of problems. It is also the only
            // table whose row is a PROBLEM rather than an occurrence: `dedup_hash` is UNIQUE and a repeat updates
            // the row (see SuffixFor), so the number of rows is the number of problems and can be driven to zero.
            //
            // `checked_state` is GENERATED: the database computes it from what is actually in the row, so the tick
            // cannot be taken on trust. "unconfirmed" is a value a query can count, not a comment somebody has to
            // read - and that is the difference between a list of solved problems and a list of claims.
            CreateOrExtend(
                "CREATE TABLE IF NOT EXISTS pb_journal_issue ("
                "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                "kind VARCHAR(32) NOT NULL DEFAULT '', severity VARCHAR(8) NOT NULL DEFAULT '', "
                "repro_class VARCHAR(16) NOT NULL DEFAULT '', status VARCHAR(12) NOT NULL DEFAULT 'open', "
                "dedup_key VARCHAR(512) NOT NULL DEFAULT '', dedup_hash CHAR(32) NOT NULL, "
                "why VARCHAR(1024) NOT NULL DEFAULT '', real_when VARCHAR(255) NOT NULL DEFAULT '', "
                "first_seen_ms BIGINT UNSIGNED NOT NULL, last_seen_ms BIGINT UNSIGNED NOT NULL, "
                "occurrences BIGINT UNSIGNED NOT NULL DEFAULT 1, value_min DOUBLE NULL, value_max DOUBLE NULL, "
                "value_unit VARCHAR(16) NOT NULL DEFAULT '', bot_name VARCHAR(32) NOT NULL DEFAULT '', "
                "bot_low INT UNSIGNED NOT NULL DEFAULT 0, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
                "zone_id INT UNSIGNED NOT NULL DEFAULT 0, pos_x FLOAT NOT NULL DEFAULT 0, "
                "pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0, "
                "run_id VARCHAR(48) NOT NULL DEFAULT '', source_table VARCHAR(32) NOT NULL DEFAULT '', "
                "source_id BIGINT UNSIGNED NOT NULL DEFAULT 0, decision_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
                "build VARCHAR(16) NOT NULL DEFAULT '', first_build VARCHAR(16) NOT NULL DEFAULT '', "
                "checked TINYINT UNSIGNED NOT NULL DEFAULT 0, checked_at_ms BIGINT UNSIGNED NOT NULL DEFAULT 0, "
                "checked_by VARCHAR(48) NOT NULL DEFAULT '', test_name VARCHAR(128) NOT NULL DEFAULT '', "
                "cleared_in_run VARCHAR(48) NOT NULL DEFAULT '', fixed_in_build VARCHAR(16) NOT NULL DEFAULT '', "
                "fixed_ms BIGINT UNSIGNED NOT NULL DEFAULT 0, merged_into BIGINT UNSIGNED NOT NULL DEFAULT 0, "
                "fixture_path VARCHAR(255) NOT NULL DEFAULT '', "
                "last_progress_ms BIGINT UNSIGNED NOT NULL DEFAULT 0, "
                "is_real VARCHAR(8) NOT NULL DEFAULT 'unknown', real_why VARCHAR(512) NOT NULL DEFAULT '', "
                "real_source VARCHAR(8) NOT NULL DEFAULT 'emitter', fixture TEXT, "
                "checked_state VARCHAR(12) GENERATED ALWAYS AS (CASE WHEN checked = 0 THEN 'unchecked' "
                "  WHEN repro_class = 'decision' AND test_name <> '' THEN 'by_test' "
                "  WHEN repro_class = 'not_offline' AND cleared_in_run <> '' AND fixed_in_build <> '' THEN 'by_run' "
                "  ELSE 'unconfirmed' END) STORED, "
                "PRIMARY KEY (id), UNIQUE KEY dedup (dedup_hash), "
                "KEY kind_last (kind, last_seen_ms), KEY status_kind (status, kind, last_seen_ms), "
                "KEY repro_status (repro_class, status, last_seen_ms), KEY checked_state_at (checked_state, "
                "last_seen_ms), KEY severity_at (severity, last_seen_ms), KEY bot_at (bot_low, last_seen_ms), "
                "KEY where_at (map_id, zone_id, last_seen_ms), KEY build_at (build, last_seen_ms), "
                "KEY first_at (first_seen_ms), KEY occurrences_at (occurrences, last_seen_ms), "
                "KEY source (source_table, source_id), KEY merged (merged_into), "
                "KEY run_at (run_id, last_seen_ms), KEY test (test_name), "
                "KEY real_at (is_real, kind, last_seen_ms), KEY real_source_at (real_source, last_seen_ms)) "
                "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                "COMMENT='[jrn40] one row per PROBLEM, driven to zero rows - NOT under retention, never swept'");

            // [jrn40] the idle domain's own record: the 15 s diagnostics window and the standing watch. The inputs
            // live HERE and not in pb_journal_issue, which only points at them - a thin uniform pointer cannot also
            // be the union of every kind's fields.
            CreateOrExtend(
                (std::string("CREATE TABLE IF NOT EXISTS pb_journal_idle ("
                             "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                             "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                             "event VARCHAR(12) NOT NULL DEFAULT '', bot_name VARCHAR(32) NOT NULL DEFAULT '', "
                             "bot_low INT UNSIGNED NOT NULL DEFAULT 0, bot_level SMALLINT UNSIGNED NULL, "
                             "role VARCHAR(8) NULL, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
                             "zone_id INT UNSIGNED NOT NULL DEFAULT 0, pos_x FLOAT NOT NULL DEFAULT 0, "
                             "pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0, "
                             "in_dungeon TINYINT UNSIGNED NULL, in_combat TINYINT UNSIGNED NULL, "
                             "window_sec FLOAT NULL, ticks SMALLINT UNSIGNED NULL, casts SMALLINT UNSIGNED NULL, "
                             "cast_wait SMALLINT UNSIGNED NULL, non_cast SMALLINT UNSIGNED NULL, "
                             "no_target SMALLINT UNSIGNED NULL, range_los SMALLINT UNSIGNED NULL, "
                             "moving SMALLINT UNSIGNED NULL, gcd SMALLINT UNSIGNED NULL, "
                             "threat SMALLINT UNSIGNED NULL, leader_hold SMALLINT UNSIGNED NULL, "
                             "other SMALLINT UNSIGNED NULL, ref_unsaid SMALLINT UNSIGNED NULL, "
                             "ref_useless SMALLINT UNSIGNED NULL, ref_impossible SMALLINT UNSIGNED NULL, "
                             "ref_prereq SMALLINT UNSIGNED NULL, ref_failed SMALLINT UNSIGNED NULL, "
                             "ref_blocked SMALLINT UNSIGNED NULL, ref_no_candidate SMALLINT UNSIGNED NULL, "
                             "gap_n SMALLINT UNSIGNED NULL, gap_avg_ms INT UNSIGNED NULL, "
                             "gap_max_ms INT UNSIGNED NULL, gap_over_150 SMALLINT UNSIGNED NULL, "
                             "gap_over_400 SMALLINT UNSIGNED NULL, top_actions VARCHAR(255) NOT NULL DEFAULT '', "
                             "stood_sec FLOAT NULL, stood_yd FLOAT NULL, doing VARCHAR(96) NOT NULL DEFAULT '', "
                             "held_by VARCHAR(48) NOT NULL DEFAULT '', "
                             "why VARCHAR(512) NOT NULL DEFAULT '', run_id VARCHAR(48) NOT NULL DEFAULT '', "
                             "build VARCHAR(16) NOT NULL DEFAULT '', "
                             "PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY event_at (event, at_ms), "
                             "KEY bot_at (bot_low, at_ms), KEY role_at (role, at_ms), "
                             "KEY where_at (map_id, zone_id, at_ms), KEY other_at (other, at_ms), "
                             "KEY stood_at (stood_sec, at_ms), KEY gap_at (gap_max_ms, at_ms), "
                             "KEY held_at (held_by, at_ms), "
                             "KEY casts_at (casts, at_ms), KEY build_at (build, at_ms), "
                             "KEY run_at (run_id, at_ms)) "
                             "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                             "COMMENT='[jrn40] idleness measured by fact: the diagnostics window and the standing "
                             "watch, for EVERY role'")
                 + parts)
                    .c_str());

            // [jrn40] something is held or awaited and nothing progresses. Four emitters, one shape; `kind` names
            // which of them wrote the row, and each writes only its own.
            CreateOrExtend(
                (std::string("CREATE TABLE IF NOT EXISTS pb_journal_stuck ("
                             "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                             "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                             "kind VARCHAR(16) NOT NULL DEFAULT '', bot_name VARCHAR(32) NOT NULL DEFAULT '', "
                             "bot_low INT UNSIGNED NOT NULL DEFAULT 0, bot_level SMALLINT UNSIGNED NULL, "
                             "role VARCHAR(8) NULL, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
                             "zone_id INT UNSIGNED NOT NULL DEFAULT 0, pos_x FLOAT NOT NULL DEFAULT 0, "
                             "pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0, "
                             "target_name VARCHAR(48) NOT NULL DEFAULT '', "
                             "target_entry INT UNSIGNED NOT NULL DEFAULT 0, "
                             "target_low INT UNSIGNED NOT NULL DEFAULT 0, target_yd FLOAT NULL, "
                             // [jrn43] the target's zone, the cause field of `wait_behind`'s key
                             "target_level SMALLINT UNSIGNED NULL, target_zone_id INT UNSIGNED NULL, "
                             "in_combat TINYINT UNSIGNED NULL, "
                             "action_running VARCHAR(96) NOT NULL DEFAULT '', why VARCHAR(512) NOT NULL DEFAULT '', "
                             "run_id VARCHAR(48) NOT NULL DEFAULT '', build VARCHAR(16) NOT NULL DEFAULT '', "
                             "PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY kind_at (kind, at_ms), "
                             "KEY bot_at (bot_low, at_ms), KEY target_at (target_entry, target_low, at_ms), "
                             "KEY where_at (map_id, zone_id, at_ms), KEY yd_at (target_yd, at_ms), "
                             "KEY role_at (role, at_ms), KEY build_at (build, at_ms), KEY run_at (run_id, at_ms), "
                             "KEY target_zone_at (target_zone_id, at_ms)) "
                             "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                             "COMMENT='[jrn40] held or awaited and not progressing: target, distance, what ran'")
                 + parts)
                    .c_str());

            // [jrn40] one alert. Written ONLY from GuildCoordinator::Alert, the funnel every alert already passes:
            // recording it there and again at the site that raised it would be the double count this journal exists
            // to prevent.
            CreateOrExtend(
                (std::string("CREATE TABLE IF NOT EXISTS pb_journal_alert ("
                             "id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, "
                             "at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, "
                             "kind VARCHAR(24) NOT NULL DEFAULT '', num INT UNSIGNED NULL, "
                             "bot_name VARCHAR(32) NOT NULL DEFAULT '', bot_low INT UNSIGNED NOT NULL DEFAULT 0, "
                             "bot_level SMALLINT UNSIGNED NULL, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
                             "zone_id INT UNSIGNED NOT NULL DEFAULT 0, pos_x FLOAT NOT NULL DEFAULT 0, "
                             "pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0, "
                             "loop_n INT UNSIGNED NULL, activity VARCHAR(32) NOT NULL DEFAULT '', "
                             "no_progress_xp TINYINT UNSIGNED NULL, no_progress_quest TINYINT UNSIGNED NULL, "
                             "same_place TINYINT UNSIGNED NULL, last_action VARCHAR(512) NOT NULL DEFAULT '', "
                             "reason VARCHAR(512) NOT NULL DEFAULT '', text TEXT, "
                             "run_id VARCHAR(48) NOT NULL DEFAULT '', build VARCHAR(16) NOT NULL DEFAULT '', "
                             "PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY kind_at (kind, at_ms), "
                             "KEY bot_at (bot_low, at_ms), KEY activity_at (activity, at_ms), "
                             "KEY where_at (map_id, zone_id, at_ms), KEY loop_at (loop_n, at_ms), "
                             "KEY build_at (build, at_ms), KEY run_at (run_id, at_ms)) "
                             "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                             "COMMENT='[jrn40] one alert, from the single funnel every alert already passes'")
                 + parts)
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

    // [jrn20] Attach reads NO configuration, and that is deliberate - it is the defence in depth behind the fix for
    // the b497 crash. The config cannot be asked anything during static initialisation: ConfigMgr keeps
    // _configOptions, _envVarCache, _criticalConfigOptions, _filename and _configLock as namespace-scope globals in
    // Config.cpp, so a query from another translation unit's file-scope initialiser reads unconstructed containers.
    // There is nothing to be tolerant with: it is an access violation and not an exception, so it cannot be caught,
    // and even GetFilename() locks that same unconstructed mutex, so there is no safe way to probe "is the config up
    // yet". The only cure is not to ask, and the caller must be a runtime path (Leader::Decisions).
    //
    // So the settings are read where they are already read - when a row is actually recorded, which is
    // unambiguously runtime. The consequence, stated rather than hidden: the boot row appears at the first recorded
    // row rather than at the instant of attaching. In practice that is the leader's first decision line, moments
    // later; it is not the process's first instruction, and a window that needs the boot row should be taken from
    // the row's own at_ms and not from an assumption about when the server started.
    void Attach()
    {
        static bool const attached = []
        {
            AttachPullObserver();
            AttachLogStream();
            return true;
        }();
        (void)attached;
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
            Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
                                "{},{},{},{},{},{},{},{},{},{},{})",
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
                                Quoted(Config().build, 16), Quoted(decision.method, 24), OrNull(decision.packSize),
                                OrNull(decision.careful), OrNull(decision.judgeX), OrNull(decision.judgeY),
                                OrNull(decision.judgeZ), Quoted(decision.kind, 24), decision.routeTargetEntry,
                                decision.routeTargetLow, decision.mapId, decision.zoneId));

        uint16 idx = 0;
        for (Option const& option : decision.options)
            Writer::Instance().Push(
                "pb_journal_option",
                Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
                                    "{},{},{},{},{},{},{},{},{},{},{},{},{},{})",
                                    id, BootId(), nowMs, hour, idx++, Quoted(option.name, 255),
                                    Quoted(option.kind, 24), Quoted(option.reason, 1024),
                                    Quoted(option.sourceKind, 16), Real(option.cost), Real(option.value),
                                    option.taken ? 1 : 0,
                                    option.fields.empty() ? std::string("NULL")
                                                          : Quoted(option.fields, Config().optionFieldsMax),
                                    Quoted(Config().build, 16), OrNull(option.fightSecLeft), OrNull(option.runInSec),
                                    OrNull(option.openSec), Quoted(option.opener, 48), OrNull(option.distance),
                                    OrNull(option.overload), OrNull(option.drop), OrNull(option.tankHealth),
                                    OrNull(option.unionSec), OrNull(option.groupDps), OrNull(option.walkSec),
                                    OrNull(option.walkYd), Quoted(option.healer, 32), OrNull(option.healerWalks),
                                    OrNull(option.healerSec), OrNull(option.due), OrNull(option.stop),
                                    OrNull(option.packMobs), OrNull(option.fightMobs), OrNull(option.unionMobs),
                                    // [test1] the world gates; OrNull keeps "never reached" out of the data as
                                    // NULL instead of writing it as a passed gate
                                    OrNull(option.gateBoss), OrNull(option.gateWalker), OrNull(option.gateBuff),
                                    OrNull(option.gateDead), OrNull(option.gateHolds), OrNull(option.gateWakes)));

        WriteActors(id, nowMs, hour, decision.actors);
        return id;
    }

    namespace
    {
        // one row per participant the judge saw, child of the decision by (decision_id, idx)
        void WriteActors(uint64 id, uint64 nowMs, uint64 hour, std::vector<Actor> const& actors)
        {
            uint16 idx = 0;
            for (Actor const& a : actors)
                Writer::Instance().Push(
                    "pb_journal_actor",
                    Acore::StringFormat(
                        "({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
                        "{},{},{},{},{})",
                        id, BootId(), nowMs, hour, idx++, uint32(a.side), Quoted(a.name, 48), a.low, a.entry,
                        a.spawnId, OrNull(a.level), OrNull(a.spec, 24), OrNull(a.role, 8), OrNull(a.health),
                        OrNull(a.healthMax), OrNull(a.healthPct), OrNull(a.mana), OrNull(a.manaMax),
                        OrNull(a.manaPct), OrNull(a.x), OrNull(a.y), OrNull(a.z), OrNull(a.tankYd),
                        OrNull(a.targetYd), OrNull(a.inCombat), OrNull(a.wanders), OrNull(a.openerReady),
                        OrNull(a.doing, 96), OrNull(a.checks, 255), OrNull(a.chkReachesSpot), OrNull(a.chkPullPath),
                        OrNull(a.chkWalk), OrNull(a.chkWalksIntoFight), Quoted(a.note, 48),
                        Quoted(Config().build, 16)));
        }
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

    uint64 RecordRoute(Route const& route)
    {
        if (!Ready())
            return 0;
        uint64 const nowMs = NowMs();
        uint64 const hour = nowMs / MS_PER_HOUR;
        uint64 const id = Writer::Instance().NextId();
        Writer::Instance().Push(
            "pb_journal_route",
            Acore::StringFormat(
                "({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{})", id,
                BootId(), nowMs, hour, Quoted(route.event, 12), Quoted(route.purpose, 48), Quoted(route.target, 96),
                route.targetEntry, route.targetLow, Quoted(route.why, 512), Quoted(route.verdict, 16),
                Quoted(route.botName, 32), route.botLow, route.mapId, route.zoneId, Real(route.fromX),
                Real(route.fromY), Real(route.fromZ), Real(route.toX), Real(route.toY), Real(route.toZ),
                Real(route.pathYd), Real(route.straightYd), route.stopCount,
                route.changedStop ? std::to_string(*route.changedStop) : std::string("NULL"),
                Quoted(route.changedWhy, 255), Quoted(route.runId, 48), route.decisionId,
                Quoted(Config().build, 16)));
        for (RouteStop const& stop : route.stops)
            Writer::Instance().Push(
                "pb_journal_route_stop",
                Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{},{})", id, BootId(), nowMs, hour,
                                    stop.seq, Real(stop.x), Real(stop.y), Real(stop.z), Quoted(stop.kind, 16),
                                    Quoted(stop.what, 96), stop.entry, stop.low, Quoted(stop.verdict, 16),
                                    Quoted(stop.why, 512), Quoted(Config().build, 16)));
        return id;
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
            // [jrn40] the tag is quoted to 32 and not 24. The old 24 did not merely clip a label: it SILENTLY
            // DESTROYED tags. "[pull_estimate_vs_actual]" (1 096 rows) and "[queue-stale-idle-victim]" (333 rows)
            // reached the table with their closing bracket cut off, so a query by the real tag found neither, and
            // the parser's own bound threw away "[conjure_outside_instances]" and "[route_blocker_required_only]"
            // whole (19 rows with an empty tag though the line plainly starts with one). A label that is wrong in
            // the table is worse than a missing one, because a count by it answers.
            Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{},{})", Writer::Instance().NextId(),
                                BootId(), nowMs, nowMs / MS_PER_HOUR, Quoted(line.tag, 32), line.botLow,
                                Quoted(line.botName, 32), line.mapId, line.zoneId, Real(line.x), Real(line.y),
                                Real(line.z), Quoted(line.text, 16000), Quoted(line.runId, 48),
                                Quoted(Config().build, 16)));
    }

    void RecordMetric(Metric const& metric)
    {
        if (!Ready())
            return;
        uint64 const nowMs = NowMs();
        Writer::Instance().Push(
            "pb_journal_metric",
            Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{})", Writer::Instance().NextId(),
                                BootId(), nowMs, nowMs / MS_PER_HOUR, Quoted(metric.name, 64), Real(metric.value),
                                Quoted(metric.unit, 16), metric.fromMs, metric.toMs, Quoted(metric.scope, 64),
                                Quoted(Config().build, 16), OrNull(metric.valueMin), OrNull(metric.valueMax),
                                OrNull(metric.valueP95), OrNull(metric.samples), OrNull(metric.bots),
                                OrNull(metric.sessions)));
    }

    // [jrn30] the three event kinds this build adds. Each is one row, written where the fact is true and nowhere
    // else: a fight's start at the combat-state transition, an aggro gain at the creature's engage, a death at the
    // kill. None of them reads the world a second time and none of them invents a value it was not handed.
    void RecordFight(Fight const& fight)
    {
        if (!Ready())
            return;
        uint64 const nowMs = NowMs();
        Writer::Instance().Push(
            "pb_journal_fight",
            Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
                                "{},{},{},{})",
                                Writer::Instance().NextId(), BootId(), nowMs, nowMs / MS_PER_HOUR,
                                Quoted(fight.event, 12), Quoted(fight.botName, 32), fight.botLow,
                                OrNull(fight.botLevel), OrNull(fight.role, 8), fight.mapId, fight.zoneId,
                                fight.areaId, Real(fight.x), Real(fight.y), Real(fight.z), fight.inDungeon ? 1 : 0,
                                Quoted(fight.enemyName, 48), fight.enemyEntry, fight.enemyLow,
                                OrNull(fight.enemyLevel), OrNull(fight.mobs), Quoted(fight.pullerName, 32),
                                fight.pullerLow, Quoted(fight.pullKind, 12), Quoted(fight.pullMethod, 16),
                                OrNull(fight.pullExpected), Real(fight.pullCost), fight.pullOrderId,
                                Quoted(fight.runId, 48), fight.decisionId, Quoted(Config().build, 16)));
    }

    void RecordAggro(Aggro const& aggro)
    {
        if (!Ready())
            return;
        uint64 const nowMs = NowMs();
        Writer::Instance().Push(
            "pb_journal_aggro",
            Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
                                "{},{})",
                                Writer::Instance().NextId(), BootId(), nowMs, nowMs / MS_PER_HOUR,
                                Quoted(aggro.botName, 32), aggro.botLow, OrNull(aggro.role, 8),
                                OrNull(aggro.botHealthPct), OrNull(aggro.botInCombat), OrNull(aggro.botMoving),
                                Quoted(aggro.mobName, 48), aggro.mobEntry, aggro.mobLow, OrNull(aggro.mobLevel),
                                aggro.mapId, aggro.zoneId, Real(aggro.x), Real(aggro.y), Real(aggro.z),
                                Quoted(aggro.reason, 16), Quoted(aggro.why, 512), Quoted(aggro.tankName, 32),
                                aggro.tankLow, OrNull(aggro.expected), aggro.orderTargetEntry, aggro.orderTargetLow,
                                Quoted(aggro.runId, 48), aggro.decisionId, Quoted(Config().build, 16)));
    }

    uint64 RecordDeath(Death const& death)
    {
        if (!Ready())
            return 0;
        uint64 const nowMs = NowMs();
        uint64 const id = Writer::Instance().NextId();
        Writer::Instance().Push(
            "pb_journal_death",
            Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
                                "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{})",
                                id, BootId(), nowMs, nowMs / MS_PER_HOUR,
                                Quoted(death.botName, 32), death.botLow, OrNull(death.botLevel),
                                OrNull(death.botClass), OrNull(death.role, 8), OrNull(death.spec, 24), death.mapId,
                                death.zoneId, death.areaId, Real(death.x), Real(death.y), Real(death.z),
                                death.inDungeon ? 1 : 0, Quoted(death.killerName, 48), death.killerEntry,
                                death.killerLow, OrNull(death.killerLevel), OrNull(death.killerHealthPct),
                                OrNull(death.healthMax), OrNull(death.damageFromKiller), OrNull(death.attackers),
                                OrNull(death.judgedPull), Quoted(death.expected, 512), OrNull(death.pullExpected),
                                Quoted(death.pullMethod, 16), Quoted(death.context, 1024),
                                Quoted(death.threat, 1024), Quoted(death.runId, 48), death.decisionId,
                                Quoted(Config().build, 16),
                                OrNull(death.facts.hitBy), OrNull(death.facts.takenThisFight),
                                OrNull(death.facts.damageFromKiller), OrNull(death.facts.fightSec),
                                OrNull(death.facts.runPastPriced), OrNull(death.facts.runPastPricedHigh),
                                OrNull(death.facts.runPastSec), OrNull(death.facts.runPastHp),
                                OrNull(death.facts.runPastTaken), OrNull(death.facts.runPastOfItsChase),
                                OrNull(death.facts.routeAgeMs), OrNull(death.facts.routeComplete),
                                OrNull(death.facts.routeThroughRefused), OrNull(death.facts.routePriced),
                                OrNull(death.facts.routeRefused), OrNull(death.facts.routeUnseen),
                                OrNull(death.facts.routeOtherFight)));
        return id;
    }

    void RecordIssue(Issue const& issue)
    {
        if (!Ready())
            return;
        uint64 const nowMs = NowMs();

        // [jrn40] The inputs come from the CALLER, in `issue.fixture`, and nothing is fetched here. The emitter is
        // holding those values at the instant the problem is seen, which is the only instant they exist; asking
        // anybody for them later would be a reading of the world at another moment.
        Writer::Instance().Push(
            "pb_journal_issue",
            Acore::StringFormat(
                // the UNIQUE identity is MD5 of the dedup key, computed in SQL FROM THAT VERY STRING: the full key
                // stays readable in its own column while two different keys can never be merged by a truncated
                // index. One owner of the key - the emitter builds it, the database only hashes it.
                "({},{},{},{},{},{},{},MD5({}),{},{},{},{},1,{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
                "{},{},{},{},{},{},{},{},{},{},{},{})",
                Writer::Instance().NextId(), BootId(), Quoted(issue.kind, 32), Quoted(issue.severity, 8),
                Quoted(issue.reproClass, 16), Quoted(issue.status.empty() ? "open" : issue.status, 12),
                Quoted(issue.dedupKey, 512), Quoted(issue.dedupKey, 512), Quoted(issue.why, 1024),
                Quoted(issue.realWhen, 255), nowMs, nowMs, OrNull(issue.value), OrNull(issue.value),
                Quoted(issue.valueUnit, 16), Quoted(issue.botName, 32), issue.botLow, issue.mapId, issue.zoneId,
                Real(issue.x), Real(issue.y), Real(issue.z), Quoted(issue.runId, 48),
                Quoted(issue.sourceTable, 32), issue.sourceId, issue.decisionId, Quoted(Config().build, 16),
                Quoted(Config().build, 16), issue.checked ? 1 : 0, issue.checked ? nowMs : 0,
                Quoted(issue.checkedBy, 48), Quoted(issue.testName, 128), Quoted(issue.clearedInRun, 48),
                // fixture_path is always inserted empty and is never written from the server: it is where the
                // OFFLINE exporter records the file it made out of this row, by its own UPDATE. The inputs
                // themselves travel in `fixture` below, from the emitter.
                Quoted(issue.fixedInBuild, 16), 0, issue.mergedInto, Quoted(std::string(), 255),
                issue.lastProgressMs,
                Quoted(issue.real.empty() ? "unknown" : issue.real, 8), Quoted(issue.realWhy, 512),
                Quoted(issue.realSource.empty() ? "emitter" : issue.realSource, 8),
                issue.fixture.empty() ? std::string("NULL") : Quoted(issue.fixture, 16000)));
    }

    uint64 RecordIdle(Idle const& idle)
    {
        if (!Ready())
            return 0;
        uint64 const nowMs = NowMs();
        uint64 const id = Writer::Instance().NextId();
        Writer::Instance().Push(
            "pb_journal_idle",
            Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
                                "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{})",
                                id, BootId(), nowMs, nowMs / MS_PER_HOUR, Quoted(idle.event, 12),
                                Quoted(idle.botName, 32), idle.botLow, OrNull(idle.botLevel), OrNull(idle.role, 8),
                                idle.mapId, idle.zoneId, Real(idle.x), Real(idle.y), Real(idle.z),
                                OrNull(idle.inDungeon), OrNull(idle.inCombat), OrNull(idle.windowSec),
                                OrNull(idle.ticks), OrNull(idle.casts), OrNull(idle.castWait), OrNull(idle.nonCast),
                                OrNull(idle.noTarget), OrNull(idle.rangeLos), OrNull(idle.moving), OrNull(idle.gcd),
                                OrNull(idle.threat), OrNull(idle.leaderHold), OrNull(idle.other),
                                OrNull(idle.refUnsaid), OrNull(idle.refUseless), OrNull(idle.refImpossible),
                                OrNull(idle.refPrereq), OrNull(idle.refFailed), OrNull(idle.refBlocked),
                                OrNull(idle.refNoCandidate), OrNull(idle.gapN), OrNull(idle.gapAvgMs),
                                OrNull(idle.gapMaxMs), OrNull(idle.gapOver150), OrNull(idle.gapOver400),
                                Quoted(idle.topActions, 255), OrNull(idle.stoodSec), OrNull(idle.stoodYd),
                                Quoted(idle.doing, 96), Quoted(idle.heldBy, 48), Quoted(idle.why, 512),
                                Quoted(idle.runId, 48), Quoted(Config().build, 16)));
        return id;
    }

    uint64 RecordStuck(Stuck const& stuck)
    {
        if (!Ready())
            return 0;
        uint64 const nowMs = NowMs();
        uint64 const id = Writer::Instance().NextId();
        Writer::Instance().Push(
            "pb_journal_stuck",
            Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{})", id,
                                BootId(), nowMs, nowMs / MS_PER_HOUR, Quoted(stuck.kind, 16),
                                Quoted(stuck.botName, 32), stuck.botLow, OrNull(stuck.botLevel),
                                OrNull(stuck.role, 8), stuck.mapId, stuck.zoneId, Real(stuck.x), Real(stuck.y),
                                Real(stuck.z), Quoted(stuck.targetName, 48), stuck.targetEntry, stuck.targetLow,
                                OrNull(stuck.targetYd), OrNull(stuck.targetLevel),
                                OrNull(stuck.targetZoneId),  // [jrn43]
                                OrNull(stuck.inCombat),
                                Quoted(stuck.actionRunning, 96), Quoted(stuck.why, 512), Quoted(stuck.runId, 48),
                                Quoted(Config().build, 16)));
        return id;
    }

    uint64 RecordAlert(Alert const& alert)
    {
        if (!Ready())
            return 0;
        uint64 const nowMs = NowMs();
        uint64 const id = Writer::Instance().NextId();
        Writer::Instance().Push(
            "pb_journal_alert",
            Acore::StringFormat("({},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{})", id,
                                BootId(), nowMs, nowMs / MS_PER_HOUR, Quoted(alert.kind, 24), OrNull(alert.num),
                                Quoted(alert.botName, 32), alert.botLow, OrNull(alert.botLevel), alert.mapId,
                                alert.zoneId, Real(alert.x), Real(alert.y), Real(alert.z), OrNull(alert.loopN),
                                Quoted(alert.activity, 32), OrNull(alert.noProgressXp),
                                OrNull(alert.noProgressQuest), OrNull(alert.samePlace),
                                Quoted(alert.lastAction, 512), Quoted(alert.reason, 512),
                                alert.text.empty() ? std::string("NULL") : Quoted(alert.text, 16000),
                                Quoted(alert.runId, 48), Quoted(Config().build, 16)));
        return id;
    }

    std::string WithoutNumbers(std::string const& text)
    {
        std::string out;
        out.reserve(text.size());
        bool inNumber = false;
        for (char const c : text)
        {
            if (c >= '0' && c <= '9')
            {
                if (!inNumber)
                    out += 'N';
                inNumber = true;
                continue;
            }
            // a decimal point or separator inside a number belongs to it, so "15.5" and "3 216" each collapse to
            // one "N" rather than to "N.N" and "N N" - two keys where there is one cause
            if (inNumber && (c == '.' || c == ','))
                continue;
            inNumber = false;
            out += c;
        }
        return out;
    }

    uint64 ClockMs() { return NowMs(); }

    uint64 QueuedRows() { return Config().enabled ? Writer::Instance().Queued() : 0; }
    uint64 DroppedRows() { return Config().enabled ? Writer::Instance().Dropped() : 0; }

    void Flush()
    {
        if (Config().enabled)
            Writer::Instance().Flush();
    }
}  // namespace Journal
