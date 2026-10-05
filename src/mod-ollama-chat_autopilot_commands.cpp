#include "mod-ollama-chat_autopilot_commands.h"
#include "mod-ollama-chat_autopilot_strategy.h"
#include "mod-ollama-chat_autopilot_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Config.h"
#include "Creature.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "Trainer.h"

#include "PlayerbotAI.h"

#include <algorithm>
#include <cctype>

namespace
{
    // What the LLM is told it can do. Every playerbots command named here was
    // checked against this fork's chat command handler.
    const char* const kDefaultReference =
        "Commands are what a player whispers to their own bot. Send any number, in order.\n"
        "STRATEGIES (behaviours that stay on until changed):\n"
        "- nc +name,-name : out-of-combat behaviours. Useful: new rpg (live in the world on their own: quests, "
        "camps, hunting spots, NPCs), quest (accept and turn in quests), grind (attack monsters nearby), travel "
        "(go to destinations), explore, gather (herbs, ore, skins), loot, food (eat/drink when low), mount, "
        "lfg (queue for dungeons), bg (queue for battlegrounds), pvp (fight enemy players), duel, start duel, "
        "emote, guild, group (invite people nearby), maintenance (keep spells, talents and gear up to date)\n"
        "- co +name,-name : combat behaviours. Useful: flee (run when losing), potions, avoid aoe, aggressive, "
        "threat (do not pull aggro), kite, save mana. Class and role strategies (tank, heal, dps, aoe...) also "
        "exist; change them only if you are sure.\n"
        "ACTIONS:\n"
        "- quest <id> : work on that quest from the quest log\n"
        "- rpg <status> : focus on one of: do quest, wander npc, go camp, go grind, wander random, travel flight, rest\n"
        "- goto <service> : walk to the nearest repair, vendor, trainer, profession, inn, flightmaster, bank or "
        "auction and use it on arrival (repair and sell junk, learn new spells, bind the hearthstone)\n"
        "- goto zone <zone name> : walk to a zone on this continent\n"
        "- go travel <place> : head for a known destination (needs nc +travel)\n"
        "- talents : spend talent points; autogear : equip the best gear they have; s gray : sell junk to a "
        "vendor nearby; repair : repair at a vendor nearby\n"
        "- follow / stay : follow the group leader or stay put\n";

    const char* const kDefaultDenied =
        "logout,reset,destroy,teleport,summon,cheat,debug,cdebug,wipe,sendmail,mail,hire,give leader,"
        "guild remove,guild demote,guild promote,guild leave,log,d,do,release,leave";

    std::string              g_reference;
    std::vector<std::string> g_denied;

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::string Trim(std::string s)
    {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        return s;
    }

    bool StartsWithWord(const std::string& text, const std::string& word)
    {
        return text == word || (text.size() > word.size() && text.compare(0, word.size(), word) == 0 &&
                                text[word.size()] == ' ');
    }

    bool Denied(const std::string& command)
    {
        return std::any_of(g_denied.begin(), g_denied.end(),
                           [&](const std::string& d) { return StartsWithWord(command, d); });
    }

