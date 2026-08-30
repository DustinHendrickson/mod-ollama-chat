-- Roleplay voice packs for mod-ollama-chat.
--
-- Race and class "voices" are what turn {bot_race} / {bot_class} from a stat
-- line into a way of speaking. The module ships sensible defaults in code;
-- rows here override them per race or per class, so a server can retune how
-- its characters sound without a rebuild.
--
--   kind = 'race'   -> id is a Races value      (1 Human, 2 Orc, 3 Dwarf,
--                                                4 Night Elf, 5 Undead,
--                                                6 Tauren, 7 Gnome, 8 Troll,
--                                                10 Blood Elf, 11 Draenei)
--   kind = 'class'  -> id is a Classes value    (1 Warrior, 2 Paladin,
--                                                3 Hunter, 4 Rogue, 5 Priest,
--                                                6 Death Knight, 7 Shaman,
--                                                8 Mage, 9 Warlock, 11 Druid)
--
-- The prompt text is appended to the bot's prompt verbatim, so write it as an
-- instruction addressed to the character ("You speak slowly and...").

CREATE TABLE IF NOT EXISTS mod_ollama_chat_voice (
    kind ENUM('race','class') NOT NULL COMMENT 'Which axis this voice applies to',
    id TINYINT UNSIGNED NOT NULL COMMENT 'Races or Classes enum value',
    prompt TEXT NOT NULL COMMENT 'Prompt fragment appended for this race/class',
    PRIMARY KEY (kind, id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
