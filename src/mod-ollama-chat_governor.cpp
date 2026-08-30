#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <iterator>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    std::mutex g_mutex;

    using GramCounts = std::unordered_map<std::string, int>;

    struct Utterance
    {
        std::string           normalized;
        std::string           opener;
        std::set<std::string> tokens;    // content words, stopwords dropped
        GramCounts            grams;     // character trigrams
        double                gramNorm = 0.0;   // sqrt(sum of squares)
        TimePoint             when;
    };

    struct BotState
    {
        TimePoint             lastSend{};
        TimePoint             lastEvent{};
        std::deque<Utterance> history;
        std::unordered_map<uint64_t, TimePoint> emoteReactions;  // player guid -> last
    };

    struct ScopeState
    {
        TimePoint             lastHuman{};
        TimePoint             lastSend{};
        std::deque<Utterance> history;
        std::deque<TimePoint> sendTimes;
    };

    std::unordered_map<uint64_t, BotState>    g_bots;
    std::unordered_map<std::string, ScopeState> g_scopes;
    std::deque<TimePoint>                     g_globalSends;

    GovernorStats g_stats{};

    inline double SecondsSince(TimePoint t, TimePoint now)
    {
        if (t.time_since_epoch().count() == 0)
            return 1e9;   // never happened
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count() / 1000.0;
    }

    void TrimWindow(std::deque<TimePoint>& times, TimePoint now, double windowSec)
    {
        while (!times.empty() && SecondsSince(times.front(), now) > windowSec)
            times.pop_front();
    }

    // ---- text normalisation ---------------------------------------------

    const std::unordered_set<std::string>& Stopwords()
    {
        static const std::unordered_set<std::string> s = {
            "a","an","the","and","or","but","if","of","to","in","on","at","for",
            "is","are","was","were","be","been","am","i","you","he","she","it",
            "we","they","me","my","your","this","that","these","those","so",
            "just","really","very","gonna","got","get","do","does","did","have",
            "has","had","will","would","can","could","should","not","no","yes"
        };
        return s;
    }

    std::string NormalizeText(const std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        for (char ch : text)
        {
            unsigned char c = static_cast<unsigned char>(ch);
            if (std::isalnum(c))
                out.push_back(static_cast<char>(std::tolower(c)));
            else if (!out.empty() && out.back() != ' ')
                out.push_back(' ');
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    std::vector<std::string> Tokenize(const std::string& normalized, bool dropStopwords)
    {
        std::vector<std::string> tokens;
        size_t start = 0;
        while (start < normalized.size())
        {
            size_t end = normalized.find(' ', start);
            if (end == std::string::npos)
                end = normalized.size();
            if (end > start)
            {
                std::string tok = normalized.substr(start, end - start);
                if (!dropStopwords || !Stopwords().count(tok))
                    tokens.push_back(std::move(tok));
            }
            start = end + 1;
        }
        return tokens;
    }

    std::string OpenerOf(const std::string& normalized)
    {
        auto tokens = Tokenize(normalized, false);
        std::string opener;
        for (size_t i = 0; i < tokens.size() && i < 3; ++i)
        {
            if (!opener.empty())
                opener.push_back(' ');
            opener += tokens[i];
        }
        return opener;
    }

    std::set<std::string> BuildTokenSet(const std::string& normalized)
    {
        std::set<std::string> out;
        for (auto& t : Tokenize(normalized, true))
            out.insert(std::move(t));
        return out;
    }

    GramCounts BuildGrams(const std::string& s)
    {
        GramCounts m;
        if (s.size() < 3)
        {
            if (!s.empty())
                m[s] = 1;
            return m;
        }
        for (size_t i = 0; i + 3 <= s.size(); ++i)
            ++m[s.substr(i, 3)];
        return m;
    }

    double GramNorm(const GramCounts& g)
    {
        double n = 0.0;
        for (const auto& [gram, c] : g)
            n += double(c) * c;
        return std::sqrt(n);
    }

    float JaccardOf(const std::set<std::string>& sa, const std::set<std::string>& sb)
    {
        if (sa.empty() || sb.empty())
            return 0.0f;

        size_t inter = 0;
        for (const auto& t : sa)
            if (sb.count(t))
                ++inter;

        const size_t uni = sa.size() + sb.size() - inter;
        return uni == 0 ? 0.0f : static_cast<float>(inter) / static_cast<float>(uni);
    }

    float CosineOf(const GramCounts& ga, double na, const GramCounts& gb, double nb)
    {
        if (na <= 0.0 || nb <= 0.0)
            return 0.0f;

        // Iterate the smaller map.
        const GramCounts& small = ga.size() <= gb.size() ? ga : gb;
        const GramCounts& large = ga.size() <= gb.size() ? gb : ga;

        double dot = 0.0;
        for (const auto& [gram, c] : small)
        {
            auto it = large.find(gram);
            if (it != large.end())
                dot += double(c) * it->second;
        }

        return static_cast<float>(dot / (na * nb));
    }

    void TrimHistory(std::deque<Utterance>& hist, size_t maxSize)
    {
        while (hist.size() > maxSize)
            hist.pop_front();
    }
}

// --------------------------------------------------------------------------

std::string Governor_MakeScopeKey(const char* sourceName, uint32_t channelId,
                                  const std::string& channelName,
                                  uint32_t guildId, uint32_t groupOrZoneId)
{
    std::string key = sourceName ? sourceName : "unknown";

    if (!channelName.empty())
    {
        key += "#";
        key += channelName;
        key += ":";
        key += std::to_string(channelId);
    }
    else if (guildId)
    {
        key += "#g";
        key += std::to_string(guildId);
    }
    else
    {
        key += "#z";
        key += std::to_string(groupOrZoneId);
    }

    return key;
}

// --- the audience rule ----------------------------------------------------

void Governor_NoteHumanMessage(const std::string& scopeKey)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_scopes[scopeKey].lastHuman = Clock::now();
}

