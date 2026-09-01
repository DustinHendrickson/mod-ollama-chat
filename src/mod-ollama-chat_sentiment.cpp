#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat-utilities.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include "Player.h"
#include <fmt/core.h>
#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

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
    g_DirtySentiments.emplace(botGuid, playerGuid);
    
    if (g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Set sentiment between bot {} and player {} to {:.2f}", 
                 botGuid, playerGuid, sentimentValue);
    }
}

std::string BuildSentimentPrompt(const std::string& message)
{
    if (!g_EnableSentimentTracking || message.empty() || g_SentimentAnalysisPrompt.empty())
        return "";

    return SafeFormat(g_SentimentAnalysisPrompt, fmt::arg("message", message));
}

float AnalyzeMessageSentiment(const std::string& prompt)
{
    if (!g_EnableSentimentTracking || prompt.empty())
        return 0.0f;
    
    if (g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Sentiment analysis prompt: {}", prompt);
    }
    
    // Sentiment is a judgement call and is never shown to players, so this is
    // the one request kind that auto think-mode turns reasoning ON for.
    OllamaApiResult api = QueryOllama(prompt, OllamaRequestKind::Sentiment);
    std::string response = api.ok ? api.text : std::string();

    if (response.empty())
    {
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Empty sentiment analysis response");
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
        LOG_INFO("module.ollamachat", "[Ollama Chat] Sentiment analysis: '{}' -> adjustment: {:.2f}", 
                 response, adjustment);
    }
    
    return adjustment;
}

void ApplySentimentAnalysis(uint64_t botGuid, uint64_t playerGuid,
                            const std::string& message, const std::string& prompt)
{
    if (!g_EnableSentimentTracking || message.empty() || prompt.empty())
        return;

    const float currentSentiment = GetBotPlayerSentiment(botGuid, playerGuid);
    const float adjustment       = AnalyzeMessageSentiment(prompt);

    if (adjustment == 0.0f)
        return;

    const float newSentiment = currentSentiment + adjustment;
    SetBotPlayerSentiment(botGuid, playerGuid, newSentiment);

    if (g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] Sentiment {:.2f} -> {:.2f} ({:+.2f}) for bot {} toward player {}",
                 currentSentiment, newSentiment, adjustment, botGuid, playerGuid);
    }
}

void UpdateBotPlayerSentiment(Player* bot, Player* player, const std::string& message)
{
    if (!g_EnableSentimentTracking || !bot || !player || message.empty())
        return;

    // Hand off rather than block. This used to run the LLM call inline, on
    // whatever thread happened to be delivering the reply.
    OllamaDispatch_SubmitSentiment(bot->GetGUID().GetRawValue(),
                                   player->GetGUID().GetRawValue(),
                                   message);
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
    g_DirtySentiments.clear();
    
    QueryResult result = CharacterDatabase.Query("SELECT bot_guid, player_guid, sentiment_value FROM mod_ollama_chat_bot_player_sentiments");
    
    if (!result)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] No existing sentiment data found in database");
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
    
    LOG_INFO("module.ollamachat", "[Ollama Chat] Loaded {} sentiment records from database", count);
}

void SaveBotPlayerSentimentsToDB()
{
    if (!g_EnableSentimentTracking)
        return;

    // Only the pairs that actually moved since the last save. This used to
    // rewrite every pair the server had ever tracked, every interval, whether
    // or not a single sentiment had changed.
    std::vector<std::pair<std::pair<uint64_t, uint64_t>, float>> changed;

    {
        std::lock_guard<std::mutex> lock(g_SentimentMutex);

        if (g_DirtySentiments.empty())
            return;

        changed.reserve(g_DirtySentiments.size());

        for (const auto& [botGuid, playerGuid] : g_DirtySentiments)
        {
            auto botIt = g_BotPlayerSentiments.find(botGuid);
            if (botIt == g_BotPlayerSentiments.end())
                continue;

            auto playerIt = botIt->second.find(playerGuid);
            if (playerIt == botIt->second.end())
                continue;    // reset out from under us; the reset did its own delete

            changed.push_back({ { botGuid, playerGuid }, playerIt->second });
        }

        g_DirtySentiments.clear();
    }

    if (changed.empty())
        return;

    // INSERT ... ON DUPLICATE KEY UPDATE rather than REPLACE INTO. REPLACE is
    // a delete plus an insert: it rewrites the row, all three secondary
    // indexes and the auto-increment counter even when only the float moved.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    for (const auto& [key, sentimentValue] : changed)
    {
        trans->Append(SafeFormat(
            "INSERT INTO mod_ollama_chat_bot_player_sentiments "
            "(bot_guid, player_guid, sentiment_value) VALUES ({}, {}, {:.3f}) "
            "ON DUPLICATE KEY UPDATE sentiment_value = VALUES(sentiment_value)",
            key.first, key.second, sentimentValue));
    }

    CharacterDatabase.CommitTransaction(trans);

    if (g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Saved {} changed sentiment record(s) to database",
                 static_cast<uint32_t>(changed.size()));
    }
}

void InitializeSentimentTracking()
{
    if (!g_EnableSentimentTracking)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Sentiment tracking is disabled");
        return;
    }
    
    LOG_INFO("module.ollamachat", "[Ollama Chat] Initializing sentiment tracking system...");
    
    // Load existing sentiment data from database
    LoadBotPlayerSentimentsFromDB();
    
    // Initialize the last save time
    g_LastSentimentSaveTime = time(nullptr);
    
    LOG_INFO("module.ollamachat", "[Ollama Chat] Sentiment tracking system initialized");
}
