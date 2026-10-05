#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_PLANNER_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_PLANNER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// --------------------------------------------------------------------------
// Autopilot planner: the LLM half of autopilot.
//
//   world thread   AutopilotPlanner_BuildPrompt formats everything -- menus
//                  from the presets, the character, its history -- into one
//                  string. AutopilotPlanner_Submit hands that string to a
//                  dispatcher worker.
//   worker         QueryOllama (kind Autopilot), then JSON parsing into an
//                  AutopilotDecision. Strings only: no Player, no config.
//   world thread   AutopilotPlanner_Drain returns finished decisions for the
//                  autopilot to validate against the live bot and apply.
//
// The worker validates SHAPE only. Whether "explore" is a real activity, or
// "cautious" a real risk option, is decided on the world thread against the
// presets, because the presets are config state a worker must not read.
//
// Cost is bounded by a token bucket (LlmCallsPerHour) shared by every bot and
// a cap on plans in flight, not by how many bots are enrolled.
// --------------------------------------------------------------------------

struct AutopilotPromptContext
{
    std::string botName;
    std::string race;
    std::string cls;
    uint32_t    level = 0;

    std::string playstyle;
    std::string playstyleDescription;
    std::string awareness;
    std::string awarenessDescription;
    std::string personality;            // may be empty

    std::string activity;               // may be empty
    uint32_t    activityMinutes = 0;
    std::string goal;                   // with measured progress; may be empty
    std::string unavailable;            // "dungeon (needs level 15), ..."
    std::string mood;                   // boredom and satisfaction, in words
    std::string playbook;               // "dungeon: cautious, ..." or empty
    std::vector<std::string> temptations;

    std::vector<std::string> decisions; // newest last
    std::vector<std::string> events;    // newest last
    std::string progress;
    std::string guards;                 // self-preservation facts, may be empty
    std::string state;
    std::string memories;
};

struct AutopilotDecision
{
    uint64_t    botGuid = 0;
    bool        ok      = false;
    std::string error;

    std::string activity;
    std::string reason;
    std::string say;
    uint32_t    minutes = 0;
    std::vector<std::pair<std::string, std::string>> disposition;   // axis -> option, unvalidated

    // Goal, unvalidated. A plain string goal arrives as kind "free". All
    // empty means "keep the current goal".
    std::string goalKind;
    std::string goalTarget;
    std::string goalText;

    std::vector<std::pair<std::string, std::string>> playbook;      // situation -> option, unvalidated

    uint64_t    latencyMs = 0;
};

// World thread. `templ` is the configured template; empty uses the default.
std::string AutopilotPlanner_BuildPrompt(const AutopilotPromptContext& ctx, const std::string& templ);

// The shipped template, for the conf documentation and `.ollama autopilot`.
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

// Exposed for tests and the `.ollama autopilot` command: parse a raw model
// reply exactly as the worker does.
AutopilotDecision AutopilotPlanner_Parse(uint64_t botGuid, const std::string& reply);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_PLANNER_H
