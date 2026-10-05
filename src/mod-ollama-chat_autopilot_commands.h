#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_COMMANDS_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_COMMANDS_H

#include "mod-ollama-chat_autopilot_route.h"

#include <cstdint>
#include <string>
#include <vector>

class Player;
class PlayerbotAI;

// --------------------------------------------------------------------------
// Running the LLM's commands on a bot.
//
// The LLM acts as the bot's master. Its commands are playerbots chat commands
// -- the same text a player would whisper to their own bot ("nc +grind",
// "co +aoe", "talents", "autogear", "s gray", "repair", "go travel <place>",
// "follow", "stay", ...) -- fed into the bot's own command handling
// (PlayerbotAI::HandleCommand) with the bot itself as the sender, which
// playerbots' security accepts. They run exactly as a master's would.
//
// A few autopilot commands fill gaps where playerbots needs a human master or
// has no command at all:
//   goto <service>      walk to the nearest repair / vendor / trainer /
//                       profession / inn / flightmaster / bank / auction,
//                       then use it on arrival (repair and sell junk, learn
//                       from the trainer, bind the hearthstone at the inn)
//   goto zone <name>    walk to a zone on this continent
//   quest <id>          work on that quest from the log
//   rpg <status>        NewRpg focus: do quest, wander npc, go grind, ...
//
// An operator deny-list blocks commands that should never come from an LLM
// (logout, reset, destroy, cheats, debug...).
//
// World thread only.
// --------------------------------------------------------------------------

// A walk with a purpose: where, and what to do on arrival.
struct AutopilotErrand
{
    bool        active    = false;
    uint32_t    map       = 0;
    float       x = 0.0f, y = 0.0f, z = 0.0f;
    uint32_t    npcEntry  = 0;      // 0 for a plain zone walk
    uint8_t     service   = 0;      // AutopilotService
    std::string label;              // "the repair vendor Corina Steele"
    uint32_t    startedAt = 0;

    // Walks longer than playerbots can manage alone follow a navmesh route,
    // one node at a time (mod-ollama-chat_autopilot_route.h).
    bool           routed = false;
    AutopilotRoute route;
    size_t         issued = SIZE_MAX;   // node last handed to the bot
};

void AutopilotCommands_Load();

// The command reference shown to the LLM (static; cache-friendly).
const std::string& AutopilotCommands_Reference();

// Run one command. Returns a short note on what happened, for the diary and
// the next prompt ("ran", "denied", "unknown zone", ...).
std::string AutopilotCommands_Run(Player* bot, PlayerbotAI* ai, const std::string& command,
                                  AutopilotErrand& errand, uint32_t now);

// True for "nc ..." / "co ..." -- strategy changes, which playerbots resets
// wipe and the autopilot re-issues.
bool AutopilotCommands_IsStrategyChange(const std::string& command);

// On the operator's deny-list (OllamaChat.Autopilot.DeniedCommands). An entry
// matches whole leading words: "nc +pvp" denies exactly that change, "nc"
// every out-of-combat strategy change.
bool AutopilotCommands_IsDenied(const std::string& command);

// Advance an errand: on arrival, use the service. Returns a diary line when
// the errand finished or failed this visit, else "".
std::string AutopilotCommands_UpdateErrand(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand, uint32_t now);

// The bot got stuck on the way: build the route again from where it stands.
// False when there is no route to rebuild, or it has been rebuilt enough.
bool AutopilotCommands_Reroute(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_COMMANDS_H
