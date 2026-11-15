-- [mount9] pb_journal_mount: EVERY CHANGE OF A BOT'S MOUNT STATE, with the facts that could justify it.
--
-- The user saw it in the game and named it: "they look stuck, they just stand there, propped up against something,
-- and they keep getting off the mount and back on. That is a plain bug. I need such cases caught in issues."
--
-- WHY THE PROBLEMS TABLE HAD NO SUCH KIND, and this is a TRAP OF ITS OWN rather than an oversight. Getting onto a
-- mount is a CAST. The standing watch's criterion answers honestly that a casting bot is not idle - and worse, a
-- cast ENDS the standing episode, so the duration restarts with every mount. Measured on b504:
--   * Emdyl held ONE point in Stormwind (-8777.3105, 665.5, 103.1), `stood_yd` 0.000 in all 352 rows, for
--     95.4 MINUTES, and the longest episode the standing watch ever reported there was 69.9 s;
--   * 154 of its 462 rows restart at 10.0 s, which is the instrument window and not the bot;
--   * over the whole table, standing rows whose engine trace shows the mount action EXECUTING average 17.8 s
--     against 72.6 s for the rest - four times shorter.
-- THE GENERAL FORM, worth more than this one defect: LEGITIMACY ESTABLISHED INSIDE ONE WINDOW DOES NOT CARRY OVER
-- TO A SEQUENCE OF WINDOWS. One mount is legitimate; the same mount in a circle is not, and a criterion that looks
-- at one window alone cannot tell them apart.
--
-- WHY EVERY JUSTIFYING FACT IS A COLUMN WITH AN INDEX (rule 1). "A dismount for a fight" and "a dismount nothing
-- explains" are two different problems, and the whole criterion of the `mount_flap` issue kind is a comparison of
-- those facts between two changes - so they are cut, grouped and compared, which makes them columns. `why` keeps
-- the words the code that took the bot off used (GuildRiding::NoteDismount) for the eyes only; nothing filters it.
--
-- NULL IS "THE EMITTER DID NOT PASS IT" AND NEVER FALSE (rule 7). `target_in_reach` is NULL when there was no
-- target at all: "nothing to be in reach of" and "a target that is not in reach" are different facts, and the
-- query that merges them answers and lies.
--
-- THE COUNTS OF THE EPISODE ARE HERE TOO (episode_mounts / episode_dismounts / episode_flips) because the issue
-- row keeps one RANGE over its occurrences while each change keeps its own standing, and "the seventh change of
-- that episode" needs both. `episode_flips` is the quantity the problem is made of: a change in the same
-- direction as the previous one, under an identical justification, with the bot still inside
-- max(combat reach, 1.5 yd) of one anchor on one map.
--
-- IDEMPOTENT BY EXECUTION, AND RUN TWICE. The server creates this table itself at startup (Journal's
-- CreateOrExtend compares the code's CREATE against information_schema), so it does not matter whether this file
-- or the build lands first. A bare ALTER after the server has already added a column gives the core updater
-- "ERROR 1060 Duplicate column name" and THE SERVER DOES NOT COME UP - that happened once already, which is why
-- every statement below is guarded by information_schema + PREPARE. And the project's own trap: a successful
-- statement that did nothing is indistinguishable from one that worked (CREATE TABLE IF NOT EXISTS on an existing
-- table said nothing while nine columns were missing), so the column guards run even on a table this file just
-- created, and the file ends with a count that can be read.

CREATE TABLE IF NOT EXISTS pb_journal_mount (
  id BIGINT UNSIGNED NOT NULL, boot_id INT UNSIGNED NOT NULL, at_ms BIGINT UNSIGNED NOT NULL,
  hour_bucket INT UNSIGNED NOT NULL, event VARCHAR(10) NOT NULL DEFAULT '',
  bot_name VARCHAR(32) NOT NULL DEFAULT '', bot_low INT UNSIGNED NOT NULL DEFAULT 0,
  bot_level SMALLINT UNSIGNED NULL, role VARCHAR(8) NULL, map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  zone_id INT UNSIGNED NOT NULL DEFAULT 0, pos_x FLOAT NOT NULL DEFAULT 0, pos_y FLOAT NOT NULL DEFAULT 0,
  pos_z FLOAT NOT NULL DEFAULT 0, mount_spell INT UNSIGNED NOT NULL DEFAULT 0,
  mount_name VARCHAR(48) NOT NULL DEFAULT '', indoors TINYINT UNSIGNED NULL, in_combat TINYINT UNSIGNED NULL,
  target_in_reach TINYINT UNSIGNED NULL, in_water TINYINT UNSIGNED NULL, on_transport TINYINT UNSIGNED NULL,
  moving TINYINT UNSIGNED NULL, has_leader TINYINT UNSIGNED NULL, leader_rides TINYINT UNSIGNED NULL,
  leader_low INT UNSIGNED NOT NULL DEFAULT 0, has_walk TINYINT UNSIGNED NULL, walk_to_x FLOAT NULL,
  walk_to_y FLOAT NULL, walk_left_yd FLOAT NULL, why TEXT NULL,
  episode_mounts INT UNSIGNED NOT NULL DEFAULT 0, episode_dismounts INT UNSIGNED NOT NULL DEFAULT 0,
  episode_flips INT UNSIGNED NOT NULL DEFAULT 0, held_sec FLOAT NULL, held_yd FLOAT NULL,
  justification VARCHAR(128) NOT NULL DEFAULT '', run_id VARCHAR(48) NOT NULL DEFAULT '',
  build VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (id, hour_bucket), KEY at_ms (at_ms), KEY event_at (event, at_ms), KEY bot_at (bot_low, at_ms),
  KEY where_at (map_id, zone_id, at_ms), KEY role_at (role, at_ms), KEY build_at (build, at_ms),
  KEY run_at (run_id, at_ms), KEY just_at (justification, at_ms), KEY flips_at (episode_flips, at_ms),
  KEY leader_at (leader_low, at_ms), KEY walk_at (has_walk, at_ms), KEY reach_at (target_in_reach, at_ms))
  ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='[mount9] every change of a mount state, with the facts that justify it'
  PARTITION BY RANGE (hour_bucket) (PARTITION pfuture VALUES LESS THAN MAXVALUE);

-- THE COLUMN GUARDS. They matter when the table already exists from an older build's column set, and they are
-- the answer to this project's own trap: a successful statement that did nothing is indistinguishable from one
-- that worked (CREATE TABLE IF NOT EXISTS on an existing table said nothing while nine columns were missing).
-- information_schema + PREPARE, with no DELIMITER and no stored procedure: the core's updater runs these files
-- itself and client-only directives are not SQL to it.

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'event');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `event` VARCHAR(10) NOT NULL DEFAULT '' COMMENT '[mount9] mount | dismount | sample: the direction as a value, not as the sign of a number'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'mount_spell');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `mount_spell` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '[mount9] which beast, by identity (rule 6); 0 = it was not on one'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'mount_name');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `mount_name` VARCHAR(48) NOT NULL DEFAULT '' COMMENT '[mount9] the beast name, for the eyes: a name is not an identity'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'indoors');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `indoors` TINYINT UNSIGNED NULL COMMENT '[mount9] justifying fact; NULL = not passed, which is not false'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'in_combat');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `in_combat` TINYINT UNSIGNED NULL COMMENT '[mount9] justifying fact'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'target_in_reach');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `target_in_reach` TINYINT UNSIGNED NULL COMMENT '[mount9] a target inside CheckMountStateAction::DismountBandYd; NULL = no target at all, a different fact'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'in_water');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `in_water` TINYINT UNSIGNED NULL COMMENT '[mount9] justifying fact'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'on_transport');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `on_transport` TINYINT UNSIGNED NULL COMMENT '[mount9] a boat, zeppelin, tram, vehicle or taxi'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'moving');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `moving` TINYINT UNSIGNED NULL COMMENT '[mount9] it was walking: the only state in which a ride has anything to do'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'has_leader');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `has_leader` TINYINT UNSIGNED NULL COMMENT '[mount9] it has a mount leader whose state it copies'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'leader_rides');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `leader_rides` TINYINT UNSIGNED NULL COMMENT '[mount9] and that leader is mounted or casting its mount'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'leader_low');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `leader_low` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '[mount9] the mount leader by guid: one flapping leader against five flapping members is COUNT(DISTINCT leader_low), not a guess'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'has_walk');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `has_walk` TINYINT UNSIGNED NULL COMMENT '[mount9] a walk was in play (NewRpgInfo::mountWalkT)'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'walk_to_x');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `walk_to_x` FLOAT NULL COMMENT '[mount9] where that walk was headed'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'walk_to_y');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `walk_to_y` FLOAT NULL COMMENT '[mount9] where that walk was headed'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'walk_left_yd');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `walk_left_yd` FLOAT NULL COMMENT '[mount9] how far it still had to go: the number MountForWalk judges on'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'why');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `why` TEXT NULL COMMENT '[mount9] the words the code that took it off used (GuildRiding::NoteDismount); empty = nobody claimed the change, a finding and not a gap'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'episode_mounts');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `episode_mounts` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '[mount9] changes onto the mount within this episode of one place'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'episode_dismounts');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `episode_dismounts` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '[mount9] and off it'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'episode_flips');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `episode_flips` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '[mount9] changes that repeated a same-direction justification inside one radius on one map: the quantity the problem is made of, and one is already the proof'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'held_sec');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `held_sec` FLOAT NULL COMMENT '[mount9] how long the episode has held one place'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'held_yd');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `held_yd` FLOAT NULL COMMENT '[mount9] the radius it stayed INSIDE, never the step that took it out'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND COLUMN_NAME = 'justification');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_mount` ADD COLUMN `justification` VARCHAR(128) NOT NULL DEFAULT '' COMMENT '[mount9] the closed set of facts the change stood under (Journal::MountState::Justification); nothing = no fact spoke, which is the worst case'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;


-- THE INDEX GUARDS. Each is paired with time, as every index in this journal is: the cuts this table exists for
-- are the justification, the flip count, the leader, whether a walk was on and whether a target was in reach.

SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND INDEX_NAME = 'event_at');
SET @sql := IF(@idx = 0, 'ALTER TABLE `pb_journal_mount` ADD KEY `event_at` (`event`, `at_ms`)', 'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND INDEX_NAME = 'just_at');
SET @sql := IF(@idx = 0, 'ALTER TABLE `pb_journal_mount` ADD KEY `just_at` (`justification`, `at_ms`)', 'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND INDEX_NAME = 'flips_at');
SET @sql := IF(@idx = 0, 'ALTER TABLE `pb_journal_mount` ADD KEY `flips_at` (`episode_flips`, `at_ms`)', 'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND INDEX_NAME = 'leader_at');
SET @sql := IF(@idx = 0, 'ALTER TABLE `pb_journal_mount` ADD KEY `leader_at` (`leader_low`, `at_ms`)', 'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND INDEX_NAME = 'walk_at');
SET @sql := IF(@idx = 0, 'ALTER TABLE `pb_journal_mount` ADD KEY `walk_at` (`has_walk`, `at_ms`)', 'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount' AND INDEX_NAME = 'reach_at');
SET @sql := IF(@idx = 0, 'ALTER TABLE `pb_journal_mount` ADD KEY `reach_at` (`target_in_reach`, `at_ms`)', 'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- HOW TO CHECK IT RATHER THAN READ IT. pb_journal_mount must have 38 columns, which is the number of fields the
-- code's Push() formats for it, and 13 indexes. Two lists are compared, never one list asked of itself:
--   SELECT COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount';                       -- expect 38
--   SELECT COUNT(DISTINCT INDEX_NAME) FROM information_schema.STATISTICS
--    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_mount';                       -- expect 13
--
-- AND THE QUERY THE NEXT RUN IS JUDGED BY - the one that says whether the fix in CheckMountStateAction holds:
--   SELECT justification, COUNT(*) changes, COUNT(DISTINCT bot_low) bots, MAX(episode_flips) worst
--     FROM pb_journal_mount WHERE build = 'b505' GROUP BY justification ORDER BY changes DESC;
-- On b504 the town case had no record at all; after the fix the row with justification 'nothing' and
-- has_walk = 0 must be GONE, while the walk rows (target_in_reach = 1) stay and keep episode_flips = 0.
