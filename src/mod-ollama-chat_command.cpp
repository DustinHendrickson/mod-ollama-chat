#include "mod-ollama-chat_command.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_capability.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_response.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat-utilities.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include <thread>
#include "Chat.h"
#include "Config.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include <fmt/core.h>

using namespace Acore::ChatCommands;

OllamaChatConfigCommand::OllamaChatConfigCommand()
    : CommandScript("OllamaChatConfigCommand")
{
}

ChatCommandTable OllamaChatConfigCommand::GetCommands() const
{
    static ChatCommandTable ollamaSentimentCommandTable =
    {
        { "view",  HandleOllamaSentimentViewCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "set",   HandleOllamaSentimentSetCommand,   SEC_ADMINISTRATOR, Console::Yes },
        { "reset", HandleOllamaSentimentResetCommand, SEC_ADMINISTRATOR, Console::Yes }
    };

    static ChatCommandTable ollamaPersonalityCommandTable =
    {
        { "get",  HandleOllamaPersonalityGetCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "set",  HandleOllamaPersonalitySetCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "list", HandleOllamaPersonalityListCommand, SEC_ADMINISTRATOR, Console::Yes }
    };

    static ChatCommandTable ollamaReloadCommandTable =
    {
        { "reload",      HandleOllamaReloadCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "status",      HandleOllamaStatusCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "test",        HandleOllamaTestCommand,    SEC_ADMINISTRATOR, Console::Yes },
        { "sentiment",   ollamaSentimentCommandTable },
        { "personality", ollamaPersonalityCommandTable }
    };

    static ChatCommandTable commandTable =
    {
        { "ollama", ollamaReloadCommandTable }
    };

    return commandTable;
}

bool OllamaChatConfigCommand::HandleOllamaReloadCommand(ChatHandler* handler)
{
    sConfigMgr->Reload();
    LoadOllamaChatConfig();
    Roleplay_Load();

    // Re-probe: the operator may have just pointed the module at a different
    // model, and think-mode support is per-model.
    OllamaCapability_Init(true);

    // Clear personality assignments if RP personalities are disabled
    // This ensures that when re-enabled later, bots get fresh random assignments
    if (!g_EnableRPPersonalities)
    {
        ClearAllBotPersonalities();
    }

    LoadBotPersonalityList();
    LoadBotConversationHistoryFromDB();
    InitializeSentimentTracking();
    handler->SendSysMessage("OllamaChat: Configuration reloaded from conf!");
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaSentimentViewCommand(ChatHandler* handler, Optional<std::string> botName, Optional<std::string> playerName)
{
    if (!g_EnableSentimentTracking)
    {
        handler->SendSysMessage("OllamaChat: Sentiment tracking is disabled.");
        return true;
    }

    if (!botName && !playerName)
    {
        // Show all sentiment data
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        if (g_BotPlayerSentiments.empty())
        {
            handler->SendSysMessage("OllamaChat: No sentiment data found.");
            return true;
        }

        handler->SendSysMessage("OllamaChat: All sentiment data:");
        for (const auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
            std::string botNameStr = bot ? bot->GetName() : std::to_string(botGuid);
            
            for (const auto& [playerGuid, sentiment] : playerMap)
            {
                Player* player = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
                std::string playerNameStr = player ? player->GetName() : std::to_string(playerGuid);
                
                handler->SendSysMessage(fmt::format("  Bot '{}' -> Player '{}': {:.3f}", 
                                        botNameStr, playerNameStr, sentiment));
            }
        }
        return true;
    }

    // Find specific bot or player
    Player* targetBot = nullptr;
    Player* targetPlayer = nullptr;

    if (botName)
    {
        targetBot = ObjectAccessor::FindPlayerByName(*botName);
        if (!targetBot)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", *botName));
            return true;
        }
        if (!PlayerbotsMgr::instance().GetPlayerbotAI(targetBot))
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", *botName));
            return true;
        }
    }

    if (playerName)
    {
        targetPlayer = ObjectAccessor::FindPlayerByName(*playerName);
        if (!targetPlayer)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' not found.", *playerName));
            return true;
        }
    }

    // Show sentiment for specific bot-player pair or all pairs involving a specific bot/player
    if (targetBot && targetPlayer)
    {
        float sentiment = GetBotPlayerSentiment(targetBot->GetGUID().GetRawValue(), targetPlayer->GetGUID().GetRawValue());
        handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' -> Player '{}': {:.3f}", 
                                targetBot->GetName(), targetPlayer->GetName(), sentiment));
    }
    else if (targetBot)
    {
        // Show all sentiments for this bot
        uint64_t botGuid = targetBot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        auto botIt = g_BotPlayerSentiments.find(botGuid);
        if (botIt == g_BotPlayerSentiments.end() || botIt->second.empty())
        {
            handler->SendSysMessage(fmt::format("OllamaChat: No sentiment data found for bot '{}'.", targetBot->GetName()));
            return true;
        }

        handler->SendSysMessage(fmt::format("OllamaChat: Sentiment data for bot '{}':", targetBot->GetName()));
        for (const auto& [playerGuid, sentiment] : botIt->second)
        {
            Player* player = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
            std::string playerNameStr = player ? player->GetName() : std::to_string(playerGuid);
            handler->SendSysMessage(fmt::format("  -> Player '{}': {:.3f}", playerNameStr, sentiment));
        }
    }
    else if (targetPlayer)
    {
        // Show all sentiments involving this player
        uint64_t playerGuid = targetPlayer->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        bool found = false;
        handler->SendSysMessage(fmt::format("OllamaChat: Sentiment data involving player '{}':", targetPlayer->GetName()));
        
        for (const auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            auto playerIt = playerMap.find(playerGuid);
            if (playerIt != playerMap.end())
            {
                Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
                std::string botNameStr = bot ? bot->GetName() : std::to_string(botGuid);
                handler->SendSysMessage(fmt::format("  Bot '{}' -> {:.3f}", botNameStr, playerIt->second));
                found = true;
            }
        }
        
        if (!found)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: No sentiment data found involving player '{}'.", targetPlayer->GetName()));
        }
    }

    return true;
}

