#include "mod-ollama-chat_autopilot_planner.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_autopilot_goals.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat-utilities.h"

#include "Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;

    // Static instructions and the command reference first, the character
    // last. Ollama reuses its KV cache for an identical prefix and hosted
    // providers cache on the prefix too, so everything above THE CHARACTER is
    // close to free on every call after the first.
    //
    // Placeholders are replaced literally (not with fmt), so the JSON braces
    // in the template need no escaping -- including in an operator's own.
    const char* const kDefaultTemplate =
        "You are the player behind a World of Warcraft character -- or, if they are in-character, the "
        "character's own mind. You decide who they are, what they want, and what they do with their time, "
        "and you make it happen by giving orders to the character's own AI, exactly as a player whispers "
        "commands to their bot. The AI does the walking, fighting and looting; you decide where, what and why: "
        "which quest to chase, when to go train, when to repair and sell, when to grind, gather, queue for a "
        "dungeon, travel to another zone or continent, or take a break. Nothing else steers them: without "
        "your orders they stand where they are.\n"
        "\n"
        "COMMANDS\n"
        "{commands}\n"
        "\n"
        "GOAL KINDS (a longer-term aim the game can measure):\n"
        "{goal_kinds}\n"
        "\n"
        "Reply with one JSON object and nothing else, shaped like this:\n"
        "{\"identity\": {\"style\": \"...\", \"outlook\": \"...\", \"profile\": \"...\"}, "
        "\"doing\": \"...\", \"commands\": [\"...\", \"...\"], \"minutes\": 30, "
        "\"goal\": {\"kind\": \"...\", \"target\": \"...\", \"text\": \"...\"}, \"reason\": \"...\"}\n"
        "- identity: who this character is. style = their way of playing in a few words; outlook = "
        "in-character (they do not know it is a game), player (plays for fun, knows it is a game) or "
        "metagamer (knows the systems and optimises); profile = two or three sentences: what they love, "
        "what bores them, what tempts them, how they see the world. {identity_rule}\n"
        "- doing: what they are actually doing now, concretely (\"turning in Wolves Across the Border, then "
        "training\"), in their voice but never only a mood -- it is checked against what happens.\n"
        "- commands: up to {max_commands} orders to give now, in order. They must carry out what doing says: "
        "if doing is a quest, give quest <id>; if it is training, goto trainer; if it is hunting, goto hunt "
        "with grind on. Orders that go somewhere run one after another, each to the end, then you are asked "
        "again. Strategies stay as they are unless you change them, so make sure what is on fits what they "
        "should be doing. Send [] to carry on as they are.\n"
        "- minutes: how long before you look again (5 to 180). Short while running an errand or when things "
        "are changing; long when they are settled into something.\n"
        "- goal: keep the current goal unless it is done, stalled or no longer fits them; omit to keep it. "
        "text is the aim in one sentence, phrased the way this character thinks.\n"
        "- reason: one short sentence on why this, now -- the way they would explain it.\n"
        "Decide from who they are, what they want, and what has actually happened to them. Look after them "
        "(repair, train, sell junk, stay alive), but people also get bored, get tempted, change their minds "
        "and take breaks. If an order did not work last time, do something else.\n"
        "\n"
        "THE CHARACTER\n"
        "{bot_name}, level {bot_level} {bot_gender} {bot_race} {bot_class}{guild}.\n"
        "{personality}"
        "Identity: {identity}\n"
        "Doing: {doing} (for {doing_minutes} minutes). Current goal: {goal}\n"
        "Strategies on: {strategies}\n"
        "Right now: {activity}{errand}\n"
        "{concerns}"
        "Quest log:\n{quests}\n"
        "Nearest services: {services}\n"
        "Zones for their level (another continent in [brackets]): {zones}\n"
        "What your last orders did:\n{results}\n"
        "Last hour: {rewards}\n"
        "Recent decisions:\n{decisions}\n"
        "{progress}\n"
        "Recent events:\n{events}\n"
        "Tempting right now:\n{temptations}\n"
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

    // Last exchange per bot, for the debug monitor. Workers write the reply
    // under this mutex only; nothing else here is shared with them.
    constexpr size_t kExchangeCap = 48;
    std::mutex                                       g_exchangeMutex;
    std::unordered_map<uint64_t, AutopilotExchange> g_exchanges;

    uint32_t UnixNow() { return uint32_t(std::time(nullptr)); }

    void RecordPrompt(uint64_t botGuid, const std::string& prompt)
    {
        std::lock_guard<std::mutex> lock(g_exchangeMutex);
        if (g_exchanges.size() >= kExchangeCap && !g_exchanges.count(botGuid))
        {
            auto oldest = std::min_element(g_exchanges.begin(), g_exchanges.end(),
                [](const auto& a, const auto& b) { return a.second.submittedAt < b.second.submittedAt; });
            g_exchanges.erase(oldest);
        }
        AutopilotExchange& e = g_exchanges[botGuid];
        e = AutopilotExchange();
        e.prompt      = prompt;
        e.submittedAt = UnixNow();
    }

    void RecordReply(uint64_t botGuid, const std::string& reply, const std::string& error, uint64_t latencyMs)
    {
        std::lock_guard<std::mutex> lock(g_exchangeMutex);
        auto it = g_exchanges.find(botGuid);
        if (it == g_exchanges.end())
            return;
        it->second.reply      = reply;
        it->second.error      = error;
        it->second.latencyMs  = latencyMs;
        it->second.answeredAt = UnixNow();
    }

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

    std::vector<std::string> ParseCommands(const nlohmann::json& value)
    {
        std::vector<std::string> out;
        auto add = [&out](const std::string& raw)
        {
            if (out.size() >= AUTOPILOT_MAX_COMMANDS)
                return;
            std::string s = Clip(raw, AUTOPILOT_MAX_COMMAND_CHARS);
            // One command per entry: a newline would be a second whisper.
            std::replace(s.begin(), s.end(), '\n', ' ');
            std::replace(s.begin(), s.end(), '\r', ' ');
            if (!s.empty())
                out.push_back(std::move(s));
        };

        if (value.is_array())
        {
            for (const auto& item : value)
                if (item.is_string())
                    add(item.get<std::string>());
        }
        else if (value.is_string())
        {
            for (const std::string& item : SplitString(value.get<std::string>(), ';'))
                add(item);
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
        RecordReply(botGuid, api.text, decision.ok ? std::string() : decision.error, api.latencyMs);

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

    // The command reference may itself mention {braces}; nothing after it is
    // a key it could contain except by an operator's own choice.
    ReplaceAll(text, "commands", ctx.commandReference);
    ReplaceAll(text, "goal_kinds", Goal_KindMenu());
    ReplaceAll(text, "max_commands", std::to_string(AUTOPILOT_MAX_COMMANDS));
    ReplaceAll(text, "identity_rule", identityRule);
    ReplaceAll(text, "bot_name", ctx.botName);
    ReplaceAll(text, "bot_level", std::to_string(ctx.level));
    ReplaceAll(text, "bot_race", ctx.race);
    ReplaceAll(text, "bot_gender", ctx.gender);
    ReplaceAll(text, "bot_class", ctx.cls);
    ReplaceAll(text, "guild", ctx.guild.empty() ? "" : ", of the guild <" + NoBraces(ctx.guild) + ">");
    ReplaceAll(text, "personality", ctx.personality.empty() ? "" : "Personality: " + NoBraces(ctx.personality) + "\n");
    ReplaceAll(text, "identity", NoBraces(identity));
    ReplaceAll(text, "doing_minutes", std::to_string(ctx.doingMinutes));
    ReplaceAll(text, "doing", ctx.doing.empty() ? "nothing decided yet" : NoBraces(ctx.doing));
    ReplaceAll(text, "goal", ctx.goal.empty() ? "none yet" : NoBraces(ctx.goal));
    ReplaceAll(text, "strategies", ctx.strategiesOn.empty() ? "-" : ctx.strategiesOn);
    ReplaceAll(text, "activity", ctx.activity.empty() ? "standing by" : NoBraces(ctx.activity));
    ReplaceAll(text, "errand", ctx.errand.empty() ? "" : " " + NoBraces(ctx.errand));
    ReplaceAll(text, "concerns", ctx.concerns.empty() ? "" : NoBraces(ctx.concerns) + "\n");
    ReplaceAll(text, "quests", ctx.questLog.empty() ? "- empty" : NoBraces(ctx.questLog));
    ReplaceAll(text, "services", ctx.services.empty() ? "none known" : NoBraces(ctx.services));
    ReplaceAll(text, "zones", ctx.zones.empty() ? "-" : NoBraces(ctx.zones));
    ReplaceAll(text, "results", Lines(ctx.lastResults, "no orders given yet"));
    ReplaceAll(text, "rewards", ctx.rewards.empty() ? "nothing rewarding happened" : ctx.rewards);
    ReplaceAll(text, "decisions", Lines(ctx.decisions, "none yet"));
    ReplaceAll(text, "progress", ctx.progress.empty() ? "No progress history yet." : NoBraces(ctx.progress));
    ReplaceAll(text, "events", Lines(ctx.events, "nothing notable"));
    ReplaceAll(text, "temptations", Lines(ctx.temptations, "nothing in particular"));
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

    RecordPrompt(botGuid, prompt);
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
                RecordReply(botGuid, std::string(), failed.error, 0);
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

bool AutopilotPlanner_LastExchange(uint64_t botGuid, AutopilotExchange& out)
{
    std::lock_guard<std::mutex> lock(g_exchangeMutex);
    auto it = g_exchanges.find(botGuid);
    if (it == g_exchanges.end())
        return false;
    out = it->second;
    return true;
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

    if (auto it = json.find("commands"); it != json.end())
        d.commands = ParseCommands(*it);

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

    // "commands": [] is a real decision (carry on), as long as something
    // else in the reply shows the model answered the question.
    if (d.commands.empty() && json.find("commands") == json.end() && d.goalKind.empty() && d.profile.empty() &&
        d.style.empty() && d.doing.empty() && d.minutes == 0)
    {
        d.error = "reply decides nothing";
        return d;
    }

    d.ok = true;
    return d;
}
