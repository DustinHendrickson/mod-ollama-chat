# Bot-Player Sentiment Tracking System

This document describes the new sentiment tracking system implemented in mod-ollama-chat, which allows bots to develop persistent relationships with players based on their interactions.

## Overview

The sentiment tracking system enables bots to remember how players treat them over time and adjust their behavior accordingly. Bots become more friendly toward players who are nice to them, and more hostile toward players who are rude or aggressive.

## How It Works

### Sentiment Values
- Each bot-player relationship has a sentiment value between **0.0** and **1.0**
- **0.0** = Extremely hostile/negative relationship
- **0.5** = Neutral relationship (default for new interactions)
- **1.0** = Extremely friendly/positive relationship

### Sentiment Analysis
1. When a player sends a message to a bot, the system analyzes the message sentiment using the LLM
2. The LLM classifies the message as POSITIVE, NEGATIVE, or NEUTRAL
3. The bot's sentiment toward that player is adjusted accordingly:
   - POSITIVE messages increase sentiment by the configured adjustment strength
   - NEGATIVE messages decrease sentiment by the configured adjustment strength
   - NEUTRAL messages cause no change

### Sentiment Integration
- Sentiment information is automatically included in bot prompts
- Bots receive context like: "Your relationship sentiment with PlayerName is 0.8 (0.0=hostile, 0.5=neutral, 1.0=friendly). Use this to guide your tone and response."
- This allows bots to respond with appropriate warmth, coldness, or neutrality based on history

## Database Schema

The system creates a new table: `mod_ollama_chat_bot_player_sentiments`

```sql
CREATE TABLE IF NOT EXISTS mod_ollama_chat_bot_player_sentiments (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    bot_guid BIGINT UNSIGNED NOT NULL,
    player_guid BIGINT UNSIGNED NOT NULL,
    sentiment_value FLOAT NOT NULL DEFAULT 0.5,
    last_updated DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY unique_sentiment (bot_guid, player_guid)
);
```

## Configuration

Add these settings to your `mod_ollama_chat.conf` file:

```ini
# Enable sentiment tracking (default: 1)
OllamaChat.EnableSentimentTracking = 1

# Default sentiment for new relationships (default: 0.5)
OllamaChat.SentimentDefaultValue = 0.5

# How much to adjust sentiment per message (default: 0.1)
OllamaChat.SentimentAdjustmentStrength = 0.1

# How often to save sentiment data in minutes (default: 10)
OllamaChat.SentimentSaveInterval = 10

# Prompt for sentiment analysis
OllamaChat.SentimentAnalysisPrompt = "Analyze the sentiment of this message: \"{message}\". Respond only with: POSITIVE, NEGATIVE, or NEUTRAL."

# Template for including sentiment in bot prompts
OllamaChat.SentimentPromptTemplate = "Your relationship sentiment with {player_name} is {sentiment_value} (0.0=hostile, 0.5=neutral, 1.0=friendly). Use this to guide your tone and response."
```

### Important: Update Your Prompt Templates

Make sure your chat and event prompt templates include the `{sentiment_info}` placeholder:

```ini
# Example chat template with sentiment
OllamaChat.ChatPromptTemplate = "You are {bot_name}, a {bot_class} bot. {bot_personality} {sentiment_info} Player {player_name} says: {player_message}"

# Example event template with sentiment  
OllamaChat.EventChatterPromptTemplate = "You are {bot_name}, a {bot_class} bot. {bot_personality} {sentiment_info} React to this event: {event_type} involving {actor_name}"
```

## Admin Commands

The system provides several admin commands for managing sentiment data:

### View Sentiment Data
```
.ollama sentiment view [botname] [playername]
```
- No arguments: Shows all sentiment data
- Bot name only: Shows all sentiments for that bot
- Player name only: Shows all sentiments involving that player  
- Both names: Shows specific bot-player sentiment

### Set Sentiment Value
```
.ollama sentiment set <botname> <playername> <value>
```
- Manually sets sentiment between a bot and player
- Value must be between 0.0 and 1.0

