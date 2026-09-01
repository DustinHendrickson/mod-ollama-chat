#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_capability.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_httpclient.h"
#include "mod-ollama-chat-utilities.h"

#include "Log.h"

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <mutex>
#include <sstream>

namespace
{
    // One client per worker thread; the client itself pools keep-alive sockets.
    thread_local OllamaHttpClient t_httpClient;

    std::mutex             g_settingsMutex;
    OllamaEndpointSettings g_settings;

    nlohmann::json BuildRequest(const OllamaEndpointSettings& cfg,
                                const std::string& prompt,
                                const OllamaThinkRequest& think,
                                uint32_t reasoningReserve)
    {
        nlohmann::json request = {
            { "model",  cfg.model },
            { "prompt", SanitizeUTF8(prompt) },
            { "stream", false },
        };

        nlohmann::json options;
        bool hasOptions = false;

        auto setOpt = [&](const char* key, auto value)
        {
            options[key] = value;
            hasOptions = true;
        };

        // num_predict caps reasoning and answer together, so when reasoning
        // tokens are expected the cap has to cover both or the answer never
        // gets emitted. 0 already means unlimited; leave it alone.
        if (cfg.numPredict > 0)          setOpt("num_predict", cfg.numPredict + reasoningReserve);
        if (cfg.temperature != 0.8f)     setOpt("temperature", cfg.temperature);
        if (cfg.topP != 0.95f)           setOpt("top_p", cfg.topP);
        if (cfg.repeatPenalty != 1.1f)   setOpt("repeat_penalty", cfg.repeatPenalty);
        if (cfg.numCtx > 0)              setOpt("num_ctx", cfg.numCtx);
        if (cfg.numThreads > 0)          setOpt("num_thread", cfg.numThreads);

        // Optional diversity controls. Omitted entirely unless the operator
        // opted in, so the model's own defaults apply and nothing changes for
        // anyone who leaves them alone.
        if (cfg.topK >= 0)                    setOpt("top_k", cfg.topK);
        if (cfg.minP >= 0.0f)                 setOpt("min_p", cfg.minP);
        if (cfg.presencePenalty > -999.0f)    setOpt("presence_penalty", cfg.presencePenalty);
        if (cfg.frequencyPenalty > -999.0f)   setOpt("frequency_penalty", cfg.frequencyPenalty);

        if (!cfg.seed.empty())
        {
            try
            {
                setOpt("seed", std::stoi(cfg.seed));
            }
            catch (const std::exception&)
            {
                if (g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Invalid seed value: {}", cfg.seed);
            }
        }

        if (hasOptions)
            request["options"] = options;

        if (!cfg.stop.empty())
        {
            std::vector<std::string> stopSeqs;
            std::stringstream ss(cfg.stop);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                const size_t start = item.find_first_not_of(" \t");
                const size_t end   = item.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos)
                    stopSeqs.push_back(item.substr(start, end - start + 1));
            }
            if (!stopSeqs.empty())
                request["stop"] = stopSeqs;
        }

        if (!cfg.systemPrompt.empty())
            request["system"] = SanitizeUTF8(cfg.systemPrompt);

        // Always explicit. Some models keep reasoning switched on unless they
        // are told otherwise, so omitting the field is not the same as
        // disabling it. The old code also sent "hidethinking", which Ollama
        // does not define and silently ignored.
        //
        // A resolved level wins over the bool: on a model that ignores
        // think:false, "low" is the only way to actually turn reasoning down.
        if (!think.level.empty())
            request["think"] = think.level;
        else
            request["think"] = think.enabled;

