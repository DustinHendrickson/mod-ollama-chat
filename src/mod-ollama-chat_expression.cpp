#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_dispatch.h"

#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "WorldPacket.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <algorithm>
#include <cctype>
#include <vector>
#include <string>
#include <unordered_map>

namespace
{
    std::string ToLower(std::string v)
    {
        for (char& c : v)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    }

    // Trim a trailing verb inflection so "waves"/"waving" both resolve to "wave".
    std::string NormalizeEmoteWord(const std::string& raw)
    {
        std::string s = ToLower(raw);

        // Drop anything that isn't a letter (punctuation, digits, spaces).
        std::string letters;
        letters.reserve(s.size());
        for (char c : s)
            if (std::isalpha(static_cast<unsigned char>(c)))
                letters.push_back(c);

        if (letters.size() > 4 && letters.compare(letters.size() - 3, 3, "ing") == 0)
            return letters.substr(0, letters.size() - 3);
        if (letters.size() > 3 && letters.back() == 's')
            return letters.substr(0, letters.size() - 1);

        return letters;
    }

    const std::unordered_map<std::string, uint32_t>& EmoteNameTable()
    {
        static const std::unordered_map<std::string, uint32_t> table = {
            // Greetings and courtesy
            { "wave", TEXT_EMOTE_WAVE },           { "hello", TEXT_EMOTE_HELLO },
            { "hi", TEXT_EMOTE_HELLO },            { "greet", TEXT_EMOTE_GREET },
            { "hail", TEXT_EMOTE_HAIL },           { "welcome", TEXT_EMOTE_WELCOME },
            { "bow", TEXT_EMOTE_BOW },             { "curtsey", TEXT_EMOTE_CURTSEY },
            { "salute", TEXT_EMOTE_SALUTE },       { "thank", TEXT_EMOTE_THANK },
            { "bye", TEXT_EMOTE_BYE },             { "farewell", TEXT_EMOTE_BYE },
            { "introduce", TEXT_EMOTE_INTRODUCE }, { "congratulate", TEXT_EMOTE_CONGRATULATE },
            { "grats", TEXT_EMOTE_CONGRATULATE },  { "praise", TEXT_EMOTE_PRAISE },
            { "commend", TEXT_EMOTE_COMMEND },     { "highfive", TEXT_EMOTE_HIGHFIVE },
            { "goodluck", TEXT_EMOTE_GOODLUCK },   { "encourage", TEXT_EMOTE_ENCOURAGE },
            { "toast", TEXT_EMOTE_TOAST },

            // Agreement / disagreement
            { "nod", TEXT_EMOTE_NOD },             { "agree", TEXT_EMOTE_AGREE },
            { "yes", TEXT_EMOTE_AGREE },           { "no", TEXT_EMOTE_NO },
            { "disagree", TEXT_EMOTE_DISAGREE },   { "shake", TEXT_EMOTE_SHAKE },
            { "shrug", TEXT_EMOTE_SHRUG },         { "veto", TEXT_EMOTE_VETO },
            { "doubt", TEXT_EMOTE_DOUBT },         { "ready", TEXT_EMOTE_READY },

            // Amusement
            { "laugh", TEXT_EMOTE_LAUGH },         { "chuckle", TEXT_EMOTE_CHUCKLE },
            { "giggle", TEXT_EMOTE_GIGGLE },       { "cackle", TEXT_EMOTE_CACKLE },
            { "guffaw", TEXT_EMOTE_GUFFAW },       { "rofl", TEXT_EMOTE_ROFL },
            { "snicker", TEXT_EMOTE_SNICKER },     { "smirk", TEXT_EMOTE_SMIRK },
            { "grin", TEXT_EMOTE_GRIN },           { "smile", TEXT_EMOTE_SMILE },
            { "joke", TEXT_EMOTE_JOKE },           { "jk", TEXT_EMOTE_JK },
            { "tease", TEXT_EMOTE_TEASE },         { "wink", TEXT_EMOTE_WINK },
            { "happy", TEXT_EMOTE_HAPPY },         { "cheer", TEXT_EMOTE_CHEER },
            { "applaud", TEXT_EMOTE_APPLAUD },     { "clap", TEXT_EMOTE_CLAP },
            { "golfclap", TEXT_EMOTE_GOLFCLAP },   { "dance", TEXT_EMOTE_DANCE },
            { "flex", TEXT_EMOTE_FLEX },           { "victory", TEXT_EMOTE_VICTORY },
            { "gloat", TEXT_EMOTE_GLOAT },

            // Distress and sorrow
            { "cry", TEXT_EMOTE_CRY },             { "sigh", TEXT_EMOTE_SIGH },
            { "mourn", TEXT_EMOTE_MOURN },         { "groan", TEXT_EMOTE_GROAN },
            { "whine", TEXT_EMOTE_WHINE },         { "moan", TEXT_EMOTE_MOAN },
            { "panic", TEXT_EMOTE_PANIC },         { "cower", TEXT_EMOTE_COWER },
            { "cringe", TEXT_EMOTE_CRINGE },       { "plead", TEXT_EMOTE_PLEAD },
            { "beg", TEXT_EMOTE_BEG },             { "grovel", TEXT_EMOTE_GROVEL },
            { "apologize", TEXT_EMOTE_APOLOGIZE }, { "sorry", TEXT_EMOTE_APOLOGIZE },
            { "comfort", TEXT_EMOTE_COMFORT },     { "soothe", TEXT_EMOTE_SOOTHE },
            { "pity", TEXT_EMOTE_PITY },           { "scared", TEXT_EMOTE_SCARED },
            { "surrender", TEXT_EMOTE_SURRENDER }, { "helpme", TEXT_EMOTE_HELPME },
            { "faint", TEXT_EMOTE_FAINT },         { "facepalm", TEXT_EMOTE_FACEPALM },
            { "blame", TEXT_EMOTE_BLAME },         { "fail", TEXT_EMOTE_FAIL },
            { "embarrass", TEXT_EMOTE_EMBARRASS }, { "blush", TEXT_EMOTE_BLUSH },
            { "bashful", TEXT_EMOTE_BASHFUL },     { "shy", TEXT_EMOTE_SHY },

            // Hostility
            { "angry", TEXT_EMOTE_ANGRY },         { "glare", TEXT_EMOTE_GLARE },
            { "growl", TEXT_EMOTE_GROWL },         { "snarl", TEXT_EMOTE_SNARL },
            { "roar", TEXT_EMOTE_ROAR },           { "threaten", TEXT_EMOTE_THREATEN },
            { "taunt", TEXT_EMOTE_TAUNT },         { "mock", TEXT_EMOTE_MOCK },
            { "insult", TEXT_EMOTE_INSULT },       { "rude", TEXT_EMOTE_RUDE },
            { "spit", TEXT_EMOTE_SPIT },           { "snub", TEXT_EMOTE_SNUB },
            { "challenge", TEXT_EMOTE_CHALLENGE }, { "brandish", TEXT_EMOTE_BRANDISH },
            { "enemy", TEXT_EMOTE_ENEMY },         { "charge", TEXT_EMOTE_CHARGE },
            { "slap", TEXT_EMOTE_SLAP },           { "shoo", TEXT_EMOTE_SHOO },
            { "stare", TEXT_EMOTE_STARE },         { "frown", TEXT_EMOTE_FROWN },
            { "glower", TEXT_EMOTE_GLOWER },

            // Thought and attention
            { "ponder", TEXT_EMOTE_PONDER },       { "think", TEXT_EMOTE_PONDER },
            { "confused", TEXT_EMOTE_CONFUSED },   { "puzzle", TEXT_EMOTE_PUZZLE },
            { "boggle", TEXT_EMOTE_BOGGLE },       { "curious", TEXT_EMOTE_CURIOUS },
            { "peer", TEXT_EMOTE_PEER },           { "listen", TEXT_EMOTE_LISTEN },
            { "point", TEXT_EMOTE_POINT },         { "beckon", TEXT_EMOTE_BECKON },
            { "talk", TEXT_EMOTE_TALK },           { "amaze", TEXT_EMOTE_AMAZE },
            { "awe", TEXT_EMOTE_AWE },             { "gasp", TEXT_EMOTE_GASP },
            { "surprised", TEXT_EMOTE_SURPRISED }, { "eyebrow", TEXT_EMOTE_EYEBROW },
            { "lost", TEXT_EMOTE_LOST },           { "serious", TEXT_EMOTE_SERIOUS },
            { "badfeeling", TEXT_EMOTE_BADFEELING },

            // States and needs
            { "tired", TEXT_EMOTE_TIRED },         { "yawn", TEXT_EMOTE_YAWN },
            { "sleep", TEXT_EMOTE_SLEEP },         { "bored", TEXT_EMOTE_BORED },
            { "hungry", TEXT_EMOTE_HUNGRY },       { "thirsty", TEXT_EMOTE_THIRSTY },
            { "cold", TEXT_EMOTE_COLD },           { "drink", TEXT_EMOTE_DRINK },
            { "eat", TEXT_EMOTE_EAT },             { "oom", TEXT_EMOTE_OOM },
            { "healme", TEXT_EMOTE_HEALME },       { "work", TEXT_EMOTE_WORK },
            { "pray", TEXT_EMOTE_PRAY },           { "kneel", TEXT_EMOTE_KNEEL },
            { "stand", TEXT_EMOTE_STAND },         { "sit", TEXT_EMOTE_SIT },
            { "calm", TEXT_EMOTE_CALM },           { "brb", TEXT_EMOTE_BRB },
            { "wait", TEXT_EMOTE_WAIT },           { "follow", TEXT_EMOTE_FOLLOW },
            { "flee", TEXT_EMOTE_FLEE },           { "incoming", TEXT_EMOTE_INCOMING },
            { "cough", TEXT_EMOTE_COUGH },         { "whistle", TEXT_EMOTE_WHISTLE },
            { "shiver", TEXT_EMOTE_SHIVER },       { "scratch", TEXT_EMOTE_SCRATCH },
            { "crack", TEXT_EMOTE_CRACK },         { "raise", TEXT_EMOTE_RAISE },
            { "ding", TEXT_EMOTE_DING },
        };
        return table;
    }

