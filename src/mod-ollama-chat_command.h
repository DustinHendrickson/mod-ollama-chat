#ifndef MOD_OLLAMA_CHAT_COMMAND_H
#define MOD_OLLAMA_CHAT_COMMAND_H

#include "ScriptMgr.h"
#include "Chat.h"

class OllamaChatConfigCommand : public CommandScript
{
public:
    OllamaChatConfigCommand();
    Acore::ChatCommands::ChatCommandTable GetCommands() const override;

    static bool HandleOllamaReloadCommand(ChatHandler* handler);
    static bool HandleOllamaSentimentViewCommand(ChatHandler* handler, Optional<std::string> botName, Optional<std::string> playerName);
    static bool HandleOllamaSentimentSetCommand(ChatHandler* handler, std::string botName, std::string playerName, float sentimentValue);
    static bool HandleOllamaSentimentResetCommand(ChatHandler* handler, Optional<std::string> botName, Optional<std::string> playerName);
    static bool HandleOllamaPersonalityGetCommand(ChatHandler* handler, std::string botName);
    static bool HandleOllamaPersonalitySetCommand(ChatHandler* handler, std::string botName, std::string personality);
    static bool HandleOllamaPersonalityListCommand(ChatHandler* handler);
};

#endif // MOD_OLLAMA_CHAT_COMMAND_H
