#ifndef MOD_OLLAMA_CHAT_DISPATCH_H
#define MOD_OLLAMA_CHAT_DISPATCH_H

#include "mod-ollama-chat_capability.h"
#include "mod-ollama-chat_handler.h"

#include "ObjectGuid.h"
#include <deque>
#include <functional>
#include <string>
#include <cstdint>

class Player;
class Channel;

// --------------------------------------------------------------------------
// Central dispatcher.
//
// THE RULE THIS FILE EXISTS TO ENFORCE:
//   worker threads do HTTP and string work only.
//   every read or write of a Player, Channel, Guild, Group or Map happens on
//   the world thread.
//
// Previously each candidate bot spawned its own detached std::thread which
// then called ObjectAccessor, Channel::Say, botAI->Say and re-entered the
// whole eligibility scan -- all racing Map::Update, and all unbounded.
//
// Now: the caller builds the prompt on the world thread and submits. A fixed
// pool of workers performs the generation. Completions come back through a
// queue that OllamaDispatch_Update() drains on the world thread, which is the
// only place delivery happens.
// --------------------------------------------------------------------------

// Resolve the zone-scoped instance of a numbered channel (General, Trade, ...)
// for this bot.
//
// ChannelMgr keys channels by their FULL name, and zone channels are named
// "General - Elwynn Forest", not "General" -- so GetChannel("General") always
// returns nullptr. Matching is by channel id plus zone-name containment, the
// same way PlayerbotAI::SayToChannel does it.
//
// World thread only.
Channel* OllamaResolveZoneChannel(Player* bot, uint32_t chatChannelId);

// Is this guid a member of this exact channel instance?
//
// The playerbots fork dropped Player::IsInChannel, and the core keeps
// Channel::IsOn private with no public replacement. This reaches IsOn itself
// so membership stays exact, including custom (unnumbered) channels.
//
// World thread only.
bool OllamaIsOnChannel(Channel const* channel, ObjectGuid guid);

struct OllamaChatRequest
{
    // Who speaks, and to whom.
    uint64_t botGuid    = 0;
    uint64_t targetGuid = 0;      // 0 when there is no specific addressee

    // Where the line goes.
    ChatChannelSourceLocal source = SRC_SAY_LOCAL;
    std::string            channelName;
    uint32_t               channelId = 0;

    // Loop control. Human-originated messages start at depth 0; each bot reply
    // to a bot increments it.
    uint8_t     chainDepth = 0;
    std::string scopeKey;

    // This line was aimed at this bot in particular -- whispered, named, or
    // said in a small party it belongs to -- rather than said to the room.
    // Decided on the world thread at submit time, where the Group is live, and
    // carried here so delivery does not have to guess it back from `source`.
    // Direct address skips the pacing cooldowns and repetition suppression.
    bool directAddress = false;

    // Generation.
    std::string       prompt;
    OllamaRequestKind kind = OllamaRequestKind::ChatReply;

    // Resolved on the world thread before submission so the worker never has
    // to touch a Player to know these.
    std::string botName;
    std::string originMessage;    // the message being replied to, if any

    // Post-delivery behaviour.
    bool triggerBotReplies = true;   // let other bots hear this line
    bool recordHistory     = false;  // append to conversation history
    bool updateSentiment   = false;  // run sentiment analysis on originMessage
};

// Submit a request. Returns false when the queue is at MaxQueueDepth, in which
// case the caller should simply skip this bot rather than pile up backlog.
bool OllamaDispatch_Submit(OllamaChatRequest request);

// Drain finished generations and deliver them. World thread only.
void OllamaDispatch_Update(uint32_t diff);

void OllamaDispatch_Start();
void OllamaDispatch_Stop();

// Fire-and-forget sentiment analysis. Runs entirely on a worker: the sentiment
// store is mutex-guarded in-memory state plus async DB writes, so it never
// needs the world thread -- and it must never run on it, because the analysis
// is a blocking HTTP call.
void OllamaDispatch_SubmitSentiment(uint64_t botGuid, uint64_t playerGuid,
                                    const std::string& message);

// Distil a bot's accumulated history into lasting memories, then clear it.
// Fire-and-forget; runs entirely on a worker.
void OllamaDispatch_SubmitCondensation(uint64_t botGuid, const std::string& prompt);

// Write or revise how a bot feels about someone.
void OllamaDispatch_SubmitRelationship(uint64_t botGuid, uint64_t otherGuid,
                                       const std::string& otherName,
                                       const std::string& prompt);

// Run arbitrary background work on a dispatcher worker -- for features that
// own their own result handling (the autopilot planner). The job must obey
// the worker rules: HTTP and string work only, no Player/Map/Channel access,
// no config string reads. Best-effort like sentiment: refused (returns false)
// when the queue is past half depth, so it never crowds out chat.
bool OllamaDispatch_SubmitJob(std::function<void()> job);

// Would SubmitJob accept a job right now? Lets a caller skip building an
// expensive request that would only be refused.
bool OllamaDispatch_BackgroundHasRoom();

// A player emoted at a bot and the bot decided to answer in words.
// World thread only.
void OllamaChat_DispatchEmoteReaction(Player* bot, Player* player, uint32_t textEmote);

struct OllamaDispatchStats
{
    uint32_t queuedRequests;
    uint32_t inFlight;
    uint32_t pendingDeliveries;
    uint32_t workers;
    uint64_t totalSubmitted;
    uint64_t totalDelivered;
    uint64_t totalDroppedQueueFull;
    uint64_t totalDroppedEmpty;
    uint64_t totalDroppedGovernor;
    uint64_t totalFailed;
    std::string lastError;
};
OllamaDispatchStats OllamaDispatch_GetStats();

// Debug trace for the monitor addon: what a bot's chat replies came to, kept
// for the most recently active few hundred bots. Any thread.
struct OllamaChatTraceLine
{
    uint32_t    at = 0;            // Unix seconds
    uint8_t     source = 0;        // ChatChannelSourceLocal
    std::string channelName;
    uint64_t    targetGuid = 0;
    uint8_t     chainDepth = 0;
    std::string heard;             // the message it answered, if any
    std::string text;              // what it said (or would have said)
    std::string outcome;           // "said", "repetitive", "cooldown", "failed: ...", ...
};
struct OllamaChatTrace
{
    std::deque<OllamaChatTraceLine> lines;   // oldest first
    std::string lastPrompt;
    std::string lastRaw;
    uint32_t    lastAt = 0;
    uint64_t    lastLatencyMs = 0;
};
bool OllamaDispatch_GetTrace(uint64_t botGuid, OllamaChatTrace& out);

// Chat requests for this bot still queued or waiting to be delivered.
uint32_t OllamaDispatch_PendingFor(uint64_t botGuid);

#endif // MOD_OLLAMA_CHAT_DISPATCH_H
