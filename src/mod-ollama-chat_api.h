#ifndef MOD_OLLAMA_CHAT_API_H
#define MOD_OLLAMA_CHAT_API_H

#include "mod-ollama-chat_capability.h"
#include <string>
#include <cstdint>
#include <string>

// Result of one generation call. The old API returned a bare string, so
// "server unreachable", "model refused to think" and "model said nothing"
// were indistinguishable -- all three surfaced as an empty reply and a
// generic log line.
struct OllamaApiResult
{
    std::string text;             // model output, reasoning already removed
    std::string thinking;         // native reasoning, kept only for debug logs
    bool        ok        = false;
    int         status    = 0;
    std::string error;
    uint64_t    latencyMs = 0;
    bool        thinkUsed = false;

    bool empty() const { return text.empty(); }
};

// --------------------------------------------------------------------------
// Immutable snapshot of everything a generation call needs from config.
//
// Worker threads used to read g_OllamaUrl / g_OllamaModel / g_OllamaSystemPrompt
// / g_OllamaStop / g_OllamaSeed directly. `.ollama reload` reassigns those
// std::strings on the world thread, which frees the old buffer underneath any
// worker mid-copy -- an access violation deep inside the HTTP client, with a
// stack that points at httplib rather than at the real cause.
//
// Config is now published once under a mutex and workers take a copy.
// --------------------------------------------------------------------------
struct OllamaEndpointSettings
{
    std::string url;
    std::string model;
    std::string systemPrompt;
    std::string stop;
    std::string seed;

    uint32_t numPredict = 0;
    uint32_t numCtx     = 0;
    uint32_t numThreads = 0;

    float temperature   = 0.8f;
    float topP          = 0.95f;
    float repeatPenalty = 1.1f;

    // Optional sampling controls. Negative means "unset" -- the field is then
    // omitted from the request entirely and the model's own default applies.
    int32_t topK             = -1;
    float   minP             = -1.0f;
    float   presencePenalty  = -1000.0f;
    float   frequencyPenalty = -1000.0f;
};

// Republish from the g_Ollama* globals. Call on the world thread after config
// load or reload.
void OllamaConfig_Publish();

// Thread-safe copy for a worker.
OllamaEndpointSettings OllamaConfig_Snapshot();

// Perform one generation. Decides whether to think based on the configured
// policy and the probed model capability, and transparently retries once
// without thinking if Ollama rejects the request for asking.
//
// Blocking. Call from a worker thread, never from the world thread.
OllamaApiResult QueryOllama(const std::string& prompt, OllamaRequestKind kind);

// Legacy shim: returns the text, or empty on any failure.
std::string QueryOllamaAPI(const std::string& prompt);

bool IsValidAPIResponse(const std::string& response);

#endif // MOD_OLLAMA_CHAT_API_H
