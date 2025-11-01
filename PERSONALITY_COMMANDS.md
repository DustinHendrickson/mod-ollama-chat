# Bot Personality Management Commands

This document describes the new personality management system for mod-ollama-chat.

## Database Changes

A new column `manual_only` has been added to the `mod_ollama_chat_personality_templates` table:

```sql
ALTER TABLE `mod_ollama_chat_personality_templates`
ADD COLUMN `manual_only` TINYINT(1) NOT NULL DEFAULT 0 AFTER `prompt`;
```

When `manual_only` is set to `1` (TRUE), that personality will NOT be randomly assigned to bots. It can only be assigned manually through commands or direct database updates.

## Commands

All commands require Administrator permission (SEC_ADMINISTRATOR).

### `.ollama personality get <botname>`

Gets the current personality assigned to a specific bot.

**Example:**
```
.ollama personality get Molarini
```

**Output:**
```
OllamaChat: Bot 'Molarini' has personality 'GAMER'
  Prompt: Focus on game mechanics, min-maxing, and efficiency.
```

### `.ollama personality set <botname> <personality>`

Sets a specific personality for a bot. This saves the personality to the database.

**Example:**
```
.ollama personality set Molarini ROLEPLAY
```

**Output:**
```
OllamaChat: Set bot 'Molarini' personality to 'ROLEPLAY'
  Prompt: Respond in-character, weaving lore into your response.
```

### `.ollama personality list`

Lists all available personalities in the system, showing which ones are manual-only.

**Example:**
```
.ollama personality list
```

**Output:**
```
OllamaChat: Available personalities (33 total, 30 random-assignable):
  - GAMER
    Focus on game mechanics, min-maxing, and efficiency.
  - ROLEPLAYER
    Respond in-character, weaving lore into your response.
  - SPECIAL_ADMIN_ONLY [MANUAL ONLY]
    A special personality that can only be assigned manually.
  ...
```

## Manual-Only Personalities

To create a manual-only personality, set the `manual_only` column to `1` in the database:

```sql
INSERT INTO `mod_ollama_chat_personality_templates` (`key`, `prompt`, `manual_only`) VALUES
('SPECIAL_NPC', 'Act like a specific named NPC with unique dialogue.', 1);
```

or update an existing one:

```sql
UPDATE `mod_ollama_chat_personality_templates` 
SET `manual_only` = 1 
WHERE `key` = 'EDGE_LORD';
```

## Use Cases

1. **Special Event Personalities**: Create personalities for special events that shouldn't be randomly assigned
2. **Admin-Controlled Personalities**: Reserve certain personalities for specific bots
3. **Testing Personalities**: Mark experimental personalities as manual-only until they're ready for general use

## Integration with Existing System

- The random personality assignment system now only uses personalities where `manual_only = 0`
- Manually assigned personalities persist across server restarts (stored in database)
- The `.ollama reload` command reloads personality templates from the database
- Bots with manually assigned personalities keep them even if personalities are reloaded