    // Walking needs NewRpg; the legacy rpg wanderer would fight it.
    void EnsureNewRpg(PlayerbotAI* ai)
    {
        if (!ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy("+new rpg", BOT_STATE_NON_COMBAT);
        if (ai->HasStrategy("rpg", BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy("-rpg", BOT_STATE_NON_COMBAT);
    }

    std::string StartErrand(Player* bot, PlayerbotAI* ai, const AutopilotPlace& place, uint8_t service,
                            const std::string& label, AutopilotErrand& errand, uint32_t now)
    {
        EnsureNewRpg(ai);
        AutopilotRpg_GoTo(ai, place.map, place.x, place.y, place.z);

        errand.active    = true;
        errand.map       = place.map;
        errand.x         = place.x;
        errand.y         = place.y;
        errand.z         = place.z;
        errand.npcEntry  = place.entry;
        errand.service   = service;
        errand.label     = label;
        errand.startedAt = now;
        return SafeFormat("walking to {} ({} yd)", label, static_cast<uint32_t>(bot->GetDistance(place.x, place.y, place.z)));
    }

    // On arrival at a trainer: learn what this trainer can teach and the bot
    // can afford, through the core trainer code (it charges and checks).
    uint32_t LearnFromTrainer(Player* bot, Creature* npc)
    {
        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
        if (!trainer || !trainer->IsTrainerValidForPlayer(bot))
            return 0;

        uint32_t learned = 0;
        for (const Trainer::Spell& spell : trainer->GetSpells())
        {
            if (!trainer->CanTeachSpell(bot, &spell) || bot->GetMoney() < spell.MoneyCost)
                continue;
            trainer->TeachSpell(npc, bot, spell.SpellId);
            if (bot->HasSpell(spell.SpellId))
                ++learned;
        }
        return learned;
    }
}

void AutopilotCommands_Load()
{
    g_reference = sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.CommandReference", "");
    if (g_reference.empty())
        g_reference = kDefaultReference;
    else
        for (size_t pos = g_reference.find("\\n"); pos != std::string::npos; pos = g_reference.find("\\n", pos + 1))
            g_reference.replace(pos, 2, "\n");

    g_denied.clear();
    for (const std::string& d : SplitString(
             sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.DeniedCommands", kDefaultDenied), ','))
        g_denied.push_back(Lower(d));
}

const std::string& AutopilotCommands_Reference() { return g_reference; }

bool AutopilotCommands_IsDenied(const std::string& command)
{
    return Denied(Lower(Trim(command)));
}

bool AutopilotCommands_IsStrategyChange(const std::string& command)
{
    const std::string c = Lower(Trim(command));
    return StartsWithWord(c, "nc") || StartsWithWord(c, "co");
}

std::string AutopilotCommands_Run(Player* bot, PlayerbotAI* ai, const std::string& raw,
                                  AutopilotErrand& errand, uint32_t now)
{
    std::string command = Trim(raw);
    if (!command.empty() && command[0] == '/')
        command = Trim(command.substr(1));
    const std::string lower = Lower(command);
    if (lower.empty())
        return "empty";
    if (Denied(lower))
        return "denied by the server";

    // --- autopilot commands -------------------------------------------------

    if (StartsWithWord(lower, "goto"))
    {
        std::string what = Trim(lower.substr(4));
        if (StartsWithWord(what, "zone"))
        {
            std::string display;
            const uint32_t zoneId = AutopilotWorld_FindZone(Trim(command.substr(lower.find("zone") + 4)), display);
            if (!zoneId)
                return "unknown zone";
            if (zoneId == bot->GetZoneId())
                return "already in " + display;
            AutopilotPlace place;
            std::string why;
            if (!AutopilotWorld_ZonePlace(bot, zoneId, place, why))
                return why;
            return StartErrand(bot, ai, place, 0, display, errand, now);
        }

        AutopilotService service;
        if (!AutopilotService_FromName(what, service))
        {
            // "goto Feralas" works too.
            std::string display;
            if (const uint32_t zoneId = AutopilotWorld_FindZone(what, display))
                return AutopilotCommands_Run(bot, ai, "goto zone " + display, errand, now);
            return "unknown destination";
        }

        AutopilotPlace place;
        if (!AutopilotWorld_NearestService(bot, service, place))
            return SafeFormat("no {} on this continent", AutopilotService_Name(service));
        return StartErrand(bot, ai, place, static_cast<uint8_t>(service),
                           SafeFormat("the {} {}", AutopilotService_Name(service), place.name), errand, now);
    }

    if (StartsWithWord(lower, "quest") || StartsWithWord(lower, "rpg do quest"))
    {
        const std::string arg = Trim(lower.substr(lower.rfind("quest") + 5));
        uint32_t questId = 0;
        try { questId = static_cast<uint32_t>(std::stoul(arg)); } catch (...) { questId = 0; }

        // Also accept a title from the quest log.
        if (!questId)
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE && !questId; ++slot)
                if (uint32 id = bot->GetQuestSlotQuestId(slot))
                    if (Quest const* q = sObjectMgr->GetQuestTemplate(id))
                        if (Lower(q->GetTitle()).find(arg) != std::string::npos && !arg.empty())
                            questId = id;

        EnsureNewRpg(ai);
        return AutopilotRpg_DoQuest(ai, questId) ? SafeFormat("working on quest {}", questId)
                                                 : "that quest is not in the log";
    }

    if (StartsWithWord(lower, "rpg"))
    {
        std::string status = Trim(lower.substr(3));
        if (StartsWithWord(status, "status"))
            status = Trim(status.substr(6));
        const int id = AutopilotRpg_StatusFromName(status);
        if (id < 0)
            return "unknown rpg status";
        EnsureNewRpg(ai);
        return AutopilotRpg_Steer(ai, { id }) ? "focus " + status : "nothing to do for " + status + " here";
    }

    // --- everything else: a playerbots command, as a master would send it --

    ai->HandleCommand(CHAT_MSG_WHISPER, command, bot);
    return "sent";
}

std::string AutopilotCommands_UpdateErrand(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand, uint32_t now)
{
    if (!errand.active)
        return "";

    if (bot->GetMapId() != errand.map)
    {
        errand.active = false;
        return "gave up on " + errand.label + " (left the continent)";
    }
    if (now - errand.startedAt > 45 * 60)
    {
        errand.active = false;
        return "never reached " + errand.label;
    }

    if (bot->GetDistance(errand.x, errand.y, errand.z) > 25.0f)
    {
        // Keep walking: NewRpg may have moved on to something else.
        if (!bot->IsInCombat() && !AutopilotRpg_IsTravelling(ai) &&
            AutopilotRpg_CurrentStatus(ai) != AutopilotRpg_StatusFromName("go camp"))
            AutopilotRpg_GoTo(ai, errand.map, errand.x, errand.y, errand.z);
        return "";
    }

    errand.active = false;
    if (!errand.npcEntry)
        return "arrived in " + errand.label;

    Creature* npc = bot->FindNearestCreature(errand.npcEntry, 40.0f);
    switch (static_cast<AutopilotService>(errand.service))
    {
        case AutopilotService::Repair:
            ai->HandleCommand(CHAT_MSG_WHISPER, "repair", bot);
            ai->HandleCommand(CHAT_MSG_WHISPER, "s gray", bot);
            return "arrived at " + errand.label + ": repairing and selling junk";
        case AutopilotService::Vendor:
            ai->HandleCommand(CHAT_MSG_WHISPER, "s gray", bot);
            return "arrived at " + errand.label + ": selling junk";
        case AutopilotService::Trainer:
        case AutopilotService::Profession:
        {
            if (!npc)
                return "arrived, but " + errand.label + " was not there";
            const uint32_t learned = LearnFromTrainer(bot, npc);
            return SafeFormat("trained at {}: learned {} spell{}", errand.label, learned, learned == 1 ? "" : "s");
        }
        case AutopilotService::Inn:
            if (!npc)
                return "arrived, but " + errand.label + " was not there";
            bot->SetHomebind(WorldLocation(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                                           bot->GetPositionZ(), bot->GetOrientation()), bot->GetAreaId());
            return "made " + errand.label + "'s inn their home";
        default:
            return "arrived at " + errand.label;
    }
}
