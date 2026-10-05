#include "mod-ollama-chat_autopilot_planner.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_autopilot_goals.h"
#include "mod-ollama-chat_autopilot_presets.h"
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

    // Static instructions, menus and schema first; the character last. Ollama
    // reuses its KV cache for an identical prefix and hosted providers cache
    // on the prefix too, so everything above THE CHARACTER is close to free on
    // every call after the first.
    //
    // Placeholders are replaced literally (not with fmt), so the JSON braces
    // in the template need no escaping -- including in an operator's own.
    const char* const kDefaultTemplate =
        "You plan what a World of Warcraft character does next, at the level of a play session. "
        "Choose what THIS character would actually want to do, given their playstyle, how they see "
        "the world, what has happened lately and how they feel about it. People get bored doing the "
        "same thing for hours; variety and small temptations are normal.\n"
        "\n"
        "ACTIVITIES (choose exactly one, by name):\n"
        "{activities}\n"
        "\n"
        "DISPOSITIONS (choose one option for each):\n"
        "{dispositions}\n"
        "\n"
        "GOAL KINDS (a longer-term aim the game can measure):\n"
        "{goal_kinds}\n"
        "\n"
        "PLAYBOOK (optional: a disposition option to use in a particular situation, overriding the one above "
        "there): situations are {situations}.\n"
        "\n"
        "Reply with one JSON object and nothing else, shaped like this:\n"
        "{\"activity\": \"...\", {disposition_shape}\"minutes\": 30, "
        "\"goal\": {\"kind\": \"...\", \"target\": \"...\", \"text\": \"...\"}, "
        "\"playbook\": {\"dungeon\": \"...\"}, \"reason\": \"...\"}\n"
        "- activity: an activity name from the list that is available to this character.\n"
        "- minutes: how long to keep at it before reconsidering (15 to 90).\n"
        "- goal: keep the current goal unless it is done, stalled or no longer fits; omit it to keep it. "
        "text is the aim in one sentence, phrased the way this character thinks about it.\n"
        "- playbook: optional; omit it to keep the current one.\n"
        "- reason: one short sentence on why this, now.\n"
        "\n"
        "THE CHARACTER\n"
        "{bot_name}, level {bot_level} {bot_race} {bot_class}.\n"
        "Playstyle: {playstyle} - {playstyle_description}\n"
        "Outlook: {awareness} - {awareness_description}\n"
        "{personality}"
        "Not available right now: {unavailable}\n"
        "Currently: {activity} (for {activity_minutes} minutes). Current goal: {goal}\n"
        "Mood: {mood}\n"
        "Playbook: {playbook}\n"
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
    // memories) goes back into later prompts. Braces in it must not be taken
    // for placeholders by the replacements that run after it.
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
    std::string activities;
    for (const AutopilotPreset& a : AutopilotPresets_Activities())
        activities += (activities.empty() ? "- " : "\n- ") + a.name + ": " + a.description;

    std::string dispositions;
    std::string shape;
    for (const AutopilotDispositionAxis& axis : AutopilotPresets_Axes())
    {
        std::string options;
        for (const AutopilotPreset& o : axis.options)
            options += (options.empty() ? "" : " | ") + o.name + " (" + o.description + ")";
        dispositions += (dispositions.empty() ? "- " : "\n- ") + axis.name + ": " + options;
        shape += "\"" + axis.name + "\": \"...\", ";
    }

    std::string text = templ.empty() ? std::string(kDefaultTemplate) : templ;

    // Conf values carry "\n" literally; the default carries real newlines.
    for (size_t pos = text.find("\\n"); pos != std::string::npos; pos = text.find("\\n", pos + 1))
        text.replace(pos, 2, "\n");

    ReplaceAll(text, "activities", activities);
    ReplaceAll(text, "dispositions", dispositions.empty() ? "- (none)" : dispositions);
    ReplaceAll(text, "disposition_shape", shape);
    ReplaceAll(text, "goal_kinds", Goal_KindMenu());
    ReplaceAll(text, "situations", "dungeon, battleground, with_player");
    ReplaceAll(text, "mood", ctx.mood.empty() ? "settled" : ctx.mood);
    ReplaceAll(text, "playbook", ctx.playbook.empty() ? "none" : ctx.playbook);
    ReplaceAll(text, "temptations", Lines(ctx.temptations, "nothing in particular"));
    ReplaceAll(text, "bot_name", ctx.botName);
    ReplaceAll(text, "bot_level", std::to_string(ctx.level));
    ReplaceAll(text, "bot_race", ctx.race);
    ReplaceAll(text, "bot_class", ctx.cls);
    ReplaceAll(text, "playstyle", ctx.playstyle);
    ReplaceAll(text, "playstyle_description", ctx.playstyleDescription);
    ReplaceAll(text, "awareness", ctx.awareness);
    ReplaceAll(text, "awareness_description", ctx.awarenessDescription);
    ReplaceAll(text, "personality", ctx.personality.empty() ? "" : "Personality: " + NoBraces(ctx.personality) + "\n");
    ReplaceAll(text, "unavailable", ctx.unavailable.empty() ? "nothing" : ctx.unavailable);
    ReplaceAll(text, "activity", ctx.activity.empty() ? "nothing in particular" : ctx.activity);
    ReplaceAll(text, "activity_minutes", std::to_string(ctx.activityMinutes));
    ReplaceAll(text, "goal", ctx.goal.empty() ? "none yet" : NoBraces(ctx.goal));
    ReplaceAll(text, "decisions", Lines(ctx.decisions, "none yet"));
    ReplaceAll(text, "progress", ctx.progress.empty() ? "No progress history yet." : NoBraces(ctx.progress));
    ReplaceAll(text, "events", Lines(ctx.events, "nothing notable"));
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

    auto str = [&](const char* key) -> std::string
    {
        auto it = json.find(key);
        return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
    };

    d.activity = Lower(Clip(str("activity"), 32));
    d.reason   = Clip(str("reason"), 200);
    d.say      = Clip(str("say"), 200);

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
            auto field = [&](const char* key) -> std::string
            {
                auto f = it->find(key);
                if (f == it->end())
                    return "";
                if (f->is_string())
                    return f->get<std::string>();
                if (f->is_number())
                    return std::to_string(f->get<int64_t>());
                return "";
            };
            d.goalKind   = Lower(Clip(field("kind"), 24));
            d.goalTarget = Clip(field("target"), 64);
            d.goalText   = Clip(field("text"), 200);
            if (d.goalKind.empty() && !d.goalText.empty())
                d.goalKind = "free";
        }
    }

    if (auto it = json.find("playbook"); it != json.end() && it->is_object())
        for (auto p = it->begin(); p != it->end(); ++p)
            if (p->is_string())
            {
                std::string option = Lower(Clip(p->get<std::string>(), 32));
                if (!option.empty())
                    d.playbook.emplace_back(Lower(Clip(p.key(), 24)), option);
            }

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

    // Everything else that is a short string is a candidate disposition
    // ("risk": "cautious"). Which keys are real axes is checked on the world
    // thread. A nested "disposition": {...} object is accepted too.
    auto collect = [&](const nlohmann::json& obj)
    {
        for (auto it = obj.begin(); it != obj.end(); ++it)
        {
            const std::string key = Lower(it.key());
            if (key == "activity" || key == "goal" || key == "reason" || key == "say" ||
                key == "minutes" || key == "playbook")
                continue;
            if (it->is_string())
            {
                std::string value = Lower(Clip(it->get<std::string>(), 32));
                if (!value.empty())
                    d.disposition.emplace_back(Clip(key, 32), value);
            }
        }
    };
    collect(json);
    for (const char* nested : { "disposition", "dispositions" })
        if (auto it = json.find(nested); it != json.end() && it->is_object())
            collect(*it);

    if (d.activity.empty())
    {
        d.error = "reply has no activity";
        return d;
    }

    d.ok = true;
    return d;
}
