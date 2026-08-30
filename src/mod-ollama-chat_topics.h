#ifndef MOD_OLLAMA_CHAT_TOPICS_H
#define MOD_OLLAMA_CHAT_TOPICS_H

#include "ObjectGuid.h"
#include <string>
#include <cstdint>

class Player;
struct OllamaWorldSnapshot;

// --------------------------------------------------------------------------
// Topic engine.
//
// The old random-chatter pool was thirteen topics picked uniformly, seven of
// which were about the bot's own inventory and spellbook, and none of which
// were about the people standing next to it. That is why bots sounded like
// they were reading their character sheet aloud.
//
// This replaces it with weighted categories and per-bot recency suppression:
//
//   People   (30) -- who is here, what they are doing, what they just did
//   World    (30) -- what is around: creatures, NPCs by role, landmarks, time
//   Activity (25) -- quests, group needs, danger
//   Self     (15) -- the original inventory/spell topics, kept but demoted
//
// Weights are config, so a server that liked the old behaviour can put Self
// back up to 100.
// --------------------------------------------------------------------------

enum class TopicCategory : uint8_t
{
    People = 0,
    World,
    Activity,
    Self,
    Guild,
    Count
};

struct TopicPick
{
    bool          valid    = false;
    TopicCategory category = TopicCategory::Self;
    std::string   key;          // provider id, for recency suppression
    std::string   text;         // the formatted prompt fragment
    bool          isGuildTopic = false;
};

// Choose a topic for this bot right now. World thread only -- reads live state.
//
// Categories are gathered LAZILY: one is chosen by weight first, and only that
// category's gatherer runs. Gathering everything up front meant a grid search,
// a quest-status walk, a spell-map walk and a bag walk on every ambient line,
// four fifths of which was thrown away.
TopicPick Topics_Pick(Player* bot, const OllamaWorldSnapshot& world);

// Record that a bot used a topic, so it is suppressed for the next few picks.
void Topics_NoteUsed(ObjectGuid botGuid, const std::string& key);

// --- witnessed event memory ----------------------------------------------
//
// The thing that makes a bot sound like it is playing WITH you rather than
// reciting facts about itself: a short memory of what it just saw happen.

void Topics_NoteWitnessedEvent(Player* witness, const std::string& text);

// Broadcast an event to every bot near the actor. Called from the event hooks.
void Topics_BroadcastEventToNearby(Player* actor, const std::string& text, float radius);

void Topics_ForgetBot(ObjectGuid botGuid);
void Topics_Update();     // expire old memories

#endif // MOD_OLLAMA_CHAT_TOPICS_H
