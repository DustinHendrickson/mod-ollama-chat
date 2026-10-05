#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_GOALS_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_GOALS_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// Autopilot goals: what a bot is working toward, in a form the game can
// measure.
//
// The model proposes a goal as {kind, target, text}. The kind and target are
// resolved here against game data on the world thread -- "Feralas" becomes a
// zone id, "mining 150" a skill id and a value -- so progress is measured
// from the bot itself rather than taken on the model's word. `text` is the
// goal in the character's own voice, which is what prompts and chat use.
//
// A goal the game cannot measure is still allowed, as kind "free": it is
// shown to the model and in chat but never completes on its own.
// --------------------------------------------------------------------------

enum class GoalKind : uint8_t
{
    None = 0,
    Free,
    ReachLevel,      // value = level
    ReachSkill,      // targetId = skill id, value = skill value
    EarnGold,        // value = copper on hand
    ExploreZone,     // targetId = zone id
    CompleteQuests,  // value = completions after baseline
    RunDungeon,      // value = dungeon entries after baseline
};

struct AutopilotGoal
{
    GoalKind    kind     = GoalKind::None;
    std::string text;               // in the character's voice
    std::string target;             // display form: "Feralas", "Mining"
    uint32_t    targetId = 0;
    uint32_t    value    = 0;
    uint32_t    baseline = 0;       // counter value when the goal was set
    uint32_t    setAt    = 0;

    bool Active() const { return kind != GoalKind::None; }
};

// Counters a goal may be measured against, besides the Player itself.
struct GoalCounters
{
    uint32_t questsTotal   = 0;
    uint32_t dungeonsTotal = 0;
};

struct GoalProgress
{
    uint32_t    current = 0;
    uint32_t    target  = 0;
    bool        done    = false;
    bool        measurable = false;
    std::string text;               // "level 27 of 30"
};

const char* Goal_KindName(GoalKind kind);
GoalKind    Goal_KindFromName(const std::string& name);

// The goal kinds and what their target means, for the prompt.
const char* Goal_KindMenu();

// Resolve a proposed goal. Returns "" and fills `out` on success, or why the
// goal was rejected. World thread (reads the Player and DBC stores).
std::string Goal_Resolve(Player* bot, const std::string& kind, const std::string& target,
                         const std::string& text, const GoalCounters& counters, AutopilotGoal& out);

// World thread.
GoalProgress Goal_Progress(Player* bot, const AutopilotGoal& goal, const GoalCounters& counters);

// Activities that serve this goal right now, with a multiplier for the
// policy's weights. Empty for free goals.
std::vector<std::pair<std::string, uint32_t>> Goal_ActivityBias(Player* bot, const AutopilotGoal& goal);

// One line for prompts and `.ollama autopilot status`.
std::string Goal_Describe(Player* bot, const AutopilotGoal& goal, const GoalCounters& counters);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_GOALS_H
