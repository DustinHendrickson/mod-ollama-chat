#include "mod-ollama-chat_progress.h"
#include "mod-ollama-chat-utilities.h"

#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "SharedDefines.h"

#include <algorithm>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    std::mutex                    g_mutex;
    std::vector<ProgressSnapshot> g_pendingSnapshots;
    std::vector<ProgressEvent>    g_pendingEvents;

    // Last time each bot's history was thinned. World thread only (flush).
    std::unordered_map<uint64_t, uint32_t> g_lastTrim;

    constexpr uint32_t kTrimEverySeconds = 3600;
    constexpr uint32_t kDay              = 86400;
    constexpr uint32_t kWeek             = 7 * kDay;
    constexpr size_t   kRowsPerInsert    = 200;

    // Primary and secondary professions, in the order they are listed.
    constexpr uint32_t kProfessionSkills[] = {
        SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING,
        SKILL_HERBALISM, SKILL_INSCRIPTION, SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING,
        SKILL_MINING, SKILL_SKINNING, SKILL_TAILORING,
        SKILL_COOKING, SKILL_FIRST_AID, SKILL_FISHING,
    };

    std::string Escape(std::string s)
    {
        CharacterDatabase.EscapeString(s);
        return s;
    }

    uint8_t DurabilityPct(Player* bot)
    {
        uint64_t cur = 0;
        uint64_t max = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            uint32 itemMax = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (itemMax == 0)
                continue;

            max += itemMax;
            cur += item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        }
        return max == 0 ? 100 : static_cast<uint8_t>((cur * 100) / max);
    }

    std::string Professions(Player* bot)
    {
        std::string out;
        for (uint32_t skill : kProfessionSkills)
        {
            if (!bot->HasSkill(skill))
                continue;

            if (!out.empty())
                out += ',';
            out += SafeFormat("{}:{}/{}", skill, bot->GetSkillValue(skill),
                              bot->GetPureMaxSkillValue(skill));
        }
        return out;
    }

    void AppendSnapshotInserts(CharacterDatabaseTransaction& trans,
                               const std::vector<ProgressSnapshot>& rows)
    {
        for (size_t start = 0; start < rows.size(); start += kRowsPerInsert)
        {
            std::string values;
            const size_t end = std::min(rows.size(), start + kRowsPerInsert);
            for (size_t i = start; i < end; ++i)
            {
                const ProgressSnapshot& s = rows[i];
                if (!values.empty())
                    values += ',';
                values += SafeFormat(
                    "({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, '{}')",
                    s.botGuid, s.takenAt, uint32_t(s.level), s.xp, s.money, uint32_t(s.mapId),
                    s.zoneId, s.areaId, uint32_t(s.durabilityPct), uint32_t(s.freeBagSlots),
                    uint32_t(s.questsActive), s.questsRewarded, s.killsTotal, s.deathsTotal,
                    s.questsTotal, Escape(s.professions));
            }

            trans->Append(
                "INSERT INTO mod_ollama_chat_autopilot_snapshots "
                "(bot_guid, taken_at, level, xp, money, map_id, zone_id, area_id, durability_pct, "
                "free_bag_slots, quests_active, quests_rewarded, kills_total, deaths_total, "
                "quests_total, professions) VALUES " + values);
        }
    }

    void AppendEventInserts(CharacterDatabaseTransaction& trans,
                            const std::vector<ProgressEvent>& rows)
    {
        for (size_t start = 0; start < rows.size(); start += kRowsPerInsert)
        {
            std::string values;
            const size_t end = std::min(rows.size(), start + kRowsPerInsert);
            for (size_t i = start; i < end; ++i)
            {
                const ProgressEvent& e = rows[i];
                if (!values.empty())
                    values += ',';
                values += SafeFormat("({}, {}, '{}', '{}')", e.botGuid, e.at,
                                     Escape(Utf8Truncate(e.type, 32)), Escape(Utf8Truncate(e.detail, 255)));
            }

            trans->Append("INSERT INTO mod_ollama_chat_autopilot_events "
                          "(bot_guid, at, type, detail) VALUES " + values);
        }
    }

    // Keep the first snapshot in each bucket for rows in [from, to).
    //
    // The derived table is aggregated, so MySQL materialises it rather than
    // merging it -- which is what lets a DELETE read its own table here.
    std::string ThinStatement(uint64_t botGuid, uint32_t from, uint32_t to, uint32_t bucket)
    {
        return SafeFormat(
            "DELETE s FROM mod_ollama_chat_autopilot_snapshots s "
            "JOIN (SELECT MIN(id) AS keep_id, taken_at DIV {2} AS bucket "
            "      FROM mod_ollama_chat_autopilot_snapshots "
            "      WHERE bot_guid = {0} AND taken_at >= {3} AND taken_at < {1} "
            "      GROUP BY bucket) k "
            "  ON s.taken_at DIV {2} = k.bucket AND s.id <> k.keep_id "
            "WHERE s.bot_guid = {0} AND s.taken_at >= {3} AND s.taken_at < {1}",
            botGuid, to, bucket, from);
    }

    // Delete everything older than the newest `keep` rows. When the bot has
    // fewer rows than that, the inner SELECT is empty, `id < NULL` matches
    // nothing, and nothing is deleted.
    std::string CapStatement(const char* table, uint64_t botGuid, uint32_t keep)
    {
        return SafeFormat(
            "DELETE FROM {0} WHERE bot_guid = {1} AND id < "
            "(SELECT id FROM (SELECT id FROM {0} WHERE bot_guid = {1} "
            "ORDER BY id DESC LIMIT 1 OFFSET {2}) t)",
            table, botGuid, keep > 0 ? keep - 1 : 0);
    }
}

