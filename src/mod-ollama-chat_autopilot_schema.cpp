#include "mod-ollama-chat_autopilot_schema.h"
#include "mod-ollama-chat-utilities.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "QueryResult.h"
#include "Log.h"

#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <map>
#include <vector>

namespace
{
    struct Column
    {
        const char* name;
        const char* definition;
    };

    struct Table
    {
        const char*         name;
        const char*         key;        // primary key column; without it the table is not ours to repair
        std::vector<Column> columns;    // in order, key first
        const char*         indexes;    // appended to CREATE TABLE
    };

    // Keep in step with data/sql/characters/base/2026_10_05_autopilot.sql.
    const std::vector<Table>& Layout()
    {
        static const std::vector<Table> tables = {
            {
                "mod_ollama_chat_autopilot", "bot_guid",
                {
                    { "bot_guid",       "BIGINT UNSIGNED NOT NULL PRIMARY KEY" },
                    { "mode",           "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "enrolled",       "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "source",         "VARCHAR(32) NOT NULL DEFAULT ''" },
                    { "style",          "VARCHAR(64) NOT NULL DEFAULT ''" },
                    { "outlook",        "VARCHAR(32) NOT NULL DEFAULT ''" },
                    { "profile",        "TEXT NULL" },
                    { "kills_total",    "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "deaths_total",   "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "quests_total",   "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "dungeons_total", "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "doing",          "VARCHAR(64) NOT NULL DEFAULT ''" },
                    { "doing_since",    "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "decided_by",     "VARCHAR(16) NOT NULL DEFAULT ''" },
                    { "strategies",     "VARCHAR(2000) NOT NULL DEFAULT ''" },
                    { "last_results",   "VARCHAR(2000) NOT NULL DEFAULT ''" },
                    { "goal_kind",      "VARCHAR(24) NOT NULL DEFAULT ''" },
                    { "goal_target",    "VARCHAR(64) NOT NULL DEFAULT ''" },
                    { "goal_target_id", "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "goal_value",     "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "goal_baseline",  "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "goal_set_at",    "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "goal_text",      "VARCHAR(255) NOT NULL DEFAULT ''" },
                    { "last_reason",    "VARCHAR(255) NOT NULL DEFAULT ''" },
                    { "baseline",       "VARCHAR(4000) NOT NULL DEFAULT ''" },
                    { "plan_until",     "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "last_plan_at",   "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "enrolled_at",    "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "updated_at",     "INT UNSIGNED NOT NULL DEFAULT 0" },
                },
                "INDEX idx_enrolled (enrolled)"
            },
            {
                "mod_ollama_chat_autopilot_snapshots", "id",
                {
                    { "id",              "BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY" },
                    { "bot_guid",        "BIGINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "taken_at",        "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "level",           "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "xp",              "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "money",           "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "map_id",          "SMALLINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "zone_id",         "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "area_id",         "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "durability_pct",  "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "free_bag_slots",  "SMALLINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "quests_active",   "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "quests_rewarded", "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "kills_total",     "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "deaths_total",    "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "quests_total",    "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "professions",     "VARCHAR(255) NOT NULL DEFAULT ''" },
                },
                "INDEX idx_bot_time (bot_guid, taken_at)"
            },
            {
                "mod_ollama_chat_autopilot_events", "id",
                {
                    { "id",       "BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY" },
                    { "bot_guid", "BIGINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "at",       "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "type",     "VARCHAR(32) NOT NULL DEFAULT ''" },
                    { "detail",   "VARCHAR(255) NOT NULL DEFAULT ''" },
                },
                "INDEX idx_bot_id (bot_guid, id)"
            },
        };
        return tables;
    }

    bool TableExists(const char* table)
    {
        QueryResult r = CharacterDatabase.Query(SafeFormat(
            "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = '{}'",
            table));
        return r && (*r)[0].Get<uint64>() > 0;
    }

    // Column name -> maximum character length (0 for non-text columns).
    std::map<std::string, uint64_t> ColumnsOf(const char* table)
    {
        std::map<std::string, uint64_t> out;
        if (QueryResult r = CharacterDatabase.Query(SafeFormat(
                "SELECT column_name, IFNULL(character_maximum_length, 0) FROM information_schema.columns "
                "WHERE table_schema = DATABASE() AND table_name = '{}'", table)))
            do { out[(*r)[0].Get<std::string>()] = (*r)[1].Get<uint64>(); } while (r->NextRow());
        return out;
    }

    // "VARCHAR(2000) ..." -> 2000; 0 when the definition is not a VARCHAR.
    uint64_t VarcharLength(const char* definition)
    {
        const std::string d = definition;
        if (d.rfind("VARCHAR(", 0) != 0)
            return 0;
        return std::strtoull(d.c_str() + 8, nullptr, 10);
    }

    void Create(const Table& t)
    {
        std::string sql = SafeFormat("CREATE TABLE IF NOT EXISTS {} (", t.name);
        for (const Column& c : t.columns)
            sql += SafeFormat("`{}` {}, ", c.name, c.definition);
        sql += SafeFormat("{}) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci", t.indexes);
        CharacterDatabase.DirectExecute(sql);
    }

    void Note(const std::string& what)
    {
        LOG_INFO("server.loading", "[Ollama Chat] Autopilot schema: {}", what);
    }
}

bool AutopilotSchema_Ensure(std::string& problem)
{
    for (const Table& t : Layout())
    {
        if (!TableExists(t.name))
        {
            Create(t);
            Note(SafeFormat("created {}", t.name));
            continue;
        }

        const std::map<std::string, uint64_t> have = ColumnsOf(t.name);

        // Without its key column the table cannot be repaired in place: move it
        // aside (nothing is deleted) and start a fresh one.
        if (!have.count(t.key))
        {
            const std::string aside = SafeFormat("{}_old_{}", t.name, uint64_t(std::time(nullptr)));
            CharacterDatabase.DirectExecute(SafeFormat("RENAME TABLE {} TO {}", t.name, aside));
            Create(t);
            Note(SafeFormat("{} had an unrecognised layout; kept it as {} and created a new one", t.name, aside));
            continue;
        }

        for (const Column& c : t.columns)
        {
            auto it = have.find(c.name);
            if (it == have.end())
            {
                CharacterDatabase.DirectExecute(SafeFormat("ALTER TABLE {} ADD COLUMN `{}` {}", t.name, c.name,
                                                           c.definition));
                Note(SafeFormat("added {}.{}", t.name, c.name));
            }
            else if (const uint64_t want = VarcharLength(c.definition); want && it->second < want)
            {
                // Too short (an earlier draft) would make every save fail
                // under strict mode once a value outgrew it.
                CharacterDatabase.DirectExecute(SafeFormat("ALTER TABLE {} MODIFY COLUMN `{}` {}", t.name, c.name,
                                                           c.definition));
                Note(SafeFormat("widened {}.{} to {} characters", t.name, c.name, want));
            }
        }

        for (const auto& [name, length] : have)
        {
            const bool wanted = std::any_of(t.columns.begin(), t.columns.end(),
                                            [&](const Column& c) { return name == c.name; });
            if (!wanted)
            {
                CharacterDatabase.DirectExecute(SafeFormat("ALTER TABLE {} DROP COLUMN `{}`", t.name, name));
                Note(SafeFormat("dropped {}.{} (from an earlier draft)", t.name, name));
            }
        }
    }

    // Check the result rather than trust the statements: a failed ALTER is
    // logged by the database layer, and here it must stop autopilot.
    for (const Table& t : Layout())
    {
        const std::map<std::string, uint64_t> have = ColumnsOf(t.name);
        for (const Column& c : t.columns)
            if (!have.count(c.name))
            {
                problem = SafeFormat("{}.{} is still missing after repair (see the database errors above)",
                                     t.name, c.name);
                return false;
            }
    }
    return true;
}
