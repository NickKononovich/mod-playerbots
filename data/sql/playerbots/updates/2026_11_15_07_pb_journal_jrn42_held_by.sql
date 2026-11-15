-- [jrn42] pb_journal_idle.held_by: WHICH legitimate stillness a standing episode stands under, by name.
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Journal.cpp reconciles a live table to the schema the code declares on every start (CreateOrExtend, [jrn32]), so
-- this file is the same migration for a database the module does not start against. The ALTER is guarded on the
-- new column over information_schema: MySQL 8 has no ADD COLUMN IF NOT EXISTS (that is MariaDB), so running this
-- twice must be made to do nothing by a query, not by a flag on the statement. Nothing is dropped.
--
-- WHY THE COLUMN EXISTS
--
-- The standing watch had one anonymous bool for "something legitimately holds it still" (dead, sitting, casting,
-- in flight, being teleported). An anonymous bool can only make the episode disappear, so no row could ever say
-- WHICH hold it was, and - worse - the holds that were not in the list at all were reported as problems while the
-- row claimed in as many words that "nothing legitimately held it still".
--
-- The numbers that made this the largest kind in pb_journal_issue, all measured on b502:
--   * 931 of 1089 out-of-combat standing rows inside Wailing Caverns (85.5 %) fell in a window in which the
--     leader of that very run had recorded a `pull` decision with verdict `held` (joined by run_id - identity -
--     and by overlap in time, which is a TIME join and is marked as one). One leader hold was therefore counted
--     again as a problem of each of the five members waiting under it: ловушка 1, count events and not rows.
--   * bot Ganna stood 120 s at (-37.33, 2475.54, -4.16) on map 369 - the Deeprun Tram platform, world coordinates
--     identical to the hundredth over twelve samples - and boarded tram car 176084 fifteen seconds after the last
--     of them. One wait of about one tram cycle (the module's own [ride133] note puts one way at 63 s), recorded
--     as twelve problems with nothing in the row to say so.
--
-- EMPTY MEANS "CHECKED AT THIS INSTANT, NOTHING HELD IT STILL". That is a finding, exactly as '' is in
-- pb_journal_decision.read_by, and it is not a blank: the names come from a closed set (Journal::Standing::
-- HoldName), one per sample, read from the world once at the sample's instant and never recomputed.
--
-- A NAMED HOLD DOES NOT DELETE THE ROW, it marks it: the issue row gets is_real='no' with the hold in real_why,
-- so the duration of a legitimate wait goes on being measured while the count the user drives to zero
-- (is_real IN ('yes','unknown')) stops counting it. That is this table's own rule - is_real is a column and not a
-- gate - and the problem a leader hold really is belongs to the leader and is already recorded, with its own
-- `why`, in pb_journal_decision.
--
-- HOW TO CHECK IT RATHER THAN READ IT. pb_journal_idle must have 48 columns, which is the number of fields in its
-- tuple (COLUMNS_IDLE in Journal.cpp and the {} count of its Push format):
--   SELECT COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_idle';
-- And the question this column exists for must be answered by index rather than by a scan:
--   EXPLAIN SELECT COUNT(*) FROM pb_journal_idle WHERE held_by = '';   -- key, never type: ALL
-- Beware `mysql ... | grep -v Warning`: that returns GREP's exit status, so rc=1 means "grep matched nothing" and
-- not "the DDL failed". Success is established by querying information_schema, never by a pipeline's exit code.

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_idle
     ADD COLUMN held_by VARCHAR(48) NOT NULL DEFAULT '''',
     ADD KEY held_at (held_by, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND COLUMN_NAME = 'held_by');
PREPARE jrn42 FROM @stmt; EXECUTE jrn42; DEALLOCATE PREPARE jrn42;
