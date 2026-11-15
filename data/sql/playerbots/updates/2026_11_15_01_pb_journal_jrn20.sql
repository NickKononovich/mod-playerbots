-- [jrn20] The MySQL journal: the server's own history in `acore_playerbots`, millisecond-stamped.
--
-- A module SQL update: the core's DB updater applies it on start, or the BUILD AGENT applies it by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Safe either way and safe before the build: every statement is CREATE TABLE IF NOT EXISTS, nothing is dropped,
-- nothing existing is touched, and no other table is altered. In particular this file does NOT touch
-- playerbots_fight_history (the fight measurements stay their owner's) and does NOT touch guild_project_runs or
-- guild_project_cert: the journal links to those by id and never copies them.
--
-- Applying it by hand is in fact OPTIONAL. The server creates these tables itself on the first journal row, exactly
-- the way `guild_project_runs` is created (GuildCoordinator::LoadRuns), with the SAME statements as below - the file
-- exists so the schema can be read and reviewed without reading C++, and so the tables can be made ahead of a build.
-- Keeping the two literally identical is deliberate: a file that drifts from the code is the kind of defect where a
-- comment costs more than a wrong number.
--
-- WHY THIS EXISTS. Playerbots.log is truncated at every server start, so the baseline a comparison needs disappears
-- with the restart; the window a measurement covers is whatever the run happened to last, so raw counts are not
-- comparable; and a count made by grep is a count of whatever a pattern matched, which is how two readers measured
-- "the same" thing and disagreed, one of them scoring zero by construction. In a table the history outlives the
-- restart (`boot_id`), the window is a WHERE clause on `at_ms`, and the count is one query everybody runs.
--
-- THE POINT OF THE DESIGN. Four diagnoses in one night were wrong the same way: a log line was taken for the verdict
-- without asking who reads it and what it counts. So every decision row carries the judge, the code tag that printed
-- it, the field the judge read (`read_field`) and WHO READS THE VERDICT (`read_by`). In `read_by`, '?' means "not
-- established yet" and '' means "checked: nobody reads it" - the second is a finding, the first only a gap, and they
-- are kept apart on purpose. The options the judge did not take are rows of their own in `pb_journal_option`, each
-- with its own reason, cost and source kind.
--
-- ONE COUNT, NOT TWO. The narrow tables are not recomputed from the wide one. A decision is written once, by the
-- judge, and the line that was printed about it is kept in the row itself (`pb_journal_decision.text`), so a grep of
-- the file and a COUNT over the table cannot disagree about how many there were. Runs are NOT duplicated here: they
-- stay in `guild_project_runs` and the journal refers to them by `run_id` only. (Trap: that table's `at` column is
-- the run's END - the row is written with time(nullptr) when the finished run is inserted, and at+duration
-- lands in the future, which settles it - so a window over runs is ORDER BY id DESC LIMIT n and never a range
-- over `at`. Two briefs called it the start; the data and GuildFives.cpp say otherwise, and an agent had
-- already lost an hour to that.)
--
-- PARTITIONS AND RETENTION. Every row carries `hour_bucket` = FLOOR(at_ms / 3600000) and the tables are RANGE
-- partitioned on it. The server's journal thread carves hours out of `pfuture` ahead of time and drops whole
-- partitions past the retention (AiPlayerbot.Journal.RetentionHours, default 48). Retention is therefore a metadata
-- change, not a DELETE of five million rows across the table that is being written to.
--
-- SETTINGS (playerbots.conf):
--   AiPlayerbot.Journal.Enable          bool,   default true
--   AiPlayerbot.Journal.RetentionHours  int,    default 48      1-2 days, the user's choice
--   AiPlayerbot.Journal.BatchRows       int,    default 500
--   AiPlayerbot.Journal.FlushMs         int,    default 1000
--   AiPlayerbot.Journal.Stream          bool,   default false   also mirror raw lines into pb_journal_line
--   AiPlayerbot.Journal.Build           string, default ''      the build number the build agent installs, e.g. b495
--
-- THE QUERIES THE INDEXES ARE FOR are listed at the end of this file.

-- ---------------------------------------------------------------------------------------------------------------
-- pb_journal_boot
-- ---------------------------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pb_journal_boot (
  boot_id INT UNSIGNED NOT NULL,
  started_ms BIGINT UNSIGNED NOT NULL,
  started_at DATETIME(3) NOT NULL,
  note VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (boot_id),
  KEY started_ms (started_ms)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn20] one row per server start: the window a comparison uses survives the restart';

-- ---------------------------------------------------------------------------------------------------------------
-- pb_journal_decision
-- ---------------------------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pb_journal_decision (
  id BIGINT UNSIGNED NOT NULL,
  boot_id INT UNSIGNED NOT NULL,
  at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL,
  topic VARCHAR(48) NOT NULL,
  choice VARCHAR(255) NOT NULL,
  verdict VARCHAR(16) NOT NULL,
  why VARCHAR(1024) NOT NULL,
  judge VARCHAR(16) NOT NULL,
  judge_name VARCHAR(32) NOT NULL,
  judge_low INT UNSIGNED NOT NULL DEFAULT 0,
  subject VARCHAR(96) NOT NULL,
  subject_kind VARCHAR(16) NOT NULL,
  subject_entry INT UNSIGNED NOT NULL DEFAULT 0,
  subject_low INT UNSIGNED NOT NULL DEFAULT 0,
  cost DOUBLE NOT NULL DEFAULT 0,
  cost_unit VARCHAR(16) NOT NULL DEFAULT '',
  tag VARCHAR(24) NOT NULL DEFAULT '',
  src VARCHAR(96) NOT NULL DEFAULT '',
  read_field VARCHAR(48) NOT NULL DEFAULT '',
  read_by VARCHAR(96) NOT NULL DEFAULT '',
  run_id VARCHAR(48) NOT NULL DEFAULT '',
  run_ms INT UNSIGNED NOT NULL DEFAULT 0,
  options_total SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  options_rejected SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  fields TEXT,
  text TEXT,
  build VARCHAR(16) NOT NULL DEFAULT '',
  method VARCHAR(24) NOT NULL DEFAULT '',
  pack_size SMALLINT UNSIGNED NULL,
  careful TINYINT UNSIGNED NULL,
  judge_x FLOAT NULL,
  judge_y FLOAT NULL,
  judge_z FLOAT NULL,
  kind VARCHAR(24) NOT NULL DEFAULT '',
  route_target_entry INT UNSIGNED NOT NULL DEFAULT 0,
  route_target_low INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (id, hour_bucket),
  KEY at_ms (at_ms),
  KEY topic_at (topic, at_ms),
  KEY verdict_at (verdict, topic, at_ms),
  KEY judge_at (judge_low, at_ms),
  KEY judge_name_at (judge_name, at_ms),
  KEY run_at (run_id, at_ms),
  KEY tag_at (tag, at_ms),
  KEY boot_at (boot_id, at_ms),
  KEY subject_at (subject_entry, subject_low, at_ms),
  KEY read_by_at (read_by, at_ms),
  KEY method_at (method, at_ms),
  KEY pack_at (pack_size, at_ms),
  KEY careful_at (careful, at_ms),
  KEY choice_at (choice(64), at_ms),
  KEY judge_spot (judge_x, judge_y),
  KEY kind_at (kind, at_ms),
  KEY build_topic (build, topic, at_ms),
  KEY route_target_at (route_target_entry, route_target_low, at_ms)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn20] one choice: what was chosen, who judged, what it read, who reads the verdict' PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

-- ---------------------------------------------------------------------------------------------------------------
-- pb_journal_option
-- ---------------------------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pb_journal_option (
  decision_id BIGINT UNSIGNED NOT NULL,
  boot_id INT UNSIGNED NOT NULL,
  at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL,
  idx SMALLINT UNSIGNED NOT NULL,
  name VARCHAR(255) NOT NULL,
  kind VARCHAR(24) NOT NULL,
  reason VARCHAR(1024) NOT NULL,
  source_kind VARCHAR(16) NOT NULL DEFAULT '',
  cost DOUBLE NOT NULL DEFAULT 0,
  value DOUBLE NOT NULL DEFAULT 0,
  taken TINYINT UNSIGNED NOT NULL DEFAULT 0,
  fields TEXT,
  build VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (decision_id, idx, hour_bucket),
  KEY at_ms (at_ms),
  KEY source_at (source_kind, at_ms),
  KEY kind_at (kind, at_ms),
  KEY taken_at (taken, at_ms),
  KEY name_at (name(64), at_ms),
  KEY build_at (build, at_ms)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn20] every option a decision weighed, the rejected ones with their own reason' PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

-- ---------------------------------------------------------------------------------------------------------------
-- pb_journal_read
-- ---------------------------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pb_journal_read (
  decision_id BIGINT UNSIGNED NOT NULL,
  boot_id INT UNSIGNED NOT NULL,
  at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL,
  reader VARCHAR(96) NOT NULL,
  PRIMARY KEY (decision_id, reader, hour_bucket),
  KEY at_ms (at_ms),
  KEY reader_at (reader, at_ms)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn20] a reader that consumed a verdict - no row here means nobody did' PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

-- ---------------------------------------------------------------------------------------------------------------
-- pb_journal_loot
-- ---------------------------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pb_journal_loot (
  id BIGINT UNSIGNED NOT NULL,
  boot_id INT UNSIGNED NOT NULL,
  at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL,
  item_entry INT UNSIGNED NOT NULL,
  item_name VARCHAR(96) NOT NULL,
  count INT UNSIGNED NOT NULL DEFAULT 1,
  quality TINYINT UNSIGNED NOT NULL DEFAULT 0,
  from_kind VARCHAR(16) NOT NULL DEFAULT '',
  from_name VARCHAR(96) NOT NULL DEFAULT '',
  from_entry INT UNSIGNED NOT NULL DEFAULT 0,
  to_name VARCHAR(32) NOT NULL DEFAULT '',
  to_low INT UNSIGNED NOT NULL DEFAULT 0,
  verdict VARCHAR(16) NOT NULL DEFAULT '',
  why VARCHAR(512) NOT NULL DEFAULT '',
  worth DOUBLE NOT NULL DEFAULT 0,
  worth_unit VARCHAR(16) NOT NULL DEFAULT '',
  run_id VARCHAR(48) NOT NULL DEFAULT '',
  decision_id BIGINT UNSIGNED NOT NULL DEFAULT 0,
  build VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (id, hour_bucket),
  KEY at_ms (at_ms),
  KEY run_at (run_id, at_ms),
  KEY item_at (item_entry, at_ms),
  KEY to_at (to_low, at_ms),
  KEY verdict_at (verdict, at_ms),
  KEY from_at (from_entry, at_ms),
  KEY build_at (build, at_ms),
  KEY decision (decision_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn20] one dropped item, where it came from, who got it and on whose verdict' PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

-- ---------------------------------------------------------------------------------------------------------------
-- pb_journal_metric
-- ---------------------------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pb_journal_metric (
  id BIGINT UNSIGNED NOT NULL,
  boot_id INT UNSIGNED NOT NULL,
  at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL,
  name VARCHAR(64) NOT NULL,
  value DOUBLE NOT NULL DEFAULT 0,
  unit VARCHAR(16) NOT NULL DEFAULT '',
  from_ms BIGINT UNSIGNED NOT NULL DEFAULT 0,
  to_ms BIGINT UNSIGNED NOT NULL DEFAULT 0,
  scope VARCHAR(64) NOT NULL DEFAULT '',
  build VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (id, hour_bucket),
  KEY name_at (name, at_ms),
  KEY name_build (name, build, at_ms),
  KEY scope_at (scope, at_ms)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn20] one measured value with its own window and build, never an implied one' PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

-- ---------------------------------------------------------------------------------------------------------------
-- pb_journal_actor
-- ---------------------------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pb_journal_actor (
  decision_id BIGINT UNSIGNED NOT NULL,
  boot_id INT UNSIGNED NOT NULL,
  at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL,
  idx SMALLINT UNSIGNED NOT NULL,
  side TINYINT UNSIGNED NOT NULL,
  name VARCHAR(48) NOT NULL DEFAULT '',
  low INT UNSIGNED NOT NULL DEFAULT 0,
  entry INT UNSIGNED NOT NULL DEFAULT 0,
  spawn_id INT UNSIGNED NOT NULL DEFAULT 0,
  level SMALLINT UNSIGNED NULL,
  spec VARCHAR(24) NULL,
  role VARCHAR(8) NULL,
  health INT UNSIGNED NULL,
  health_max INT UNSIGNED NULL,
  health_pct FLOAT NULL,
  mana INT UNSIGNED NULL,
  mana_max INT UNSIGNED NULL,
  mana_pct FLOAT NULL,
  pos_x FLOAT NULL,
  pos_y FLOAT NULL,
  pos_z FLOAT NULL,
  tank_yd FLOAT NULL,
  target_yd FLOAT NULL,
  in_combat TINYINT UNSIGNED NULL,
  wanders TINYINT UNSIGNED NULL,
  opener_ready TINYINT UNSIGNED NULL,
  doing VARCHAR(96) NULL,
  checks VARCHAR(255) NULL,
  chk_reaches_spot TINYINT UNSIGNED NULL,
  chk_pull_path TINYINT UNSIGNED NULL,
  chk_walk TINYINT UNSIGNED NULL,
  chk_walks_into_fight TINYINT UNSIGNED NULL,
  note VARCHAR(48) NOT NULL DEFAULT '',
  build VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (decision_id, idx, hour_bucket),
  KEY at_ms (at_ms),
  KEY who_at (low, at_ms),
  KEY entry_at (entry, at_ms),
  KEY spawn_at (spawn_id, at_ms),
  KEY side_health (side, health_pct, at_ms),
  KEY side_mana (side, mana_pct, at_ms),
  KEY role_at (role, at_ms),
  KEY level_at (level, at_ms),
  KEY checks_at (chk_reaches_spot, chk_pull_path, chk_walk, at_ms),
  KEY build_at (build, at_ms)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn20] every participant of a decision and its state - NULL means not given' PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

-- ---------------------------------------------------------------------------------------------------------------
-- pb_journal_route
-- ---------------------------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pb_journal_route (
  id BIGINT UNSIGNED NOT NULL,
  boot_id INT UNSIGNED NOT NULL,
  at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL,
  event VARCHAR(12) NOT NULL,
  purpose VARCHAR(48) NOT NULL DEFAULT '',
  target VARCHAR(96) NOT NULL DEFAULT '',
  target_entry INT UNSIGNED NOT NULL DEFAULT 0,
  target_low INT UNSIGNED NOT NULL DEFAULT 0,
  why VARCHAR(512) NOT NULL DEFAULT '',
  verdict VARCHAR(16) NOT NULL DEFAULT '',
  bot_name VARCHAR(32) NOT NULL DEFAULT '',
  bot_low INT UNSIGNED NOT NULL DEFAULT 0,
  map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  zone_id INT UNSIGNED NOT NULL DEFAULT 0,
  from_x FLOAT NOT NULL DEFAULT 0,
  from_y FLOAT NOT NULL DEFAULT 0,
  from_z FLOAT NOT NULL DEFAULT 0,
  to_x FLOAT NOT NULL DEFAULT 0,
  to_y FLOAT NOT NULL DEFAULT 0,
  to_z FLOAT NOT NULL DEFAULT 0,
  path_yd FLOAT NOT NULL DEFAULT 0,
  straight_yd FLOAT NOT NULL DEFAULT 0,
  stop_count SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  changed_stop SMALLINT UNSIGNED NULL,
  changed_why VARCHAR(255) NOT NULL DEFAULT '',
  run_id VARCHAR(48) NOT NULL DEFAULT '',
  decision_id BIGINT UNSIGNED NOT NULL DEFAULT 0,
  build VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (id, hour_bucket),
  KEY at_ms (at_ms),
  KEY bot_at (bot_low, at_ms),
  KEY event_at (event, at_ms),
  KEY run_at (run_id, at_ms),
  KEY purpose_at (purpose, at_ms),
  KEY verdict_at (verdict, at_ms),
  KEY target_at (target_entry, target_low, at_ms),
  KEY decision (decision_id),
  KEY build_at (build, at_ms)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn20] a route as events: planned, changed, leg started, leg ended' PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

-- ---------------------------------------------------------------------------------------------------------------
-- pb_journal_route_stop
-- ---------------------------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pb_journal_route_stop (
  route_id BIGINT UNSIGNED NOT NULL,
  boot_id INT UNSIGNED NOT NULL,
  at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL,
  seq SMALLINT UNSIGNED NOT NULL,
  pos_x FLOAT NOT NULL DEFAULT 0,
  pos_y FLOAT NOT NULL DEFAULT 0,
  pos_z FLOAT NOT NULL DEFAULT 0,
  kind VARCHAR(16) NOT NULL DEFAULT '',
  what VARCHAR(96) NOT NULL DEFAULT '',
  entry INT UNSIGNED NOT NULL DEFAULT 0,
  low INT UNSIGNED NOT NULL DEFAULT 0,
  verdict VARCHAR(16) NOT NULL DEFAULT '',
  why VARCHAR(512) NOT NULL DEFAULT '',
  build VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (route_id, seq, hour_bucket),
  KEY at_ms (at_ms),
  KEY kind_at (kind, at_ms),
  KEY verdict_at (verdict, at_ms),
  KEY what_at (entry, low, at_ms)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn20] one stop of a route: where, what is there, and its own verdict' PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

-- ---------------------------------------------------------------------------------------------------------------
-- pb_journal_line
-- ---------------------------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pb_journal_line (
  id BIGINT UNSIGNED NOT NULL,
  boot_id INT UNSIGNED NOT NULL,
  at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL,
  tag VARCHAR(24) NOT NULL DEFAULT '',
  bot_low INT UNSIGNED NOT NULL DEFAULT 0,
  bot_name VARCHAR(32) NOT NULL DEFAULT '',
  map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  zone_id INT UNSIGNED NOT NULL DEFAULT 0,
  pos_x FLOAT NOT NULL DEFAULT 0,
  pos_y FLOAT NOT NULL DEFAULT 0,
  pos_z FLOAT NOT NULL DEFAULT 0,
  text TEXT NOT NULL,
  build VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (id, hour_bucket),
  KEY tag_at (tag, at_ms),
  KEY bot_at (bot_low, at_ms),
  KEY build_at (build, at_ms)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[jrn20] the raw stream - the file stays the live stream, this is the history' PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

-- ---------------------------------------------------------------------------------------------------------------
-- THE QUERIES THE INDEXES ARE FOR. Each one is named so that an index nobody queries can be found and dropped, and
-- so that a question nobody indexed can be found and indexed. Run them with --database acore_playerbots.
-- ---------------------------------------------------------------------------------------------------------------
--
-- 1. "Why was bot X standing at that moment"           -> judge_name_at, then the options of the answers found
--    SELECT d.at_ms, d.verdict, d.why, o.name, o.reason
--      FROM pb_journal_decision d LEFT JOIN pb_journal_option o ON o.decision_id = d.id
--     WHERE d.judge_name = 'Relara' AND d.at_ms BETWEEN ? AND ? ORDER BY d.at_ms;
--
-- 2. "Every refusal of this kind over a window"        -> verdict_at (verdict, topic, at_ms)
--    SELECT at_ms, why, COUNT(*) FROM pb_journal_decision
--     WHERE verdict = 'held' AND topic = 'pull' AND at_ms BETWEEN ? AND ? GROUP BY why ORDER BY 3 DESC;
--
-- 3. "Share of decisions that ended in a refusal, by kind"  -> topic_at
--    SELECT topic, COUNT(*) total, SUM(verdict <> 'taken') refused,
--           ROUND(100 * SUM(verdict <> 'taken') / COUNT(*), 1) pct
--      FROM pb_journal_decision WHERE at_ms BETWEEN ? AND ? GROUP BY topic ORDER BY refused DESC;
--
-- 4. "A verdict nobody reads"                          -> read_by_at, and the LEFT JOIN that returns no reader
--    SELECT d.topic, d.tag, COUNT(*) FROM pb_journal_decision d
--      LEFT JOIN pb_journal_read r ON r.decision_id = d.id
--     WHERE d.at_ms BETWEEN ? AND ? AND r.reader IS NULL AND d.read_by = '' GROUP BY 1, 2 ORDER BY 3 DESC;
--
-- 5. "Share of clean runs over the last 30 for a dungeon"  -> guild_project_runs, NOT a second runs table.
--    SELECT AVG(result = 'clear') FROM (SELECT result FROM guild_project_runs
--      WHERE content = 'wc-n' ORDER BY id DESC LIMIT 30) t;      -- ORDER BY id, because `at` is the START time
--
-- 6. "What dropped in that run and where it went"      -> run_at on pb_journal_loot
--    SELECT item_name, count, from_name, to_name, verdict, why FROM pb_journal_loot
--     WHERE run_id = ? ORDER BY at_ms;
--
-- 7. "What changed between build N and N+1"            -> name_build on pb_journal_metric, build on the others
--    SELECT name, build, COUNT(*) n, AVG(value) FROM pb_journal_metric
--     WHERE build IN ('b494', 'b495') GROUP BY name, build ORDER BY name, build;
--
-- 8. "Where a priced option's number came from, and whether a drop source was considered at all" -> name_at,
--    source_at. This is the Mageweave question: the rate, the source chosen, and the sources NOT chosen.
--    SELECT d.choice, d.verdict, d.why, o.name, o.source_kind, o.cost, o.value, o.taken, o.reason
--      FROM pb_journal_decision d JOIN pb_journal_option o ON o.decision_id = d.id
--     WHERE d.topic = 'profession_plan' AND o.name LIKE '%Mageweave%' ORDER BY d.at_ms, o.idx;
--    -- no row with source_kind = 'drop' is itself the answer: the drop source was never weighed.
--
-- 9. "The pull options a leader rejected inside one run"    -> run_at + decision_id
--    SELECT d.run_ms, d.verdict, o.kind, o.name, o.reason, o.cost FROM pb_journal_decision d
--      JOIN pb_journal_option o ON o.decision_id = d.id
--     WHERE d.run_id = ? AND d.topic = 'pull' AND o.taken = 0 ORDER BY d.run_ms, o.idx;
--
-- 10. "One bot's whole stream around a moment"         -> bot_at on pb_journal_line
--     SELECT at_ms, tag, text FROM pb_journal_line
--      WHERE bot_low = ? AND at_ms BETWEEN ? AND ? ORDER BY at_ms;
