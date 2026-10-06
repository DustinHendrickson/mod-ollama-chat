#include "mod-ollama-chat_autopilot_commands.h"
#include "mod-ollama-chat_autopilot_strategy.h"
#include "mod-ollama-chat_autopilot_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Bag.h"
#include "CellImpl.h"
#include "Config.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Creature.h"
#include "GameObject.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Opcodes.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "SharedDefines.h"
#include "Trainer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <list>
#include <map>

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
        "duel, emote, guild, group (invite people nearby), maintenance (upkeep with their own skills and things: "
        "learn recipes in their bags, disenchant and enchant items, destroy junk when bags are full, use quest "
        "items, share quests)\n"
        "- co +name,-name : combat behaviours. Useful: flee (run when losing), potions, avoid aoe, aggressive, "
        "threat (do not pull aggro), kite, save mana. Class and role strategies (tank, heal, dps, aoe...) also "
        "exist; change them only if you are sure.\n"
        "ORDERS:\n"
        "- goto <service> : go to the nearest repair, vendor, trainer, profession, inn or flightmaster and use "
        "it (repair and sell junk, make the inn their home, learn the flight point there). trainer = their "
        "own class trainer, for new spells and ranks as they level; or "
        "to the nearest bank or auction house (they only go there; at a bank, bank <item> stores an item and "
        "bank -<item> takes one out; they cannot trade at the auction house)\n"
        "- goto profession <name> : go to that profession's trainer and learn what it teaches them: the "
        "profession itself (a new primary profession needs a free slot -- two at most; Cooking, First Aid and "
        "Fishing are free), its next rank once their skill allows, and recipes. Which professions to take up "
        "is the character's choice: Alchemy, Blacksmithing, Enchanting, Engineering, Herbalism, Inscription, "
        "Jewelcrafting, Leatherworking, Mining, Skinning, Tailoring; Cooking, First Aid, Fishing\n"
        "- goto zone <zone name> : travel to a zone anywhere in the world; they walk, take boats, zeppelins and "
        "portals, and fly between flight points they have learned\n"
        "- goto hunt : go to the nearest group of monsters of their level (grind only fights what is right "
        "around them, so send them here to fight)\n"
        "- quest <id> : go to where that quest's objective is; once it is complete, go to whoever takes it in "
        "and turn it in. On arrival they stay and work the objective, the nearest thing needed first: "
        "creatures to kill, objects to use, and the creatures and objects that give the quest's items "
        "(keep loot on so they pick the items up), until it is done or a while has passed; you hear the "
        "count. When a quest ready to "
        "turn in offers rewards to choose from (listed under it), pick one: quest <id> reward <n>. Each quest "
        "ready to turn in says who takes it and where: one on another continent is a long trip, so weigh it\n"
        "- abandon <id> : drop a quest from the log, as a player does with one not worth the trip or not for "
        "them (marked in the log)\n"
        "- talents spec <name> : follow one of their class's talent specs (listed with their facts) and spend "
        "every unspent point along it; give it again after levelling to spend new points. Which spec is the "
        "character's choice. talents autopick lets their AI choose instead\n"
        "- GEAR AND MONEY (they keep what they loot, buy and are given; nothing is handed to them): equip upgrade "
        "puts on the best gear in their bags; e <item name> wears one item; use <item name> uses one; open "
        "items opens loot boxes and clams in their bags. At a vendor (goto vendor first): s gray sells junk, s "
        "vendor sells everything a vendor should have, s <item name> sells one item, b vendor buys what they "
        "can use (gear upgrades, ammo, food and drink, reagents) with their own money, repair repairs. "
        "Watch their gold: buying and training cost money\n"
        "- PROFESSIONS AND THE WORLD AROUND THEM (what is nearby and what they could craft is listed with "
        "their facts): nc +loot loots bodies, and skins them with Skinning and a skinning knife; nc +gather "
        "picks herbs and mines ore they have the skill for as they pass; nc +master fishing fishes at water "
        "nearby (needs Fishing and a fishing pole); open <object> walks to a node, chest or other object "
        "nearby and opens or uses it; craft <recipe> [count|all] makes something from their own materials "
        "(they walk to a forge, anvil or fire first if it needs one); disenchant <item> (Enchanting). "
        "Making things still worth skill points raises the profession\n"
        "- follow / stay : follow the group leader or stay put\n"
        "ORDER OF ORDERS: orders run in the order you give them. Everything after a goto or quest waits until "
        "they get there (so goto vendor, then b vendor, buys at the vendor), except nc/co changes, which apply "
        "at once. Each trip ends on arrival (goto hunt too: put it last, or the next order leads them away from "
        "the monsters). A new plan with a goto or quest replaces any orders still waiting.\n";

    const char* const kDefaultDenied =
        "logout,reset,destroy,teleport,summon,cheat,debug,cdebug,wipe,sendmail,mail,hire,give leader,"
        "guild remove,guild demote,guild promote,guild leave,log,d,do,release,leave,rpg";

    std::string              g_reference;
    uint32_t                 g_huntMinutes = 20;
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
    // --- crafting -------------------------------------------------------------

    constexpr uint32_t kDisenchantSpell = 13262;

    struct Recipe
    {
        uint32_t    spell = 0;
        uint32_t    item  = 0;
        std::string name;   // what it makes
    };

    // A profession recipe the bot knows: a spell on a skill line that creates
    // an item.
    std::vector<Recipe> KnownRecipes(Player* bot)
    {
        std::vector<Recipe> out;
        for (auto const& [spellId, spell] : bot->GetSpellMap())
        {
            if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info)
                continue;
            uint32_t item = 0;
            for (SpellEffectInfo const& e : info->GetEffects())
                if (e.Effect == SPELL_EFFECT_CREATE_ITEM && e.ItemType)
                    item = e.ItemType;
            if (!item)
                continue;
            auto bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
            if (bounds.first == bounds.second)
                continue;
            ItemTemplate const* t = sObjectMgr->GetItemTemplate(item);
            out.push_back({ spellId, item, t ? t->Name1 : std::string(info->SpellName[0] ? info->SpellName[0] : "?") });
        }
        return out;
    }

    // How many times the bags hold this recipe's materials.
    uint32_t Craftable(Player* bot, SpellInfo const* info)
    {
        uint32_t times = UINT32_MAX;
        for (size_t i = 0; i < info->Reagent.size(); ++i)
            if (info->Reagent[i] > 0 && info->ReagentCount[i])
                times = std::min<uint32_t>(times, bot->GetItemCount(uint32_t(info->Reagent[i]), false) /
                                                      info->ReagentCount[i]);
        return times == UINT32_MAX ? 0 : times;
    }

    // "2 Rough Stone (have 1)"
    std::string MissingMaterials(Player* bot, SpellInfo const* info)
    {
        std::string out;
        for (size_t i = 0; i < info->Reagent.size(); ++i)
        {
            if (info->Reagent[i] <= 0 || !info->ReagentCount[i])
                continue;
            const uint32_t have = bot->GetItemCount(uint32_t(info->Reagent[i]), false);
            ItemTemplate const* t = sObjectMgr->GetItemTemplate(uint32_t(info->Reagent[i]));
            out += SafeFormat("{}{} {} (have {})", out.empty() ? "" : ", ", info->ReagentCount[i],
                              t ? t->Name1 : std::to_string(info->Reagent[i]), have);
        }
        return out;
    }

    // Still worth skill points (below where the recipe turns grey).
    bool RaisesSkill(Player* bot, uint32_t spellId)
    {
        auto bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto it = bounds.first; it != bounds.second; ++it)
            if (it->second->TrivialSkillLineRankHigh &&
                bot->GetSkillValue(it->second->SkillLine) < it->second->TrivialSkillLineRankHigh)
                return true;
        return false;
    }

    const Recipe* FindRecipe(const std::vector<Recipe>& recipes, const std::string& name)
    {
        for (const Recipe& r : recipes)
            if (Lower(r.name) == name)
                return &r;
        for (const Recipe& r : recipes)
            if (Lower(r.name).find(name) != std::string::npos)
                return &r;
        return nullptr;
    }

    // An item in the bags (not worn) by name.
    Item* FindBagItem(Player* bot, const std::string& name)
    {
        auto matches = [&](Item* item)
        {
            return item && item->GetTemplate() && Lower(item->GetTemplate()->Name1).find(name) != std::string::npos;
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); matches(item))
                return item;
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* b = bot->GetBagByPos(bag))
                for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                    if (Item* item = b->GetItemByPos(uint8(slot)); matches(item))
                        return item;
        return nullptr;
    }

    // --- the world around the bot ---------------------------------------------

    struct DeadCreatureCheck
    {
        Player* bot;
        float   range;
        bool operator()(Creature* c) const { return c && !c->IsAlive() && bot->IsWithinDistInMap(c, range); }
    };

    struct NearbyObjectCheck
    {
        Player* bot;
        float   range;
        bool operator()(GameObject* go) const
        {
            return go && go->isSpawned() && bot->IsWithinDistInMap(go, range);
        }
    };

    std::list<GameObject*> ObjectsNear(Player* bot, float range)
    {
        std::list<GameObject*> out;
        NearbyObjectCheck check{ bot, range };
        Acore::GameObjectListSearcher<NearbyObjectCheck> searcher(bot, out, check);
        Cell::VisitObjects(bot, searcher, range);
        return out;
    }

    // The skill a gathering node (or lockbox) asks for, and how much; 0 if none.
    uint32_t NodeSkill(GameObject* go, uint32_t& required)
    {
        required = 0;
        LockEntry const* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
        if (!lock)
            return 0;
        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
            if (lock->Type[i] == LOCK_KEY_SKILL)
                if (const SkillType skill = SkillByLockType(LockType(lock->Index[i])); skill != SKILL_NONE)
                {
                    required = lock->Skill[i];
                    return uint32_t(skill);
                }
        return 0;
    }

    std::string SkillName(uint32_t skill)
    {
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
        return line && line->name[0] ? std::string(line->name[0]) : std::to_string(skill);
    }

    // What a quest still needs from the world, by kind: creatures to kill
    // (or speak to), objects to use, and the creatures and objects that give
    // the items still missing.
    struct QuestNeeds
    {
        std::vector<uint32_t> kill;
        std::vector<uint32_t> use;
        std::vector<uint32_t> lootCreatures;
        std::vector<uint32_t> lootObjects;

        bool Any() const { return !kill.empty() || !use.empty() || !lootCreatures.empty() || !lootObjects.empty(); }
        std::vector<uint32_t> Creatures() const
        {
            std::vector<uint32_t> out = kill;
            out.insert(out.end(), lootCreatures.begin(), lootCreatures.end());
            return out;
        }
        std::vector<uint32_t> Objects() const
        {
            std::vector<uint32_t> out = use;
            out.insert(out.end(), lootObjects.begin(), lootObjects.end());
            return out;
        }
    };

    QuestNeeds NeedsOf(Player* bot, Quest const* quest)
    {
        QuestNeeds n;
        const uint16 slot = QuestSlot(bot, quest->GetQuestId());
        if (slot >= MAX_QUEST_LOG_SIZE)
            return n;
        auto add = [](std::vector<uint32_t>& list, uint32_t entry)
        {
            if (std::find(list.begin(), list.end(), entry) == list.end())
                list.push_back(entry);
        };
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            const int32 entry = quest->RequiredNpcOrGo[i];
            if (entry && quest->RequiredNpcOrGoCount[i] &&
                bot->GetQuestSlotCounter(slot, i) < quest->RequiredNpcOrGoCount[i])
                add(entry > 0 ? n.kill : n.use, uint32_t(entry > 0 ? entry : -entry));
        }
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            const uint32 item = quest->RequiredItemId[i];
            if (!item || bot->GetItemCount(item, true) >= quest->RequiredItemCount[i])
                continue;
            for (uint32_t c : AutopilotWorld_CreaturesDropping(item))
                add(n.lootCreatures, c);
            for (uint32_t o : AutopilotWorld_ObjectsHolding(item))
                add(n.lootObjects, o);
        }
        return n;
    }

    bool QuestObjectivePlace(Player* bot, Quest const* quest, AutopilotPlace& out, std::string& what)
    {
        // The nearest of what it still needs: a creature to kill, an object to
        // use, or a creature or object that gives a missing item.
        const QuestNeeds needs = NeedsOf(bot, quest);
        if (AutopilotWorld_SpawnBeyond(bot, needs.Creatures(), needs.Objects(), 0.0f, out))
        {
            what = out.name;
            return true;
        }
        for (uint32_t entry : needs.kill)   // only on another continent
            if (AutopilotWorld_NearestSpawn(bot, entry, out))
            {
                what = out.name;
                return true;
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

    g_huntMinutes = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.QuestHuntMinutes", 20);

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

    // With NoHandouts, the orders playerbots answers with free items: gear
    // conjured from templates (autogear), or the maintenance package (spells,
    // consumables, repairs). The character earns, buys and loots instead.
    if (AutopilotStrategy_NoHandouts())
        for (const char* freebie : { "maintenance", "autogear", "bis", "cheat" })
            if (StartsWithWord(c, freebie))
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

        // "goto profession mining", or just "goto mining": that profession's
        // trainer. Which profession is the character's choice, so a bare
        // "goto profession" asks for one instead of taking the nearest.
        {
            const bool named = StartsWithWord(what, "profession");
            const std::string profName = named ? Trim(what.substr(10)) : what;
            std::string display;
            const uint32_t skill = AutopilotWorld_ProfessionSkill(profName, display);
            if (skill)
            {
                AutopilotPlace place;
                if (!AutopilotWorld_NearestProfessionTrainer(bot, skill, place))
                    return "no " + display + " trainer on this continent";
                if (errand.active)
                    AutopilotCommands_StopErrand(ai, errand);
                return StartErrand(bot, errand, AutopilotErrandKind::Service,
                                   static_cast<uint8_t>(AutopilotService::Profession), place.entry, 0, place, 25.0f,
                                   SafeFormat("the {} trainer {}", display, place.name), now);
            }
            if (named)
                return "say which profession: goto profession <name> (" + AutopilotWorld_ProfessionNames() + ")";
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

    // "craft copper bar 5", "craft linen bandage all": a recipe the bot knows,
    // from its own materials, at a crafting station if the recipe needs one.
    if (StartsWithWord(lower, "craft"))
    {
        std::string arg = Trim(lower.substr(5));
        uint32_t want = 1;
        bool all = false;
        if (const size_t sp = arg.rfind(' '); sp != std::string::npos)
        {
            const std::string last = arg.substr(sp + 1);
            if (last == "all")
                all = true;
            else
                try { want = std::max<uint32_t>(1, uint32_t(std::stoul(last))); } catch (...) { want = 0; }
            if (all || want)
                arg = Trim(arg.substr(0, sp));
            if (!want)
                want = 1;
        }

        const std::vector<Recipe> recipes = KnownRecipes(bot);
        const Recipe* recipe = arg.empty() ? nullptr : FindRecipe(recipes, arg);
        if (!recipe)
            return "they know no recipe for that (their recipes are listed with their facts)";
        SpellInfo const* info = sSpellMgr->GetSpellInfo(recipe->spell);
        const uint32_t can = info ? Craftable(bot, info) : 0;
        if (!can)
            return SafeFormat("not enough materials for {}: needs {}", recipe->name, MissingMaterials(bot, info));
        const uint32_t count = std::min<uint32_t>({ all ? can : want, can, 20 });

        AutopilotErrand next;
        next.kind       = AutopilotErrandKind::Craft;
        next.craftSpell = recipe->spell;
        next.craftLeft  = count;
        next.label      = SafeFormat("crafting {} x{}", recipe->name, count);
        next.startedAt  = now;

        if (info->RequiresSpellFocus)
        {
            AutopilotPlace station;
            if (!AutopilotWorld_NearestSpellFocus(bot, info->RequiresSpellFocus, station))
                return SafeFormat("{} needs {}, and there is none on this continent", recipe->name,
                                  AutopilotWorld_SpellFocusName(info->RequiresSpellFocus));
            if (station.distance > 6.0f)
            {
                const std::string why = AutopilotTravel_Start(bot, { station.map, station.x, station.y, station.z },
                                                              4.0f, 0, next.trip, now);
                if (!why.empty())
                    return why;
                if (errand.active)
                    AutopilotCommands_StopErrand(ai, errand);
                next.active = true;
                errand      = std::move(next);
                return SafeFormat("on the way to the {} ({} yd) to craft {} x{}", station.name,
                                  uint32_t(station.distance), recipe->name, count);
            }
        }
        if (errand.active)
            AutopilotCommands_StopErrand(ai, errand);
        next.active = true;
        errand      = std::move(next);
        return SafeFormat("crafting {} x{}", recipe->name, count);
    }

    // "disenchant worn shortsword": an item in the bags, with their own
    // Enchanting skill.
    if (StartsWithWord(lower, "disenchant"))
    {
        const std::string name = Trim(lower.substr(10));
        if (!bot->HasSpell(kDisenchantSpell))
            return "they cannot disenchant (no Enchanting)";
        Item* item = name.empty() ? nullptr : FindBagItem(bot, name);
        if (!item)
            return "no item called that in their bags";
        ItemTemplate const* t = item->GetTemplate();
        if (!t->DisenchantID || int32(t->RequiredDisenchantSkill) < 0 ||
            bot->GetSkillValue(SKILL_ENCHANTING) < t->RequiredDisenchantSkill)
            return t->Name1 + " cannot be disenchanted by them (not disenchantable, or too little skill)";
        AutopilotMove_Stop(ai);
        return ai->CastSpell(kDisenchantSpell, bot, item) ? "done: disenchanting " + t->Name1
                                                          : "could not disenchant " + t->Name1 + " right now";
    }

    // "open copper vein", "open battered chest": an object nearby, opened the
    // way their AI loots (gathering skills included) or used like the client.
    // "open items" stays playerbots' own order (boxes in the bags).
    if (StartsWithWord(lower, "open") && lower != "open items")
    {
        const std::string name = Trim(lower.substr(4));
        GameObject* best = nullptr;
        float bestDist = 0.0f;
        for (GameObject* go : ObjectsNear(bot, 50.0f))
        {
            if (name.empty() || Lower(go->GetGOInfo()->name).find(name) == std::string::npos ||
                go->GetGoState() != GO_STATE_READY)
                continue;
            const float d = bot->GetDistance(go);
            if (!best || d < bestDist)
            {
                best     = go;
                bestDist = d;
            }
        }
        if (!best)
            return "nothing called that within 50 yards";

        AutopilotPlace place;
        place.map   = best->GetMapId();
        place.x     = best->GetPositionX();
        place.y     = best->GetPositionY();
        place.z     = best->GetPositionZ();
        place.entry = best->GetEntry();
        if (errand.active)
            AutopilotCommands_StopErrand(ai, errand);
        const std::string result = StartErrand(bot, errand, AutopilotErrandKind::Open, 0, 0, 0, place, 3.0f,
                                               best->GetGOInfo()->name, now);
        errand.objectGuid = best->GetGUID().GetRawValue();
        return result;
    }

    // "abandon 6121" or "abandon Lessons Anew": drop a quest from the log the
    // way the client's Abandon button does (playerbots' drop needs a master).
    if (StartsWithWord(lower, "abandon"))
    {
        const std::string arg = Trim(lower.substr(7));
        uint32_t questId = 0;
        try { questId = static_cast<uint32_t>(std::stoul(arg)); } catch (...) { questId = 0; }
        uint16 slot = MAX_QUEST_LOG_SIZE;
        for (uint16 s = 0; s < MAX_QUEST_LOG_SIZE && slot == MAX_QUEST_LOG_SIZE; ++s)
            if (uint32 id = bot->GetQuestSlotQuestId(s))
            {
                Quest const* q = sObjectMgr->GetQuestTemplate(id);
                if (id == questId || (!questId && q && !arg.empty() && Lower(q->GetTitle()).find(arg) != std::string::npos))
                {
                    slot    = s;
                    questId = id;
                }
            }
        if (slot == MAX_QUEST_LOG_SIZE)
            return "that quest is not in the log";

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (errand.active && errand.questId == questId)
            AutopilotCommands_StopErrand(ai, errand);
        WorldPacket raw(CMSG_QUESTLOG_REMOVE_QUEST, 1);
        raw << uint8(slot);
        WorldPackets::Quest::QuestLogRemoveQuest packet(std::move(raw));
        packet.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(packet);
        return bot->GetQuestSlotQuestId(slot) == questId
            ? std::string("could not abandon it (a quest item it gave them cannot be taken back)")
            : "done: abandoned " + (quest ? quest->GetTitle() : std::to_string(questId));
    }

    if (StartsWithWord(lower, "quest"))
    {
        std::string arg = Trim(lower.substr(5));

        // "quest 33 reward 2": which of the quest's reward choices to take.
        uint32_t rewardChoice = 0;
        if (const size_t at = arg.find("reward"); at != std::string::npos)
        {
            try { rewardChoice = static_cast<uint32_t>(std::stoul(Trim(arg.substr(at + 6)))); }
            catch (...) { rewardChoice = 0; }
            arg = Trim(arg.substr(0, at));
        }

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
        // A quest for another class or race cannot be turned in or done; a
        // trip across the world for it would be wasted.
        if (!bot->SatisfyQuestClass(quest, false) || !bot->SatisfyQuestRace(quest, false))
            return "that quest is not for their class or race and cannot be finished; abandon " +
                   std::to_string(questId) + " to clear it";
        if (rewardChoice && rewardChoice > quest->GetRewChoiceItemsCount())
            return quest->GetRewChoiceItemsCount()
                ? SafeFormat("that quest's reward choices are 1 to {}", quest->GetRewChoiceItemsCount())
                : std::string("that quest has no reward to choose");

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
            const std::string result = StartErrand(bot, errand, AutopilotErrandKind::QuestTurnIn, 0, place.entry,
                                                   questId, place, 25.0f,
                                                   SafeFormat("{} to turn in {}", place.name, quest->GetTitle()), now);
            errand.rewardChoice = static_cast<uint8_t>(rewardChoice);
            return result;
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

namespace
{
    // "Kobold Vermin 3/8, Gold Dust 2/10"
    std::string HuntProgress(Player* bot, Quest const* quest)
    {
        std::string out;
        const uint16 slot = QuestSlot(bot, quest->GetQuestId());
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT && slot < MAX_QUEST_LOG_SIZE; ++i)
        {
            const int32 entry = quest->RequiredNpcOrGo[i];
            if (!entry || !quest->RequiredNpcOrGoCount[i])
                continue;
            std::string name = std::to_string(entry);
            if (entry > 0)
            {
                if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(uint32_t(entry)))
                    name = t->Name;
            }
            else if (GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(uint32_t(-entry)))
                name = t->name;
            out += SafeFormat("{}{} {}/{}", out.empty() ? "" : ", ", name, bot->GetQuestSlotCounter(slot, i),
                              quest->RequiredNpcOrGoCount[i]);
        }
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            const uint32 item = quest->RequiredItemId[i];
            if (!item)
                continue;
            ItemTemplate const* t = sObjectMgr->GetItemTemplate(item);
            out += SafeFormat("{}{} {}/{}", out.empty() ? "" : ", ", t ? t->Name1 : std::to_string(item),
                              std::min<uint32_t>(bot->GetItemCount(item, true), quest->RequiredItemCount[i]),
                              quest->RequiredItemCount[i]);
        }
        return out;
    }

    void EndHunt(PlayerbotAI* ai, AutopilotErrand& errand, AutopilotErrandUpdate& u, std::string note)
    {
        AutopilotBot_ClearQuestTarget(ai);
        errand.hunting = false;
        errand.active  = false;
        u.finished     = true;
        u.note         = std::move(note);
    }

    bool Contains(const std::vector<uint32_t>& list, uint32_t entry)
    {
        return std::find(list.begin(), list.end(), entry) != list.end();
    }

    // A player working a quest: the nearest thing it still needs first -- a
    // creature to kill (or that drops a quest item), an object to use, or one
    // that holds a quest item -- and, with none around, on to where more of
    // them are. Ends when the objective is done or the time runs out (the
    // model hears the progress either way).
    AutopilotErrandUpdate Hunt(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand, uint32_t now)
    {
        AutopilotErrandUpdate u;
        Quest const* quest = sObjectMgr->GetQuestTemplate(errand.questId);
        const QuestStatus status = quest ? bot->GetQuestStatus(errand.questId) : QUEST_STATUS_NONE;
        if (!quest || status != QUEST_STATUS_INCOMPLETE)
        {
            EndHunt(ai, errand, u, quest && status == QUEST_STATUS_COMPLETE
                ? SafeFormat("finished the objective of {}: ready to turn in", quest->GetTitle())
                : std::string("the quest is no longer in the log"));
            return u;
        }

        const QuestNeeds needs = NeedsOf(bot, quest);
        if (!needs.Any())
        {
            EndHunt(ai, errand, u, SafeFormat("nothing more to find for {} here ({}); the rest is something else",
                                              quest->GetTitle(), HuntProgress(bot, quest)));
            return u;
        }
        if (now >= errand.huntUntil)
        {
            EndHunt(ai, errand, u, SafeFormat("worked on {} for {} minutes, not done yet: {}", quest->GetTitle(),
                                              g_huntMinutes, HuntProgress(bot, quest)));
            return u;
        }

        // Between spawn points: the walk there.
        if (errand.trip.active)
        {
            std::string note;
            if (AutopilotTravel_Update(bot, ai, errand.trip, now, note) == AutopilotTripState::Going)
                return u;
        }

        if (bot->IsInCombat() || bot->IsNonMeleeSpellCast(false))
            return u;   // fighting (quest target first) or opening something

        const std::vector<uint32_t> creatures = needs.Creatures();
        const std::vector<uint32_t> objects   = needs.Objects();

        Creature* creature = nullptr;
        float creatureDist = 0.0f;
        {
            std::list<Creature*> nearby;
            bot->GetCreatureListWithEntryInGrid(nearby, creatures, 50.0f);
            for (Creature* c : nearby)
            {
                if (!c || !c->IsAlive() || !bot->IsValidAttackTarget(c) ||
                    (c->hasLootRecipient() && !c->isTappedBy(bot)) || !bot->IsWithinLOSInMap(c))
                    continue;
                const float d = bot->GetDistance(c);
                if (!creature || d < creatureDist)
                {
                    creature     = c;
                    creatureDist = d;
                }
            }
        }

        GameObject* object = nullptr;
        float objectDist = 0.0f;
        if (!objects.empty())
        {
            std::list<GameObject*> nearby;
            bot->GetGameObjectListWithEntryInGrid(nearby, objects, 50.0f);
            for (GameObject* o : nearby)
            {
                if (!o || !o->isSpawned() || o->GetGoState() != GO_STATE_READY ||
                    o->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) || !bot->IsWithinLOSInMap(o))
                    continue;
                const float d = bot->GetDistance(o);
                if (!object || d < objectDist)
                {
                    object     = o;
                    objectDist = d;
                }
            }
        }

        if (creature && (!object || creatureDist <= objectDist))
        {
            AutopilotBot_SetQuestTarget(ai, creature->GetGUID().GetRawValue());
            if (creatureDist > 25.0f)
            {
                if (!AutopilotMove_IsMoving(ai))
                    AutopilotMove_To(ai, creature->GetPositionX(), creature->GetPositionY(),
                                     creature->GetPositionZ(), true);
            }
            else
                AutopilotBot_EngageQuestTarget(ai);
            return u;
        }

        if (object)
        {
            AutopilotBot_ClearQuestTarget(ai);
            if (Contains(needs.lootObjects, object->GetEntry()))
            {
                // Holds a quest item: playerbots' looting walks to it and
                // opens it, taking what the bot needs.
                if (objectDist > 20.0f && !AutopilotMove_IsMoving(ai))
                    AutopilotMove_To(ai, object->GetPositionX(), object->GetPositionY(), object->GetPositionZ(), true);
                else
                    AutopilotBot_LootObject(ai, object->GetGUID().GetRawValue());
                return u;
            }
            // An objective says to use it: step up and use it, as the client
            // does (use, then report use).
            if (objectDist > INTERACTION_DISTANCE - 1.0f)
            {
                if (!AutopilotMove_IsMoving(ai))
                    AutopilotMove_To(ai, object->GetPositionX(), object->GetPositionY(), object->GetPositionZ(), true);
                return u;
            }
            if (now - errand.lastUseAt >= 3)
            {
                errand.lastUseAt = now;
                AutopilotMove_Stop(ai);
                WorldPacket use(CMSG_GAMEOBJ_USE, 8);
                use << object->GetGUID();
                bot->GetSession()->HandleGameObjectUseOpcode(use);
                WorldPacket report(CMSG_GAMEOBJ_REPORT_USE, 8);
                report << object->GetGUID();
                bot->GetSession()->HandleGameobjectReportUse(report);
            }
            return u;
        }

        // Nothing in sight: on to the next place they are.
        AutopilotBot_ClearQuestTarget(ai);
        AutopilotPlace place;
        if (!AutopilotWorld_SpawnBeyond(bot, creatures, objects, 35.0f, place))
        {
            EndHunt(ai, errand, u, SafeFormat("found no more of what {} needs nearby: {}", quest->GetTitle(),
                                              HuntProgress(bot, quest)));
            return u;
        }
        AutopilotTravel_Start(bot, { place.map, place.x, place.y, place.z }, 15.0f, 0, errand.trip, now);
        return u;
    }
}

