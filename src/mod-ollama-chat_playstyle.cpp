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

    struct DefaultProfile
    {
        const char* name;
        const char* description;
        const char* activities;
        const char* span;
        const char* disposition;
    };

    const DefaultProfile kDefaultProfiles[] = {
        { "explorer",
          "Lives to see new places. Quests and fights are mostly a reason to travel somewhere new.",
          "explore:6,travel:4,quest:2,town:1,rest:1", "25-50", "risk:cautious,greed:content,social:social" },
        { "speedrunner",
          "Levels as efficiently as possible: chained quests, quick turn-ins, dungeons at the right level, no detours.",
          "quest:6,grind:3,dungeon:3,town:1", "30-60", "risk:bold,greed:content,social:reserved" },
        { "quester",
          "Enjoys the stories and finishing quest chains; follows the quest log wherever it leads.",
          "quest:8,town:2,travel:1,rest:1", "30-60", "risk:balanced,greed:content,social:social" },
        { "grinder",
          "Happy hunting monsters for experience and loot for hours on end.",
          "grind:7,quest:2,town:1,rest:1", "40-80", "risk:bold,greed:greedy,social:reserved" },
        { "crafter",
          "Cares about professions and gold: gathering, crafting, selling, and time in towns.",
          "gather:6,town:4,quest:1,rest:1", "30-60", "risk:cautious,greed:greedy,social:social" },
        { "roleplayer",
          "Lives as their character: visits places that matter to their people and calling, rests at inns, acts in character.",
          "town:3,explore:3,quest:3,rest:2,travel:1", "20-45", "risk:balanced,greed:content,social:social" },
        { "dungeonrunner",
          "Wants groups and dungeons; levels in between only to reach the next one.",
          "dungeon:6,quest:3,grind:2,town:1", "30-60", "risk:bold,greed:greedy,social:social" },
        { "casual",
          "Plays relaxed and varied: a bit of everything, and plenty of breaks.",
          "quest:3,explore:2,grind:2,town:2,gather:1,rest:2", "15-35", "risk:cautious,greed:content,social:social" },
        { "pvper",
          "Seeks fights with the other faction: battlegrounds and world PvP.",
          "pvp:6,grind:2,quest:2,town:1", "30-60", "risk:bold,greed:content,social:reserved" },
    };

    struct DefaultAwareness { const char* name; const char* description; };

    const DefaultAwareness kDefaultAwarenessText[] = {
        { "immersed",
          "Does not know this is a game. Thinks entirely as the character: goals are about coin, duty, kin, "
          "faith and survival -- never levels, loot tables or experience." },
        { "player",
          "A person enjoying the game casually. Knows it is a game and cares about fun, friends and steady progress." },
        { "metagamer",
          "Knows the game's systems well and optimises: experience per hour, gear upgrades, efficient routes." },
    };

    std::unordered_map<std::string, PlaystyleProfile> g_profiles;
    std::unordered_map<std::string, std::string>      g_awarenessText;

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

    // "a:b,c:d" -> pairs, lowercased.
    std::vector<std::pair<std::string, std::string>> ParsePairs(const std::string& text)
    {
        std::vector<std::pair<std::string, std::string>> out;
        for (const std::string& entry : SplitString(text, ','))
        {
            size_t colon = entry.find(':');
            if (colon == std::string::npos)
                continue;
            std::string a = Lower(entry.substr(0, colon));
            std::string b = Lower(entry.substr(colon + 1));
            a.erase(a.find_last_not_of(" \t") + 1);
            b.erase(0, b.find_first_not_of(" \t"));
            if (!a.empty() && !b.empty())
                out.emplace_back(a, b);
        }
        return out;
    }

    void LoadProfiles()
    {
        g_profiles.clear();
        for (const WeightedName& style : g_playstyles)
        {
            const DefaultProfile* def = nullptr;
            for (const DefaultProfile& d : kDefaultProfiles)
                if (style.name == d.name)
                    def = &d;

            const std::string key = "OllamaChat.Autopilot.Playstyle." + style.name;
            auto setting = [&](const char* suffix, const char* fallback)
            {
                return sConfigMgr->GetOption<std::string>(key + suffix, fallback ? fallback : "", false);
            };

            PlaystyleProfile p;
            p.name        = style.name;
            p.description = setting(".Description", def ? def->description : nullptr);
            if (p.description.empty())
                p.description = style.name;

            for (const WeightedName& w : ParseWeights(setting(".Activities", def ? def->activities : "quest:1"),
                                                      (key + ".Activities").c_str()))
                p.activityWeights.emplace_back(w.name, w.weight);

            std::vector<std::string> span = SplitString(setting(".SpanMinutes", def ? def->span : "20-45"), '-');
            try
            {
                if (!span.empty())
                    p.spanMinMinutes = static_cast<uint32_t>(std::stoul(span[0]));
                p.spanMaxMinutes = span.size() > 1 ? static_cast<uint32_t>(std::stoul(span[1])) : p.spanMinMinutes;
            }
            catch (...)
            {
                LOG_ERROR("module.ollamachat", "[Ollama Chat] {}.SpanMinutes: expected min-max.", key);
            }
            p.spanMinMinutes = std::max<uint32_t>(1, p.spanMinMinutes);
            p.spanMaxMinutes = std::max(p.spanMinMinutes, p.spanMaxMinutes);

            p.dispositions = ParsePairs(setting(".Disposition", def ? def->disposition : ""));
            g_profiles[style.name] = std::move(p);
        }

        g_awarenessText.clear();
        for (const WeightedName& a : g_awareness)
        {
            const char* fallback = nullptr;
            for (const DefaultAwareness& d : kDefaultAwarenessText)
                if (a.name == d.name)
                    fallback = d.description;

            std::string text = sConfigMgr->GetOption<std::string>(
                "OllamaChat.Autopilot.Awareness." + a.name + ".Description", fallback ? fallback : "", false);
            g_awarenessText[a.name] = text.empty() ? a.name : text;
        }
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

    LoadProfiles();
}

const PlaystyleProfile* Playstyle_Profile(const std::string& name)
{
    auto it = g_profiles.find(Lower(name));
    return it == g_profiles.end() ? nullptr : &it->second;
}

std::string Awareness_Description(const std::string& name)
{
    auto it = g_awarenessText.find(Lower(name));
    if (it != g_awarenessText.end())
        return it->second;
    // Immersed is forced at roleplay strictness 2 even when an operator's
    // weight list leaves it out.
    for (const DefaultAwareness& d : kDefaultAwarenessText)
        if (Lower(name) == d.name)
            return d.description;
    return name;
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
