-- LLM autopilot for mod-ollama-chat. See docs/autopilot-plan.md.
--
--   autopilot            One row per bot that has ever been considered: how it
--                        was enrolled, the identity the LLM wrote for it, and
--                        the LLM's current plan (its goal, the strategies it
--                        switched, what its last orders did). The row
--                        survives unenrollment so a bot that comes back
--                        keeps who it is. `mode` records an explicit GM
--                        decision, which always beats the selection rules.
--
--   autopilot_snapshots  Periodic progress samples. Counters are running
--                        totals, so thinning old rows (hourly after a day,
--                        daily after a week) loses resolution but never
--                        loses counts.
--
--   autopilot_events     Notable moments: plans, levels, deaths, quests, rare
--                        loot, zone changes, goals. Trimmed to the newest N per
--                        bot.
--
-- Times are unix seconds so the downsampling buckets are plain integer
-- division and do not depend on the server time zone.
--
-- The worldserver also checks these tables at startup and repairs them to
-- this layout (mod-ollama-chat_autopilot_schema.cpp: missing tables created,
-- missing columns added, short VARCHARs widened; an unknown column is
-- dropped only when NOT NULL with no default), because the
-- updater never re-runs a CREATE TABLE IF NOT EXISTS against an existing
-- table. Keep the two in step.

CREATE TABLE IF NOT EXISTS mod_ollama_chat_autopilot (
    bot_guid BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    mode TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = selection rules decide, 1 = forced on, 2 = forced off',
    enrolled TINYINT UNSIGNED NOT NULL DEFAULT 0,
    source VARCHAR(32) NOT NULL DEFAULT '' COMMENT 'Which rule or command enrolled it',
    style VARCHAR(64) NOT NULL DEFAULT '' COMMENT 'Playstyle in a few words, written by the LLM',
    outlook VARCHAR(32) NOT NULL DEFAULT '' COMMENT 'in-character, player or metagamer, chosen by the LLM',
    profile TEXT NULL COMMENT 'Who the character is, written by the LLM',
    kills_total INT UNSIGNED NOT NULL DEFAULT 0,
    deaths_total INT UNSIGNED NOT NULL DEFAULT 0,
    quests_total INT UNSIGNED NOT NULL DEFAULT 0,
    dungeons_total INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Dungeon entries since enrollment',
    doing VARCHAR(64) NOT NULL DEFAULT '' COMMENT 'The LLM''s label for what the bot is doing',
    doing_since INT UNSIGNED NOT NULL DEFAULT 0,
    decided_by VARCHAR(16) NOT NULL DEFAULT '',
    strategies VARCHAR(2000) NOT NULL DEFAULT '' COMMENT 'Strategies the LLM switched with nc/co, as it last set them: +nc:grind,-co:aoe',
    last_results VARCHAR(2000) NOT NULL DEFAULT '' COMMENT 'The LLM''s last orders and what each did, one per line',
    goal_kind VARCHAR(24) NOT NULL DEFAULT '' COMMENT 'reach_level, reach_skill, earn_gold, explore_zone, complete_quests, run_dungeon, free; empty = none',
    goal_target VARCHAR(64) NOT NULL DEFAULT '' COMMENT 'Display form of the target',
    goal_target_id INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Zone or skill id',
    goal_value INT UNSIGNED NOT NULL DEFAULT 0,
    goal_baseline INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Counter value when the goal was set',
    goal_set_at INT UNSIGNED NOT NULL DEFAULT 0,
    goal_text VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'The goal in the character''s own words',
    last_reason VARCHAR(255) NOT NULL DEFAULT '',
    baseline VARCHAR(4000) NOT NULL DEFAULT '' COMMENT 'Both engines'' strategies before the LLM changed any: nc:a,b;co:c,d',
    plan_until INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'When the LLM asked to be consulted again',
    last_plan_at INT UNSIGNED NOT NULL DEFAULT 0,
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
