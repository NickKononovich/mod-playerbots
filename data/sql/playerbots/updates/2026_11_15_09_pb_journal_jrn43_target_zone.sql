-- [jrn43] pb_journal_stuck.target_zone_id: the ZONE of the row's target - for `wait_behind`, of the member walked to.
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Journal.cpp reconciles a live table to the schema the code declares on every start (CreateOrExtend, [jrn32]), so
-- this file is the same migration for a database the module does not start against. The ALTER is guarded on the new
-- column over information_schema: MySQL 8 has no ADD COLUMN IF NOT EXISTS (that is MariaDB), so running this twice
-- has to be made to do nothing by a query and not by a flag on the statement. Nothing is dropped.
--
-- WHY THE COLUMN EXISTS
--
-- The `wait_behind` kind had exactly one cause field in its deduplication key, and that field could not come out
-- false: the emitter ([wait14], NewRpgAction.cpp) walks to meet a member only when
-- `member->GetMapId() == bot->GetMapId()`, while the key asked whether the two were on one map. So it read
-- "same map" in all 43 live rows of b502 and in every row the kind could ever write - and a statement that cannot
-- be false is counted in this project as a defect of its own, because a query over it answers.
--
-- What it hid, measured on b502: 43 rows, one issue row, 38 occurrences, 14 waiting bots, 16 members waited for,
-- 7 zones, and the distance from 84 yd to 6 894 yd - a follow that has not caught up and a member a continent away
-- were one problem. 31 of the 43 were between 100 and 1 000 yd, 10 above 1 000 yd.
--
-- The zone is the same fact one step finer, it is read from the member at the same instant as everything else in
-- the row, and it does differ. The key is now `wait_behind|same zone` / `wait_behind|another zone`
-- (Journal::Key::WaitBehind, asserted by a test over both values).
--
-- AND THE COLUMN IS WHY THE SPLIT CAN BE FAULTED. The row carried the BOT's zone only, so nobody could check by a
-- query whether the key's new field was taken correctly - a cause field no query can verify is a comment. NULL
-- means "the emitter did not pass it" (the other three kinds of this table do not), which is not zone 0.
--
-- HOW TO CHECK IT RATHER THAN READ IT. pb_journal_stuck must have 25 columns, which is the number of fields in its
-- tuple (COLUMNS_STUCK in Journal.cpp and the {} count of its Push format):
--   SELECT COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_stuck';
-- The split itself, on the next run:
--   SELECT i.dedup_key, COUNT(*), MIN(s.target_yd), MAX(s.target_yd)
--     FROM pb_journal_issue i JOIN pb_journal_stuck s ON s.id = i.source_id
--    WHERE i.kind = 'wait_behind' GROUP BY i.dedup_key;
-- Beware `mysql ... | grep -v Warning`: that returns GREP's exit status, so rc=1 means "grep matched nothing" and
-- not "the DDL failed". Success is established by querying information_schema, never by a pipeline's exit code.

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_stuck
     ADD COLUMN target_zone_id INT UNSIGNED NULL,
     ADD KEY target_zone_at (target_zone_id, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_stuck' AND COLUMN_NAME = 'target_zone_id');
PREPARE jrn43 FROM @stmt; EXECUTE jrn43; DEALLOCATE PREPARE jrn43;
