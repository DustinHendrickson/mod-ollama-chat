#ifndef MOD_OLLAMA_CHAT_CAPABILITY_H
#define MOD_OLLAMA_CHAT_CAPABILITY_H

#include <string>
#include <cstdint>

// --------------------------------------------------------------------------
// Think-mode capability detection and policy.
//
// Two separate questions the old code conflated into one bool:
//
//   CAN this model think?   -- answered by probing Ollama, cached per
//                              (url, model), and self-healed at runtime when a
//                              request comes back rejected.
//   SHOULD it think here?   -- answered per request kind. Reasoning costs
//                              seconds and tokens; a fifteen-word tavern line
//                              does not need it, a sentiment judgement does.
//
// Getting the first one wrong used to silence every bot on the server with
// nothing in the log but a generic empty-response error.
// --------------------------------------------------------------------------

enum class OllamaThinkPolicy : uint8_t
{
    Auto = 0,   // capability-gated and kind-gated (default)
    On   = 1,   // always think when the model supports it
    Off  = 2,   // never think
};

enum class OllamaRequestKind : uint8_t
{
    ChatReply = 0,
    RandomChatter,
    EventChatter,
    Sentiment,
    RoleplayReply,
};

enum class OllamaThinkSupport : uint8_t
{
    Unknown = 0,      // probe has not completed yet
    Supported,
    Unsupported,
    ProbeFailed,      // could not reach Ollama; treated as unsupported
};

// Kick off (or re-run) the capability probe. Runs on a background thread so a
// missing or slow Ollama cannot stall worldserver startup. Safe to call again
// after a config reload; re-probes when url or model changed, or when forced.
void OllamaCapability_Init(bool force = false);

OllamaThinkSupport OllamaCapability_GetSupport();
bool OllamaCapability_SupportsThinking();

// The policy decision for one request.
bool OllamaCapability_ShouldThink(OllamaRequestKind kind);

// True when an HTTP failure is Ollama telling us the model cannot think.
bool OllamaCapability_IsThinkRejection(int status, const std::string& body);

// Self-heal: called when a live request was rejected for asking to think.
// Flips the cached support flag off and logs once.
void OllamaCapability_NoteThinkRejected();

// Latency guard: feeds the rolling average that auto mode uses to back off
// think mode when the model turns out to be too slow for chat.
void OllamaCapability_NoteLatency(uint64_t milliseconds, bool thinkUsed);

// True once the latency guard has disabled think for the rest of the session.
bool OllamaCapability_ThinkDisabledByLatency();

// Human-readable summary for the .ollama status command.
std::string OllamaCapability_StatusText();

// Parse "auto" / "on" / "off" (and the legacy bool) into a policy.
OllamaThinkPolicy OllamaCapability_ParsePolicy(const std::string& text, bool legacyBool);

#endif // MOD_OLLAMA_CHAT_CAPABILITY_H