    // A one-shot event so the gesture lands just after the chat line rather
    // than at the same instant, which reads as mechanical.
    class BotExpressionEvent : public BasicEvent
    {
    public:
        BotExpressionEvent(ObjectGuid botGuid, ObjectGuid targetGuid, uint32_t emoteId)
            : _botGuid(botGuid), _targetGuid(targetGuid), _emoteId(emoteId) { }

        bool Execute(uint64 /*time*/, uint32 /*diff*/) override
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(_botGuid);
            if (!bot || !bot->IsInWorld())
                return true;

            Player* target = ObjectAccessor::FindConnectedPlayer(_targetGuid);
            const bool targetUsable = target && target->IsInWorld() &&
                                      target->GetMapId() == bot->GetMapId();

            if (g_EnableBotFacing && targetUsable && IsSafeForFacing(bot))
            {
                if (g_BotFacingMaxDistance <= 0.0f ||
                    bot->GetDistance(target) <= g_BotFacingMaxDistance)
                {
                    bot->SetFacingToObject(target);
                }
            }

            if (_emoteId && g_EnableBotEmotes)
                BotPlayTextEmote(bot, _emoteId, targetUsable ? target->GetGUID() : ObjectGuid::Empty);

            return true;
        }

    private:
        ObjectGuid _botGuid;
        ObjectGuid _targetGuid;
        uint32_t   _emoteId;
    };
}

