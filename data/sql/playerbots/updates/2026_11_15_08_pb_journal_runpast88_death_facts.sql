-- [runpast88] THE DEATH'S INPUTS AS COLUMNS, not as words inside a sentence.
--
-- Measured, not supposed: over the b502 hour pb_journal_death held eight rows, and in every one of them
-- `attackers` was 0, `damage_from_killer` was 0, `judged_pull` was 0 and `expected` was empty - while the
-- `context` column of the same row carried, in English, the number of creatures that had hit the bot that fight,
-- what the run-past chase had been priced at, what actually arrived, and how its last route search had seen each
-- of those creatures. The emitter was holding all of it; only the sentence got it.
--
-- The user's first rule is that nothing is filtered by text or by JSON. "How many deaths took more hitters than
-- the route search had priced" has to be a WHERE over columns: a LIKE over text was measured at 384 ms across
-- 411 819 rows and grows linearly. So each of those quantities gets a column, and the ones that are searched,
-- grouped or compared across builds get an index paired with the time.
--
-- EVERY ONE OF THEM IS NULL-ABLE AND NULL MEANS "THE EMITTER DID NOT HOLD IT". That is not a zero:
-- fight_hit_by = 0 would say nobody hit the bot, runpast_priced = 0 would say the chase was priced free, and a
-- query that cannot tell the two apart still answers - and lies.
--
-- Two of the old columns change meaning in the same pass, because a false zero is worse than a gap:
--   * `damage_from_killer` (DamageTaken::From) is written only for a death in a dungeon. DeathRecorder::IsTracked
--     accumulates damage only for a player whose map is a dungeon, so out in the world there was never anything to
--     read and the 0 said "the killer dealt it nothing". All eight b502 deaths were in the open world. The
--     open-world measure is the other instrument's and lands in `fight_damage_from_killer`.
--   * `attackers` (the core's own attacker list at the kill hook) is written only when that list is not empty,
--     which at a death it never is: the kill clears it before PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE runs. The
--     count that answers is `fight_hit_by`.
-- Old rows keep their zeros - they are what the old emitter wrote, and rewriting history is not a migration.

ALTER TABLE `pb_journal_death`
    -- the fight it died in, as SoloSafety measured it from its own damage events (open world and dungeon alike)
    ADD COLUMN `fight_hit_by` SMALLINT UNSIGNED NULL AFTER `attackers`,
    ADD COLUMN `fight_taken` INT UNSIGNED NULL AFTER `fight_hit_by`,
    ADD COLUMN `fight_damage_from_killer` INT UNSIGNED NULL AFTER `fight_taken`,
    ADD COLUMN `fight_sec` FLOAT NULL AFTER `fight_damage_from_killer`,
    -- [runpast87] the committed run-past chase it was walking under when it died
    ADD COLUMN `runpast_priced` FLOAT NULL AFTER `fight_sec`,
    -- [death386] the HIGH end of that chase - the number SurvivesRunPast was actually asked. The line printed the
    -- mean, so the journal held a price no decision had been taken on: b502 Lanbuzz 577 printed, 677 judged,
    -- 911 arrived. A query that sets a price against the damage uses THIS column.
    ADD COLUMN `runpast_priced_high` FLOAT NULL AFTER `runpast_priced`,
    ADD COLUMN `runpast_sec` FLOAT NULL AFTER `runpast_priced_high`,
    ADD COLUMN `runpast_hp` FLOAT NULL AFTER `runpast_sec`,
    ADD COLUMN `runpast_taken` INT UNSIGNED NULL AFTER `runpast_hp`,
    -- [surv20] the killer or a hitter is one of that chase's own chasers: without it a chase's price read as the
    -- price of a fight it was never about
    ADD COLUMN `runpast_of_its_chase` TINYINT UNSIGNED NULL AFTER `runpast_taken`,
    -- [deaths] how its last route search had seen the mobs that went on to hit it, each word of that sentence a count
    ADD COLUMN `route_age_ms` INT UNSIGNED NULL AFTER `runpast_of_its_chase`,
    ADD COLUMN `route_complete` TINYINT UNSIGNED NULL AFTER `route_age_ms`,
    ADD COLUMN `route_through_refused` TINYINT UNSIGNED NULL AFTER `route_complete`,
    ADD COLUMN `route_priced` SMALLINT UNSIGNED NULL AFTER `route_through_refused`,
    ADD COLUMN `route_refused` SMALLINT UNSIGNED NULL AFTER `route_priced`,
    ADD COLUMN `route_unseen` SMALLINT UNSIGNED NULL AFTER `route_refused`,
    -- [surv21] in the search's range and left out because its fight was not the bot's - a different thing from
    -- "unseen", and the sentence used to say the same word for both
    ADD COLUMN `route_other_fight` SMALLINT UNSIGNED NULL AFTER `route_unseen`;

-- Indexes paired with the time, one per question that gets asked:
--   "a death where more mobs hit it than its search had priced"  -> fight_hit_by, route_priced
--   "a death under a chase priced below what arrived"            -> runpast_priced_high, runpast_taken
--   "a death where a mob the search never saw came"              -> route_unseen
--   "how old was the estimate"                                   -> route_age_ms
ALTER TABLE `pb_journal_death`
    ADD INDEX `fight_hit_by_at` (`fight_hit_by`, `at_ms`),
    ADD INDEX `runpast_priced_high_at` (`runpast_priced_high`, `at_ms`),
    ADD INDEX `runpast_taken_at` (`runpast_taken`, `at_ms`),
    ADD INDEX `route_unseen_at` (`route_unseen`, `at_ms`),
    ADD INDEX `route_priced_at` (`route_priced`, `at_ms`),
    ADD INDEX `route_age_ms_at` (`route_age_ms`, `at_ms`);
