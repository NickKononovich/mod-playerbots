-- [jrn57] pb_journal_idle: the engine's own answer on the ticks the bucket chain decided BEFORE `other`.
--
-- WHY. The engine records on EVERY tick what it would have done and why it did not
-- (PlayerbotAI::NoteIdleDiagRefusal, the first refusal of the relevance-ordered queue, with the name of the
-- action). `ref_top_action` / `ref_top_cause` / `ref_top_ticks` count that pair only on a tick that reached the
-- `other` bucket, and `no_target`, `range_los` and `moving` are decided earlier - so on those the answer was in
-- hand and thrown away. Measured over the closed b504 corpus (at_ms <= 1794764479861): 161 rows of
-- pb_journal_issue, 1 248 cases, name a bucket and no cause at all, among them every one of the 38 090 idle
-- ticks a healer in a dungeon produced across three builds (1 028 windows, all other buckets exactly zero).
--
-- WHY SEPARATE COLUMNS AND NOT A WIDENING OF ref_top_*. `ref_unsaid` .. `ref_no_candidate` sum EXACTLY to
-- `other` (30 842 against 30 842 over b502's hour) and [jrn43]'s deduplication key `other(useless)` stands on
-- that invariant. Counting an early tick into the same place would break a measurement already taken.
--
-- WHY NOT IN THE DEDUPLICATION KEY, measured rather than asserted: over consecutive windows of the same bot on
-- the same map (gap <= 1.5 windows - a join by TIME, marked as one) the refused action flips in 56 of 450 pairs
-- (12.4 %) and its cause in 42 of 450 (9.3 %), against 0.7 % for the `in_group` grapheme that did earn a place
-- in a key and 26.4 % for one that did not. That unstable, it would split a single episode over several keys -
-- the ladder [jrn41] removed. A column is where the slice is a WHERE.
--
-- NULL on every row written before b505 means "the emitter did not carry it", which is NOT a tick of zero.
--
-- The source of truth is the CREATE in src/Ai/Journal/Journal.cpp, which CreateOrExtend grows on start-up; this
-- file is the same statement for a database the module will not be started against. Every operator is guarded by
-- information_schema through PREPARE/EXECUTE, because MySQL 8 has no ADD COLUMN IF NOT EXISTS (that is MariaDB)
-- and a bare ALTER here would bring the start-up down, exactly as CreateOrExtend's own additions would collide.

SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD COLUMN ref_early_action VARCHAR(64) NULL',
    'DO 0') FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND COLUMN_NAME = 'ref_early_action');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;

SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD COLUMN ref_early_cause VARCHAR(12) NULL',
    'DO 0') FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND COLUMN_NAME = 'ref_early_cause');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;

SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD COLUMN ref_early_ticks SMALLINT UNSIGNED NULL',
    'DO 0') FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND COLUMN_NAME = 'ref_early_ticks');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;

-- Each name paired with time, the way rule 1 asks: the first questions are "which action" and "which reason",
-- on their own and across builds. `ref_early_ticks` gets none, like `ref_top_ticks`: the spread of the counts is
-- the problem row's own value range, and nothing slices by it.
SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD KEY ref_early_action_at (ref_early_action, at_ms)',
    'DO 0') FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND INDEX_NAME = 'ref_early_action_at');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;

SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD KEY ref_early_cause_at (ref_early_cause, at_ms)',
    'DO 0') FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND INDEX_NAME = 'ref_early_cause_at');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;

-- ---------------------------------------------------------------------------------------------------------------
-- [jrn58] AND THE SAME QUESTION OVER *EVERY* REFUSAL OF EVERY TICK, plus the leader's own published readiness.
--
-- WHY. NoteIdleDiagRefusal keeps a tick's FIRST refusal only, and the tank's "pull action" sits at relevance
-- 105.0 against ACTION_DEFAULT (~10) for the whole rest of its rotation, so it refuses first on EVERY tick and
-- hides every other refusal behind it. Measured over the closed b504 corpus (at_ms <= 1794764479861): a tank in
-- combat wrote 278 windows that carry ONE pair between them - `useless` / `pull action`, 195 in a dungeon and 83
-- in the world - while a dps in a dungeon shows 30 pairs over 324 windows and a healer in the world 12 over 294.
-- And `ref_top_ticks` equals `other` EXACTLY for the tank: 1 851 of 1 851 in a dungeon, 415 of 415 in the world.
-- 100 % of its unexplained ticks on one pair, by construction - a value that cannot come out otherwise is not a
-- measurement. The cost of believing it: "this is the leader's lawful plan" explains 29 ticks of 2 266 (1.3 %).
--
-- `ref_any_pairs` IS THE FACT THAT MOVES, and that is why it exists as a column of its own: the tank's "pull
-- action" is refused on every idle tick, so it stays the BUSIEST pair even after every refusal is recorded - the
-- busiest pair cannot show whether the fix worked, the count of pairs can, and it is the readiness criterion
-- ("more than one pair over the tank's windows on b505"). `ref_any_events` is NOT a count of ticks: one tick can
-- refuse several actions, which is why the name differs and why these columns do NOT enter the invariant
-- "ref_unsaid .. ref_no_candidate sum exactly to `other`".
--
-- `leader_ready` EXISTS BECAUSE `leader_hold` LIES BY ITS OWN NAME. Its only writer is
-- LeaderCommandMultiplier::Log (ThreatStrategy.cpp) - the CHAT command path, Stop / DontAttack - and the dungeon
-- leader's pull verdict never touches it: 0 in all 278 tank windows while the leader was holding them. The old
-- name keeps its old meaning and the new fact gets its own column, which is the [jrn44] lesson (in_combat_other
-- was added BESIDE bot_in_combat, never over it, so old rows cannot be made to lie about what was measured).
-- The value is read from Leader::Readiness, written by the leader and meant to be read from a map thread;
-- nothing re-decides it here. NULL = no fresh verdict (no group, not a led five, the run ended) - NOT "not
-- ready". With it, "the leader's plan or a defect" becomes a WHERE instead of a join by time.

SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD COLUMN ref_any_action VARCHAR(64) NULL',
    'DO 0') FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND COLUMN_NAME = 'ref_any_action');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;

SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD COLUMN ref_any_cause VARCHAR(12) NULL',
    'DO 0') FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND COLUMN_NAME = 'ref_any_cause');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;

SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD COLUMN ref_any_events SMALLINT UNSIGNED NULL',
    'DO 0') FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND COLUMN_NAME = 'ref_any_events');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;

SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD COLUMN ref_any_pairs SMALLINT UNSIGNED NULL',
    'DO 0') FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND COLUMN_NAME = 'ref_any_pairs');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;

SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD COLUMN leader_ready TINYINT UNSIGNED NULL',
    'DO 0') FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND COLUMN_NAME = 'leader_ready');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;

SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD KEY ref_any_action_at (ref_any_action, at_ms)',
    'DO 0') FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND INDEX_NAME = 'ref_any_action_at');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;

-- the pair count gets an index because it is what the fix is judged by, and the leader's readiness because it
-- turns "the leader's plan or a defect" from a join by time into a WHERE
SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD KEY ref_any_pairs_at (ref_any_pairs, at_ms)',
    'DO 0') FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND INDEX_NAME = 'ref_any_pairs_at');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;

SET @add := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE pb_journal_idle ADD KEY leader_ready_at (leader_ready, at_ms)',
    'DO 0') FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_idle' AND INDEX_NAME = 'leader_ready_at');
PREPARE s FROM @add; EXECUTE s; DEALLOCATE PREPARE s;
