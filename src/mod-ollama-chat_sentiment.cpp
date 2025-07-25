#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat-utilities.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include "Player.h"
#include <fmt/core.h>
#include <algorithm>
#include <mutex>

float GetBotPlayerSentiment(uint64_t botGuid, uint64_t playerGuid)
{
    if (!g_EnableSentimentTracking)
        return g_SentimentDefaultValue;

    std::lock_guard<std::mutex> lock(g_SentimentMutex);
    
    auto botIt = g_BotPlayerSentiments.find(botGuid);
    if (botIt != g_BotPlayerSentiments.end())
    {
        auto playerIt = botIt->second.find(playerGuid);
        if (playerIt != botIt->second.end())
        {
            return playerIt->second;
        }
    }
    
    // Return default value if not found
    return g_SentimentDefaultValue;
}

void SetBotPlayerSentiment(uint64_t botGuid, uint64_t playerGuid, float sentimentValue)
{
    if (!g_EnableSentimentTracking)
        return;

    // Clamp sentiment value to valid range [0.0, 1.0]
    sentimentValue = std::max(0.0f, std::min(1.0f, sentimentValue));
    
    std::lock_guard<std::mutex> lock(g_SentimentMutex);
    g_BotPlayerSentiments[botGuid][playerGuid] = sentimentValue;
    
    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[OllamaChat] Set sentiment between bot {} and player {} to {:.2f}", 
                 botGuid, playerGuid, sentimentValue);
    }
}

float AnalyzeMessageSentiment(const std::string& message)
{
    if (!g_EnableSentimentTracking || message.empty())
        return 0.0f;

    // Format the sentiment analysis prompt
    std::string prompt = SafeFormat(g_SentimentAnalysisPrompt, fmt::arg("message", message));
    
    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[OllamaChat] Sentiment analysis prompt: {}", prompt);
    }
    
    // Query the LLM for sentiment analysis
    std::string response = QueryOllamaAPI(prompt);
    
    if (response.empty())
    {
        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[OllamaChat] Empty sentiment analysis response");
        return 0.0f;
    }
    
    // Convert response to uppercase for comparison
    std::string upperResponse = response;
    std::transform(upperResponse.begin(), upperResponse.end(), upperResponse.begin(), ::toupper);
    
    // Parse the sentiment response
    float adjustment = 0.0f;
    if (upperResponse.find("POSITIVE") != std::string::npos)
    {
        adjustment = g_SentimentAdjustmentStrength;
    }
    else if (upperResponse.find("NEGATIVE") != std::string::npos)
    {
        adjustment = -g_SentimentAdjustmentStrength;
    }
    // NEUTRAL or unrecognized = 0.0f (no change)
    
    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[OllamaChat] Sentiment analysis: '{}' -> adjustment: {:.2f}", 
                 response, adjustment);
    }
    
    return adjustment;
}

void UpdateBotPlayerSentiment(Player* bot, Player* player, const std::string& message)
{
    if (!g_EnableSentimentTracking || !bot || !player)
        return;

    uint64_t botGuid = bot->GetGUID().GetRawValue();
    uint64_t playerGuid = player->GetGUID().GetRawValue();
    
    // Get current sentiment
    float currentSentiment = GetBotPlayerSentiment(botGuid, playerGuid);
    
    // Analyze the message sentiment
    float adjustment = AnalyzeMessageSentiment(message);
    
    // Apply the adjustment
    float newSentiment = currentSentiment + adjustment;
    
    // Set the updated sentiment
    SetBotPlayerSentiment(botGuid, playerGuid, newSentiment);
    
    if (g_DebugEnabled && adjustment != 0.0f)
    {
        LOG_INFO("server.loading", "[OllamaChat] Updated sentiment: {} -> {} ({:+.2f}) for bot {} and player {}", 
                 currentSentiment, newSentiment, adjustment, bot->GetName(), player->GetName());
    }
}

std::string GetSentimentPromptAddition(Player* bot, Player* player)
{
    if (!g_EnableSentimentTracking || !bot || !player || g_SentimentPromptTemplate.empty())
        return "";

    uint64_t botGuid = bot->GetGUID().GetRawValue();
    uint64_t playerGuid = player->GetGUID().GetRawValue();
    
    float sentimentValue = GetBotPlayerSentiment(botGuid, playerGuid);
    
    return SafeFormat(
        g_SentimentPromptTemplate,
        fmt::arg("player_name", player->GetName()),
        fmt::arg("sentiment_value", sentimentValue)
    );
}

void LoadBotPlayerSentimentsFromDB()
{
    if (!g_EnableSentimentTracking)
        return;

    std::lock_guard<std::mutex> lock(g_SentimentMutex);
    g_BotPlayerSentiments.clear();
    
    QueryResult result = CharacterDatabase.Query("SELECT bot_guid, player_guid, sentiment_value FROM mod_ollama_chat_bot_player_sentiments");
    
    if (!result)
    {
        LOG_INFO("server.loading", "[OllamaChat] No existing sentiment data found in database");
        return;
    }
    
    uint32_t count = 0;
    do
    {
        Field* fields = result->Fetch();
        uint64_t botGuid = fields[0].Get<uint64_t>();
        uint64_t playerGuid = fields[1].Get<uint64_t>();
        float sentimentValue = fields[2].Get<float>();
        
        g_BotPlayerSentiments[botGuid][playerGuid] = sentimentValue;
        count++;
        
    } while (result->NextRow());
    
    LOG_INFO("server.loading", "[OllamaChat] Loaded {} sentiment records from database", count);
}

void SaveBotPlayerSentimentsToDB()
{
    if (!g_EnableSentimentTracking)
        return;

    std::lock_guard<std::mutex> lock(g_SentimentMutex);
    
    if (g_BotPlayerSentiments.empty())
        return;
    
    // Use REPLACE INTO to update existing records or insert new ones
    for (const auto& [botGuid, playerMap] : g_BotPlayerSentiments)
    {
        for (const auto& [playerGuid, sentimentValue] : playerMap)
        {
            CharacterDatabase.Execute(SafeFormat(
                "REPLACE INTO mod_ollama_chat_bot_player_sentiments (bot_guid, player_guid, sentiment_value) "
                "VALUES ({}, {}, {:.3f})",
                botGuid, playerGuid, sentimentValue));
        }
    }
    
    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[OllamaChat] Saved sentiment data to database");
    }
}

void InitializeSentimentTracking()
{
    if (!g_EnableSentimentTracking)
    {
        LOG_INFO("server.loading", "[OllamaChat] Sentiment tracking is disabled");
        return;
    }
    
    LOG_INFO("server.loading", "[OllamaChat] Initializing sentiment tracking system...");
    
    // Load existing sentiment data from database
    LoadBotPlayerSentimentsFromDB();
    
    // Initialize the last save time
    g_LastSentimentSaveTime = time(nullptr);
    
    LOG_INFO("server.loading", "[OllamaChat] Sentiment tracking system initialized");
}