namespace
{
    // At the bench: one craft per visit (the walk waits while it casts), until
    // the count is made or the materials run out.
    AutopilotErrandUpdate CraftStep(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand)
    {
        AutopilotErrandUpdate u;
        if (bot->IsNonMeleeSpellCast(false) || bot->IsInCombat())
            return u;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(errand.craftSpell);
        std::string made = errand.label;
        if (const size_t at = made.find("crafting "); at == 0)
            made = made.substr(9);
        if (const size_t x = made.rfind(" x"); x != std::string::npos)
            made = made.substr(0, x);

        auto finish = [&](std::string note)
        {
            errand.active = false;
            u.finished    = true;
            u.note        = std::move(note);
        };

        if (!info)
            return finish("that recipe is gone"), u;
        if (!errand.craftLeft)
            return finish(SafeFormat("crafted {} x{}", made, errand.craftDone)), u;
        if (!Craftable(bot, info))
            return finish(SafeFormat("ran out of materials for {} after {}", made, errand.craftDone)), u;

        if (bot->isMoving())
            AutopilotMove_Stop(ai);
        if (!ai->CanCastSpell(errand.craftSpell, bot, true) || !ai->CastSpell(errand.craftSpell, bot))
            return finish(SafeFormat("could not craft {} here after {} (a tool or {} missing?)", made, errand.craftDone,
                                     info->RequiresSpellFocus
                                         ? AutopilotWorld_SpellFocusName(info->RequiresSpellFocus)
                                         : std::string("something else"))), u;
        --errand.craftLeft;
        ++errand.craftDone;
        return u;
    }

