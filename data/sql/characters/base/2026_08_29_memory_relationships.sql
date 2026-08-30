-- Long-term memory and relationships for mod-ollama-chat.
--
-- Conversation history (mod_ollama_chat_history) is a sliding window: once a
-- line falls out of it the bot has no idea it ever happened. These two tables
-- hold what survives that window.
--
--   memories       Short narrator-style notes distilled from history when it
--                  crosses a token budget, each scored 1-10 for importance.
--                  The raw history is cleared afterwards; the memories persist.
--
--   relationships  One sentence per (bot, person) pair on how that bot feels
--                  about them, rewritten as the relationship develops.
--
-- Both are bounded at prompt-build time by their own token budgets, so they
-- cannot grow the prompt without limit no matter how long a bot has been alive.

CREATE TABLE IF NOT EXISTS mod_ollama_chat_memories (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    bot_guid BIGINT UNSIGNED NOT NULL,
    memory_text TEXT NOT NULL COMMENT 'Short third-person note, typically under 20 words',
    importance TINYINT UNSIGNED NOT NULL DEFAULT 5 COMMENT '1 = trivial, 10 = defining',
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_bot_guid (bot_guid),
    INDEX idx_bot_importance (bot_guid, importance)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS mod_ollama_chat_relationships (
    bot_guid BIGINT UNSIGNED NOT NULL,
    other_guid BIGINT UNSIGNED NOT NULL COMMENT 'Player or other bot',
    other_name VARCHAR(64) NOT NULL DEFAULT '',
    description TEXT NOT NULL COMMENT 'How this bot feels about them, in its own words',
    mentions INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Mention counter; resets on each revision',
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (bot_guid, other_guid),
    INDEX idx_bot_guid (bot_guid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