        return request;
    }

    // Ollama answers non-streaming requests with a single JSON object, but the
    // streaming shape (one object per line) still shows up behind some proxies,
    // so accumulate across lines either way.
    void ParseGenerateBody(const std::string& body, std::string& outText,
                           std::string& outThinking, std::string& outError)
    {
        std::ostringstream text;
        std::ostringstream thinking;
        bool parsedAny = false;

        std::stringstream ss(body);
        std::string line;

        while (std::getline(ss, line))
        {
            if (line.empty() || std::all_of(line.begin(), line.end(),
                                            [](unsigned char c) { return std::isspace(c); }))
                continue;

            try
            {
                nlohmann::json parsed = nlohmann::json::parse(line);
                parsedAny = true;

                if (parsed.contains("error") && parsed["error"].is_string())
                {
                    outError = parsed["error"].get<std::string>();
                    continue;
                }

                if (parsed.contains("response") && parsed["response"].is_string())
                    text << parsed["response"].get<std::string>();

                // Native reasoning arrives here, separate from "response".
                if (parsed.contains("thinking") && parsed["thinking"].is_string())
                    thinking << parsed["thinking"].get<std::string>();
            }
            catch (const std::exception& e)
            {
                if (outError.empty())
                    outError = std::string("JSON parse failure: ") + e.what();
            }
        }

        if (!parsedAny && outError.empty())
            outError = "no JSON object in response body";

        outText     = text.str();
        outThinking = thinking.str();
    }

    OllamaApiResult PerformOnce(const OllamaEndpointSettings& cfg,
                                const std::string& prompt,
                                const OllamaThinkRequest& think,
                                uint32_t reasoningReserve)
    {
        OllamaApiResult result;

        // The latency guard measures deliberate reasoning only. Reasoning a
        // model does on its own is not something backing think off can fix.
        result.thinkUsed = think.wanted;

        const nlohmann::json request = BuildRequest(cfg, prompt, think, reasoningReserve);

        const auto started = std::chrono::steady_clock::now();
        OllamaHttpResult http = t_httpClient.PostEx(cfg.url, request.dump());
        const auto finished = std::chrono::steady_clock::now();

        result.latencyMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count());

        result.status = http.status;

        if (!http.error.empty())
        {
            result.error = http.error;
            return result;
        }

        if (!http.ok())
        {
            result.error = "HTTP " + std::to_string(http.status);
            if (!http.body.empty())
                result.error += ": " + http.body;
            return result;
        }

        std::string parseError;
        ParseGenerateBody(http.body, result.text, result.thinking, parseError);

        if (!parseError.empty())
        {
            result.error = parseError;
            return result;
        }

        result.ok = true;
        return result;
    }
}

// --------------------------------------------------------------------------

void OllamaConfig_Publish()
{
    OllamaEndpointSettings next;
    next.url              = g_OllamaUrl;
    next.model            = g_OllamaModel;
    next.systemPrompt     = g_OllamaSystemPrompt;
    next.stop             = g_OllamaStop;
    next.seed             = g_OllamaSeed;
    next.numPredict       = g_OllamaNumPredict;
    next.numCtx           = g_OllamaNumCtx;
    next.numThreads       = g_OllamaNumThreads;
    next.temperature      = g_OllamaTemperature;
    next.topP             = g_OllamaTopP;
    next.repeatPenalty    = g_OllamaRepeatPenalty;
    next.topK             = g_OllamaTopK;
    next.minP             = g_OllamaMinP;
    next.presencePenalty  = g_OllamaPresencePenalty;
    next.frequencyPenalty = g_OllamaFrequencyPenalty;

    std::lock_guard<std::mutex> lock(g_settingsMutex);
    g_settings = std::move(next);
}

OllamaEndpointSettings OllamaConfig_Snapshot()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    return g_settings;
}

