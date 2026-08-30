#ifndef MOD_OLLAMA_CHAT_EVENTS_H
#define MOD_OLLAMA_CHAT_EVENTS_H

#include "ScriptMgr.h"
#include "Player.h"
#include <string>

// --------------------------------------------------------------------------
// Event chatter.
//
// Every hook below now carries `override`. Two of them previously did not, and
// silently overrode nothing:
//
//   OnPlayerCompleteAchievement  -- the core's virtual is
//                                   OnPlayerAchievementComplete (word order).
//   OnGameObjectUse              -- never was a PlayerScript hook at all.
//
// Both compiled fine and simply never ran. With `override` a future core
// rename becomes a build error instead of a feature that quietly stops working.
// --------------------------------------------------------------------------

class OllamaBotEventChatter
{
public:
    void DispatchGameEvent(Player* source, std::string type, std::string detail);

    // World thread only -- reads live world state.
    std::string BuildPrompt(Player* bot, std::string promptTemplate, std::string eventType,
                            std::string eventDetail, std::string actorName);
};

class ChatOnKill : public PlayerScript
{
public:
    ChatOnKill();
    void OnPlayerCreatureKill(Player* killer, Creature* victim) override;
    void OnPlayerPVPKill(Player* killer, Player* killed) override;
    void OnPlayerCreatureKilledByPet(Player* owner, Creature* victim) override;
};

class ChatOnLoot : public PlayerScript
{
public:
    ChatOnLoot();
    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 count) override;
};

class ChatOnDeath : public PlayerScript
{
public:
    ChatOnDeath();
    void OnPlayerJustDied(Player* player) override;
};

class ChatOnQuest : public PlayerScript
{
public:
    ChatOnQuest();
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override;
};

class ChatOnLearn : public PlayerScript
{
public:
    ChatOnLearn();
    void OnPlayerLearnSpell(Player* player, uint32 spellID) override;
};

class ChatOnDuel : public PlayerScript
{
public:
    ChatOnDuel();
    void OnPlayerDuelRequest(Player* target, Player* challenger) override;
    void OnPlayerDuelStart(Player* player1, Player* player2) override;
    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override;
};

class ChatOnLevelUp : public PlayerScript
{
public:
    ChatOnLevelUp();
    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override;
};

// FIXED: was OnPlayerCompleteAchievement, which overrode nothing.
class ChatOnAchievement : public PlayerScript
{
public:
    ChatOnAchievement();
    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override;
};

// FIXED: gameobject interaction is not a PlayerScript hook. AzerothCore has no
// generic "player used any gameobject" hook, so this uses the gossip-hello
// path, which covers interactive objects. Returning false leaves normal
// handling untouched.
class ChatOnGameObjectUse : public AllGameObjectScript
{
public:
    ChatOnGameObjectUse();
    bool CanGameObjectGossipHello(Player* player, GameObject* go) override;
};

// FIXED: OnGuildMember* were never AzerothCore hooks, and the class was never
// even registered. These are the real guild hooks.
class ChatOnGuild : public GuildScript
{
public:
    ChatOnGuild();
    void OnAddMember(Guild* guild, Player* player, uint8& plRank) override;
    void OnRemoveMember(Guild* guild, Player* player, bool isDisbanding, bool isKicked) override;
    void OnEvent(Guild* guild, uint8 eventType, ObjectGuid::LowType playerGuid1,
                 ObjectGuid::LowType playerGuid2, uint8 newRank) override;
};

// Guild login announcements: a login hook plus a guild check, since there is
// no OnGuildMemberLogin in the core.
class ChatOnGuildLogin : public PlayerScript
{
public:
    ChatOnGuildLogin();
    void OnPlayerLogin(Player* player) override;
};

#endif // MOD_OLLAMA_CHAT_EVENTS_H
