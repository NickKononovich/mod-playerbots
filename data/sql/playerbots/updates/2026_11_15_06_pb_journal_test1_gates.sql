-- [test1] The chain-pull judge's WORLD GATES as six columns on pb_journal_option.
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Journal.cpp reconciles a live table to the schema the code declares on every start (CreateOrExtend, [jrn32]), so
-- this file is the same migration for a database the module does not start against. The ALTER is guarded on one
-- sentinel column over information_schema: MySQL 8 has no ADD COLUMN IF NOT EXISTS (that is MariaDB), so running
-- this twice must be made to do nothing by a query, not by a flag on the statement. Nothing is dropped.
--
-- WHY THESE COLUMNS EXIST
--
-- A chain-pull judgement is one candidate's verdict, so it is an OPTION of a pull decision, not a decision of its
-- own (JournalPull.cpp: `chain_pull` -> pb_journal_option). The row carried the judge's twenty numbers and its
-- `kind` - and `kind` is only the name of the gate that REJECTED. It says nothing about the gates the judge passed
-- before it, and the judge returns at the first gate that trips, so everything before the rejection was invisible.
-- The consequence, measured and not guessed: ReplayChainPull over all 159 exported fixtures answered Undecided for
-- every single one. The recorded corpus could not replay a single verdict it had itself recorded.
--
-- NULL IS "THE JUDGE NEVER REACHED THIS GATE". It is NOT false, and the whole value of the columns depends on the
-- difference. A refusal at the walker gate computed no buff answer, no dead answer, no holds answer and no wakes
-- answer; writing those as 0 would let a query "every refusal where the group was unbuffed" count rows where the
-- judge never looked at the buffs, and the replay would reproduce a verdict nobody ever made. The emitter leaves
-- the key out of the chain_pull event entirely for a gate it did not reach, JBool turns an absent key into an empty
-- Optional, and OrNull writes that as NULL - absence travels the whole way instead of being flattened on a hop.
--
-- HOW TO CHECK IT RATHER THAN READ IT. pb_journal_option must have 40 columns, which is the number of fields in
-- its tuple (COLUMNS_OPTION in Journal.cpp and the {} count of its Push format):
--   SELECT COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_option';
-- And each gate must be able to answer by index rather than by a scan:
--   EXPLAIN SELECT COUNT(*) FROM pb_journal_option WHERE gate_holds = 0;   -- key, never type: ALL
-- Beware `mysql ... | grep -v Warning`: that returns GREP's exit status, so rc=1 means "grep matched nothing" and
-- not "the DDL failed". Success is established by querying information_schema, never by a pipeline's exit code.

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_option
     ADD COLUMN gate_boss TINYINT UNSIGNED NULL,
     ADD COLUMN gate_walker TINYINT UNSIGNED NULL,
     ADD COLUMN gate_buff TINYINT UNSIGNED NULL,
     ADD COLUMN gate_dead TINYINT UNSIGNED NULL,
     ADD COLUMN gate_holds TINYINT UNSIGNED NULL,
     ADD COLUMN gate_wakes TINYINT UNSIGNED NULL,
     ADD KEY gate_boss_at (gate_boss, at_ms),
     ADD KEY gate_walker_at (gate_walker, at_ms),
     ADD KEY gate_buff_at (gate_buff, at_ms),
     ADD KEY gate_dead_at (gate_dead, at_ms),
     ADD KEY gate_holds_at (gate_holds, at_ms),
     ADD KEY gate_wakes_at (gate_wakes, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_option' AND COLUMN_NAME = 'gate_boss');
PREPARE test1 FROM @stmt; EXECUTE test1; DEALLOCATE PREPARE test1;
