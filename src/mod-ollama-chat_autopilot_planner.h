#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_PLANNER_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_PLANNER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// --------------------------------------------------------------------------
// Autopilot planner: the LLM is the decision-maker.
//
// The model decides who the character is (its identity, written by the model
// the first time and revised when it sees fit), what it is working toward,
// and which playerbots strategies to switch on and off to get there. The code
// only reports facts and carries out the choice within the allow-list.
//
//   world thread   AutopilotPlanner_BuildPrompt formats the facts -- the
//                  character, its identity, its live strategies, history,
//                  progress, temptations -- and the allow-lists into one
//                  string. AutopilotPlanner_Submit hands it to a worker.
//   worker         QueryOllama (kind Autopilot), then JSON parsing into an
//                  AutopilotDecision. Strings only: no Player, no config.
//   world thread   AutopilotPlanner_Drain returns decisions for the autopilot
//                  to validate against the allow-lists and the live bot.
//
// Cost is bounded by a token bucket (LlmCallsPerHour) shared by every bot and
// a cap on plans in flight. When a bot cannot be planned for, it keeps doing
// what the model last chose.
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
    std::string liveStrategies;         // "on: quest, loot; off: grind, explore"
    std::string rpgStatus;              // live NewRpg status
    std::string rpgFocus;               // what the model asked for last time
    std::string goal;                   // with measured progress; may be empty
    std::string playbook;               // may be empty

    std::vector<std::string> decisions; // newest last
    std::vector<std::string> events;    // newest last
    std::vector<std::string> temptations;
    std::string progress;               // change over recent snapshots
    std::string rewards;                // last hour, plain counts
    std::string guards;                 // self-preservation facts, may be empty
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
    std::string say;
    uint32_t    minutes = 0;

    std::vector<std::pair<std::string, bool>> strategies;   // name -> on, unvalidated

    bool                     rpgGiven = false;               // "rpg" present ([] clears)
    std::vector<std::string> rpg;

    bool playbookGiven = false;
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, bool>>>> playbook;

    // Goal, unvalidated. A plain string goal arrives as kind "free". All
    // empty means "keep the current goal".
    std::string goalKind;
    std::string goalTarget;
    std::string goalText;

    uint64_t    latencyMs = 0;
};

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
