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

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_STRATEGY_H