    // At the object: opened through playerbots' looting (which handles the
    // gathering skills and takes what is inside), or used like the client.
    AutopilotErrandUpdate OpenStep(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand)
    {
        AutopilotErrandUpdate u;
        u.finished    = true;
        errand.active = false;
        GameObject* go = ObjectAccessor::GetGameObject(*bot, ObjectGuid(errand.objectGuid));
        if (!go || !go->isSpawned() || go->GetGoState() != GO_STATE_READY)
        {
            u.note = errand.label + " was gone or already used";
            return u;
        }
        uint32_t required = 0;
        const uint32_t skill = NodeSkill(go, required);
        if (skill && bot->GetSkillValue(skill) < required)
        {
            u.note = SafeFormat("{} needs {} {}; they have {}", errand.label, SkillName(skill), required,
                                bot->GetSkillValue(skill));
            return u;
        }
        if (go->GetGoType() == GAMEOBJECT_TYPE_CHEST)
        {
            AutopilotBot_LootObject(ai, go->GetGUID().GetRawValue());
            u.note = "opening " + errand.label + " (their looting takes what is inside)";
            return u;
        }
        WorldPacket use(CMSG_GAMEOBJ_USE, 8);
        use << go->GetGUID();
        bot->GetSession()->HandleGameObjectUseOpcode(use);
        WorldPacket report(CMSG_GAMEOBJ_REPORT_USE, 8);
        report << go->GetGUID();
        bot->GetSession()->HandleGameobjectReportUse(report);
        u.note = "used " + errand.label;
        return u;
    }
}

