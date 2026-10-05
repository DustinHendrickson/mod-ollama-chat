#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_PRESETS_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_PRESETS_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

class Player;
class PlayerbotAI;

// --------------------------------------------------------------------------
// Autopilot presets: the only vocabulary the planner (LLM or policy) has.
//
// The model never names a strategy. It picks an ACTIVITY ("quest", "explore")
// and one option per DISPOSITION axis ("risk: cautious"), and each of those is
// a preset defined in conf as a short spec:
//
//     nc:+new rpg,+quest,-grind ; co:+flee ; rpg:do quest,wander npc ;
//     level:15-80 ; needs:gathering
//
//   nc / co   strategy changes for the non-combat / combat engine
//   rpg       NewRpg statuses the bot may be in while this is active
//   level     availability band
//   needs     "gathering" = only for bots with herbalism, mining or skinning
//
// Combat class, spec and role strategies are never in the shipped presets; an
// operator who adds them owns the result.
//
// World thread only (built from config strings, applied to PlayerbotAI).
// --------------------------------------------------------------------------

struct AutopilotStrategyOps
{
    std::vector<std::string> add;
    std::vector<std::string> remove;

    bool Empty() const { return add.empty() && remove.empty(); }
};

struct AutopilotPreset
{
    std::string          name;
    std::string          description;
    AutopilotStrategyOps nc;
    AutopilotStrategyOps co;
    std::vector<int>     rpg;               // allowed NewRpg statuses; empty = leave alone
    uint32_t             minLevel = 0;      // 0 = unbounded
    uint32_t             maxLevel = 0;
    bool                 needsGathering = false;
};

struct AutopilotDispositionAxis
{
    std::string                  name;      // "risk"
    std::string                  neutral;   // option used when nothing else is chosen
    std::vector<AutopilotPreset> options;
};

void AutopilotPresets_Load();

const std::vector<AutopilotPreset>&          AutopilotPresets_Activities();
const AutopilotPreset*                       AutopilotPresets_Activity(const std::string& name);
const std::vector<AutopilotDispositionAxis>& AutopilotPresets_Axes();
const AutopilotPreset* AutopilotPresets_Disposition(const std::string& axis, const std::string& option);

// Why the bot cannot take this activity right now, or "" when it can.
std::string AutopilotPresets_Unavailable(const AutopilotPreset& preset, Player* bot);

// What each strategy autopilot has touched looked like before it first did,
// keyed "nc:name" / "co:name" -> present. Recorded on first touch and used to
// restore exactly that on revert -- so leaving "greedy" puts loot/gather back
// to what playerbots had (usually on), not off, and leaving "reserved" does not
// hand every bot a duel habit it never had. Clear it whenever playerbots
// resets the bot, since the defaults are back.
using AutopilotBaseline = std::map<std::string, bool>;

// Apply a preset's strategy changes, skipping names in `locked` (strategies a
// human changed by hand). Records first-touch state in `baseline`. Returns
// true when anything was changed.
bool AutopilotPresets_Apply(PlayerbotAI* ai, const AutopilotPreset& preset,
                            const std::set<std::string>& locked, AutopilotBaseline& baseline);

// Undo a preset: put every strategy it manages back to its baseline state,
// except names `next` also manages and names in `locked`. Names with no
// baseline were never changed by us and are left alone.
void AutopilotPresets_Revert(PlayerbotAI* ai, const AutopilotPreset& preset, const AutopilotPreset* next,
                             const std::set<std::string>& locked, const AutopilotBaseline& baseline);

// Managed strategy names whose live state differs from what the preset wants.
std::vector<std::string> AutopilotPresets_Drift(PlayerbotAI* ai, const AutopilotPreset& preset,
                                                const std::set<std::string>& locked);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_PRESETS_H
