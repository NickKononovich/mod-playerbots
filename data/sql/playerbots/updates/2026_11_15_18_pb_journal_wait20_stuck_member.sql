-- [wait20] pb_journal_stuck: where the OTHER one is, whether it walks, where to, and the status it runs.
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Journal.cpp reconciles a live table to the schema the code declares on every start (CreateOrExtend, [jrn32]), so
-- a server that has already started on this build has added these columns AND these indexes itself, and a bare
-- ALTER here would then fail with "Duplicate column name" and stop the server coming up. Hence every statement is
-- guarded by a query over information_schema - the columns on one sentinel, each index on its own name - so running
-- this file twice does nothing the second time. Nothing is dropped. MySQL 8 has no ADD COLUMN IF NOT EXISTS (that
-- is MariaDB), which is why the guard is a query and not a flag.
--
-- WHY THE COLUMNS EXIST
--
-- `wait_behind` is written when a member is beyond the group reward distance. Until now the leader WALKED to it,
-- and the row carried the gap and nothing at all about the other end of that gap. Measured over the live rows:
-- 17 of 19 cases were a member more than a KILOMETRE away - Rollothan for Alnia 4 038 yd (16 rows of one episode),
-- Lanbuzz for Nelaeleas 4 037 yd inside one zone, Relara for Dekorhan 6 894 yd - and one episode ran 1 073 s.
-- Four kilometres is not a lag that walking closes: it is the other end of the world, and whatever holds the
-- member there is a cause of its own at that end. The user's decision (2026-11-15): do not walk to meet it.
-- So the walk is gone and the record replaces it, and a record of a gap has to be able to say where the other one
-- is and what it is doing - otherwise it names no cause and the row is only a number.
--
-- WHY COLUMNS AND NOT A PHRASE IN `why`: the questions are "what were they doing when this happened", "were they
-- walking at all", "where to" and "did it change between builds" - filters, groupings and comparisons, which in
-- this project means columns with indexes paired with time, never a LIKE over text (rule 1).
--
-- NULL means the emitter did not pass it (a member off the world, on another map), and it is NOT the point
-- (0, 0, 0) and NOT "standing": absence is not zero (rule 7).
--
-- HOW TO CHECK IT RATHER THAN READ IT. pb_journal_stuck must have 33 columns - the number of names in COLUMNS_STUCK
-- in Journal.cpp and of {} in its Push format (tools/jrn_fmt_check.py counts both):
--   SELECT COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_stuck';
--   SELECT INDEX_NAME FROM information_schema.STATISTICS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_stuck'
--      AND INDEX_NAME IN ('target_doing_at', 'target_moving_at');
-- And the question they were added for, on the next run - what the members left behind were actually doing:
--   SELECT target_doing, target_moving, COUNT(*) cases, ROUND(MAX(target_yd)) farthest
--     FROM pb_journal_stuck WHERE kind = 'wait_behind' GROUP BY 1, 2 ORDER BY cases DESC;
-- Beware `mysql ... | grep -v Warning`: that returns GREP's exit status, so rc=1 means "grep matched nothing" and
-- not "the DDL failed". Success is established by querying information_schema, never by a pipeline's exit code.

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_stuck
     ADD COLUMN target_x FLOAT NULL,
     ADD COLUMN target_y FLOAT NULL,
     ADD COLUMN target_z FLOAT NULL,
     ADD COLUMN target_moving TINYINT UNSIGNED NULL,
     ADD COLUMN target_to_x FLOAT NULL,
     ADD COLUMN target_to_y FLOAT NULL,
     ADD COLUMN target_to_z FLOAT NULL,
     ADD COLUMN target_doing VARCHAR(32) NULL')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_stuck' AND COLUMN_NAME = 'target_doing');
PREPARE wait20a FROM @stmt; EXECUTE wait20a; DEALLOCATE PREPARE wait20a;

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_stuck ADD KEY target_doing_at (target_doing, at_ms)')
  FROM information_schema.STATISTICS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_stuck' AND INDEX_NAME = 'target_doing_at');
PREPARE wait20b FROM @stmt; EXECUTE wait20b; DEALLOCATE PREPARE wait20b;

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_stuck ADD KEY target_moving_at (target_moving, at_ms)')
  FROM information_schema.STATISTICS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_stuck' AND INDEX_NAME = 'target_moving_at');
PREPARE wait20c FROM @stmt; EXECUTE wait20c; DEALLOCATE PREPARE wait20c;
