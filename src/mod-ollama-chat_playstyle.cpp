#include "mod-ollama-chat_playstyle.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"

#include "Config.h"
#include "Log.h"

#include <algorithm>
#include <unordered_map>

namespace
{
    struct WeightedName
    {
        std::string name;
        uint32_t    weight = 0;
    };

    std::vector<WeightedName>                   g_playstyles;
    std::vector<WeightedName>                   g_awareness;
    std::unordered_map<uint32_t, std::string>   g_guildPlaystyles;

    const char* const kDefaultPlaystyles =
        "explorer:12,speedrunner:10,quester:16,grinder:8,crafter:12,"
        "roleplayer:10,dungeonrunner:10,casual:16,pvper:6";

    const char* const kDefaultAwareness = "immersed:30,player:50,metagamer:20";

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // "name:weight,name:weight". A bare name gets weight 1; weight 0 keeps the
    // name valid for commands but never assigns it.
    std::vector<WeightedName> ParseWeights(const std::string& text, const char* settingName)
    {
        std::vector<WeightedName> out;
        for (const std::string& entry : SplitString(text, ','))
        {
            WeightedName w;
            size_t colon = entry.find(':');
            w.name = Lower(colon == std::string::npos ? entry : entry.substr(0, colon));
            w.weight = 1;
            if (colon != std::string::npos)
            {
                try { w.weight = static_cast<uint32_t>(std::stoul(entry.substr(colon + 1))); }
                catch (...)
                {
                    LOG_ERROR("module.ollamachat", "[Ollama Chat] {}: bad weight in '{}', using 1.",
                              settingName, entry);
                }
            }

            // Trim again: SplitString trims the entry, not the halves.
            w.name.erase(0, w.name.find_first_not_of(" \t"));
            w.name.erase(w.name.find_last_not_of(" \t") + 1);
            if (!w.name.empty())
                out.push_back(std::move(w));
        }
        return out;
    }

    uint64_t Mix(uint64_t x) { return OllamaStableHash(x); }

    std::string PickWeighted(const std::vector<WeightedName>& list, uint64_t seed)
    {
        uint64_t total = 0;
        for (const WeightedName& w : list)
            total += w.weight;
        if (total == 0)
            return list.empty() ? std::string() : list.front().name;

        uint64_t roll = Mix(seed) % total;
        for (const WeightedName& w : list)
        {
            if (roll < w.weight)
                return w.name;
            roll -= w.weight;
        }
        return list.back().name;
    }

    bool Contains(const std::vector<WeightedName>& list, const std::string& name)
    {
        const std::string key = Lower(name);
        return std::any_of(list.begin(), list.end(),
                           [&](const WeightedName& w) { return w.name == key; });
    }

    std::vector<std::string> Names(const std::vector<WeightedName>& list)
    {
        std::vector<std::string> out;
        for (const WeightedName& w : list)
            out.push_back(w.name);
        return out;
    }
}

void Playstyle_LoadConfig()
{
    g_playstyles = ParseWeights(
        sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.Playstyle.Weights", kDefaultPlaystyles),
        "OllamaChat.Autopilot.Playstyle.Weights");
    if (g_playstyles.empty())
        g_playstyles = ParseWeights(kDefaultPlaystyles, "default playstyles");

    g_awareness = ParseWeights(
        sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.Awareness.Weights", kDefaultAwareness),
        "OllamaChat.Autopilot.Awareness.Weights");
    if (g_awareness.empty())
        g_awareness = ParseWeights(kDefaultAwareness, "default awareness");

    g_guildPlaystyles.clear();
    const std::string pins =
        sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.Select.GuildPlaystyles", "");
    for (const std::string& entry : SplitString(pins, ','))
    {
        size_t colon = entry.find(':');
        if (colon == std::string::npos)
            continue;

        uint32_t guildId = 0;
        try { guildId = static_cast<uint32_t>(std::stoul(entry.substr(0, colon))); }
        catch (...) { guildId = 0; }

        std::string style = Lower(entry.substr(colon + 1));
        style.erase(0, style.find_first_not_of(" \t"));

        if (guildId == 0 || !Contains(g_playstyles, style))
        {
            LOG_ERROR("module.ollamachat",
                      "[Ollama Chat] OllamaChat.Autopilot.Select.GuildPlaystyles: ignoring '{}' "
                      "(needs guildId:playstyle with a playstyle from Playstyle.Weights).", entry);
            continue;
        }
        g_guildPlaystyles[guildId] = style;
    }
}

std::string Playstyle_Assign(uint64_t botGuid, uint32_t guildId)
{
    if (guildId != 0)
    {
        auto it = g_guildPlaystyles.find(guildId);
        if (it != g_guildPlaystyles.end())
            return it->second;
    }
    return PickWeighted(g_playstyles, botGuid);
}

std::string Awareness_Assign(uint64_t botGuid)
{
    if (g_RoleplayEnable && g_RoleplayStrictness >= 2)
        return "immersed";

    // A different seed from the playstyle pick, or the two would correlate.
    return PickWeighted(g_awareness, botGuid ^ 0xA5A5A5A5DEADBEEFull);
}

bool Playstyle_Exists(const std::string& name) { return Contains(g_playstyles, name); }
bool Awareness_Exists(const std::string& name) { return Contains(g_awareness, name); }

bool Awareness_Allowed(const std::string& name)
{
    if (g_RoleplayEnable && g_RoleplayStrictness >= 2)
        return Lower(name) == "immersed";
    return Awareness_Exists(name);
}

// splitmix64: cheap, stateless, and well mixed for sequential guids.
uint64_t OllamaStableHash(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

std::vector<std::string> Playstyle_List() { return Names(g_playstyles); }
std::vector<std::string> Awareness_List() { return Names(g_awareness); }
