-- [jrn40] THE LIST OF PROBLEMS and the three domain records it points at.
--
-- A module SQL update: the core's DB updater applies it on start, or by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Every statement is CREATE TABLE IF NOT EXISTS or an ALTER guarded on information_schema, so a second run does
-- nothing. The CREATE statements below are EXTRACTED FROM Journal.cpp's own DDL rather than retyped: a file that
-- drifts from the code is the defect where a comment costs more than a wrong number, and this project has already
-- paid for it once (pb_journal_decision ran with 28 columns against a 37-field tuple and the table would have gone
-- silently empty).
--
-- WHAT THIS IS FOR. The user: "we already have a separate table for deaths, but we could create an issues table and
-- put into it what we consider problems - idling, deaths, wipes, getting stuck and so on", and "we will analyse
-- every problem there until there are 0 rows".
--
-- pb_journal_issue IS A THIN, UNIFORM POINTER and not a copy of anybody's inputs. A death has pb_journal_death with
-- its killer and the pool behind its health; copying those here would put one quantity in two places and make the
-- columns the union of every kind's fields, NULL for all the others. So the inputs stay with their domain
-- (pb_journal_death, pb_journal_idle, pb_journal_stuck, pb_journal_alert, guild_project_runs) and the issue row
-- reaches them through source_table + source_id.
--
-- THE FOUR THINGS THAT KEEP IT HONEST, all proven by execution against a live table before this file was written:
--
--  1. IT IS NOT UNDER RETENTION AND IS NOT PARTITIONED. Every other journal table keeps 24-48 h and is cleaned by
--     dropping hour partitions; a list of problems that evaporates every two days is not a list of problems. The
--     sweep in Journal.cpp does not name this table, and must not.
--     CONSEQUENCE, and it is why fixture_path exists: the DOMAIN record this row points at IS swept, so a week
--     later source_id reaches nothing and the only surviving copy of the inputs is the fixture file taken when the
--     problem was first seen.
--
--  2. A ROW IS A PROBLEM, NOT AN OCCURRENCE. dedup_hash is UNIQUE (MD5 of dedup_key, computed in SQL from that
--     very string, so the full key stays readable and two keys can never be merged by a truncated index) and a
--     repeat UPDATES the row: occurrences rises, last_seen_ms moves, first_seen_ms / first_build / id / fixture
--     are left alone. Measured: 2 occurrences inserted in one statement produced 1 row with occurrences = 2.
--     This is trap #1 of the project - one cause printed 3-4 times, a run's report line printed in five places,
--     inflating counts by up to 7.3x - and without the merge the table could never reach zero.
--
--  3. NUMBERS COME OUT OF THE REASON. dedup_key is the cause with every run of digits replaced by "N", and the
--     range they spanned is kept in value_min / value_max. Trap #2: "177 at 15 %, 158 at 14 %..." looked like
--     fifteen causes and was ONE cause behind 3 216 holds. Measured on the live stream: keying the idle
--     diagnostics on the WHOLE LINE gives 757 "causes" over 3 900 rows; keying on the cause alone gives 1 - the
--     757 were a trailing list of action names, the same trap in new clothes. So the key is built from chosen
--     cause fields and never from the line, and which fields each kind uses is written in C:/WoW/CLAUDE.md.
--
--  4. A ROW CLOSES CHECKABLY AND REOPENS BY ITSELF.
--     checked_state is a GENERATED column, so the tick cannot be taken on trust and cannot disagree with the
--     evidence: "unchecked" / "by_test" (repro_class decision + test_name) / "by_run" (repro_class not_offline +
--     cleared_in_run + fixed_in_build) / "unconfirmed" (checked with neither). Measured: checked = 1 with nothing
--     behind it reports "unconfirmed", which is a value a query counts rather than a comment somebody reads.
--     And an occurrence arriving after fixed_ms on a build not older than fixed_in_build sets status = 'returned'
--     AND zeroes checked in the same statement. Measured: a b501 sighting of a row fixed in b500 came back as
--     returned / unchecked with occurrences = 3 and first_build still b500. The build is compared as a NUMBER
--     (b501 -> 501), because comparing "b99" against "b100" as text is a defect waiting for build 1000.
--
-- is_real IS A COLUMN, NOT A GATE. The user: "better to over-worry than under-worry. If we add rubbish, we will
-- decide it is rubbish. But decide it gradually, not by filtering first and then finding we have no data." The
-- asymmetry is real and this project lived it: positions were not written at all, so no question about where a bot
-- stood had an answer. Three values, and the third is not a gap by negligence - it is the project's own '' against
-- '?' distinction: 'yes' (the emitter knows at runtime, real_when says by what criterion), 'no' (judged rubbish,
-- real_why says why IN WORDS, and the row STAYS - marking it is a case analysed, not a case vanished), 'unknown'
-- (not known at runtime; post-analysis fills it). The count driven to zero is is_real IN ('yes','unknown').
-- real_source says whether a value was measured at runtime ('emitter') or inferred later ('post'), because a month
-- on there is otherwise no telling the measured from the supposed.
--
-- The column is named is_real and not "real" because REAL is a reserved word in MySQL.
--
-- pb_journal_line gains run_id, and its tag column widens from 24 to 32. The width was not a cosmetic limit: it
-- SILENTLY DESTROYED tags. "[pull_estimate_vs_actual]" (1 096 rows) and "[queue-stale-idle-victim]" (333 rows)
-- reached the table with the closing bracket cut off, so a query for the real tag matched neither; and the
-- parser's own matching bound threw away "[conjure_outside_instances]" and "[route_blocker_required_only]" whole
-- (19 rows with an empty tag though the line plainly starts with one). A label that is wrong is worse than one
-- that is missing, because a count by it answers. run_id empty means "outside a run", not "lost": a line out of
-- the log appender has no run to name, and without the column all 411 819 stream rows could be joined to a run
-- only by nearness in time - which is exactly what a fixture must never bake in.

