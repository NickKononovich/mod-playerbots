-- [jrn30]/[jrn31]/[jrn32] The journal's own instrument, repaired: the events it had no row for, the judge's
-- numbers out of JSON and into columns, server statistics, and the schema-drift trap closed.
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Every statement is either CREATE TABLE IF NOT EXISTS or an ALTER guarded on information_schema, so running it
-- twice does nothing the second time. Nothing is dropped and no other owner's table is touched: this file does NOT
-- touch playerbots_fight_history (the fight measurements stay their owner's) and does not touch guild_project_runs
-- or guild_project_cert - the journal links to those by id and never copies them.
--
-- WHY THE GUARDED ALTERS EXIST, and it is the most expensive lesson in this file.
--
-- `CREATE TABLE IF NOT EXISTS` on a table that already exists does NOTHING, and reports success. So the moment a
-- build adds a column to the code's INSERT while the live table was made by an older build, every insert of that
-- table fails with "Unknown column" - and because the journal's writes are asynchronous, the error goes to the
-- database log and the table simply stays EMPTY while the code is entirely correct. That is precisely the failure
-- the whole journal exists to prevent: an instrument reading zero, and the zero not being about the world.
--
-- It had already happened and was caught by query, not by reading code: pb_journal_decision had 28 columns in the
-- live database while Record() wrote a 37-field tuple (method, pack_size, careful, judge_x/y/z, kind,
-- route_target_entry, route_target_low added by the previous build). The only two ALTERs anywhere in Journal.cpp
-- were about partitions. Journal.cpp now reconciles a live table to the schema the code declares, reading the
-- column list out of the very CREATE statement it just issued (CreateOrExtend) so the two cannot drift; this file
-- is the same migration for a database the module does not start against.
--
-- HOW TO CHECK IT RATHER THAN READ IT. For every table the number of columns in the live database must equal the
-- number of fields in that table's tuple (the COLUMNS_* lists in Journal.cpp):
--   SELECT TABLE_NAME, COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME LIKE 'pb_journal%' GROUP BY TABLE_NAME;
-- And beware: `mysql ... | grep -v Warning` returns GREP's exit status, so rc=1 means "grep matched nothing", not
-- "the DDL failed". Success is checked by querying information_schema, never by an exit code.
--
-- WHAT THIS FILE ADDS, and the measurement behind each one (all taken on the live journal, a 2.69 h window with
-- 411 819 stream rows and 9 457 decisions):
--
--  * pb_journal_fight - A FIGHT'S START. playerbots_fight_history held 9 467 finished fights in that window and the
--    journal held NOT ONE beginning: a fight's start was reachable only through a sampler running about every 22 s
--    and catching roughly half the bots. The row is written at the combat-state transition the core announces
--    (OnPlayerEnterCombat fires only on not-in-combat -> in-combat), so it is one row per fight per bot with no
--    clock of its own. The fight's END stays in playerbots_fight_history: one quantity, one owner.
--
--  * pb_journal_aggro - AGGRO GAINED, with the REASON AS A CODE in its own indexed column. "An error of the
--    estimate" was not merely unrecorded, it was inexpressible: nothing said why a mob was on a bot, so the one
--    question worth asking could not be asked. Codes: came | path | estimate | off_tank | add | unknown.
--    "off_tank" is declared and has NO emitter yet (a mid-fight threat switch is not an engage; it needs a watch in
--    the leader's own tick), so a query finding none of it must read that as "not measured", not "never happens".
--
--  * pb_journal_death - A REAL DEATH, where a prevented one cannot reach it. 20 502 stream lines in that window
--    carried a tag beginning with "[death" and 17 of them were deaths: the rest is the death-PREVENTION domain
--    printing under its own numbered tags ("[death384] holds its hit at ...", "[deaths20] within the reach of ...")
--    about a death that did NOT happen. A count by tag family overstated deaths by about 1 200 times, and
--    `LIKE '[death%'` is exactly the query shape that produces it. The count is now COUNT(*) over this table.
--    A death is a CALCULATION BUG in this project, so the row carries the judge's inputs and not only the outcome.
--
--  * pb_journal_option + 20 columns - THE CHAIN-PULL JUDGE'S NUMBERS. They lived only inside `fields`, and
--    "every refusal where the healer had more than N yards to walk" is a query we want to ask. A LIKE over `text`
--    was measured at 384 ms over 411 819 rows and grows linearly (about 8 s on a day of a thousand bots), while an
--    indexed column answers in single-digit milliseconds. `fields` keeps what is read with the eyes.
--
--  * pb_journal_decision + map_id, zone_id - WHERE the judge stood, as the map says it. The user asked for it in so
--    many words, and "what happens in this zone" is a question that gets asked.
--
--  * pb_journal_metric + value_min/value_max/value_p95/samples/bots/sessions - THE MEAN HIDES THE SPIKE. A mean of
--    50 ms over a window holding one 900 ms tick is 50 ms, and the 900 ms is what the bots stand on; a row with
--    only the mean draws a reassuring flat line over the defect. `bots` and `sessions` are in the row because a
--    tick time at 75 bots and one at 300 are not the same number, and nothing else in the row would say so.
--
-- NULL MEANS "THE EMITTER DID NOT SUPPLY IT" AND IS NOT ZERO, everywhere below. A mob at 0 % health is a corpse; a
-- mob whose health was never recorded is a gap, and a query that cannot tell them apart will answer anyway.
--
-- The partitioning clause is identical to the one Journal.cpp uses: one catch-all partition at creation, and the
-- writer's sweep carves the hours out of it and drops the hours past the retention.

-- ---------------------------------------------------------------------------------------------------------------
-- 1. The three new tables.
-- ---------------------------------------------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS pb_journal_fight (
  id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL, event VARCHAR(12) NOT NULL DEFAULT '',
  bot_name VARCHAR(32) NOT NULL DEFAULT '', bot_low INT UNSIGNED NOT NULL DEFAULT 0,
  bot_level SMALLINT UNSIGNED NULL, role VARCHAR(8) NULL, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  zone_id INT UNSIGNED NOT NULL DEFAULT 0, area_id INT UNSIGNED NOT NULL DEFAULT 0,
  pos_x FLOAT NOT NULL DEFAULT 0, pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0,
  in_dungeon TINYINT UNSIGNED NOT NULL DEFAULT 0, enemy_name VARCHAR(48) NOT NULL DEFAULT '',
  enemy_entry INT UNSIGNED NOT NULL DEFAULT 0, enemy_low INT UNSIGNED NOT NULL DEFAULT 0,
  enemy_level SMALLINT UNSIGNED NULL, mobs SMALLINT UNSIGNED NULL,
  puller_name VARCHAR(32) NOT NULL DEFAULT '', puller_low INT UNSIGNED NOT NULL DEFAULT 0,
  pull_kind VARCHAR(12) NOT NULL DEFAULT '', pull_method VARCHAR(16) NOT NULL DEFAULT '',
  pull_expected SMALLINT UNSIGNED NULL, pull_cost DOUBLE NOT NULL DEFAULT 0,
  pull_order_id INT UNSIGNED NOT NULL DEFAULT 0, run_id VARCHAR(48) NOT NULL DEFAULT '',
  decision_id BIGINT UNSIGNED NOT NULL DEFAULT 0, build VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY bot_at (bot_low, at_ms),
  KEY enemy_at (enemy_entry, enemy_low, at_ms), KEY where_at (map_id, zone_id, at_ms),
  KEY event_at (event, at_ms), KEY pull_kind_at (pull_kind, at_ms), KEY puller_at (puller_low, at_ms),
  KEY mobs_at (mobs, at_ms), KEY run_at (run_id, at_ms), KEY build_at (build, at_ms),
  KEY decision (decision_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='[jrn30] a fight BEGINS: who, where, against whom, how many, pulled by whom'
  PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

CREATE TABLE IF NOT EXISTS pb_journal_aggro (
  id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL, bot_name VARCHAR(32) NOT NULL DEFAULT '',
  bot_low INT UNSIGNED NOT NULL DEFAULT 0, role VARCHAR(8) NULL, bot_health_pct FLOAT NULL,
  bot_in_combat TINYINT UNSIGNED NULL, bot_moving TINYINT UNSIGNED NULL,
  mob_name VARCHAR(48) NOT NULL DEFAULT '', mob_entry INT UNSIGNED NOT NULL DEFAULT 0,
  mob_low INT UNSIGNED NOT NULL DEFAULT 0, mob_level SMALLINT UNSIGNED NULL,
  map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, zone_id INT UNSIGNED NOT NULL DEFAULT 0,
  pos_x FLOAT NOT NULL DEFAULT 0, pos_y FLOAT NOT NULL DEFAULT 0, pos_z FLOAT NOT NULL DEFAULT 0,
  reason VARCHAR(16) NOT NULL DEFAULT '', why VARCHAR(512) NOT NULL DEFAULT '',
  tank_name VARCHAR(32) NOT NULL DEFAULT '', tank_low INT UNSIGNED NOT NULL DEFAULT 0,
  expected TINYINT UNSIGNED NULL, order_target_entry INT UNSIGNED NOT NULL DEFAULT 0,
  order_target_low INT UNSIGNED NOT NULL DEFAULT 0, run_id VARCHAR(48) NOT NULL DEFAULT '',
  decision_id BIGINT UNSIGNED NOT NULL DEFAULT 0, build VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY reason_at (reason, at_ms),
  KEY bot_at (bot_low, at_ms), KEY mob_at (mob_entry, mob_low, at_ms),
  KEY where_at (map_id, zone_id, at_ms), KEY expected_at (expected, at_ms), KEY tank_at (tank_low, at_ms),
  KEY reason_bot (reason, bot_low, at_ms), KEY run_at (run_id, at_ms), KEY build_at (build, at_ms),
  KEY decision (decision_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='[jrn30] aggro gained, with the REASON as a code: came/path/estimate/add'
  PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

CREATE TABLE IF NOT EXISTS pb_journal_death (
  id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL, bot_name VARCHAR(32) NOT NULL DEFAULT '',
  bot_low INT UNSIGNED NOT NULL DEFAULT 0, bot_level SMALLINT UNSIGNED NULL,
  bot_class SMALLINT UNSIGNED NULL, role VARCHAR(8) NULL, spec VARCHAR(24) NULL,
  map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, zone_id INT UNSIGNED NOT NULL DEFAULT 0,
  area_id INT UNSIGNED NOT NULL DEFAULT 0, pos_x FLOAT NOT NULL DEFAULT 0, pos_y FLOAT NOT NULL DEFAULT 0,
  pos_z FLOAT NOT NULL DEFAULT 0, in_dungeon TINYINT UNSIGNED NOT NULL DEFAULT 0,
  killer_name VARCHAR(48) NOT NULL DEFAULT '', killer_entry INT UNSIGNED NOT NULL DEFAULT 0,
  killer_low INT UNSIGNED NOT NULL DEFAULT 0, killer_level SMALLINT UNSIGNED NULL,
  killer_health_pct FLOAT NULL, health_max INT UNSIGNED NULL, damage_from_killer INT UNSIGNED NULL,
  attackers SMALLINT UNSIGNED NULL, judged_pull TINYINT UNSIGNED NULL,
  expected VARCHAR(512) NOT NULL DEFAULT '', pull_expected SMALLINT UNSIGNED NULL,
  pull_method VARCHAR(16) NOT NULL DEFAULT '', context VARCHAR(1024) NOT NULL DEFAULT '',
  threat VARCHAR(1024) NOT NULL DEFAULT '', run_id VARCHAR(48) NOT NULL DEFAULT '',
  decision_id BIGINT UNSIGNED NOT NULL DEFAULT 0, build VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY bot_at (bot_low, at_ms),
  KEY killer_at (killer_entry, killer_low, at_ms), KEY where_at (map_id, zone_id, at_ms),
  KEY judged_at (judged_pull, at_ms), KEY level_at (bot_level, at_ms), KEY dungeon_at (in_dungeon, at_ms),
  KEY run_at (run_id, at_ms), KEY build_at (build, at_ms), KEY decision (decision_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='[jrn30] a real death with the judges inputs - prevention is never written here'
  PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

-- ---------------------------------------------------------------------------------------------------------------
-- 2. The columns an older table is missing. Each ALTER is guarded on one sentinel column of its own set, so this
--    migration either runs whole or does nothing - MySQL 8 has no ADD COLUMN IF NOT EXISTS, which is why the guard
--    is a prepared statement over information_schema rather than a flag on the statement.
-- ---------------------------------------------------------------------------------------------------------------

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_decision
     ADD COLUMN map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0,
     ADD COLUMN zone_id INT UNSIGNED NOT NULL DEFAULT 0,
     ADD KEY where_at (map_id, zone_id, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_decision' AND COLUMN_NAME = 'map_id');
PREPARE jrn30 FROM @stmt; EXECUTE jrn30; DEALLOCATE PREPARE jrn30;

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_option
     ADD COLUMN fight_sec_left FLOAT NULL,
     ADD COLUMN run_in_sec FLOAT NULL,
     ADD COLUMN open_sec FLOAT NULL,
     ADD COLUMN opener VARCHAR(48) NOT NULL DEFAULT '''',
     ADD COLUMN distance FLOAT NULL,
     ADD COLUMN overload FLOAT NULL,
     ADD COLUMN drop_pct FLOAT NULL,
     ADD COLUMN tank_health FLOAT NULL,
     ADD COLUMN union_sec FLOAT NULL,
     ADD COLUMN group_dps FLOAT NULL,
     ADD COLUMN walk_sec FLOAT NULL,
     ADD COLUMN walk_yd FLOAT NULL,
     ADD COLUMN healer VARCHAR(32) NOT NULL DEFAULT '''',
     ADD COLUMN healer_walks TINYINT UNSIGNED NULL,
     ADD COLUMN healer_sec FLOAT NULL,
     ADD COLUMN due TINYINT UNSIGNED NULL,
     ADD COLUMN stop SMALLINT UNSIGNED NULL,
     ADD COLUMN pack_mobs SMALLINT UNSIGNED NULL,
     ADD COLUMN fight_mobs SMALLINT UNSIGNED NULL,
     ADD COLUMN union_mobs SMALLINT UNSIGNED NULL,
     ADD KEY drop_at (drop_pct, at_ms),
     ADD KEY overload_at (overload, at_ms),
     ADD KEY walk_at (walk_yd, at_ms),
     ADD KEY healer_walk_at (healer_walks, healer_sec, at_ms),
     ADD KEY union_at (union_mobs, at_ms),
     ADD KEY fight_left_at (fight_sec_left, at_ms),
     ADD KEY tank_health_at (tank_health, at_ms),
     ADD KEY opener_at (opener, at_ms),
     ADD KEY due_at (due, at_ms),
     ADD KEY stop_at (stop, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_option' AND COLUMN_NAME = 'fight_sec_left');
PREPARE jrn30 FROM @stmt; EXECUTE jrn30; DEALLOCATE PREPARE jrn30;

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_metric
     ADD COLUMN value_min DOUBLE NULL,
     ADD COLUMN value_max DOUBLE NULL,
     ADD COLUMN value_p95 DOUBLE NULL,
     ADD COLUMN samples INT UNSIGNED NULL,
     ADD COLUMN bots INT UNSIGNED NULL,
     ADD COLUMN sessions INT UNSIGNED NULL,
     ADD KEY name_max (name, value_max, at_ms),
     ADD KEY name_p95 (name, value_p95, at_ms),
     ADD KEY name_value (name, value, at_ms),
     ADD KEY name_bots (name, bots, at_ms),
     ADD KEY scope_name (scope, name, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_metric' AND COLUMN_NAME = 'value_max');
PREPARE jrn30 FROM @stmt; EXECUTE jrn30; DEALLOCATE PREPARE jrn30;

-- The columns the build before this one added to pb_journal_decision and never migrated: this is the repair of the
-- drift described at the top. Guarded the same way, so a database that already has them is untouched.
SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_decision
     ADD COLUMN method VARCHAR(24) NOT NULL DEFAULT '''',
     ADD COLUMN pack_size SMALLINT UNSIGNED NULL,
     ADD COLUMN careful TINYINT UNSIGNED NULL,
     ADD COLUMN judge_x FLOAT NULL,
     ADD COLUMN judge_y FLOAT NULL,
     ADD COLUMN judge_z FLOAT NULL,
     ADD COLUMN kind VARCHAR(24) NOT NULL DEFAULT '''',
     ADD COLUMN route_target_entry INT UNSIGNED NOT NULL DEFAULT 0,
     ADD COLUMN route_target_low INT UNSIGNED NOT NULL DEFAULT 0,
     ADD KEY method_at (method, at_ms),
     ADD KEY pack_at (pack_size, at_ms),
     ADD KEY careful_at (careful, at_ms),
     ADD KEY choice_at (choice(64), at_ms),
     ADD KEY judge_spot (judge_x, judge_y),
     ADD KEY kind_at (kind, at_ms),
     ADD KEY build_topic (build, topic, at_ms),
     ADD KEY route_target_at (route_target_entry, route_target_low, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_decision' AND COLUMN_NAME = 'method');
PREPARE jrn30 FROM @stmt; EXECUTE jrn30; DEALLOCATE PREPARE jrn30;
