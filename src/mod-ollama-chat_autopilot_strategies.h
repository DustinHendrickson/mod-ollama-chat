#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_STRATEGIES_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_STRATEGIES_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

class PlayerbotAI;

// --------------------------------------------------------------------------
// The playerbots strategies the LLM may switch on and off.
//
// The LLM drives a bot by turning existing playerbots strategies on and off
// ("+quest", "-grind", "+flee") and by naming the NewRpg statuses it wants the
// bot in ("do quest", "wander npc"). This file holds the allow-list it may
// touch -- with a one-line description of each, so the model knows what it is
// choosing -- and applies the model's desired state to a bot.
//
// The allow-list is the safety boundary, not a menu of plans: class, spec and
// role strategies, `chat`, `default`, `follow` and the like are never on it, so
// the model cannot break a bot's combat rotation or its command handling.
// Operators can widen or narrow it in conf.
//
// World thread only.
// --------------------------------------------------------------------------

// "nc:quest" / "co:flee" -> wanted on (true) or off (false).
using AutopilotDesired = std::map<std::string, bool>;

// What each strategy autopilot has touched looked like before it first did,
// same keys -> present. Recorded on first touch and restored exactly when a
// strategy stops being managed, so a default like `loot` is put back on rather
// than switched off. Clear it whenever playerbots resets the bot.
using AutopilotBaseline = std::map<std::string, bool>;

struct AutopilotStrategyInfo
{
    std::string name;
    bool        combat = false;     // which engine: combat (co) or non-combat (nc)
    std::string description;

    std::string Key() const { return (combat ? "co:" : "nc:") + name; }
};

void AutopilotStrategies_Load();

const std::vector<AutopilotStrategyInfo>& AutopilotStrategies_Allowed();

// Null when the name is not on the allow-list.
const AutopilotStrategyInfo* AutopilotStrategies_Find(const std::string& name);

// NewRpg statuses the model may ask for, with descriptions.
const std::vector<std::pair<std::string, std::string>>& AutopilotStrategies_RpgStatuses();
bool AutopilotStrategies_RpgAllowed(const std::string& status);

// Bring the bot's strategies to `wanted`. Keys that were managed before
// (`applied`) but are no longer wanted go back to their baseline; wanted keys
// are set. Names in `locked` are left alone. Updates `applied` and `baseline`.
void AutopilotStrategies_Apply(PlayerbotAI* ai, const AutopilotDesired& wanted, AutopilotDesired& applied,
                               const std::set<std::string>& locked, AutopilotBaseline& baseline);

// Keys in `wanted` whose live state differs, skipping `locked`.
std::vector<std::string> AutopilotStrategies_Drift(PlayerbotAI* ai, const AutopilotDesired& wanted,
                                                   const std::set<std::string>& locked);

// Put every key in `applied` back to its baseline, then forget them.
void AutopilotStrategies_RestoreAll(PlayerbotAI* ai, AutopilotDesired& applied, const AutopilotBaseline& baseline);

// The bot's allowed strategies as they are now: "on: quest, grind, loot;
// off: explore, travel" -- for the prompt.
std::string AutopilotStrategies_DescribeLive(PlayerbotAI* ai);

// "+nc:quest,-nc:grind,+co:flee" from a desired map, for status and the DB.
std::string AutopilotStrategies_Format(const AutopilotDesired& d);
AutopilotDesired AutopilotStrategies_Parse(const std::string& text);   // inverse of Format

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_STRATEGIES_H
