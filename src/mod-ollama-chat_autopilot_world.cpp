#include "mod-ollama-chat_autopilot_world.h"
#include "mod-ollama-chat_progress.h"
#include "mod-ollama-chat-utilities.h"

#include "CreatureData.h"
#include "DBCStores.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Trainer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace
{
    struct Spawn
    {
        float    x = 0.0f, y = 0.0f, z = 0.0f;
        uint32_t entry   = 0;
        uint32_t faction = 0;   // faction template id
        uint32_t flags   = 0;   // npcflag
    };

    // map id -> service NPC spawns on it.
    std::unordered_map<uint32_t, std::vector<Spawn>> g_spawns;

    // creature entry -> every spawn of it (quest givers, enders, objectives).
    struct SpawnAt
    {
        uint32_t map = 0;
        float    x = 0.0f, y = 0.0f, z = 0.0f;
    };
    std::unordered_map<uint32_t, std::vector<SpawnAt>> g_byEntry;

    // quest id -> creature entries that take it in.
    std::unordered_map<uint32_t, std::vector<uint32_t>> g_questEnders;

    constexpr uint32_t kServiceMask =
        UNIT_NPC_FLAG_REPAIR | UNIT_NPC_FLAG_VENDOR_MASK | UNIT_NPC_FLAG_TRAINER_CLASS |
        UNIT_NPC_FLAG_TRAINER_PROFESSION | UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_FLIGHTMASTER |
        UNIT_NPC_FLAG_BANKER | UNIT_NPC_FLAG_AUCTIONEER;

    uint32_t FlagFor(AutopilotService s)
    {
        switch (s)
        {
            case AutopilotService::Repair:       return UNIT_NPC_FLAG_REPAIR;
            case AutopilotService::Vendor:       return UNIT_NPC_FLAG_VENDOR_MASK;
            case AutopilotService::Trainer:      return UNIT_NPC_FLAG_TRAINER_CLASS;
            case AutopilotService::Profession:   return UNIT_NPC_FLAG_TRAINER_PROFESSION;
            case AutopilotService::Inn:          return UNIT_NPC_FLAG_INNKEEPER;
            case AutopilotService::FlightMaster: return UNIT_NPC_FLAG_FLIGHTMASTER;
            case AutopilotService::Bank:         return UNIT_NPC_FLAG_BANKER;
            case AutopilotService::Auction:      return UNIT_NPC_FLAG_AUCTIONEER;
            default:                             return 0;
        }
    }

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Not hostile to the bot: friendly towns and neutral goblin ports both
    // serve it.
    bool Usable(Player* bot, const Spawn& s)
    {
        FactionTemplateEntry const* mine = bot->GetFactionTemplateEntry();
        FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(s.faction);
        return mine && theirs && !mine->IsHostileTo(*theirs) && !theirs->IsHostileTo(*mine);
    }

    bool Serves(Player* bot, const Spawn& s, AutopilotService service)
    {
        if (!(s.flags & FlagFor(service)) || !Usable(bot, s))
            return false;

        // A class trainer only teaches its own class.
        if (service == AutopilotService::Trainer)
            if (Trainer::Trainer* trainer = sObjectMgr->GetTrainer(s.entry))
                return trainer->IsTrainerValidForPlayer(bot);
        return true;
    }

    std::string NameOf(uint32_t entry)
    {
        if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(entry))
            return t->Name;
        return std::to_string(entry);
    }

    bool InZone(uint32_t zoneId, float x, float y)
    {
        float zx = x, zy = y;
        Map2ZoneCoordinates(zx, zy, zoneId);
        return zx >= 0.0f && zx <= 100.0f && zy >= 0.0f && zy <= 100.0f;
    }
}

const char* AutopilotService_Name(AutopilotService s)
{
    switch (s)
    {
        case AutopilotService::Repair:       return "repair";
        case AutopilotService::Vendor:       return "vendor";
        case AutopilotService::Trainer:      return "trainer";
        case AutopilotService::Profession:   return "profession trainer";
        case AutopilotService::Inn:          return "inn";
        case AutopilotService::FlightMaster: return "flightmaster";
        case AutopilotService::Bank:         return "bank";
        case AutopilotService::Auction:      return "auction house";
        default:                             return "?";
    }
}

