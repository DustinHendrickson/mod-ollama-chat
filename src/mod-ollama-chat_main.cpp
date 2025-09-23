#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_random.h"
#include "mod-ollama-chat_events.h"
#include "mod-ollama-chat_command.h"
#include "mod-ollama-chat_rag.h"
#include "Log.h"

void Addmod_ollama_chatScripts()
{
    LOG_INFO("server.loading", "[Ollama Chat] Registering mod-ollama-chat scripts.");
    new OllamaChatConfigWorldScript();
    new PlayerBotChatHandler();
    new OllamaBotRandomChatter();

    LOG_INFO("server.loading", "[Ollama Chat] Registering mod-ollama-chat events.");
    new ChatOnKill();
    new ChatOnLoot();
    new ChatOnDeath();
    new ChatOnQuest();
    new ChatOnLearn();
    new ChatOnDuel();
    new ChatOnLevelUp();
    new ChatOnAchievement();
    new ChatOnGameObjectUse();
    new OllamaChatConfigCommand();
}
