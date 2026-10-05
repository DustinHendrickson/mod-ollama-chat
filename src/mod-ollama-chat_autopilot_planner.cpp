#include "mod-ollama-chat_autopilot_planner.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_autopilot_goals.h"
#include "mod-ollama-chat_autopilot_strategies.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat-utilities.h"

#include "Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

namespace
{
    using Clock = std::chrono::steady_clock;

    // Static instructions and allow-lists first, the character last. Ollama
    // reuses its KV cache for an identical prefix and hosted providers cache
    // on the prefix too, so everything above THE CHARACTER is close to free on
    // every call after the first.
    //
    // Placeholders are replaced literally (not with fmt), so the JSON braces
    // in the template need no escaping -- including in an operator's own.
    const char* const kDefaultTemplate =
        "You are the mind behind a World of Warcraft character that plays on its own. You decide who this "
        "character is, what they want, and what they do with their time -- the way a real player decides "
        "their evening, or the way the character themselves would live, depending on their outlook. You do "
        "not steer, target or cast: you switch the character's behaviours (strategies) on and off, choose "
        "what they focus on, and set their goals. Their own AI does the walking and fighting.\n"
        "\n"
        "STRATEGIES you may switch on (+name) or off (-name):\n"
        "{strategies}\n"
        "\n"
        "RPG FOCUS -- what the character concentrates on while new rpg is on (pick one or more):\n"
        "{rpg_statuses}\n"
        "\n"
        "GOAL KINDS (a longer-term aim the game can measure):\n"
        "{goal_kinds}\n"
        "\n"
        "PLAYBOOK: combat strategies to switch only while in a situation ({situations}), on top of the "
        "rest -- e.g. careful in dungeons, reckless in battlegrounds.\n"
        "\n"
        "Reply with one JSON object and nothing else, shaped like this:\n"
        "{\"identity\": {\"style\": \"...\", \"outlook\": \"...\", \"profile\": \"...\"}, "
        "\"doing\": \"...\", \"strategies\": [\"+name\", \"-name\"], \"rpg\": [\"...\"], \"minutes\": 45, "
        "\"goal\": {\"kind\": \"...\", \"target\": \"...\", \"text\": \"...\"}, "
        "\"playbook\": {\"dungeon\": [\"+name\"]}, \"reason\": \"...\"}\n"
        "- identity: who this character is. style = their way of playing in a few words; outlook = "
        "in-character (they do not know it is a game), player (plays for fun, knows it is a game) or "
        "metagamer (knows the systems and optimises); profile = two or three sentences: what they love, "
        "what bores them, what tempts them, how they see the world. {identity_rule}\n"
        "- doing: a few words for what they are doing now, in their own terms.\n"
        "- strategies: only the changes to make now; anything you do not mention stays as it is. Make sure "
        "the strategies that are on actually fit what they are doing (e.g. new rpg + quest for questing, "
        "grind for hunting, gather for gathering, lfg to queue for a dungeon).\n"
        "- rpg: the focus while new rpg is on; [] lets them roam as they please; omit to keep the current one.\n"
        "- minutes: how long before you reconsider (10 to 180).\n"
        "- goal: keep the current goal unless it is done, stalled or no longer fits them; omit to keep it. "
        "text is the aim in one sentence, phrased the way this character thinks.\n"
        "- playbook: optional; omit to keep it.\n"
        "- reason: one short sentence on why this, now -- the way they would explain it.\n"
        "Decide from who they are and what has actually happened to them. People get bored, get tempted, "
        "change their minds and take breaks.\n"
        "\n"
        "THE CHARACTER\n"
        "{bot_name}, level {bot_level} {bot_race} {bot_class}{guild}.\n"
        "{personality}"
        "Identity: {identity}\n"
        "Doing: {doing} (for {doing_minutes} minutes). Current goal: {goal}\n"
        "Strategies now: {live_strategies}\n"
        "RPG focus: {rpg_focus} (right now: {rpg_status})\n"
        "Playbook: {playbook}\n"
        "Last hour: {rewards}\n"
        "Recent decisions:\n{decisions}\n"
        "{progress}\n"
        "Recent events:\n{events}\n"
        "Tempting right now:\n{temptations}\n"
        "{guards}"
        "{state}\n"
        "{memories}";