### Reset Sentiment Data
```
.ollama sentiment reset [botname] [playername]
```
- No arguments: Resets ALL sentiment data
- Bot name only: Resets all sentiments for that bot
- Player name only: Resets all sentiments involving that player
- Both names: Resets specific bot-player sentiment to default

## Performance Considerations

### Memory Usage
- Sentiment data is cached in memory for fast access
- Data is periodically saved to database (configurable interval)
- Memory usage scales with number of unique bot-player interactions

### LLM Load
- Each player message triggers an additional LLM call for sentiment analysis
- Consider your LLM server capacity when enabling this feature
- Sentiment analysis uses a simple prompt and expects short responses

### Database Load
- Sentiment data is batched and saved periodically
- Uses REPLACE INTO for efficient upserts
- Minimal database impact with reasonable save intervals

## Examples

### Example Interaction Flow

1. **First Interaction**: Player "Alice" says "Hello there!" to bot "BotTank"
   - No existing sentiment → starts at 0.5 (neutral)
   - Message analyzed as POSITIVE
   - Sentiment increases to 0.6
   - Bot responds neutrally/positively

2. **Positive Interaction**: Alice says "You're the best tank ever!"
   - Current sentiment: 0.6
   - Message analyzed as POSITIVE  
   - Sentiment increases to 0.7
   - Bot responds more warmly

3. **Negative Interaction**: Alice says "You suck at tanking!"
   - Current sentiment: 0.7
   - Message analyzed as NEGATIVE
   - Sentiment decreases to 0.6
   - Bot response becomes cooler

### Example Bot Responses Based on Sentiment

**High Sentiment (0.8+)**:
- "Hey Alice, great to see you again!"
- "Always happy to help you out!"
- Enthusiastic, helpful tone

**Neutral Sentiment (0.4-0.6)**:
- "Hello Alice."
- "Sure, I can help with that."
- Polite but not enthusiastic

**Low Sentiment (0.2-)**:
- "What do you want?"
- "I suppose I could help..."
- Cold, reluctant tone

## Troubleshooting

### Sentiment Not Updating
1. Check that `OllamaChat.EnableSentimentTracking = 1`
2. Verify your LLM is responding correctly to sentiment analysis prompts
3. Check server logs for sentiment analysis debug messages
4. Ensure your chat templates include `{sentiment_info}` placeholder

### Performance Issues
1. Increase `OllamaChat.SentimentSaveInterval` to reduce database writes
2. Consider reducing `OllamaChat.SentimentAdjustmentStrength` for fewer LLM calls
3. Monitor your LLM server load

### Database Issues
1. Ensure the sentiment table was created properly
2. Check database permissions for the characters database
3. Verify database connection in server logs

## Technical Implementation Details

### Files Added/Modified
- `mod-ollama-chat_sentiment.h` - Sentiment system header
- `mod-ollama-chat_sentiment.cpp` - Sentiment system implementation  
- `mod-ollama-chat_config.h` - Added sentiment configuration variables
- `mod-ollama-chat_config.cpp` - Added sentiment config loading
- `mod-ollama-chat_handler.cpp` - Integrated sentiment into chat processing
- `mod-ollama-chat_events.cpp` - Integrated sentiment into event system
- `mod-ollama-chat_command.h/.cpp` - Added sentiment admin commands
- `mod-ollama-chat_random.cpp` - Added sentiment periodic saving
- `2025_07_25_sentiment_tracking.sql` - Database schema

### Thread Safety
- All sentiment data access is protected by mutex locks
- Sentiment analysis runs in background threads
- Database operations are batched for efficiency

### Integration Points
1. **Chat Handler**: Sentiment analysis and updates happen during chat processing
2. **Event System**: Event prompts include sentiment context when actor is a player
3. **Prompt Generation**: All bot prompts automatically include sentiment information
4. **Periodic Saves**: Sentiment data is saved alongside conversation history

This system provides a persistent, evolving relationship dynamic that makes bot interactions feel more personal and reactive to player behavior over time.
