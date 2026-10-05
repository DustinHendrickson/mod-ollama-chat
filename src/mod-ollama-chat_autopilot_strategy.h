#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_STRATEGY_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_STRATEGY_H

// --------------------------------------------------------------------------
// The `autopilot` playerbots strategy.
//
// mod-playerbots has no extension point for outside strategies, but it does
// not need one: every bot's AiObjectContext is one of the ten class
// subclasses, each of which keeps its strategy creators in a PUBLIC static
// SharedNamedObjectContextList, and each per-bot list holds a *reference* to
// that shared creator map. Adding a context to all ten after playerbots has
// built them makes the name resolvable for every bot, existing or future.
//
// The strategy itself is a marker. It has no triggers and no actions; it is
// how autopilot enrollment shows up in `nc ?`, and how a master can ask for it
// with `nc +autopilot`. The autopilot table, not the marker, is the source of
// truth, because playerbots resets wipe strategies constantly.
//
// This is the only file that includes playerbots engine headers.
// --------------------------------------------------------------------------

inline constexpr char const* AUTOPILOT_STRATEGY_NAME = "autopilot";

// Register the strategy. Call once on the world thread at startup, after
// mod-playerbots has initialised (OnStartup is), and before bots log in.
// Returns true when the name resolves in all ten class contexts afterwards.
bool AutopilotStrategy_Register();

bool AutopilotStrategy_IsRegistered();

// --------------------------------------------------------------------------
// NewRpg steering. World thread only.
//
// The LLM's `rpg <status>`, `quest <id>` and `goto` orders, and the no-teleport
// handling of stuck walks, act on the bot's NewRpg state through these.
// Steer uses playerbots' own target selection (grind spots, camps, quests
// with POIs, flight paths), so nothing here duplicates it.
//
// Status ids are playerbots' NewRpgStatus values carried as int, so callers
// need no playerbots engine headers.
// --------------------------------------------------------------------------

#include <string>
#include <vector>

class PlayerbotAI;

// -1 for an unknown name. Names are playerbots' own: "do quest", "wander npc".
int         AutopilotRpg_StatusFromName(const std::string& name);
std::string AutopilotRpg_StatusName(int status);

int  AutopilotRpg_CurrentStatus(PlayerbotAI* ai);

// Taking or walking to a flight. Never interrupt one.
bool AutopilotRpg_IsTravelling(PlayerbotAI* ai);

// Walk to a spot using NewRpg's own "go camp" status: playerbots does the
// pathing, and on arrival switches to wandering the NPCs there. Needs the
// `new rpg` strategy on. Returns false if the bot has no AI.
bool AutopilotRpg_GoTo(PlayerbotAI* ai, uint32_t map, float x, float y, float z);

// Work on one specific quest from the bot's log with NewRpg's "do quest"
// status (playerbots finds the objectives and the turn-in). False when the
// quest is not in the log or not in a state to work on.
bool AutopilotRpg_DoQuest(PlayerbotAI* ai, uint32_t questId);

// The quest NewRpg is working on right now, or 0.
uint32_t AutopilotRpg_CurrentQuest(PlayerbotAI* ai);

// NewRpg's MoveFarTo teleports a bot to its destination after `stuckTime`
// (90s) without real progress. True when that is close: the bot has been
// stuck on its current far destination for at least `ms`.
bool AutopilotRpg_IsStuck(PlayerbotAI* ai, uint32_t ms);

// Give up on the current far destination: forget it (so NewRpg's stuck
// counter starts over for whatever comes next) and return to idle.
void AutopilotRpg_Abandon(PlayerbotAI* ai);

// Move the bot into one of `allowed` that is available right now. Returns
// false, changing nothing, when none is -- rather than letting playerbots fall
// back to sitting down.
bool AutopilotRpg_Steer(PlayerbotAI* ai, const std::vector<int>& allowed);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_STRATEGY_H
