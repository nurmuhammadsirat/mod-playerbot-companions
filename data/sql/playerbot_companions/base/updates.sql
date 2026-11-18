CREATE TABLE IF NOT EXISTS `updates` (
  `name` varchar(200) NOT NULL,
  `hash` char(40) DEFAULT '',
  `state` enum('RELEASED','ARCHIVED','CUSTOM') NOT NULL DEFAULT 'RELEASED',
  `timestamp` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `speed` int unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
