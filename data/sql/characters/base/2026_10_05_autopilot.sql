-- LLM autopilot for mod-ollama-chat. See docs/autopilot-plan.md.
--
--   autopilot            One row per bot that has ever been considered. The row
--                        survives unenrollment so a bot that comes back keeps
--                        its playstyle and its history. `mode` records an
--                        explicit GM decision, which always beats the
--                        selection rules.
--
--   autopilot_snapshots  Periodic progress samples. Counters are running
--                        totals, so thinning old rows (hourly after a day,
--                        daily after a week) loses resolution but never
--                        loses counts.
--
--   autopilot_events     Notable moments: levels, deaths, quests, rare loot,
--                        zone changes, enrollment. Trimmed to the newest N per
--                        bot.
--
-- Times are unix seconds so the downsampling buckets are plain integer
-- division and do not depend on the server time zone.

CREATE TABLE IF NOT EXISTS mod_ollama_chat_autopilot (
    bot_guid BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    mode TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = selection rules decide, 1 = forced on, 2 = forced off',
    enrolled TINYINT UNSIGNED NOT NULL DEFAULT 0,
    source VARCHAR(32) NOT NULL DEFAULT '' COMMENT 'Which rule or command enrolled it',
    playstyle VARCHAR(32) NOT NULL DEFAULT '',
    awareness VARCHAR(32) NOT NULL DEFAULT '',
    kills_total INT UNSIGNED NOT NULL DEFAULT 0,
    deaths_total INT UNSIGNED NOT NULL DEFAULT 0,
    quests_total INT UNSIGNED NOT NULL DEFAULT 0,
    activity VARCHAR(32) NOT NULL DEFAULT '' COMMENT 'Current activity preset',
    activity_since INT UNSIGNED NOT NULL DEFAULT 0,
    decided_by VARCHAR(16) NOT NULL DEFAULT '' COMMENT 'llm, policy, guard or gm',
    dispositions VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'axis:option,...',
    goal_text VARCHAR(255) NOT NULL DEFAULT '',
    last_reason VARCHAR(255) NOT NULL DEFAULT '',
    enrolled_at INT UNSIGNED NOT NULL DEFAULT 0,
    updated_at INT UNSIGNED NOT NULL DEFAULT 0,
    INDEX idx_enrolled (enrolled)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS mod_ollama_chat_autopilot_snapshots (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    bot_guid BIGINT UNSIGNED NOT NULL,
    taken_at INT UNSIGNED NOT NULL,
    level TINYINT UNSIGNED NOT NULL,
    xp INT UNSIGNED NOT NULL,
    money INT UNSIGNED NOT NULL COMMENT 'Copper',
    map_id SMALLINT UNSIGNED NOT NULL,
    zone_id INT UNSIGNED NOT NULL,
    area_id INT UNSIGNED NOT NULL,
    durability_pct TINYINT UNSIGNED NOT NULL,
    free_bag_slots SMALLINT UNSIGNED NOT NULL,
    quests_active TINYINT UNSIGNED NOT NULL,
    quests_rewarded INT UNSIGNED NOT NULL COMMENT 'Lifetime, from the character',
    kills_total INT UNSIGNED NOT NULL COMMENT 'Since enrollment',
    deaths_total INT UNSIGNED NOT NULL COMMENT 'Since enrollment',
    quests_total INT UNSIGNED NOT NULL COMMENT 'Completions since enrollment',
    professions VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'skillId:value/max,...',
    INDEX idx_bot_time (bot_guid, taken_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS mod_ollama_chat_autopilot_events (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    bot_guid BIGINT UNSIGNED NOT NULL,
    at INT UNSIGNED NOT NULL,
    type VARCHAR(32) NOT NULL,
    detail VARCHAR(255) NOT NULL DEFAULT '',
    INDEX idx_bot_id (bot_guid, id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
