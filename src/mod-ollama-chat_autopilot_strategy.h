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

// Walk to a point. generatePath = false for a straight line (onto or off a
// boat's deck, which is not on the navmesh).
void AutopilotMove_To(PlayerbotAI* ai, float x, float y, float z, bool generatePath);

// Keep playerbots' movement off the bot for `ms`: waiting at a dock, riding a
// boat. overCombat also holds combat movement (only sensible on a deck).
void AutopilotMove_Hold(PlayerbotAI* ai, uint32_t ms, bool overCombat);

bool AutopilotMove_IsMoving(PlayerbotAI* ai);
void AutopilotMove_Stop(PlayerbotAI* ai);

// Turn in / pick up quests at this NPC, through playerbots' own action.
bool AutopilotQuest_TalkTo(PlayerbotAI* ai, Creature* npc);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_STRATEGY_H
