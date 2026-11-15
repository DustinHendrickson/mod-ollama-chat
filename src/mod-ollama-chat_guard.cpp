#include "mod-ollama-chat_guard.h"
#include "mod-ollama-chat_config.h"

// llm_guard.hpp defines NOMINMAX unconditionally; the MSVC build already
// passes /DNOMINMAX, so keep the core's definition and avoid a C4005.
#pragma push_macro("NOMINMAX")
#undef NOMINMAX
#define LLM_GUARD_IMPLEMENTATION
#include "llm/llm_guard.hpp"
#undef LLM_GUARD_IMPLEMENTATION
#undef NOMINMAX
#pragma pop_macro("NOMINMAX")

OllamaGuardedText OllamaGuard_PlayerMessage(const std::string& msg)
{
    OllamaGuardedText out;

    if (!g_GuardEnable || msg.empty())
    {
        out.text = msg;
        return out;
    }

    llm::GuardResult scan = llm::scan(msg);

    out.text           = std::move(scan.scrubbed);
    out.blocked        = scan.injection_detected;
    out.injectionScore = scan.injection_score;
    out.piiRedacted    = scan.matches.size();
    return out;
}
