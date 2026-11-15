-- [dps20] three columns the first pass could not have known it needed; the batch of 1937 group fights it produced
-- showed it. Additive only, no column dropped or changed, no row touched.
--
-- `casting_s`: `control_s` was a lie by conflation. One bucket of the swing gate held
-- CASTING | CHARGING | STUNNED | FLEEING | CONFUSED under the name "controlled", so a destro warlock read 97 % of
-- its window as time taken off it when it was casting - its whole job. A stun is a defect to chase, a cast is not,
-- and one column that cannot tell them apart answers nothing. `control_s` is now stun / fear / confusion alone and
-- `casting_s` is its own cast or charge. Rows written before this migration have the two mixed in `control_s`:
-- read them as "not swinging, reason unsplit", not as control.
--
-- `cast_s`: the six buckets are the gate of a WHITE SWING, so a caster's `swings` and `swings_allowed` are 0 by
-- construction and the specs that sit lowest against the ideal had no diagnostic column at all. This is the
-- execute time of its own casts (the [dpsparts] busy share), which is the caster's side of the same question.
--
-- `assumed_buffs`: how many buff categories the formula and the ideal assumed and the bot did not wear at the
-- fight's open. PlannedBuffs skips every category the bot already has, so a denominator that rises in a group
-- means the party's buffs were genuinely missing - the ideal is what the spec does when the party does its job.
-- That is a finding about the five, not a defect of the instrument, and storing the count is what stops it being
-- re-argued from the share alone (a feral tank read +47.5 % denominator against +9.7 % damage in a group).
ALTER TABLE `playerbots_fight_history`
    ADD COLUMN `casting_s` FLOAT NOT NULL DEFAULT 0
        COMMENT 'busy with its own cast or charge (was mixed into control_s before this column)' AFTER `switch_s`,
    ADD COLUMN `cast_s` FLOAT NOT NULL DEFAULT 0
        COMMENT 'execute time of its own casts: the caster side, where swings/swings_allowed are 0' AFTER `casting_s`,
    ADD COLUMN `assumed_buffs` TINYINT UNSIGNED NOT NULL DEFAULT 0
        COMMENT 'buff categories the denominator assumed and the bot did not wear at the open' AFTER `cast_s`;
