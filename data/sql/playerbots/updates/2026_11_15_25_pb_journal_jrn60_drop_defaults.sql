-- [jrn60] СНЯТИЕ ДЕФОЛТОВ СО ВСЕХ `NOT NULL` КОЛОНОК ЖУРНАЛА
--
-- Слова пользователя: «дефолт нам не нужен». Это инвариант, а не разовая уборка, поэтому
-- проверка живёт разделом в C:/WoW/tools/audit_run.sh, и правильный ответ у неё — НОЛЬ:
--
--   SELECT COUNT(*) FROM information_schema.COLUMNS
--    WHERE TABLE_SCHEMA = 'acore_playerbots' AND TABLE_NAME LIKE 'pb_journal%'
--      AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
--
-- До этого файла: 262 колонки (ровно столько же мест `NOT NULL DEFAULT` было в CREATE
-- внутри src/Ai/Journal/Journal.cpp — два списка сошлись числом). После: 0.
--
-- ЗАСЫПКА СТАРЫХ СТРОК НЕ НУЖНА, и это проверено, а не предположено: колонка объявлена
-- `NOT NULL`, значит `NULL` в ней не лежит ни в одной старой строке — там стоит ноль или
-- пустая строка, подставленные при вставке. Снятие дефолта — изменение ОДНИХ МЕТАДАННЫХ,
-- ни одна строка не перезаписывается. Ни одной колонки, которую пришлось бы засыпать, не
-- нашлось: `NULL`-евых колонок с дефолтом в базе 0.
--
-- ЧЕГО ЭТО НЕ ЛОВИТ — сказано честно, чтобы никто не принял это за защиту от пропавшего
-- писателя. `Writer::Loop` строит каждую вставку как
-- `INSERT INTO <таблица> <полный список колонок> VALUES ...`: все колонки всегда названы, и
-- у не-`Optional` поля `StringFormat` всегда выдаёт литерал. Значит дефолт НЕ ПОДСТАВЛЯЛСЯ
-- НИКОГДА, и его снятие не меняет ни одной сегодняшней вставки. Проверено исполнением на
-- копии `pb_journal_death`: вставка в журнальной форме (все 56 колонок, нули и пустые строки
-- литералами) со снятыми дефолтами ПРОШЛА.
--
-- ЧТО ЭТО ЛОВИТ, и ради чего делается — будущую ЧАСТИЧНУЮ вставку. Дефолт, который никогда
-- не срабатывает, — это ловушка, ждущая дня, когда кто-нибудь напишет `INSERT` не по полному
-- списку: тогда он молча подставит ноль или пустую строку вместо того, чтобы упасть, и в
-- колонке окажется величина, которой никто не мерил, неотличимая от измеренной (железное
-- правило 7: отсутствие не есть ноль). После снятия такой `INSERT` падает с `ERROR 1364`.
-- Та же копия, та же частичная вставка: С дефолтами MySQL её ПРИНЯЛ и выдумал `bot_low = 0`
-- и `bot_name = ''`; БЕЗ дефолтов — отказал. Это и есть цель: громкий отказ вместо тихой
-- выдумки.
--
-- УСЛОВИЕ, БЕЗ КОТОРОГО ВСЁ ЭТО НИЧЕГО НЕ ПРОВЕРЯЕТ: строгий режим. Проверено запросом —
-- `@@sql_mode` и `@@GLOBAL.sql_mode` оба несут `STRICT_TRANS_TABLES`. Без него пропуск
-- колонки не упал бы, а молча вставил ноль, и эта миграция поставила бы проверку, которая
-- не проверяет.
--
-- ЦЕНА, НАЗВАННАЯ ПРЯМО: журнал пишет пачками до 500 строк одним `INSERT`. Если такая
-- частичная вставка когда-нибудь появится, упадёт ВСЯ ПАЧКА, а не одна строка. Пользователь
-- об этом предупреждён и выбрал громкий отказ.
--
-- И ПРАВКА В КОДЕ ОБЯЗАТЕЛЬНА РЯДОМ С ЭТИМ ФАЙЛОМ (сделана тем же подходом): `CreateOrExtend`
-- не трогает уже существующую колонку, поэтому снятый в базе дефолт переживает подъём — но
-- колонку, которую этот список ДОБАВИТ позже, он добавит с тем дефолтом, что объявлен в
-- `CREATE`. Оставить `DEFAULT` в `Journal.cpp` значило бы возвращать их обратно по одной
-- новой колонке за раз, а базу с нуля родить со всеми 262.
--
-- `TEXT`/`BLOB` в списке отсутствуют по природе: таким типам дефолт не назначается вообще.
-- Проверено: среди 262 были только varchar/char, int/bigint/smallint/tinyint, float/double.
-- `pb_journal_issue.checked_state` — STORED GENERATED, у него `COLUMN_DEFAULT` пуст, и
-- фильтр ниже его не берёт: `ALTER COLUMN ... DROP DEFAULT` на генерируемой колонке упал бы.
--
-- ИДЕМПОТЕНТНОСТЬ — не обещанием, а построением: список колонок собирается из
-- `information_schema` в момент исполнения, поэтому второй прогон не находит ни одной
-- колонки с дефолтом и выполняет `DO 0`. Прогнана дважды, результат прочитан обратно
-- запросом. Колонки, которой в этой базе нет, в списке не окажется — голый `ALTER` на
-- отсутствующую колонку уже стоил падения подъёма.