// --------------------------------------------------------------------------

namespace
{
    // Curated per mood: names a model reaches for naturally and that read as a
    // real WoW gesture. Deliberately a sample, not the whole table -- pasting
    // 150 tags into every prompt is how bots end up talking about their emote
    // list instead of using it.
    const std::vector<std::vector<const char*>>& GestureSamples()
    {
        static const std::vector<std::vector<const char*>> samples = {
            { "wave", "bow", "salute", "greet", "thank" },
            { "nod", "agree", "shrug", "no", "shake" },
            { "laugh", "grin", "smile", "chuckle", "cheer" },
            { "sigh", "cry", "apologize", "comfort", "facepalm" },
            { "glare", "frown", "growl", "mock", "taunt" },
            { "ponder", "point", "listen", "curious", "peer" },
            { "yawn", "bored", "tired", "whistle", "pray" },
        };
        return samples;
    }
}

std::string Expression_BuildGesturePrompt()
{
    const auto& samples = GestureSamples();

    // Shuffle the moods, then take one tag from each of the first few, so the
    // examples are never all the same flavour.
    std::vector<size_t> order(samples.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;

    for (size_t i = order.size(); i-- > 1; )
        std::swap(order[i], order[urand(0, static_cast<uint32>(i))]);

    const size_t take = std::min<size_t>(4, order.size());

    std::string tags;
    for (size_t i = 0; i < take; ++i)
    {
        const auto& bucket = samples[order[i]];
        const char* name   = bucket[urand(0, static_cast<uint32>(bucket.size() - 1))];

        if (!tags.empty())
            tags += (i + 1 == take) ? " or " : ", ";

        tags += "[emote:";
        tags += name;
        tags += "]";
    }

    return " You may end your reply with a single gesture tag such as " + tags +
           " when one genuinely fits -- any common emote name works. Omit it otherwise.";
}

uint32_t LookupTextEmoteId(const std::string& name)
{
    const std::string key = NormalizeEmoteWord(name);
    if (key.empty())
        return 0;

    const auto& table = EmoteNameTable();
    auto it = table.find(key);
    return it == table.end() ? 0u : it->second;
}

std::string LookupTextEmoteName(uint32_t id)
{
    if (!id)
        return "";

    // The DBC carries no usable name string for text emotes, so reverse the
    // same table the parser uses. First match wins, which is fine because the
    // aliases all describe the same gesture.
    for (const auto& [name, value] : EmoteNameTable())
        if (value == id)
            return name;

    return "";
}

uint32_t ExtractEmoteTag(std::string& response)
{
    uint32_t found = 0;

    auto consider = [&found](const std::string& word)
    {
        if (!found)
            found = LookupTextEmoteId(word);
    };

    // ---- form 1: [emote:name] -------------------------------------------
    static const std::string open = "[emote:";
    for (;;)
    {
        size_t start = response.find(open);
        if (start == std::string::npos)
            break;
        size_t close = response.find(']', start);
        if (close == std::string::npos)
        {
            response.erase(start);
            break;
        }

        const size_t nameStart = start + open.size();
        consider(response.substr(nameStart, close - nameStart));
        response.erase(start, close - start + 1);
    }

    // ---- form 2: *waves* -------------------------------------------------
    for (;;)
    {
        size_t start = response.find('*');
        if (start == std::string::npos)
            break;
        size_t close = response.find('*', start + 1);
        if (close == std::string::npos)
        {
            response.erase(start, 1);
            break;
        }

        // Only treat short runs as gestures; a long asterisk span is emphasis.
        const std::string inner = response.substr(start + 1, close - start - 1);
        if (inner.size() <= 24)
            consider(inner);

        response.erase(start, close - start + 1);
    }

    // ---- form 3: a bare /wave, anywhere in the line ----------------------
    // scanFrom advances past non-emote slashes. Without it, a line containing
    // "and/or" or a URL would rescan the same slash forever.
    for (size_t scanFrom = 0; ; )
    {
        const size_t slash = response.find('/', scanFrom);
        if (slash == std::string::npos)
            break;

        size_t end = slash + 1;
        while (end < response.size() &&
               std::isalpha(static_cast<unsigned char>(response[end])))
            ++end;

        const std::string word = response.substr(slash + 1, end - slash - 1);
        if (LookupTextEmoteId(word) == 0)
        {
            // Not an emote -- leave it in the text and look past it.
            scanFrom = slash + 1;
            continue;
        }

        consider(word);
        response.erase(slash, end - slash);
        scanFrom = slash;
    }

    // Tidy up whitespace the removals left behind.
    while (!response.empty() && std::isspace(static_cast<unsigned char>(response.front())))
        response.erase(response.begin());
    while (!response.empty() && std::isspace(static_cast<unsigned char>(response.back())))
        response.pop_back();

    return found;
}

bool IsSafeForFacing(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive())
        return false;
    if (!bot->IsStopped() || bot->IsInCombat())
        return false;
    if (bot->IsInFlight() || bot->GetTransport())
        return false;
    if (bot->IsBeingTeleported())
        return false;
    if (bot->IsNonMeleeSpellCast(false))
        return false;

    MotionMaster* mm = bot->GetMotionMaster();
    return mm && mm->GetMotionSlotType(MOTION_SLOT_CONTROLLED) == NULL_MOTION_TYPE;
}