CREATE TABLE IF NOT EXISTS pb_journal_issue (id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL,
  kind VARCHAR(32) NOT NULL DEFAULT '', severity VARCHAR(8) NOT NULL DEFAULT '',
  repro_class VARCHAR(16) NOT NULL DEFAULT '', status VARCHAR(12) NOT NULL DEFAULT 'open',
  dedup_key VARCHAR(512) NOT NULL DEFAULT '', dedup_hash CHAR(32) NOT NULL, why VARCHAR(1024) NOT NULL DEFAULT '',
  real_when VARCHAR(255) NOT NULL DEFAULT '', first_seen_ms BIGINT UNSIGNED NOT NULL,
  last_seen_ms BIGINT UNSIGNED NOT NULL, occurrences BIGINT UNSIGNED NOT NULL DEFAULT 1, value_min DOUBLE NULL,
  value_max DOUBLE NULL, value_unit VARCHAR(16) NOT NULL DEFAULT '', bot_name VARCHAR(32) NOT NULL DEFAULT '',
  bot_low INT UNSIGNED NOT NULL DEFAULT 0, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  zone_id INT UNSIGNED NOT NULL DEFAULT 0, pos_x FLOAT NOT NULL DEFAULT 0, pos_y FLOAT NOT NULL DEFAULT 0,
  pos_z FLOAT NOT NULL DEFAULT 0, run_id VARCHAR(48) NOT NULL DEFAULT '',
  source_table VARCHAR(32) NOT NULL DEFAULT '', source_id BIGINT UNSIGNED NOT NULL DEFAULT 0,
  decision_id BIGINT UNSIGNED NOT NULL DEFAULT 0, build VARCHAR(16) NOT NULL DEFAULT '',
  first_build VARCHAR(16) NOT NULL DEFAULT '', checked TINYINT UNSIGNED NOT NULL DEFAULT 0,
  checked_at_ms BIGINT UNSIGNED NOT NULL DEFAULT 0, checked_by VARCHAR(48) NOT NULL DEFAULT '',
  test_name VARCHAR(128) NOT NULL DEFAULT '', cleared_in_run VARCHAR(48) NOT NULL DEFAULT '',
  fixed_in_build VARCHAR(16) NOT NULL DEFAULT '', fixed_ms BIGINT UNSIGNED NOT NULL DEFAULT 0,
  merged_into BIGINT UNSIGNED NOT NULL DEFAULT 0, fixture_path VARCHAR(255) NOT NULL DEFAULT '',
  last_progress_ms BIGINT UNSIGNED NOT NULL DEFAULT 0, is_real VARCHAR(8) NOT NULL DEFAULT 'unknown',
  real_why VARCHAR(512) NOT NULL DEFAULT '', real_source VARCHAR(8) NOT NULL DEFAULT 'emitter',
  checked_state VARCHAR(12) GENERATED ALWAYS AS (CASE WHEN checked = 0 THEN 'unchecked'
    WHEN repro_class = 'decision' AND test_name <> '' THEN 'by_test'
    WHEN repro_class = 'not_offline' AND cleared_in_run <> '' AND fixed_in_build <> '' THEN 'by_run'
    ELSE 'unconfirmed' END) STORED,
  PRIMARY KEY (id), UNIQUE KEY dedup (dedup_hash), KEY kind_last (kind, last_seen_ms), KEY status_kind (status, kind,
  last_seen_ms), KEY repro_status (repro_class, status, last_seen_ms), KEY checked_state_at (checked_state,
  last_seen_ms), KEY severity_at (severity, last_seen_ms), KEY bot_at (bot_low, last_seen_ms), KEY where_at (map_id,
  zone_id, last_seen_ms), KEY build_at (build, last_seen_ms), KEY first_at (first_seen_ms),
  KEY occurrences_at (occurrences, last_seen_ms), KEY source (source_table, source_id), KEY merged (merged_into),
  KEY run_at (run_id, last_seen_ms), KEY test (test_name), KEY real_at (is_real, kind, last_seen_ms),
  KEY real_source_at (real_source,
  last_seen_ms)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn40] one row per PROBLEM,
  driven to zero rows - NOT under retention, never swept';