    // --- budget -----------------------------------------------------------

    std::mutex        g_budgetMutex;
    float             g_tokens        = 0.0f;
    float             g_capacity      = 1.0f;
    float             g_perSecond     = 0.0f;
    uint32_t          g_maxConcurrent = 2;
    Clock::time_point g_lastRefill    = Clock::now();
    bool              g_budgetInit    = false;

    std::atomic<uint32_t> g_inFlight{ 0 };
    std::atomic<uint64_t> g_submitted{ 0 };
    std::atomic<uint64_t> g_refused{ 0 };
    std::atomic<uint64_t> g_parsed{ 0 };
    std::atomic<uint64_t> g_failed{ 0 };

    std::mutex                     g_doneMutex;
    std::vector<AutopilotDecision> g_done;
    std::string                    g_lastError;

    void RefillLocked()
    {
        const Clock::time_point now = Clock::now();
        const float seconds = std::chrono::duration<float>(now - g_lastRefill).count();
        g_lastRefill = now;
        g_tokens = std::min(g_capacity, g_tokens + seconds * g_perSecond);
    }

    // --- text helpers -----------------------------------------------------

    void ReplaceAll(std::string& text, const std::string& key, const std::string& value)
    {
        const std::string token = "{" + key + "}";
        for (size_t pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + value.size()))
            text.replace(pos, token.size(), value);
    }

    // Free text that came from the model or from players (goals, reasons,
    // identities, memories) goes back into later prompts. Braces in it must
    // not be taken for placeholders by the replacements that run after it.
    std::string NoBraces(std::string s)
    {
        std::replace(s.begin(), s.end(), '{', '(');
        std::replace(s.begin(), s.end(), '}', ')');
        return s;
    }

    std::string Lines(const std::vector<std::string>& items, const char* none)
    {
        if (items.empty())
            return std::string("- ") + none;
        std::string out;
        for (const std::string& item : items)
            out += (out.empty() ? "- " : "\n- ") + NoBraces(item);
        return out;
    }

    std::string Trim(std::string s)
    {
        s.erase(0, s.find_first_not_of(" \t\r\n\"'"));
        s.erase(s.find_last_not_of(" \t\r\n\"'") + 1);
        return s;
    }

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // The first balanced {...}, honouring strings and escapes -- unlike plain
    // brace counting, a "}" inside a reason does not end the object early.
    std::string ExtractObject(const std::string& text)
    {
        const size_t start = text.find('{');
        if (start == std::string::npos)
            return "";

        int  depth    = 0;
        bool inString = false;
        bool escaped  = false;
        for (size_t i = start; i < text.size(); ++i)
        {
            const char c = text[i];
            if (inString)
            {
                if (escaped)        escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"')  inString = false;
                continue;
            }
            if (c == '"')
                inString = true;
            else if (c == '{')
                ++depth;
            else if (c == '}' && --depth == 0)
                return text.substr(start, i - start + 1);
        }
        return "";
    }

    std::string Clip(std::string s, size_t max)
    {
        s = Trim(SanitizeUTF8(s));
        if (s.size() > max)
            s = Utf8Truncate(std::move(s), max);
        return s;
    }

    // "+quest" / "-grind" / "quest" (= on). Returns false for anything else.
    bool ParseChange(const std::string& raw, std::pair<std::string, bool>& out)
    {
        std::string s = Lower(Clip(raw, 40));
        bool on = true;
        if (!s.empty() && (s[0] == '+' || s[0] == '-'))
        {
            on = s[0] == '+';
            s = Trim(s.substr(1));
        }
        if (s.empty())
            return false;
        out = { s, on };
        return true;
    }

    std::vector<std::pair<std::string, bool>> ParseChanges(const nlohmann::json& value)
    {
        std::vector<std::pair<std::string, bool>> out;
        std::pair<std::string, bool> change;

        if (value.is_array())
        {
            for (const auto& item : value)
                if (item.is_string() && ParseChange(item.get<std::string>(), change))
                    out.push_back(change);
        }
        else if (value.is_string())
        {
            for (const std::string& item : SplitString(value.get<std::string>(), ','))
                if (ParseChange(item, change))
                    out.push_back(change);
        }
        else if (value.is_object())
        {
            // {"on": [...], "off": [...]} -- some models prefer it.
            for (const char* key : { "on", "off" })
                if (auto it = value.find(key); it != value.end() && it->is_array())
                    for (const auto& item : *it)
                        if (item.is_string() && ParseChange(item.get<std::string>(), change))
                            out.emplace_back(change.first, std::string_view(key) == "on");
        }
        return out;
    }

    void RunPlan(uint64_t botGuid, const std::string& prompt)
    {
        OllamaApiResult api = QueryOllama(prompt, OllamaRequestKind::Autopilot);

        AutopilotDecision decision;
        if (!api.ok)
        {
            decision.botGuid = botGuid;
            decision.error   = api.error.empty() ? "request failed" : api.error;
        }
        else
        {
            decision = AutopilotPlanner_Parse(botGuid, api.text);
        }
        decision.latencyMs = api.latencyMs;

        if (decision.ok)
            ++g_parsed;
        else
            ++g_failed;

        std::lock_guard<std::mutex> lock(g_doneMutex);
        if (!decision.ok)
            g_lastError = decision.error;
        g_done.push_back(std::move(decision));
    }
}