void BotPlayTextEmote(Player* bot, uint32_t textEmoteId, ObjectGuid targetGuid)
{
    if (!bot || !textEmoteId || !bot->IsInWorld() || !bot->IsAlive())
        return;

    WorldSession* session = bot->GetSession();
    if (!session)
        return;

    // Hand the core a client-shaped packet and let WorldSession do the rest.
    // This is what mod-playerbots' PlayerbotAI::PlayEmote does, and it gets us
    // the animation, the localized broadcast, the correct per-race/gender
    // variant number, achievement criteria and CreatureAI::ReceiveEmote for
    // free -- none of which a hand-built SMSG_TEXT_EMOTE would carry.
    WorldPacket data(CMSG_TEXT_EMOTE, 4 + 4 + 8);
    data << uint32(textEmoteId);
    data << uint32(0);              // emoteNum; the core recomputes the variant
    data << targetGuid;

    session->HandleTextEmoteOpcode(data);
}

void ScheduleBotExpression(Player* bot, ObjectGuid targetGuid,
                           uint32_t textEmoteId, uint32_t delayMs)
{
    if (!bot)
        return;

    const bool wantFacing = g_EnableBotFacing && !targetGuid.IsEmpty();
    const bool wantEmote  = g_EnableBotEmotes && textEmoteId != 0;
    if (!wantFacing && !wantEmote)
        return;

    bot->m_Events.AddEvent(
        new BotExpressionEvent(bot->GetGUID(), targetGuid, textEmoteId),
        bot->m_Events.CalculateTime(delayMs));
}

