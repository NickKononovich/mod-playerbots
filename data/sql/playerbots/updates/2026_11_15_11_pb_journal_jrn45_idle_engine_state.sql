-- [jrn45] `in_group` and `engine_state` on pb_journal_idle: the two facts the instrument's gate used to decide on.
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Journal.cpp reconciles a live table to the schema the code declares on every start (CreateOrExtend, [jrn32]), so
-- this file is the same migration for a database the module does not start against. The ALTER is guarded on one
-- sentinel column over information_schema: MySQL 8 has no ADD COLUMN IF NOT EXISTS (that is MariaDB), so running
-- this twice must be made to do nothing by a query, not by a flag on the statement. Nothing is dropped.
--
-- WHY THE COLUMNS EXIST
--
-- PlayerbotAI::UpdateIdleDiag returned at once unless the bot was IN COMBAT, IN A GROUP and INSIDE AN INSTANCE. So
-- `in_combat` read 1 in all 3 923 windows of b502's hour - which looks like a fact about bots and is only a fact
-- about the gate - and about a bot levelling alone in the world the instrument said NOTHING AT ALL. All eight deaths
-- of that hour were exactly those bots. The three conditions are now the columns `in_combat`, `in_group` and
-- `in_dungeon`, and the window is written for every bot everywhere.
--
-- `engine_state` is the one that answers the breakage this was dug out for. Thelengyl L27 died after a 113.9 s fight
-- with 9.6 s of activity, and the alert that caught it said "engine non-combat" while the core held the bot in
-- combat. That pair of facts lived only inside the `why` sentence of the standing rows, so counting it meant a LIKE
-- over text - which this project does not do. It is BotState: 0 = BOT_STATE_COMBAT, 1 = BOT_STATE_NON_COMBAT,
-- 2 = BOT_STATE_DEAD. NULL means the emitter did not pass it (every row written before [jrn45]), and 0 IS NOT
-- absence - a query that confuses the two will answer and lie.
--
-- WHAT THE WIDER COVERAGE COSTS, as a number: 4 723 window rows over 2.27 h = 2 081 rows/h now, against
-- 73 bots x 3600/15 s = 17 520 rows/h at full coverage (x8.4), at 1 214 bytes a row
-- (SUM(DATA_LENGTH+INDEX_LENGTH)/COUNT(*) over information_schema.PARTITIONS) - 0.06 GB/day becoming 0.51 GB/day
-- beside the journal's existing 1.67. Retention is the knob, never the detail.
--
-- HOW TO CHECK IT RATHER THAN READ IT. pb_journal_idle must have 50 columns, which is the number of fields in its
-- tuple (COLUMNS_IDLE in Journal.cpp and the {} count of its Push format):
--   SELECT COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_idle';
-- The question the columns were added for, on the next run - the engine on its non-combat set while the core holds
-- the bot in combat, by role and by place, with the ticks that produced nothing:
--   SELECT role, in_dungeon, in_group, COUNT(*) windows, SUM(ticks) ticks, SUM(casts) casts,
--          SUM(no_target) no_target, SUM(other) other
--     FROM pb_journal_idle
--    WHERE event = 'window' AND in_combat = 1 AND engine_state = 1
--    GROUP BY 1, 2, 3;
-- Before this build that query could not be written at all: there was no column, and no such row either.
-- Beware `mysql ... | grep -v Warning`: that returns GREP's exit status, so rc=1 means "grep matched nothing" and
-- not "the DDL failed". Success is established by querying information_schema, never by a pipeline's exit code.

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_idle
     ADD COLUMN in_group TINYINT UNSIGNED NULL,
     ADD COLUMN engine_state TINYINT UNSIGNED NULL,
     ADD KEY engine_combat_at (engine_state, in_combat, at_ms),
     ADD KEY group_at (in_group, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND COLUMN_NAME = 'engine_state');
PREPARE jrn45 FROM @stmt; EXECUTE jrn45; DEALLOCATE PREPARE jrn45;