uint32_t Progress_Now()
{
    return static_cast<uint32_t>(time(nullptr));
}

ProgressSnapshot Progress_Capture(Player* bot)
{
    ProgressSnapshot s;
    s.botGuid        = bot->GetGUID().GetRawValue();
    s.takenAt        = Progress_Now();
    s.level          = bot->GetLevel();
    s.xp             = bot->GetUInt32Value(PLAYER_XP);
    s.money          = bot->GetMoney();
    s.mapId          = static_cast<uint16_t>(bot->GetMapId());
    s.zoneId         = bot->GetZoneId();
    s.areaId         = bot->GetAreaId();
    s.durabilityPct  = DurabilityPct(bot);
    s.freeBagSlots   = static_cast<uint16_t>(std::min<uint32>(bot->GetFreeInventorySpace(), 0xFFFF));
    s.questsRewarded = static_cast<uint32_t>(bot->GetRewardedQuestCount());
    s.professions    = Professions(bot);

    uint32_t active = 0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (bot->GetQuestSlotQuestId(slot))
            ++active;
    s.questsActive = static_cast<uint8_t>(active);

    return s;
}

void Progress_QueueSnapshot(ProgressSnapshot snapshot)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pendingSnapshots.push_back(std::move(snapshot));
}

void Progress_QueueEvent(uint64_t botGuid, std::string type, std::string detail)
{
    ProgressEvent e;
    e.botGuid = botGuid;
    e.at      = Progress_Now();
    e.type    = std::move(type);
    e.detail  = std::move(detail);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_pendingEvents.push_back(std::move(e));
}

void Progress_Forget(uint64_t botGuid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::erase_if(g_pendingSnapshots, [botGuid](const ProgressSnapshot& s) { return s.botGuid == botGuid; });
    std::erase_if(g_pendingEvents, [botGuid](const ProgressEvent& e) { return e.botGuid == botGuid; });
}

