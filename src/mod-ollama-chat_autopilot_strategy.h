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
// An autopilot activity is, in part, "the set of NewRpg statuses this bot may
// be in" (questing allows do quest / wander npc / go camp, and so on). When
// the bot drifts outside the set, Steer moves it back in by calling
// playerbots' own NewRpgBaseAction::RandomChangeStatus restricted to the set.
// That uses playerbots' weights and its own target selection (grind spots,
// camps, quests with POIs, flight paths), so nothing here duplicates it.
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

// Move the bot into one of `allowed` that is available right now. Returns
// false, changing nothing, when none is -- rather than letting playerbots fall
// back to sitting down.
bool AutopilotRpg_Steer(PlayerbotAI* ai, const std::vector<int>& allowed);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_STRATEGY_H
