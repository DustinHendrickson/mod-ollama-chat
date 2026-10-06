#include "mod-ollama-chat_autopilot_commands.h"
#include "mod-ollama-chat_autopilot_strategy.h"
#include "mod-ollama-chat_autopilot_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Config.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "Trainer.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"

#include <algorithm>
#include <cctype>

namespace
{
    // What the LLM is told it can do. Every playerbots command named here was
    // checked against this fork's chat command handler.
    const char* const kDefaultReference =
        "You are their controller: nothing else decides where they go or what they set out to do. Without "
        "orders they stay where they are and only do what their strategies make them do around them.\n"
        "STRATEGIES (behaviours that stay on until changed):\n"
        "- nc +name,-name : out-of-combat behaviours. Useful: grind (fight monsters around them), quest (accept "
        "and turn in quests with NPCs right next to them), loot, gather (herbs, ore, skins), food (eat and drink "
        "when low), mount, lfg (queue for dungeons), bg (queue for battlegrounds), pvp (fight enemy players), "
        "duel, emote, guild, group (invite people nearby), maintenance (keep spells, talents and gear up to date)\n"
        "- co +name,-name : combat behaviours. Useful: flee (run when losing), potions, avoid aoe, aggressive, "
        "threat (do not pull aggro), kite, save mana. Class and role strategies (tank, heal, dps, aoe...) also "
        "exist; change them only if you are sure.\n"
        "ORDERS:\n"
        "- goto <service> : go to the nearest repair, vendor, trainer, profession, inn or flightmaster and use "
        "it (repair and sell junk, learn new spells, make the inn their home, learn the flight point there); or "
        "to the nearest bank or auction house (they only go there; at a bank, bank <item> stores an item and "
        "bank -<item> takes one out; they cannot trade at the auction house)\n"
        "- goto zone <zone name> : travel to a zone anywhere in the world; they walk, take boats, zeppelins and "
        "portals, and fly between flight points they have learned\n"
        "- goto hunt : go to the nearest group of monsters of their level (grind only fights what is right "
        "around them, so send them here to fight)\n"
        "- quest <id> : go to where that quest's objective is; once it is complete, go to whoever takes it in "
        "and turn it in. Turn on what the objective needs (grind, loot, gather) yourself\n"
        "- talents : spend talent points; autogear : equip the best gear they have; s gray : sell junk to a "
        "vendor nearby; repair : repair at a vendor nearby\n"
        "- follow / stay : follow the group leader or stay put\n"
        "Orders that send them somewhere (goto, quest) are carried out one after another, in the order you "
        "give them, so list them in the order they should happen. Each ends on arrival (goto hunt too: put it "
        "last, or the next order leads them away from the monsters). New orders of that kind replace any still "
        "waiting.\n";

    const char* const kDefaultDenied =
        "logout,reset,destroy,teleport,summon,cheat,debug,cdebug,wipe,sendmail,mail,hire,give leader,"
        "guild remove,guild demote,guild promote,guild leave,log,d,do,release,leave,rpg";

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

