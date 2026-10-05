#include "mod-ollama-chat_autopilot.h"
#include "mod-ollama-chat_autopilot_strategy.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_playstyle.h"
#include "mod-ollama-chat_progress.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QuestDef.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // ----------------------------------------------------------------------
    // Config. Written on the world thread by Autopilot_LoadConfig; the map
    // threads only ever read `enable` (a bool: a benign racy read).
    // ----------------------------------------------------------------------
    struct Config
    {
        bool     enable            = false;
        bool     debug             = false;
        bool     allowMasterEnroll = true;
        bool     altBots           = false;
        uint32_t randomBotPercent  = 0;
        uint32_t minLevel          = 1;
        uint32_t maxLevel          = 255;
        uint32_t maxEnrolled       = 0;     // 0 = no cap

        std::unordered_set<uint32_t>    guilds;
        std::unordered_set<uint32_t>    accounts;
        std::unordered_set<std::string> includeNames;   // lowercase
        std::unordered_set<std::string> excludeNames;   // lowercase

        uint32_t sweepIntervalMs         = 1000;
        uint32_t botsPerSweep            = 25;
        uint32_t snapshotIntervalMinutes = 30;
        uint32_t flushIntervalSeconds    = 60;
        uint32_t snapshotRetention       = 500;
        uint32_t eventRetention          = 300;
    };

    Config g_cfg;

    enum : uint8_t
    {
        MODE_RULES = 0,   // selection rules decide
        MODE_ON    = 1,   // a GM forced it on
        MODE_OFF   = 2,   // a GM forced it off
    };

    // One per bot that has ever been enrolled or had a GM decision. Bots the
    // rules never matched get no row at all, so a realm of thousands of random
    // bots does not grow a table of thousands of "no" rows.
    struct Row
    {
        uint8_t     mode        = MODE_RULES;
        bool        enrolled    = false;
        std::string source;
        std::string playstyle;
        std::string awareness;
        uint32_t    killsTotal  = 0;
        uint32_t    deathsTotal = 0;
        uint32_t    questsTotal = 0;
        uint32_t    enrolledAt  = 0;
        bool        dirty       = false;
    };

    // Per online bot. Reset at every login.
    struct Online
    {
        bool     evaluated      = false;  // rules applied since login/reload
        bool     markerOurs     = false;  // we added the strategy, not a master
        uint32_t nextSnapshotAt = 0;
        uint32_t lastSnapshotAt = 0;
        uint32_t lastZone       = 0;
    };

    // Guards everything below. The world thread holds it for the sweep and
    // commands; map threads hold it briefly in the progress hooks. Nothing
    // called while holding it calls back into this file. Lock order is this
    // mutex, then the progress queue's.
    std::mutex                           g_mutex;
    std::unordered_map<uint64_t, Row>    g_rows;
    std::unordered_map<uint64_t, Online> g_online;
    std::vector<uint64_t>                g_roster;     // round-robin order
    size_t                               g_cursor        = 0;
    uint32_t                             g_enrolledCount = 0;
    uint32_t                             g_cappedCount   = 0;   // enrolled via a capped rule

    bool     g_tablesOk    = false;
    uint32_t g_sweepTimer  = 0;
    uint32_t g_flushTimer  = 0;

    constexpr uint32_t kLogoutSnapshotMinGap = 300;

    // ----------------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------------

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::unordered_set<uint32_t> ParseIds(const std::string& text)
    {
        std::unordered_set<uint32_t> out;
        for (const std::string& token : SplitString(text, ','))
        {
            try { if (uint32_t id = static_cast<uint32_t>(std::stoul(token))) out.insert(id); }
            catch (...) { LOG_ERROR("module.ollamachat", "[Ollama Chat] Autopilot: '{}' is not an id.", token); }
        }
        return out;
    }

    std::unordered_set<std::string> ParseNames(const std::string& text)
    {
        std::unordered_set<std::string> out;
        for (const std::string& token : SplitString(text, ','))
            out.insert(Lower(token));
        return out;
    }

    std::string ZoneName(uint32_t zoneId)
    {
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId))
            if (area->area_name[0] && *area->area_name[0])
                return area->area_name[0];
        return std::to_string(zoneId);
    }

    std::string Span(uint32_t seconds)
    {
        if (seconds < 120)       return SafeFormat("{}s", seconds);
        if (seconds < 2 * 3600)  return SafeFormat("{}m", seconds / 60);
        if (seconds < 2 * 86400) return SafeFormat("{}h", seconds / 3600);
        return SafeFormat("{}d", seconds / 86400);
    }

    std::string Ago(uint32_t then, uint32_t now)
    {
        return Span(now > then ? now - then : 0) + " ago";
    }

    const char* ModeName(uint8_t mode)
    {
        switch (mode)
        {
            case MODE_ON:  return "forced on";
            case MODE_OFF: return "forced off";
            default:       return "rules";
        }
    }

    // Sources the MaxEnrolled cap applies to. Hand-picked bots (a GM command,
    // a name list, a master's request) are deliberate and never capped.
    bool IsCappedSource(const std::string& source)
    {
        return source == "random" || source == "guild" || source == "account" || source == "alt";
    }

    // Keep the two enrollment counters in step with an enrolled row. g_mutex held.
    void CountEnrolled(Row const& row, int delta)
    {
        auto bump = [delta](uint32_t& n)
        {
            if (delta > 0)
                ++n;
            else if (n > 0)
                --n;
        };

        bump(g_enrolledCount);
        if (IsCappedSource(row.source))
            bump(g_cappedCount);
    }

    PlayerbotAI* BotAI(Player* bot)
    {
        return bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    }

    bool HasMarker(PlayerbotAI* ai)
    {
        return ai->HasStrategy(AUTOPILOT_STRATEGY_NAME, BOT_STATE_NON_COMBAT);
    }

    // ----------------------------------------------------------------------
    // Selection. World thread, g_mutex held.
    // ----------------------------------------------------------------------

    // The source that enrolls this bot, or "" when nothing does. Pure: it
    // changes nothing, so `preview` can call it too.
    std::string EvaluateRules(Player* bot, PlayerbotAI* ai, Row const* row, bool markerOurs)
    {
        if (row && row->mode == MODE_ON)
            return "command";
        if (row && row->mode == MODE_OFF)
            return "";

        const std::string name = Lower(bot->GetName());
        if (g_cfg.excludeNames.count(name))
            return "";
        if (g_cfg.includeNames.count(name))
            return "name";

        // A master's request is sticky once granted, and a marker we did not
        // put there is a request. Neither is bound by the level band: both are
        // a human asking for this specific bot.
        if (g_cfg.allowMasterEnroll)
        {
            if (row && row->enrolled && row->source == "master")
                return "master";
            if (!markerOurs && HasMarker(ai))
                return "master";
        }

        const uint32_t level = bot->GetLevel();
        if (level < g_cfg.minLevel || level > g_cfg.maxLevel)
            return "";

        if (bot->GetGuildId() && g_cfg.guilds.count(bot->GetGuildId()))
            return "guild";
        if (bot->GetSession() && g_cfg.accounts.count(bot->GetSession()->GetAccountId()))
            return "account";

        if (sRandomPlayerbotMgr.IsRandomBot(bot))
        {
            // Monotonic in the percent: raising it only ever adds bots.
            if (g_cfg.randomBotPercent > 0 &&
                OllamaStableHash(bot->GetGUID().GetRawValue()) % 100 < g_cfg.randomBotPercent)
                return "random";
        }
        else if (g_cfg.altBots)
        {
            return "alt";
        }

        return "";
    }

    void TakeSnapshot(Player* bot, Row& row, Online& ob, uint32_t now)
    {
        ProgressSnapshot s = Progress_Capture(bot);
        s.killsTotal  = row.killsTotal;
        s.deathsTotal = row.deathsTotal;
        s.questsTotal = row.questsTotal;
        Progress_QueueSnapshot(std::move(s));

        // Counters are persisted alongside snapshots rather than on every
        // kill, so the row only goes dirty here.
        row.dirty         = true;
        ob.lastSnapshotAt = now;

        const uint32_t interval = std::max<uint32_t>(1, g_cfg.snapshotIntervalMinutes) * 60;
        ob.nextSnapshotAt = now + interval;
    }

    // Apply a selection result and bring the marker in line with it.
    void Apply(Player* bot, PlayerbotAI* ai, uint64_t guid, Online& ob, const std::string& source)
    {
        const uint32_t now = Progress_Now();
        auto it = g_rows.find(guid);
        const bool wasEnrolled = it != g_rows.end() && it->second.enrolled;

        bool want = !source.empty();
        if (want && !wasEnrolled && IsCappedSource(source) &&
            g_cfg.maxEnrolled > 0 && g_cappedCount >= g_cfg.maxEnrolled)
        {
            want = false;
            if (g_cfg.debug)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Autopilot: {} matched '{}' but MaxEnrolled ({}) is reached.",
                         bot->GetName(), source, g_cfg.maxEnrolled);
        }

        if (want)
        {
            Row& row = g_rows[guid];
            const bool firstEver = row.enrolledAt == 0;

            if (!row.enrolled)
            {
                row.enrolled = true;
                row.source   = source;
                if (row.playstyle.empty())
                    row.playstyle = Playstyle_Assign(guid, bot->GetGuildId());
                if (row.awareness.empty())
                    row.awareness = Awareness_Assign(guid);
                if (firstEver)
                    row.enrolledAt = now;
                CountEnrolled(row, +1);

                Progress_QueueEvent(guid, "enrolled",
                    SafeFormat("{} ({} / {})", source, row.playstyle, row.awareness));
                if (g_cfg.debug)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: enrolled {} via {} as {} / {}.",
                             bot->GetName(), source, row.playstyle, row.awareness);
            }
            else if (row.source != source)
            {
                // Already enrolled bots keep their place even if the rule
                // that holds them now is a capped one.
                CountEnrolled(row, -1);
                row.source = source;
                CountEnrolled(row, +1);
            }
            row.dirty = true;

            if (!HasMarker(ai))
                ai->ChangeStrategy(std::string("+") + AUTOPILOT_STRATEGY_NAME, BOT_STATE_NON_COMBAT);
            ob.markerOurs = true;

            // A first enrollment gets a baseline now; otherwise snapshots are
            // staggered across the interval so a wave of logins does not
            // produce a wave of writes.
            const uint32_t interval = std::max<uint32_t>(1, g_cfg.snapshotIntervalMinutes) * 60;
            if (firstEver)
                TakeSnapshot(bot, row, ob, now);
            else if (ob.nextSnapshotAt == 0)
                ob.nextSnapshotAt = now + static_cast<uint32_t>(OllamaStableHash(guid) % interval);
            return;
        }

        if (wasEnrolled)
        {
            Row& row = it->second;
            CountEnrolled(row, -1);
            row.enrolled = false;
            row.dirty    = true;

            Progress_QueueEvent(guid, "unenrolled", ModeName(row.mode));
            if (g_cfg.debug)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: unenrolled {}.", bot->GetName());
        }

        if (HasMarker(ai))
            ai->ChangeStrategy(std::string("-") + AUTOPILOT_STRATEGY_NAME, BOT_STATE_NON_COMBAT);
        ob.markerOurs = false;
    }

    void Reevaluate(Player* bot, PlayerbotAI* ai, uint64_t guid, Online& ob)
    {
        auto it = g_rows.find(guid);
        Row const* row = it == g_rows.end() ? nullptr : &it->second;
        Apply(bot, ai, guid, ob, EvaluateRules(bot, ai, row, ob.markerOurs));
        ob.evaluated = true;
    }

    // ----------------------------------------------------------------------
    // Sweep. World thread, g_mutex held.
    // ----------------------------------------------------------------------

    void VisitBot(uint64_t guid, Online& ob, uint32_t now)
    {
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(guid));
        if (!bot || !bot->IsInWorld())
            return;

        // The AI attaches inside mod-playerbots' own login hook, whose order
        // relative to ours is undefined; until then there is nothing to do.
        PlayerbotAI* ai = BotAI(bot);
        if (!ai)
            return;

        if (!ob.evaluated)
        {
            Reevaluate(bot, ai, guid, ob);
        }
        else
        {
            auto it = g_rows.find(guid);
            const bool enrolled = it != g_rows.end() && it->second.enrolled;
            const bool marker   = HasMarker(ai);

            if (enrolled && !marker)
            {
                // A playerbots reset wiped it. The table is the truth.
                ai->ChangeStrategy(std::string("+") + AUTOPILOT_STRATEGY_NAME, BOT_STATE_NON_COMBAT);
                ob.markerOurs = true;
            }
            else if (!enrolled && marker && !ob.markerOurs)
            {
                // A master asked mid-session (`nc +autopilot`). The rules
                // decide whether that is allowed, including a GM's forced off.
                Reevaluate(bot, ai, guid, ob);
            }
        }

        auto it = g_rows.find(guid);
        if (it == g_rows.end() || !it->second.enrolled)
            return;
        Row& row = it->second;

        const uint32_t zone = bot->GetZoneId();
        if (ob.lastZone != 0 && zone != ob.lastZone)
            Progress_QueueEvent(guid, "zone", ZoneName(zone));
        ob.lastZone = zone;

        if (ob.nextSnapshotAt != 0 && now >= ob.nextSnapshotAt)
            TakeSnapshot(bot, row, ob, now);
    }

    void SaveRowsLocked()
    {
        std::string values;
        uint32_t    count = 0;
        const uint32_t now = Progress_Now();

        auto flush = [&]()
        {
            if (values.empty())
                return;
            CharacterDatabase.Execute(
                "INSERT INTO mod_ollama_chat_autopilot "
                "(bot_guid, mode, enrolled, source, playstyle, awareness, kills_total, "
                "deaths_total, quests_total, enrolled_at, updated_at) VALUES " + values +
                " ON DUPLICATE KEY UPDATE mode = VALUES(mode), enrolled = VALUES(enrolled), "
                "source = VALUES(source), playstyle = VALUES(playstyle), "
                "awareness = VALUES(awareness), kills_total = VALUES(kills_total), "
                "deaths_total = VALUES(deaths_total), quests_total = VALUES(quests_total), "
                "enrolled_at = VALUES(enrolled_at), updated_at = VALUES(updated_at)");
            values.clear();
            count = 0;
        };

        for (auto& [guid, row] : g_rows)
        {
            if (!row.dirty)
                continue;

            std::string source    = row.source;
            std::string playstyle = row.playstyle;
            std::string awareness = row.awareness;
            CharacterDatabase.EscapeString(source);
            CharacterDatabase.EscapeString(playstyle);
            CharacterDatabase.EscapeString(awareness);

            if (!values.empty())
                values += ',';
            values += SafeFormat("({}, {}, {}, '{}', '{}', '{}', {}, {}, {}, {}, {})",
                                 guid, uint32_t(row.mode), row.enrolled ? 1 : 0, source,
                                 playstyle, awareness, row.killsTotal, row.deathsTotal,
                                 row.questsTotal, row.enrolledAt, now);
            row.dirty = false;

            if (++count >= 200)
                flush();
        }
        flush();
    }

    // Shared shape of every progress hook: find the enrolled row, under lock.
    template <typename Fn>
    void WithEnrolledRow(Player* player, Fn&& fn)
    {
        if (!player || !Autopilot_IsActive())
            return;

        const uint64_t guid = player->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_rows.find(guid);
        if (it == g_rows.end() || !it->second.enrolled)
            return;
        fn(guid, it->second);
    }

    // ----------------------------------------------------------------------
    // Commands. World thread.
    // ----------------------------------------------------------------------

    Player* FindBot(ChatHandler* handler, const std::string& name)
    {
        Player* bot = ObjectAccessor::FindPlayerByName(name);
        if (!bot)
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: '{}' is not online.", name));
            return nullptr;
        }
        if (!OllamaIsBotPlayer(bot) || !BotAI(bot))
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: '{}' is not a bot.", name));
            return nullptr;
        }
        return bot;
    }

    bool SetMode(ChatHandler* handler, const std::string& name, uint8_t mode)
    {
        Player* bot = FindBot(handler, name);
        if (!bot)
            return true;

        const uint64_t guid = bot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_mutex);

        Row& row  = g_rows[guid];
        row.mode  = mode;
        row.dirty = true;

        Online& ob = g_online[guid];
        if (std::find(g_roster.begin(), g_roster.end(), guid) == g_roster.end())
            g_roster.push_back(guid);

        Reevaluate(bot, BotAI(bot), guid, ob);

        Row const& after = g_rows[guid];
        handler->SendSysMessage(SafeFormat(
            "OllamaChat: {} is now {} (mode: {}{}).", bot->GetName(),
            after.enrolled ? "on autopilot" : "not on autopilot", ModeName(after.mode),
            after.enrolled ? SafeFormat(", source: {}, {} / {}", after.source, after.playstyle,
                                        after.awareness)
                           : std::string()));
        if (!Autopilot_IsActive())
            handler->SendSysMessage("OllamaChat: note - autopilot is not active, see `.ollama autopilot status`.");
        return true;
    }

    bool HandleOn(ChatHandler* handler, std::string name)    { return SetMode(handler, name, MODE_ON); }
    bool HandleOff(ChatHandler* handler, std::string name)   { return SetMode(handler, name, MODE_OFF); }
    bool HandleRules(ChatHandler* handler, std::string name) { return SetMode(handler, name, MODE_RULES); }

    void SendInactiveReasons(ChatHandler* handler)
    {
        if (!g_cfg.enable)
            handler->SendSysMessage("  - OllamaChat.Autopilot.Enable is 0");
        if (!g_EnableChatBotSnapshotTemplate)
            handler->SendSysMessage("  - OllamaChat.EnableChatBotSnapshotTemplate is 0 (required)");
        if (!g_tablesOk)
            handler->SendSysMessage("  - autopilot tables are missing (apply data/sql/characters/base/2026_10_05_autopilot.sql)");
        if (!AutopilotStrategy_IsRegistered())
            handler->SendSysMessage("  - the 'autopilot' playerbots strategy failed to register (see startup log)");
    }

    bool HandleStatus(ChatHandler* handler, Optional<std::string> name)
    {
        if (!name)
        {
            std::lock_guard<std::mutex> lock(g_mutex);

            uint32_t onlineEnrolled = 0;
            for (uint64_t guid : g_roster)
            {
                auto it = g_rows.find(guid);
                if (it != g_rows.end() && it->second.enrolled)
                    ++onlineEnrolled;
            }

            handler->SendSysMessage(SafeFormat("OllamaChat Autopilot: {}",
                                               Autopilot_IsActive() ? "active" : "INACTIVE"));
            if (!Autopilot_IsActive())
                SendInactiveReasons(handler);
            handler->SendSysMessage(SafeFormat(
                "  enrolled: {} total, {} online | bots tracked online: {} | rule-enrolled {} of cap {}",
                g_enrolledCount, onlineEnrolled, g_roster.size(), g_cappedCount,
                g_cfg.maxEnrolled ? std::to_string(g_cfg.maxEnrolled) : std::string("none")));
            handler->SendSysMessage(SafeFormat(
                "  sweep: {} bots every {} ms (full cycle ~{}s) | snapshots every {} min",
                g_cfg.botsPerSweep, g_cfg.sweepIntervalMs,
                g_cfg.botsPerSweep ? (g_roster.size() / std::max<uint32_t>(1, g_cfg.botsPerSweep) + 1) *
                                         g_cfg.sweepIntervalMs / 1000
                                   : 0,
                g_cfg.snapshotIntervalMinutes));
            return true;
        }

        Player* bot = FindBot(handler, *name);
        if (!bot)
            return true;

        const uint64_t guid = bot->GetGUID().GetRawValue();
        const uint32_t now  = Progress_Now();
        Row row;
        bool hasRow = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_rows.find(guid);
            if (it != g_rows.end())
            {
                row    = it->second;
                hasRow = true;
            }
        }

        if (!hasRow)
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: {} has never been on autopilot.", bot->GetName()));
            return true;
        }

        PlayerbotAI* ai = BotAI(bot);
        handler->SendSysMessage(SafeFormat(
            "OllamaChat: {} - {} (mode: {}, source: {}) | marker strategy: {}",
            bot->GetName(), row.enrolled ? "ON autopilot" : "not on autopilot", ModeName(row.mode),
            row.source.empty() ? "-" : row.source, ai && HasMarker(ai) ? "present" : "absent"));
        handler->SendSysMessage(SafeFormat(
            "  playstyle: {} | awareness: {} | enrolled since: {}",
            row.playstyle, row.awareness, row.enrolledAt ? Ago(row.enrolledAt, now) : "-"));
        handler->SendSysMessage(SafeFormat(
            "  since enrollment: {} kills, {} deaths, {} quests completed",
            row.killsTotal, row.deathsTotal, row.questsTotal));

        std::vector<ProgressSnapshot> snaps = Progress_LoadSnapshots(guid, now > 86400 ? now - 86400 : 0, 2000);
        if (snaps.empty())
        {
            handler->SendSysMessage("  no snapshots in the last 24h yet.");
            return true;
        }

        const ProgressSnapshot& last  = snaps.back();
        const ProgressSnapshot& first = snaps.front();
        handler->SendSysMessage(SafeFormat(
            "  latest ({}): level {}, {}, {} | durability {}% | {} free bag slots | {} quests in log",
            Ago(last.takenAt, now), uint32_t(last.level), Progress_FormatMoney(last.money),
            ZoneName(last.zoneId), uint32_t(last.durabilityPct), uint32_t(last.freeBagSlots),
            uint32_t(last.questsActive)));
        if (!last.professions.empty())
            handler->SendSysMessage("  professions: " + Progress_DescribeProfessions(last.professions));
        if (snaps.size() > 1)
            handler->SendSysMessage(SafeFormat(
                "  last {} ({} snapshots): {:+} levels, {}, {} quests, {} kills, {} deaths",
                Span(now > first.takenAt ? now - first.takenAt : 0), snaps.size(),
                int(last.level) - int(first.level),
                Progress_FormatMoney(int64_t(last.money) - int64_t(first.money)),
                last.questsTotal - first.questsTotal, last.killsTotal - first.killsTotal,
                last.deathsTotal - first.deathsTotal));
        return true;
    }

    bool HandleHistory(ChatHandler* handler, std::string name, Optional<uint32> count)
    {
        Player* bot = FindBot(handler, name);
        if (!bot)
            return true;

        const uint32_t limit = std::clamp<uint32_t>(count.value_or(15), 1, 100);
        const uint32_t now   = Progress_Now();
        std::vector<ProgressEvent> events = Progress_LoadEvents(bot->GetGUID().GetRawValue(), limit);
        if (events.empty())
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: no autopilot history for {}.", bot->GetName()));
            return true;
        }

        handler->SendSysMessage(SafeFormat("OllamaChat: last {} events for {} (newest first):",
                                           events.size(), bot->GetName()));
        for (const ProgressEvent& e : events)
            handler->SendSysMessage(SafeFormat("  [{}] {}: {}", Ago(e.at, now), e.type, e.detail));
        return true;
    }

    bool HandlePreview(ChatHandler* handler)
    {
        std::unordered_map<std::string, uint32_t> bySource;
        uint32_t bots = 0, matched = 0, matchedCapped = 0, enrolledNow = 0;

        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto const& [ptrGuid, player] : ObjectAccessor::GetPlayers())
        {
            if (!player || !player->IsInWorld() || !OllamaIsBotPlayer(player))
                continue;
            PlayerbotAI* ai = BotAI(player);
            if (!ai)
                continue;

            ++bots;
            const uint64_t guid = player->GetGUID().GetRawValue();
            auto rowIt = g_rows.find(guid);
            Row const* row = rowIt == g_rows.end() ? nullptr : &rowIt->second;
            if (row && row->enrolled)
                ++enrolledNow;

            auto onIt = g_online.find(guid);
            const bool markerOurs = onIt != g_online.end() && onIt->second.markerOurs;

            const std::string source = EvaluateRules(player, ai, row, markerOurs);
            if (source.empty())
                continue;

            ++matched;
            ++bySource[source];
            if (IsCappedSource(source))
                ++matchedCapped;
        }

        uint32_t wouldEnroll = matched;
        if (g_cfg.maxEnrolled > 0 && matchedCapped > g_cfg.maxEnrolled)
            wouldEnroll = matched - (matchedCapped - g_cfg.maxEnrolled);

        handler->SendSysMessage(SafeFormat(
            "OllamaChat Autopilot preview: {} bots online, rules match {}, currently enrolled {}.",
            bots, matched, enrolledNow));
        for (auto const& [source, n] : bySource)
            handler->SendSysMessage(SafeFormat("  {}: {}", source, n));
        if (wouldEnroll != matched)
            handler->SendSysMessage(SafeFormat("  MaxEnrolled {} limits that to ~{}.", g_cfg.maxEnrolled, wouldEnroll));

        const uint32_t interval = std::max<uint32_t>(1, g_cfg.snapshotIntervalMinutes);
        handler->SendSysMessage(SafeFormat(
            "  diary load: ~{} snapshot rows/hour; history settles at <= {} snapshots and {} events per bot.",
            wouldEnroll * 60 / interval, g_cfg.snapshotRetention, g_cfg.eventRetention));
        handler->SendSysMessage(
            "  LLM planning is not wired up yet (phase 2), so enrollment costs no LLM calls today.");
        if (!Autopilot_IsActive())
        {
            handler->SendSysMessage("  Autopilot is not active:");
            SendInactiveReasons(handler);
        }
        return true;
    }

    bool HandlePlaystyle(ChatHandler* handler, std::string name, Optional<std::string> style)
    {
        Player* bot = FindBot(handler, name);
        if (!bot)
            return true;

        std::string all;
        for (const std::string& s : Playstyle_List())
            all += (all.empty() ? "" : ", ") + s;

        const uint64_t guid = bot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_rows.find(guid);
        if (!style)
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: {} playstyle: {} (available: {})", bot->GetName(),
                                               it == g_rows.end() || it->second.playstyle.empty()
                                                   ? "-" : it->second.playstyle, all));
            return true;
        }
        if (!Playstyle_Exists(*style))
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: unknown playstyle '{}'. Available: {}", *style, all));
            return true;
        }
        if (it == g_rows.end())
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: {} has never been on autopilot; enroll it first.", bot->GetName()));
            return true;
        }

        it->second.playstyle = Lower(*style);
        it->second.dirty     = true;
        Progress_QueueEvent(guid, "playstyle", it->second.playstyle + " (set by GM)");
        handler->SendSysMessage(SafeFormat("OllamaChat: {} playstyle set to {}.", bot->GetName(), it->second.playstyle));
        return true;
    }

    bool HandleAwareness(ChatHandler* handler, std::string name, Optional<std::string> level)
    {
        Player* bot = FindBot(handler, name);
        if (!bot)
            return true;

        std::string all;
        for (const std::string& s : Awareness_List())
            all += (all.empty() ? "" : ", ") + s;

        const uint64_t guid = bot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_rows.find(guid);
        if (!level)
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: {} awareness: {} (available: {})", bot->GetName(),
                                               it == g_rows.end() || it->second.awareness.empty()
                                                   ? "-" : it->second.awareness, all));
            return true;
        }
        if (!Awareness_Allowed(*level))
        {
            handler->SendSysMessage(SafeFormat(
                "OllamaChat: awareness '{}' is not available{}. Available: {}", *level,
                g_RoleplayEnable && g_RoleplayStrictness >= 2 ? " at roleplay strictness 2 (immersed only)" : "",
                all));
            return true;
        }
        if (it == g_rows.end())
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: {} has never been on autopilot; enroll it first.", bot->GetName()));
            return true;
        }

        it->second.awareness = Lower(*level);
        it->second.dirty     = true;
        Progress_QueueEvent(guid, "awareness", it->second.awareness + " (set by GM)");
        handler->SendSysMessage(SafeFormat("OllamaChat: {} awareness set to {}.", bot->GetName(), it->second.awareness));
        return true;
    }
}

