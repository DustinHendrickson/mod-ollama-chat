#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_STRATEGY_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_STRATEGY_H

#include <cstdint>

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
// The strategy is a marker. It has no triggers and no actions; it is how
// autopilot enrollment shows up in `nc ?`, and how a master can ask for it
// with `nc +autopilot`. With NoHandouts it also carries a multiplier that
// drops playerbots' handout actions for the bot (see the .cpp). The autopilot table, not the marker, is the source of
// truth, because playerbots resets wipe strategies constantly.
//
// This is the only file that includes playerbots engine headers.
// --------------------------------------------------------------------------

class PlayerbotAI;

inline constexpr char const* AUTOPILOT_STRATEGY_NAME = "autopilot";

// Register the strategy. Call once on the world thread at startup, after
// mod-playerbots has initialised (OnStartup is), and before bots log in.
// Returns true when the name resolves in all ten class contexts afterwards.
bool AutopilotStrategy_Register();

bool AutopilotStrategy_IsRegistered();

// OllamaChat.Autopilot.NoHandouts: the marker strategy drops playerbots' free
// level-up maintenance, re-roll, dungeon-finder refresh and free-repair
// release for enrolled bots, and freebie orders are refused. Any thread.
void AutopilotStrategy_SetNoHandouts(bool on);
bool AutopilotStrategy_NoHandouts();

// Playerbots revives a bot through its re-roll (bags emptied) once its
// "death count" reaches 5; keep it below that. World thread.
void AutopilotBot_ClearDeathCount(PlayerbotAI* ai);

// While a bot is on a trip, its grind strategy neither pulls nor wanders (the
// marker strategy's multiplier). World thread sets it; any thread reads.
void AutopilotStrategy_SetTravelling(uint64_t botGuid, bool on);

// --------------------------------------------------------------------------
// Moving a bot for the autopilot. World thread only.
//
// The LLM is the bot's controller, so autopilot does not use playerbots'
// NewRpg for travel: it moves the bot itself, and records each move as the
// bot's last movement (MOVEMENT_NORMAL) so playerbots' own out-of-combat
// movement waits for it. Combat outranks it.
// --------------------------------------------------------------------------

#include <cstdint>

class Creature;
class PlayerbotAI;

// Walk to a point. With generatePath, only along a real navmesh path: false,
// and no movement, when there is none (never a straight line through walls).
// generatePath = false walks straight on purpose (onto or off a deck, which
// is not on the navmesh).
bool AutopilotMove_To(PlayerbotAI* ai, float x, float y, float z, bool generatePath);

// Keep playerbots' movement off the bot for `ms`: waiting at a dock, riding a
// boat. overCombat also holds combat movement (only sensible on a deck).
void AutopilotMove_Hold(PlayerbotAI* ai, uint32_t ms, bool overCombat);

bool AutopilotMove_IsMoving(PlayerbotAI* ai);

// Attacked: stop autopilot's own walk at once (a caster cannot cast while its
// path spline is still running) and release its claim on the bot's movement,
// so playerbots' combat AI has the bot. True when a walk was stopped.
bool AutopilotMove_Yield(PlayerbotAI* ai);
void AutopilotMove_Stop(PlayerbotAI* ai);

// Turn in / pick up quests at this NPC, through playerbots' own action.
bool AutopilotQuest_TalkTo(PlayerbotAI* ai, Creature* npc);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_STRATEGY_H
