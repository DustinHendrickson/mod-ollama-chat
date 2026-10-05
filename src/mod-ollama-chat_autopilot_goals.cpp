#include "mod-ollama-chat_autopilot_goals.h"
#include "mod-ollama-chat_progress.h"
#include "mod-ollama-chat-utilities.h"

#include "DBCStores.h"
#include "Player.h"
#include "SharedDefines.h"
#include "World.h"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace
{
    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::string Trim(std::string s)
    {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        return s;
    }

    // First run of digits, or 0. "level 30" -> 30, "150 mining" -> 150.
    uint32_t FirstNumber(const std::string& text)
    {
        size_t start = text.find_first_of("0123456789");
        if (start == std::string::npos)
            return 0;
        size_t end = text.find_first_not_of("0123456789", start);
        try
        {
            return static_cast<uint32_t>(std::stoul(text.substr(start, end - start)));
        }
        catch (...)
        {
            return 0;
        }
    }

    // A top-level zone (not a sub-area) by name, exact match preferred.
    uint32_t FindZone(const std::string& name, std::string& display)
    {
        const std::string want = Lower(Trim(name));
        if (want.empty())
            return 0;

        uint32_t partial = 0;
        for (uint32_t i = 0; i < sAreaTableStore.GetNumRows(); ++i)
        {
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(i);
            if (!area || area->zone != 0 || !area->area_name[0] || !*area->area_name[0])
                continue;

            const std::string have = Lower(area->area_name[0]);
            if (have == want)
            {
                display = area->area_name[0];
                return area->ID;
            }
            if (!partial && (have.find(want) != std::string::npos || want.find(have) != std::string::npos))
            {
                partial = area->ID;
                display = area->area_name[0];
            }
        }
        return partial;
    }

    uint32_t FindProfession(const std::string& text)
    {
        const std::string want = Lower(text);
        for (uint32_t skill : Progress_ProfessionSkills())
            if (want.find(Lower(Progress_SkillName(skill))) != std::string::npos)
                return skill;
        return 0;
    }
}

const char* Goal_KindName(GoalKind kind)
{
    switch (kind)
    {
        case GoalKind::Free:           return "free";
        case GoalKind::ReachLevel:     return "reach_level";
        case GoalKind::ReachSkill:     return "reach_skill";
        case GoalKind::EarnGold:       return "earn_gold";
        case GoalKind::ExploreZone:    return "explore_zone";
        case GoalKind::CompleteQuests: return "complete_quests";
        case GoalKind::RunDungeon:     return "run_dungeon";
        default:                       return "none";
    }
}

GoalKind Goal_KindFromName(const std::string& name)
{
    const std::string k = Lower(Trim(name));
    if (k == "reach_level" || k == "level")                          return GoalKind::ReachLevel;
    if (k == "reach_skill" || k == "skill" || k == "profession")     return GoalKind::ReachSkill;
    if (k == "earn_gold" || k == "gold")                             return GoalKind::EarnGold;
    if (k == "explore_zone" || k == "explore" || k == "visit")       return GoalKind::ExploreZone;
    if (k == "complete_quests" || k == "quests" || k == "quest")     return GoalKind::CompleteQuests;
    if (k == "run_dungeon" || k == "dungeon" || k == "dungeons")     return GoalKind::RunDungeon;
    if (k == "free" || k.empty())                                    return GoalKind::Free;
    return GoalKind::None;
}

const char* Goal_KindMenu()
{
    return "- reach_level: target is the level, e.g. \"30\"\n"
           "- reach_skill: target is a profession and value, e.g. \"mining 150\"\n"
           "- earn_gold: target is how much more gold to have, e.g. \"50\"\n"
           "- explore_zone: target is a zone name, e.g. \"Feralas\"\n"
           "- complete_quests: target is how many, e.g. \"10\"\n"
           "- run_dungeon: target is how many dungeon runs, e.g. \"1\"\n"
           "- free: anything else; target may be empty (it will never be marked done)";
}

