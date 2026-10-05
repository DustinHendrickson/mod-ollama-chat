#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_H

#include "ChatCommand.h"
#include "ScriptMgr.h"

#include <cstdint>

class Player;

// --------------------------------------------------------------------------
// LLM autopilot: macro-level control of playerbots. See docs/autopilot-plan.md.
//
// Phase 1 (this file today) is the foundation, with no LLM control yet:
//
//   selection   which bots are autopilot characters, from conf rules plus
//               explicit GM on/off. Rules are evaluated once per login (and on
//               reload), never per tick.
//   marker      the `autopilot` playerbots strategy mirrors enrollment and is
//               re-added after playerbots resets wipe it.
//   diary       staggered progress snapshots and notable events, written in
//               batches (see mod-ollama-chat_progress.h).
//
// THREADING
//   Autopilot_Update and the commands run on the world thread, in
//   WorldScript::OnUpdate, which runs after MapMgr::Update has joined its
//   workers -- so they may touch any Player and PlayerbotAI.
//   The PlayerScript hooks fire inside Player::Update on MAP threads, several
//   at once. They touch only the player they were given and this module's
//   mutex-guarded state.
// --------------------------------------------------------------------------

// Read OllamaChat.Autopilot.* settings. World thread. Called from
// LoadOllamaChatConfig, so it runs at startup and on `.ollama reload`.
void Autopilot_LoadConfig();

// Register the strategy, check the tables, load rows. World thread, at startup.
void Autopilot_Load();

// Throttled sweep plus periodic flush. World thread.
void Autopilot_Update(uint32_t diff);

// Write everything pending. World thread, at shutdown.
void Autopilot_SaveAll();

// True when the feature is on and its prerequisites are met.
bool Autopilot_IsActive();

// Cheap; any thread.
bool Autopilot_IsEnrolled(uint64_t botGuid);

// `.ollama autopilot ...`
Acore::ChatCommands::ChatCommandTable const& Autopilot_CommandTable();

// Login/logout roster plus the progress hooks.
class AutopilotPlayerScript : public PlayerScript
{
public:
    AutopilotPlayerScript();

    void OnPlayerLogin(Player* player) override;
    void OnPlayerLogout(Player* player) override;
    void OnPlayerCreatureKill(Player* killer, Creature* victim) override;
    void OnPlayerPVPKill(Player* killer, Player* killed) override;
    void OnPlayerJustDied(Player* player) override;
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override;
    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override;
    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 count) override;
    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override;
};

// Keeps the set of guilds with a human member current when one joins.
class AutopilotGuildScript : public GuildScript
{
public:
    AutopilotGuildScript();

    void OnAddMember(Guild* guild, Player* player, uint8& plRank) override;
};

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_H
