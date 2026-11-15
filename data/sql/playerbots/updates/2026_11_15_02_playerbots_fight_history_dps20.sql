-- [dps20] the fight measurement grows the columns the log already carried and the table did not. Additive only: no
-- column is dropped or changed, no row is touched, so it applies before or after any other ALTER of this table.
--
-- `ideal_dps`: the [dps] line prints BOTH `formula=` and `ideal=` and they are different quantities - the formula is
-- the rotation the bot's own triggers would run, the ideal is the wowsims / Rawr priority for the spec. Only
-- `formula_dps` was stored, so every window that wanted the user's rule (90 % of wowsims for every spec, 100 % the
-- aim) computed actual/formula and printed it under the name actual/ideal: one quantity under another's name.
--
-- `shared`: "a mob took damage from someone else" was the reason that threw a sample away. It is a fact of the
-- fight now, not a verdict on it: the damage measured is this bot's alone, so another member hitting the same mob
-- takes nothing from it. That rule is why 75 055 of 75 074 rows were solo and not one was measured in a dungeon.
--
-- `shortfall` and the bucket seconds: the cause of the undercount beside the share. A share with no cause says
-- "sixty percent" and does not say whether to fix it. They are the numbers the [dpsparts] and [meleeup] lines
-- already printed and nothing stored.
ALTER TABLE `playerbots_fight_history`
    ADD COLUMN `ideal_dps` FLOAT NOT NULL DEFAULT 0
        COMMENT 'the wowsims / Rawr priority for the spec, the share the user rule is about' AFTER `formula_dps`,
    ADD COLUMN `window_s` FLOAT NOT NULL DEFAULT 0
        COMMENT 'the window the swing buckets tile: the span of the damage events + the lead swing' AFTER `active_s`,
    ADD COLUMN `able_s` FLOAT NOT NULL DEFAULT 0
        COMMENT 'in reach, facing, swinging, free to act: the time the swing timer could run' AFTER `window_s`,
    ADD COLUMN `range_s` FLOAT NOT NULL DEFAULT 0 COMMENT 'out of melee reach' AFTER `able_s`,
    ADD COLUMN `facing_s` FLOAT NOT NULL DEFAULT 0 COMMENT 'in reach, not facing it' AFTER `range_s`,
    ADD COLUMN `no_swing_s` FLOAT NOT NULL DEFAULT 0 COMMENT 'no melee attack under way' AFTER `facing_s`,
    ADD COLUMN `control_s` FLOAT NOT NULL DEFAULT 0 COMMENT 'stunned, feared, confused, fleeing' AFTER `no_swing_s`,
    ADD COLUMN `switch_s` FLOAT NOT NULL DEFAULT 0 COMMENT 'swinging at a unit of no fight of its own'
        AFTER `control_s`,
    ADD COLUMN `swings` SMALLINT UNSIGNED NOT NULL DEFAULT 0
        COMMENT 'the swing timer own output: landed white hits + next-swing strikes' AFTER `switch_s`,
    ADD COLUMN `swings_allowed` FLOAT NOT NULL DEFAULT 0 COMMENT '... what the able time allowed it'
        AFTER `swings`,
    ADD COLUMN `shared` TINYINT UNSIGNED NOT NULL DEFAULT 0
        COMMENT 'a mob of the fight took damage from someone else: a fact, no longer a verdict' AFTER `why`,
    ADD COLUMN `shortfall` VARCHAR(255) NOT NULL DEFAULT ''
        COMMENT 'why it fell short: rotation spells never cast, and where its swings went' AFTER `shared`,
    ADD COLUMN `run_id` VARCHAR(48) NOT NULL DEFAULT ''
        COMMENT 'the harness run this fight belongs to; the decision journal joins on it, (guid, time) is a guess'
        AFTER `time`,
    ADD KEY `idx_ctx_map` (`ctx`, `map`, `valid`),
    ADD KEY `idx_run` (`run_id`);
-- `run_id` is added by the table's owner and stays empty until one thing exists that does not yet: a public,
-- lock-safe read on GuildCoordinator for "the run id of the five this member is in" (GuildFive::runId is held
-- behind the coordinator's own locks, and the measurement runs on a map thread inside a damage event). That
-- accessor is the coordinator's to add, and stamping it here is one line at the fight's open once it is there.
-- Until then the per-dungeon share joins on `map` - which this change makes trustworthy, because `map` is now
-- taken at the fight's OPEN - and on `time`.