std::string Goal_Resolve(Player* bot, const std::string& kindName, const std::string& target,
                         const std::string& text, const GoalCounters& counters, AutopilotGoal& out)
{
    AutopilotGoal g;
    g.kind  = Goal_KindFromName(kindName);
    g.text  = Trim(text);
    g.setAt = static_cast<uint32_t>(time(nullptr));

    if (g.kind == GoalKind::None)
        return "unknown goal kind '" + kindName + "'";

    const uint32_t number = FirstNumber(target);

    switch (g.kind)
    {
        case GoalKind::ReachLevel:
        {
            const uint32_t maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
            if (number <= bot->GetLevel())
                return SafeFormat("level {} is already reached", number);
            if (number > maxLevel)
                return SafeFormat("level {} is above the cap ({})", number, maxLevel);
            g.value  = number;
            g.target = std::to_string(number);
            break;
        }
        case GoalKind::ReachSkill:
        {
            g.targetId = FindProfession(target + " " + text);
            if (!g.targetId)
                return "no profession named in '" + target + "'";
            if (!bot->HasSkill(g.targetId))
                return "does not know " + Progress_SkillName(g.targetId);

            const uint32_t current = bot->GetSkillValue(g.targetId);
            g.value = number ? number : std::max<uint32_t>(current + 1, bot->GetPureMaxSkillValue(g.targetId));
            if (g.value <= current)
                return SafeFormat("{} is already {}", Progress_SkillName(g.targetId), current);
            g.value  = std::min<uint32_t>(g.value, 450);
            g.target = Progress_SkillName(g.targetId);
            break;
        }
        case GoalKind::EarnGold:
        {
            const uint32_t gold = std::clamp<uint32_t>(number ? number : 10, 1, 100000);
            g.value  = bot->GetMoney() + gold * 10000;
            g.target = SafeFormat("{} more gold", gold);
            break;
        }
        case GoalKind::ExploreZone:
        {
            std::string display;
            g.targetId = FindZone(target, display);
            if (!g.targetId)
                return "no zone named '" + target + "'";
            if (g.targetId == bot->GetZoneId())
                return "already in " + display;
            g.target = display;
            break;
        }
        case GoalKind::CompleteQuests:
            g.value    = std::clamp<uint32_t>(number ? number : 5, 1, 100);
            g.baseline = counters.questsTotal;
            g.target   = SafeFormat("{} quests", g.value);
            break;
        case GoalKind::RunDungeon:
            if (bot->GetLevel() < 15)
                return "too low for dungeons";
            g.value    = std::clamp<uint32_t>(number ? number : 1, 1, 20);
            g.baseline = counters.dungeonsTotal;
            g.target   = SafeFormat("{} dungeon run{}", g.value, g.value == 1 ? "" : "s");
            break;
        case GoalKind::Free:
            if (g.text.empty())
                g.text = Trim(target);
            if (g.text.empty())
                return "a free goal needs text";
            g.target = Trim(target);
            break;
        default:
            break;
    }

    if (g.text.empty())
        g.text = g.target;

    out = std::move(g);
    return "";
}

GoalProgress Goal_Progress(Player* bot, const AutopilotGoal& goal, const GoalCounters& counters)
{
    GoalProgress p;
    p.measurable = true;
    p.target     = goal.value;

    switch (goal.kind)
    {
        case GoalKind::ReachLevel:
            p.current = bot->GetLevel();
            p.done    = p.current >= goal.value;
            p.text    = SafeFormat("level {} of {}", p.current, goal.value);
            break;
        case GoalKind::ReachSkill:
            p.current = bot->HasSkill(goal.targetId) ? bot->GetSkillValue(goal.targetId) : 0;
            p.done    = p.current >= goal.value;
            p.text    = SafeFormat("{} {} of {}", goal.target, p.current, goal.value);
            break;
        case GoalKind::EarnGold:
            p.current = bot->GetMoney();
            p.done    = p.current >= goal.value;
            p.text    = SafeFormat("{} of {} gold on hand", p.current / 10000, goal.value / 10000);
            break;
        case GoalKind::ExploreZone:
            p.current = bot->GetZoneId() == goal.targetId ? 1 : 0;
            p.target  = 1;
            p.done    = p.current == 1;
            p.text    = p.done ? "arrived in " + goal.target : "not in " + goal.target + " yet";
            break;
        case GoalKind::CompleteQuests:
            p.current = counters.questsTotal > goal.baseline ? counters.questsTotal - goal.baseline : 0;
            p.done    = p.current >= goal.value;
            p.text    = SafeFormat("{} of {} quests", p.current, goal.value);
            break;
        case GoalKind::RunDungeon:
            p.current = counters.dungeonsTotal > goal.baseline ? counters.dungeonsTotal - goal.baseline : 0;
            p.done    = p.current >= goal.value;
            p.text    = SafeFormat("{} of {} dungeon runs", p.current, goal.value);
            break;
        default:
            p.measurable = false;
            break;
    }
    return p;
}

std::string Goal_Describe(Player* bot, const AutopilotGoal& goal, const GoalCounters& counters)
{
    if (!goal.Active())
        return "";

    GoalProgress p = Goal_Progress(bot, goal, counters);
    if (!p.measurable)
        return goal.text;
    return SafeFormat("{} [{}: {}{}]", goal.text, Goal_KindName(goal.kind), p.text, p.done ? ", done" : "");
}
