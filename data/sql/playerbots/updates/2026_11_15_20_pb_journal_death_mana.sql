-- [death474] pb_journal_death: THE MANA THE DEATH HAPPENED AT, as columns.
--
-- Why. The pull has a mana gate already ([mana90], ManaBudget::Of in SoloSafety::JudgeAlone: RestFirst when the mana
-- does not reach the end of the judged fight, Refuse when it does not reach it at full), and it cannot be CHECKED.
-- b503: seven of the nine deaths are `[death465] lost out of mana`, and the two pulls the resto druid Selalleas
-- opened at 4 % and at 9 % mana existed only inside the `context` SENTENCE:
--   "pulled Dragonmaw Raider judged 1 mobs 477 dmg at hp 100 % mana 4 %"
-- Nothing in this project is filtered by text (rule 1), so "the deaths where the gate let a pull through at low
-- mana" had no query at all. Four columns, each paired with time in its own index where a question gets asked of it.
--
-- `mana` stands beside `mana_max` because a share without its denominator is unverifiable (the project's own rule:
-- "11 %" without the pool is not a measurement). `heal_per_mana` is the one input of the budget that turns mana into
-- survival (SurvivalKit::HealPerMana, the health the bot's own best heal buys per point of mana) - with it the row
-- answers "would the gate have held this pull" by arithmetic rather than by a guess.
--
-- NULL means THE EMITTER DID NOT HOLD IT and never a zero: a bot with no mana bar at all (a warrior, a rogue) leaves
-- all four empty, and so do the rows written before this column existed. A query that merges the two answers anyway
-- and lies (rule 7).
--
-- IDEMPOTENT BY EXECUTION, and run twice. The server adds the columns itself at startup (Journal's CreateOrExtend
-- compares the code's CREATE against information_schema), so it does not matter whether this file or the build lands
-- first - but a bare ALTER after the server has already added them gives the core updater
-- "ERROR 1060 Duplicate column name" and THE SERVER DOES NOT COME UP. MySQL 8 has no ADD COLUMN IF NOT EXISTS, so
-- the guard is information_schema + PREPARE. This file is the record of the change.

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_death' AND COLUMN_NAME = 'mana');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_death` ADD COLUMN `mana` INT UNSIGNED NULL COMMENT '[death474] the mana it had when it died; NULL: no mana bar, or written before the column'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_death' AND COLUMN_NAME = 'mana_max');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_death` ADD COLUMN `mana_max` INT UNSIGNED NULL COMMENT '[death474] the pool the share above is of: a share without its denominator is unverifiable'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_death' AND COLUMN_NAME = 'mana_pct');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_death` ADD COLUMN `mana_pct` FLOAT NULL COMMENT '[death474] the same mana as a share of the pool, so the gate can be read without arithmetic'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_death' AND COLUMN_NAME = 'heal_per_mana');
SET @sql := IF(@col = 0,
  "ALTER TABLE `pb_journal_death` ADD COLUMN `heal_per_mana` FLOAT NULL COMMENT '[death474] health its own best heal buys per point of mana (SurvivalKit::HealPerMana): the budget input that turns mana into survival'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_death' AND INDEX_NAME = 'mana_pct_at');
SET @sql := IF(@idx = 0, 'ALTER TABLE `pb_journal_death` ADD KEY `mana_pct_at` (`mana_pct`, `at_ms`)', 'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_death' AND INDEX_NAME = 'heal_per_mana_at');
SET @sql := IF(@idx = 0, 'ALTER TABLE `pb_journal_death` ADD KEY `heal_per_mana_at` (`heal_per_mana`, `at_ms`)', 'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;
