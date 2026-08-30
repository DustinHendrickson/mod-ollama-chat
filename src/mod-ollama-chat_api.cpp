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
                                const std::string& prompt, bool think)
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

        if (cfg.numPredict > 0)          setOpt("num_predict", cfg.numPredict);
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
        request["think"] = think;

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
                                const std::string& prompt, bool think)
    {
        OllamaApiResult result;
        result.thinkUsed = think;

        const nlohmann::json request = BuildRequest(cfg, prompt, think);

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
    const bool wantThink = OllamaCapability_ShouldThink(kind);

    result = PerformOnce(cfg, prompt, wantThink);

    // Self-heal: the model told us it cannot think. Remember that, and answer
    // this request anyway instead of leaving the bot mute.
    if (!result.ok && wantThink &&
        OllamaCapability_IsThinkRejection(result.status, result.error))
    {
        OllamaCapability_NoteThinkRejected();
        result = PerformOnce(cfg, prompt, false);
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
        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] Generation ok in {}ms (think={}), {} chars.",
                 result.latencyMs, result.thinkUsed ? "yes" : "no", result.text.size());

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
