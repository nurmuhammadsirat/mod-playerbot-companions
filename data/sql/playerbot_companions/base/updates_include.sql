CREATE TABLE IF NOT EXISTS `updates_include` (
  `path` varchar(200) NOT NULL,
  `state` enum('RELEASED','ARCHIVED','CUSTOM') NOT NULL DEFAULT 'RELEASED',
  PRIMARY KEY (`path`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DELETE FROM `updates_include` WHERE `path` = '$/data/sql/playerbot_companions/updates';
INSERT INTO `updates_include` (`path`, `state`) VALUES ('$/data/sql/playerbot_companions/updates', 'RELEASED');