-- ЛОВУШКА, ПОЙМАННАЯ ИСПОЛНЕНИЕМ ЭТОГО ЖЕ ФАЙЛА (первая редакция упала на ней): список
-- клаузул собирается `GROUP_CONCAT`, а у него есть `group_concat_max_len`, по умолчанию
-- **1024 байта**. У `pb_journal_issue` 33 колонки с дефолтом, их клаузулы длиннее — список
-- молча ОБРЕЗАЛСЯ посередине, и `ALTER` упал с `ERROR 1064 ... near ''`. Из 262 колонок
-- успели сняться 149, шесть таблиц после issue не обработались вовсе. Обрезка опасна не
-- падением, а тем, что могла дать СИНТАКСИЧЕСКИ ВЕРНЫЙ, но неполный `ALTER` — то есть
-- успешную операцию, сделавшую половину дела: ровно наша ловушка №3.
-- Поэтому здесь ДВЕ защиты, а не одна: предел поднят явно, И число собранных клаузул
-- сверяется с числом найденных колонок. Разошлись — исполняется обращение к таблице с
-- говорящим именем `group_concat_was_truncated__raise_group_concat_max_len`, которой нет, и
-- миграция падает С ЧИТАЕМОЙ ПРИЧИНОЙ. Громкий отказ вместо тихой выдумки — то же правило,
-- ради которого снимаются сами дефолты, применённое к этому файлу.

-- ПОЧЕМУ ИМЕННО `_25_`, А НЕ `_21_` (первая редакция лежала там, и это было дефектом): файлы
-- применяются в порядке имён, а миграции `_21_pb_journal_mount` (19 дефолтов), `_22_alert20`
-- (2) и `_24_fight20` (1) ДОБАВЛЯЮТ колонки с дефолтом. Снятие, стоящее до них, на базе с нуля
-- оставило бы 22 колонки с дефолтом. Этот файл обязан быть ПОСЛЕДНИМ среди `pb_journal_*`.
-- И это же причина, по которой инвариант не может держаться порядком файлов: следующая
-- миграция снова встанет после этой. Держит его РАЗДЕЛ 15 в C:/WoW/tools/audit_run.sh —
-- он печатает число после каждого прогона, и ноль там единственный правильный ответ.

SET SESSION group_concat_max_len = 1048576;

-- Один блок на таблицу: `ALTER TABLE` целит в одну таблицу, а циклов в простом SQL нет.

SET @t := 'pb_journal_boot';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_decision';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_option';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_read';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_loot';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_metric';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_line';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_fight';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_aggro';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_death';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_actor';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_route';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_route_stop';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_issue';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_idle';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_stuck';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_alert';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_spot';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_spot_reject';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @t := 'pb_journal_mount';
SET @cl := NULL;
SELECT COUNT(*), GROUP_CONCAT(CONCAT('ALTER COLUMN `', COLUMN_NAME, '` DROP DEFAULT') ORDER BY ORDINAL_POSITION)
  INTO @n, @cl
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = @t AND IS_NULLABLE = 'NO' AND COLUMN_DEFAULT IS NOT NULL;
SET @built := (LENGTH(@cl) - LENGTH(REPLACE(@cl, 'ALTER COLUMN', ''))) / LENGTH('ALTER COLUMN');
SET @s := IF(@cl IS NULL, 'DO 0',
          IF(@built <> @n, 'SELECT 1 FROM `group_concat_was_truncated__raise_group_concat_max_len`',
             CONCAT('ALTER TABLE `', @t, '` ', @cl)));
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

