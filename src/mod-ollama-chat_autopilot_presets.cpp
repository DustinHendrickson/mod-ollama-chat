#include "mod-ollama-chat_autopilot_presets.h"
#include "mod-ollama-chat_autopilot_strategy.h"
#include "mod-ollama-chat-utilities.h"

#include "Config.h"
#include "Log.h"
#include "Player.h"
#include "SharedDefines.h"

#include "PlayerbotAI.h"

#include <algorithm>

namespace
{
    std::vector<AutopilotPreset>          g_activities;
    std::vector<AutopilotDispositionAxis> g_axes;

    struct DefaultActivity { const char* name; const char* spec; const char* description; };

    // Every activity drops the legacy `rpg` and `move random` wanderers, which
    // would otherwise fight NewRpg for the bot's feet. Explorers, townsfolk and
    // resters also drop `grind` so they stop picking fights on the way.
    const DefaultActivity kDefaultActivities[] = {
        { "quest",   "nc:+new rpg,+quest,-grind,-rpg,-move random;rpg:do quest,wander npc,go camp",
                     "Work through the quest log: travel to objectives, turn in, pick up new quests." },
        { "grind",   "nc:+new rpg,+grind,-rpg,-move random;rpg:go grind,wander random",
                     "Hunt monsters at a good spot for experience and loot." },
        { "explore", "nc:+new rpg,-grind,-rpg,-move random;rpg:wander random,travel flight,go camp",
                     "Wander and travel to see new places; avoid picking fights." },
        { "travel",  "nc:+new rpg,-rpg,-move random;rpg:travel flight",
                     "Take a flight path somewhere else." },
        { "town",    "nc:+new rpg,-grind,-rpg,-move random;rpg:wander npc,go camp,rest",
                     "Spend time around towns and camps: vendors, trainers, quest givers, a rest." },
        { "gather",  "nc:+new rpg,+gather,-rpg,-move random;rpg:wander random,go grind;needs:gathering",
                     "Gather herbs, ore or skins and work on professions." },
        { "dungeon", "nc:+lfg,+new rpg,-grind,-rpg,-move random;rpg:wander npc,go camp,rest;level:15-80",
                     "Queue for a dungeon and wait around town for the group." },
        { "pvp",     "nc:+bg,+new rpg,-rpg,-move random;rpg:outdoor pvp,wander random;level:10-80",
                     "Battlegrounds and fighting the other faction in the open world." },
        { "rest",    "nc:+new rpg,-grind,-rpg,-move random;rpg:rest",
                     "Take a break: sit, eat, drink, catch your breath." },
    };

    struct DefaultOption { const char* axis; const char* option; const char* spec; const char* description; };

    const DefaultOption kDefaultDispositions[] = {
        { "risk",   "cautious", "co:+flee,+potions,+avoid aoe,-aggressive",
                                "Avoids danger: flees when hurt, uses potions early, stays out of bad ground." },
        { "risk",   "balanced", "", "No particular bias." },
        { "risk",   "bold",     "co:+aggressive,-flee",
                                "Takes fights head on and does not back down." },
        { "greed",  "greedy",   "nc:+loot,+gather",
                                "Loots everything and stops for every node." },
        { "greed",  "content",  "", "Takes what comes." },
        { "social", "social",   "nc:+emote,+chat,+start duel",
                                "Outgoing: emotes, chats, challenges people to duels." },
        { "social", "neutral",  "", "No particular bias." },
        { "social", "reserved", "nc:-emote,-start duel",
                                "Keeps to themselves." },
    };

    const char* const kDefaultActivityList = "quest,grind,explore,travel,town,gather,dungeon,pvp,rest";
    const char* const kDefaultAxes         = "risk:balanced,greed:content,social:neutral";

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    void ParseOps(const std::string& list, AutopilotStrategyOps& ops)
    {
        for (const std::string& token : SplitString(list, ','))
        {
            if (token.size() < 2)
                continue;
            if (token[0] == '+')
                ops.add.push_back(Lower(token.substr(1)));
            else if (token[0] == '-')
                ops.remove.push_back(Lower(token.substr(1)));
        }
    }

