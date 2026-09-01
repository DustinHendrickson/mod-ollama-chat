#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat-utilities.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <mutex>
#include <unordered_map>

namespace
{
    std::mutex g_mutex;

    struct OllamaMemoryState
    {
        std::vector<BotMemoryEntry>                    memories;
        std::unordered_map<uint64_t, BotRelationship>  relationships;
        bool condensing = false;    // a job is already in flight
        std::unordered_map<uint64_t, bool> relationshipPending;
        bool dirty = false;
    };

    std::unordered_map<uint64_t, OllamaMemoryState> g_state;

    std::string Escape(std::string v)
    {
        CharacterDatabase.EscapeString(v);
        return v;
    }

    uint64_t NowSeconds()
    {
        return static_cast<uint64_t>(time(nullptr));
    }

    // Count whole-word, case-insensitive occurrences of `name` in `text`.
    uint32_t CountMentions(const std::string& text, const std::string& name)
    {
        if (name.empty() || text.empty())
            return 0;

        auto lower = [](std::string v)
        {
            for (char& c : v)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return v;
        };

        const std::string hay = lower(text);
        const std::string needle = lower(name);

        uint32_t count = 0;
        size_t pos = 0;
        while ((pos = hay.find(needle, pos)) != std::string::npos)
        {
            const bool startOk = (pos == 0) ||
                                 !std::isalnum(static_cast<unsigned char>(hay[pos - 1]));
            const size_t end = pos + needle.size();
            const bool endOk = (end >= hay.size()) ||
                               !std::isalnum(static_cast<unsigned char>(hay[end]));
            if (startOk && endOk)
                ++count;
            pos = end;
        }
        return count;
    }

    // Total tokens currently held in a bot's raw conversation history.
    uint32_t HistoryTokens(uint64_t botGuid)
    {
        uint32_t total = 0;

        std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);
        auto botIt = g_BotConversationHistory.find(botGuid);
        if (botIt == g_BotConversationHistory.end())
            return 0;

        for (const auto& [playerGuid, history] : botIt->second)
            for (const auto& pair : history)
                total += Memory_EstimateTokens(pair.playerMessage) + Memory_EstimateTokens(pair.botReply);

        return total;
    }

    std::string RenderHistory(uint64_t botGuid)
    {
        std::string out;

        std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);
        auto botIt = g_BotConversationHistory.find(botGuid);
        if (botIt == g_BotConversationHistory.end())
            return out;

        for (const auto& [playerGuid, history] : botIt->second)
        {
            Player* other = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
            const std::string name = other ? other->GetName() : "someone";

            for (const auto& pair : history)
            {
                if (!pair.playerMessage.empty())
                    out += name + ": " + pair.playerMessage + "\n";
                if (!pair.botReply.empty())
                    out += "You: " + pair.botReply + "\n";
            }
        }
        return out;
    }

    void ClearHistory(uint64_t botGuid)
    {
        {
            std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);
            g_BotConversationHistory.erase(botGuid);
        }

        // The rows outlived the in-memory window until now, so every restart
        // reloaded a conversation that had already been condensed and paid to
        // condense it all over again.
        DeleteBotConversationHistoryFromDB(botGuid);
    }

    // Parse the model's condensation output. One memory per line, optionally
    // prefixed with an importance score: "7 | He saved my life at the bridge."
    std::vector<BotMemoryEntry> ParseMemories(const std::string& text)
    {
        std::vector<BotMemoryEntry> out;

        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();

            std::string line = text.substr(start, end - start);
            start = end + 1;

            // Trim and strip list markers the model adds unasked.
            size_t a = line.find_first_not_of(" \t\r-*•");
            if (a == std::string::npos)
                continue;
            size_t b = line.find_last_not_of(" \t\r");
            line = line.substr(a, b - a + 1);
            if (line.empty())
                continue;

            BotMemoryEntry entry;
            entry.importance = 5;

            // Leading "N |" or "N."
            size_t sep = line.find('|');
            if (sep != std::string::npos && sep <= 3)
            {
                try
                {
                    const int score = std::stoi(line.substr(0, sep));
                    entry.importance = static_cast<uint8_t>(std::clamp(score, 1, 10));
                    line = line.substr(sep + 1);
                }
                catch (const std::exception&) { }
            }

            a = line.find_first_not_of(" \t");
            if (a == std::string::npos)
                continue;
            line = line.substr(a);

            if (line.size() < 8)     // not a real memory
                continue;

            entry.text      = line;
            entry.createdAt = NowSeconds();
            out.push_back(std::move(entry));

            if (out.size() >= 12)    // sanity cap on one condensation pass
                break;
        }

        return out;
    }
}