std::string AutopilotCommands_DescribeCraftable(Player* bot)
{
    struct Line
    {
        std::string text;
        bool        raises = false;
    };
    std::vector<Line> lines;
    for (const Recipe& r : KnownRecipes(bot))
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(r.spell);
        const uint32_t can = info ? Craftable(bot, info) : 0;
        if (!can)
            continue;
        const bool raises = RaisesSkill(bot, r.spell);
        lines.push_back({ SafeFormat("{} x{}{}{}", r.name, can, raises ? " (raises skill)" : "",
                                     info->RequiresSpellFocus
                                         ? " (at a " + AutopilotWorld_SpellFocusName(info->RequiresSpellFocus) + ")"
                                         : std::string()),
                          raises });
    }
    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.raises && !b.raises; });
    std::string out;
    for (size_t i = 0; i < lines.size() && i < 8; ++i)
        out += (out.empty() ? "" : ", ") + lines[i].text;
    if (lines.size() > 8)
        out += SafeFormat(", and {} more", lines.size() - 8);
    return out;
}

std::string AutopilotCommands_DescribeSurroundings(Player* bot)
{
    std::vector<std::string> parts;

    // Bodies: theirs to loot, and to skin.
    {
        std::list<Creature*> dead;
        DeadCreatureCheck check{ bot, 30.0f };
        Acore::CreatureListSearcher<DeadCreatureCheck> searcher(bot, dead, check);
        Cell::VisitObjects(bot, searcher, 30.0f);
        uint32_t loot = 0, skin = 0;
        const bool skinner = bot->HasSkill(SKILL_SKINNING);
        for (Creature* c : dead)
        {
            if (bot->isAllowedToLoot(c) && !c->loot.isLooted())
                ++loot;
            else if (skinner && c->HasUnitFlag(UNIT_FLAG_SKINNABLE))
                ++skin;
        }
        if (loot)
            parts.push_back(SafeFormat("{} bod{} to loot (the loot strategy takes it)", loot, loot == 1 ? "y" : "ies"));
        if (skin)
            parts.push_back(SafeFormat("{} bod{} to skin (loot strategy, with a skinning knife)", skin,
                                       skin == 1 ? "y" : "ies"));
    }

    // Objects worth a look: nodes, chests, fishing pools, crafting stations.
    struct Seen
    {
        uint32_t    count = 0;
        float       nearest = 0.0f;
        std::string note;
    };
    std::map<std::string, Seen> seen;
    for (GameObject* go : ObjectsNear(bot, 40.0f))
    {
        if (go->GetGoState() != GO_STATE_READY)
            continue;
        std::string note;
        switch (go->GetGoType())
        {
            case GAMEOBJECT_TYPE_CHEST:
            {
                uint32_t required = 0;
                if (const uint32_t skill = NodeSkill(go, required))
                {
                    const uint32_t have = bot->GetSkillValue(skill);
                    if (have >= required)
                        note = SafeFormat("they can gather it: {} {}", SkillName(skill), have);
                    else if (have)
                        note = SafeFormat("needs {} {}, they have {}", SkillName(skill), required, have);
                    else
                        note = SafeFormat("needs {}, which they do not have", SkillName(skill));
                }
                else if (!go->GetGOInfo()->GetLootId())
                    continue;   // a chest-type prop with nothing in it
                else
                    note = "can be opened";
                break;
            }
            case GAMEOBJECT_TYPE_FISHINGHOLE:
                if (!bot->HasSkill(SKILL_FISHING))
                    continue;
                note = "a fishing pool (nc +master fishing beside it)";
                break;
            case GAMEOBJECT_TYPE_SPELL_FOCUS:
                note = "a crafting station";
                break;
            default:
                continue;
        }
        Seen& s = seen[go->GetGOInfo()->name];
        const float d = bot->GetDistance(go);
        if (!s.count || d < s.nearest)
            s.nearest = d;
        ++s.count;
        s.note = note;
    }
    std::vector<std::pair<float, std::string>> objects;
    for (auto const& [name, s] : seen)
        objects.emplace_back(s.nearest, SafeFormat("{}{} ({:.0f} yd, {})", name, s.count > 1 ? SafeFormat(" x{}", s.count)
                                                                                         : std::string(),
                                                   s.nearest, s.note));
    std::sort(objects.begin(), objects.end());
    for (size_t i = 0; i < objects.size() && i < 6; ++i)
        parts.push_back(objects[i].second);

    std::string out;
    for (const std::string& p : parts)
        out += (out.empty() ? "" : "; ") + p;
    return out;
}

