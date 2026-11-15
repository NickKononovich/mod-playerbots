-- [feed20] user 2026-11-15: the certification had nowhere to keep its own history. guild_project_cert holds ONE row
-- per dungeon - the current phase, the batch counters and the LAST batch's per-five verdicts as text - so every
-- earlier wave is overwritten the moment the next one ends. On 15.11 the table said wc-n "batches 4, passed_batch 0"
-- and could show the verdicts of batch 4 only; the verdicts of batches 2 and 3 exist nowhere at all, in the table or
-- in a surviving log. This table keeps one row per wave instead, and it is a JOURNAL, not the criterion: whether a
-- dungeon is certified is decided over guild_project_runs (ORDER BY id DESC LIMIT 30, all of them clean by
-- IsCleanRun), never by a count of waves. kind tells a certification wave from the speed drill.
-- Nothing a bot decides reads this table.
CREATE TABLE IF NOT EXISTS `guild_project_cert_waves` (
    `content`     VARCHAR(16)      NOT NULL COMMENT 'rfc-n, wc-n, ... (guild_project_runs.content)',
    `kind`        VARCHAR(16)      NOT NULL DEFAULT 'cert' COMMENT 'cert = 5 fives at once / speed = the speed drill',
    `wave`        INT UNSIGNED     NOT NULL COMMENT 'the wave number of this content and kind (CertState::batches)',
    `started_at`  INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT 'unix s, when the wave was dispatched',
    `ended_at`    INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT 'unix s, 0 = still in flight',
    `fives`       TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'fives sent at once (5 = the rule)',
    `clean_fives` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'of them clean, by the one IsCleanRun definition',
    `run_ids`     VARCHAR(255)     NOT NULL DEFAULT '' COMMENT 'guild_project_runs ids of the wave, comma separated',
    `passed`      TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 = the wave counted (every five clean)',
    `why`         TEXT             NULL COMMENT 'why it did not count, in words, per five',
    PRIMARY KEY (`content`, `kind`, `wave`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='[feed20] one row per certification / speed wave: the history guild_project_cert overwrites';

-- [feed20] the fact that an unclean run has been DIAGNOSED. The user's rule - every unclean run gets its diagnosis
-- before the next wave, a repeat without the analysis is how a problem stays behind - was not checkable by anything:
-- the diagnosis lived in a report file and the run row knew nothing of it. On the run itself, so the rule is one
-- query: SELECT ... WHERE NOT clean AND diagnosed_at = 0. No decision of a bot reads these columns either.
ALTER TABLE `guild_project_runs`
    ADD COLUMN `diagnosed_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix s the diagnosis was put on, 0 = none',
    ADD COLUMN `diagnosis`    TEXT         NULL             COMMENT 'the diagnosis in words';
