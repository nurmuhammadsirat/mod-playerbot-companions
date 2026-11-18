CREATE TABLE IF NOT EXISTS `playerbot_companions` (
  `owner_guid` int unsigned NOT NULL,
  `owner_created_at` bigint NOT NULL,
  `bot_guid` int unsigned NOT NULL,
  `bot_created_at` bigint NOT NULL,
  `recruited_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`owner_guid`, `owner_created_at`, `bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