bool AutopilotService_FromName(const std::string& raw, AutopilotService& out)
{
    const std::string n = Lower(raw);
    if (n == "repair" || n == "armorer" || n == "smith")                 { out = AutopilotService::Repair;       return true; }
    if (n == "vendor" || n == "merchant" || n == "shop" || n == "sell")  { out = AutopilotService::Vendor;       return true; }
    if (n == "trainer" || n == "class trainer")                          { out = AutopilotService::Trainer;      return true; }
    if (n == "profession" || n == "profession trainer")                  { out = AutopilotService::Profession;   return true; }
    if (n == "inn" || n == "innkeeper" || n == "tavern")                 { out = AutopilotService::Inn;          return true; }
    if (n == "flightmaster" || n == "flight master" || n == "flight")    { out = AutopilotService::FlightMaster; return true; }
    if (n == "bank" || n == "banker")                                    { out = AutopilotService::Bank;         return true; }
    if (n == "auction" || n == "auctioneer" || n == "auction house" || n == "ah") { out = AutopilotService::Auction; return true; }
    return false;
}

void AutopilotWorld_Build()
{
    g_spawns.clear();
    g_byEntry.clear();
    g_questEnders.clear();

    if (QuestRelations const* enders = sObjectMgr->GetCreatureQuestInvolvedRelationMap())
        for (auto const& [creatureEntry, questId] : *enders)
            g_questEnders[questId].push_back(creatureEntry);

    size_t count = 0;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        g_byEntry[data.id].push_back(SpawnAt{ data.mapid, data.posX, data.posY, data.posZ });

        CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(data.id);
        if (!t)
            continue;
        const uint32_t flags = (t->npcflag | data.npcflag) & kServiceMask;
        if (!flags)
            continue;

        Spawn s;
        s.x = data.posX;
        s.y = data.posY;
        s.z = data.posZ;
        s.entry   = data.id;
        s.faction = t->faction;
        s.flags   = flags;
        g_spawns[data.mapid].push_back(s);
        ++count;
    }
    LOG_INFO("server.loading", "[Ollama Chat] Autopilot: indexed {} service NPC spawns.", count);
}

bool AutopilotWorld_NearestService(Player* bot, AutopilotService service, AutopilotPlace& out)
{
    auto it = g_spawns.find(bot->GetMapId());
    if (it == g_spawns.end())
        return false;

    const Spawn* best = nullptr;
    float bestDist = 0.0f;
    for (const Spawn& s : it->second)
    {
        if (!Serves(bot, s, service))
            continue;
        const float d = bot->GetDistance(s.x, s.y, s.z);
        if (!best || d < bestDist)
        {
            best     = &s;
            bestDist = d;
        }
    }
    if (!best)
        return false;

    out.map      = bot->GetMapId();
    out.x        = best->x;
    out.y        = best->y;
    out.z        = best->z;
    out.entry    = best->entry;
    out.name     = NameOf(best->entry);
    out.distance = bestDist;
    return true;
}

bool AutopilotWorld_ZonePlace(Player* bot, uint32_t zoneId, AutopilotPlace& out, std::string& why)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
    if (!area)
    {
        why = "unknown zone";
        return false;
    }
    auto it = g_spawns.find(area->mapid);
    if (it == g_spawns.end())
    {
        why = "no known destination in that zone";
        return false;
    }

    // Prefer the friendly service NPC nearest the zone's centre: a town,
    // camp or flight point, which is somewhere a walk can actually end.
    float cx = 50.0f, cy = 50.0f;
    Zone2MapCoordinates(cx, cy, zoneId);

    const Spawn* best = nullptr;
    float bestDist = 0.0f;
    for (const Spawn& s : it->second)
    {
        if (!Usable(bot, s) || !InZone(zoneId, s.x, s.y))
            continue;
        const float d = std::hypot(s.x - cx, s.y - cy);
        if (!best || d < bestDist)
        {
            best     = &s;
            bestDist = d;
        }
    }
    if (!best)
    {
        why = "no friendly town or camp found in that zone";
        return false;
    }

    out.map      = area->mapid;
    out.x        = best->x;
    out.y        = best->y;
    out.z        = best->z;
    out.entry    = best->entry;
    out.name     = NameOf(best->entry);
    out.distance = area->mapid == bot->GetMapId() ? bot->GetDistance(best->x, best->y, best->z) : 0.0f;
    return true;
}