OllamaApiResult QueryOllama(const std::string& prompt, OllamaRequestKind kind)
{
    OllamaApiResult result;

    if (prompt.empty())
    {
        result.error = "empty prompt";
        return result;
    }

    const OllamaEndpointSettings cfg = OllamaConfig_Snapshot();

    // One place decides what the "think" field should be: policy for this
    // request kind, plus everything learned about this model so far.
    OllamaThinkRequest think = OllamaCapability_ResolveThink(kind);

    // Reasoning tokens come out of the same num_predict budget as the answer.
    // Reserve headroom whenever we expect them -- because reasoning was asked
    // for, or because this model produces it regardless.
    const bool expectReasoning = think.wanted || OllamaCapability_ReasonsUnconditionally();
    const uint32_t reserve     = expectReasoning ? g_ReasoningTokenReserve : 0;

    // Every attempt goes through here so a refused reasoning level self-heals
    // on whichever attempt happens to carry it, not just the first.
    auto perform = [&](OllamaThinkRequest& req, uint32_t budget)
    {
        OllamaApiResult r = PerformOnce(cfg, prompt, req, budget);

        // Ollama would not take a string reasoning level. Drop to the boolean
        // form, remember it for this model, and answer rather than lose this
        // request.
        if (!r.ok && !req.level.empty() && r.status >= 400 && r.status < 500)
        {
            OllamaCapability_NoteEffortLevelRejected();
            req.level.clear();
            r = PerformOnce(cfg, prompt, req, budget);
        }

        return r;
    };

    result = perform(think, reserve);

    // Self-heal: the model told us it cannot think. Remember that, and answer
    // this request anyway instead of leaving the bot mute.
    if (!result.ok && think.wanted &&
        OllamaCapability_IsThinkRejection(result.status, result.error))
    {
        OllamaCapability_NoteThinkRejected();
        think  = OllamaThinkRequest{};
        result = PerformOnce(cfg, prompt, think, 0);
    }

    // Self-heal: HTTP 200, reasoning present, answer empty. The model spent
    // the entire budget thinking -- it ignored think:false, or the cap was too
    // small to cover reasoning plus a reply. Remember that this model reasons
    // unconditionally, re-resolve (which now yields the low effort level), and
    // retry with headroom so this message is not lost.
    if (result.ok && result.text.empty() && !result.thinking.empty() &&
        cfg.numPredict > 0 && reserve == 0 && g_ReasoningTokenReserve > 0)
    {
        if (!think.wanted)
            OllamaCapability_NoteUnconditionalReasoning();

        think  = OllamaCapability_ResolveThink(kind);
        result = perform(think, g_ReasoningTokenReserve);
    }

    // Still nothing but reasoning. Say so plainly -- this used to surface only
    // as "produced nothing usable after cleanup", which points at the wrong
    // part of the pipeline entirely.
    if (result.ok && result.text.empty() && !result.thinking.empty())
    {
        LOG_ERROR("module.ollamachat",
                  "[Ollama Chat] Model '{}' returned {} characters of reasoning and no answer. "
                  "NumPredict={} plus ReasoningTokenReserve={} was not enough to finish "
                  "reasoning and reply; raise one of them, or set NumPredict = 0.",
                  cfg.model, result.thinking.size(), cfg.numPredict, g_ReasoningTokenReserve);
    }

    if (result.ok)
        OllamaCapability_NoteLatency(result.latencyMs, result.thinkUsed);

    if (!result.ok)
    {
        LOG_ERROR("module.ollamachat",
                  "[Ollama Chat] Generation failed (model '{}', {}ms): {}",
                  cfg.model, result.latencyMs,
                  result.error.empty() ? "unknown error" : result.error);
    }
    else if (g_DebugEnabled)
    {
        const std::string thinkText = !think.level.empty()
                                    ? think.level
                                    : (think.enabled ? std::string("yes") : std::string("no"));

        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] Generation ok in {}ms (think={}), {} chars.",
                 result.latencyMs, thinkText, result.text.size());

        if (g_DebugShowFullPrompt && !result.thinking.empty())
            LOG_INFO("module.ollamachat", "[Ollama Chat] Model reasoning: {}", result.thinking);
    }

    return result;
}

std::string QueryOllamaAPI(const std::string& prompt)
{
    OllamaApiResult r = QueryOllama(prompt, OllamaRequestKind::ChatReply);
    return r.ok ? r.text : std::string();
}

bool IsValidAPIResponse(const std::string& response)
{
    return !response.empty();
}
