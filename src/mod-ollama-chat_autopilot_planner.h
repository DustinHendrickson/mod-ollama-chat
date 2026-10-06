#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_PLANNER_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_PLANNER_H

#include <cstdint>
#include <string>
#include <vector>

// --------------------------------------------------------------------------
// Autopilot planner: the LLM is the bot's master.
//
// The model decides who the character is, what it is working toward, and
// what to do about it now -- and says so as a list of commands, the same ones
// a player whispers to their own bot ("nc +grind", "talents", "goto trainer",
// "quest 783"). The code reports facts and runs the commands.
//
//   world thread   AutopilotPlanner_BuildPrompt formats the facts -- the
//                  character, its identity, its live strategies, quest log,
//                  services nearby, history, what its last commands did --
//                  and the command reference into one string.
//                  AutopilotPlanner_Submit hands it to a worker.
//   worker         QueryOllama (kind Autopilot), then JSON parsing into an
//                  AutopilotDecision. Strings only: no Player, no config.
//   world thread   AutopilotPlanner_Drain returns decisions for the autopilot
//                  to run.
//
// Cost is bounded by a token bucket (LlmCallsPerHour) shared by every bot and
// a cap on plans in flight. When a bot cannot be planned for, it keeps doing
// what the model last told it to.
// --------------------------------------------------------------------------

struct AutopilotPromptContext
{
    std::string botName;
    std::string race;
    std::string cls;
    uint32_t    level = 0;
    std::string guild;                  // may be empty
    std::string personality;            // chat personality, may be empty
    bool        mustBeInCharacter = false;   // roleplay strictness 2

    // Identity the model wrote earlier. All empty = create one now.
    std::string style;
    std::string outlook;
    std::string profile;

    std::string doing;                  // the model's own label for the current plan
    uint32_t    doingMinutes = 0;
    std::string goal;                   // with measured progress; may be empty

    std::string commandReference;       // static: what commands exist
    std::string strategiesOn;           // "nc: ...; co: ..." live
    std::string activity;               // what the bot is doing on its orders right now
    std::string errand;                 // walk in progress, may be empty
    std::string questLog;               // "- [id] title (level, done/not)" lines
    std::string services;               // nearest services with distance
    std::string zones;                  // zones on this continent for their level

    std::vector<std::string> lastResults;  // "command -> what happened"
    std::vector<std::string> decisions;    // newest last
    std::vector<std::string> events;       // newest last
    std::vector<std::string> temptations;
    std::string progress;               // change over recent snapshots
    std::string rewards;                // last hour, plain counts
    std::string concerns;               // health, gear, bags, money facts
    std::string state;
    std::string memories;
};

struct AutopilotDecision
{
    uint64_t    botGuid = 0;
    bool        ok      = false;
    std::string error;

    // Identity: any non-empty field replaces the stored one.
    std::string style;
    std::string outlook;
    std::string profile;

    std::string doing;
    std::string reason;
    uint32_t    minutes = 0;

    std::vector<std::string> commands;  // in order, unvalidated

    // Goal, unvalidated. A plain string goal arrives as kind "free". All
    // empty means "keep the current goal".
    std::string goalKind;
    std::string goalTarget;
    std::string goalText;

    uint64_t    latencyMs = 0;
};

inline constexpr size_t AUTOPILOT_MAX_COMMANDS      = 8;
inline constexpr size_t AUTOPILOT_MAX_COMMAND_CHARS = 120;

// World thread. `templ` is the configured template; empty uses the default.
std::string AutopilotPlanner_BuildPrompt(const AutopilotPromptContext& ctx, const std::string& templ);

const char* AutopilotPlanner_DefaultTemplate();

// --- budget ---------------------------------------------------------------

void AutopilotPlanner_ConfigureBudget(uint32_t callsPerHour, uint32_t maxConcurrent);

// True when a plan may be submitted now. Foreground plans may spend the whole
// bucket; background plans leave a quarter of it for foreground ones.
bool AutopilotPlanner_CanSubmit(bool foreground);

// Spends a token and queues the job. False when the dispatcher refused it
// (queue past half depth); the token is refunded in that case.
bool AutopilotPlanner_Submit(uint64_t botGuid, std::string prompt);

std::vector<AutopilotDecision> AutopilotPlanner_Drain();

struct AutopilotPlannerStats
{
    uint32_t inFlight      = 0;
    float    tokens        = 0.0f;
    float    capacity      = 0.0f;
    uint64_t submitted     = 0;
    uint64_t refused       = 0;
    uint64_t parsed        = 0;
    uint64_t failed        = 0;
    std::string lastError;
};
AutopilotPlannerStats AutopilotPlanner_GetStats();

// Parse a raw model reply exactly as the worker does.
AutopilotDecision AutopilotPlanner_Parse(uint64_t botGuid, const std::string& reply);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_PLANNER_H
