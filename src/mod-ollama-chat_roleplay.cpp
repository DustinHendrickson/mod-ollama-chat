#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    std::mutex g_mutex;
    std::unordered_map<uint8_t, std::string> g_raceVoice;
    std::unordered_map<uint8_t, std::string> g_classVoice;
    std::vector<std::string> g_metaTerms;

    // Defaults. Each entry is register + touchstones + how they refer to the
    // other side -- the things that actually make a line sound like it came
    // from that character rather than from a stat block.
    void SeedRaceDefaults()
    {
        g_raceVoice[RACE_HUMAN] =
            "You speak plainly and directly, like a soldier or a tradesman of Stormwind. "
            "You measure things against Lordaeron's fall and the wars you grew up hearing about. "
            "You call the Horde 'the Horde' or 'orcs', with wariness rather than hatred.";

        g_raceVoice[RACE_DWARF] =
            "You speak bluntly and warmly, fond of ale, stone, and old grudges. "
            "You measure worth in craftsmanship and endurance. You reference Ironforge, the "
            "Explorers' League, and your forebears. You are hard to impress and quick to laugh.";

        g_raceVoice[RACE_NIGHTELF] =
            "You speak formally and a little coolly, as one who has watched centuries pass. "
            "You reference Elune, the forests, and the long sleep of the druids. You find haste "
            "and greed distasteful, and you say so obliquely rather than bluntly.";

        g_raceVoice[RACE_GNOME] =
            "You speak quickly and enthusiastically, prone to technical tangents and optimistic "
            "estimates. You reference gadgets, schematics, Gnomeregan, and calculations nobody "
            "asked for. You are cheerful about danger.";

        g_raceVoice[RACE_DRAENEI] =
            "You speak formally, with a faintly archaic cadence and unusual sentence order. "
            "You reference the Light, the Naaru, and the long exile. You are courteous even to "
            "enemies, and you find casual cruelty genuinely bewildering.";

        g_raceVoice[RACE_ORC] =
            "You speak bluntly, valuing strength, honour and directness. You reference the "
            "Warchief, the old shamanic ways, and the shame of the demon blood. You have no "
            "patience for flattery. You call the Alliance 'the Alliance' or 'humans'.";

        g_raceVoice[RACE_UNDEAD_PLAYER] =
            "You speak with dry, grim humour and a certain detachment from life's urgencies. "
            "You reference the Dark Lady, the Plaguelands, and your own death matter-of-factly. "
            "You call the living 'breathers' or 'the living'. You are rarely sentimental.";

        g_raceVoice[RACE_TAUREN] =
            "You speak slowly and thoughtfully, in measured sentences. You reference the "
            "Earthmother, the balance, the hunt, and the ancestors. You are courteous and "
            "reluctant to give offence, and you dislike waste of any kind.";

        g_raceVoice[RACE_TROLL] =
            "You speak with a distinct rhythmic cadence, dropping the occasional 'mon' and "
            "'ya'. You reference the loa, the old empires, and voodoo. You are superstitious "
            "in a matter-of-fact way and you find most things funnier than they are.";

        g_raceVoice[RACE_BLOODELF] =
            "You speak with polished, slightly haughty precision. You reference Silvermoon, "
            "the Sunwell, and the addiction that followed its loss. You are proud, image-"
            "conscious, and quietly desperate not to look diminished.";
    }

    void SeedClassDefaults()
    {
        g_classVoice[CLASS_WARRIOR] =
            "You notice ground, reach, and who is standing where. You judge people by whether "
            "they will hold a line.";
        g_classVoice[CLASS_PALADIN] =
            "You notice oaths kept and broken, and cowardice. You speak of duty without irony, "
            "which others sometimes find tiresome.";
        g_classVoice[CLASS_HUNTER] =
            "You notice tracks, wind, terrain and beasts before you notice people. You are more "
            "comfortable outdoors and you say so.";
        g_classVoice[CLASS_ROGUE] =
            "You notice exits, purses, and who is watching whom. You rarely volunteer anything "
            "and you deflect direct questions.";
        g_classVoice[CLASS_PRIEST] =
            "You notice wounds, exhaustion and the dying. You have seen a great deal of pain "
            "and it has made you either very gentle or very dry.";
        g_classVoice[CLASS_DEATH_KNIGHT] =
            "You notice what is already dead. You speak curtly, carry obvious guilt, and dislike "
            "being asked about the time you served the Lich King.";
        g_classVoice[CLASS_SHAMAN] =
            "You notice weather, water, stone and the mood of the elements. You talk about them "
            "as though they were people, because to you they are.";
        g_classVoice[CLASS_MAGE] =
            "You notice inefficiency and error, and you correct them unprompted. You are "
            "impatient with slowness and secretly delighted to be asked a question.";
        g_classVoice[CLASS_WARLOCK] =
            "You notice weakness and what it is worth. You speak in terms of costs and bargains, "
            "and you are unbothered by things that unsettle other people.";
        g_classVoice[CLASS_DRUID] =
            "You notice the season, the growth, and what is out of balance. You measure time in "
            "longer units than everyone around you.";
    }

    std::string ToLower(std::string v)
    {
        for (char& c : v)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    }
}

