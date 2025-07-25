CREATE TABLE IF NOT EXISTS mod_ollama_chat_bot_player_sentiments (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    bot_guid BIGINT UNSIGNED NOT NULL,
    player_guid BIGINT UNSIGNED NOT NULL,
    sentiment_value FLOAT NOT NULL DEFAULT 0.5 COMMENT 'Sentiment value: 0.0=hostile, 0.5=neutral, 1.0=friendly',
    last_updated DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    -- Unique constraint to ensure one sentiment record per bot-player pair
    UNIQUE KEY unique_sentiment (bot_guid, player_guid),
    -- Indexes for performance
    INDEX idx_bot_guid (bot_guid),
    INDEX idx_player_guid (player_guid),
    INDEX idx_sentiment_value (sentiment_value)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
