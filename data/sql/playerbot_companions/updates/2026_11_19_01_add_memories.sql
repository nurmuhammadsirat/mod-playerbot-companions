CREATE TABLE IF NOT EXISTS `playerbot_companion_memories` (
  `id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `owner_guid` int unsigned NOT NULL,
  `owner_created_at` bigint NOT NULL,
  `bot_guid` int unsigned NOT NULL,
  `bot_created_at` bigint NOT NULL,
  `memory_type` enum('adventure','boss_kill','dungeon_completion') NOT NULL,
  `map_id` int unsigned NOT NULL,
  `instance_id` int unsigned NOT NULL,
  `difficulty` tinyint unsigned NOT NULL,
  `subject_id` int unsigned NOT NULL,
  `occurred_at` bigint NOT NULL,
  `summary` varchar(255) NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_companion_memory` (
    `owner_guid`, `owner_created_at`, `bot_guid`, `bot_created_at`, `memory_type`,
    `map_id`, `instance_id`, `difficulty`, `subject_id`, `occurred_at`
  ),
  KEY `idx_companion_recent` (`owner_guid`, `owner_created_at`, `bot_guid`, `bot_created_at`, `occurred_at`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
