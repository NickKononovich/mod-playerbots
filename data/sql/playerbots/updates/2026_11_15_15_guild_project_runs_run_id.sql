-- [pull85] guild_project_runs.run_id - THE RUN'S OWN IDENTITY.
--
-- Why. Every journal table carries `run_id` ('20261115-135852-wc-n-five6-3'). This table, which is the ONE owner of
-- a run, carried only `id`, `leader` and `members` - so "the decisions, the idles and the stalls of THIS run" could
-- not be joined by identity at all. Establishing the cause of the wc-n run 691 failure meant gluing the run to its
-- journal rows by the time window (`at - duration` .. `at`, remembering that `at` is the run's END) and then
-- checking the glue against `judge_low = leader` before it could be believed. That check happened to work only
-- because the two overlapping wc-n runs of 2026-11-15 had different leaders (1768 and 1791); in a certification
-- batch of five fives under one content key it stops working. A glue by window is not an identity (rule 6).
--
-- The column is written by GuildCoordinator::RecordRun from the five's own `runId` and nowhere else, so the run
-- keeps one identity instead of a second copy of one. The server also adds this column itself at startup
-- (GuildCoordinator::LoadRuns, the same information_schema guard the diagnosis columns use), so it does not matter
-- whether this file or the build lands first; this file is the record of the change.
--
-- Rows written before it keep the empty string, which means "the emitter did not supply it" - not "no run".
ALTER TABLE `guild_project_runs`
    ADD COLUMN `run_id` VARCHAR(64) NOT NULL DEFAULT ''
        COMMENT '[pull85] the five run id every journal table carries; empty: written before the column existed',
    ADD KEY `run` (`run_id`);
