#ifndef MOD_OLLAMA_CHAT_MEMORY_H
#define MOD_OLLAMA_CHAT_MEMORY_H

#include "ObjectGuid.h"
#include <string>
#include <vector>
#include <cstdint>

class Player;

// --------------------------------------------------------------------------
// Long-term memory and relationships.
//
// Conversation history alone is a sliding window: once a line falls out of it,
// the bot has no idea it ever happened. That is why bots feel like they meet
// you fresh every session no matter how much you have talked.
//
// Two mechanisms fix that, both bounded so the prompt never grows without
// limit:
//
//   MEMORY        History accumulates until it crosses a token budget. At that
//                 point it is condensed by the model into a handful of short
//                 narrator-style memories, each with an importance score, and
//                 the raw history is cleared. At prompt-build time the most
//                 important memories are selected within a separate, smaller
//                 token budget.
//
//   RELATIONSHIPS When a name comes up often enough in a bot's history, the
//                 model is asked to write (or revise) a sentence on how that
//                 bot feels about that person. That sentence goes into future
//                 prompts, so a bot's attitude persists and evolves instead of
//                 resetting.
//
// Both work in normal mode and roleplay mode; roleplay mode simply asks for
// in-character phrasing and allows a larger budget.
//
// Sentiment tracking (a single number) is complementary and unchanged: this is
// the qualitative half.
// --------------------------------------------------------------------------

struct BotMemoryEntry
{
    std::string text;
    uint8_t     importance = 5;   // 1..10
    uint64_t    createdAt  = 0;   // unix seconds
};

struct BotRelationship
{
    uint64_t    otherGuid = 0;
    std::string otherName;
    std::string description;
    uint32_t    mentions  = 0;
    uint64_t    updatedAt = 0;
};

void Memory_Load();
void Memory_SaveAll();
void Memory_ForgetBot(ObjectGuid botGuid);

// Called on the world thread after a bot exchange is recorded. Counts name
// mentions and, when a threshold is crossed, queues a condensation or a
// relationship update through the dispatcher.
void Memory_NoteExchange(uint64_t botGuid, uint64_t otherGuid,
                         const std::string& otherName,
                         const std::string& incomingMessage,
                         const std::string& botReply);

// Prompt fragments. World thread only.
//
// `about` may be null; when set, that person's relationship line is listed
// first so the bot's attitude to whoever it is talking to always survives the
// budget.
std::string Memory_BuildPromptSection(Player* bot, Player* about);

// --- worker-side entry points (called from the dispatcher) ----------------

// Condense a bot's accumulated history into memories, then clear the history.
void Memory_RunCondensation(uint64_t botGuid, const std::string& prompt);

// Write or revise how a bot feels about someone.
void Memory_RunRelationshipUpdate(uint64_t botGuid, uint64_t otherGuid,
                                  const std::string& otherName,
                                  const std::string& prompt);

// --- prompt builders (world thread; they read config templates) -----------

std::string Memory_BuildCondensationPrompt(Player* bot);
std::string Memory_BuildRelationshipPrompt(Player* bot, uint64_t otherGuid,
                                           const std::string& otherName);

// Rough token estimate. Deliberately cheap: characters / 4, the usual
// approximation, which is accurate enough for a budget.
uint32_t Memory_EstimateTokens(const std::string& text);

#endif // MOD_OLLAMA_CHAT_MEMORY_H