    bool ParseSpec(const std::string& spec, AutopilotPreset& out, const std::string& settingName)
    {
        bool ok = true;
        for (const std::string& clause : SplitString(spec, ';'))
        {
            const size_t colon = clause.find(':');
            if (colon == std::string::npos)
            {
                LOG_ERROR("module.ollamachat", "[Ollama Chat] {}: clause '{}' has no ':'.", settingName, clause);
                ok = false;
                continue;
            }

            std::string key   = Lower(clause.substr(0, colon));
            std::string value = clause.substr(colon + 1);
            key.erase(key.find_last_not_of(" \t") + 1);

            if (key == "nc")
                ParseOps(value, out.nc);
            else if (key == "co")
                ParseOps(value, out.co);
            else if (key == "rpg")
            {
                for (const std::string& name : SplitString(value, ','))
                {
                    int status = AutopilotRpg_StatusFromName(Lower(name));
                    if (status < 0)
                    {
                        LOG_ERROR("module.ollamachat", "[Ollama Chat] {}: unknown rpg status '{}'.", settingName, name);
                        ok = false;
                    }
                    else
                        out.rpg.push_back(status);
                }
            }
            else if (key == "level")
            {
                std::vector<std::string> band = SplitString(value, '-');
                try
                {
                    if (!band.empty())
                        out.minLevel = static_cast<uint32_t>(std::stoul(band[0]));
                    if (band.size() > 1)
                        out.maxLevel = static_cast<uint32_t>(std::stoul(band[1]));
                }
                catch (...)
                {
                    LOG_ERROR("module.ollamachat", "[Ollama Chat] {}: bad level band '{}'.", settingName, value);
                    ok = false;
                }
            }
            else if (key == "needs")
            {
                std::string need = Lower(value);
                need.erase(0, need.find_first_not_of(" \t"));
                if (need == "gathering")
                    out.needsGathering = true;
                else
                {
                    LOG_ERROR("module.ollamachat", "[Ollama Chat] {}: unknown need '{}'.", settingName, value);
                    ok = false;
                }
            }
            else
            {
                LOG_ERROR("module.ollamachat", "[Ollama Chat] {}: unknown clause '{}'.", settingName, key);
                ok = false;
            }
        }
        return ok;
    }

    // Conf value, else the shipped default, else empty.
    std::string Setting(const std::string& key, const char* fallback)
    {
        return sConfigMgr->GetOption<std::string>(key, fallback ? fallback : "", false);
    }

    const DefaultActivity* FindDefaultActivity(const std::string& name)
    {
        for (const DefaultActivity& d : kDefaultActivities)
            if (name == d.name)
                return &d;
        return nullptr;
    }

    const DefaultOption* FindDefaultOption(const std::string& axis, const std::string& option)
    {
        for (const DefaultOption& d : kDefaultDispositions)
            if (axis == d.axis && option == d.option)
                return &d;
        return nullptr;
    }

    std::vector<std::string> DefaultOptionsFor(const std::string& axis)
    {
        std::vector<std::string> out;
        for (const DefaultOption& d : kDefaultDispositions)
            if (axis == d.axis)
                out.push_back(d.option);
        return out;
    }

    void ApplyOps(PlayerbotAI* ai, const AutopilotStrategyOps& ops, BotState state,
                  const std::set<std::string>& locked, bool& changed)
    {
        std::string command;
        for (const std::string& name : ops.add)
            if (!locked.count(name) && !ai->HasStrategy(name, state))
                command += (command.empty() ? "+" : ",+") + name;
        for (const std::string& name : ops.remove)
            if (!locked.count(name) && ai->HasStrategy(name, state))
                command += (command.empty() ? "-" : ",-") + name;

        if (!command.empty())
        {
            ai->ChangeStrategy(command, state);
            changed = true;
        }
    }