// ==========================================================================
// Lifecycle
// ==========================================================================

void Autopilot_LoadConfig()
{
    Config c;
    c.enable            = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.Enable", false);
    c.debug             = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.Debug", false);
    c.allowMasterEnroll = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.AllowMasterEnroll", true);
    c.altBots           = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.Select.AltBots", false);
    c.randomBotPercent  = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Select.RandomBotPercent", 0));
    c.minLevel          = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Select.MinLevel", 1);
    c.maxLevel          = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Select.MaxLevel", 255);
    c.maxEnrolled       = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.MaxEnrolled", 0);
    c.guilds            = ParseIds(sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.Select.Guilds", ""));
    c.accounts          = ParseIds(sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.Select.Accounts", ""));
    c.includeNames      = ParseNames(sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.Select.Include", ""));
    c.excludeNames      = ParseNames(sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.Select.Exclude", ""));

    c.sweepIntervalMs         = std::max<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.SweepIntervalMs", 1000));
    c.botsPerSweep            = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.BotsPerSweep", 25));
    c.snapshotIntervalMinutes = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.SnapshotIntervalMinutes", 30));
    c.flushIntervalSeconds    = std::max<uint32_t>(5, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.FlushIntervalSeconds", 60));
    c.snapshotRetention       = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.SnapshotRetention", 500);
    c.eventRetention          = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.EventRetention", 300);

    if (c.minLevel > c.maxLevel)
        std::swap(c.minLevel, c.maxLevel);

    Playstyle_LoadConfig();

    std::lock_guard<std::mutex> lock(g_mutex);
    g_cfg = std::move(c);

    // Rules may have changed: re-evaluate every online bot on its next visit.
    for (auto& [guid, ob] : g_online)
        ob.evaluated = false;

    if (g_cfg.enable && !g_EnableChatBotSnapshotTemplate)
        LOG_WARN("module.ollamachat",
                 "[Ollama Chat] OllamaChat.Autopilot.Enable is on, but autopilot needs "
                 "OllamaChat.EnableChatBotSnapshotTemplate = 1. Autopilot stays off.");
}

void Autopilot_Load()
{
    AutopilotStrategy_Register();

    g_tablesOk = false;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() "
            "AND table_name IN ('mod_ollama_chat_autopilot', 'mod_ollama_chat_autopilot_snapshots', "
            "'mod_ollama_chat_autopilot_events')"))
        g_tablesOk = (*result)[0].Get<uint64>() == 3;

    if (!g_tablesOk)
    {
        if (g_cfg.enable)
            LOG_ERROR("server.loading",
                      "[Ollama Chat] Autopilot tables are missing; apply "
                      "data/sql/characters/base/2026_10_05_autopilot.sql. Autopilot stays off.");
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_rows.clear();
    g_enrolledCount = 0;
    g_cappedCount   = 0;

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT bot_guid, mode, enrolled, source, playstyle, awareness, kills_total, "
            "deaths_total, quests_total, enrolled_at FROM mod_ollama_chat_autopilot"))
    {
        do
        {
            Field* f = result->Fetch();
            Row row;
            row.mode        = f[1].Get<uint8>();
            row.enrolled    = f[2].Get<uint8>() != 0;
            row.source      = f[3].Get<std::string>();
            row.playstyle   = f[4].Get<std::string>();
            row.awareness   = f[5].Get<std::string>();
            row.killsTotal  = f[6].Get<uint32>();
            row.deathsTotal = f[7].Get<uint32>();
            row.questsTotal = f[8].Get<uint32>();
            row.enrolledAt  = f[9].Get<uint32>();
            if (row.enrolled)
                CountEnrolled(row, +1);
            g_rows.emplace(f[0].Get<uint64>(), std::move(row));
        } while (result->NextRow());
    }

    LOG_INFO("server.loading", "[Ollama Chat] Autopilot: loaded {} rows, {} enrolled.",
             g_rows.size(), g_enrolledCount);
}

bool Autopilot_IsActive()
{
    return g_cfg.enable && g_EnableChatBotSnapshotTemplate && g_tablesOk &&
           AutopilotStrategy_IsRegistered();
}

bool Autopilot_IsEnrolled(uint64_t botGuid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_rows.find(botGuid);
    return it != g_rows.end() && it->second.enrolled;
}

void Autopilot_Update(uint32_t diff)
{
    if (!g_tablesOk)
        return;

    g_flushTimer += diff;
    if (g_flushTimer >= g_cfg.flushIntervalSeconds * 1000)
    {
        g_flushTimer = 0;
        Autopilot_SaveAll();
    }

    if (!Autopilot_IsActive())
        return;

    g_sweepTimer += diff;
    if (g_sweepTimer < g_cfg.sweepIntervalMs)
        return;
    g_sweepTimer = 0;

    const uint32_t now = Progress_Now();
    std::lock_guard<std::mutex> lock(g_mutex);

    // A fixed number of bots per sweep, round-robin: the cost per tick is the
    // same whether ten bots are online or ten thousand.
    const size_t visits = std::min<size_t>(g_cfg.botsPerSweep, g_roster.size());
    for (size_t i = 0; i < visits; ++i)
    {
        if (g_cursor >= g_roster.size())
            g_cursor = 0;

        const uint64_t guid = g_roster[g_cursor++];
        auto it = g_online.find(guid);
        if (it != g_online.end())
            VisitBot(guid, it->second, now);
    }
}

void Autopilot_SaveAll()
{
    if (!g_tablesOk)
        return;

    uint32_t snapshotRetention;
    uint32_t eventRetention;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        SaveRowsLocked();
        snapshotRetention = g_cfg.snapshotRetention;
        eventRetention    = g_cfg.eventRetention;
    }
    Progress_Flush(snapshotRetention, eventRetention);
}

// ==========================================================================
// Commands
// ==========================================================================

ChatCommandTable const& Autopilot_CommandTable()
{
    static ChatCommandTable table =
    {
        { "on",        HandleOn,        SEC_ADMINISTRATOR, Console::Yes },
        { "off",       HandleOff,       SEC_ADMINISTRATOR, Console::Yes },
        { "rules",     HandleRules,     SEC_ADMINISTRATOR, Console::Yes },
        { "status",    HandleStatus,    SEC_ADMINISTRATOR, Console::Yes },
        { "history",   HandleHistory,   SEC_ADMINISTRATOR, Console::Yes },
        { "preview",   HandlePreview,   SEC_ADMINISTRATOR, Console::Yes },
        { "playstyle", HandlePlaystyle, SEC_ADMINISTRATOR, Console::Yes },
        { "awareness", HandleAwareness, SEC_ADMINISTRATOR, Console::Yes },
    };
    return table;
}

// ==========================================================================
// Hooks
// ==========================================================================

AutopilotPlayerScript::AutopilotPlayerScript()
    : PlayerScript("AutopilotPlayerScript", {
          PLAYERHOOK_ON_LOGIN,
          PLAYERHOOK_ON_LOGOUT,
          PLAYERHOOK_ON_CREATURE_KILL,
          PLAYERHOOK_ON_PLAYER_JUST_DIED,
          PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
          PLAYERHOOK_ON_LEVEL_CHANGED,
          PLAYERHOOK_ON_STORE_NEW_ITEM,
          PLAYERHOOK_ON_ACHI_COMPLETE,
      }) { }

void AutopilotPlayerScript::OnPlayerLogin(Player* player)
{
    // Tracked even while autopilot is off, so turning it on with a reload
    // picks up bots that are already online. Session test, not the AI lookup:
    // the AI may not be attached yet (see OllamaIsBotPlayer).
    if (!player || !OllamaIsBotPlayer(player))
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_online[guid] = Online();
    if (std::find(g_roster.begin(), g_roster.end(), guid) == g_roster.end())
        g_roster.push_back(guid);
}

void AutopilotPlayerScript::OnPlayerLogout(Player* player)
{
    if (!player)
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);

    auto onIt = g_online.find(guid);
    if (onIt == g_online.end())
        return;

    // Close the session with a snapshot unless one is very recent, so the
    // counters gathered since the last one are not lost.
    auto rowIt = g_rows.find(guid);
    const uint32_t now = Progress_Now();
    if (Autopilot_IsActive() && rowIt != g_rows.end() && rowIt->second.enrolled &&
        now - onIt->second.lastSnapshotAt >= kLogoutSnapshotMinGap)
    {
        TakeSnapshot(player, rowIt->second, onIt->second, now);
    }

    g_online.erase(onIt);

    auto pos = std::find(g_roster.begin(), g_roster.end(), guid);
    if (pos != g_roster.end())
    {
        const size_t index = static_cast<size_t>(pos - g_roster.begin());
        *pos = g_roster.back();
        g_roster.pop_back();
        // The swapped-in bot now sits where the cursor already passed; step
        // back so it is not skipped for a whole cycle.
        if (index < g_cursor && g_cursor > 0)
            --g_cursor;
    }
}

