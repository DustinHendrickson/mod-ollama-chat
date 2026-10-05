#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_capability.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_httpclient.h"
#include "mod-ollama-chat-utilities.h"

#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <nlohmann/json.hpp>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
    // One client per worker thread; the client itself pools keep-alive sockets.
    thread_local OllamaHttpClient t_httpClient;

    std::mutex             g_settingsMutex;
    OllamaEndpointSettings g_settings;

    // Per-session self-heals for OpenAI-format endpoints, learned from one
    // HTTP 400 each and remembered so the next request does not repeat it:
    //  - newer OpenAI models reject `max_tokens` and want
    //    `max_completion_tokens` instead; older and third-party servers only
    //    know `max_tokens`
    //  - OpenAI reasoning models reject explicit temperature / top_p /
    //    penalty values and only accept their defaults
    std::atomic<bool> g_useMaxCompletionTokens{ false };
    std::atomic<bool> g_samplingRejected{ false };
    std::atomic<bool> g_maxCompletionLogged{ false };
    std::atomic<bool> g_samplingLogged{ false };
    std::atomic<bool> g_authFailureLogged{ false };
    std::atomic<bool> g_rateLimitLogged{ false };

    // 429 is the universal "slow down"; 503 is a busy upstream; 529 is
    // Anthropic's "overloaded". All are worth one short wait and a retry.
    bool IsRateLimited(int status)
    {
        return status == 429 || status == 503 || status == 529;
    }

    std::string ToLower(std::string v)
    {
        for (char& c : v)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    }

    // OllamaChat.Stop is a comma-separated list.
    std::vector<std::string> ParseStopSequences(const std::string& stop)
    {
        std::vector<std::string> stopSeqs;
        std::stringstream ss(stop);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            const size_t start = item.find_first_not_of(" \t");
            const size_t end   = item.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos)
                stopSeqs.push_back(item.substr(start, end - start + 1));
        }
        return stopSeqs;
    }

    // ----------------------------------------------------------------------
    // Ollama native: POST /api/generate
    // ----------------------------------------------------------------------
    nlohmann::json BuildOllamaRequest(const OllamaEndpointSettings& cfg,
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

        // Constrained decoding: Ollama only emits valid JSON with this set.
        if (cfg.jsonOutput)
            request["format"] = "json";

        if (!cfg.stop.empty())
        {
            const std::vector<std::string> stopSeqs = ParseStopSequences(cfg.stop);
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

    // ----------------------------------------------------------------------
    // OpenAI Chat Completions format: POST .../v1/chat/completions
    //
    // Spoken by OpenAI, OpenRouter, Groq, Mistral, DeepSeek, xAI, Gemini's
    // compatibility endpoint, LM Studio, vLLM, llama.cpp server and Ollama's
    // own /v1 route. Ollama's separate "system" + "prompt" become a two-message
    // conversation. Knobs with no equivalent (num_ctx, num_thread,
    // repeat_penalty, min_p, top_k) are dropped rather than sent: unknown
    // fields are an HTTP 400 on the strict providers.
    // ----------------------------------------------------------------------
    nlohmann::json BuildOpenAIRequest(const OllamaEndpointSettings& cfg,
                                      const std::string& prompt,
                                      const OllamaThinkRequest& think,
                                      uint32_t reasoningReserve)
    {
        nlohmann::json messages = nlohmann::json::array();
        if (!cfg.systemPrompt.empty())
            messages.push_back({ { "role", "system" }, { "content", SanitizeUTF8(cfg.systemPrompt) } });
        messages.push_back({ { "role", "user" }, { "content", SanitizeUTF8(prompt) } });

        nlohmann::json request = {
            { "model",    cfg.model },
            { "messages", std::move(messages) },
            { "stream",   false },
        };

        if (cfg.numPredict > 0)
        {
            const char* key = g_useMaxCompletionTokens.load() ? "max_completion_tokens" : "max_tokens";
            request[key] = cfg.numPredict + reasoningReserve;
        }

        if (!g_samplingRejected.load())
        {
            request["temperature"] = cfg.temperature;
            request["top_p"]       = cfg.topP;
            if (cfg.presencePenalty > -999.0f)  request["presence_penalty"]  = cfg.presencePenalty;
            if (cfg.frequencyPenalty > -999.0f) request["frequency_penalty"] = cfg.frequencyPenalty;
        }

        if (!cfg.seed.empty())
        {
            try { request["seed"] = std::stoi(cfg.seed); }
            catch (const std::exception&)
            {
                if (g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Invalid seed value: {}", cfg.seed);
            }
        }

        if (!cfg.stop.empty())
        {
            const std::vector<std::string> stopSeqs = ParseStopSequences(cfg.stop);
            if (!stopSeqs.empty())
                request["stop"] = stopSeqs;
        }

        // There is no "think: false" in this format: a reasoning model reasons
        // regardless, and a non-reasoning model rejects the field outright
        // (which the think-rejection self-heal then remembers). So the field
        // is only ever sent when reasoning is wanted, or when the module has
        // learned this model reasons unconditionally and wants it turned down.
        if (!think.level.empty())
            request["reasoning_effort"] = think.level;
        else if (think.wanted)
            request["reasoning_effort"] = "medium";

        return request;
    }

    // ----------------------------------------------------------------------
    // Anthropic Messages API: POST https://api.anthropic.com/v1/messages
    //
    // max_tokens is mandatory here. Extended thinking takes a token budget
    // rather than a flag, must be at least 1024, and is incompatible with
    // temperature / top_k, so those are left out when thinking is on.
    // Current Claude models also reject temperature and top_p together, so
    // only temperature is sent.
    // ----------------------------------------------------------------------
    nlohmann::json BuildAnthropicRequest(const OllamaEndpointSettings& cfg,
                                         const std::string& prompt,
                                         const OllamaThinkRequest& think,
                                         uint32_t reasoningReserve)
    {
        constexpr uint32_t kDefaultMaxTokens  = 1024;
        constexpr uint32_t kMinThinkingBudget = 1024;

        uint32_t maxTokens = cfg.numPredict > 0 ? cfg.numPredict : kDefaultMaxTokens;
        uint32_t budget    = 0;

        if (think.wanted)
        {
            budget     = std::max(kMinThinkingBudget, reasoningReserve);
            maxTokens += budget;   // max_tokens must exceed budget_tokens
        }

        nlohmann::json request = {
            { "model",      cfg.model },
            { "max_tokens", maxTokens },
            { "messages",   nlohmann::json::array({
                  { { "role", "user" }, { "content", SanitizeUTF8(prompt) } } }) },
            { "stream",     false },
        };

        if (!cfg.systemPrompt.empty())
            request["system"] = SanitizeUTF8(cfg.systemPrompt);

        if (!cfg.stop.empty())
        {
            const std::vector<std::string> stopSeqs = ParseStopSequences(cfg.stop);
            if (!stopSeqs.empty())
                request["stop_sequences"] = stopSeqs;
        }

        if (think.wanted)
        {
            request["thinking"] = { { "type", "enabled" }, { "budget_tokens", budget } };
        }
        else
        {
            request["temperature"] = cfg.temperature;
            if (cfg.topK >= 0)
                request["top_k"] = cfg.topK;
        }

        return request;
    }

    nlohmann::json BuildRequest(const OllamaEndpointSettings& cfg,
                                const std::string& prompt,
                                const OllamaThinkRequest& think,
                                uint32_t reasoningReserve)
    {
        switch (cfg.provider)
        {
            case OllamaProvider::OpenAI:    return BuildOpenAIRequest(cfg, prompt, think, reasoningReserve);
            case OllamaProvider::Anthropic: return BuildAnthropicRequest(cfg, prompt, think, reasoningReserve);
            default:                        return BuildOllamaRequest(cfg, prompt, think, reasoningReserve);
        }
    }

    // Pull a human-readable message out of an error body. Ollama sends
    // {"error":"..."}; OpenAI-format and Anthropic send
    // {"error":{"message":"..."}}. Anything else comes back as-is, trimmed,
    // so a proxy's HTML error page does not flood the log.
    std::string ExtractErrorMessage(const std::string& body)
    {
        try
        {
            nlohmann::json parsed = nlohmann::json::parse(body);
            if (parsed.contains("error"))
            {
                const auto& err = parsed["error"];
                if (err.is_string())
                    return err.get<std::string>();
                if (err.is_object() && err.contains("message") && err["message"].is_string())
                    return err["message"].get<std::string>();
            }
        }
        catch (const std::exception&)
        {
        }

        constexpr size_t kMaxRaw = 400;
        return body.size() > kMaxRaw ? body.substr(0, kMaxRaw) + "..." : body;
    }

    // choices[0].message.content, plus whatever reasoning field the server
    // exposes (OpenRouter: "reasoning"; DeepSeek, vLLM: "reasoning_content").
    // Content may also arrive as an array of typed parts.
    void ParseOpenAIBody(const std::string& body, std::string& outText,
                         std::string& outThinking, std::string& outError)
    {
        try
        {
            nlohmann::json parsed = nlohmann::json::parse(body);

            if (parsed.contains("error"))
            {
                outError = ExtractErrorMessage(body);
                return;
            }

            if (!parsed.contains("choices") || !parsed["choices"].is_array() || parsed["choices"].empty())
            {
                outError = "no choices in response";
                return;
            }

            const auto& choice = parsed["choices"][0];
            if (!choice.contains("message") || !choice["message"].is_object())
            {
                outError = "first choice has no message";
                return;
            }

            const auto& msg = choice["message"];
            if (msg.contains("content"))
            {
                const auto& content = msg["content"];
                if (content.is_string())
                {
                    outText = content.get<std::string>();
                }
                else if (content.is_array())
                {
                    std::ostringstream text;
                    for (const auto& part : content)
                    {
                        if (part.is_object() && part.contains("text") && part["text"].is_string())
                            text << part["text"].get<std::string>();
                    }
                    outText = text.str();
                }
            }

            for (const char* key : { "reasoning", "reasoning_content" })
            {
                if (msg.contains(key) && msg[key].is_string())
                {
                    outThinking = msg[key].get<std::string>();
                    break;
                }
            }
        }
        catch (const std::exception& e)
        {
            outError = std::string("JSON parse failure: ") + e.what();
        }
    }

    // content[] is a list of typed blocks: "text" is the answer, "thinking"
    // is the reasoning when extended thinking was enabled.
    void ParseAnthropicBody(const std::string& body, std::string& outText,
                            std::string& outThinking, std::string& outError)
    {
        try
        {
            nlohmann::json parsed = nlohmann::json::parse(body);

            if (parsed.contains("error") ||
                (parsed.contains("type") && parsed["type"].is_string() && parsed["type"] == "error"))
            {
                outError = ExtractErrorMessage(body);
                return;
            }

            if (!parsed.contains("content") || !parsed["content"].is_array())
            {
                outError = "no content array in response";
                return;
            }

            std::ostringstream text;
            std::ostringstream thinking;
            for (const auto& block : parsed["content"])
            {
                if (!block.is_object() || !block.contains("type") || !block["type"].is_string())
                    continue;

                const std::string type = block["type"].get<std::string>();
                if (type == "text" && block.contains("text") && block["text"].is_string())
                    text << block["text"].get<std::string>();
                else if (type == "thinking" && block.contains("thinking") && block["thinking"].is_string())
                    thinking << block["thinking"].get<std::string>();
            }

            outText     = text.str();
            outThinking = thinking.str();
        }
        catch (const std::exception& e)
        {
            outError = std::string("JSON parse failure: ") + e.what();
        }
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
        OllamaHttpResult http = t_httpClient.PostEx(cfg.url, request.dump(), 0,
                                                    OllamaBuildRequestHeaders(cfg), cfg.verifyCerts);
        const auto finished = std::chrono::steady_clock::now();

        result.latencyMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count());

        result.status            = http.status;
        result.retryAfterSeconds = http.retryAfterSeconds;

        if (!http.error.empty())
        {
            result.error = http.error;
            return result;
        }

        if (!http.ok())
        {
            result.error = "HTTP " + std::to_string(http.status);
            if (!http.body.empty())
                result.error += ": " + ExtractErrorMessage(http.body);
            return result;
        }

        std::string parseError;
        switch (cfg.provider)
        {
            case OllamaProvider::OpenAI:
                ParseOpenAIBody(http.body, result.text, result.thinking, parseError);
                break;
            case OllamaProvider::Anthropic:
                ParseAnthropicBody(http.body, result.text, result.thinking, parseError);
                break;
            default:
                ParseGenerateBody(http.body, result.text, result.thinking, parseError);
                break;
        }

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

    next.autopilotModel        = g_AutopilotModel;
    next.autopilotNumPredict   = g_AutopilotNumPredict;
    next.autopilotSystemPrompt = g_AutopilotSystemPrompt;

    next.provider     = g_OllamaProvider;
    next.apiKey       = g_OllamaApiKey;
    next.apiKeyHeader = g_OllamaApiKeyHeader;
    next.verifyCerts  = (g_OllamaVerifyCertificates > 0) ||
                        (g_OllamaVerifyCertificates < 0 && !g_OllamaApiKey.empty());

    // A provider or model change invalidates what was learned about the old one.
    g_useMaxCompletionTokens.store(false);
    g_samplingRejected.store(false);
    g_maxCompletionLogged.store(false);
    g_samplingLogged.store(false);
    g_authFailureLogged.store(false);
    g_rateLimitLogged.store(false);

    std::lock_guard<std::mutex> lock(g_settingsMutex);
    g_settings = std::move(next);
}

OllamaEndpointSettings OllamaConfig_Snapshot()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    return g_settings;
}

OllamaHttpHeaders OllamaBuildRequestHeaders(const OllamaEndpointSettings& cfg)
{
    OllamaHttpHeaders headers;

    if (cfg.provider == OllamaProvider::Anthropic)
        headers.emplace_back("anthropic-version", "2023-06-01");

    if (cfg.apiKey.empty())
        return headers;

    // Operator override for services with their own header convention
    // (Azure OpenAI: "api-key", Gemini native: "x-goog-api-key", custom
    // gateways). "Bearer " is added for Authorization unless already present.
    if (!cfg.apiKeyHeader.empty())
    {
        std::string value = cfg.apiKey;
        if (ToLower(cfg.apiKeyHeader) == "authorization" && ToLower(value).rfind("bearer ", 0) != 0)
            value = "Bearer " + value;
        headers.emplace_back(cfg.apiKeyHeader, value);
        return headers;
    }

    switch (cfg.provider)
    {
        case OllamaProvider::Anthropic:
            headers.emplace_back("x-api-key", cfg.apiKey);
            break;

        case OllamaProvider::OpenAI:
        default:
            // Bearer auth is also what Ollama's hosted service and most
            // reverse proxies in front of a local Ollama expect.
            headers.emplace_back("Authorization", "Bearer " + cfg.apiKey);
            break;
    }

    return headers;
}

OllamaApiResult QueryOllama(const std::string& prompt, OllamaRequestKind kind)
{
    OllamaApiResult result;

    if (prompt.empty())
    {
        result.error = "empty prompt";
        return result;
    }

    OllamaEndpointSettings settings = OllamaConfig_Snapshot();

    // A plan is a JSON object several times longer than a chat line, and may
    // be routed to a separate (often cheaper) model.
    if (kind == OllamaRequestKind::Autopilot)
    {
        if (!settings.autopilotModel.empty())
            settings.model = settings.autopilotModel;
        settings.numPredict   = settings.autopilotNumPredict;
        settings.systemPrompt = settings.autopilotSystemPrompt;
        settings.stop.clear();          // chat stop sequences can cut JSON short
        settings.jsonOutput   = true;
    }

    const OllamaEndpointSettings& cfg = settings;

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
    // One attempt plus bounded back-off on rate limiting. The sleep happens on
    // this worker thread, which is what the bounded pool and queue cap are
    // for: a throttled provider slows replies down instead of losing them.
    auto performWithBackoff = [&](OllamaThinkRequest& req, uint32_t budget)
    {
        OllamaApiResult r = PerformOnce(cfg, prompt, req, budget);

        for (uint32_t attempt = 0;
             attempt < g_RateLimitRetries && !r.ok && IsRateLimited(r.status);
             ++attempt)
        {
            uint32_t wait = r.retryAfterSeconds > 0
                                ? static_cast<uint32_t>(r.retryAfterSeconds)
                                : (2u << attempt);               // 2s, 4s, 8s ...
            wait = std::min(wait, g_RateLimitMaxWaitSeconds);
            if (wait == 0)
                break;

            if (!g_rateLimitLogged.exchange(true))
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Provider returned HTTP {} (rate limited / busy); waiting {}s "
                         "and retrying. Further occurrences are logged at debug level only. If this "
                         "is frequent, lower OllamaChat.MaxConcurrentQueries or the chatter rates.",
                         r.status, wait);
            else if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] HTTP {}; retry {}/{} after {}s.",
                         r.status, attempt + 1, g_RateLimitRetries, wait);

            std::this_thread::sleep_for(std::chrono::seconds(wait));
            r = PerformOnce(cfg, prompt, req, budget);
        }

        return r;
    };

    auto perform = [&](OllamaThinkRequest& req, uint32_t budget)
    {
        OllamaApiResult r = performWithBackoff(req, budget);

        // OpenAI-format parameter rejections. Each is learned once per
        // session and the request is retried immediately so no reply is lost.
        if (!r.ok && cfg.provider == OllamaProvider::OpenAI && r.status == 400)
        {
            const std::string lower = ToLower(r.error);

            if (!g_useMaxCompletionTokens.load() &&
                lower.find("max_completion_tokens") != std::string::npos)
            {
                g_useMaxCompletionTokens.store(true);
                if (!g_maxCompletionLogged.exchange(true))
                    LOG_INFO("module.ollamachat",
                             "[Ollama Chat] Model '{}' wants max_completion_tokens instead of "
                             "max_tokens; switching for the rest of this session.", cfg.model);
                r = PerformOnce(cfg, prompt, req, budget);
            }
            else if (!g_samplingRejected.load() &&
                     (lower.find("unsupported") != std::string::npos ||
                      lower.find("not supported") != std::string::npos) &&
                     (lower.find("temperature") != std::string::npos ||
                      lower.find("top_p") != std::string::npos ||
                      lower.find("presence_penalty") != std::string::npos ||
                      lower.find("frequency_penalty") != std::string::npos))
            {
                g_samplingRejected.store(true);
                if (!g_samplingLogged.exchange(true))
                    LOG_INFO("module.ollamachat",
                             "[Ollama Chat] Model '{}' rejects explicit sampling parameters "
                             "(temperature/top_p/penalties); sending its defaults for the rest "
                             "of this session.", cfg.model);
                r = PerformOnce(cfg, prompt, req, budget);
            }
        }

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

        // A rejected credential fails every request identically; say what to
        // check once instead of leaving the operator to infer it from a
        // stream of HTTP 401 lines.
        if ((result.status == 401 || result.status == 403) && !g_authFailureLogged.exchange(true))
        {
            LOG_ERROR("module.ollamachat",
                      "[Ollama Chat] The provider rejected the credentials (HTTP {}). {}",
                      result.status,
                      cfg.apiKey.empty()
                          ? "OllamaChat.ApiKey is empty; this provider needs one."
                          : "Check that OllamaChat.ApiKey is a valid key for this provider and that "
                            "OllamaChat.ApiKeyHeader (if set) is the header it expects.");
        }
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
