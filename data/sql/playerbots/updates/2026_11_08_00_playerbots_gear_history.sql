-- [guild15] every gear change of a guild bot: what it took off, what it put on, where the new item came from
CREATE TABLE IF NOT EXISTS `playerbots_gear_history` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `guid` INT UNSIGNED NOT NULL,
    `name` VARCHAR(12) NOT NULL DEFAULT '',
    `slot` TINYINT UNSIGNED NOT NULL,
    `old_entry` INT UNSIGNED NOT NULL DEFAULT 0,
    `old_ilvl` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    `new_entry` INT UNSIGNED NOT NULL DEFAULT 0,
    `new_ilvl` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    `source` VARCHAR(16) NOT NULL DEFAULT 'other',
    `level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `time` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_guid_slot` (`guid`, `slot`, `time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