    void DriftOps(PlayerbotAI* ai, const AutopilotStrategyOps& ops, BotState state,
                  const std::set<std::string>& locked, std::vector<std::string>& out)
    {
        for (const std::string& name : ops.add)
            if (!locked.count(name) && !ai->HasStrategy(name, state))
                out.push_back(name);
        for (const std::string& name : ops.remove)
            if (!locked.count(name) && ai->HasStrategy(name, state))
                out.push_back(name);
    }
}

void AutopilotPresets_Load()
{
    std::vector<AutopilotPreset> activities;
    for (const std::string& rawName : SplitString(Setting("OllamaChat.Autopilot.Activities", kDefaultActivityList), ','))
    {
        AutopilotPreset preset;
        preset.name = Lower(rawName);

        const DefaultActivity* def = FindDefaultActivity(preset.name);
        const std::string key = "OllamaChat.Autopilot.Activity." + preset.name;
        const std::string spec = Setting(key, def ? def->spec : nullptr);
        preset.description = Setting(key + ".Description", def ? def->description : nullptr);

        if (spec.empty() && !def)
        {
            LOG_ERROR("module.ollamachat", "[Ollama Chat] Autopilot activity '{}' has no {} setting; skipped.",
                      preset.name, key);
            continue;
        }
        ParseSpec(spec, preset, key);
        if (preset.description.empty())
            preset.description = preset.name;
        activities.push_back(std::move(preset));
    }

    std::vector<AutopilotDispositionAxis> axes;
    for (const std::string& entry : SplitString(Setting("OllamaChat.Autopilot.Dispositions", kDefaultAxes), ','))
    {
        // "risk:balanced" = axis risk, neutral option balanced.
        const size_t colon = entry.find(':');
        AutopilotDispositionAxis axis;
        axis.name    = Lower(colon == std::string::npos ? entry : entry.substr(0, colon));
        axis.neutral = colon == std::string::npos ? std::string() : Lower(entry.substr(colon + 1));

        const std::string axisKey = "OllamaChat.Autopilot.Disposition." + axis.name;
        std::string defaults;
        for (const std::string& o : DefaultOptionsFor(axis.name))
            defaults += (defaults.empty() ? "" : ",") + o;

        for (const std::string& rawOption : SplitString(Setting(axisKey, defaults.c_str()), ','))
        {
            AutopilotPreset option;
            option.name = Lower(rawOption);

            const DefaultOption* def = FindDefaultOption(axis.name, option.name);
            const std::string key = axisKey + "." + option.name;
            ParseSpec(Setting(key, def ? def->spec : nullptr), option, key);
            option.description = Setting(key + ".Description", def ? def->description : nullptr);
            if (option.description.empty())
                option.description = option.name;
            axis.options.push_back(std::move(option));
        }

        if (axis.options.empty())
            continue;
        if (axis.neutral.empty() || std::none_of(axis.options.begin(), axis.options.end(),
                [&](const AutopilotPreset& p) { return p.name == axis.neutral; }))
            axis.neutral = axis.options.front().name;
        axes.push_back(std::move(axis));
    }

    g_activities = std::move(activities);
    g_axes       = std::move(axes);

    LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: {} activities, {} disposition axes.",
             g_activities.size(), g_axes.size());
}

const std::vector<AutopilotPreset>& AutopilotPresets_Activities() { return g_activities; }

const AutopilotPreset* AutopilotPresets_Activity(const std::string& name)
{
    const std::string key = Lower(name);
    for (const AutopilotPreset& p : g_activities)
        if (p.name == key)
            return &p;
    return nullptr;
}

const std::vector<AutopilotDispositionAxis>& AutopilotPresets_Axes() { return g_axes; }