// --------------------------------------------------------------------------

uint32_t Memory_EstimateTokens(const std::string& text)
{
    return static_cast<uint32_t>((text.size() + 3) / 4);
}

void Memory_Load()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.clear();

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT bot_guid, memory_text, importance, UNIX_TIMESTAMP(created_at) "
            "FROM mod_ollama_chat_memories ORDER BY importance DESC"))
    {
        uint32_t loaded = 0;
        do
        {
            Field* f = result->Fetch();
            BotMemoryEntry e;
            e.text       = f[1].Get<std::string>();
            e.importance = f[2].Get<uint8>();
            e.createdAt  = f[3].Get<uint64>();

            if (!e.text.empty())
            {
                g_state[f[0].Get<uint64>()].memories.push_back(std::move(e));
                ++loaded;
            }
        } while (result->NextRow());

        LOG_INFO("module.ollamachat", "[Ollama Chat] Loaded {} bot memories.", loaded);
    }

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT bot_guid, other_guid, other_name, description, mentions, "
            "UNIX_TIMESTAMP(updated_at) FROM mod_ollama_chat_relationships"))
    {
        uint32_t loaded = 0;
        do
        {
            Field* f = result->Fetch();
            BotRelationship r;
            r.otherGuid   = f[1].Get<uint64>();
            r.otherName   = f[2].Get<std::string>();
            r.description = f[3].Get<std::string>();
            r.mentions    = f[4].Get<uint32>();
            r.updatedAt   = f[5].Get<uint64>();

            g_state[f[0].Get<uint64>()].relationships[r.otherGuid] = std::move(r);
            ++loaded;
        } while (result->NextRow());

        LOG_INFO("module.ollamachat", "[Ollama Chat] Loaded {} bot relationships.", loaded);
    }
}

void Memory_SaveAll()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto& [botGuid, state] : g_state)
    {
        if (!state.dirty)
            continue;

        // The delete and the reinserts have to land together. As separate
        // async statements, a crash between them left the bot with no memories
        // at all, which is worse than a stale set.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        trans->Append(SafeFormat(
            "DELETE FROM mod_ollama_chat_memories WHERE bot_guid = {}", botGuid));

        std::string values;
        for (const BotMemoryEntry& m : state.memories)
        {
            if (!values.empty())
                values += ',';

            values += SafeFormat("({}, '{}', {}, FROM_UNIXTIME({}))",
                                 botGuid, Escape(m.text), uint32_t(m.importance), m.createdAt);
        }

        if (!values.empty())
        {
            trans->Append("INSERT INTO mod_ollama_chat_memories "
                          "(bot_guid, memory_text, importance, created_at) VALUES " + values);
        }

        for (const auto& [otherGuid, r] : state.relationships)
        {
            if (r.description.empty())
                continue;

            // ON DUPLICATE KEY UPDATE rather than REPLACE INTO: REPLACE is a
            // delete plus an insert, so it rewrites the row and every index
            // entry even when nothing about the relationship changed.
            trans->Append(SafeFormat(
                "INSERT INTO mod_ollama_chat_relationships "
                "(bot_guid, other_guid, other_name, description, mentions, updated_at) "
                "VALUES ({}, {}, '{}', '{}', {}, FROM_UNIXTIME({})) "
                "ON DUPLICATE KEY UPDATE other_name = VALUES(other_name), "
                "description = VALUES(description), mentions = VALUES(mentions), "
                "updated_at = VALUES(updated_at)",
                botGuid, otherGuid, Escape(r.otherName), Escape(r.description),
                r.mentions, r.updatedAt ? r.updatedAt : NowSeconds()));
        }

        CharacterDatabase.CommitTransaction(trans);

        state.dirty = false;
    }
}

void Memory_ForgetBot(ObjectGuid botGuid)
{
    // Memories persist in the database; this only drops the in-memory copy for
    // a character who has logged out.
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.erase(botGuid.GetRawValue());
}

// --------------------------------------------------------------------------