uint32_t AutopilotWorld_FindZone(const std::string& name, std::string& display)
{
    std::string want = Lower(name);
    want.erase(0, want.find_first_not_of(" \t"));
    want.erase(want.find_last_not_of(" \t") + 1);
    if (want.empty())
        return 0;

    uint32_t partial = 0;
    for (uint32_t i = 0; i < sAreaTableStore.GetNumRows(); ++i)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(i);
        if (!area || area->zone != 0 || !area->area_name[0] || !*area->area_name[0])
            continue;

        const std::string have = Lower(area->area_name[0]);
        if (have == want)
        {
            display = area->area_name[0];
            return area->ID;
        }
        if (!partial && (have.find(want) != std::string::npos || want.find(have) != std::string::npos))
        {
            partial = area->ID;
            display = area->area_name[0];
        }
    }
    return partial;
}

std::string AutopilotWorld_DescribeServices(Player* bot)
{
    std::string out;
    for (uint8_t i = 0; i < static_cast<uint8_t>(AutopilotService::Count); ++i)
    {
        const AutopilotService service = static_cast<AutopilotService>(i);
        AutopilotPlace place;
        if (!AutopilotWorld_NearestService(bot, service, place))
            continue;

        const uint32_t zone = sMapMgr->GetZoneId(bot->GetPhaseMask(), place.map, place.x, place.y, place.z);
        out += SafeFormat("{}{}: {} ({}, {} yd)", out.empty() ? "" : "; ", AutopilotService_Name(service),
                          place.name, Progress_ZoneName(zone), static_cast<uint32_t>(place.distance));
    }
    return out;
}

std::string AutopilotWorld_ZonesForLevel(Player* bot)
{
    const int32_t level = static_cast<int32_t>(bot->GetLevel());
    std::vector<std::pair<int32_t, std::string>> zones;
    for (uint32_t i = 0; i < sAreaTableStore.GetNumRows(); ++i)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(i);
        if (!area || area->zone != 0 || area->mapid != bot->GetMapId() || area->area_level <= 0 ||
            !area->area_name[0] || !*area->area_name[0])
            continue;
        if (area->area_level < level - 4 || area->area_level > level + 6)
            continue;
        zones.emplace_back(area->area_level, area->area_name[0]);
    }

    std::sort(zones.begin(), zones.end(), [level](const auto& a, const auto& b)
    {
        return std::abs(a.first - level) < std::abs(b.first - level);
    });

    std::string out;
    for (size_t i = 0; i < zones.size() && i < 10; ++i)
        out += SafeFormat("{}{} ({})", out.empty() ? "" : ", ", zones[i].second, zones[i].first);
    return out;
}

bool AutopilotWorld_NearestSpawn(Player* bot, uint32_t entry, AutopilotPlace& out)
{
    auto it = g_byEntry.find(entry);
    if (it == g_byEntry.end() || it->second.empty())
        return false;

    const SpawnAt* best = nullptr;
    float bestDist = 0.0f;
    for (const SpawnAt& s : it->second)
    {
        if (s.map != bot->GetMapId())
            continue;
        const float d = bot->GetDistance(s.x, s.y, s.z);
        if (!best || d < bestDist)
        {
            best     = &s;
            bestDist = d;
        }
    }
    if (!best)
        best = &it->second.front();   // another continent: the travel planner gets there

    out.map      = best->map;
    out.x        = best->x;
    out.y        = best->y;
    out.z        = best->z;
    out.entry    = entry;
    out.name     = NameOf(entry);
    out.distance = best->map == bot->GetMapId() ? bestDist : 0.0f;
    return true;
}

const std::vector<uint32_t>& AutopilotWorld_QuestEnders(uint32_t questId)
{
    static const std::vector<uint32_t> kNone;
    auto it = g_questEnders.find(questId);
    return it == g_questEnders.end() ? kNone : it->second;
}