AutopilotErrandUpdate AutopilotCommands_UpdateErrand(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand,
                                                     uint32_t now)
{
    AutopilotErrandUpdate u;
    if (!errand.active)
        return u;
    if (errand.hunting)
        return Hunt(bot, ai, errand, now);
    // Crafting where it stands (no trip, or the trip to the station is over).
    if (errand.kind == AutopilotErrandKind::Craft && !errand.trip.active)
        return CraftStep(bot, ai, errand);

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
        {
            // Creatures to kill: stay and hunt them, as a player would.
            Quest const* quest = sObjectMgr->GetQuestTemplate(errand.questId);
            if (quest && g_huntMinutes && bot->GetQuestStatus(errand.questId) == QUEST_STATUS_INCOMPLETE &&
                NeedsOf(bot, quest).Any())
            {
                errand.active    = true;
                errand.hunting   = true;
                errand.huntUntil = now + g_huntMinutes * 60;
                errand.label     = "hunting for " + quest->GetTitle();
                u.finished       = false;
                u.note           = SafeFormat("arrived; hunting for {} ({})", quest->GetTitle(), HuntProgress(bot, quest));
                return u;
            }
            u.note = "arrived at " + errand.label;
            return u;
        }

        case AutopilotErrandKind::Craft:
            // At the station: keep the errand and craft from the next visit.
            errand.active = true;
            u.finished    = false;
            u.note        = "at the crafting station";
            return u;

        case AutopilotErrandKind::Open:
            return OpenStep(bot, ai, errand);

        case AutopilotErrandKind::QuestTurnIn:
        {
            if (!npc)
            {
                u.note = "arrived, but " + errand.label.substr(0, errand.label.find(" to turn in")) + " was not there";
                return u;
            }
            Quest const* quest = sObjectMgr->GetQuestTemplate(errand.questId);

            // The reward the model chose ("quest <id> reward <n>"), taken the
            // way a player clicks it -- before playerbots' turn-in, which
            // would pick one by its own stat weights.
            std::string chose;
            if (quest && errand.rewardChoice && errand.rewardChoice <= quest->GetRewChoiceItemsCount() &&
                !bot->GetQuestRewardStatus(errand.questId) && bot->CanRewardQuest(quest, false))
            {
                const uint32_t index = errand.rewardChoice - 1u;
                if (bot->CanRewardQuest(quest, index, false))
                {
                    bot->RewardQuest(quest, index, npc, true);
                    if (ItemTemplate const* item = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[index]))
                        chose = ", taking " + item->Name1;
                }
            }

            // Talking to the quest giver also takes any follow-up quest it offers.
            AutopilotQuest_TalkTo(ai, npc);

            // No choice from the model: playerbots picks for random bots; an
            // alt asks its master, and an autopilot bot's master is the LLM,
            // which cannot answer a whisper. Take the best usable choice.
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
                ? "turned in the quest with " + npc->GetName() + chose
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
            if (learned)
                u.note = SafeFormat("trained at {}: learned {} spell{}", errand.label, learned, learned == 1 ? "" : "s");
            else if (static_cast<AutopilotService>(errand.service) == AutopilotService::Profession)
                u.note = "nothing to learn at " + errand.label +
                         ": no free profession slot for it, too little skill for the next rank, or not enough "
                         "money (for new class spells, goto trainer)";
            else
                u.note = "nothing new to learn at " + errand.label + " at this level";
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
    if (errand.hunting)
        AutopilotBot_ClearQuestTarget(ai);
    errand.hunting = false;
    errand.active = false;
}
