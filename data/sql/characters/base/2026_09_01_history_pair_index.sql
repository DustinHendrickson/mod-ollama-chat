-- Index for the per-pair conversation history trim.
--
-- mod_ollama_chat_history shipped with only its primary key and a UNIQUE key
-- over (bot_guid, player_guid, player_message(255), bot_reply(255)). Neither
-- can serve "the newest N rows for this bot/player pair", so the old cleanup
-- fell back to a ROW_NUMBER() window over the entire table on every save.
--
-- SaveBotConversationHistoryToDB() now trims by auto-increment id per pair,
-- which this index covers end to end -- no scan, no filesort.
--
-- Guarded so the file stays safe to re-apply: base/ scripts are idempotent by
-- convention here, and a bare ALTER would fail the second time.

SET @have_index := (
    SELECT COUNT(*) FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'mod_ollama_chat_history'
      AND INDEX_NAME   = 'idx_pair_recent'
);

SET @ddl := IF(@have_index = 0,
    'ALTER TABLE mod_ollama_chat_history ADD INDEX idx_pair_recent (bot_guid, player_guid, id)',
    'DO 0');

PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