CREATE TABLE IF NOT EXISTS pb_journal_idle (id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL,
  at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, event VARCHAR(12) NOT NULL DEFAULT '',
  bot_name VARCHAR(32) NOT NULL DEFAULT '', bot_low INT UNSIGNED NOT NULL DEFAULT 0, bot_level SMALLINT UNSIGNED NULL,
  role VARCHAR(8) NULL, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, zone_id INT UNSIGNED NOT NULL DEFAULT 0,
  pos_x FLOAT NOT NULL DEFAULT 0, pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0,
  in_dungeon TINYINT UNSIGNED NULL, in_combat TINYINT UNSIGNED NULL, window_sec FLOAT NULL,
  ticks SMALLINT UNSIGNED NULL, casts SMALLINT UNSIGNED NULL, cast_wait SMALLINT UNSIGNED NULL,
  non_cast SMALLINT UNSIGNED NULL, no_target SMALLINT UNSIGNED NULL, range_los SMALLINT UNSIGNED NULL,
  moving SMALLINT UNSIGNED NULL, gcd SMALLINT UNSIGNED NULL, threat SMALLINT UNSIGNED NULL,
  leader_hold SMALLINT UNSIGNED NULL, other SMALLINT UNSIGNED NULL, ref_unsaid SMALLINT UNSIGNED NULL,
  ref_useless SMALLINT UNSIGNED NULL, ref_impossible SMALLINT UNSIGNED NULL, ref_prereq SMALLINT UNSIGNED NULL,
  ref_failed SMALLINT UNSIGNED NULL, ref_blocked SMALLINT UNSIGNED NULL, ref_no_candidate SMALLINT UNSIGNED NULL,
  gap_n SMALLINT UNSIGNED NULL, gap_avg_ms INT UNSIGNED NULL, gap_max_ms INT UNSIGNED NULL,
  gap_over_150 SMALLINT UNSIGNED NULL, gap_over_400 SMALLINT UNSIGNED NULL,
  top_actions VARCHAR(255) NOT NULL DEFAULT '', stood_sec FLOAT NULL, stood_yd FLOAT NULL,
  doing VARCHAR(96) NOT NULL DEFAULT '', why VARCHAR(512) NOT NULL DEFAULT '', run_id VARCHAR(48) NOT NULL DEFAULT '',
  build VARCHAR(16) NOT NULL DEFAULT '', PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY event_at (event,
  at_ms), KEY bot_at (bot_low, at_ms), KEY role_at (role, at_ms), KEY where_at (map_id, zone_id, at_ms),
  KEY other_at (other, at_ms), KEY stood_at (stood_sec, at_ms), KEY gap_at (gap_max_ms, at_ms), KEY casts_at (casts,
  at_ms), KEY build_at (build, at_ms), KEY run_at (run_id,
  at_ms))
  ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='[jrn40] idleness measured by fact: the diagnostics window and the standing watch,
  for EVERY role' PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);
