ALTER TABLE `playerbot_companions`
  ADD COLUMN `milestones` tinyint unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `request_map` int unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `request_state` tinyint unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `request_repeat` tinyint unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `request_at` bigint NOT NULL DEFAULT '0';

ALTER TABLE `playerbot_companion_memories`
  MODIFY COLUMN `memory_type` enum('adventure','boss_kill','dungeon_completion','milestone','tradition') NOT NULL,
  ADD COLUMN `pinned` tinyint unsigned NOT NULL DEFAULT '0';

CREATE TABLE IF NOT EXISTS `playerbot_companion_lineups` (
  `owner_guid` int unsigned NOT NULL,
  `owner_created_at` bigint NOT NULL,
  `name` varchar(32) NOT NULL,
  `bot_guid` int unsigned NOT NULL,
  `bot_created_at` bigint NOT NULL,
  PRIMARY KEY (`owner_guid`, `owner_created_at`, `name`, `bot_guid`),
  KEY `idx_lineup_bot` (`owner_guid`, `owner_created_at`, `bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=`ascii` COLLATE=`ascii_general_ci`;
