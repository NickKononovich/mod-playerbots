-- [jrn53] The twelve PROSE columns of the journal become TEXT.
--
-- WHY: nothing filters on them. Iron rule 1 says what is searched, sliced, grouped or compared across
-- builds is a column with an index, and prose is none of those - it is read with the eyes in a row that
-- has already been found. Holding it to VARCHAR(512) or VARCHAR(2048) bought nothing and cost the end of
-- every explanation: an explanation cut off lies about why a row was closed, and that row is read later
-- by whoever was not there when it was written.
--
-- NOT IN THIS LIST, and not to be touched: pb_journal_aggro.reason (VARCHAR(16): came / path / estimate /
-- add) and pb_journal_spot_reject.reason (VARCHAR(64)). Those are CODES, they ARE filtered on, they carry
-- indexes, and a code has a known width.
--
-- IDEMPOTENT BY EXECUTION, not by reading. A bare ALTER TABLE in a migration today stopped the server from
-- starting at all ("Duplicate column name 'run_id'") because the server had already added the column itself
-- on start-up. So every statement asks information_schema first and PREPAREs a no-op when there is nothing
-- to do, and this file is run TWICE before it is believed: a successful statement that did nothing is
-- indistinguishable from one that worked, so the second run is the proof.
--
-- TEXT takes no DEFAULT in MySQL, so these columns become NULLable. By iron rule 7 that is an improvement
-- and not a loss: NULL now means "the emitter passed nothing", where the empty string used to mean both
-- that and "checked, there is no text".

DELIMITER $$

DROP PROCEDURE IF EXISTS pb_jrn53_prose_to_text $$
CREATE PROCEDURE pb_jrn53_prose_to_text()
BEGIN
    DECLARE done INT DEFAULT 0;
    DECLARE t VARCHAR(64);
    DECLARE c VARCHAR(64);
    DECLARE cur CURSOR FOR
        SELECT tab, col FROM (
            SELECT 'pb_journal_decision'  AS tab, 'why'         AS col UNION ALL
            SELECT 'pb_journal_option',         'reason'              UNION ALL
            SELECT 'pb_journal_loot',           'why'                 UNION ALL
            SELECT 'pb_journal_route',          'why'                 UNION ALL
            SELECT 'pb_journal_route',          'changed_why'         UNION ALL
            SELECT 'pb_journal_route_stop',     'why'                 UNION ALL
            SELECT 'pb_journal_aggro',          'why'                 UNION ALL
            SELECT 'pb_journal_issue',          'why'                 UNION ALL
            SELECT 'pb_journal_issue',          'real_why'            UNION ALL
            SELECT 'pb_journal_idle',           'why'                 UNION ALL
            SELECT 'pb_journal_stuck',          'why'                 UNION ALL
            SELECT 'pb_journal_alert',          'reason'
        ) AS prose;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;

    OPEN cur;
    read_loop: LOOP
        FETCH cur INTO t, c;
        IF done = 1 THEN
            LEAVE read_loop;
        END IF;

        -- the table may not exist yet (a fresh database the server has not started against), and the
        -- column may already be TEXT (the server added it, or this file ran before): both are no-ops
        SET @sql = (
            SELECT IF(COUNT(*) = 1,
                      CONCAT('ALTER TABLE `', t, '` MODIFY COLUMN `', c, '` TEXT NULL'),
                      'DO 0')
            FROM information_schema.COLUMNS
            WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = t AND COLUMN_NAME = c
              AND DATA_TYPE <> 'text');

        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
    END LOOP;
    CLOSE cur;
END $$

DELIMITER ;

CALL pb_jrn53_prose_to_text();
DROP PROCEDURE IF EXISTS pb_jrn53_prose_to_text;
