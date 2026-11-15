-- [jrn51] `ref_top_action`, `ref_top_cause`, `ref_top_ticks` on pb_journal_idle: WHICH action the engine turned
-- down, beside the reason it already gave.
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Journal.cpp reconciles a live table to the schema the code declares on every start (CreateOrExtend, [jrn32]), so
-- a server that has already started on this build has added these columns AND these indexes itself, and a bare
-- ALTER here would then fail with "Duplicate column name" and stop the server from coming up - that happened today,
-- to this very table. Hence: every statement is guarded by a query over information_schema, the columns on one
-- sentinel and each index on its own name, so running this file twice does nothing the second time. Nothing is
-- dropped. MySQL 8 has no ADD COLUMN IF NOT EXISTS (that is MariaDB), which is why the guard is a query and not a
-- flag on the statement.
--
-- WHY THE COLUMNS EXIST
--
-- b503 left 522 unchecked idle problems over 3 888 cases, and its LARGEST key was
--   idle|healer|gcd+other(useless+failed)|world   307 cases
-- `other` means "this tick produced nothing and the list of causes has no name for it", and ref_useless / ref_failed
-- are the engine's own answer: one action was turned down by its own usefulness check, another ran and reported
-- failure. WHICH action was never recorded anywhere - not in a column, not even in the printed line, because
-- `top_actions` lists the actions that RAN. So the biggest cause of the build could be counted and not read.
--
-- The engine holds the action in its hand at every refusal site (Engine::DoNextAction), so it passes the name
-- instead of being asked for it afterwards - one judge of the fact, at the instant of the fact. The window keeps a
-- tally of (refusal, action) pairs and the row names the busiest one: the pair and never the reason alone, because
-- an action refused as USELESS and another refused as FAILED are two defects and one name would merge them.
--
-- WHY COLUMNS AND NOT A PHRASE IN `why`: the first questions asked of this are "which action", "which reason",
-- "how often" and "did it change between builds" - all of them filters, groupings and comparisons, which in this
-- project means a column with an index paired with time, never a LIKE over text.
--
-- NULL means no tick of the window fell into `other` at all. That is absence, and it is NOT a refusal of "none":
-- `ref_top_cause = 'unsaid'` is "a tick produced nothing and the engine named no reason", a different fact.
--
-- THE KEY IS NOT TOUCHED. The refused action does not enter dedup_key: a window is 15 s of ticks and the action
-- varies inside it, which is the same reason the engine's action trace is kept out (IssueKey.h says so at length,
-- and the live table measured it: one standing episode wrote six samples that became five rows). The key keeps
-- naming WHICH reasons fired; the name of the action is read in a row already found.
--
-- HOW TO CHECK IT RATHER THAN READ IT. pb_journal_idle must have 53 columns - the number of fields in its tuple
-- (COLUMNS_IDLE in Journal.cpp and the {} count of its Push format, checked by tools/jrn_fmt_check.py):
--   SELECT COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_idle';
--   SELECT INDEX_NAME FROM information_schema.STATISTICS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_idle'
--      AND INDEX_NAME IN ('ref_action_at', 'ref_cause_at');
-- The question the columns were added for, on the next run - what exactly the engine keeps turning down, by place:
--   SELECT in_dungeon, role, ref_top_cause, ref_top_action, COUNT(*) windows, SUM(ref_top_ticks) ticks
--     FROM pb_journal_idle
--    WHERE event = 'window' AND in_combat = 1 AND ref_top_action IS NOT NULL
--    GROUP BY 1, 2, 3, 4
--    ORDER BY ticks DESC;
-- Before this build that query could not be written at all: there was no column, and no such row either.
-- Beware `mysql ... | grep -v Warning`: that returns GREP's exit status, so rc=1 means "grep matched nothing" and
-- not "the DDL failed". Success is established by querying information_schema, never by a pipeline's exit code.

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_idle
     ADD COLUMN ref_top_action VARCHAR(64) NULL,
     ADD COLUMN ref_top_cause VARCHAR(12) NULL,
     ADD COLUMN ref_top_ticks SMALLINT UNSIGNED NULL')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND COLUMN_NAME = 'ref_top_ticks');
PREPARE jrn51a FROM @stmt; EXECUTE jrn51a; DEALLOCATE PREPARE jrn51a;

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_idle ADD KEY ref_action_at (ref_top_action, at_ms)')
  FROM information_schema.STATISTICS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND INDEX_NAME = 'ref_action_at');
PREPARE jrn51b FROM @stmt; EXECUTE jrn51b; DEALLOCATE PREPARE jrn51b;

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_idle ADD KEY ref_cause_at (ref_top_cause, at_ms)')
  FROM information_schema.STATISTICS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND INDEX_NAME = 'ref_cause_at');
PREPARE jrn51c FROM @stmt; EXECUTE jrn51c; DEALLOCATE PREPARE jrn51c;
