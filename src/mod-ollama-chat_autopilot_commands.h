#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_COMMANDS_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_COMMANDS_H

#include "mod-ollama-chat_autopilot_travel.h"

#include <cstdint>
#include <string>
#include <vector>

class Player;
class PlayerbotAI;

// --------------------------------------------------------------------------
// Running the LLM's orders on a bot.
//
// The LLM is the bot's controller. Its orders are playerbots chat commands --
// the same text a player would whisper to their own bot ("nc +grind",
// "co +aoe", "talents", "autogear", "s gray", "follow", "stay", ...) -- fed
// into the bot's own command handling (PlayerbotAI::HandleCommand) with the
// bot itself as the sender, which playerbots' security accepts.
//
// Getting somewhere is the autopilot's own, because playerbots' long-range
// movement is NewRpg, a controller in its own right that the LLM replaces:
//   goto <service>      the nearest repair / vendor / trainer / profession /
//                       inn / flightmaster / bank / auction, used on arrival
//   goto zone <name>    any zone in the world, by foot, flight, boat or
//                       zeppelin as needed (mod-ollama-chat_autopilot_travel.h)
//   quest <id>          to where the quest's objective is, or once complete,
//                       to the NPC who takes it in, and turn it in
//
// An operator deny-list blocks commands that should never come from an LLM
// (logout, reset, destroy, cheats, debug...).
//
// World thread only.
// --------------------------------------------------------------------------

enum class AutopilotErrandKind : uint8_t
{
    Place,            // a zone, or anywhere
    Service,          // use the NPC on arrival
    QuestTurnIn,      // talk to the NPC on arrival
    QuestObjective,   // get there and work the objective
    Craft,            // (walk to a crafting station, then) craft a recipe N times
    Open,             // walk to an object and open or use it
    Mailbox           // walk to a mailbox and collect the mail
};

// A trip with a purpose: where, and what to do on arrival.
struct AutopilotErrand
{
    bool                active    = false;
    AutopilotErrandKind kind      = AutopilotErrandKind::Place;
    uint8_t             service   = 0;      // AutopilotService
    uint32_t            npcEntry  = 0;
    uint32_t            questId   = 0;
    uint8_t             rewardChoice = 0;   // 1-based, from "quest <id> reward <n>"; 0 = not chosen
    std::string         label;              // "the repair vendor Corina Steele"
    uint32_t            startedAt = 0;
    AutopilotTrip       trip;

    // A quest with creatures still to kill does not end on arrival: the bot
    // hunts them -- nearest needed creature first, walking between their spawn
    // points -- until the objective is done or OllamaChat.Autopilot.QuestHuntMinutes.
    bool                hunting   = false;
    uint32_t            huntUntil = 0;
    uint32_t            lastUseAt = 0;      // last time it used an objective object

    // Craft: the recipe spell and how many are left / done. Open: the object.
    uint32_t            craftSpell = 0;
    uint32_t            craftLeft  = 0;
    uint32_t            craftDone  = 0;
    uint32_t            craftItem  = 0;      // what it makes, to count what lands in the bags
    uint32_t            craftHad   = 0;
    uint32_t            craftMisses = 0;
    bool                craftCasting = false;
    uint64_t            objectGuid = 0;
};

void AutopilotCommands_Load();

// The command reference shown to the LLM (static; cache-friendly).
const std::string& AutopilotCommands_Reference();

// Run one command. Returns a short note on what happened, for the diary and
// the next prompt ("sent", "denied", "unknown zone", ...).
std::string AutopilotCommands_Run(Player* bot, PlayerbotAI* ai, const std::string& command,
                                  AutopilotErrand& errand, uint32_t now);

// True for "nc ..." / "co ..." -- strategy changes, which playerbots resets
// wipe and the autopilot re-issues.
bool AutopilotCommands_IsStrategyChange(const std::string& command);

// True for "goto ..." and "quest ..." -- orders that send the bot somewhere.
// One runs at a time; the rest of a plan's errands wait their turn.
bool AutopilotCommands_IsErrand(const std::string& command);

// On the operator's deny-list (OllamaChat.Autopilot.DeniedCommands). An entry
// matches whole leading words: "nc +pvp" denies exactly that change, "nc"
// every out-of-combat strategy change.
bool AutopilotCommands_IsDenied(const std::string& command);

// Trimmed, without a leading "/". Every order goes through this first.
std::string AutopilotCommands_Normalize(const std::string& raw);

struct AutopilotErrandUpdate
{
    std::string note;        // a diary line, or ""
    bool        finished = false;
};

// Advance an errand: travel, then on arrival do the job.
AutopilotErrandUpdate AutopilotCommands_UpdateErrand(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand,
                                                     uint32_t now);

// For the prompt: what the bot can craft from its bags right now ("Copper Bar
// x6 (raises skill) (at a Forge)"), and what is around it worth acting on:
// bodies to loot or skin, gathering nodes and chests, fishing pools, crafting
// stations. World thread.
std::string AutopilotCommands_DescribeCraftable(Player* bot);
std::string AutopilotCommands_DescribeSurroundings(Player* bot);

// End an errand early (unenrolled, joined a group): stop the bot.
void AutopilotCommands_StopErrand(PlayerbotAI* ai, AutopilotErrand& errand);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_COMMANDS_H