bool Governor_HasRecentHuman(const std::string& scopeKey)
{
    if (!g_RequireRecentHuman)
        return true;

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_scopes.find(scopeKey);
    if (it == g_scopes.end())
    {
        ++g_stats.blockedNoAudience;
        return false;
    }

    if (SecondsSince(it->second.lastHuman, Clock::now()) <= double(g_HumanWindowSeconds))
        return true;

    ++g_stats.blockedNoAudience;
    return false;
}

// --- chain depth ----------------------------------------------------------

bool Governor_ChainDepthAllowed(uint8_t depth)
{
    if (depth < g_MaxChainDepth)
        return true;

    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_stats.blockedChainDepth;
    return false;
}

uint32_t Governor_ApplyChainDecay(uint32_t baseChancePct, uint8_t depth)
{
    if (depth == 0 || g_ChainChanceDecayPct >= 100)
        return baseChancePct;

    double chance = baseChancePct;
    for (uint8_t i = 0; i < depth; ++i)
        chance = chance * double(g_ChainChanceDecayPct) / 100.0;

    return static_cast<uint32_t>(chance + 0.5);
}

// --- cooldowns and rate limits -------------------------------------------

namespace
{
    bool CheckSendLocked(ObjectGuid botGuid, const std::string& scopeKey,
                         TimePoint now, bool reserve)
    {
        const uint64_t raw = botGuid.GetRawValue();
        BotState&   bot   = g_bots[raw];
        ScopeState& scope = g_scopes[scopeKey];

        if (g_BotCooldownSeconds > 0 &&
            SecondsSince(bot.lastSend, now) < double(g_BotCooldownSeconds))
        {
            ++g_stats.blockedCooldown;
            return false;
        }

        if (g_ScopeCooldownSeconds > 0 &&
            SecondsSince(scope.lastSend, now) < double(g_ScopeCooldownSeconds))
        {
            ++g_stats.blockedCooldown;
            return false;
        }

        TrimWindow(scope.sendTimes, now, 60.0);
        if (g_ScopeMessagesPerMinute > 0 &&
            scope.sendTimes.size() >= g_ScopeMessagesPerMinute)
        {
            ++g_stats.blockedRate;
            return false;
        }

        TrimWindow(g_globalSends, now, 60.0);
        if (g_GlobalMessagesPerMinute > 0 &&
            g_globalSends.size() >= g_GlobalMessagesPerMinute)
        {
            ++g_stats.blockedRate;
            return false;
        }

        if (reserve)
        {
            bot.lastSend   = now;
            scope.lastSend = now;
            scope.sendTimes.push_back(now);
            g_globalSends.push_back(now);
        }

        return true;
    }
}

