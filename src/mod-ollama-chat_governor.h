#ifndef MOD_OLLAMA_CHAT_GOVERNOR_H
#define MOD_OLLAMA_CHAT_GOVERNOR_H

#include "ObjectGuid.h"
#include <string>
#include <cstdint>

// --------------------------------------------------------------------------
// Conversation governor.
//
// Four independent brakes, because bot chat fails in four different ways:
//
//   1. Chain depth + decay  -- stops a bot->bot reply chain running away.
//   2. The audience rule    -- bots only answer other bots while a real player
//                              has spoken in that scope recently.
//   3. Cooldowns and rates  -- stops one bot, or one crowd, dominating.
//   4. Repetition scoring   -- stops the same line, and the same opener,
//                              being said twice.
//
// All entry points are mutex-guarded; the dispatcher consults some of these
// from worker threads.
// --------------------------------------------------------------------------

// Build the key that identifies a conversation space. Channel messages key on
// the channel name, guild chat on the guild id, party/raid on the group, and
// say/yell on the zone so that proximity chat in one zone does not rate-limit
// another.
std::string Governor_MakeScopeKey(const char* sourceName, uint32_t channelId,
                                  const std::string& channelName,
                                  uint32_t guildId, uint32_t groupOrZoneId);

// --- the audience rule ----------------------------------------------------

void Governor_NoteHumanMessage(const std::string& scopeKey);
bool Governor_HasRecentHuman(const std::string& scopeKey);

// --- chain depth ----------------------------------------------------------

bool     Governor_ChainDepthAllowed(uint8_t depth);
uint32_t Governor_ApplyChainDecay(uint32_t baseChancePct, uint8_t depth);

// --- cooldowns and rate limits -------------------------------------------

// Checks per-bot cooldown, per-scope cooldown, scope rate and global rate.
// On success the send is reserved (all counters advance), so call this exactly
// once per message actually being committed.
bool Governor_TryConsumeSend(ObjectGuid botGuid, const std::string& scopeKey);

// Same checks without reserving, for deciding whether to spend an LLM call.
bool Governor_CanSend(ObjectGuid botGuid, const std::string& scopeKey);

// --- repetition -----------------------------------------------------------

// True when text is too close to something this bot said recently, or to
// something recently said in this scope, or reuses a recent opener.
bool Governor_IsRepetitive(ObjectGuid botGuid, const std::string& scopeKey,
                           const std::string& text);

void Governor_RecordUtterance(ObjectGuid botGuid, const std::string& scopeKey,
                              const std::string& text);

// Similarity in [0,1]; exposed for the .ollama status command and testing.
float Governor_Similarity(const std::string& a, const std::string& b);

// --- per-feature debounces -----------------------------------------------

bool Governor_TryConsumeEmoteReaction(ObjectGuid botGuid, ObjectGuid playerGuid);

// Replaces the old raw-Player*-keyed event cooldown map. Only consumes the
// cooldown when the event is actually committed.
bool Governor_TryConsumeEventCooldown(ObjectGuid botGuid);

// --- maintenance ----------------------------------------------------------

void Governor_OnPlayerLogout(ObjectGuid guid);
void Governor_Update();   // prune expired state; called from the world tick
void Governor_Reset();

// Snapshot for the .ollama status command.
struct GovernorStats
{
    uint32_t trackedBots;
    uint32_t trackedScopes;
    uint32_t sendsLastMinute;
    uint32_t blockedCooldown;
    uint32_t blockedRate;
    uint32_t blockedRepetition;
    uint32_t blockedChainDepth;
    uint32_t blockedNoAudience;
};
GovernorStats Governor_GetStats();

#endif // MOD_OLLAMA_CHAT_GOVERNOR_H
