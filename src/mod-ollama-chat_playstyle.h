#ifndef MOD_OLLAMA_CHAT_PLAYSTYLE_H
#define MOD_OLLAMA_CHAT_PLAYSTYLE_H

#include <cstdint>
#include <string>
#include <vector>

// --------------------------------------------------------------------------
// Autopilot playstyle and awareness.
//
// PLAYSTYLE is the one main way a bot likes to play: explorer, speedrunner,
// crafter, roleplayer and so on. It decides which activities the bot leans
// toward and which rewards tempt it.
//
// AWARENESS is how the bot thinks about what it is doing:
//   immersed   -- the character does not know it is a game
//   player     -- a person playing casually
//   metagamer  -- knows the systems and optimises them
//
// Both come from weighted lists in conf, so an operator can add a playstyle
// without a code change. Assignment is a deterministic function of the guid,
// so re-evaluating a bot that lost its row gives it the same answer.
//
// World thread only (reads config strings).
// --------------------------------------------------------------------------

void Playstyle_LoadConfig();

// Pick for a newly enrolled bot. `guildId` may be 0. A guild pinned in
// OllamaChat.Autopilot.Select.GuildPlaystyles always gets its pinned style.
std::string Playstyle_Assign(uint64_t botGuid, uint32_t guildId);
std::string Awareness_Assign(uint64_t botGuid);

bool Playstyle_Exists(const std::string& name);
bool Awareness_Exists(const std::string& name);

// Awareness levels currently allowed. Roleplay strictness 2 allows only
// "immersed".
bool Awareness_Allowed(const std::string& name);

std::vector<std::string> Playstyle_List();
std::vector<std::string> Awareness_List();

// What a playstyle means to the planner. Shipped defaults for the nine stock
// playstyles; every field can be overridden or supplied for a new playstyle
// with OllamaChat.Autopilot.Playstyle.<name>.Description / .Activities /
// .SpanMinutes / .Disposition.
struct PlaystyleProfile
{
    std::string name;
    std::string description;
    std::vector<std::pair<std::string, uint32_t>> activityWeights;  // activity -> weight
    uint32_t spanMinMinutes = 20;      // how long an activity holds its interest
    uint32_t spanMaxMinutes = 45;
    std::vector<std::pair<std::string, std::string>> dispositions;  // axis -> option

    // What feels rewarding, as reward channel -> weight. Channels: level,
    // quest, kills, pvp, loot, gold, skill, discovery.
    std::vector<std::pair<std::string, uint32_t>> rewards;

    // Boredom points per minute in one activity when nothing rewarding is
    // happening (see the autopilot mood model).
    float boredomRate = 1.5f;
};

// Null for an unknown name.
const PlaystyleProfile* Playstyle_Profile(const std::string& name);

std::string Awareness_Description(const std::string& name);

// Stable, well-mixed hash of a guid. Used wherever autopilot needs a
// deterministic per-bot choice (assignment, percent selection, staggering).
uint64_t OllamaStableHash(uint64_t value);

#endif // MOD_OLLAMA_CHAT_PLAYSTYLE_H
