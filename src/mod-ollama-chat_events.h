#ifndef MOD_OLLAMA_CHAT_EVENTS_H
#define MOD_OLLAMA_CHAT_EVENTS_H

#include "ScriptMgr.h"
#include "Player.h"
#include <string>

class OllamaBotEventChatter
{
public:
    void DispatchGameEvent(Player* source, std::string type, std::string detail);
    void QueueEvent(Player* bot, std::string type, std::string detail, std::string actorName, bool isGuildEvent = false);
    std::string BuildPrompt(Player* bot, std::string promptTemplate, std::string eventType, std::string eventDetail, std::string actorName);
};

class ChatOnKill : public PlayerScript
{
public:
    ChatOnKill();
    void OnPlayerCreatureKill(Player* killer, Creature* victim);
    void OnPlayerPVPKill(Player* killer, Player* killed);
    void OnPlayerCreatureKilledByPet(Player* owner, Creature* victim);
};

class ChatOnLoot : public PlayerScript
{
public:
    ChatOnLoot();
    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 count);
};

class ChatOnDeath : public PlayerScript
{
public:
    ChatOnDeath();
    void OnPlayerJustDied(Player* player);
};

class ChatOnQuest : public PlayerScript
{
public:
    ChatOnQuest();
    void OnPlayerCompleteQuest(Player* player, Quest const* quest);
};

class ChatOnLearn : public PlayerScript
{
public:
    ChatOnLearn();
    void OnPlayerLearnSpell(Player* player, uint32 spellID);
};

class ChatOnDuel : public PlayerScript
{
public:
    ChatOnDuel();
    void OnPlayerDuelRequest(Player* target, Player* challenger);
    void OnPlayerDuelStart(Player* player1, Player* player2);
    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type);
};

// Extra events:
class ChatOnLevelUp : public PlayerScript
{
public:
    ChatOnLevelUp();
    void OnPlayerLevelChanged(Player* player, uint8 oldLevel);
};

class ChatOnAchievement : public PlayerScript
{
public:
    ChatOnAchievement();
    void OnPlayerCompleteAchievement(Player* player, AchievementEntry const* achievement);
};

class ChatOnGameObjectUse : public PlayerScript
{
public:
    ChatOnGameObjectUse();
    void OnGameObjectUse(Player* player, GameObject* go);
};

class ChatOnGuildMemberChange : public PlayerScript
{
public:
    ChatOnGuildMemberChange();
    void OnGuildMemberJoin(Player* player, Guild* guild);
    void OnGuildMemberLeave(Player* player, Guild* guild);
    void OnGuildMemberRankChange(Player* player, Guild* guild, uint8 oldRank, uint8 newRank);
    void OnGuildMemberLogin(Player* player, Guild* guild);
};


#endif // MOD_OLLAMA_CHAT_EVENTS_H
