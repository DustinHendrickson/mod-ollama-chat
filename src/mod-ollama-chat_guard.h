#ifndef MOD_OLLAMA_CHAT_GUARD_H
#define MOD_OLLAMA_CHAT_GUARD_H

#include <cstddef>
#include <string>

// --------------------------------------------------------------------------
// Input guard for player-typed chat.
//
// Whatever a real player types in say, party, guild, a channel or a whisper
// is pasted into a bot's prompt, into its conversation history and into the
// sentiment prompt -- and, with a hosted provider configured, sent off the
// server. Two things are checked before that happens:
//
//   - PII (email, phone, SSN, card numbers that pass Luhn, API keys) is
//     replaced with a placeholder such as [EMAIL] in the copy the bots see.
//     What other players see in chat is never touched.
//   - Lines that read as a prompt-injection attempt ("ignore previous
//     instructions ... you are now ... system prompt") are not answered.
//
// Backed by the bundled single-header llm-guard (deps/llm). Pure string work,
// safe on any thread. Controlled by OllamaChat.Guard.Enable.
// --------------------------------------------------------------------------

struct OllamaGuardedText
{
    std::string text;                 // what goes into prompts and history
    bool        blocked        = false;   // injection detected; do not answer
    double      injectionScore = 0.0;     // 0.0 - 1.0
    size_t      piiRedacted    = 0;       // number of spans replaced
};

OllamaGuardedText OllamaGuard_PlayerMessage(const std::string& msg);

#endif // MOD_OLLAMA_CHAT_GUARD_H