const char* AutopilotPlanner_DefaultTemplate()
{
    return kDefaultTemplate;
}

std::string AutopilotPlanner_BuildPrompt(const AutopilotPromptContext& ctx, const std::string& templ)
{
    std::string strategies;
    for (const AutopilotStrategyInfo& s : AutopilotStrategies_Allowed())
        strategies += SafeFormat("{}- {} ({}): {}", strategies.empty() ? "" : "\n", s.name,
                                 s.combat ? "combat" : "out of combat", s.description);

    std::string rpg;
    for (const auto& [name, description] : AutopilotStrategies_RpgStatuses())
        rpg += SafeFormat("{}- {}: {}", rpg.empty() ? "" : "\n", name, description);

    std::string text = templ.empty() ? std::string(kDefaultTemplate) : templ;

    // Conf values carry "\n" literally; the default carries real newlines.
    for (size_t pos = text.find("\\n"); pos != std::string::npos; pos = text.find("\\n", pos + 1))
        text.replace(pos, 2, "\n");

    const bool hasIdentity = !ctx.profile.empty() || !ctx.style.empty();
    std::string identityRule = hasIdentity
        ? "They already have one (below): include identity only if who they are has genuinely changed."
        : "They have NO identity yet: you must write one now, from everything you know about them below.";
    if (ctx.mustBeInCharacter)
        identityRule += " This realm is strictly in character: outlook must be in-character, and goals, "
                        "reasons and 'doing' must be in the character's own in-world terms.";

    std::string identity = "none yet";
    if (hasIdentity)
        identity = SafeFormat("{} ({}). {}", ctx.style.empty() ? "?" : ctx.style,
                              ctx.outlook.empty() ? "?" : ctx.outlook, ctx.profile);

    ReplaceAll(text, "strategies", strategies.empty() ? "- (none allowed)" : strategies);
    ReplaceAll(text, "rpg_statuses", rpg.empty() ? "- (none allowed)" : rpg);
    ReplaceAll(text, "goal_kinds", Goal_KindMenu());
    ReplaceAll(text, "situations", "dungeon, battleground, with_player");
    ReplaceAll(text, "identity_rule", identityRule);
    ReplaceAll(text, "bot_name", ctx.botName);
    ReplaceAll(text, "bot_level", std::to_string(ctx.level));
    ReplaceAll(text, "bot_race", ctx.race);
    ReplaceAll(text, "bot_class", ctx.cls);
    ReplaceAll(text, "guild", ctx.guild.empty() ? "" : ", of the guild <" + NoBraces(ctx.guild) + ">");
    ReplaceAll(text, "personality", ctx.personality.empty() ? "" : "Personality: " + NoBraces(ctx.personality) + "\n");
    ReplaceAll(text, "identity", NoBraces(identity));
    ReplaceAll(text, "doing_minutes", std::to_string(ctx.doingMinutes));
    ReplaceAll(text, "doing", ctx.doing.empty() ? "nothing decided yet" : NoBraces(ctx.doing));
    ReplaceAll(text, "goal", ctx.goal.empty() ? "none yet" : NoBraces(ctx.goal));
    ReplaceAll(text, "live_strategies", ctx.liveStrategies);
    ReplaceAll(text, "rpg_focus", ctx.rpgFocus.empty() ? "free roaming" : ctx.rpgFocus);
    ReplaceAll(text, "rpg_status", ctx.rpgStatus.empty() ? "-" : ctx.rpgStatus);
    ReplaceAll(text, "playbook", ctx.playbook.empty() ? "none" : ctx.playbook);
    ReplaceAll(text, "rewards", ctx.rewards.empty() ? "nothing rewarding happened" : ctx.rewards);
    ReplaceAll(text, "decisions", Lines(ctx.decisions, "none yet"));
    ReplaceAll(text, "progress", ctx.progress.empty() ? "No progress history yet." : NoBraces(ctx.progress));
    ReplaceAll(text, "events", Lines(ctx.events, "nothing notable"));
    ReplaceAll(text, "temptations", Lines(ctx.temptations, "nothing in particular"));
    ReplaceAll(text, "guards", ctx.guards.empty() ? "" : ctx.guards + "\n");
    ReplaceAll(text, "state", NoBraces(ctx.state));
    ReplaceAll(text, "memories", NoBraces(ctx.memories));
    return text;
}