// --------------------------------------------------------------------------

uint32_t GetMirrorEmote(uint32_t inbound)
{
    switch (inbound)
    {
        case TEXT_EMOTE_WAVE:         return TEXT_EMOTE_WAVE;
        case TEXT_EMOTE_HELLO:        return TEXT_EMOTE_HELLO;
        case TEXT_EMOTE_GREET:        return TEXT_EMOTE_GREET;
        case TEXT_EMOTE_HAIL:         return TEXT_EMOTE_WAVE;
        case TEXT_EMOTE_BOW:          return TEXT_EMOTE_BOW;
        case TEXT_EMOTE_CURTSEY:      return TEXT_EMOTE_BOW;
        case TEXT_EMOTE_SALUTE:       return TEXT_EMOTE_SALUTE;
        case TEXT_EMOTE_NOD:          return TEXT_EMOTE_NOD;
        case TEXT_EMOTE_AGREE:        return TEXT_EMOTE_AGREE;
        case TEXT_EMOTE_CHEER:        return TEXT_EMOTE_CHEER;
        case TEXT_EMOTE_APPLAUD:      return TEXT_EMOTE_CLAP;
        case TEXT_EMOTE_CLAP:         return TEXT_EMOTE_CLAP;
        case TEXT_EMOTE_LAUGH:        return TEXT_EMOTE_CHUCKLE;
        case TEXT_EMOTE_DANCE:        return TEXT_EMOTE_DANCE;
        case TEXT_EMOTE_THANK:        return TEXT_EMOTE_BOW;
        case TEXT_EMOTE_CONGRATULATE: return TEXT_EMOTE_THANK;
        case TEXT_EMOTE_HUG:          return TEXT_EMOTE_HUG;
        case TEXT_EMOTE_HIGHFIVE:     return TEXT_EMOTE_HIGHFIVE;
        case TEXT_EMOTE_TOAST:        return TEXT_EMOTE_TOAST;
        case TEXT_EMOTE_BYE:          return TEXT_EMOTE_WAVE;
        default:                      return 0;
    }
}

