#include "mod-ollama-chat_personality.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "Log.h"
#include "mod-ollama-chat_config.h"
#include "DatabaseEnv.h"
#include <random>
#include <vector>
#include <atomic>
#include <mutex>

// Internal personality map. Runs on map threads (kill, loot and level events)
// as well as the world thread, so the shared map is only touched under
// g_BotPersonalityMutex.
std::string GetBotPersonality(Player* bot)
{
    const uint64_t botGuid = bot->GetGUID().GetRawValue();
    std::string chosenPersonality;
    {
        std::lock_guard<std::mutex> lock(g_BotPersonalityMutex);

        auto it = g_BotPersonalityList.find(botGuid);
        if (!g_EnableRPPersonalities || g_PersonalityKeysRandomOnly.empty())
        {
            // RP personalities disabled or config not loaded
            if (it == g_BotPersonalityList.end())
                g_BotPersonalityList.emplace(botGuid, "default");
            else
                it->second = "default";
            return "default";
        }
        if (it != g_BotPersonalityList.end())
        {
            if (it->second.empty())
                it->second = "default";
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Using existing personality '{}' for bot {}", it->second,
                         bot->GetName());
            return it->second;
        }

        // Otherwise, assign randomly from config (only from non-manual personalities)
        const uint32 newIdx = urand(0, g_PersonalityKeysRandomOnly.size() - 1);
        chosenPersonality   = g_PersonalityKeysRandomOnly[newIdx];
        g_BotPersonalityList.emplace(botGuid, chosenPersonality);
    }

    // Saved if the table exists. Checked once: the lookup is a synchronous
    // query, and this can run on a map thread.
    static std::atomic<int> tableState{ 0 };   // 0 unknown, 1 present, 2 missing
    if (tableState.load() == 0)
    {
        QueryResult tableExists = CharacterDatabase.Query(
            "SELECT * FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_ollama_chat_personality' LIMIT 1;");
        tableState.store(tableExists ? 1 : 2);
        if (!tableExists)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Please source the required database table first");
    }
    if (tableState.load() == 1)
        CharacterDatabase.Execute("INSERT INTO mod_ollama_chat_personality (guid, personality) VALUES ({}, '{}')", botGuid, chosenPersonality);

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] Assigned new personality '{}' to bot {}", chosenPersonality, bot->GetName());
    return chosenPersonality;
}


std::string GetPersonalityPromptAddition(const std::string& personality)
{
    auto it = g_PersonalityPrompts.find(personality);
    if (it != g_PersonalityPrompts.end())
        return it->second;
    return g_DefaultPersonalityPrompt;
}

bool SetBotPersonality(Player* bot, const std::string& personality)
{
    if (!bot)
        return false;
    
    uint64_t botGuid = bot->GetGUID().GetRawValue();
    
    // Check if personality exists
    if (g_PersonalityPrompts.find(personality) == g_PersonalityPrompts.end() && personality != "default")
    {
        return false;
    }
    
    // Update in memory
    g_BotPersonalityList[botGuid] = personality;
    
    // Update in database
    CharacterDatabase.Execute("REPLACE INTO mod_ollama_chat_personality (guid, personality) VALUES ({}, '{}')", 
                             botGuid, personality);
    
    if(g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Set personality '{}' for bot {}", personality, bot->GetName());
    }
    
    return true;
}

std::vector<std::string> GetAllPersonalityKeys()
{
    return g_PersonalityKeys;
}

bool PersonalityExists(const std::string& personality)
{
    if (personality == "default")
        return true;
    return g_PersonalityPrompts.find(personality) != g_PersonalityPrompts.end();
}

void ClearAllBotPersonalities()
{
    g_BotPersonalityList.clear();
    if(g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Cleared all bot personality assignments due to RP personalities being disabled");
    }
}
