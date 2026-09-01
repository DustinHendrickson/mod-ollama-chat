
CREATE TABLE IF NOT EXISTS mod_ollama_chat_history (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    bot_guid BIGINT UNSIGNED NOT NULL,
    player_guid BIGINT UNSIGNED NOT NULL,
    timestamp DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    player_message TEXT NOT NULL,
    bot_reply TEXT NOT NULL,
    -- This unique key prevents any duplicates for the same conversation turn
    UNIQUE KEY unique_history (bot_guid, player_guid, player_message(255), bot_reply(255)),
    -- Covers the per-pair "newest N rows" trim in SaveBotConversationHistoryToDB().
    INDEX idx_pair_recent (bot_guid, player_guid, id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;