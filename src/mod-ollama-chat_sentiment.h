#ifndef MOD_OLLAMA_CHAT_SENTIMENT_H
#define MOD_OLLAMA_CHAT_SENTIMENT_H

#include <string>
#include <cstdint>
#include "Player.h"

// --------------------------------------------
// Sentiment Tracking Functions
// --------------------------------------------

/**
 * Get the current sentiment value between a bot and player
 * @param botGuid GUID of the bot
 * @param playerGuid GUID of the player
 * @return Sentiment value (0.0-1.0), or default value if not found
 */
float GetBotPlayerSentiment(uint64_t botGuid, uint64_t playerGuid);

/**
 * Set the sentiment value between a bot and player
 * @param botGuid GUID of the bot
 * @param playerGuid GUID of the player
 * @param sentimentValue New sentiment value (0.0-1.0)
 */
void SetBotPlayerSentiment(uint64_t botGuid, uint64_t playerGuid, float sentimentValue);

/**
 * Analyze the sentiment of a message using LLM
 * @param message The message to analyze
 * @return Sentiment adjustment (-1.0 to 1.0)
 */
float AnalyzeMessageSentiment(const std::string& message);

/**
 * Update sentiment based on a player's message to a bot
 * @param bot The bot receiving the message
 * @param player The player sending the message
 * @param message The message content
 */
void UpdateBotPlayerSentiment(Player* bot, Player* player, const std::string& message);

/**
 * Get sentiment prompt addition for including in bot responses
 * @param bot The bot
 * @param player The player they're responding to
 * @return Formatted sentiment prompt addition
 */
std::string GetSentimentPromptAddition(Player* bot, Player* player);

/**
 * Load all sentiment data from database into memory
 */
void LoadBotPlayerSentimentsFromDB();

/**
 * Save all sentiment data from memory to database
 */
void SaveBotPlayerSentimentsToDB();

/**
 * Initialize the sentiment tracking system
 */
void InitializeSentimentTracking();

#endif // MOD_OLLAMA_CHAT_SENTIMENT_H
