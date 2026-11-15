-- [jrn50] pb_journal_route.to_x / to_y / to_z become NULLABLE.
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Nothing is dropped and no row is rewritten: a MODIFY that only relaxes NOT NULL keeps every existing value.
--
-- WHY
--
-- A leg can end with its target gone from the world - killed, or despawned - and then there is no place left to
-- write. The columns were NOT NULL DEFAULT 0, so that case landed as the point (0, 0, 0), which is a statement
-- about geometry and not about absence. That is trap 7 of this project ("absence is not zero") appearing in a
-- brand new column, and the row was only distinguishable by `straight_yd` coming out negative - an INDIRECT
-- marker, which holds exactly until somebody tidies that one up as well.
--
-- Note that Journal.cpp's CreateOrExtend ([jrn32]) can only ADD what the live table lacks; it never relaxes an
-- existing column. So on a database that already carries this table, THIS FILE is the only thing that changes it,
-- and the code's own DDL covers only a database created from scratch. Both are needed, and they are written to say
-- the same thing.
--
-- HOW TO CHECK IT RATHER THAN READ IT:
--   SELECT COLUMN_NAME, IS_NULLABLE, COLUMN_DEFAULT FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_route'
--      AND COLUMN_NAME IN ('to_x', 'to_y', 'to_z');
-- Expected: three rows, IS_NULLABLE = YES. Beware `mysql ... | grep -v Warning`: that returns GREP's status.
--
-- And what the next run must show - the legs that ended with nowhere to point at, told apart from the rest by the
-- column itself and not by a second number:
--   SELECT verdict, COUNT(*) legs, SUM(to_x IS NULL) target_gone FROM pb_journal_route
--    WHERE event = 'leg_end' GROUP BY verdict;

SET @stmt := (SELECT IF(COUNT(*) = 3, 'DO 0',
  'ALTER TABLE pb_journal_route
     MODIFY COLUMN to_x FLOAT NULL,
     MODIFY COLUMN to_y FLOAT NULL,
     MODIFY COLUMN to_z FLOAT NULL')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_route'
   AND COLUMN_NAME IN ('to_x', 'to_y', 'to_z') AND IS_NULLABLE = 'YES');
PREPARE jrn50b FROM @stmt; EXECUTE jrn50b; DEALLOCATE PREPARE jrn50b;
