-- [cert20] ЭПОХА СЕРТИФИКАЦИИ: баг, мешающий сертифицировать, обнуляет счётчик чистых прогонов.
--
-- Слово пользователя (2026-11-15): «раз баг есть, который мешает сертифицировать, значит, обнуляй счётчик
-- до нуля. Это только если был рестарт, то тогда нейтрально.» Прерванный прогон остаётся нейтральным
-- (не засчитывается и не записывается в ошибки) — это его прежнее слово, и оно не отменено.
--
-- ПОЧЕМУ ОТМЕТКОЙ, А НЕ УДАЛЕНИЕМ ИСТОРИИ. Прогонов в истории 699 (rfc-n 297, wc-n 261, dm-n 88, sfk-n 52).
-- Удалить их значило бы потерять единственное доказательство того, ЧТО именно было сломано и когда; удалений
-- у нас ноль по правилу, и к записям это относится так же, как к вещам. Поэтому счётчик обнуляется ГРАНИЦЕЙ:
-- в серию чистых прогонов входят только прогоны, закончившиеся НЕ РАНЬШЕ `epoch_at`. Всё, что до неё, остаётся
-- в таблице и читается запросом, но к сертификату не относится.
--
-- `epoch_why` — чем именно обнулено, СЛОВАМИ. Без него через неделю будет не отличить «обнулили из-за бага»
-- от «так исторически вышло», и граница превратится в число без причины.
--
-- ИДЕМПОТЕНТНО. В MySQL 8 нет ADD COLUMN IF NOT EXISTS (это MariaDB), поэтому охрана через information_schema
-- + PREPARE: сервер добавляет колонки сам при старте, и голый ALTER уже стоил падения подъёма (ERROR 1060).
SET @c := (SELECT COUNT(*) FROM information_schema.COLUMNS
           WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'guild_project_cert' AND COLUMN_NAME = 'epoch_at');
SET @s := IF(@c = 0,
  "ALTER TABLE `guild_project_cert` ADD COLUMN `epoch_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '[cert20] прогоны, закончившиеся раньше, в серию чистых не входят; 0 = граница не ставилась'",
  'DO 0');
PREPARE s FROM @s; EXECUTE s; DEALLOCATE PREPARE s;

SET @c := (SELECT COUNT(*) FROM information_schema.COLUMNS
           WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'guild_project_cert' AND COLUMN_NAME = 'epoch_why');
SET @s := IF(@c = 0,
  "ALTER TABLE `guild_project_cert` ADD COLUMN `epoch_why` TEXT COMMENT '[cert20] чем обнулено, словами; NULL = эмитент не передал'",
  'DO 0');
PREPARE s FROM @s; EXECUTE s; DEALLOCATE PREPARE s;
