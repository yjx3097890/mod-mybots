-- mod-mybots schema for acore_characters.
-- Applied automatically by AzerothCore DB Updater / ac-db-import
-- (modules/mod-mybots/data/sql/db-characters/). Jobs unused until P1/P2.
CREATE TABLE IF NOT EXISTS `mybots_job` (
  `id` CHAR(36) NOT NULL,
  `char_guid` INT UNSIGNED NOT NULL,
  `account_id` INT UNSIGNED NOT NULL,
  `type` VARCHAR(32) NOT NULL,
  `status` VARCHAR(16) NOT NULL,
  `payload` TEXT,
  `error` TEXT,
  `created_at` INT UNSIGNED NOT NULL,
  `updated_at` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_char_status` (`char_guid`, `status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `mybots_job_step` (
  `job_id` CHAR(36) NOT NULL,
  `ordinal` INT NOT NULL,
  `op` VARCHAR(32) NOT NULL,
  `status` VARCHAR(16) NOT NULL,
  `detail` TEXT,
  PRIMARY KEY (`job_id`, `ordinal`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `mybots_quest_script` (
  `quest_id` INT UNSIGNED NOT NULL,
  `script` MEDIUMTEXT NOT NULL,
  `updated_at` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`quest_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `mybots_patrol` (
  `id` VARCHAR(64) NOT NULL,
  `name` VARCHAR(128) NOT NULL,
  `waypoints` MEDIUMTEXT NOT NULL,
  `account_id` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `mybots_event` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `char_guid` INT UNSIGNED NOT NULL,
  `job_id` CHAR(36) DEFAULT NULL,
  `kind` VARCHAR(32) NOT NULL,
  `message` TEXT,
  `created_at` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_char_time` (`char_guid`, `created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