const AutopilotPreset* AutopilotPresets_Disposition(const std::string& axis, const std::string& option)
{
    const std::string a = Lower(axis);
    const std::string o = Lower(option);
    for (const AutopilotDispositionAxis& ax : g_axes)
    {
        if (ax.name != a)
            continue;
        for (const AutopilotPreset& p : ax.options)
            if (p.name == o)
                return &p;
    }
    return nullptr;
}

std::string AutopilotPresets_Unavailable(const AutopilotPreset& preset, Player* bot)
{
    const uint32_t level = bot->GetLevel();
    if (preset.minLevel && level < preset.minLevel)
        return SafeFormat("needs level {}", preset.minLevel);
    if (preset.maxLevel && level > preset.maxLevel)
        return SafeFormat("only up to level {}", preset.maxLevel);
    if (preset.needsGathering &&
        !bot->HasSkill(SKILL_HERBALISM) && !bot->HasSkill(SKILL_MINING) && !bot->HasSkill(SKILL_SKINNING))
        return "no gathering profession";
    return "";
}

namespace
{
    std::string BaselineKey(BotState state, const std::string& name)
    {
        return (state == BOT_STATE_COMBAT ? "co:" : "nc:") + name;
    }

    void RecordBaseline(PlayerbotAI* ai, const AutopilotStrategyOps& ops, BotState state,
                        AutopilotBaseline& baseline)
    {
        for (const auto* list : { &ops.add, &ops.remove })
            for (const std::string& name : *list)
                baseline.emplace(BaselineKey(state, name), ai->HasStrategy(name, state));
    }

    bool Mentions(const AutopilotStrategyOps* ops, const std::string& name)
    {
        return ops && (std::find(ops->add.begin(), ops->add.end(), name) != ops->add.end() ||
                       std::find(ops->remove.begin(), ops->remove.end(), name) != ops->remove.end());
    }

    void RestoreOps(PlayerbotAI* ai, const AutopilotStrategyOps& ops, const AutopilotStrategyOps* nextOps,
                    BotState state, const std::set<std::string>& locked, const AutopilotBaseline& baseline)
    {
        AutopilotStrategyOps restore;
        for (const auto* list : { &ops.add, &ops.remove })
        {
            for (const std::string& name : *list)
            {
                if (Mentions(nextOps, name))
                    continue;
                auto it = baseline.find(BaselineKey(state, name));
                if (it == baseline.end())
                    continue;
                (it->second ? restore.add : restore.remove).push_back(name);
            }
        }
        bool changed = false;
        ApplyOps(ai, restore, state, locked, changed);
    }
}

bool AutopilotPresets_Apply(PlayerbotAI* ai, const AutopilotPreset& preset,
                            const std::set<std::string>& locked, AutopilotBaseline& baseline)
{
    RecordBaseline(ai, preset.nc, BOT_STATE_NON_COMBAT, baseline);
    RecordBaseline(ai, preset.co, BOT_STATE_COMBAT, baseline);

    bool changed = false;
    ApplyOps(ai, preset.nc, BOT_STATE_NON_COMBAT, locked, changed);
    ApplyOps(ai, preset.co, BOT_STATE_COMBAT, locked, changed);
    return changed;
}

void AutopilotPresets_Revert(PlayerbotAI* ai, const AutopilotPreset& preset, const AutopilotPreset* next,
                             const std::set<std::string>& locked, const AutopilotBaseline& baseline)
{
    RestoreOps(ai, preset.nc, next ? &next->nc : nullptr, BOT_STATE_NON_COMBAT, locked, baseline);
    RestoreOps(ai, preset.co, next ? &next->co : nullptr, BOT_STATE_COMBAT, locked, baseline);
}

std::vector<std::string> AutopilotPresets_Drift(PlayerbotAI* ai, const AutopilotPreset& preset,
                                                const std::set<std::string>& locked)
{
    std::vector<std::string> out;
    DriftOps(ai, preset.nc, BOT_STATE_NON_COMBAT, locked, out);
    DriftOps(ai, preset.co, BOT_STATE_COMBAT, locked, out);
    return out;
}
