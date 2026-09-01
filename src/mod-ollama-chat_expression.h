#ifndef MOD_OLLAMA_CHAT_EXPRESSION_H
#define MOD_OLLAMA_CHAT_EXPRESSION_H

#include "ObjectGuid.h"
#include "ScriptMgr.h"
#include <string>
#include <cstdint>

class Player;

// --------------------------------------------------------------------------
// Bot embodiment: facing, gestures, and reactions to inbound emotes.
//
// THREADING: ScheduleBotExpression() and BotPlayTextEmote() mutate world state
// and must only be called from the world/map thread. The dispatcher delivers
// replies there, so the delivery path may call them directly. Never call them
// from an LLM worker thread -- note that EventProcessor::AddEvent is itself
// not thread-safe, so routing through bot->m_Events does not make an off-thread
// call safe.
// --------------------------------------------------------------------------

// Parse and remove a gesture tag from an LLM reply. Recognises the tag form
// the prompt asks for plus the two forms models emit unprompted anyway:
//
//   [emote:wave]      the documented form
//   *waves*           roleplay asterisks
//   /wave             a literal slash command
//
// Unrecognised tags are stripped from the text rather than left in the chat
// line. Returns the TEXT_EMOTE_* id, or 0 when none was found.
uint32_t ExtractEmoteTag(std::string& response);

// The gesture sentence appended to a chat prompt.
//
// This used to be a fixed string naming the same four tags every time --
// wave, nod, shrug, laugh -- against a table that understands about 150 names
// across seven moods. A model reaches for the example it was shown, so
// greetings always waved and three whole categories (sorrow, hostility,
// curiosity) effectively never fired.
//
// Returns four tags drawn from four different moods, varied per call, and says
// outright that any common emote name works so the list reads as examples
// rather than as the menu.
//
// Returns "" when emotes are off, or when the GestureChance roll says this
// particular line goes without one -- so callers append it unconditionally and
// the "should there be a gesture at all" decision lives in exactly one place.
std::string Expression_BuildGesturePrompt();

// Resolve an emote name ("wave", "waves", "waving") to TEXT_EMOTE_*. 0 if unknown.
uint32_t LookupTextEmoteId(const std::string& name);

// Reverse lookup, for describing an inbound emote to the model in words.
// Returns an empty string for ids not in the table.
std::string LookupTextEmoteName(uint32_t id);

// Play a real text emote from the bot, targeting targetGuid (may be empty).
// Routes through WorldSession::HandleTextEmoteOpcode so the animation, the
// localized SMSG_TEXT_EMOTE broadcast, per-race/gender variant numbering,
// achievement criteria and CreatureAI::ReceiveEmote all behave exactly as they
// do for a real player. World thread only.
void BotPlayTextEmote(Player* bot, uint32_t textEmoteId, ObjectGuid targetGuid);

// Queue "turn toward target, then gesture" on the bot's own EventProcessor,
// which Player::Update ticks on the map thread. World thread only.
void ScheduleBotExpression(Player* bot, ObjectGuid targetGuid,
                           uint32_t textEmoteId, uint32_t delayMs);

// True when turning the bot would not fight movement, combat or transport state.
bool IsSafeForFacing(Player* bot);

// Reaction tables for inbound emotes.
uint32_t GetMirrorEmote(uint32_t inbound);   // wave -> wave, bow -> bow
uint32_t GetCounterEmote(uint32_t inbound);  // flex -> laugh, rude -> glare

// Reacts when a player emotes at a bot.
class ChatOnEmote : public PlayerScript
{
public:
    ChatOnEmote();
    void OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 emoteNum,
                           ObjectGuid guid) override;
};

#endif // MOD_OLLAMA_CHAT_EXPRESSION_H
