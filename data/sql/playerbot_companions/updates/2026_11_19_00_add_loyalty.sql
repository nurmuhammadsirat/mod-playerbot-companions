ALTER TABLE `playerbot_companions`
  ADD COLUMN `loyalty` tinyint unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `last_shared_at` bigint NOT NULL DEFAULT '0';