bool OllamaChatConfigCommand::HandleOllamaSentimentSetCommand(ChatHandler* handler, std::string botName, std::string playerName, float sentimentValue)
{
    if (!g_EnableSentimentTracking)
    {
        handler->SendSysMessage("OllamaChat: Sentiment tracking is disabled.");
        return true;
    }

    Player* bot = ObjectAccessor::FindPlayerByName(botName);
    if (!bot)
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", botName));
        return true;
    }
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", botName));
        return true;
    }

    Player* player = ObjectAccessor::FindPlayerByName(playerName);
    if (!player)
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' not found.", playerName));
        return true;
    }

    if (sentimentValue < 0.0f || sentimentValue > 1.0f)
    {
        handler->SendSysMessage("OllamaChat: Sentiment value must be between 0.0 and 1.0.");
        return true;
    }

    SetBotPlayerSentiment(bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue(), sentimentValue);
    handler->SendSysMessage(fmt::format("OllamaChat: Set sentiment between bot '{}' and player '{}' to {:.3f}.", 
                            botName, playerName, sentimentValue));
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaSentimentResetCommand(ChatHandler* handler, Optional<std::string> botName, Optional<std::string> playerName)
{
    if (!g_EnableSentimentTracking)
    {
        handler->SendSysMessage("OllamaChat: Sentiment tracking is disabled.");
        return true;
    }

    if (!botName && !playerName)
    {
        // Reset all sentiment data
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        uint32_t count = 0;
        for (const auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            count += playerMap.size();
        }
        g_BotPlayerSentiments.clear();
        g_DirtySentiments.clear();
        CharacterDatabase.Execute("DELETE FROM mod_ollama_chat_bot_player_sentiments");
        handler->SendSysMessage(fmt::format("OllamaChat: Reset all sentiment data ({} records).", count));
        return true;
    }

    Player* targetBot = nullptr;
    Player* targetPlayer = nullptr;

    if (botName)
    {
        targetBot = ObjectAccessor::FindPlayerByName(*botName);
        if (!targetBot)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", *botName));
            return true;
        }
        if (!PlayerbotsMgr::instance().GetPlayerbotAI(targetBot))
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", *botName));
            return true;
        }
    }

    if (playerName)
    {
        targetPlayer = ObjectAccessor::FindPlayerByName(*playerName);
        if (!targetPlayer)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' not found.", *playerName));
            return true;
        }
    }

    if (targetBot && targetPlayer)
    {
        // Reset specific bot-player sentiment
        SetBotPlayerSentiment(targetBot->GetGUID().GetRawValue(), targetPlayer->GetGUID().GetRawValue(), g_SentimentDefaultValue);
        handler->SendSysMessage(fmt::format("OllamaChat: Reset sentiment between bot '{}' and player '{}' to default ({:.3f}).", 
                                targetBot->GetName(), targetPlayer->GetName(), g_SentimentDefaultValue));
    }
    else if (targetBot)
    {
        // Reset all sentiments for this bot
        uint64_t botGuid = targetBot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        auto botIt = g_BotPlayerSentiments.find(botGuid);
        if (botIt != g_BotPlayerSentiments.end())
        {
            uint32_t count = botIt->second.size();
            g_BotPlayerSentiments.erase(botIt);

            for (auto it = g_DirtySentiments.begin(); it != g_DirtySentiments.end(); )
            {
                if (it->first == botGuid)
                    it = g_DirtySentiments.erase(it);
                else
                    ++it;
            }

            CharacterDatabase.Execute(SafeFormat(
                "DELETE FROM mod_ollama_chat_bot_player_sentiments WHERE bot_guid = {}", botGuid));
            handler->SendSysMessage(fmt::format("OllamaChat: Reset all sentiment data for bot '{}' ({} records).", 
                                    targetBot->GetName(), count));
        }
        else
        {
            handler->SendSysMessage(fmt::format("OllamaChat: No sentiment data found for bot '{}'.", targetBot->GetName()));
        }
    }
    else if (targetPlayer)
    {
        // Reset all sentiments involving this player
        uint64_t playerGuid = targetPlayer->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        uint32_t count = 0;
        for (auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            auto playerIt = playerMap.find(playerGuid);
            if (playerIt != playerMap.end())
            {
                playerMap.erase(playerIt);
                count++;
            }
        }

        for (auto it = g_DirtySentiments.begin(); it != g_DirtySentiments.end(); )
        {
            if (it->second == playerGuid)
                it = g_DirtySentiments.erase(it);
            else
                ++it;
        }

        CharacterDatabase.Execute(SafeFormat(
            "DELETE FROM mod_ollama_chat_bot_player_sentiments WHERE player_guid = {}", playerGuid));

        handler->SendSysMessage(fmt::format("OllamaChat: Reset all sentiment data involving player '{}' ({} records).", 
                                targetPlayer->GetName(), count));
    }

    return true;
}

