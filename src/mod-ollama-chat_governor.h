#ifndef MOD_OLLAMA_CHAT_GOVERNOR_H
#define MOD_OLLAMA_CHAT_GOVERNOR_H

#include "ObjectGuid.h"
#include <string>
#include <vector>
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

// --- open conversations ---------------------------------------------------

// A bot that has already answered someone is in a conversation with them, and
// the next thing that person says is a turn in it rather than ambient chatter.
// This is what makes a bot keep talking after the opening exchange without
// having to be named again every single line.
//
// Recorded per scope, so answering someone in party does not exempt this bot
// from pacing in General. Only ever recorded against a real player.
void Governor_NoteConversation(ObjectGuid botGuid, ObjectGuid playerGuid,
                               const std::string& scopeKey);
bool Governor_InConversation(ObjectGuid botGuid, ObjectGuid playerGuid,
                             const std::string& scopeKey);

// --- cooldowns and rate limits -------------------------------------------

// Checks per-bot cooldown, per-scope cooldown, scope rate and global rate.
// On success the send is reserved (all counters advance), so call this exactly
// once per message actually being committed.
// `directAddress` marks a reply the bot owes someone who spoke straight to it
// -- a whisper. Those skip the per-bot and per-scope pacing cooldowns, which
// exist to stop ambient chatter running hot and have no business silencing an
// answer to a direct question. The global messages-per-minute ceiling still
// applies: that one protects the LLM backend, not the pacing.
bool Governor_TryConsumeSend(ObjectGuid botGuid, const std::string& scopeKey,
                             bool directAddress = false);

// Same checks without reserving, for deciding whether to spend an LLM call.
bool Governor_CanSend(ObjectGuid botGuid, const std::string& scopeKey,
                      bool directAddress = false);

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

// Per-bot pacing state for the monitor addon. Seconds are -1 for "never".
struct GovernorConversation
{
    uint64_t    playerGuid = 0;
    std::string scopeKey;
    double      secondsAgo = 0.0;
};
struct GovernorBotDebug
{
    double   sinceSend  = -1.0;
    double   sinceEvent = -1.0;
    uint32_t remembered = 0;   // utterances held for the repetition check
    std::vector<GovernorConversation> conversations;
};
bool Governor_GetBotDebug(ObjectGuid bot, GovernorBotDebug& out);

#endif // MOD_OLLAMA_CHAT_GOVERNOR_H