void AutopilotPlanner_ConfigureBudget(uint32_t callsPerHour, uint32_t maxConcurrent)
{
    std::lock_guard<std::mutex> lock(g_budgetMutex);
    RefillLocked();

    // Five minutes' worth of calls may burst; never less than two so one
    // foreground and one background plan can both get through.
    g_perSecond     = static_cast<float>(callsPerHour) / 3600.0f;
    g_capacity      = std::max(2.0f, static_cast<float>(callsPerHour) / 12.0f);
    g_maxConcurrent = std::max<uint32_t>(1, maxConcurrent);
    if (callsPerHour == 0)
        g_capacity = 0.0f;

    // Start full on first configuration so a fresh server plans right away.
    if (!g_budgetInit)
    {
        g_tokens     = g_capacity;
        g_budgetInit = true;
    }
    g_tokens = std::min(g_tokens, g_capacity);
}

bool AutopilotPlanner_CanSubmit(bool foreground)
{
    if (g_inFlight.load() >= g_maxConcurrent)
        return false;

    std::lock_guard<std::mutex> lock(g_budgetMutex);
    RefillLocked();
    const float reserve = foreground ? 0.0f : g_capacity * 0.25f;
    return g_tokens >= 1.0f + reserve;
}

bool AutopilotPlanner_Submit(uint64_t botGuid, std::string prompt)
{
    {
        std::lock_guard<std::mutex> lock(g_budgetMutex);
        RefillLocked();
        if (g_tokens < 1.0f)
            return false;
        g_tokens -= 1.0f;
    }

    ++g_inFlight;
    const bool queued = OllamaDispatch_SubmitJob(
        [botGuid, prompt = std::move(prompt)]()
        {
            // Release the slot however this ends. Without it, an exception
            // (the dispatcher's worker catches and logs it) would leak a slot,
            // and after MaxConcurrentPlans of those planning stops for good.
            struct Release { ~Release() { --g_inFlight; } } release;
            try
            {
                RunPlan(botGuid, prompt);
            }
            catch (const std::exception& e)
            {
                AutopilotDecision failed;
                failed.botGuid = botGuid;
                failed.error   = std::string("exception: ") + e.what();
                ++g_failed;
                std::lock_guard<std::mutex> lock(g_doneMutex);
                g_lastError = failed.error;
                g_done.push_back(std::move(failed));
            }
        });

    if (!queued)
    {
        --g_inFlight;
        ++g_refused;
        std::lock_guard<std::mutex> lock(g_budgetMutex);
        g_tokens = std::min(g_capacity, g_tokens + 1.0f);
        return false;
    }

    ++g_submitted;
    return true;
}

std::vector<AutopilotDecision> AutopilotPlanner_Drain()
{
    std::vector<AutopilotDecision> out;
    std::lock_guard<std::mutex> lock(g_doneMutex);
    out.swap(g_done);
    return out;
}

