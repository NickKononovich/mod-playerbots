-- [maze20] pb_journal_route: `candidates` and `tries_skipped` - what the maze search actually spent before it
-- declared "there is no way round".
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Journal.cpp reconciles a live table to the schema the code declares on every start (CreateOrExtend, [jrn32]), so
-- a server already started on this build has added the columns and the index itself; every statement below is
-- therefore guarded by a query over information_schema, and running the file twice does nothing the second time.
-- Nothing is dropped.
--
-- WHY THE COLUMNS EXIST
--
-- "No way round" was declared after FOUR tries inside 200 yd (MAZE_VIA_TRIES), and the code itself said what that
-- four is: a search cost bound, not a behaviour rule. How many tries really went, and why each one fell away, was
-- written down nowhere - 137 `route_zone_lost` cases over 7 zones on 2 maps with no record of the search behind
-- them, and 125 of those 137 (91.2 %) carried source_table = '' and source_id = 0, so they could not even be
-- joined to a search. Raising a bound one knows nothing about trades one ignorance for another, so the record came
-- first and the bound went to 8 tries and 400 yd after it.
--
-- THE PRICE OF THE RAISE, named: each try is TWO route searches (the leg to the via and the leg on to the goal),
-- so one look's worst case goes from 8 searches to 16, and slow searches already show as [perf26]. `candidates` is
-- what makes that answerable instead of arguable: a look whose vias in reach never reached the bound was never
-- bounded by it.
--
-- The vias themselves are CHILD rows (pb_journal_route_stop, kind 'via'), one per try with its own verdict and
-- reason - homogeneous participants of one event, which rule 1 makes rows and not a printed list.
--
-- NULL means this emitter counts no vias, which is every route event except the maze give-up. It is not 0 vias.
--
-- HOW TO CHECK IT RATHER THAN READ IT. pb_journal_route must have 31 columns - the names in COLUMNS_ROUTE in
-- Journal.cpp and the {} of its Push format (tools/jrn_fmt_check.py counts both):
--   SELECT COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_route';
--   SELECT INDEX_NAME FROM information_schema.STATISTICS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_route'
--      AND INDEX_NAME = 'candidates_at';
-- And the question the columns were added for, on the next run - whether the bound was ever what decided it:
--   SELECT candidates, stop_count tries, tries_skipped, COUNT(*) give_ups
--     FROM pb_journal_route WHERE event = 'leg_end' AND verdict = 'abandoned'
--    GROUP BY 1, 2, 3 ORDER BY give_ups DESC;
--   SELECT s.verdict, LEFT(s.why, 60) why, COUNT(*) vias
--     FROM pb_journal_route_stop s WHERE s.kind = 'via' GROUP BY 1, 2 ORDER BY vias DESC;
-- Beware `mysql ... | grep -v Warning`: that returns GREP's exit status, so rc=1 means "grep matched nothing" and
-- not "the DDL failed". Success is established by querying information_schema, never by a pipeline's exit code.

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_route
     ADD COLUMN candidates INT UNSIGNED NULL,
     ADD COLUMN tries_skipped INT UNSIGNED NULL')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_route' AND COLUMN_NAME = 'tries_skipped');
PREPARE maze20a FROM @stmt; EXECUTE maze20a; DEALLOCATE PREPARE maze20a;

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_route ADD KEY candidates_at (candidates, at_ms)')
  FROM information_schema.STATISTICS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_route' AND INDEX_NAME = 'candidates_at');
PREPARE maze20b FROM @stmt; EXECUTE maze20b; DEALLOCATE PREPARE maze20b;