    std::string StartErrand(Player* bot, AutopilotErrand& errand, AutopilotErrandKind kind, uint8_t service,
                            uint32_t npcEntry, uint32_t questId, const AutopilotPlace& place, float radius,
                            const std::string& label, uint32_t now)
    {
        AutopilotErrand next;
        next.kind      = kind;
        next.service   = service;
        next.npcEntry  = npcEntry;
        next.questId   = questId;
        next.label     = label;
        next.startedAt = now;

        const std::string why = AutopilotTravel_Start(bot, { place.map, place.x, place.y, place.z }, radius,
                                                      npcEntry, next.trip, now);
        if (!why.empty())
            return why;

        next.active = true;
        errand      = std::move(next);
        return place.map == bot->GetMapId()
            ? SafeFormat("on the way to {} ({} yd)", label, uint32_t(bot->GetDistance(place.x, place.y, place.z)))
            : SafeFormat("on the way to {} (another continent)", label);
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

    uint16 QuestSlot(Player* bot, uint32_t questId)
    {
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (bot->GetQuestSlotQuestId(slot) == questId)
                return slot;
        return MAX_QUEST_LOG_SIZE;
    }

    // Where to go for a quest that is not done yet: the nearest spawn of a
    // creature it still needs killed or spoken to, else the quest's own map
    // marker for an objective.
    bool QuestObjectivePlace(Player* bot, Quest const* quest, AutopilotPlace& out, std::string& what)
    {
        const uint16 slot = QuestSlot(bot, quest->GetQuestId());
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            const int32 npcOrGo = quest->RequiredNpcOrGo[i];
            if (npcOrGo <= 0 || !quest->RequiredNpcOrGoCount[i])
                continue;
            if (slot < MAX_QUEST_LOG_SIZE && bot->GetQuestSlotCounter(slot, i) >= quest->RequiredNpcOrGoCount[i])
                continue;
            if (AutopilotWorld_NearestSpawn(bot, uint32_t(npcOrGo), out))
            {
                what = out.name;
                return true;
            }
        }

        if (QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(quest->GetQuestId()))
        {
            const QuestPOI* best = nullptr;
            for (const QuestPOI& poi : *pois)
            {
                if (poi.points.empty() || poi.ObjectiveIndex < 0)
                    continue;
                if (!best || (poi.MapId == bot->GetMapId() && best->MapId != bot->GetMapId()))
                    best = &poi;
            }
            if (best)
            {
                float x = 0.0f, y = 0.0f;
                for (const QuestPOIPoint& p : best->points)
                {
                    x += float(p.x);
                    y += float(p.y);
                }
                x /= float(best->points.size());
                y /= float(best->points.size());

                out.map = best->MapId;
                out.x   = x;
                out.y   = y;
                out.z   = 0.0f;   // unknown; the travel planner finds the ground
                if (best->MapId == bot->GetMapId())
                {
                    const float h = bot->GetMap()->GetHeight(x, y, bot->GetPositionZ() + 100.0f, true, 400.0f);
                    out.z = h > INVALID_HEIGHT ? h : bot->GetPositionZ();
                }
                what = "the quest's objective area";
                return true;
            }
        }
        return false;
    }
}

