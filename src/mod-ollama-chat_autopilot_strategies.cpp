#include "mod-ollama-chat_autopilot_strategies.h"
#include "mod-ollama-chat_autopilot_strategy.h"
#include "mod-ollama-chat-utilities.h"

#include "Config.h"
#include "Log.h"

#include "AiObjectContext.h"
#include "PlayerbotAI.h"

#include <algorithm>

namespace
{
    struct Known { const char* name; bool combat; const char* description; };

    // Descriptions for the strategies an operator is likely to allow. Every
    // name here exists in this fork's StrategyContext.
    const Known kKnown[] = {
        // non-combat
        { "new rpg",     false, "live in the world on their own: travel to quests, camps, grind spots and NPCs; "
                                "follows the rpg focus you choose" },
        { "quest",       false, "talk to quest givers, accept, share and turn in quests" },
        { "grind",       false, "attack monsters nearby for experience and loot" },
        { "rpg",         false, "older wander-and-interact behaviour; do not combine with new rpg" },
        { "travel",      false, "pick distant destinations and travel to them" },
        { "explore",     false, "explore the surroundings" },
        { "move random", false, "wander aimlessly" },
        { "gather",      false, "gather herbs, ore and skins on the way" },
        { "loot",        false, "loot corpses and objects" },
        { "lfg",         false, "queue for dungeons through the dungeon finder" },
        { "bg",          false, "queue for battlegrounds" },
        { "pvp",         false, "fight enemy players in the open world" },
        { "duel",        false, "accept duels" },
        { "start duel",  false, "challenge people to duels" },
        { "emote",       false, "use emotes" },
        { "guild",       false, "guild business: tabards, petitions, recruiting" },
        { "group",       false, "invite nearby people into a group, leave groups that wander off" },
        { "attack tagged", false, "join in on monsters others are already fighting" },
        // combat
        { "flee",        true,  "run away when a fight goes badly" },
        { "potions",     true,  "drink healing and mana potions when low" },
        { "avoid aoe",   true,  "move out of area damage" },
        { "aggressive",  true,  "attack anything nearby during a fight" },
        { "threat",      true,  "watch threat and avoid pulling aggro off the tank" },
        { "kite",        true,  "keep distance while fighting" },
    };

    const char* const kDefaultNonCombat =
        "new rpg,quest,grind,rpg,travel,explore,move random,gather,loot,lfg,bg,pvp,duel,start duel,emote,guild,group,"
        "attack tagged";
    const char* const kDefaultCombat = "flee,potions,avoid aoe,aggressive,threat,kite";

    struct KnownRpg { const char* name; const char* description; };
    const KnownRpg kRpg[] = {
        { "do quest",      "travel to and work on quest objectives" },
        { "wander npc",    "visit nearby NPCs: vendors, trainers, quest givers" },
        { "go camp",       "head to a nearby camp or town" },
        { "go grind",      "head to a good hunting spot for their level" },
        { "wander random", "roam the area" },
        { "travel flight", "take a flight path somewhere else" },
        { "rest",          "sit down and take a break" },
        { "outdoor pvp",   "join world PvP objectives in this zone" },
    };
    const char* const kDefaultRpg =
        "do quest,wander npc,go camp,go grind,wander random,travel flight,rest,outdoor pvp";

    std::vector<AutopilotStrategyInfo>               g_allowed;
    std::vector<std::pair<std::string, std::string>> g_rpg;

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    BotState StateOf(const std::string& key)
    {
        return key.rfind("co:", 0) == 0 ? BOT_STATE_COMBAT : BOT_STATE_NON_COMBAT;
    }

    std::string NameOf(const std::string& key)
    {
        return key.size() > 3 ? key.substr(3) : key;
    }

    void Set(PlayerbotAI* ai, const std::string& key, bool on)
    {
        const BotState state = StateOf(key);
        const std::string name = NameOf(key);
        if (ai->HasStrategy(name, state) != on)
            ai->ChangeStrategy((on ? "+" : "-") + name, state);
    }

    void LoadList(const char* settingKey, const char* fallback, bool combat)
    {
        for (const std::string& raw : SplitString(sConfigMgr->GetOption<std::string>(settingKey, fallback), ','))
        {
            AutopilotStrategyInfo info;
            info.name   = Lower(raw);
            info.combat = combat;

            std::string def = info.name;
            for (const Known& k : kKnown)
                if (info.name == k.name)
                    def = k.description;
            info.description = sConfigMgr->GetOption<std::string>(
                "OllamaChat.Autopilot.StrategyDescription." + info.name, def, false);

            if (std::any_of(g_allowed.begin(), g_allowed.end(),
                            [&](const AutopilotStrategyInfo& a) { return a.name == info.name; }))
            {
                LOG_ERROR("module.ollamachat", "[Ollama Chat] {}: '{}' is listed twice; strategy names must be "
                          "unique across the combat and non-combat lists.", settingKey, info.name);
                continue;
            }
            g_allowed.push_back(std::move(info));
        }
    }
}

