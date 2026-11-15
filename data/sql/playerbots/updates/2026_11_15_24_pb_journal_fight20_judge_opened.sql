-- [fight20] / [death476] / [alert20b] five columns that turn three text-only answers into columns.
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Journal.cpp reconciles a live table to the schema the code declares on every start (CreateOrExtend, [jrn32]), so
-- this file is the same migration for a database the module does not start against. Every ALTER is guarded on its
-- own new column over information_schema: MySQL 8 has no ADD COLUMN IF NOT EXISTS (that is MariaDB), so running
-- this file twice must be made to do nothing by a QUERY and not by a flag on the statement. Nothing is dropped and
-- no existing column changes meaning.
--
-- ===================================================================================================================
-- 1. pb_journal_fight: judge_opened, unjudged_inside, unjudged_moving  ([fight20])
-- ===================================================================================================================
-- "371 fights no judge opened" was taken apart out of the TEXT of the [solo] / [surv10] lines, which is what rule 1
-- forbids - and that is exactly why the share the zone rule [dpszero3] takes away could not be counted on b504 at
-- all. All three facts are the emitter's own at the instant the fight begins (SoloSafety::OpenFactsOf: the bot's
-- engage verdict plus ONE reading of the first mob's aggro disc through AggroDisc, the same judge the bot's own
-- avoidance uses - no second judge of that question).
--
-- WHY A FUNCTION ASKED AT THE HOOK AND NOT A FIELD OpenIfNeeded FILLS IN, measured and not assumed: over b504 the
-- [solo] UNJUDGED line came AFTER its fight row in 283 of the 290 pairs that join within 2 s (5 in the same
-- millisecond, 2 before). OpenIfNeeded runs on the bot's own tick, the fight row is written inside the core's
-- combat-state transition, so a field written by OpenIfNeeded would have been NULL for all but a handful of rows -
-- a dead column, which is the one failure this work was asked to avoid. The answer is therefore computed once per
-- fight by whoever asks first (normally the hook, at the fight's real start) and the [solo] sentence now prints
-- that same reading instead of taking its own a tick later.
--
-- ALL THREE STAY NULL-ABLE, ON PURPOSE. judge_opened is NULL when SoloSafety is not this bot's judge at all - in a
-- dungeon its leader is - and that is not a measured "the bot did not open it". unjudged_inside and unjudged_moving
-- are NULL when the judge DID open the fight: the quantity does not exist then, and a 0 would read as "it stood
-- outside the disc". This is rule 7, and it is the same reason to_x/y/z on pb_journal_route had to be made NULL-able
-- by 2026_11_15_14 after a NOT NULL DEFAULT 0 had been asserting the point (0,0,0).
--
-- THE TWO UNJUDGED COLUMNS ARE NOT ONE COLUMN, and that is deliberate: standing inside the disc is the defect
-- [dpszero3] takes away, walking through it is a different one that [dpszero2] already refuses on the walk, and a
-- single column could not tell a fix for the first from a fix for the second afterwards.
--
-- HOW TO CHECK IT RATHER THAN READ IT. pb_journal_fight must have 34 columns, which is the number of names in
-- COLUMNS_FIGHT in Journal.cpp and the {} count of its Push format (jrn_fmt_check.py prints both):
--   SELECT COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_fight';
-- And the query the whole thing exists for, on the first run after it:
--   SELECT judge_opened, unjudged_inside, unjudged_moving, COUNT(*) fights
--     FROM pb_journal_fight WHERE event = 'start' AND in_dungeon = 0 GROUP BY 1, 2, 3;
-- The expectation that can be refuted: judge_opened = 0 AND unjudged_inside = 1 AND unjudged_moving = 0 should be
-- NOTICEABLY FEWER than b504's 162 - those are the bots [dpszero3] leads away. If it is not fewer, there was
-- nowhere to lead them, and that shows as a `stand_no_ground` issue row rather than as a guess.
--
-- Beware `mysql ... | grep -v Warning`: that returns GREP's exit status, so rc=1 means "grep matched nothing" and
-- not "the DDL failed". Success is established by querying information_schema, never by a pipeline's exit code.

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_fight
     ADD COLUMN judge_opened TINYINT UNSIGNED NULL,
     ADD COLUMN unjudged_inside TINYINT UNSIGNED NULL,
     ADD COLUMN unjudged_moving TINYINT UNSIGNED NULL,
     ADD KEY judge_opened_at (judge_opened, at_ms),
     ADD KEY unjudged_inside_at (unjudged_inside, at_ms),
     ADD KEY unjudged_moving_at (unjudged_moving, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_fight' AND COLUMN_NAME = 'judge_opened');
PREPARE fight20 FROM @stmt; EXECUTE fight20; DEALLOCATE PREPARE fight20;

-- ===================================================================================================================
-- 2. pb_journal_death: killer_spawn_id  ([death476])
-- ===================================================================================================================
-- `killer_low` IS NOT AN IDENTITY, and that was rule 6 broken inside the instrument itself. It is
-- Creature::GetGUID().GetCounter() - a counter handed out at runtime, new on every boot. Measured: Mor'Ladim
-- (entry 522) carries killer_low 3009 on b503 and 2195 on b504, while acore_world.creature holds exactly ONE row
-- for that entry, guid 5086. In that same table 3009 is an Elder Crag Boar (entry 1127) and 2195 a Bloodsail
-- Warlock (entry 1564), so a JOIN on creature.guid answers with the wrong creature. The counter 7365 even appears
-- in the journal under TWO different entries (433 Shadowhide Gnoll and 4003 Windshear Geomancer) while in
-- `creature` it is an Adder (3300).
--
-- Four of b503's and b504's deaths really are one spawn, and the row could not say so. With this column
-- "which spawn killed how many bots" becomes a GROUP BY over an identity:
--   SELECT killer_entry, killer_spawn_id, COUNT(*) deaths FROM pb_journal_death
--    WHERE killer_spawn_id IS NOT NULL GROUP BY 1, 2 ORDER BY 3 DESC;
--
-- THE OLD COLUMN IS NOT REUSED AND ITS MEANING IS NOT CHANGED - the lesson of [jrn44], where in_combat_other was
-- added BESIDE bot_in_combat rather than over it, so that no row written earlier is made to lie about what it
-- measured. NULL here means the killer has no spawn row at all (a player, a summon, a temporary creature), which
-- is not spawn 0. pb_journal_death must have 56 columns afterwards.

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_death
     ADD COLUMN killer_spawn_id INT UNSIGNED NULL,
     ADD KEY killer_spawn_at (killer_spawn_id, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_death' AND COLUMN_NAME = 'killer_spawn_id');
PREPARE death476 FROM @stmt; EXECUTE death476; DEALLOCATE PREPARE death476;

-- ===================================================================================================================
-- 3. pb_journal_alert: quest_id  ([alert20b])
-- ===================================================================================================================
-- The quest's TITLE was sitting in the DEDUP KEY, where a title is an IDENTITY and never the shape of a defect.
-- WithoutNumbers already replaced the digits, so q1234 became qN - and the title right behind it stayed. Measured
-- on the live table: 391 alert events under 112 keys, of which 78 carry a quest and 53 of those 78 hold a SINGLE
-- event; 72 of the 78 are the one shape `<kind>|DO_QUEST qN <Title>` wearing 72 names. That is trap #2 in a new
-- dress - the same one that made 757 "causes" out of 3 900 idle rows.
--
-- THE TITLE IS REMOVED FROM THE KEY BY ITS OWN EXACT TEXT AND NEVER BY CUTTING AFTER qN, and the measurement says
-- why: the tails behind qN are not all names. Two of them are SHAPES - `qN has no unspent objective spot left)` and
-- `qN is postponed, the choice runs again)` - and a blind cut merges those two, which is the WIDER of the two
-- errors this key can make: a fix for one turns a test green while the other lives on beneath it. Cutting blind
-- leaves 41 keys of the 112; removing the exact title leaves 43, and the two extra keys are exactly those shapes.
-- Both directions are asserted by IssueKeyAlert.* in tests/unit/IssueKeyTest.cpp.
--
-- NAMED RATHER THAN HIDDEN: the 78 keys of b503 and b504 that carry a title do NOT keep their text - merging them
-- is the point of this change - so counts previously taken on those particular keys stop meaning what they meant.
-- A key with no quest in it is byte for byte what it was (`alert|DEATH|`, `alert|STUCK|GO_CAMP N yd`,
-- `alert|LOOP|WANDER_RANDOM`), so every count taken on those goes on meaning exactly what it meant.
--
-- The column's owner is the bot's own RPG plan (NewRpgInfo::DoQuest::questId), read in the same instant as the rest
-- of the row by ONE collector for every alert kind (GuildCoordinator::AlertQuest, beside AlertSubject).
-- NULL = there was no PlayerbotAI to ask; 0 = asked, and the plan names no quest. Those are different answers.
-- pb_journal_alert must have 28 columns afterwards.
--   SELECT quest_id, kind, COUNT(*) FROM pb_journal_alert GROUP BY 1, 2 ORDER BY 3 DESC;
--   SELECT COUNT(*) keys_, SUM(occurrences) events FROM pb_journal_issue WHERE kind LIKE 'alert%';

SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_alert
     ADD COLUMN quest_id INT UNSIGNED NULL,
     ADD KEY quest_at (quest_id, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_alert' AND COLUMN_NAME = 'quest_id');
PREPARE alert20b FROM @stmt; EXECUTE alert20b; DEALLOCATE PREPARE alert20b;