void Progress_Flush(uint32_t maxSnapshotsPerBot, uint32_t maxEventsPerBot)
{
    std::vector<ProgressSnapshot> snapshots;
    std::vector<ProgressEvent>    events;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        snapshots.swap(g_pendingSnapshots);
        events.swap(g_pendingEvents);
    }

    if (snapshots.empty() && events.empty())
        return;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    AppendSnapshotInserts(trans, snapshots);
    AppendEventInserts(trans, events);

    // Thin each touched bot at most once an hour. The inserts above are in the
    // same transaction, so the trims see them.
    std::unordered_set<uint64_t> touched;
    for (const ProgressSnapshot& s : snapshots)
        touched.insert(s.botGuid);
    for (const ProgressEvent& e : events)
        touched.insert(e.botGuid);

    const uint32_t now = Progress_Now();
    for (uint64_t guid : touched)
    {
        uint32_t& last = g_lastTrim[guid];
        if (last != 0 && now - last < kTrimEverySeconds)
            continue;
        last = now;

        if (now > kDay)
            trans->Append(ThinStatement(guid, now > kWeek ? now - kWeek : 0, now - kDay, 3600));
        if (now > kWeek)
            trans->Append(ThinStatement(guid, 0, now - kWeek, kDay));
        if (maxSnapshotsPerBot > 0)
            trans->Append(CapStatement("mod_ollama_chat_autopilot_snapshots", guid, maxSnapshotsPerBot));
        if (maxEventsPerBot > 0)
            trans->Append(CapStatement("mod_ollama_chat_autopilot_events", guid, maxEventsPerBot));
    }

    CharacterDatabase.CommitTransaction(trans);
}

std::vector<ProgressEvent> Progress_LoadEvents(uint64_t botGuid, uint32_t limit)
{
    std::vector<ProgressEvent> out;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_pendingEvents.rbegin(); it != g_pendingEvents.rend() && out.size() < limit; ++it)
            if (it->botGuid == botGuid)
                out.push_back(*it);
    }

    if (out.size() >= limit)
        return out;

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT at, type, detail FROM mod_ollama_chat_autopilot_events "
            "WHERE bot_guid = {} ORDER BY id DESC LIMIT {}",
            botGuid, limit - static_cast<uint32_t>(out.size())))
    {
        do
        {
            Field* f = result->Fetch();
            ProgressEvent e;
            e.botGuid = botGuid;
            e.at      = f[0].Get<uint32>();
            e.type    = f[1].Get<std::string>();
            e.detail  = f[2].Get<std::string>();
            out.push_back(std::move(e));
        } while (result->NextRow());
    }
    return out;
}

uint8_t Progress_DurabilityPct(Player* bot)
{
    return DurabilityPct(bot);
}

const char* Progress_SnapshotColumns()
{
    return "taken_at, level, xp, money, map_id, zone_id, area_id, durability_pct, "
           "free_bag_slots, quests_active, quests_rewarded, kills_total, deaths_total, "
           "quests_total, professions";
}

ProgressSnapshot Progress_ReadSnapshot(Field* f, uint64_t botGuid)
{
    ProgressSnapshot s;
    s.botGuid        = botGuid;
    s.takenAt        = f[0].Get<uint32>();
    s.level          = f[1].Get<uint8>();
    s.xp             = f[2].Get<uint32>();
    s.money          = f[3].Get<uint32>();
    s.mapId          = f[4].Get<uint16>();
    s.zoneId         = f[5].Get<uint32>();
    s.areaId         = f[6].Get<uint32>();
    s.durabilityPct  = f[7].Get<uint8>();
    s.freeBagSlots   = f[8].Get<uint16>();
    s.questsActive   = f[9].Get<uint8>();
    s.questsRewarded = f[10].Get<uint32>();
    s.killsTotal     = f[11].Get<uint32>();
    s.deathsTotal    = f[12].Get<uint32>();
    s.questsTotal    = f[13].Get<uint32>();
    s.professions    = f[14].Get<std::string>();
    return s;
}

std::vector<ProgressSnapshot> Progress_LoadSnapshots(uint64_t botGuid, uint32_t sinceUnix,
                                                     uint32_t limit)
{
    std::vector<ProgressSnapshot> out;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT {} FROM mod_ollama_chat_autopilot_snapshots "
            "WHERE bot_guid = {} AND taken_at >= {} ORDER BY taken_at ASC LIMIT {}",
            Progress_SnapshotColumns(), botGuid, sinceUnix, limit))
    {
        do
        {
            out.push_back(Progress_ReadSnapshot(result->Fetch(), botGuid));
        } while (result->NextRow());
    }
    return out;
}

