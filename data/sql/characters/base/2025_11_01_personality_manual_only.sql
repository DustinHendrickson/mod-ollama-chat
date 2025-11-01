-- Add manual_only column to personality templates table
ALTER TABLE `mod_ollama_chat_personality_templates`
ADD COLUMN `manual_only` TINYINT(1) NOT NULL DEFAULT 0 AFTER `prompt`;