// --------------------------------------------------------------------------

void Roleplay_Load()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    g_raceVoice.clear();
    g_classVoice.clear();
    g_metaTerms.clear();

    SeedRaceDefaults();
    SeedClassDefaults();

    for (const std::string& term : SplitString(g_RoleplayMetaTermList, '|'))
    {
        std::string t = ToLower(term);
        if (!t.empty())
            g_metaTerms.push_back(t);
    }

    // Optional DB overlay so servers can retune voices without a rebuild.
    // Table is created on demand by the module's SQL; absence is not an error.
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT kind, id, prompt FROM mod_ollama_chat_voice"))
    {
        uint32_t loaded = 0;
        do
        {
            Field* fields = result->Fetch();
            const std::string kind = ToLower(fields[0].Get<std::string>());
            const uint8_t     id   = fields[1].Get<uint8>();
            const std::string text = fields[2].Get<std::string>();

            if (text.empty())
                continue;

            if (kind == "race")
                g_raceVoice[id] = text;
            else if (kind == "class")
                g_classVoice[id] = text;
            else
                continue;

            ++loaded;
        } while (result->NextRow());

        if (loaded)
            LOG_INFO("module.ollamachat",
                     "[Ollama Chat] Loaded {} roleplay voice overrides from the database.", loaded);
    }
}

std::string Roleplay_BuildVoicePrompt(Player* bot)
{
    if (!g_RoleplayEnable || !bot)
        return "";

    std::lock_guard<std::mutex> lock(g_mutex);

    std::string out;

    if (g_RoleplayUseRaceVoice)
    {
        auto it = g_raceVoice.find(bot->getRace());
        if (it != g_raceVoice.end())
            out += " " + it->second;
    }

    if (g_RoleplayUseClassVoice)
    {
        auto it = g_classVoice.find(bot->getClass());
        if (it != g_classVoice.end())
            out += " " + it->second;
    }

    if (g_RoleplayStrictness >= 1)
    {
        out += " Stay in character at all times. You are a person living in this world, not a "
               "player of a game: never mention servers, patches, classes as 'specs', damage "
               "numbers, experience points, or anything outside the world itself.";
    }

    if (g_RoleplayStrictness >= 2)
    {
        out += " Describe injuries, distances and dangers the way a person would, never as "
               "figures. Do not break character for any reason.";
    }

    if (g_RoleplayFactionAttitude)
    {
        out += (bot->GetTeamId() == TEAM_ALLIANCE)
                   ? " You are loyal to the Alliance and wary of the Horde."
                   : " You are loyal to the Horde and wary of the Alliance.";
    }

    return out;
}

std::string Roleplay_FilterMetaTerms(const std::string& text)
{
    if (!g_RoleplayEnable || !g_RoleplayBlockMetaTerms || g_RoleplayStrictness < 1)
        return text;

    std::vector<std::string> terms;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        terms = g_metaTerms;
    }
    if (terms.empty())
        return text;

    const std::string lower = ToLower(text);

    for (const std::string& term : terms)
    {
        size_t pos = 0;
        while ((pos = lower.find(term, pos)) != std::string::npos)
        {
            const bool startOk = (pos == 0) ||
                                 !std::isalnum(static_cast<unsigned char>(lower[pos - 1]));
            const size_t endPos = pos + term.size();
            const bool endOk = (endPos >= lower.size()) ||
                               !std::isalnum(static_cast<unsigned char>(lower[endPos]));

            if (startOk && endOk)
            {
                // A whole-word match on an out-of-world term. Rather than
                // mangle the sentence around it, reject the line so the bot
                // stays silent instead of stepping out of character.
                return "";
            }
            pos = endPos;
        }
    }

    return text;
}

std::string Roleplay_DescribeHealth(uint32_t current, uint32_t max)
{
    if (max == 0)
        return "unhurt";

    const float pct = float(current) / float(max);

    if (pct >= 0.99f) return "unhurt";
    if (pct >= 0.75f) return "lightly hurt";
    if (pct >= 0.50f) return "bloodied";
    if (pct >= 0.25f) return "badly wounded";
    if (pct >  0.0f)  return "barely standing";
    return "dead";
}

bool Roleplay_IsLanguageBarrier(Player* speaker, Player* listener)
{
    if (!g_RoleplayEnable || !g_RoleplayCrossFactionGibberish)
        return false;
    if (!speaker || !listener)
        return false;

    // The client renders cross-faction say/yell as gibberish anyway, so a
    // fluent reply is the single most immersion-breaking thing the module can
    // do. Skipping the call fixes the immersion and saves a round trip.
    return speaker->GetTeamId() != listener->GetTeamId();
}

bool Roleplay_UseRoleplayVariations()
{
    return g_RoleplayEnable && g_RoleplayStrictness >= 2 &&
           !g_RoleplayPromptVariations.empty();
}
