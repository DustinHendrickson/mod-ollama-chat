#include "mod-ollama-chat_command.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_personality.h"
#include "Chat.h"
#include "Config.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotMgr.h"

using namespace Acore::ChatCommands;

OllamaChatConfigCommand::OllamaChatConfigCommand()
    : CommandScript("OllamaChatConfigCommand")
{
}

ChatCommandTable OllamaChatConfigCommand::GetCommands() const
{
    static ChatCommandTable ollamaSentimentCommandTable =
    {
        { "view", HandleOllamaSentimentViewCommand, SEC_ADMINISTRATOR, Console::Yes },
        { "set", HandleOllamaSentimentSetCommand, SEC_ADMINISTRATOR, Console::Yes },
        { "reset", HandleOllamaSentimentResetCommand, SEC_ADMINISTRATOR, Console::Yes }
    };

    static ChatCommandTable ollamaPersonalityCommandTable =
    {
        { "get", HandleOllamaPersonalityGetCommand, SEC_ADMINISTRATOR, Console::Yes },
        { "set", HandleOllamaPersonalitySetCommand, SEC_ADMINISTRATOR, Console::Yes },
        { "list", HandleOllamaPersonalityListCommand, SEC_ADMINISTRATOR, Console::Yes }
    };

    static ChatCommandTable ollamaReloadCommandTable =
    {
        { "reload", HandleOllamaReloadCommand, SEC_ADMINISTRATOR, Console::Yes },
        { "sentiment", ollamaSentimentCommandTable },
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
                
                handler->PSendSysMessage("  Bot '%s' -> Player '%s': %.3f", 
                                        botNameStr.c_str(), playerNameStr.c_str(), sentiment);
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
            handler->PSendSysMessage("OllamaChat: Bot '%s' not found.", botName->c_str());
            return true;
        }
        if (!sPlayerbotsMgr->GetPlayerbotAI(targetBot))
        {
            handler->PSendSysMessage("OllamaChat: Player '%s' is not a bot.", botName->c_str());
            return true;
        }
    }

    if (playerName)
    {
        targetPlayer = ObjectAccessor::FindPlayerByName(*playerName);
        if (!targetPlayer)
        {
            handler->PSendSysMessage("OllamaChat: Player '%s' not found.", playerName->c_str());
            return true;
        }
    }

    // Show sentiment for specific bot-player pair or all pairs involving a specific bot/player
    if (targetBot && targetPlayer)
    {
        float sentiment = GetBotPlayerSentiment(targetBot->GetGUID().GetRawValue(), targetPlayer->GetGUID().GetRawValue());
        handler->PSendSysMessage("OllamaChat: Bot '%s' -> Player '%s': %.3f", 
                                targetBot->GetName().c_str(), targetPlayer->GetName().c_str(), sentiment);
    }
    else if (targetBot)
    {
        // Show all sentiments for this bot
        uint64_t botGuid = targetBot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        auto botIt = g_BotPlayerSentiments.find(botGuid);
        if (botIt == g_BotPlayerSentiments.end() || botIt->second.empty())
        {
            handler->PSendSysMessage("OllamaChat: No sentiment data found for bot '%s'.", targetBot->GetName().c_str());
            return true;
        }

        handler->PSendSysMessage("OllamaChat: Sentiment data for bot '%s':", targetBot->GetName().c_str());
        for (const auto& [playerGuid, sentiment] : botIt->second)
        {
            Player* player = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
            std::string playerNameStr = player ? player->GetName() : std::to_string(playerGuid);
            handler->PSendSysMessage("  -> Player '%s': %.3f", playerNameStr.c_str(), sentiment);
        }
    }
    else if (targetPlayer)
    {
        // Show all sentiments involving this player
        uint64_t playerGuid = targetPlayer->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        bool found = false;
        handler->PSendSysMessage("OllamaChat: Sentiment data involving player '%s':", targetPlayer->GetName().c_str());
        
        for (const auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            auto playerIt = playerMap.find(playerGuid);
            if (playerIt != playerMap.end())
            {
                Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
                std::string botNameStr = bot ? bot->GetName() : std::to_string(botGuid);
                handler->PSendSysMessage("  Bot '%s' -> %.3f", botNameStr.c_str(), playerIt->second);
                found = true;
            }
        }
        
        if (!found)
        {
            handler->PSendSysMessage("OllamaChat: No sentiment data found involving player '%s'.", targetPlayer->GetName().c_str());
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
        handler->PSendSysMessage("OllamaChat: Bot '%s' not found.", botName.c_str());
        return true;
    }
    if (!sPlayerbotsMgr->GetPlayerbotAI(bot))
    {
        handler->PSendSysMessage("OllamaChat: Player '%s' is not a bot.", botName.c_str());
        return true;
    }

    Player* player = ObjectAccessor::FindPlayerByName(playerName);
    if (!player)
    {
        handler->PSendSysMessage("OllamaChat: Player '%s' not found.", playerName.c_str());
        return true;
    }

    if (sentimentValue < 0.0f || sentimentValue > 1.0f)
    {
        handler->SendSysMessage("OllamaChat: Sentiment value must be between 0.0 and 1.0.");
        return true;
    }

    SetBotPlayerSentiment(bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue(), sentimentValue);
    handler->PSendSysMessage("OllamaChat: Set sentiment between bot '%s' and player '%s' to %.3f.", 
                            botName.c_str(), playerName.c_str(), sentimentValue);
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
        handler->PSendSysMessage("OllamaChat: Reset all sentiment data (%u records).", count);
        return true;
    }

    Player* targetBot = nullptr;
    Player* targetPlayer = nullptr;

    if (botName)
    {
        targetBot = ObjectAccessor::FindPlayerByName(*botName);
        if (!targetBot)
        {
            handler->PSendSysMessage("OllamaChat: Bot '%s' not found.", botName->c_str());
            return true;
        }
        if (!sPlayerbotsMgr->GetPlayerbotAI(targetBot))
        {
            handler->PSendSysMessage("OllamaChat: Player '%s' is not a bot.", botName->c_str());
            return true;
        }
    }

    if (playerName)
    {
        targetPlayer = ObjectAccessor::FindPlayerByName(*playerName);
        if (!targetPlayer)
        {
            handler->PSendSysMessage("OllamaChat: Player '%s' not found.", playerName->c_str());
            return true;
        }
    }

    if (targetBot && targetPlayer)
    {
        // Reset specific bot-player sentiment
        SetBotPlayerSentiment(targetBot->GetGUID().GetRawValue(), targetPlayer->GetGUID().GetRawValue(), g_SentimentDefaultValue);
        handler->PSendSysMessage("OllamaChat: Reset sentiment between bot '%s' and player '%s' to default (%.3f).", 
                                targetBot->GetName().c_str(), targetPlayer->GetName().c_str(), g_SentimentDefaultValue);
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
            handler->PSendSysMessage("OllamaChat: Reset all sentiment data for bot '%s' (%u records).", 
                                    targetBot->GetName().c_str(), count);
        }
        else
        {
            handler->PSendSysMessage("OllamaChat: No sentiment data found for bot '%s'.", targetBot->GetName().c_str());
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
        
        handler->PSendSysMessage("OllamaChat: Reset all sentiment data involving player '%s' (%u records).", 
                                targetPlayer->GetName().c_str(), count);
    }

    return true;
}