bool Governor_TryConsumeSend(ObjectGuid botGuid, const std::string& scopeKey)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return CheckSendLocked(botGuid, scopeKey, Clock::now(), true);
}

bool Governor_CanSend(ObjectGuid botGuid, const std::string& scopeKey)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return CheckSendLocked(botGuid, scopeKey, Clock::now(), false);
}

// --- repetition -----------------------------------------------------------

float Governor_Similarity(const std::string& a, const std::string& b)
{
    const std::string na = NormalizeText(a);
    const std::string nb = NormalizeText(b);
    if (na.empty() || nb.empty())
        return 0.0f;
    if (na == nb)
        return 1.0f;

    const std::set<std::string> ta = BuildTokenSet(na);
    const std::set<std::string> tb = BuildTokenSet(nb);
    const GramCounts ga = BuildGrams(na);
    const GramCounts gb = BuildGrams(nb);

    return std::max(JaccardOf(ta, tb), CosineOf(ga, GramNorm(ga), gb, GramNorm(gb)));
}

bool Governor_IsRepetitive(ObjectGuid botGuid, const std::string& scopeKey,
                           const std::string& text)
{
    const std::string norm = NormalizeText(text);
    if (norm.empty())
        return false;

    const std::string opener = OpenerOf(norm);

    // Built once for the candidate; every stored line already carries its own.
    const std::set<std::string> candTokens = BuildTokenSet(norm);
    const GramCounts            candGrams  = BuildGrams(norm);
    const double                candNorm   = GramNorm(candGrams);

    std::lock_guard<std::mutex> lock(g_mutex);
    const TimePoint now = Clock::now();

    auto tooSimilar = [&](const std::deque<Utterance>& hist) -> bool
    {
        for (const auto& u : hist)
        {
            if (SecondsSince(u.when, now) > double(g_RepetitionWindowSeconds))
                continue;
            if (u.normalized == norm)
                return true;

            // Cheap reject first: lines of wildly different length cannot be
            // similar enough to matter.
            const size_t la = norm.size(), lb = u.normalized.size();
            const size_t lo = la < lb ? la : lb, hi = la < lb ? lb : la;
            if (hi > 0 && double(lo) / double(hi) < 0.4)
                continue;

            if (JaccardOf(candTokens, u.tokens) >= g_RepetitionSimilarityThreshold)
                return true;
            if (CosineOf(candGrams, candNorm, u.grams, u.gramNorm) >= g_RepetitionSimilarityThreshold)
                return true;
        }
        return false;
    };

    auto botIt = g_bots.find(botGuid.GetRawValue());
    if (botIt != g_bots.end() && tooSimilar(botIt->second.history))
    {
        ++g_stats.blockedRepetition;
        return true;
    }

    auto scopeIt = g_scopes.find(scopeKey);
    if (scopeIt != g_scopes.end())
    {
        if (tooSimilar(scopeIt->second.history))
        {
            ++g_stats.blockedRepetition;
            return true;
        }

        // Opener collision: every bot starting "aye lad" is a distinct problem
        // from every bot saying the same whole line, and whole-line similarity
        // does not catch it.
        if (g_OpenerHistorySize > 0 && !opener.empty())
        {
            uint32_t checked = 0;
            for (auto it = scopeIt->second.history.rbegin();
                 it != scopeIt->second.history.rend() && checked < g_OpenerHistorySize;
                 ++it, ++checked)
            {
                if (SecondsSince(it->when, now) > double(g_RepetitionWindowSeconds))
                    continue;
                if (!it->opener.empty() && it->opener == opener)
                {
                    ++g_stats.blockedRepetition;
                    return true;
                }
            }
        }
    }

    return false;
}