void AutopilotStrategies_Load()
{
    g_allowed.clear();
    LoadList("OllamaChat.Autopilot.Strategies.NonCombat", kDefaultNonCombat, false);
    LoadList("OllamaChat.Autopilot.Strategies.Combat", kDefaultCombat, true);

    g_rpg.clear();
    for (const std::string& raw : SplitString(
             sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.RpgStatuses", kDefaultRpg), ','))
    {
        const std::string name = Lower(raw);
        if (AutopilotRpg_StatusFromName(name) < 0)
        {
            LOG_ERROR("module.ollamachat", "[Ollama Chat] OllamaChat.Autopilot.RpgStatuses: unknown status '{}'.", name);
            continue;
        }
        std::string description = name;
        for (const KnownRpg& k : kRpg)
            if (name == k.name)
                description = k.description;
        g_rpg.emplace_back(name, description);
    }
}

const std::vector<AutopilotStrategyInfo>& AutopilotStrategies_Allowed() { return g_allowed; }

const AutopilotStrategyInfo* AutopilotStrategies_Find(const std::string& name)
{
    const std::string key = Lower(name);
    for (const AutopilotStrategyInfo& a : g_allowed)
        if (a.name == key)
            return &a;
    return nullptr;
}

const std::vector<std::pair<std::string, std::string>>& AutopilotStrategies_RpgStatuses() { return g_rpg; }

bool AutopilotStrategies_RpgAllowed(const std::string& status)
{
    const std::string key = Lower(status);
    return std::any_of(g_rpg.begin(), g_rpg.end(), [&](const auto& r) { return r.first == key; });
}

namespace
{
    // Strategies playerbots removes when `key` is added (Engine::addStrategy
    // drops siblings: `quest` and `accept all quests` are one such pair).
    std::vector<std::string> SiblingKeys(PlayerbotAI* ai, const std::string& key)
    {
        std::vector<std::string> out;
        const std::string prefix = key.substr(0, 3);
        for (const std::string& name : ai->GetAiObjectContext()->GetSiblingStrategy(NameOf(key)))
            if (name != NameOf(key))
                out.push_back(prefix + name);
        return out;
    }

    // Back to the baseline -- including any sibling our add knocked off.
    void Restore(PlayerbotAI* ai, const std::string& key, const AutopilotBaseline& baseline)
    {
        if (auto base = baseline.find(key); base != baseline.end())
            Set(ai, key, base->second);
        for (const std::string& sibling : SiblingKeys(ai, key))
            if (auto base = baseline.find(sibling); base != baseline.end() && base->second)
                Set(ai, sibling, true);
    }
}

bool AutopilotStrategies_Apply(PlayerbotAI* ai, const AutopilotDesired& wanted, AutopilotDesired& applied,
                               const std::set<std::string>& locked, AutopilotBaseline& baseline)
{
    bool learned = false;

    // No longer managed: back to what the bot had before we touched it.
    for (auto it = applied.begin(); it != applied.end();)
    {
        if (wanted.count(it->first) || locked.count(NameOf(it->first)))
        {
            ++it;
            continue;
        }
        Restore(ai, it->first, baseline);
        it = applied.erase(it);
    }

    for (const auto& [key, on] : wanted)
    {
        if (locked.count(NameOf(key)))
            continue;

        // First touch: remember the strategy and its siblings as they are.
        if (baseline.emplace(key, ai->HasStrategy(NameOf(key), StateOf(key))).second)
        {
            learned = true;
            for (const std::string& sibling : SiblingKeys(ai, key))
                baseline.emplace(sibling, ai->HasStrategy(NameOf(sibling), StateOf(sibling)));
        }
        Set(ai, key, on);
        applied[key] = on;
    }
    return learned;
}

void AutopilotStrategies_RestoreBaseline(PlayerbotAI* ai, const AutopilotBaseline& baseline)
{
    for (const auto& [key, on] : baseline)
        Set(ai, key, on);
}

std::vector<std::string> AutopilotStrategies_Drift(PlayerbotAI* ai, const AutopilotDesired& wanted,
                                                   const std::set<std::string>& locked)
{
    std::vector<std::string> out;
    for (const auto& [key, on] : wanted)
        if (!locked.count(NameOf(key)) && ai->HasStrategy(NameOf(key), StateOf(key)) != on)
            out.push_back(NameOf(key));
    return out;
}

std::string AutopilotStrategies_DescribeLive(PlayerbotAI* ai)
{
    std::string on, off;
    for (const AutopilotStrategyInfo& a : g_allowed)
    {
        const bool has = ai->HasStrategy(a.name, a.combat ? BOT_STATE_COMBAT : BOT_STATE_NON_COMBAT);
        std::string& list = has ? on : off;
        list += (list.empty() ? "" : ", ") + a.name;
    }
    return "on: " + (on.empty() ? std::string("none") : on) + "; off: " + (off.empty() ? std::string("none") : off);
}

std::string AutopilotStrategies_Format(const AutopilotDesired& d)
{
    // Comma-separated: names like "new rpg" contain spaces.
    std::string out;
    for (const auto& [key, on] : d)
        out += (out.empty() ? "" : ",") + std::string(on ? "+" : "-") + key;
    return out;
}

AutopilotDesired AutopilotStrategies_Parse(const std::string& text)
{
    AutopilotDesired out;
    for (const std::string& token : SplitString(text, ','))
        if (token.size() > 4 && (token[0] == '+' || token[0] == '-'))
            out[token.substr(1)] = token[0] == '+';
    return out;
}