bool OllamaChatConfigCommand::HandleOllamaPersonalityGetCommand(ChatHandler* handler, std::string botName)
{
    Player* bot = ObjectAccessor::FindPlayerByName(botName);
    if (!bot)
    {
        handler->PSendSysMessage("OllamaChat: Bot '%s' not found.", botName.c_str());
        return true;
    }
    
    if (!sPlayerbotsMgr->GetPlayerbotAI(bot))
    {
        handler->PSendSysMessage("OllamaChat: Player '%s' is not a bot.", botName.c_str());
        return true;
    }
    
    std::string personality = GetBotPersonality(bot);
    std::string prompt = GetPersonalityPromptAddition(personality);
    
    handler->PSendSysMessage("OllamaChat: Bot '%s' has personality '%s'", botName.c_str(), personality.c_str());
    handler->PSendSysMessage("  Prompt: %s", prompt.c_str());
    
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaPersonalitySetCommand(ChatHandler* handler, std::string botName, std::string personality)
{
    Player* bot = ObjectAccessor::FindPlayerByName(botName);
    if (!bot)
    {
        handler->PSendSysMessage("OllamaChat: Bot '%s' not found.", botName.c_str());
        return true;
    }
    
    if (!sPlayerbotsMgr->GetPlayerbotAI(bot))
    {
        handler->PSendSysMessage("OllamaChat: Player '%s' is not a bot.", botName.c_str());
        return true;
    }
    
    if (!PersonalityExists(personality))
    {
        handler->PSendSysMessage("OllamaChat: Personality '%s' does not exist. Use '.ollama personality list' to see available personalities.", personality.c_str());
        return true;
    }
    
    if (SetBotPersonality(bot, personality))
    {
        std::string prompt = GetPersonalityPromptAddition(personality);
        handler->PSendSysMessage("OllamaChat: Set bot '%s' personality to '%s'", botName.c_str(), personality.c_str());
        handler->PSendSysMessage("  Prompt: %s", prompt.c_str());
    }
    else
    {
        handler->PSendSysMessage("OllamaChat: Failed to set personality for bot '%s'.", botName.c_str());
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
    
    handler->PSendSysMessage("OllamaChat: Available personalities (%zu total, %zu random-assignable):", 
                            personalities.size(), g_PersonalityKeysRandomOnly.size());
    
    for (const auto& personality : personalities)
    {
        std::string prompt = GetPersonalityPromptAddition(personality);
        
        // Check if this personality is manual-only
        bool isManualOnly = (std::find(g_PersonalityKeysRandomOnly.begin(), g_PersonalityKeysRandomOnly.end(), personality) 
                            == g_PersonalityKeysRandomOnly.end());
        
        std::string manualTag = isManualOnly ? " [MANUAL ONLY]" : "";
        
        handler->PSendSysMessage("  - %s%s", personality.c_str(), manualTag.c_str());
        handler->PSendSysMessage("    %s", prompt.c_str());
    }
    
    return true;
}
