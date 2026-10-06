#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_H

#include "ChatCommand.h"
#include "ScriptMgr.h"

#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// LLM autopilot: the LLM runs selected playerbots. See docs/autopilot-plan.md.
//
// The model is each bot's master. It decides who the bot is (an identity it
// writes itself), what it is working toward, and gives the orders a player
// would whisper to their own bot -- strategies, quests, training, repairs,
// where to go (mod-ollama-chat_autopilot_commands.h). This file carries them
// out and reports facts back:
//
//   selection   which bots are autopilot characters, from conf rules plus
//               explicit GM on/off. Rules are evaluated once per login (and on
//               reload), never per tick.
//   control     runs the model's orders (combat ones only while a human leads
//               the bot or it is in an instance), puts its strategies back
//               after playerbots resets, walks errands to their end, and
//               holds playerbots' random-bot teleports so the bot travels on
//               foot. Hands the bot back exactly as it was when unenrolled.
//   planning    asks the model when it wanted to be asked again, or when
//               something happened (an errand ended, a goal was reached, gear
//               is breaking); within a shared hourly budget. When it cannot be
//               asked, the bot keeps doing what it was last told.
//   marker      the `autopilot` playerbots strategy mirrors enrollment and
//               doubles as the reset detector.
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
// Why Autopilot_IsActive() is false, one line per cause; empty when active.
std::vector<std::string> Autopilot_InactiveReasons();

// The monitor window's buttons (on, off, replan, status <name>), answered as
// system messages to `gm` exactly like the .ollama autopilot commands. The
// caller checks OllamaChat.Monitor.MinSecurity. False for an unknown `sub`.
bool Autopilot_MonitorCommand(Player* gm, const std::string& sub, const std::string& name);

// A whisper between two players, either of them perhaps an autopilot bot:
// kept for the bot's prompt, and a reason to ask the model soon when it is
// about grouping or answers the bot. Any thread; takes the autopilot mutex.
void Autopilot_NoteWhisper(Player* from, Player* to, const std::string& text);

// `.ollama autopilot ...`
Acore::ChatCommands::ChatCommandTable const& Autopilot_CommandTable();

// For the monitor addon (mod-ollama-chat_monitor.cpp). World thread only.
struct AutopilotMonitorRow
{
    uint64_t    guid  = 0;
    std::string name;
    uint8_t     level = 0;
    uint8_t     cls   = 0;
    uint32_t    zone  = 0;
    std::string tier;
    std::string state;    // "fighting", "dead", "travelling", "waiting on the model", "idle", ...
    std::string doing;    // the model's own label
};
std::vector<AutopilotMonitorRow> Autopilot_MonitorList();

// Lines for one page of the monitor ("overview", "travel", "planner",
// "events"). "# Title" lines head a section, "key: value" lines are facts.
// False when the bot has never been on autopilot.
bool Autopilot_MonitorPage(Player* bot, const std::string& page, std::vector<std::string>& lines);

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
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 lowGuid) override;
};

// Keeps the set of guilds with a human member current when one joins.
class AutopilotGuildScript : public GuildScript
{
public:
    AutopilotGuildScript();

    void OnAddMember(Guild* guild, Player* player, uint8& plRank) override;
};

// Dungeon-finder proposals for enrolled bots. Playerbots' own accept refreshes a
// random bot queued alone (bags emptied, free consumables); with NoHandouts it
// is dropped for enrolled bots and autopilot accepts the proposal itself, the
// way the client does.
class AutopilotServerScript : public ServerScript
{
public:
    AutopilotServerScript();

    void OnPacketSent(WorldSession* session, WorldPacket const& packet) override;
};

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_H