uint32_t GetCounterEmote(uint32_t inbound)
{
    switch (inbound)
    {
        case TEXT_EMOTE_FLEX:      return TEXT_EMOTE_LAUGH;
        case TEXT_EMOTE_RUDE:      return TEXT_EMOTE_GLARE;
        case TEXT_EMOTE_SPIT:      return TEXT_EMOTE_ANGRY;
        case TEXT_EMOTE_INSULT:    return TEXT_EMOTE_SNARL;
        case TEXT_EMOTE_MOCK:      return TEXT_EMOTE_FROWN;
        case TEXT_EMOTE_TAUNT:     return TEXT_EMOTE_THREATEN;
        case TEXT_EMOTE_THREATEN:  return TEXT_EMOTE_BRANDISH;
        case TEXT_EMOTE_CHALLENGE: return TEXT_EMOTE_READY;
        case TEXT_EMOTE_POKE:      return TEXT_EMOTE_EYEBROW;
        case TEXT_EMOTE_TICKLE:    return TEXT_EMOTE_GIGGLE;
        case TEXT_EMOTE_KISS:      return TEXT_EMOTE_BLUSH;
        case TEXT_EMOTE_FLIRT:     return TEXT_EMOTE_BASHFUL;
        case TEXT_EMOTE_CRY:       return TEXT_EMOTE_COMFORT;
        case TEXT_EMOTE_MOURN:     return TEXT_EMOTE_COMFORT;
        case TEXT_EMOTE_PLEAD:     return TEXT_EMOTE_PONDER;
        case TEXT_EMOTE_BEG:       return TEXT_EMOTE_SHRUG;
        case TEXT_EMOTE_HELPME:    return TEXT_EMOTE_READY;
        case TEXT_EMOTE_OOM:       return TEXT_EMOTE_NOD;
        case TEXT_EMOTE_PONDER:    return TEXT_EMOTE_SHRUG;
        case TEXT_EMOTE_STARE:     return TEXT_EMOTE_EYEBROW;
        case TEXT_EMOTE_ROAR:      return TEXT_EMOTE_CHEER;
        default:                   return 0;
    }
}

// --------------------------------------------------------------------------

ChatOnEmote::ChatOnEmote() : PlayerScript("ChatOnEmote", { PLAYERHOOK_ON_TEXT_EMOTE }) { }

void ChatOnEmote::OnPlayerTextEmote(Player* player, uint32 textEmote,
                                    uint32 /*emoteNum*/, ObjectGuid guid)
{
    if (!g_Enable || !g_EnableEmoteReactions || !player)
        return;

    if (guid.IsEmpty())
        return;

    // Only real players get a reaction; bot-to-bot emoting would loop.
    if (OllamaIsBotPlayer(player))
        return;

    Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
    if (!bot || bot == player || !bot->IsInWorld())
        return;

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!botAI || !botAI->IsBotAI())
        return;

    if (g_DisableRepliesInCombat && bot->IsInCombat())
        return;

    if (player->GetMapId() != bot->GetMapId())
        return;
    if (g_SayDistance > 0.0f && bot->GetDistance(player) > g_SayDistance)
        return;

    // Debounce per (player, bot) pair so /spam cannot flood the queue.
    if (!Governor_TryConsumeEmoteReaction(bot->GetGUID(), player->GetGUID()))
        return;

    if (urand(1, 100) > g_EmoteReplyChance)
        return;

    // Pick a response style by configured weight.
    const uint32_t total = g_EmoteReplyMirrorWeight + g_EmoteReplyCounterWeight +
                           g_EmoteReplySpeakWeight;
    if (total == 0)
        return;

    uint32_t roll = urand(0, total - 1);
    uint32_t reactEmote = 0;
    bool speak = false;

    if (roll < g_EmoteReplyMirrorWeight)
    {
        reactEmote = GetMirrorEmote(textEmote);
    }
    else if (roll < g_EmoteReplyMirrorWeight + g_EmoteReplyCounterWeight)
    {
        reactEmote = GetCounterEmote(textEmote);
    }
    else
    {
        speak = true;
    }

    // Fall back to a gesture if the table had nothing for this emote.
    if (!speak && !reactEmote)
        reactEmote = GetMirrorEmote(textEmote);
    if (!speak && !reactEmote)
        reactEmote = GetCounterEmote(textEmote);

    if (reactEmote)
    {
        ScheduleBotExpression(bot, player->GetGUID(), reactEmote, g_BotExpressionDelayMs);

        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} reacting to emote {} from {} with emote {}",
                     bot->GetName(), textEmote, player->GetName(), reactEmote);
        return;
    }

    if (speak)
    {
        // Turn toward them regardless, then answer in words via the event path.
        ScheduleBotExpression(bot, player->GetGUID(), 0, g_BotExpressionDelayMs);
        OllamaChat_DispatchEmoteReaction(bot, player, textEmote);
    }
}