CREATE TABLE IF NOT EXISTS pb_journal_stuck (id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL,
  at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, kind VARCHAR(16) NOT NULL DEFAULT '',
  bot_name VARCHAR(32) NOT NULL DEFAULT '', bot_low INT UNSIGNED NOT NULL DEFAULT 0, bot_level SMALLINT UNSIGNED NULL,
  role VARCHAR(8) NULL, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, zone_id INT UNSIGNED NOT NULL DEFAULT 0,
  pos_x FLOAT NOT NULL DEFAULT 0, pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0,
  target_name VARCHAR(48) NOT NULL DEFAULT '', target_entry INT UNSIGNED NOT NULL DEFAULT 0,
  target_low INT UNSIGNED NOT NULL DEFAULT 0, target_yd FLOAT NULL, target_level SMALLINT UNSIGNED NULL,
  in_combat TINYINT UNSIGNED NULL, action_running VARCHAR(96) NOT NULL DEFAULT '',
  why VARCHAR(512) NOT NULL DEFAULT '', run_id VARCHAR(48) NOT NULL DEFAULT '', build VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY kind_at (kind, at_ms), KEY bot_at (bot_low, at_ms),
  KEY target_at (target_entry, target_low, at_ms), KEY where_at (map_id, zone_id, at_ms), KEY yd_at (target_yd,
  at_ms), KEY role_at (role, at_ms), KEY build_at (build, at_ms), KEY run_at (run_id,
  at_ms)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn40] held or awaited and not progressing: target,
  distance, what ran' PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);
CREATE TABLE IF NOT EXISTS pb_journal_alert (id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL,
  at_ms BIGINT UNSIGNED NOT NULL, hour_bucket INT UNSIGNED NOT NULL, kind VARCHAR(24) NOT NULL DEFAULT '',
  num INT UNSIGNED NULL, bot_name VARCHAR(32) NOT NULL DEFAULT '', bot_low INT UNSIGNED NOT NULL DEFAULT 0,
  bot_level SMALLINT UNSIGNED NULL, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  zone_id INT UNSIGNED NOT NULL DEFAULT 0, pos_x FLOAT NOT NULL DEFAULT 0, pos_y FLOAT NOT NULL DEFAULT 0,
  pos_z FLOAT NOT NULL DEFAULT 0, loop_n INT UNSIGNED NULL, activity VARCHAR(32) NOT NULL DEFAULT '',
  no_progress_xp TINYINT UNSIGNED NULL, no_progress_quest TINYINT UNSIGNED NULL, same_place TINYINT UNSIGNED NULL,
  last_action VARCHAR(512) NOT NULL DEFAULT '', reason VARCHAR(512) NOT NULL DEFAULT '', text TEXT,
  run_id VARCHAR(48) NOT NULL DEFAULT '', build VARCHAR(16) NOT NULL DEFAULT '', PRIMARY KEY (id, hour_bucket),
  KEY at_ms (at_ms), KEY kind_at (kind, at_ms), KEY bot_at (bot_low, at_ms), KEY activity_at (activity, at_ms),
  KEY where_at (map_id, zone_id, at_ms), KEY loop_at (loop_n, at_ms), KEY build_at (build, at_ms), KEY run_at (run_id,
  at_ms)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn40] one alert,
  from the single funnel every alert already passes'
  PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_line
     ADD COLUMN run_id VARCHAR(48) NOT NULL DEFAULT '''',
     MODIFY COLUMN tag VARCHAR(32) NOT NULL DEFAULT '''',
     ADD KEY run_at (run_id, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_line' AND COLUMN_NAME = 'run_id');
PREPARE jrn40 FROM @stmt; EXECUTE jrn40; DEALLOCATE PREPARE jrn40;