void AutopilotPlayerScript::OnPlayerCreatureKill(Player* killer, Creature* /*victim*/)
{
    WithEnrolledRow(killer, [](uint64_t, Row& row) { ++row.killsTotal; });
}

void AutopilotPlayerScript::OnPlayerJustDied(Player* player)
{
    WithEnrolledRow(player, [player](uint64_t guid, Row& row)
    {
        ++row.deathsTotal;
        Progress_QueueEvent(guid, "death", ZoneName(player->GetZoneId()));
    });
}

void AutopilotPlayerScript::OnPlayerCompleteQuest(Player* player, Quest const* quest)
{
    if (!quest)
        return;

    WithEnrolledRow(player, [quest](uint64_t guid, Row& row)
    {
        ++row.questsTotal;
        Progress_QueueEvent(guid, "quest", quest->GetTitle());
    });
}

void AutopilotPlayerScript::OnPlayerLevelChanged(Player* player, uint8 oldLevel)
{
    WithEnrolledRow(player, [player, oldLevel](uint64_t guid, Row&)
    {
        Progress_QueueEvent(guid, "level",
                            SafeFormat("{} -> {}", uint32_t(oldLevel), uint32_t(player->GetLevel())));

        // A level is a natural moment for a snapshot: take one on the next visit.
        auto it = g_online.find(guid);
        if (it != g_online.end())
            it->second.nextSnapshotAt = 1;
    });
}

void AutopilotPlayerScript::OnPlayerStoreNewItem(Player* player, Item* item, uint32 /*count*/)
{
    if (!item || !item->GetTemplate() || item->GetTemplate()->Quality < ITEM_QUALITY_RARE)
        return;

    ItemTemplate const* tmpl = item->GetTemplate();
    WithEnrolledRow(player, [tmpl](uint64_t guid, Row&)
    {
        const char* quality = tmpl->Quality >= ITEM_QUALITY_LEGENDARY ? "legendary"
                            : tmpl->Quality == ITEM_QUALITY_EPIC      ? "epic"
                                                                      : "rare";
        Progress_QueueEvent(guid, "loot", SafeFormat("{} ({})", tmpl->Name1, quality));
    });
}

void AutopilotPlayerScript::OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement)
{
    if (!achievement || !achievement->name[0])
        return;

    WithEnrolledRow(player, [achievement](uint64_t guid, Row&)
    {
        Progress_QueueEvent(guid, "achievement", achievement->name[0]);
    });
}