void Memory_NoteExchange(uint64_t botGuid, uint64_t otherGuid,
                         const std::string& otherName,
                         const std::string& incomingMessage,
                         const std::string& botReply)
{
    if (!g_MemoryEnable || botGuid == 0)
        return;

    Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
    if (!bot)
        return;

    // ---- relationship mention counting ---------------------------------
    if (g_RelationshipEnable && otherGuid != 0 && !otherName.empty())
    {
        const std::string combined = incomingMessage + " " + botReply;
        const uint32_t hits = 1 + CountMentions(combined, otherName);

        bool trigger = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            OllamaMemoryState& state = g_state[botGuid];
            BotRelationship& rel = state.relationships[otherGuid];

            rel.otherGuid = otherGuid;
            if (rel.otherName.empty())
                rel.otherName = otherName;
            rel.mentions += hits;
            state.dirty = true;

            if (rel.mentions >= g_RelationshipMentionThreshold &&
                !state.relationshipPending[otherGuid])
            {
                state.relationshipPending[otherGuid] = true;
                rel.mentions = 0;      // reset the counter for the next revision
                trigger = true;
            }
        }

        if (trigger)
        {
            std::string prompt = Memory_BuildRelationshipPrompt(bot, otherGuid, otherName);
            if (!prompt.empty())
                OllamaDispatch_SubmitRelationship(botGuid, otherGuid, otherName, prompt);
            else
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_state[botGuid].relationshipPending[otherGuid] = false;
            }
        }
    }

    // ---- condensation ---------------------------------------------------
    if (g_MemoryHistoryTokenLimit == 0)
        return;

    if (HistoryTokens(botGuid) < g_MemoryHistoryTokenLimit)
        return;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        OllamaMemoryState& state = g_state[botGuid];
        if (state.condensing)
            return;
        state.condensing = true;
    }

    std::string prompt = Memory_BuildCondensationPrompt(bot);
    if (prompt.empty())
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state[botGuid].condensing = false;
        return;
    }

    OllamaDispatch_SubmitCondensation(botGuid, prompt);
}

// --------------------------------------------------------------------------

std::string Memory_BuildCondensationPrompt(Player* bot)
{
    if (!bot || g_MemoryCondensePrompt.empty())
        return "";

    const std::string history = RenderHistory(bot->GetGUID().GetRawValue());
    if (history.empty())
        return "";

    return SafeFormat(g_MemoryCondensePrompt,
                      fmt::arg("bot_name", bot->GetName()),
                      fmt::arg("history", history));
}

std::string Memory_BuildRelationshipPrompt(Player* bot, uint64_t otherGuid,
                                           const std::string& otherName)
{
    if (!bot || g_RelationshipUpdatePrompt.empty())
        return "";

    std::string existing;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_state.find(bot->GetGUID().GetRawValue());
        if (it != g_state.end())
        {
            auto relIt = it->second.relationships.find(otherGuid);
            if (relIt != it->second.relationships.end())
                existing = relIt->second.description;
        }
    }

    const std::string history = RenderHistory(bot->GetGUID().GetRawValue());

    return SafeFormat(g_RelationshipUpdatePrompt,
                      fmt::arg("bot_name", bot->GetName()),
                      fmt::arg("other_name", otherName),
                      fmt::arg("existing", existing.empty() ? "nothing yet" : existing),
                      fmt::arg("history", history));
}

// --------------------------------------------------------------------------

