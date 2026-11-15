-- [jrn50] pb_journal_actor: the three supply columns, and the index that makes them worth having.
--
-- A module SQL update: the core's DB updater applies it on start, or it can be applied by hand with
--   C:/WoW/deps/mysql/bin/mysql.exe -uacore -pacore acore_playerbots < this file
-- Journal.cpp reconciles a live table to the schema the code declares on every start (CreateOrExtend, [jrn32]), so
-- this file is the same migration for a database the module does not start against. Nothing is dropped.
--
-- WHY THESE THREE COLUMNS
--
-- A five failed wc-n giving 58 % of the run to resting: its paladin tank had run out of water. Establishing that
-- afterwards took a LIKE over `text`, which is the single slow form of read this journal has (384 ms over 411 819
-- rows, and it grows linearly - about 8 s at a day of a thousand bots), and rule 1 forbids it outright: anything
-- that is filtered on is a column.
--
--   drinks       what the member CARRIES at that instant (the bags, counted by CountRestItems / CountBagFood -
--                the one counter of that quantity)
--   foods        the same for food
--   drinks_need  what the five's gate PRICED its run at ([dun194] drinkNeed), written by the owner of that figure
--                (GuildFives, on the five_enter row) and nowhere else
--
-- Both sides are needed together: "0 drinks" is only a finding beside "the run needs 19".
--
-- NULL HERE MEANS "THE EMITTER DID NOT SUPPLY IT", and it is not 0. A mob has no bags at all, so every mob row
-- keeps NULL; a member whose bags were not counted is not a member carrying nothing. A query that cannot tell the
-- two apart answers anyway - and lies.
--
-- HOW TO CHECK IT RATHER THAN READ IT (the column list of the code against the column list of the database):
--   SELECT GROUP_CONCAT(COLUMN_NAME ORDER BY ORDINAL_POSITION)
--     FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME = 'pb_journal_actor';
-- Expected, character for character, COLUMNS_ACTOR from Journal.cpp: 38 columns, the last three being
-- drinks, foods, drinks_need. Beware `mysql ... | grep -v Warning`: that returns GREP's exit status.
--
-- What the next run must show, as a WHERE and not a grep - the member that went in with nothing to rest with:
--   SELECT a.name, a.role, a.level, a.drinks, a.drinks_need, a.mana, a.mana_max,
--          ROUND(a.mana_pct, 1) mana_pct, d.topic, d.verdict, LEFT(d.why, 70) why
--     FROM pb_journal_actor a JOIN pb_journal_decision d ON d.id = a.decision_id
--    WHERE a.side = 0 AND a.drinks = 0 AND a.mana_max > 0
--    ORDER BY a.at_ms DESC LIMIT 20;
--   SELECT a.name, MIN(a.drinks) carried, MAX(a.drinks_need) priced
--     FROM pb_journal_actor a WHERE a.side = 0 AND a.drinks_need IS NOT NULL
--    GROUP BY a.name HAVING carried < priced ORDER BY priced - carried DESC LIMIT 20;

-- Idempotent the way the other journal updates are: MySQL 8 has no ADD COLUMN IF NOT EXISTS, so the presence of
-- the column is asked of information_schema and the ALTER is skipped when it is already there. The three columns
-- go at the END of the table, which is what makes the add an INSTANT one - the insert names its columns, so the
-- physical order never matters to the writer.
SET @stmt := (SELECT IF(COUNT(*) > 0, 'DO 0',
  'ALTER TABLE pb_journal_actor
     ADD COLUMN drinks SMALLINT UNSIGNED NULL,
     ADD COLUMN foods SMALLINT UNSIGNED NULL,
     ADD COLUMN drinks_need SMALLINT UNSIGNED NULL,
     ADD KEY supply_at (side, drinks, at_ms)')
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'pb_journal_actor' AND COLUMN_NAME = 'drinks');
PREPARE jrn50 FROM @stmt; EXECUTE jrn50; DEALLOCATE PREPARE jrn50;
