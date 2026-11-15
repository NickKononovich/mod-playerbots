-- [alert20] pb_journal_alert: what legitimately held the bot, and this alert's own magnitude with its unit.
--
-- WHY held_by. The STUCK site asks the one judge of legitimate waits (GuildCoordinator::WaitReason) and skips when
-- it names one; the LOOP site asked nothing at all. Measured over the live corpus by a time-join to the standing
-- watch (+-90 s, same bot_low): 0 of 62 STUCK rows with an identity have a legitimate hold, against 9 of 221 LOOP
-- rows (4.1 %). The judge's answer now rides on the row, so the share is a column cut and not a cross-table join.
-- '' = checked at this instant and nothing held it (a FINDING, as '' is in read_by) - never "not asked".
--
-- WHY value / value_unit. pb_journal_issue.value used to hold the member's "#N" with the unit `member`: an
-- identity in a quantity's column, so a merged group's value_min..value_max were the numbers of two different
-- bots instead of a range. The member's number keeps its own column (`num`). NULL = this site holds no magnitude.
--
-- Idempotent BY EXECUTION, and every statement is guarded through information_schema because the runtime
-- CreateOrExtend adds the columns itself: a bare ALTER here would bring the next start down.
SET @db := DATABASE();

SET @sql := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA=@db AND TABLE_NAME='pb_journal_alert' AND COLUMN_NAME='held_by') = 0,
               'ALTER TABLE pb_journal_alert ADD COLUMN held_by VARCHAR(64) NOT NULL DEFAULT '''' AFTER last_action',
               'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

SET @sql := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA=@db AND TABLE_NAME='pb_journal_alert' AND COLUMN_NAME='value') = 0,
               'ALTER TABLE pb_journal_alert ADD COLUMN value DOUBLE NULL AFTER held_by',
               'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

SET @sql := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA=@db AND TABLE_NAME='pb_journal_alert' AND COLUMN_NAME='value_unit') = 0,
               'ALTER TABLE pb_journal_alert ADD COLUMN value_unit VARCHAR(16) NOT NULL DEFAULT '''' AFTER value',
               'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

SET @sql := IF((SELECT COUNT(*) FROM information_schema.STATISTICS
                 WHERE TABLE_SCHEMA=@db AND TABLE_NAME='pb_journal_alert' AND INDEX_NAME='held_at') = 0,
               'ALTER TABLE pb_journal_alert ADD KEY held_at (held_by, at_ms)',
               'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

SET @sql := IF((SELECT COUNT(*) FROM information_schema.STATISTICS
                 WHERE TABLE_SCHEMA=@db AND TABLE_NAME='pb_journal_alert' AND INDEX_NAME='value_at') = 0,
               'ALTER TABLE pb_journal_alert ADD KEY value_at (value_unit, value, at_ms)',
               'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;