std::string Memory_BuildPromptSection(Player* bot, Player* about)
{
    if (!g_MemoryEnable || !bot)
        return "";

    const uint64_t botGuid   = bot->GetGUID().GetRawValue();
    const uint64_t aboutGuid = about ? about->GetGUID().GetRawValue() : 0;

    std::vector<BotMemoryEntry> memories;
    std::vector<BotRelationship> relationships;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_state.find(botGuid);
        if (it == g_state.end())
            return "";

        memories = it->second.memories;
        for (const auto& [guid, rel] : it->second.relationships)
            if (!rel.description.empty())
                relationships.push_back(rel);
    }

    std::string out;

    // ---- relationships: whoever we are talking to comes first ----------
    if (g_RelationshipEnable && !relationships.empty())
    {
        std::sort(relationships.begin(), relationships.end(),
                  [aboutGuid](const BotRelationship& a, const BotRelationship& b)
                  {
                      if ((a.otherGuid == aboutGuid) != (b.otherGuid == aboutGuid))
                          return a.otherGuid == aboutGuid;
                      return a.updatedAt > b.updatedAt;
                  });

        std::string lines;
        uint32_t taken = 0;
        for (const BotRelationship& r : relationships)
        {
            if (g_RelationshipMaxPerPrompt > 0 && taken >= g_RelationshipMaxPerPrompt)
                break;
            lines += " - " + r.otherName + ": " + r.description + "\n";
            ++taken;
        }

        if (!lines.empty() && !g_RelationshipPromptTemplate.empty())
            out += SafeFormat(g_RelationshipPromptTemplate, fmt::arg("relationships", lines));
    }

    // ---- memories: most important first, inside a token budget ---------
    if (!memories.empty() && !g_MemoryPromptTemplate.empty())
    {
        std::sort(memories.begin(), memories.end(),
                  [](const BotMemoryEntry& a, const BotMemoryEntry& b)
                  {
                      if (a.importance != b.importance)
                          return a.importance > b.importance;
                      return a.createdAt > b.createdAt;
                  });

        std::string lines;
        uint32_t used = 0;
        for (const BotMemoryEntry& m : memories)
        {
            const uint32_t cost = Memory_EstimateTokens(m.text) + 4;
            if (g_MemoryPromptTokenBudget > 0 && used + cost > g_MemoryPromptTokenBudget)
                continue;      // try the next, shorter one rather than stopping
            lines += " - " + m.text + "\n";
            used += cost;
        }

        if (!lines.empty())
            out += SafeFormat(g_MemoryPromptTemplate, fmt::arg("memories", lines));
    }

    return out;
}

// --------------------------------------------------------------------------
// Worker-side
// --------------------------------------------------------------------------

void Memory_RunCondensation(uint64_t botGuid, const std::string& prompt)
{
    OllamaApiResult api = QueryOllama(prompt, OllamaRequestKind::Sentiment);

    std::vector<BotMemoryEntry> fresh;
    if (api.ok)
        fresh = ParseMemories(api.text);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        OllamaMemoryState& state = g_state[botGuid];
        state.condensing = false;

        if (!fresh.empty())
        {
            state.memories.insert(state.memories.end(), fresh.begin(), fresh.end());

            // Keep the most important, drop the rest.
            std::sort(state.memories.begin(), state.memories.end(),
                      [](const BotMemoryEntry& a, const BotMemoryEntry& b)
                      {
                          if (a.importance != b.importance)
                              return a.importance > b.importance;
                          return a.createdAt > b.createdAt;
                      });

            if (g_MemoryMaxPerBot > 0 && state.memories.size() > g_MemoryMaxPerBot)
                state.memories.resize(g_MemoryMaxPerBot);

            state.dirty = true;
        }
    }

    if (!api.ok)
    {
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat",
                     "[Ollama Chat] Memory condensation failed for bot {}: {}",
                     botGuid, api.error);
        return;      // keep the history; we will try again next time
    }

    // The history has been distilled; drop it so the window starts fresh.
    ClearHistory(botGuid);

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] Condensed history for bot {} into {} memories.",
                 botGuid, fresh.size());
}

void Memory_RunRelationshipUpdate(uint64_t botGuid, uint64_t otherGuid,
                                  const std::string& otherName,
                                  const std::string& prompt)
{
    OllamaApiResult api = QueryOllama(prompt, OllamaRequestKind::Sentiment);

    std::string description;
    if (api.ok)
    {
        description = api.text;

        // One sentence, no markup, bounded length.
        for (char& c : description)
            if (c == '\n' || c == '\r' || c == '\t')
                c = ' ';

        const size_t cut = description.find_first_of(" \t");
        (void)cut;

        if (description.size() > g_RelationshipMaxLength)
        {
            description.resize(g_RelationshipMaxLength);
            const size_t lastSpace = description.find_last_of(' ');
            if (lastSpace != std::string::npos && lastSpace > g_RelationshipMaxLength / 2)
                description.erase(lastSpace);
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    OllamaMemoryState& state = g_state[botGuid];
    state.relationshipPending[otherGuid] = false;

    if (description.empty())
        return;

    BotRelationship& rel = state.relationships[otherGuid];
    rel.otherGuid   = otherGuid;
    rel.otherName   = otherName;
    rel.description = std::move(description);
    rel.updatedAt   = NowSeconds();
    state.dirty     = true;

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] Bot {} relationship with {} updated: {}",
                 botGuid, otherName, rel.description);
}