bool OllamaChatConfigCommand::HandleOllamaPersonalityGetCommand(ChatHandler* handler, std::string botName)
{
    Player* bot = ObjectAccessor::FindPlayerByName(botName);
    if (!bot)
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", botName));
        return true;
    }
    
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", botName));
        return true;
    }
    
    std::string personality = GetBotPersonality(bot);
    std::string prompt = GetPersonalityPromptAddition(personality);
    
    handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' has personality '{}'", botName, personality));
    handler->SendSysMessage(fmt::format("  Prompt: {}", prompt));
    
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaPersonalitySetCommand(ChatHandler* handler, std::string botName, std::string personality)
{
    Player* bot = ObjectAccessor::FindPlayerByName(botName);
    if (!bot)
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", botName));
        return true;
    }
    
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", botName));
        return true;
    }
    
    if (!PersonalityExists(personality))
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Personality '{}' does not exist. Use '.ollama personality list' to see available personalities.", personality));
        return true;
    }
    
    if (SetBotPersonality(bot, personality))
    {
        std::string prompt = GetPersonalityPromptAddition(personality);
        handler->SendSysMessage(fmt::format("OllamaChat: Set bot '{}' personality to '{}'", botName, personality));
        handler->SendSysMessage(fmt::format("  Prompt: {}", prompt));
    }
    else
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Failed to set personality for bot '{}'.", botName));
    }
    
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaPersonalityListCommand(ChatHandler* handler)
{
    std::vector<std::string> personalities = GetAllPersonalityKeys();
    
    if (personalities.empty())
    {
        handler->SendSysMessage("OllamaChat: No personalities loaded.");
        return true;
    }
    
    handler->SendSysMessage(fmt::format("OllamaChat: Available personalities ({} total, {} random-assignable):", 
                            personalities.size(), g_PersonalityKeysRandomOnly.size()));
    
    for (const auto& personality : personalities)
    {
        std::string prompt = GetPersonalityPromptAddition(personality);
        
        // Check if this personality is manual-only
        bool isManualOnly = (std::find(g_PersonalityKeysRandomOnly.begin(), g_PersonalityKeysRandomOnly.end(), personality) 
                            == g_PersonalityKeysRandomOnly.end());
        
        std::string manualTag = isManualOnly ? " [MANUAL ONLY]" : "";
        
        handler->SendSysMessage(fmt::format("  - {}{}", personality, manualTag));
        handler->SendSysMessage(fmt::format("    {}", prompt));
    }
    
    return true;
}


// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