AutopilotPlannerStats AutopilotPlanner_GetStats()
{
    AutopilotPlannerStats s;
    s.inFlight  = g_inFlight.load();
    s.submitted = g_submitted.load();
    s.refused   = g_refused.load();
    s.parsed    = g_parsed.load();
    s.failed    = g_failed.load();
    {
        std::lock_guard<std::mutex> lock(g_budgetMutex);
        RefillLocked();
        s.tokens   = g_tokens;
        s.capacity = g_capacity;
    }
    {
        std::lock_guard<std::mutex> lock(g_doneMutex);
        s.lastError = g_lastError;
    }
    return s;
}

AutopilotDecision AutopilotPlanner_Parse(uint64_t botGuid, const std::string& reply)
{
    AutopilotDecision d;
    d.botGuid = botGuid;

    const std::string object = ExtractObject(reply);
    if (object.empty())
    {
        d.error = "no JSON object in reply";
        return d;
    }

    nlohmann::json json = nlohmann::json::parse(object, nullptr, /*allow_exceptions*/ false);
    if (json.is_discarded() || !json.is_object())
    {
        d.error = "reply is not valid JSON";
        return d;
    }

    auto str = [](const nlohmann::json& obj, const char* key) -> std::string
    {
        auto it = obj.find(key);
        if (it == obj.end())
            return "";
        if (it->is_string())
            return it->get<std::string>();
        if (it->is_number())
            return std::to_string(it->get<int64_t>());
        return "";
    };

    d.doing  = Clip(str(json, "doing"), 64);
    d.reason = Clip(str(json, "reason"), 200);
    d.say    = Clip(str(json, "say"), 200);

    if (auto it = json.find("minutes"); it != json.end())
    {
        if (it->is_number())
            d.minutes = static_cast<uint32_t>(std::clamp(it->get<double>(), 0.0, 1440.0));
        else if (it->is_string())
        {
            try { d.minutes = static_cast<uint32_t>(std::stoul(it->get<std::string>())); }
            catch (...) { d.minutes = 0; }
        }
    }

    // Identity: {"style", "outlook", "profile"}, or a bare description.
    if (auto it = json.find("identity"); it != json.end())
    {
        if (it->is_string())
            d.profile = Clip(it->get<std::string>(), 600);
        else if (it->is_object())
        {
            d.style   = Clip(str(*it, "style"), 64);
            d.outlook = Lower(Clip(str(*it, "outlook"), 32));
            d.profile = Clip(str(*it, "profile"), 600);
        }
    }

    if (auto it = json.find("strategies"); it != json.end())
        d.strategies = ParseChanges(*it);

    if (auto it = json.find("rpg"); it != json.end())
    {
        d.rpgGiven = true;
        if (it->is_array())
        {
            for (const auto& item : *it)
                if (item.is_string())
                    if (std::string s = Lower(Clip(item.get<std::string>(), 32)); !s.empty())
                        d.rpg.push_back(s);
        }
        else if (it->is_string())
        {
            for (const std::string& item : SplitString(it->get<std::string>(), ','))
                if (std::string s = Lower(Clip(item, 32)); !s.empty())
                    d.rpg.push_back(s);
        }
    }

    if (auto it = json.find("playbook"); it != json.end() && it->is_object())
    {
        d.playbookGiven = true;
        for (auto p = it->begin(); p != it->end(); ++p)
            d.playbook.emplace_back(Lower(Clip(p.key(), 24)), ParseChanges(*p));
    }

    // Goal: {"kind", "target", "text"}, or a bare sentence (a free goal).
    if (auto it = json.find("goal"); it != json.end())
    {
        if (it->is_string())
        {
            d.goalText = Clip(it->get<std::string>(), 200);
            if (!d.goalText.empty())
                d.goalKind = "free";
        }
        else if (it->is_object())
        {
            d.goalKind   = Lower(Clip(str(*it, "kind"), 24));
            d.goalTarget = Clip(str(*it, "target"), 64);
            d.goalText   = Clip(str(*it, "text"), 200);
            if (d.goalKind.empty() && !d.goalText.empty())
                d.goalKind = "free";
        }
    }

    if (d.strategies.empty() && !d.rpgGiven && d.goalKind.empty() && d.profile.empty() && d.style.empty() &&
        d.doing.empty())
    {
        d.error = "reply decides nothing";
        return d;
    }

    d.ok = true;
    return d;
}