void Governor_RecordUtterance(ObjectGuid botGuid, const std::string& scopeKey,
                              const std::string& text)
{
    const std::string norm = NormalizeText(text);
    if (norm.empty())
        return;

    Utterance u;
    u.normalized = norm;
    u.opener     = OpenerOf(norm);
    u.tokens     = BuildTokenSet(norm);
    u.grams      = BuildGrams(norm);
    u.gramNorm   = GramNorm(u.grams);
    u.when       = Clock::now();

    std::lock_guard<std::mutex> lock(g_mutex);

    BotState& bot = g_bots[botGuid.GetRawValue()];
    bot.history.push_back(u);
    TrimHistory(bot.history, g_BotHistorySize);

    ScopeState& scope = g_scopes[scopeKey];
    scope.history.push_back(std::move(u));
    TrimHistory(scope.history, g_ScopeHistorySize);
}

// --- per-feature debounces -----------------------------------------------

bool Governor_TryConsumeEmoteReaction(ObjectGuid botGuid, ObjectGuid playerGuid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const TimePoint now = Clock::now();

    BotState& bot = g_bots[botGuid.GetRawValue()];
    TimePoint& last = bot.emoteReactions[playerGuid.GetRawValue()];

    if (SecondsSince(last, now) < double(g_EmoteReactionCooldownSeconds))
        return false;

    last = now;
    return true;
}

bool Governor_TryConsumeEventCooldown(ObjectGuid botGuid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const TimePoint now = Clock::now();

    BotState& bot = g_bots[botGuid.GetRawValue()];
    if (SecondsSince(bot.lastEvent, now) < double(g_EventCooldownTime))
        return false;

    bot.lastEvent = now;
    return true;
}

// --- maintenance ----------------------------------------------------------

void Governor_OnPlayerLogout(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bots.erase(guid.GetRawValue());

    // Also drop this player from every bot's emote debounce table.
    const uint64_t raw = guid.GetRawValue();
    for (auto& [botGuid, state] : g_bots)
        state.emoteReactions.erase(raw);
}

void Governor_Update()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const TimePoint now = Clock::now();

    TrimWindow(g_globalSends, now, 60.0);
    g_stats.sendsLastMinute = static_cast<uint32_t>(g_globalSends.size());

    const double staleAfter = std::max<double>(600.0, double(g_RepetitionWindowSeconds) * 2.0);

    for (auto it = g_scopes.begin(); it != g_scopes.end(); )
    {
        ScopeState& s = it->second;
        TrimWindow(s.sendTimes, now, 60.0);

        while (!s.history.empty() &&
               SecondsSince(s.history.front().when, now) > double(g_RepetitionWindowSeconds))
            s.history.pop_front();

        const bool idle = s.history.empty() && s.sendTimes.empty() &&
                          SecondsSince(s.lastHuman, now) > staleAfter &&
                          SecondsSince(s.lastSend, now)  > staleAfter;

        it = idle ? g_scopes.erase(it) : std::next(it);
    }

    for (auto& [guid, bot] : g_bots)
    {
        while (!bot.history.empty() &&
               SecondsSince(bot.history.front().when, now) > double(g_RepetitionWindowSeconds))
            bot.history.pop_front();

        for (auto it = bot.emoteReactions.begin(); it != bot.emoteReactions.end(); )
            it = (SecondsSince(it->second, now) > staleAfter)
                     ? bot.emoteReactions.erase(it)
                     : std::next(it);
    }

    g_stats.trackedBots   = static_cast<uint32_t>(g_bots.size());
    g_stats.trackedScopes = static_cast<uint32_t>(g_scopes.size());
}

void Governor_Reset()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bots.clear();
    g_scopes.clear();
    g_globalSends.clear();
    g_stats = GovernorStats{};
}

GovernorStats Governor_GetStats()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    GovernorStats out = g_stats;
    out.trackedBots   = static_cast<uint32_t>(g_bots.size());
    out.trackedScopes = static_cast<uint32_t>(g_scopes.size());
    out.sendsLastMinute = static_cast<uint32_t>(g_globalSends.size());
    return out;
}