std::string Progress_DescribeProfessions(const std::string& compact)
{
    std::string out;
    for (const std::string& entry : SplitString(compact, ','))
    {
        size_t colon = entry.find(':');
        if (colon == std::string::npos)
            continue;

        uint32_t skill = 0;
        try { skill = static_cast<uint32_t>(std::stoul(entry.substr(0, colon))); }
        catch (...) { continue; }

        std::string name = std::to_string(skill);
        if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill))
            if (line->name[0] && *line->name[0])
                name = line->name[0];

        if (!out.empty())
            out += ", ";
        out += name + " " + entry.substr(colon + 1);
    }
    return out;
}

std::string Progress_ZoneName(uint32_t zoneId)
{
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId))
        if (area->area_name[0] && *area->area_name[0])
            return area->area_name[0];
    return std::to_string(zoneId);
}

const std::vector<uint32_t>& Progress_ProfessionSkills()
{
    static const std::vector<uint32_t> skills(std::begin(kProfessionSkills), std::end(kProfessionSkills));
    return skills;
}

std::string Progress_SkillName(uint32_t skillId)
{
    if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(skillId))
        if (line->name[0] && *line->name[0])
            return line->name[0];
    return std::to_string(skillId);
}

std::string Progress_Summarize(const std::vector<ProgressSnapshot>& oldestFirst)
{
    if (oldestFirst.size() < 2)
        return "";

    const ProgressSnapshot& a = oldestFirst.front();
    const ProgressSnapshot& b = oldestFirst.back();
    const uint32_t span = b.takenAt > a.takenAt ? b.takenAt - a.takenAt : 0;
    if (span < 60)
        return "";

    const std::string window = span >= 7200 ? SafeFormat("{}h", span / 3600) : SafeFormat("{}m", span / 60);

    std::string out = SafeFormat("Over the last {}: ", window);
    out += a.level == b.level ? SafeFormat("still level {}", uint32_t(b.level))
                              : SafeFormat("level {} -> {}", uint32_t(a.level), uint32_t(b.level));

    const int64_t money = int64_t(b.money) - int64_t(a.money);
    out += money >= 0 ? ", +" + Progress_FormatMoney(money) : ", " + Progress_FormatMoney(money);
    out += SafeFormat(", {} quests done, {} kills, {} deaths.",
                      b.questsTotal - a.questsTotal, b.killsTotal - a.killsTotal,
                      b.deathsTotal - a.deathsTotal);

    std::vector<uint32_t> zones;
    for (const ProgressSnapshot& s : oldestFirst)
        if (std::find(zones.begin(), zones.end(), s.zoneId) == zones.end())
            zones.push_back(s.zoneId);
    out += " Zones:";
    for (size_t i = 0; i < zones.size(); ++i)
        out += (i ? ", " : " ") + Progress_ZoneName(zones[i]);
    out += ".";

    // Profession progress, only where it moved.
    auto parse = [](const std::string& compact)
    {
        std::unordered_map<uint32_t, uint32_t> values;
        for (const std::string& entry : SplitString(compact, ','))
        {
            size_t colon = entry.find(':');
            size_t slash = entry.find('/');
            if (colon == std::string::npos)
                continue;
            try
            {
                values[static_cast<uint32_t>(std::stoul(entry.substr(0, colon)))] =
                    static_cast<uint32_t>(std::stoul(entry.substr(colon + 1, slash - colon - 1)));
            }
            catch (...) { }
        }
        return values;
    };

    const auto before = parse(a.professions);
    const auto after  = parse(b.professions);
    std::string skills;
    for (const auto& [skill, value] : after)
    {
        auto it = before.find(skill);
        const uint32_t was = it == before.end() ? 0 : it->second;
        if (value == was)
            continue;

        std::string name = std::to_string(skill);
        if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill))
            if (line->name[0] && *line->name[0])
                name = line->name[0];
        skills += SafeFormat("{}{} {} -> {}", skills.empty() ? " " : ", ", name, was, value);
    }
    if (!skills.empty())
        out += skills + ".";

    return out;
}

std::string Progress_FormatMoney(int64_t copper)
{
    const char* sign = copper < 0 ? "-" : "";
    uint64_t c = static_cast<uint64_t>(copper < 0 ? -copper : copper);
    return SafeFormat("{}{}g {}s {}c", sign, c / 10000, (c / 100) % 100, c % 100);
}