void AutopilotCommands_Load()
{
    AutopilotRoute_LoadConfig();
    AutopilotTravel_LoadConfig();

    g_reference = sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.CommandReference", "");
    if (g_reference.empty())
        g_reference = kDefaultReference;
    else
        for (size_t pos = g_reference.find("\\n"); pos != std::string::npos; pos = g_reference.find("\\n", pos + 1))
            g_reference.replace(pos, 2, "\n");

    g_denied.clear();
    for (const std::string& d : SplitString(
             sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.DeniedCommands", kDefaultDenied), ','))
        g_denied.push_back(Lower(Trim(d)));
}

const std::string& AutopilotCommands_Reference() { return g_reference; }

bool AutopilotCommands_IsDenied(const std::string& command)
{
    const std::string c = Lower(Trim(command));

    // Playerbots splits one whisper into several commands on its separator
    // and strips "#w " / "#p " style prefixes before running them, so either
    // would slip a denied command past a leading-words check ("stay\reset").
    // One order is one command.
    const std::string& sep = sPlayerbotAIConfig.commandSeparator;
    if ((!sep.empty() && c.find(sep) != std::string::npos) || (!c.empty() && c[0] == '#'))
        return true;

    return Denied(c);
}

std::string AutopilotCommands_Normalize(const std::string& raw)
{
    std::string command = Trim(raw);
    while (!command.empty() && command[0] == '/')
        command = Trim(command.substr(1));
    return command;
}

bool AutopilotCommands_IsStrategyChange(const std::string& command)
{
    const std::string c = Lower(Trim(command));
    return StartsWithWord(c, "nc") || StartsWithWord(c, "co");
}

bool AutopilotCommands_IsErrand(const std::string& command)
{
    const std::string c = Lower(Trim(command));
    return StartsWithWord(c, "goto") || StartsWithWord(c, "quest");
}

std::string AutopilotCommands_Run(Player* bot, PlayerbotAI* ai, const std::string& raw,
                                  AutopilotErrand& errand, uint32_t now)
{
    const std::string command = AutopilotCommands_Normalize(raw);
    const std::string lower   = Lower(command);
    if (lower.empty())
        return "empty";
    if (AutopilotCommands_IsDenied(command))
        return "denied by the server";

    // --- autopilot orders ----------------------------------------------------

    // A new trip from the deck of a ship at sea, or from the sky, would start
    // walking from there. Finish the crossing first.
    if ((StartsWithWord(lower, "goto") || StartsWithWord(lower, "quest")) &&
        (bot->IsInFlight() || bot->GetTransport() ||
         (errand.active && AutopilotTravel_IsTimeCritical(errand.trip))))
        return "not now: " + (errand.active ? AutopilotTravel_Describe(bot, errand.trip)
                                            : std::string("in the middle of a crossing")) +
               "; give the order once they arrive";

    if (StartsWithWord(lower, "goto"))
    {
        const std::string what = Trim(lower.substr(4));

        // Grind only fights what is around the bot; this is how it goes
        // somewhere with monsters worth fighting.
        if (what == "hunt" || what == "hunting" || what == "hunting ground" || what == "grind" || what == "mobs")
        {
            AutopilotPlace place;
            if (!AutopilotWorld_HuntingGround(bot, place))
                return "no monsters of their level found on this continent";
            if (errand.active)
                AutopilotCommands_StopErrand(ai, errand);
            return StartErrand(bot, errand, AutopilotErrandKind::Place, 0, 0, 0, place, 25.0f,
                               "a hunting ground: " + place.name, now);
        }

        AutopilotService service;
        if (!StartsWithWord(what, "zone") && AutopilotService_FromName(what, service))
        {
            AutopilotPlace place;
            if (!AutopilotWorld_NearestService(bot, service, place))
                return SafeFormat("no {} on this continent", AutopilotService_Name(service));
            if (errand.active)
                AutopilotCommands_StopErrand(ai, errand);
            return StartErrand(bot, errand, AutopilotErrandKind::Service, static_cast<uint8_t>(service), place.entry,
                               0, place, 25.0f, SafeFormat("the {} {}", AutopilotService_Name(service), place.name),
                               now);
        }

        // "goto zone Feralas", or just "goto Feralas".
        std::string name = StartsWithWord(what, "zone") ? Trim(command.substr(lower.find("zone") + 4))
                                                        : Trim(command.substr(4));
        std::string display;
        const uint32_t zoneId = AutopilotWorld_FindZone(name, display);
        if (!zoneId)
            return "unknown zone or destination";
        if (zoneId == bot->GetZoneId())
            return "already in " + display;

        AutopilotPlace place;
        std::string why;
        if (!AutopilotWorld_ZonePlace(bot, zoneId, place, why))
            return why;
        if (!AutopilotTravel_CanReach(bot, place.map))
            return display + " cannot be reached from here by foot, flight, boat or zeppelin";
        if (errand.active)
            AutopilotCommands_StopErrand(ai, errand);
        return StartErrand(bot, errand, AutopilotErrandKind::Place, 0, 0, 0, place, 40.0f, display, now);
    }

    if (StartsWithWord(lower, "quest"))
    {
        const std::string arg = Trim(lower.substr(5));
        uint32_t questId = 0;
        try { questId = static_cast<uint32_t>(std::stoul(arg)); } catch (...) { questId = 0; }

        // Also accept a title from the quest log.
        if (!questId && !arg.empty())
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE && !questId; ++slot)
                if (uint32 id = bot->GetQuestSlotQuestId(slot))
                    if (Quest const* q = sObjectMgr->GetQuestTemplate(id))
                        if (Lower(q->GetTitle()).find(arg) != std::string::npos)
                            questId = id;

        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        const QuestStatus status = quest ? bot->GetQuestStatus(questId) : QUEST_STATUS_NONE;
        if (!quest || (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE))
            return "that quest is not in the log";

        AutopilotPlace place;
        if (status == QUEST_STATUS_COMPLETE)
        {
            bool found = false;
            for (uint32_t ender : AutopilotWorld_QuestEnders(questId))
            {
                AutopilotPlace p;
                if (!AutopilotWorld_NearestSpawn(bot, ender, p))
                    continue;
                if (!found || (p.map == bot->GetMapId() && (place.map != bot->GetMapId() || p.distance < place.distance)))
                {
                    place = p;
                    found = true;
                }
            }
            if (!found)
                return "nobody to turn that quest in to could be found";
            if (errand.active)
                AutopilotCommands_StopErrand(ai, errand);
            return StartErrand(bot, errand, AutopilotErrandKind::QuestTurnIn, 0, place.entry, questId, place, 25.0f,
                               SafeFormat("{} to turn in {}", place.name, quest->GetTitle()), now);
        }

        std::string what;
        if (!QuestObjectivePlace(bot, quest, place, what))
            return "no idea where that quest's objective is";
        if (errand.active)
            AutopilotCommands_StopErrand(ai, errand);
        return StartErrand(bot, errand, AutopilotErrandKind::QuestObjective, 0, 0, questId, place, 40.0f,
                           SafeFormat("{} for {}", what, quest->GetTitle()), now);
    }

    // --- everything else: a playerbots command, as a master would send it --

    ai->HandleCommand(CHAT_MSG_WHISPER, command, bot);
    return "sent";
}

AutopilotErrandUpdate AutopilotCommands_UpdateErrand(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand,
                                                     uint32_t now)
{
    AutopilotErrandUpdate u;
    if (!errand.active)
        return u;

    std::string note;
    const AutopilotTripState state = AutopilotTravel_Update(bot, ai, errand.trip, now, note);

    if (state == AutopilotTripState::Going)
    {
        u.note = note;   // a milestone (boarded, landed...), or nothing
        return u;
    }

    errand.active = false;
    u.finished    = true;
    if (state == AutopilotTripState::Failed)
    {
        u.note = SafeFormat("never reached {}{}", errand.label, note.empty() ? "" : ": " + note);
        return u;
    }

    Creature* npc = errand.npcEntry ? bot->FindNearestCreature(errand.npcEntry, 40.0f) : nullptr;
    switch (errand.kind)
    {
        case AutopilotErrandKind::Place:
            u.note = "arrived in " + errand.label;
            return u;

        case AutopilotErrandKind::QuestObjective:
            u.note = "arrived at " + errand.label;
            return u;

        case AutopilotErrandKind::QuestTurnIn:
        {
            if (!npc)
            {
                u.note = "arrived, but " + errand.label.substr(0, errand.label.find(" to turn in")) + " was not there";
                return u;
            }
            AutopilotQuest_TalkTo(ai, npc);

            // Playerbots picks a reward itself for random bots; an alt asks
            // its master to choose, and an autopilot bot's master is the LLM,
            // which cannot answer a whisper. Take the best usable choice.
            Quest const* quest = sObjectMgr->GetQuestTemplate(errand.questId);
            if (quest && !bot->GetQuestRewardStatus(errand.questId) && bot->CanRewardQuest(quest, false))
            {
                uint32_t choice = 0, bestLevel = 0;
                for (uint32_t i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
                {
                    ItemTemplate const* item = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
                    if (!item || bot->CanUseItem(item) != EQUIP_ERR_OK)
                        continue;
                    if (item->ItemLevel > bestLevel)
                    {
                        bestLevel = item->ItemLevel;
                        choice    = i;
                    }
                }
                if (bot->CanRewardQuest(quest, choice, false))
                    bot->RewardQuest(quest, choice, npc, true);
            }

            u.note = bot->GetQuestRewardStatus(errand.questId)
                ? "turned in the quest with " + npc->GetName()
                : "talked to " + npc->GetName() + " but the quest was not turned in (it may need choosing a reward)";
            return u;
        }

        case AutopilotErrandKind::Service:
            break;
    }

    switch (static_cast<AutopilotService>(errand.service))
    {
        case AutopilotService::Repair:
            ai->HandleCommand(CHAT_MSG_WHISPER, "repair", bot);
            ai->HandleCommand(CHAT_MSG_WHISPER, "s gray", bot);
            u.note = "arrived at " + errand.label + ": repairing and selling junk";
            return u;
        case AutopilotService::Vendor:
            ai->HandleCommand(CHAT_MSG_WHISPER, "s gray", bot);
            u.note = "arrived at " + errand.label + ": selling junk";
            return u;
        case AutopilotService::Trainer:
        case AutopilotService::Profession:
        {
            if (!npc)
            {
                u.note = "arrived, but " + errand.label + " was not there";
                return u;
            }
            const uint32_t learned = LearnFromTrainer(bot, npc);
            u.note = SafeFormat("trained at {}: learned {} spell{}", errand.label, learned, learned == 1 ? "" : "s");
            return u;
        }
        case AutopilotService::FlightMaster:
            if (!npc)
            {
                u.note = "arrived, but " + errand.label + " was not there";
                return u;
            }
            // Talking to a flight master is how a flight point is discovered;
            // trips only fly between discovered ones.
            bot->GetSession()->SendLearnNewTaxiNode(npc);
            u.note = "arrived at " + errand.label + " and learned the flight point";
            return u;
        case AutopilotService::Inn:
            if (!npc)
            {
                u.note = "arrived, but " + errand.label + " was not there";
                return u;
            }
            bot->SetHomebind(WorldLocation(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                                           bot->GetPositionZ(), bot->GetOrientation()), bot->GetAreaId());
            u.note = "made " + errand.label + "'s inn their home";
            return u;
        default:
            u.note = "arrived at " + errand.label;
            return u;
    }
}

void AutopilotCommands_StopErrand(PlayerbotAI* ai, AutopilotErrand& errand)
{
    AutopilotTravel_Stop(ai, errand.trip);
    errand.active = false;
}