bool OllamaChatConfigCommand::HandleOllamaStatusCommand(ChatHandler* handler)
{
    const OllamaDispatchStats dispatch = OllamaDispatch_GetStats();
    const GovernorStats       gov      = Governor_GetStats();

    handler->PSendSysMessage("|cff00ff00[Ollama Chat] Status|r");
    handler->PSendSysMessage("Module: %s   Endpoint: %s   Model: %s",
                             g_Enable ? "enabled" : "DISABLED",
                             g_OllamaUrl.c_str(), g_OllamaModel.c_str());
    handler->PSendSysMessage("Think: %s", OllamaCapability_StatusText().c_str());

    handler->PSendSysMessage("Dispatcher: %u workers, %u queued, %u in flight, %u awaiting delivery",
                             dispatch.workers, dispatch.queuedRequests,
                             dispatch.inFlight, dispatch.pendingDeliveries);
    handler->PSendSysMessage("Totals: %llu submitted, %llu delivered, %llu failed",
                             (unsigned long long)dispatch.totalSubmitted,
                             (unsigned long long)dispatch.totalDelivered,
                             (unsigned long long)dispatch.totalFailed);
    handler->PSendSysMessage("Dropped: %llu queue-full, %llu empty-after-cleanup, %llu by governor",
                             (unsigned long long)dispatch.totalDroppedQueueFull,
                             (unsigned long long)dispatch.totalDroppedEmpty,
                             (unsigned long long)dispatch.totalDroppedGovernor);

    handler->PSendSysMessage("Governor: %u bots, %u scopes tracked, %u sends in the last minute",
                             gov.trackedBots, gov.trackedScopes, gov.sendsLastMinute);
    handler->PSendSysMessage("Blocked: %u cooldown, %u rate, %u repetition, %u chain-depth, %u no-audience",
                             gov.blockedCooldown, gov.blockedRate, gov.blockedRepetition,
                             gov.blockedChainDepth, gov.blockedNoAudience);

    handler->PSendSysMessage("Roleplay: %s (strictness %u)   Emote reactions: %s",
                             g_RoleplayEnable ? "on" : "off",
                             (uint32)g_RoleplayStrictness,
                             g_EnableEmoteReactions ? "on" : "off");
    handler->PSendSysMessage("Topic weights: people %u / world %u / activity %u / self %u / guild %u",
                             g_TopicWeightPeople, g_TopicWeightWorld, g_TopicWeightActivity,
                             g_TopicWeightSelf, g_TopicWeightGuild);

    if (!dispatch.lastError.empty())
        handler->PSendSysMessage("|cffff0000Last error:|r %s", dispatch.lastError.c_str());
    else
        handler->PSendSysMessage("Last error: none");

    return true;
}

bool OllamaChatConfigCommand::HandleOllamaTestCommand(ChatHandler* handler, Acore::ChatCommands::Tail prompt)
{
    std::string text(prompt);
    if (text.empty())
    {
        handler->SendSysMessage("Usage: .ollama test <prompt>");
        handler->SetSentErrorMessage(true);
        return false;
    }

    handler->PSendSysMessage("[Ollama Chat] Sending test prompt, please wait...");

    // Blocking HTTP must not run on the world thread, so do the round trip on
    // a scratch thread and report from there. Turns "the bots are quiet" into
    // a one-command diagnosis: you see the raw output and the cleaned output
    // side by side.
    std::thread([text]()
    {
        OllamaApiResult api = QueryOllama(text, OllamaRequestKind::ChatReply);

        if (!api.ok)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] TEST FAILED after {}ms: {}",
                     api.latencyMs, api.error.empty() ? "unknown error" : api.error);
            return;
        }

        uint32_t emote = 0;
        const std::string cleaned = ProcessLlmResponse(api.text, "Tester", &emote);

        LOG_INFO("module.ollamachat", "[Ollama Chat] TEST ok in {}ms (think={}).",
                 api.latencyMs, api.thinkUsed ? "yes" : "no");
        LOG_INFO("module.ollamachat", "[Ollama Chat] TEST raw     : {}", api.text);
        LOG_INFO("module.ollamachat", "[Ollama Chat] TEST cleaned : {}", cleaned);
        if (emote)
            LOG_INFO("module.ollamachat", "[Ollama Chat] TEST emote   : {}", emote);
        if (!api.thinking.empty())
            LOG_INFO("module.ollamachat", "[Ollama Chat] TEST thinking: {}", api.thinking);
    }).detach();

    handler->PSendSysMessage("[Ollama Chat] Result will appear in the server log (module.ollamachat).");
    return true;
}
