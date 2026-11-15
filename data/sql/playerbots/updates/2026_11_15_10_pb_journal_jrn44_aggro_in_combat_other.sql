-- [jrn44] `in_combat_other` on pb_journal_aggro: was the bot in combat with anybody OTHER than the mob engaging it.
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Journal.cpp reconciles a live table to the schema the code declares on every start (CreateOrExtend, [jrn32]), so
-- this file is the same migration for a database the module does not start against. The ALTER is guarded on the new
-- column over information_schema: MySQL 8 has no ADD COLUMN IF NOT EXISTS (that is MariaDB), so running this twice
-- must be made to do nothing by a query, not by a flag on the statement. Nothing is dropped.
--
-- WHY THE COLUMN EXISTS
--
-- The reason code of this table was DEAD, and measured dead on b502: 4 661 rows, 4 661 of them `add`. Not one
-- `came`, not one `path`, and not one `estimate` - the code "an error of the estimate", which is the single case the
-- user asked the table to be built for. The cause was not a missing emitter but the order of the branches: the first
-- one read `bot->IsInCombat()`, and Journal::NoteAggro runs from Creature::AtEngage, which the core reaches only
-- AFTER it has put the bot and the mob in combat. So `IsInCombat()` is true for the FIRST mob of every fight, and
-- the first mob of a fight cannot be a mob joining a fight already running.
--
-- THE PROOF IS IN THE DATA, not in reading the code: 2 621 of the 4 661 rows match a pb_journal_fight `start` row of
-- the same bot against the same (enemy_entry, enemy_low) at the SAME at_ms - the first mob of that fight - and the
-- fight row's own `mobs` count (creatures holding the bot on their threat list at that instant) is 0 in ALL 2 621.
-- The bot was in combat with nobody and the row said it was already fighting.
--
-- `bot_in_combat` STAYS AND IS NOT REUSED. It is the core's flag, it cannot come out false at this instant, and
-- rewriting its meaning would make 4 661 old rows lie about what was measured. The new fact gets its own column, and
-- its NULL in every pre-[jrn44] row means "not measured" - which is not a 0.
--
-- HOW TO CHECK IT RATHER THAN READ IT. pb_journal_aggro must have 30 columns, which is the number of fields in its
-- tuple (COLUMNS_AGGRO in Journal.cpp and the {} count of its Push format):
--   SELECT COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_aggro';
-- The repair itself, on the next run - the first mob of a fight must no longer be an addition:
--   SELECT a.reason, a.in_combat_other, COUNT(*)
--     FROM pb_journal_aggro a
--     JOIN pb_journal_fight f ON f.bot_low = a.bot_low AND f.enemy_entry = a.mob_entry
--                            AND f.enemy_low = a.mob_low AND f.at_ms = a.at_ms
--    GROUP BY 1, 2;
-- Expected after [jrn44]: no row of that join with reason = 'add', and in_combat_other = 0 throughout it.
-- Beware `mysql ... | grep -v Warning`: that returns GREP's exit status, so rc=1 means "grep matched nothing" and
-- not "the DDL failed". Success is established by querying information_schema, never by a pipeline's exit code.

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_aggro
     ADD COLUMN in_combat_other TINYINT UNSIGNED NULL,
     ADD KEY in_combat_other_at (in_combat_other, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_aggro' AND COLUMN_NAME = 'in_combat_other');
PREPARE jrn44 FROM @stmt; EXECUTE jrn44; DEALLOCATE PREPARE jrn44;
