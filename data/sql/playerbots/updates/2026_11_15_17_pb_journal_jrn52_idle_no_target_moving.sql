-- [jrn52] `no_target_moving` and `range_los_moving` on pb_journal_idle: the part of the two biggest idle buckets
-- in which the bot WAS WALKING.
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Journal.cpp reconciles a live table to the schema the code declares on every start (CreateOrExtend, [jrn32]), so
-- a server that has already come up on this build has added these columns and this index itself, and a bare ALTER
-- would then fail with "Duplicate column name" and stop the next start - that happened today to this very table.
-- Every statement is therefore guarded by a query over information_schema: the columns on one sentinel, the index
-- on its own name, so a second run of this file does nothing. Nothing is dropped. MySQL 8 has no
-- ADD COLUMN IF NOT EXISTS (that is MariaDB), which is why the guard is a query and not a flag.
--
-- WHY THE COLUMNS EXIST
--
-- The largest of b503's ten unexplained dungeon causes was `noTarget` - 548 of the 1 435 cases - and it turned out
-- not to be one cause but two under one name. PlayerbotAI::UpdateIdleDiag asked `!target` and then "out of reach or
-- out of sight" BEFORE it asked `isMoving()`, so the `moving` bucket could only ever catch a tick whose target was
-- already in reach AND in sight while the bot still moved. Measured over the build's dungeon combat windows:
--   moving    6 002 ticks of 96 070   (6.2 %)
--   noTarget 46 119
--   rangeLos 14 851
-- A bot WALKING to the next pack was counted as "it had no target"; a bot RUNNING AT its mob as "out of range".
--
-- MEASURED WITHOUT A RUN, from the positions this table already carries (LAG over the same bot's windows, same
-- map), for the 548 noTarget-dominant and 245 rangeLos-dominant problem windows:
--
--   class                                              noTarget      rangeLos
--   travelled 20 yd or more during the window           373 (68.1 %)  155 (63.3 %)
--   its leader had ordered it to stand                    9 ( 1.6 %)    8 ( 3.3 %)
--   moved 5-20 yd: not stationary, tick unresolved       118 (21.5 %)   63 (25.7 %)
--   stood inside 5 yd with no named hold                  48 ( 8.8 %)   19 ( 7.8 %)
--
-- So 91.2 % of the first and 92.2 % of the second belong to a bot that was not standing still or was held by its
-- own leader - the same shape the standing watch found when 85.5 % of its episodes turned out to be a legitimate
-- wait. The remaining 67 windows are the defect candidate, and in EVERY ONE of them the bot did something (a cast,
-- an executed action or a cast in flight): not one is a bot that did nothing.
--
-- WHY THE WINDOW'S DISPLACEMENT IS NOT ENOUGH, and why these columns are needed: 15 s of displacement is a fact
-- about the WINDOW, not about the ticks inside it - a bot can kill a mob and then run 30 yd. The share above is
-- therefore a bound and not an attribution. `no_target_moving` is the attribution, counted on the tick itself by
-- the one judge of it (the chain, now the seam Journal::IdleWindow::Classify, asserted by IdleWindowChain.*).
--
-- PARTS, NEVER REPLACEMENTS. `no_target` and `range_los` keep the meaning they have had since [grp20], so every
-- count taken on an earlier build still means what it meant, and `no_target - no_target_moving` is the part in
-- which the bot stood. NULL means the emitter did not pass it (every row before this build) and is NOT "it never
-- walked" - a query that confuses the two will answer and lie.
--
-- WHAT IS DELIBERATELY NOT DONE HERE
--
-- 1. THE WALK IS NOT CALLED LEGITIMATE. A walk to the next pack is the dungeon flow working; a walk in circles is
--    a defect; 15 s of counters cannot tell them apart, and whether a walk had a purpose belongs to the route
--    domain (pb_journal_route, still empty, awaiting its emitters). So TravelTicks is reported by number in
--    real_why and is NOT added to MechanicTicks: is_real stays 'yes' for such a window. Marking a circle as
--    legitimate would be inventing a criterion instead of measuring one.
-- 2. THE DEDUPLICATION KEY IS NOT SPLIT YET, and that is an open item rather than a decision. While `noTarget`
--    names both the walking and the standing part, one key covers two defects and a fix for either would turn a
--    test green over the other - the wide direction IssueKey.h exists to prevent. The mitigation that exists
--    today is this column with its index: the slice is a WHERE, not a LIKE. The key's labels live in
--    src/Ai/Journal/IssueKey.cpp, which another agent is editing in the same tree, and two agents do not edit one
--    file. The split belongs in the next pass, with the b503 keys' text left intact for the windows that have no
--    walking part at all, exactly as [jrn45] added its suffix only for the new case.
--
-- HOW TO CHECK IT RATHER THAN READ IT. pb_journal_idle must have 55 columns - the number of fields in its tuple
-- (COLUMNS_IDLE in Journal.cpp and the {} count of its Push format, checked by tools/jrn_fmt_check.py):
--   SELECT COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_idle';
--   SELECT DISTINCT INDEX_NAME FROM information_schema.STATISTICS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_idle'
--      AND INDEX_NAME = 'no_target_moving_at';
-- The question the columns were added for, on the first run that carries them - travel against standing, which
-- until now could only be bounded from the positions and never attributed to the ticks:
--   SELECT role, in_dungeon, SUM(no_target) no_target, SUM(no_target_moving) walking,
--          SUM(no_target) - SUM(no_target_moving) standing,
--          SUM(range_los) range_los, SUM(range_los_moving) closing
--     FROM pb_journal_idle
--    WHERE event = 'window' AND in_combat = 1 AND no_target_moving IS NOT NULL
--    GROUP BY 1, 2;
-- Beware `mysql ... | grep -v Warning`: that returns GREP's exit status, so rc=1 means "grep matched nothing" and
-- not "the DDL failed". Success is established by querying information_schema, never by a pipeline's exit code.

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_idle
     ADD COLUMN no_target_moving SMALLINT UNSIGNED NULL,
     ADD COLUMN range_los_moving SMALLINT UNSIGNED NULL')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND COLUMN_NAME = 'range_los_moving');
PREPARE jrn52a FROM @stmt; EXECUTE jrn52a; DEALLOCATE PREPARE jrn52a;

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_idle ADD KEY no_target_moving_at (no_target_moving, at_ms)')
  FROM information_schema.STATISTICS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND INDEX_NAME = 'no_target_moving_at');
PREPARE jrn52b FROM @stmt; EXECUTE jrn52b; DEALLOCATE PREPARE jrn52b;
