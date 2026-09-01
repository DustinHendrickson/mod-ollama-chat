#ifndef MOD_OLLAMA_CHAT_HANDLER_H
#define MOD_OLLAMA_CHAT_HANDLER_H

#include "ScriptMgr.h"
#include <string>
#include <cstdint>

class Channel;
class Player;

enum ChatChannelSourceLocal
{
    SRC_UNDEFINED_LOCAL  = 0,
    SRC_SAY_LOCAL        = 1,
    SRC_PARTY_LOCAL      = 2,
    SRC_RAID_LOCAL       = 3,
    SRC_GUILD_LOCAL      = 4,
    SRC_OFFICER_LOCAL    = 5,
    SRC_YELL_LOCAL       = 6,
    SRC_WHISPER_LOCAL    = 7,
    SRC_GENERAL_LOCAL    = 17
};

extern const char* ChatChannelSourceLocalStr[];

std::string rtrim(const std::string& s);
ChatChannelSourceLocal GetChannelSourceLocal(uint32_t type);

// Called after a bot has spoken so other bots can hear it.
//
// chainDepth is how many bot->bot hops led here; a message from a real player
// is depth 0. The governor refuses beyond MaxChainDepth and decays the reply
// chance at each hop, which is what stops the endless bot conversations.
void ProcessBotChatMessage(Player* bot, const std::string& msg,
                           ChatChannelSourceLocal sourceLocal, Channel* channel,
                           uint8_t chainDepth = 1);

void SaveBotConversationHistoryToDB();
void DeleteBotConversationHistoryFromDB(uint64_t botGuid);
void AppendBotConversation(uint64_t botGuid, uint64_t playerGuid,
                           const std::string& playerMessage, const std::string& botReply);

// Prompt builders. World thread only -- they read live world state.
std::string GenerateBotPrompt(Player* bot, std::string playerMessage, Player* player);
std::string BuildEmoteReactionPrompt(Player* bot, Player* player, uint32_t textEmote);

// Bounded, distance-sorted snapshot helpers used by the prompt builders and
// the topic engine.
std::string GenerateBotGameStateSnapshot(Player* bot);

class PlayerBotChatHandler : public PlayerScript
{
public:
    PlayerBotChatHandler() : PlayerScript("PlayerBotChatHandler", {
        PLAYERHOOK_CAN_PLAYER_USE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
    }) {}

    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* group) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Guild* guild) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel) override;

    static void ProcessChat(Player* player, uint32_t type, uint32_t lang, std::string& msg,
                            ChatChannelSourceLocal sourceLocal, Channel* channel = nullptr,
                            Player* receiver = nullptr, uint8_t chainDepth = 0);
};

// Housekeeping: prunes the per-bot maps that used to grow forever.
class OllamaChatMaintenance : public PlayerScript
{
public:
    OllamaChatMaintenance() : PlayerScript("OllamaChatMaintenance", { PLAYERHOOK_ON_LOGOUT }) {}
    void OnPlayerLogout(Player* player) override;
};

#endif // MOD_OLLAMA_CHAT_HANDLER_H
