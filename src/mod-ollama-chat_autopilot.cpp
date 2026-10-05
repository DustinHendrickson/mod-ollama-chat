#include "mod-ollama-chat_autopilot.h"
#include "mod-ollama-chat_autopilot_goals.h"
#include "mod-ollama-chat_autopilot_planner.h"
#include "mod-ollama-chat_autopilot_strategies.h"
#include "mod-ollama-chat_autopilot_strategy.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_progress.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Group.h"
#include "Guild.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QuestDef.h"
#include "WorldSession.h"

#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "PlayerbotRepository.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>
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
    enum class Tier : uint8_t { Dormant, Background, Foreground };

    // How far a real player's presence reaches when deciding a bot's tier.
    enum class Scope : uint8_t { Range, Zone, Map, World, Always };

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

        // Control.
        bool     control                 = true;    // apply the model's strategies at all
        uint32_t withRealPlayer          = 1;       // 0 hands off, 1 combat strategies only
        bool     llmEnable               = true;
        uint32_t llmCallsPerHour         = 300;
        uint32_t maxConcurrentPlans      = 2;
        uint32_t decisionIntervalMinutes = 15;      // foreground
        uint32_t goalRefreshMinutes      = 90;      // background
        uint32_t defaultPlanMinutes      = 45;
        float    foregroundRange         = 100.0f;  // yards, for Scope::Range

        // Reach: who gets LLM time.
        Scope    foregroundScope         = Scope::Zone;
        Scope    backgroundScope         = Scope::Map;
        Tier     minimumTier             = Tier::Dormant;

        // Guilds with a human member.
        bool     selectRealPlayerGuilds  = false;
        Tier     realGuildTier           = Tier::Background;
        uint32_t realGuildRefreshMinutes = 10;
        uint32_t planTimeoutSeconds      = 300;
        std::string promptTemplate;

        // Guards.
        uint32_t guardDeaths             = 3;
        uint32_t guardDeathWindowMinutes = 15;
        uint32_t guardDurabilityPct      = 20;
        uint32_t guardFreeBagSlots       = 2;
        uint32_t guardHoldMinutes        = 10;

        uint32_t goalStaleMinutes        = 120;
    };

    Config g_cfg;

    enum : uint8_t
    {
        MODE_RULES = 0,   // selection rules decide
        MODE_ON    = 1,   // a GM forced it on
        MODE_OFF   = 2,   // a GM forced it off
    };

    const char* TierName(Tier t)
    {
        switch (t)
        {
            case Tier::Foreground: return "foreground";
            case Tier::Background: return "background";
            default:               return "dormant";
        }
    }

    // One per bot that has ever been enrolled or had a GM decision. Bots the
    // rules never matched get no row at all, so a realm of thousands of random
    // bots does not grow a table of thousands of "no" rows.
    //
    // Everything about who the bot is and what it does comes from the model:
    // the identity, the strategies it wants on and off, its rpg focus, goal
    // and playbook. Nothing here is chosen by code.
    struct Row
    {
        uint8_t     mode        = MODE_RULES;
        bool        enrolled    = false;
        std::string source;
        uint32_t    enrolledAt  = 0;

        // Identity, written by the model.
        std::string style;
        std::string outlook;
        std::string profile;

        // The model's current plan.
        std::string      doing;
        uint32_t         doingSince = 0;
        std::string      decidedBy;
        AutopilotDesired desired;                         // strategies it wants on/off
        std::vector<std::string> rpg;                     // rpg focus; empty = roam freely
        std::map<std::string, AutopilotDesired> playbook; // situation -> combat strategies
        AutopilotGoal    goal;
        std::string      lastReason;

        // What each strategy autopilot has touched looked like before it first
        // did. Persisted, so it survives logouts and is never re-learned from
        // playerbots' own store (which can capture our changes for alts).
        AutopilotBaseline baseline;

        // When the model asked to be consulted again, and when it last was.
        // Persisted, so a relog does not cost a fresh plan.
        uint32_t         planUntil  = 0;
        uint32_t         lastPlanAt = 0;

        // Facts.
        uint32_t    killsTotal    = 0;
        uint32_t    deathsTotal   = 0;
        uint32_t    questsTotal   = 0;
        uint32_t    dungeonsTotal = 0;

        bool        dirty       = false;

        bool HasIdentity() const { return !profile.empty() || !style.empty(); }
    };

    GoalCounters Counters(const Row& row)
    {
        GoalCounters c;
        c.questsTotal   = row.questsTotal;
        c.dungeonsTotal = row.dungeonsTotal;
        return c;
    }

    // Reward facts for the prompt. Plain counts; the model judges what they
    // mean to this character.
    constexpr const char* kRewardChannels[] = {
        "level", "quest", "kills", "pvp", "loot", "gold", "skill", "discovery",
    };

    struct Reward
    {
        uint32_t    at = 0;
        const char* channel = "";
        uint32_t    amount = 0;
    };

    constexpr size_t kSnapshotRing = 12;
    constexpr size_t kEventRing    = 8;
    constexpr size_t kDecisionRing = 3;

    // Per online bot. Reset at every login.
    struct Online
    {
        bool     evaluated      = false;  // rules applied since login/reload
        bool     markerOurs     = false;  // we added the strategy, not a master
        bool     loginSettled   = false;  // evaluated at least once since login
        uint32_t nextSnapshotAt = 0;
        uint32_t lastSnapshotAt = 0;
        uint32_t lastZone       = 0;

        // Planning.
        bool     planPending     = false;
        uint32_t planSubmittedAt = 0;
        bool     urgentPlan      = false;
        Tier     tier            = Tier::Dormant;

        // What autopilot is enforcing right now.
        AutopilotDesired  applied;
        bool              controlled = false;

        // Strategies a human changed by hand while grouped with the bot, and
        // the group that lock belongs to.
        std::set<std::string> locked;
        uint64_t              lockGroup = 0;

        // Safety overrides of the rpg focus.
        std::deque<uint32_t>     deathTimes;
        std::unordered_map<std::string, uint32_t> guardCooldown;   // kind -> may fire again at
        std::vector<std::string> guardRpg;
        uint32_t                 guardUntil  = 0;
        std::string              lastGuard;
        uint32_t                 lastGuardAt = 0;

        // Recent history, for prompts without a DB round trip.
        bool                          historyRequested = false;
        std::deque<ProgressSnapshot>  snapshots;   // oldest first
        std::deque<ProgressEvent>     events;      // oldest first
        std::deque<std::string>       decisions;   // oldest first

        // Facts the model reads.
        std::deque<Reward>     rewards;            // last hour
        uint32_t               lastMoney    = 0;
        uint32_t               lastSkillSum = 0;
        uint32_t               killsPending = 0;
        std::unordered_set<uint32_t> zonesSeen;    // this session
        std::unordered_set<uint32_t> skillCapsNoted;
        std::deque<std::pair<uint32_t, std::string>> temptations;

        // Goal stall detection.
        uint32_t goalProgressValue = 0;
        uint32_t goalProgressAt    = 0;

        // Situation on the previous visit, for boundary decisions.
        bool     situationKnown = false;
        uint8_t  prevInstance   = 0;     // 0 none, 1 dungeon, 2 battleground
        bool     prevGrouped    = false;
        bool     prevInFlight   = false;
        uint32_t lastDungeonMap = 0;     // for telling a run-back after a wipe from a new run
        uint32_t lastDungeonAt  = 0;
    };

    // Guards everything below. The world thread holds it for the sweep and
    // commands; map threads hold it briefly in the progress hooks. Nothing
    // called while holding it calls back into this file. Lock order is this
    // mutex, then the progress queue's / memory's / planner's.
    std::mutex                           g_mutex;
    std::unordered_map<uint64_t, Row>    g_rows;
    std::unordered_map<uint64_t, Online> g_online;
    std::vector<uint64_t>                g_roster;     // round-robin order
    std::unordered_set<uint64_t>         g_realOnline; // real players, for tiers
    std::unordered_set<uint32_t>         g_realGuilds; // guilds with a human member, online or not
    size_t                               g_cursor        = 0;
    uint32_t                             g_enrolledCount = 0;
    uint32_t                             g_cappedCount   = 0;   // enrolled via a capped rule

    // World thread only.
    QueryCallbackProcessor g_callbacks;
    bool     g_tablesOk    = false;
    uint32_t g_sweepTimer  = 0;
    uint32_t g_flushTimer  = 0;
    uint32_t g_guildTimer  = 0;
    bool     g_wasActive   = false;
    uint32_t g_orphanTimer = 0;
    constexpr uint32_t kOrphanCheckMs = 60 * 60 * 1000;

    // Counters for `.ollama autopilot status`.
    uint64_t g_statPlans    = 0;
    uint64_t g_statInvalid  = 0;
    uint64_t g_statRejected = 0;   // strategy names off the allow-list
    uint64_t g_statGuard    = 0;
    uint64_t g_statSteer    = 0;
    uint64_t g_statLocks    = 0;

    constexpr uint32_t kLogoutSnapshotMinGap = 300;
    constexpr uint32_t kUrgentGapSeconds     = 300;
    constexpr uint32_t kHour                 = 3600;
    constexpr size_t   kRewardCap            = 256;
    constexpr size_t   kTemptationCap        = 4;

    // ----------------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------------

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // splitmix64: a stable, well-mixed per-bot hash for selection and
    // staggering, so the same bots are picked across restarts.
    uint64_t StableHash(uint64_t x)
    {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
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
    // a name list, a master's request, a human's guild) are never capped.
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

    // Return a bot to exactly what playerbots would give it without us: its
    // defaults, plus -- for alts -- the strategies its master saved, which
    // ResetStrategies alone does not reload (its Load call is commented out).
    // Mirrors PlayerbotHolder::OnBotLogin.
    //
    // Then every strategy autopilot ever touched is set back to its recorded
    // baseline, which undoes anything playerbots' store captured from us (it
    // saves every strategy when a grouped alt logs out, or on any nc/co
    // command) and anything ResetStrategies re-rolled differently.
    void HandBack(PlayerbotAI* ai, const AutopilotBaseline& baseline)
    {
        ai->ResetStrategies();
        if (Player* bot = ai->GetBot())
            if (!sRandomPlayerbotMgr.IsRandomBot(bot))
                PlayerbotRepository::instance().Load(ai);
        AutopilotStrategies_RestoreBaseline(ai, baseline);
    }

    // The marker sits on both engines. The non-combat one is the enrollment
    // marker a master can add; each also tells us when playerbots reset that
    // engine (`nc !` / `co !` reset one engine only).
    bool HasMarker(PlayerbotAI* ai, BotState state = BOT_STATE_NON_COMBAT)
    {
        return ai->HasStrategy(AUTOPILOT_STRATEGY_NAME, state);
    }

    void AddMarker(PlayerbotAI* ai)
    {
        for (BotState state : { BOT_STATE_NON_COMBAT, BOT_STATE_COMBAT })
            if (!HasMarker(ai, state))
                ai->ChangeStrategy(std::string("+") + AUTOPILOT_STRATEGY_NAME, state);
    }

    void RemoveMarker(PlayerbotAI* ai)
    {
        for (BotState state : { BOT_STATE_NON_COMBAT, BOT_STATE_COMBAT })
            if (HasMarker(ai, state))
                ai->ChangeStrategy(std::string("-") + AUTOPILOT_STRATEGY_NAME, state);
    }

    template <typename T>
    void PushCapped(std::deque<T>& ring, T value, size_t cap)
    {
        ring.push_back(std::move(value));
        while (ring.size() > cap)
            ring.pop_front();
    }

    // Diary entry plus the in-memory ring the planner reads. g_mutex held.
    void RecordEvent(uint64_t guid, const std::string& type, const std::string& detail)
    {
        Progress_QueueEvent(guid, type, detail);

        // The prompt already carries the model's own plans (as recent
        // decisions) and its identity; echoing them here would push real
        // facts out of the short event list.
        if (type == "plan" || type == "identity" || type == "identity_changed")
            return;

        auto it = g_online.find(guid);
        if (it == g_online.end())
            return;

        ProgressEvent e;
        e.botGuid = guid;
        e.at      = Progress_Now();
        e.type    = type;
        e.detail  = detail;
        PushCapped(it->second.events, std::move(e), kEventRing);
    }

    // "+quest -grind +flee" -- the model's own words for a change list.
    std::string ChangesText(const AutopilotDesired& d)
    {
        std::string out;
        for (const auto& [key, on] : d)
            out += SafeFormat("{}{}{}", out.empty() ? "" : " ", on ? "+" : "-",
                              key.size() > 3 ? key.substr(3) : key);
        return out;
    }

    std::string JoinList(const std::vector<std::string>& items)
    {
        std::string out;
        for (const std::string& s : items)
            out += (out.empty() ? "" : ", ") + s;
        return out;
    }

    // Playbook <-> DB: "dungeon=+co:flee|+co:potions;battleground=+co:aggressive"
    std::string SerializePlaybook(const std::map<std::string, AutopilotDesired>& playbook)
    {
        std::string out;
        for (const auto& [situation, changes] : playbook)
        {
            std::string list = AutopilotStrategies_Format(changes);
            std::replace(list.begin(), list.end(), ',', '|');
            out += (out.empty() ? "" : ";") + situation + "=" + list;
        }
        return out;
    }

    std::map<std::string, AutopilotDesired> ParsePlaybook(const std::string& text)
    {
        std::map<std::string, AutopilotDesired> out;
        for (const std::string& entry : SplitString(text, ';'))
        {
            const size_t eq = entry.find('=');
            if (eq == std::string::npos)
                continue;
            std::string list = entry.substr(eq + 1);
            std::replace(list.begin(), list.end(), '|', ',');
            out[entry.substr(0, eq)] = AutopilotStrategies_Parse(list);
        }
        return out;
    }

    // ----------------------------------------------------------------------
    // Situation: what is allowed to change right now. World thread.
    // ----------------------------------------------------------------------

    struct Situation
    {
        bool     dead           = false;
        bool     inCombat       = false;
        bool     travelling     = false;
        bool     inFlight       = false;   // actually on a taxi
        bool     withRealPlayer = false;   // in a group with a human
        bool     inInstance     = false;   // dungeon, raid, battleground, arena
        bool     inBattleground = false;   // battleground or arena
        bool     follower       = false;   // in a bot group, not its leader
        uint64_t groupGuid      = 0;

        // Key into the playbook, or "" in the open world.
        const char* PlaybookKey() const
        {
            if (inBattleground) return "battleground";
            if (inInstance)     return "dungeon";
            if (withRealPlayer) return "with_player";
            return "";
        }

        // Combat strategies and the playbook hold everywhere -- dungeons, a
        // human's group -- except where the operator asked for hands off.
        bool CanUseCombat() const
        {
            return !dead && !(withRealPlayer && g_cfg.withRealPlayer == 0);
        }

        // Out-of-combat strategies and the rpg focus only while the bot is
        // its own: something else owns its movement in a group or instance.
        bool CanUseNonCombat() const
        {
            return !dead && !withRealPlayer && !inInstance && !follower;
        }
    };

    Situation Classify(Player* bot, PlayerbotAI* ai)
    {
        Situation s;
        s.dead       = !bot->IsAlive();
        s.inCombat   = bot->IsInCombat();
        s.travelling = AutopilotRpg_IsTravelling(ai);
        s.inFlight   = bot->IsInFlight();

        if (Map* map = bot->GetMap())
        {
            s.inBattleground = map->IsBattlegroundOrArena();
            s.inInstance     = map->IsDungeon() || s.inBattleground;
        }

        if (Group* group = bot->GetGroup())
        {
            s.groupGuid      = group->GetGUID().GetRawValue();
            s.withRealPlayer = OllamaGroupHasRealPlayer(bot);
            s.follower       = !s.withRealPlayer && group->GetLeaderGUID() != bot->GetGUID();
        }
        return s;
    }

    // Does this real player put the bot within `scope`?
    bool InScope(Scope scope, Player* bot, Player* p)
    {
        switch (scope)
        {
            case Scope::Always:
            case Scope::World:  return true;
            case Scope::Map:    return p->GetMapId() == bot->GetMapId();
            case Scope::Zone:   return p->GetMapId() == bot->GetMapId() && p->GetZoneId() == bot->GetZoneId();
            case Scope::Range:  return p->GetMapId() == bot->GetMapId() &&
                                       bot->GetDistance(p) <= g_cfg.foregroundRange;
        }
        return false;
    }

    // Who is watching, and how much LLM time that earns. O(real players),
    // from our own login roster rather than a walk over every online
    // character. World thread, g_mutex held.
    Tier ComputeTier(Player* bot)
    {
        Tier tier = Tier::Dormant;
        auto raise = [&tier](Tier t) { if (t > tier) tier = t; };

        if (g_cfg.backgroundScope == Scope::Always)
            raise(Tier::Background);
        if (g_cfg.foregroundScope == Scope::Always)
            raise(Tier::Foreground);

        for (uint64_t guid : g_realOnline)
        {
            if (tier == Tier::Foreground)
                break;

            Player* p = ObjectAccessor::FindPlayer(ObjectGuid(guid));
            if (!p || !p->IsInWorld())
                continue;

            // A guildmate who is online can hear about it in guild chat.
            if (bot->GetGuildId() && p->GetGuildId() == bot->GetGuildId())
                raise(Tier::Foreground);
            else if (InScope(g_cfg.foregroundScope, bot, p))
                raise(Tier::Foreground);
            else if (InScope(g_cfg.backgroundScope, bot, p))
                raise(Tier::Background);
        }

        raise(g_cfg.minimumTier);
        if (bot->GetGuildId() && g_realGuilds.count(bot->GetGuildId()))
            raise(g_cfg.realGuildTier);

        return tier;
    }

    bool IsBotAccount(uint32_t accountId)
    {
        return sPlayerbotAIConfig.IsInRandomAccountList(accountId) ||
               sRandomPlayerbotMgr.IsAddClassAccount(accountId);
    }

    // A real-player guild changed: bots already online re-run the rules on
    // their next visit, so a newly founded guild picks up its bots without a
    // relog. g_mutex held.
    void OnRealGuildsChanged()
    {
        if (!g_cfg.selectRealPlayerGuilds)
            return;
        for (auto& [guid, ob] : g_online)
            ob.evaluated = false;
    }

    // Which guilds have a human member, online or not. A member is human
    // when its account is neither a random-bot nor an addclass account -- a
    // human's own alt bots count, since the account is theirs. Asynchronous:
    // one row per (guild, account) pair, a few thousand at most.
    void RefreshRealGuilds()
    {
        g_callbacks.AddCallback(CharacterDatabase.AsyncQuery(
            "SELECT DISTINCT gm.guildid, c.account FROM guild_member gm "
            "JOIN characters c ON c.guid = gm.guid")
            .WithCallback([](QueryResult result)
            {
                std::unordered_set<uint32_t> guilds;
                if (result)
                {
                    do
                    {
                        Field* f = result->Fetch();
                        if (!IsBotAccount(f[1].Get<uint32>()))
                            guilds.insert(f[0].Get<uint32>());
                    } while (result->NextRow());
                }

                std::lock_guard<std::mutex> lock(g_mutex);
                if (guilds != g_realGuilds)
                {
                    g_realGuilds.swap(guilds);
                    OnRealGuildsChanged();
                }
            }));
    }

    // ----------------------------------------------------------------------
    // Carrying out the model's plan. World thread, g_mutex held.
    // ----------------------------------------------------------------------

    // Is this stored key still something the model may control? The operator
    // may have narrowed the allow-list, or moved a name between the combat and
    // non-combat lists, since the model chose it.
    bool StillAllowed(const std::string& key)
    {
        const AutopilotStrategyInfo* info = AutopilotStrategies_Find(key.size() > 3 ? key.substr(3) : key);
        return info && info->Key() == key;
    }

    // The strategies to enforce right now: the model's choices that are still
    // allowed, filtered by what the situation allows, plus the playbook entry
    // for the situation.
    //
    // A focus only works with `new rpg` on and the legacy `rpg` off, so when
    // the model chose a focus those two follow it -- unless the model itself
    // turned `new rpg` off (its explicit choice wins; the focus then simply
    // does nothing) or the operator took them off the allow-list.
    AutopilotDesired Wanted(const Row& row, const Online& ob, const Situation& sit)
    {
        AutopilotDesired wanted;
        if (!sit.CanUseCombat())
            return wanted;

        for (const auto& [key, on] : row.desired)
            if (StillAllowed(key) && (key.rfind("co:", 0) == 0 || sit.CanUseNonCombat()))
                wanted[key] = on;

        const bool focus = !row.rpg.empty() || !ob.guardRpg.empty();
        if (focus && sit.CanUseNonCombat())
        {
            auto chosen = row.desired.find("nc:new rpg");
            const bool modelTurnedOff = chosen != row.desired.end() && !chosen->second;
            if (StillAllowed("nc:new rpg") && (!modelTurnedOff || !ob.guardRpg.empty()))
            {
                wanted["nc:new rpg"] = true;
                if (StillAllowed("nc:rpg"))
                    wanted["nc:rpg"] = false;
            }
        }

        if (const std::string key = sit.PlaybookKey(); !key.empty())
            if (auto it = row.playbook.find(key); it != row.playbook.end())
                for (const auto& [k, on] : it->second)
                    if (k.rfind("co:", 0) == 0 && StillAllowed(k))
                        wanted[k] = on;

        return wanted;
    }

    // Restore whatever a playerbots reset wiped, recognise a human's hand on
    // the controls, and bring the bot to what the model wants for this
    // situation. The marker on each engine is that engine's reset detector:
    // a reset clears it together with everything else, a human toggling
    // `co -flee` does not.
    void Reassert(PlayerbotAI* ai, uint64_t guid, Row& row, Online& ob, const Situation& sit)
    {
        const bool ncReset = !HasMarker(ai, BOT_STATE_NON_COMBAT);
        const bool coReset = !HasMarker(ai, BOT_STATE_COMBAT);
        if (ncReset || coReset)
        {
            AddMarker(ai);
            ob.markerOurs = true;
            // That engine is back to playerbots' defaults: nothing of ours is
            // left on it. The baseline stays -- it records the bot as it was
            // before autopilot, and a reset does not change that.
            for (auto it = ob.applied.begin(); it != ob.applied.end();)
            {
                const bool combat = it->first.rfind("co:", 0) == 0;
                it = (combat ? coReset : ncReset) ? ob.applied.erase(it) : std::next(it);
            }
        }

        if (!g_cfg.control)
        {
            // Diary only (`Autopilot.Control = 0`, possibly just reloaded):
            // nothing of ours may stay on the bot.
            if (ob.controlled || !ob.applied.empty())
            {
                HandBack(ai, row.baseline);
                AddMarker(ai);
                ob.applied.clear();
                ob.controlled = false;
            }
            return;
        }

        const AutopilotDesired wanted = Wanted(row, ob, sit);

        // Someone in the group switched something we had set, by hand.
        // Theirs now, until the group breaks up.
        if (sit.withRealPlayer && ob.controlled)
        {
            AutopilotDesired held;
            for (const auto& [key, on] : wanted)
                if (auto it = ob.applied.find(key); it != ob.applied.end() && it->second == on)
                    held[key] = on;

            for (const std::string& name : AutopilotStrategies_Drift(ai, held, ob.locked))
            {
                ob.locked.insert(name);
                ++g_statLocks;
                RecordEvent(guid, "locked", name + " (changed by a player in the group)");
            }
        }

        if (AutopilotStrategies_Apply(ai, wanted, ob.applied, ob.locked, row.baseline))
            row.dirty = true;
        if (!ob.applied.empty())
            ob.controlled = true;
    }

    std::vector<int> StatusIds(const std::vector<std::string>& names)
    {
        std::vector<int> out;
        for (const std::string& n : names)
            if (int s = AutopilotRpg_StatusFromName(n); s >= 0)
                out.push_back(s);
        return out;
    }

    // Keep the bot in the rpg focus the model chose (or a guard's override).
    void SteerRpg(PlayerbotAI* ai, const Row& row, const Online& ob, const Situation& sit)
    {
        if (!sit.CanUseNonCombat() || sit.inCombat || sit.travelling)
            return;

        const std::vector<std::string>& focus = ob.guardRpg.empty() ? row.rpg : ob.guardRpg;
        if (focus.empty() || !ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
            return;

        const std::vector<int> allowed = StatusIds(focus);
        const int status = AutopilotRpg_CurrentStatus(ai);
        if (status != AutopilotRpg_StatusFromName("idle") &&
            std::find(allowed.begin(), allowed.end(), status) != allowed.end())
            return;

        if (AutopilotRpg_Steer(ai, allowed))
            ++g_statSteer;
    }

    // ----------------------------------------------------------------------
    // Facts: rewards, temptations, goals, boundaries. g_mutex held.
    // ----------------------------------------------------------------------

    void NoteReward(Online& ob, const char* channel, uint32_t amount, uint32_t now)
    {
        if (amount == 0)
            return;
        while (!ob.rewards.empty() && now - ob.rewards.front().at > kHour)
            ob.rewards.pop_front();
        PushCapped(ob.rewards, Reward{ now, channel, amount }, kRewardCap);
    }

    std::string RewardSummary(const Online& ob, uint32_t now)
    {
        std::string out;
        for (const char* channel : kRewardChannels)
        {
            uint32_t total = 0;
            for (const Reward& r : ob.rewards)
                if (now - r.at <= kHour && std::string_view(channel) == r.channel)
                    total += r.amount;
            if (total)
                out += SafeFormat("{}{} {}", out.empty() ? "" : ", ", total, channel);
        }
        return out;
    }

    void AddTemptation(Online& ob, std::string text, uint32_t now)
    {
        while (!ob.temptations.empty() && now - ob.temptations.front().first > kHour)
            ob.temptations.pop_front();
        PushCapped(ob.temptations, std::make_pair(now, std::move(text)), kTemptationCap);
    }

    // Gold and profession progress since the last visit, as reward facts,
    // and a nudge when a profession hits its cap.
    void UpdateFacts(Player* bot, Online& ob, uint32_t now)
    {
        // Income arrives in coppers and silvers between visits a few seconds
        // apart, so the baseline only moves when a whole gold is booked (by
        // exactly that much) or when the bot spends.
        const uint32_t money = bot->GetMoney();
        if (ob.lastMoney == 0 || money < ob.lastMoney)
            ob.lastMoney = money;
        else if (money >= ob.lastMoney + 10000)
        {
            const uint32_t gold = (money - ob.lastMoney) / 10000;
            NoteReward(ob, "gold", gold, now);
            ob.lastMoney += gold * 10000;
        }

        uint32_t skillSum = 0;
        for (uint32_t skill : Progress_ProfessionSkills())
        {
            if (!bot->HasSkill(skill))
                continue;
            const uint32_t value = bot->GetSkillValue(skill);
            const uint32_t max   = bot->GetPureMaxSkillValue(skill);
            skillSum += value;
            if (value >= max && max < 450 && ob.skillCapsNoted.insert(skill * 1000 + max).second && ob.lastSkillSum)
                AddTemptation(ob, SafeFormat("{} has reached its cap of {}; a trainer could teach more",
                                             Progress_SkillName(skill), max), now);
        }
        if (ob.lastSkillSum && skillSum > ob.lastSkillSum)
            NoteReward(ob, "skill", skillSum - ob.lastSkillSum, now);
        ob.lastSkillSum = skillSum;
    }

    void SetGoal(uint64_t guid, Row& row, Online* ob, AutopilotGoal goal, const char* by)
    {
        row.goal  = std::move(goal);
        row.dirty = true;
        if (ob)
        {
            ob->goalProgressAt    = 0;
            ob->goalProgressValue = 0;
        }
        RecordEvent(guid, "goal", SafeFormat("{} [{} {}] ({})", row.goal.text, Goal_KindName(row.goal.kind),
                                             row.goal.target, by));
    }

    // Done or stalled goals are moments to ask the model again.
    void CheckGoal(Player* bot, uint64_t guid, Row& row, Online& ob, uint32_t now)
    {
        if (!row.goal.Active())
            return;

        const GoalProgress p = Goal_Progress(bot, row.goal, Counters(row));
        if (!p.measurable)
            return;

        if (p.done)
        {
            RecordEvent(guid, "goal_done", row.goal.text);
            AddTemptation(ob, "just achieved: " + row.goal.text, now);
            row.goal      = AutopilotGoal();
            row.dirty     = true;
            ob.urgentPlan = true;
            return;
        }

        if (ob.goalProgressAt == 0 || p.current != ob.goalProgressValue)
        {
            ob.goalProgressValue = p.current;
            ob.goalProgressAt    = now;
        }
        else if (g_cfg.goalStaleMinutes && now - ob.goalProgressAt > g_cfg.goalStaleMinutes * 60)
        {
            RecordEvent(guid, "goal_stalled", SafeFormat("{} ({} for {})", row.goal.text, p.text,
                                                         Span(now - ob.goalProgressAt)));
            ob.goalProgressAt = now;   // once per stale window, not every visit
            ob.urgentPlan     = true;
        }
    }

    // Situation changes are the natural moments to reconsider: leaving a
    // dungeon, a battleground ending, a group breaking up, a flight landing.
    void CheckBoundaries(Player* bot, uint64_t guid, Row& row, Online& ob, const Situation& sit, uint32_t now)
    {
        const uint8_t instance = sit.inBattleground ? 2 : sit.inInstance ? 1 : 0;
        const bool    grouped  = sit.withRealPlayer || sit.follower;

        if (ob.situationKnown)
        {
            if (instance == 1 && ob.prevInstance != 1)
            {
                // Walking back in after a wipe is the same run, not a new one.
                const uint32_t mapId = bot->GetMapId();
                if (mapId != ob.lastDungeonMap || now - ob.lastDungeonAt > 30 * 60)
                {
                    ++row.dungeonsTotal;
                    row.dirty = true;
                    RecordEvent(guid, "dungeon", bot->GetMap() ? bot->GetMap()->GetMapName() : "a dungeon");
                }
                ob.lastDungeonMap = mapId;
            }
            if (instance == 1)
                ob.lastDungeonAt = now;

            auto boundary = [&](const char* what)
            {
                RecordEvent(guid, "boundary", what);
                ob.urgentPlan = true;
            };

            if (instance == 0 && ob.prevInstance != 0)
                boundary(ob.prevInstance == 2 ? "the battleground is over" : "left the dungeon");
            if (!grouped && ob.prevGrouped)
                boundary("left the group");
            if (!sit.inFlight && ob.prevInFlight)
                boundary("landed after a flight");
        }

        ob.situationKnown = true;
        ob.prevInstance   = instance;
        ob.prevGrouped    = grouped;
        ob.prevInFlight   = sit.inFlight;
    }

    // Deterministic self-preservation, checked before anyone is asked. It
    // only overrides the rpg focus, for a while; the reason goes into the
    // next prompt. Returns true when a guard fired.
    bool CheckGuards(Player* bot, uint64_t guid, Online& ob, uint32_t now)
    {
        const uint32_t window = g_cfg.guardDeathWindowMinutes * 60;
        while (!ob.deathTimes.empty() && now - ob.deathTimes.front() > window)
            ob.deathTimes.pop_front();

        if (ob.guardUntil && now >= ob.guardUntil)
        {
            ob.guardRpg.clear();
            ob.guardUntil = 0;
        }
        if (!ob.guardRpg.empty())
            return false;

        // A guard that fired keeps quiet for a while. Some conditions cannot
        // be fixed by what it does (a broke bot cannot repair, bags can be
        // full of unsellable items); re-firing after every hold would only
        // churn and fill the diary.
        auto ready = [&](const char* kind)
        {
            auto it = ob.guardCooldown.find(kind);
            return it == ob.guardCooldown.end() || now >= it->second;
        };

        std::string reason;
        const char* kind = nullptr;
        std::vector<std::string> focus;

        if (g_cfg.guardDeaths > 0 && ob.deathTimes.size() >= g_cfg.guardDeaths && ready("deaths"))
        {
            reason = SafeFormat("died {} times in {} minutes in {}", ob.deathTimes.size(),
                                g_cfg.guardDeathWindowMinutes, Progress_ZoneName(bot->GetZoneId()));
            kind  = "deaths";
            focus = { "rest" };
            ob.deathTimes.clear();
        }
        else if (g_cfg.guardDurabilityPct > 0 && Progress_DurabilityPct(bot) < g_cfg.guardDurabilityPct &&
                 ready("durability"))
        {
            reason = SafeFormat("gear is badly damaged ({}% durability)", uint32_t(Progress_DurabilityPct(bot)));
            kind   = "durability";
            focus  = { "wander npc", "go camp" };
        }
        else if (g_cfg.guardFreeBagSlots > 0 && bot->GetFreeInventorySpace() < g_cfg.guardFreeBagSlots &&
                 ready("bags"))
        {
            reason = "bags are full";
            kind   = "bags";
            focus  = { "wander npc", "go camp" };
        }

        if (!kind)
            return false;

        ob.guardCooldown[kind] = now + g_cfg.guardHoldMinutes * 60 * 4;
        ob.guardRpg    = std::move(focus);
        ob.guardUntil  = now + g_cfg.guardHoldMinutes * 60;
        ob.lastGuard   = reason;
        ob.lastGuardAt = now;
        ++g_statGuard;
        RecordEvent(guid, "guard", reason);
        return true;
    }

    // ----------------------------------------------------------------------
    // Asking the model. World thread, g_mutex held.
    // ----------------------------------------------------------------------

    AutopilotPromptContext BuildPromptContext(Player* bot, PlayerbotAI* ai, const Row& row,
                                              const Online& ob, Tier tier, uint32_t now)
    {
        AutopilotPromptContext ctx;
        ctx.botName = bot->GetName();
        ctx.level   = bot->GetLevel();
        ctx.race    = ai->GetChatHelper()->FormatRace(bot->getRace());
        ctx.cls     = ai->GetChatHelper()->FormatClass(bot->getClass());
        if (Guild* guild = bot->GetGuild())
            ctx.guild = guild->GetName();
        ctx.mustBeInCharacter = g_RoleplayEnable && g_RoleplayStrictness >= 2;

        if (g_EnableRPPersonalities)
        {
            const std::string key = GetBotPersonality(bot);
            if (!key.empty() && key != "default")
                ctx.personality = key + " - " + GetPersonalityPromptAddition(key);
        }

        ctx.style   = row.style;
        ctx.outlook = row.outlook;
        ctx.profile = row.profile;

        ctx.doing        = row.doing;
        ctx.doingMinutes = row.doingSince && now > row.doingSince ? (now - row.doingSince) / 60 : 0;
        ctx.goal         = Goal_Describe(bot, row.goal, Counters(row));
        if (row.goal.Active() && ob.goalProgressAt && now - ob.goalProgressAt > 1800)
            ctx.goal += SafeFormat(" (no progress for {})", Span(now - ob.goalProgressAt));

        ctx.liveStrategies = AutopilotStrategies_DescribeLive(ai);
        ctx.rpgStatus      = AutopilotRpg_StatusName(AutopilotRpg_CurrentStatus(ai));
        ctx.rpgFocus       = JoinList(row.rpg);
        for (const auto& [situation, changes] : row.playbook)
            ctx.playbook += (ctx.playbook.empty() ? "" : "; ") + situation + ": " + ChangesText(changes);

        ctx.decisions.assign(ob.decisions.begin(), ob.decisions.end());
        for (const ProgressEvent& e : ob.events)
            ctx.events.push_back(SafeFormat("[{}] {}: {}", Ago(e.at, now), e.type, e.detail));
        for (const auto& [at, text] : ob.temptations)
            if (now - at <= kHour)
                ctx.temptations.push_back(text);
        ctx.rewards = RewardSummary(ob, now);

        std::vector<ProgressSnapshot> window(ob.snapshots.begin(), ob.snapshots.end());
        window.push_back(Progress_Capture(bot));
        window.back().killsTotal  = row.killsTotal;
        window.back().deathsTotal = row.deathsTotal;
        window.back().questsTotal = row.questsTotal;
        ctx.progress = Progress_Summarize(window);

        if (!ob.lastGuard.empty() && now - ob.lastGuardAt < kHour)
            ctx.guards = SafeFormat("Recently ({}): their instincts made them stop because {}.",
                                    Ago(ob.lastGuardAt, now), ob.lastGuard);

        // The full surroundings scan is for bots someone can see. A choice for
        // an unwatched bot needs only the macro state.
        const ProgressSnapshot& cur = window.back();
        ctx.state = SafeFormat(
            "Now: in {}, {} gold, gear at {}% durability, {} free bag slots, {} quests in the log.",
            Progress_ZoneName(cur.zoneId), cur.money / 10000, uint32_t(cur.durabilityPct),
            uint32_t(cur.freeBagSlots), uint32_t(cur.questsActive));
        if (!cur.professions.empty())
            ctx.state += " Professions: " + Progress_DescribeProfessions(cur.professions) + ".";
        if (tier == Tier::Foreground && g_EnableChatBotSnapshotTemplate)
            ctx.state += "\n" + GenerateBotGameStateSnapshot(bot);

        if (g_MemoryEnable)
            ctx.memories = Memory_BuildPromptSection(bot, nullptr);

        return ctx;
    }

    // Ask the model, within the budget. Returns true when a plan was submitted.
    bool TrySubmitPlan(Player* bot, PlayerbotAI* ai, uint64_t guid, Row& row, Online& ob,
                       uint32_t now, bool force)
    {
        if (!g_cfg.llmEnable || ob.planPending)
            return false;

        ob.tier = ComputeTier(bot);
        if (ob.tier == Tier::Dormant && !force)
            return false;

        // The tier's interval is a minimum gap between plans for one bot; the
        // model's own `minutes` says when it wants to be asked again. Events
        // (a level, a goal done) may ask sooner, but never more than once per
        // kUrgentGapSeconds, so a burst of events is one plan, not several.
        const uint32_t sinceLast = row.lastPlanAt ? now - row.lastPlanAt : UINT32_MAX;
        const uint32_t gap = (ob.tier == Tier::Foreground ? g_cfg.decisionIntervalMinutes
                                                          : g_cfg.goalRefreshMinutes) * 60;
        const bool allowed = force || sinceLast >= gap || (ob.urgentPlan && sinceLast >= kUrgentGapSeconds);
        if (!allowed)
            return false;

        // Cheap checks before the prompt is built: the budget, and room in
        // the shared request queue (chat comes first).
        if (!AutopilotPlanner_CanSubmit(force || ob.tier == Tier::Foreground) ||
            !OllamaDispatch_BackgroundHasRoom())
            return false;

        std::string prompt = AutopilotPlanner_BuildPrompt(BuildPromptContext(bot, ai, row, ob, ob.tier, now),
                                                          g_cfg.promptTemplate);
        if (!AutopilotPlanner_Submit(guid, std::move(prompt)))
            return false;

        ob.planPending     = true;
        ob.planSubmittedAt = now;
        ob.urgentPlan      = false;

        if (g_cfg.debug)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: asked the model about {} ({}{}).",
                     bot->GetName(), TierName(ob.tier), row.HasIdentity() ? "" : ", first plan");
        return true;
    }

    std::string NormaliseOutlook(const std::string& raw)
    {
        const std::string o = Lower(raw);
        if (o.find("meta") != std::string::npos)                                         return "metagamer";
        if (o.find("player") != std::string::npos || o.find("casual") != std::string::npos) return "player";
        if (o.find("character") != std::string::npos || o.find("immers") != std::string::npos) return "in-character";
        return o;
    }

    void ApplyDecision(const AutopilotDecision& d, uint32_t now)
    {
        auto rowIt = g_rows.find(d.botGuid);
        auto onIt  = g_online.find(d.botGuid);
        Online* ob = onIt == g_online.end() ? nullptr : &onIt->second;
        if (ob)
        {
            ob->planPending = false;
        }
        if (rowIt == g_rows.end() || !rowIt->second.enrolled)
            return;

        Row& row = rowIt->second;
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(d.botGuid));
        PlayerbotAI* ai = BotAI(bot);
        const std::string name = bot ? bot->GetName() : std::to_string(d.botGuid);

        // An attempt counts against the spacing even when the reply is
        // unusable, so a model that keeps failing is not hammered.
        row.lastPlanAt = now;
        row.dirty      = true;

        if (!d.ok)
        {
            // Keep doing what the model last chose; it is asked again at the
            // next opportunity.
            ++g_statInvalid;
            if (g_cfg.debug)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: no usable plan for {}: {}", name, d.error);
            return;
        }

        ++g_statPlans;
        row.dirty     = true;
        row.decidedBy = "llm";

        // Identity.
        const bool mustBeInCharacter = g_RoleplayEnable && g_RoleplayStrictness >= 2;
        if (!d.style.empty() || !d.outlook.empty() || !d.profile.empty())
        {
            const bool first = !row.HasIdentity();
            const std::string before = row.style + "|" + row.outlook + "|" + row.profile;
            if (!d.style.empty())
                row.style = d.style;
            if (!d.outlook.empty())
                row.outlook = NormaliseOutlook(d.outlook);
            if (!d.profile.empty())
                row.profile = d.profile;
            if (mustBeInCharacter)
                row.outlook = "in-character";
            // Models often echo the identity back unchanged; only a real change
            // is worth a diary line.
            if (before != row.style + "|" + row.outlook + "|" + row.profile)
                RecordEvent(d.botGuid, first ? "identity" : "identity_changed",
                            SafeFormat("{} ({}): {}", row.style, row.outlook, row.profile));
        }

        // Strategies: only names on the allow-list are taken.
        AutopilotDesired changes;
        std::vector<std::string> rejected;
        for (const auto& [strategy, on] : d.strategies)
        {
            if (const AutopilotStrategyInfo* info = AutopilotStrategies_Find(strategy))
            {
                row.desired[info->Key()] = on;
                changes[info->Key()]     = on;
            }
            else
            {
                rejected.push_back(strategy);
            }
        }
        if (!rejected.empty())
        {
            g_statRejected += rejected.size();
            if (g_cfg.debug)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: {} asked for strategies not on the "
                         "allow-list: {}", name, JoinList(rejected));
        }

        if (d.rpgGiven)
        {
            row.rpg.clear();
            for (const std::string& s : d.rpg)
                if (AutopilotStrategies_RpgAllowed(s) && std::find(row.rpg.begin(), row.rpg.end(), s) == row.rpg.end())
                    row.rpg.push_back(s);
        }

        if (d.playbookGiven)
        {
            row.playbook.clear();
            for (const auto& [situation, list] : d.playbook)
            {
                if (situation != "dungeon" && situation != "battleground" && situation != "with_player")
                    continue;
                AutopilotDesired entry;
                for (const auto& [strategy, on] : list)
                    if (const AutopilotStrategyInfo* info = AutopilotStrategies_Find(strategy); info && info->combat)
                        entry[info->Key()] = on;
                if (!entry.empty())
                    row.playbook[situation] = std::move(entry);
            }
        }

        // Goal: resolved against the live bot so progress can be measured.
        if (bot && !d.goalKind.empty())
        {
            AutopilotGoal goal;
            const std::string why = Goal_Resolve(bot, d.goalKind, d.goalTarget, d.goalText, Counters(row), goal);
            if (!why.empty())
            {
                if (g_cfg.debug)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: goal for {} not taken: {}", name, why);
            }
            else if (goal.kind == row.goal.kind && Lower(goal.target) == Lower(row.goal.target) &&
                     (goal.kind == GoalKind::EarnGold || goal.value == row.goal.value))
            {
                // The prompt shows the current goal, so models echo it back.
                // Same kind and target is the same aim: keep its baseline, or
                // "earn 50 gold" (money now + 50) would never complete.
                if (!goal.text.empty())
                    row.goal.text = goal.text;
            }
            else
            {
                SetGoal(d.botGuid, row, ob, std::move(goal), "llm");
            }
        }

        if (!d.doing.empty() && d.doing != row.doing)
        {
            row.doing      = d.doing;
            row.doingSince = now;
        }
        if (!d.reason.empty())
            row.lastReason = d.reason;

        const uint32_t minutes = d.minutes ? std::clamp<uint32_t>(d.minutes, 10, 180) : g_cfg.defaultPlanMinutes;
        const std::string summary = SafeFormat(
            "{}{}{} for {}m: {}", row.doing.empty() ? "?" : row.doing,
            changes.empty() ? "" : " [" + ChangesText(changes) + "]",
            row.rpg.empty() ? "" : " focus " + JoinList(row.rpg), minutes, d.reason.empty() ? "-" : d.reason);
        RecordEvent(d.botGuid, "plan", summary);
        row.planUntil = now + minutes * 60;

        if (g_cfg.debug)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: {} -> {}", name, summary);

        if (!ob)
            return;   // logged out while the model was thinking: applied at next login

        PushCapped(ob->decisions, summary, kDecisionRing);

        if (bot && ai)
        {
            const Situation sit = Classify(bot, ai);
            Reassert(ai, d.botGuid, row, *ob, sit);
            SteerRpg(ai, row, *ob, sit);
        }
    }

    // ----------------------------------------------------------------------
    // History for prompts, loaded once per session without blocking.
    // ----------------------------------------------------------------------

    void RequestHistory(uint64_t guid, Online& ob)
    {
        ob.historyRequested = true;

        g_callbacks.AddCallback(CharacterDatabase.AsyncQuery(SafeFormat(
            "SELECT {} FROM mod_ollama_chat_autopilot_snapshots WHERE bot_guid = {} "
            "ORDER BY taken_at DESC LIMIT {}", Progress_SnapshotColumns(), guid, kSnapshotRing))
            .WithCallback([guid](QueryResult result)
            {
                if (!result)
                    return;
                std::vector<ProgressSnapshot> loaded;   // newest first
                do { loaded.push_back(Progress_ReadSnapshot(result->Fetch(), guid)); } while (result->NextRow());

                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = g_online.find(guid);
                if (it == g_online.end())
                    return;
                auto& ring = it->second.snapshots;
                const uint32_t oldest = ring.empty() ? UINT32_MAX : ring.front().takenAt;
                for (const ProgressSnapshot& s : loaded)
                    if (s.takenAt < oldest)
                        ring.push_front(s);
                while (ring.size() > kSnapshotRing)
                    ring.pop_front();
            }));

        g_callbacks.AddCallback(CharacterDatabase.AsyncQuery(SafeFormat(
            "SELECT at, type, detail FROM mod_ollama_chat_autopilot_events WHERE bot_guid = {} "
            "ORDER BY id DESC LIMIT {}", guid, kEventRing))
            .WithCallback([guid](QueryResult result)
            {
                if (!result)
                    return;
                std::vector<ProgressEvent> loaded;   // newest first
                do
                {
                    Field* f = result->Fetch();
                    ProgressEvent e;
                    e.botGuid = guid;
                    e.at      = f[0].Get<uint32>();
                    e.type    = f[1].Get<std::string>();
                    e.detail  = f[2].Get<std::string>();
                    loaded.push_back(std::move(e));
                } while (result->NextRow());

                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = g_online.find(guid);
                if (it == g_online.end())
                    return;
                auto& ring = it->second.events;
                const uint32_t oldest = ring.empty() ? UINT32_MAX : ring.front().at;
                for (const ProgressEvent& e : loaded)
                    if (e.at < oldest)
                        ring.push_front(e);
                while (ring.size() > kEventRing)
                    ring.pop_front();
            }));
    }

    // ----------------------------------------------------------------------
    // Selection. World thread, g_mutex held.
    // ----------------------------------------------------------------------

    // The source that enrolls this bot, or "" when nothing does. Pure: it
    // changes nothing, so `preview` can call it too.
    std::string EvaluateRules(Player* bot, PlayerbotAI* ai, Row const* row, bool markerIsRequest)
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

        // Bots that share a guild with a human are always in, whatever their
        // level, and never count against MaxEnrolled: they are the ones a
        // player actually lives alongside.
        if (g_cfg.selectRealPlayerGuilds && bot->GetGuildId() && g_realGuilds.count(bot->GetGuildId()))
            return "realguild";

        // A master's request is sticky once granted, and a marker we did not
        // put there is a request. Neither is bound by the level band: both are
        // a human asking for this specific bot.
        if (g_cfg.allowMasterEnroll)
        {
            if (row && row->enrolled && row->source == "master")
                return "master";
            if (markerIsRequest && HasMarker(ai))
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
                StableHash(bot->GetGUID().GetRawValue()) % 100 < g_cfg.randomBotPercent)
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
        PushCapped(ob.snapshots, s, kSnapshotRing);
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
                if (firstEver)
                    row.enrolledAt = now;
                CountEnrolled(row, +1);

                // A bot without an identity is the model's to define first.
                if (!row.HasIdentity())
                    ob.urgentPlan = true;

                RecordEvent(guid, "enrolled", source);
                if (g_cfg.debug)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: enrolled {} via {}.", bot->GetName(), source);
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
                AddMarker(ai);
            ob.markerOurs = true;

            // A first enrollment gets a baseline now; otherwise snapshots are
            // staggered across the interval so a wave of logins does not
            // produce a wave of writes.
            const uint32_t interval = std::max<uint32_t>(1, g_cfg.snapshotIntervalMinutes) * 60;
            if (firstEver)
                TakeSnapshot(bot, row, ob, now);
            else if (ob.nextSnapshotAt == 0)
                ob.nextSnapshotAt = now + static_cast<uint32_t>(StableHash(guid) % interval);
            return;
        }

        if (wasEnrolled)
        {
            Row& row = it->second;
            CountEnrolled(row, -1);
            row.enrolled = false;
            row.dirty    = true;

            RecordEvent(guid, "unenrolled", ModeName(row.mode));
            if (g_cfg.debug)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: unenrolled {}.", bot->GetName());
        }

        // Hand the bot back to playerbots exactly as it would be without us.
        if (ob.controlled || !ob.applied.empty())
        {
            static const AutopilotBaseline kNone;
            HandBack(ai, it != g_rows.end() ? it->second.baseline : kNone);
            ob.controlled = false;
            ob.applied.clear();
        }
        ob.planPending = false;
        ob.guardRpg.clear();

        RemoveMarker(ai);
        ob.markerOurs = false;
    }

    void Reevaluate(Player* bot, PlayerbotAI* ai, uint64_t guid, Online& ob)
    {
        auto it = g_rows.find(guid);
        Row const* row = it == g_rows.end() ? nullptr : &it->second;
        // Only a marker that appears mid-session is a master asking. One present
        // at login was restored from playerbots_db_store (playerbots saves every
        // strategy, ours included, when a grouped bot logs out) and means nothing.
        Apply(bot, ai, guid, ob, EvaluateRules(bot, ai, row, ob.loginSettled && !ob.markerOurs));
        ob.loginSettled = true;
        ob.evaluated    = true;
    }

    // ----------------------------------------------------------------------
    // Sweep. World thread, g_mutex held.
    // ----------------------------------------------------------------------

    void Control(Player* bot, PlayerbotAI* ai, uint64_t guid, Row& row, Online& ob, uint32_t now)
    {
        const Situation sit = Classify(bot, ai);
        if (sit.dead)
            return;

        // Locks belong to one group with a human in it.
        if (!sit.withRealPlayer || sit.groupGuid != ob.lockGroup)
        {
            ob.locked.clear();
            ob.lockGroup = sit.withRealPlayer ? sit.groupGuid : 0;
        }

        if (!ob.historyRequested)
            RequestHistory(guid, ob);

        CheckBoundaries(bot, guid, row, ob, sit, now);
        UpdateFacts(bot, ob, now);
        CheckGoal(bot, guid, row, ob, now);
        if (g_cfg.control && sit.CanUseNonCombat())
            CheckGuards(bot, guid, ob, now);

        // Out-of-combat strategies follow the situation: joining a group puts
        // them back to the bot's own, leaving it puts the model's back.
        Reassert(ai, guid, row, ob, sit);

        // A plan that never came back (provider down, server restarted the
        // dispatcher) must not block the next one forever.
        if (ob.planPending && now - ob.planSubmittedAt > g_cfg.planTimeoutSeconds)
            ob.planPending = false;

        if (!g_cfg.control || !sit.CanUseNonCombat())
            return;

        // Ask the model when it wanted to be asked again, when something
        // happened that warrants it, or when the bot has no identity yet.
        // When it cannot be asked (no budget, nobody around), the bot keeps
        // doing what it last chose.
        const bool due = !row.HasIdentity() || ob.urgentPlan || now >= row.planUntil;
        if (due)
            TrySubmitPlan(bot, ai, guid, row, ob, now, false);

        SteerRpg(ai, row, ob, sit);
    }

    // ----------------------------------------------------------------------
    // Deleted characters. Their rows would otherwise sit in the table for
    // good -- and rule-enrolled ones would hold a MaxEnrolled place for a bot
    // that no longer exists, so random-bot churn slowly fills the cap.
    // ----------------------------------------------------------------------

    // Drop the in-memory state for these bots. Their DB rows are deleted by
    // the caller. g_mutex held.
    void ForgetRows(const std::vector<uint64_t>& guids)
    {
        for (uint64_t guid : guids)
        {
            auto it = g_rows.find(guid);
            if (it != g_rows.end())
            {
                if (it->second.enrolled)
                    CountEnrolled(it->second, -1);
                g_rows.erase(it);
            }
            Progress_Forget(guid);
        }
    }

    std::string DeleteRowsSql(const char* table, const std::string& guidList)
    {
        return SafeFormat("DELETE FROM {} WHERE bot_guid IN ({})", table, guidList);
    }

    // Rows whose character is gone, from any cause: a client delete while
    // autopilot was off, playerbots' bulk reset (raw SQL, no script hook), or
    // an operator's own tooling. Every bot with history has a row in the
    // main table, so that is the only one that needs scanning.
    constexpr const char* kOrphanQuery =
        "SELECT a.bot_guid FROM mod_ollama_chat_autopilot a "
        "LEFT JOIN characters c ON c.guid = a.bot_guid WHERE c.guid IS NULL";

    void DeleteOrphans(const std::vector<uint64_t>& guids)
    {
        if (guids.empty())
            return;

        std::string list;
        for (uint64_t guid : guids)
            list += (list.empty() ? "" : ",") + std::to_string(guid);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append(DeleteRowsSql("mod_ollama_chat_autopilot", list));
        trans->Append(DeleteRowsSql("mod_ollama_chat_autopilot_snapshots", list));
        trans->Append(DeleteRowsSql("mod_ollama_chat_autopilot_events", list));
        CharacterDatabase.CommitTransaction(trans);
    }

    void PruneOrphansAsync()
    {
        g_callbacks.AddCallback(CharacterDatabase.AsyncQuery(kOrphanQuery)
            .WithCallback([](QueryResult result)
            {
                if (!result)
                    return;

                std::vector<uint64_t> guids;
                do { guids.push_back((*result)[0].Get<uint64>()); } while (result->NextRow());

                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    ForgetRows(guids);
                }
                DeleteOrphans(guids);
                LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: removed {} rows for deleted characters.",
                         guids.size());
            }));
    }

    // Autopilot went inactive: hand every bot we touched back to playerbots
    // and take the marker off. Rows are kept, so turning it back on resumes
    // where each bot left off. One pass over online bots, on the transition
    // only. g_mutex held.
    void ReleaseAll()
    {
        for (auto& [guid, ob] : g_online)
        {
            if (PlayerbotAI* ai = BotAI(ObjectAccessor::FindPlayer(ObjectGuid(guid))))
            {
                if (ob.controlled || !ob.applied.empty())
                {
                    static const AutopilotBaseline kNone;
                    auto row = g_rows.find(guid);
                    HandBack(ai, row != g_rows.end() ? row->second.baseline : kNone);
                }
                RemoveMarker(ai);
            }
            ob.controlled = false;
            ob.applied.clear();
            ob.guardRpg.clear();
            ob.markerOurs  = false;
            ob.evaluated   = false;
            ob.planPending = false;
        }
        LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot is off; bots handed back to playerbots.");
    }

    // Returns true for a full visit (an evaluation, or an enrolled bot), which
    // is what the sweep budgets; false for a cheap check of an unenrolled bot.
    bool VisitBot(uint64_t guid, Online& ob, uint32_t now)
    {
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(guid));
        if (!bot || !bot->IsInWorld())
            return false;

        // The AI attaches inside mod-playerbots' own login hook, whose order
        // relative to ours is undefined; until then there is nothing to do.
        PlayerbotAI* ai = BotAI(bot);
        if (!ai)
            return false;

        bool full = false;
        if (!ob.evaluated)
        {
            Reevaluate(bot, ai, guid, ob);
            full = true;
        }
        else
        {
            auto it = g_rows.find(guid);
            const bool enrolled = it != g_rows.end() && it->second.enrolled;

            // A master asked mid-session (`nc +autopilot`). The rules decide
            // whether that is allowed, including a GM's forced off.
            if (!enrolled && !ob.markerOurs && HasMarker(ai))
            {
                Reevaluate(bot, ai, guid, ob);
                full = true;
            }
        }

        auto it = g_rows.find(guid);
        if (it == g_rows.end() || !it->second.enrolled)
            return full;
        Row& row = it->second;

        const uint32_t zone = bot->GetZoneId();
        if (ob.lastZone != 0 && zone != ob.lastZone)
        {
            RecordEvent(guid, "zone", Progress_ZoneName(zone));
            // A zone the bot has not been to this session counts as discovery.
            if (ob.zonesSeen.insert(zone).second)
                NoteReward(ob, "discovery", 1, now);
        }
        else if (ob.lastZone == 0)
        {
            ob.zonesSeen.insert(zone);
        }
        ob.lastZone = zone;

        if (ob.nextSnapshotAt != 0 && now >= ob.nextSnapshotAt)
            TakeSnapshot(bot, row, ob, now);

        Control(bot, ai, guid, row, ob, now);
        return true;
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
                "(bot_guid, mode, enrolled, source, style, outlook, profile, kills_total, deaths_total, "
                "quests_total, dungeons_total, doing, doing_since, decided_by, strategies, rpg, playbook, "
                "goal_kind, goal_target, goal_target_id, goal_value, goal_baseline, goal_set_at, goal_text, "
                "last_reason, baseline, plan_until, last_plan_at, enrolled_at, updated_at) VALUES " + values +
                " ON DUPLICATE KEY UPDATE mode = VALUES(mode), enrolled = VALUES(enrolled), "
                "source = VALUES(source), style = VALUES(style), outlook = VALUES(outlook), "
                "profile = VALUES(profile), kills_total = VALUES(kills_total), "
                "deaths_total = VALUES(deaths_total), quests_total = VALUES(quests_total), "
                "dungeons_total = VALUES(dungeons_total), doing = VALUES(doing), "
                "doing_since = VALUES(doing_since), decided_by = VALUES(decided_by), "
                "strategies = VALUES(strategies), rpg = VALUES(rpg), playbook = VALUES(playbook), "
                "goal_kind = VALUES(goal_kind), goal_target = VALUES(goal_target), "
                "goal_target_id = VALUES(goal_target_id), goal_value = VALUES(goal_value), "
                "goal_baseline = VALUES(goal_baseline), goal_set_at = VALUES(goal_set_at), "
                "goal_text = VALUES(goal_text), last_reason = VALUES(last_reason), "
                "baseline = VALUES(baseline), plan_until = VALUES(plan_until), "
                "last_plan_at = VALUES(last_plan_at), "
                "enrolled_at = VALUES(enrolled_at), updated_at = VALUES(updated_at)");
            values.clear();
            count = 0;
        };

        auto esc = [](std::string s, size_t max)
        {
            if (s.size() > max)
                s = Utf8Truncate(std::move(s), max);
            CharacterDatabase.EscapeString(s);
            return s;
        };

        for (auto& [guid, row] : g_rows)
        {
            if (!row.dirty)
                continue;

            if (!values.empty())
                values += ',';
            const AutopilotGoal& g = row.goal;
            values += SafeFormat(
                "({}, {}, {}, '{}', '{}', '{}', '{}', {}, {}, {}, {}, '{}', {}, '{}', '{}', '{}', '{}', "
                "'{}', '{}', {}, {}, {}, {}, '{}', '{}', '{}', {}, {}, {}, {})",
                guid, uint32_t(row.mode), row.enrolled ? 1 : 0, esc(row.source, 32),
                esc(row.style, 64), esc(row.outlook, 32), esc(row.profile, 1000),
                row.killsTotal, row.deathsTotal, row.questsTotal, row.dungeonsTotal,
                esc(row.doing, 64), row.doingSince, esc(row.decidedBy, 16),
                esc(AutopilotStrategies_Format(row.desired), 1000), esc(JoinList(row.rpg), 255),
                esc(SerializePlaybook(row.playbook), 1000),
                g.Active() ? Goal_KindName(g.kind) : "", esc(g.target, 64), g.targetId, g.value,
                g.baseline, g.setAt, esc(g.text, 255), esc(row.lastReason, 255),
                esc(AutopilotStrategies_Format(row.baseline), 1000), row.planUntil, row.lastPlanAt,
                row.enrolledAt, now);
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

    Online* OnlineOf(uint64_t guid)
    {
        auto it = g_online.find(guid);
        return it == g_online.end() ? nullptr : &it->second;
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

    Online& TrackOnline(uint64_t guid)
    {
        Online& ob = g_online[guid];
        if (std::find(g_roster.begin(), g_roster.end(), guid) == g_roster.end())
            g_roster.push_back(guid);
        return ob;
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

        Online& ob = TrackOnline(guid);
        Reevaluate(bot, BotAI(bot), guid, ob);

        Row const& after = g_rows[guid];
        handler->SendSysMessage(SafeFormat(
            "OllamaChat: {} is now {} (mode: {}{}).", bot->GetName(),
            after.enrolled ? "on autopilot" : "not on autopilot", ModeName(after.mode),
            after.enrolled ? ", source: " + after.source : std::string()));
        if (!Autopilot_IsActive())
            handler->SendSysMessage("OllamaChat: note - autopilot is not active, see `.ollama autopilot status`.");
        return true;
    }

    bool HandleOn(ChatHandler* handler, std::string name)    { return SetMode(handler, name, MODE_ON); }
    bool HandleOff(ChatHandler* handler, std::string name)   { return SetMode(handler, name, MODE_OFF); }
    bool HandleRules(ChatHandler* handler, std::string name) { return SetMode(handler, name, MODE_RULES); }

    void SendInactiveReasons(ChatHandler* handler)
    {
        if (!g_Enable)
            handler->SendSysMessage("  - OllamaChat.Enable is 0");
        if (!g_cfg.enable)
            handler->SendSysMessage("  - OllamaChat.Autopilot.Enable is 0");
        if (!g_EnableChatBotSnapshotTemplate)
            handler->SendSysMessage("  - OllamaChat.EnableChatBotSnapshotTemplate is 0 (required)");
        if (!g_tablesOk)
            handler->SendSysMessage("  - autopilot tables are missing or out of date (apply data/sql/characters/base/2026_10_05_autopilot.sql)");
        if (!AutopilotStrategy_IsRegistered())
            handler->SendSysMessage("  - the 'autopilot' playerbots strategy failed to register (see startup log)");
    }

    // Look up an enrolled, online bot for a control command. g_mutex held.
    bool ControlTarget(ChatHandler* handler, Player* bot, Row*& row, Online*& ob)
    {
        const uint64_t guid = bot->GetGUID().GetRawValue();
        auto rowIt = g_rows.find(guid);
        if (rowIt == g_rows.end() || !rowIt->second.enrolled)
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: {} is not on autopilot.", bot->GetName()));
            return false;
        }
        row = &rowIt->second;
        ob  = &TrackOnline(guid);
        return true;
    }

    bool HandleStatus(ChatHandler* handler, Optional<std::string> name)
    {
        if (!name)
        {
            std::lock_guard<std::mutex> lock(g_mutex);

            uint32_t onlineEnrolled = 0, withIdentity = 0;
            uint32_t tiers[3] = { 0, 0, 0 };
            for (uint64_t guid : g_roster)
            {
                auto it = g_rows.find(guid);
                if (it == g_rows.end() || !it->second.enrolled)
                    continue;
                ++onlineEnrolled;
                if (it->second.HasIdentity())
                    ++withIdentity;
                if (auto on = g_online.find(guid); on != g_online.end())
                    ++tiers[static_cast<int>(on->second.tier)];
            }

            handler->SendSysMessage(SafeFormat("OllamaChat Autopilot: {}",
                                               Autopilot_IsActive() ? "active" : "INACTIVE"));
            if (!Autopilot_IsActive())
                SendInactiveReasons(handler);
            handler->SendSysMessage(SafeFormat(
                "  enrolled: {} total, {} online ({} with an identity from the model) | rule-enrolled {} of cap {}",
                g_enrolledCount, onlineEnrolled, withIdentity, g_cappedCount,
                g_cfg.maxEnrolled ? std::to_string(g_cfg.maxEnrolled) : std::string("none")));
            handler->SendSysMessage(SafeFormat(
                "  tiers (as last computed): {} foreground, {} background, {} dormant",
                tiers[2], tiers[1], tiers[0]));

            const AutopilotPlannerStats ps = AutopilotPlanner_GetStats();
            handler->SendSysMessage(SafeFormat(
                "  plans applied {} | unusable replies {} | strategies refused (not allowed) {} | guards {} | "
                "rpg steers {} | human locks {}",
                g_statPlans, g_statInvalid, g_statRejected, g_statGuard, g_statSteer, g_statLocks));
            handler->SendSysMessage(SafeFormat(
                "  llm: {} | budget {:.1f}/{:.0f} tokens ({}/h) | in flight {} | submitted {}, refused {}, "
                "parsed {}, failed {}{}",
                g_cfg.llmEnable ? "on" : "off", ps.tokens, ps.capacity, g_cfg.llmCallsPerHour, ps.inFlight,
                ps.submitted, ps.refused, ps.parsed, ps.failed,
                ps.lastError.empty() ? "" : " | last error: " + ps.lastError));
            return true;
        }

        Player* bot = FindBot(handler, *name);
        if (!bot)
            return true;

        const uint64_t guid = bot->GetGUID().GetRawValue();
        const uint32_t now  = Progress_Now();
        Row    row;
        Online ob;
        bool   hasRow = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (auto it = g_rows.find(guid); it != g_rows.end())
            {
                row    = it->second;
                hasRow = true;
            }
            if (auto on = g_online.find(guid); on != g_online.end())
                ob = on->second;
        }

        if (!hasRow)
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: {} has never been on autopilot.", bot->GetName()));
            return true;
        }

        PlayerbotAI* ai = BotAI(bot);
        handler->SendSysMessage(SafeFormat(
            "OllamaChat: {} - {} (mode: {}, source: {}) | tier: {}{}",
            bot->GetName(), row.enrolled ? "ON autopilot" : "not on autopilot", ModeName(row.mode),
            row.source.empty() ? "-" : row.source, TierName(ob.tier),
            ob.planPending ? " | waiting on the model" : ""));
        handler->SendSysMessage(row.HasIdentity()
            ? SafeFormat("  identity: {} ({}) - {}", row.style, row.outlook, row.profile)
            : std::string("  identity: none yet - the model writes one on its first plan"));
        handler->SendSysMessage(SafeFormat(
            "  doing: {} (since {}) | next plan due: {}",
            row.doing.empty() ? "-" : row.doing, row.doingSince ? Ago(row.doingSince, now) : "-",
            row.planUntil > now ? "in " + Span(row.planUntil - now) : "now"));
        handler->SendSysMessage("  wants: " + (row.desired.empty() ? std::string("-") : ChangesText(row.desired)));
        handler->SendSysMessage(SafeFormat("  rpg focus: {}{} | now: {}",
            row.rpg.empty() ? "free roaming" : JoinList(row.rpg),
            ob.guardRpg.empty() ? "" : " (overridden: " + ob.lastGuard + ")",
            ai ? AutopilotRpg_StatusName(AutopilotRpg_CurrentStatus(ai)) : "-"));
        if (ai)
            handler->SendSysMessage("  live: " + AutopilotStrategies_DescribeLive(ai));
        for (const auto& [situation, changes] : row.playbook)
            handler->SendSysMessage(SafeFormat("  playbook {}: {}", situation, ChangesText(changes)));
        handler->SendSysMessage("  goal: " + (row.goal.Active() ? Goal_Describe(bot, row.goal, Counters(row))
                                                                : std::string("-")));
        handler->SendSysMessage("  last reason: " + (row.lastReason.empty() ? std::string("-") : row.lastReason));
        const std::string rewards = RewardSummary(ob, now);
        handler->SendSysMessage("  last hour: " + (rewards.empty() ? std::string("nothing rewarding") : rewards));
        for (const auto& [at, text] : ob.temptations)
            if (now - at <= kHour)
                handler->SendSysMessage(SafeFormat("  tempted ({}): {}", Ago(at, now), text));
        if (!ob.locked.empty())
        {
            std::string locked;
            for (const std::string& s : ob.locked)
                locked += (locked.empty() ? "" : ", ") + s;
            handler->SendSysMessage("  left to the group's human: " + locked);
        }
        handler->SendSysMessage(SafeFormat(
            "  since enrollment: {} kills, {} deaths, {} quests completed, {} dungeons",
            row.killsTotal, row.deathsTotal, row.questsTotal, row.dungeonsTotal));

        std::vector<ProgressSnapshot> snaps = Progress_LoadSnapshots(guid, now > 86400 ? now - 86400 : 0, 2000);
        if (std::string summary = Progress_Summarize(snaps); !summary.empty())
            handler->SendSysMessage("  " + summary);
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
        uint32_t tiers[3] = { 0, 0, 0 };

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
            const bool markerIsRequest = onIt != g_online.end() && onIt->second.loginSettled &&
                                         !onIt->second.markerOurs;

            const std::string source = EvaluateRules(player, ai, row, markerIsRequest);
            if (source.empty())
                continue;

            ++matched;
            ++bySource[source];
            ++tiers[static_cast<int>(ComputeTier(player))];
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

        handler->SendSysMessage(SafeFormat(
            "  tiers right now: {} foreground, {} background, {} dormant.", tiers[2], tiers[1], tiers[0]));
        const uint32_t wanted =
            tiers[2] * 60 / std::max<uint32_t>(1, g_cfg.decisionIntervalMinutes) +
            tiers[1] * 60 / std::max<uint32_t>(1, g_cfg.goalRefreshMinutes);
        handler->SendSysMessage(SafeFormat(
            "  LLM: up to ~{} plans/hour at the tier intervals; the budget allows {}/hour. Bots that cannot be "
            "planned for keep doing what the model last chose; dormant bots wait until a player comes near.",
            wanted, g_cfg.llmCallsPerHour));

        if (!Autopilot_IsActive())
        {
            handler->SendSysMessage("  Autopilot is not active:");
            SendInactiveReasons(handler);
        }
        return true;
    }

    // `identity <bot>` shows it; `identity <bot> clear` has the model write a
    // new one on its next plan.
    bool HandleIdentity(ChatHandler* handler, std::string name, Optional<std::string> action)
    {
        Player* bot = FindBot(handler, name);
        if (!bot)
            return true;

        std::lock_guard<std::mutex> lock(g_mutex);
        Row*    row = nullptr;
        Online* ob  = nullptr;
        if (!ControlTarget(handler, bot, row, ob))
            return true;

        if (action && Lower(*action) == "clear")
        {
            row->style.clear();
            row->outlook.clear();
            row->profile.clear();
            row->dirty     = true;
            ob->urgentPlan = true;
            RecordEvent(bot->GetGUID().GetRawValue(), "identity", "cleared by GM; the model will write a new one");
            handler->SendSysMessage(SafeFormat("OllamaChat: {}'s identity cleared; the model writes a new one on "
                                               "its next plan.", bot->GetName()));
            return true;
        }

        handler->SendSysMessage(row->HasIdentity()
            ? SafeFormat("OllamaChat: {} - {} ({}): {}", bot->GetName(), row->style, row->outlook, row->profile)
            : SafeFormat("OllamaChat: {} has no identity yet.", bot->GetName()));
        return true;
    }

    bool HandleReplan(ChatHandler* handler, std::string name)
    {
        Player* bot = FindBot(handler, name);
        if (!bot)
            return true;

        std::lock_guard<std::mutex> lock(g_mutex);
        Row*    row = nullptr;
        Online* ob  = nullptr;
        if (!ControlTarget(handler, bot, row, ob))
            return true;

        PlayerbotAI* ai = BotAI(bot);
        if (!Classify(bot, ai).CanUseNonCombat())
        {
            ob->urgentPlan = true;
            handler->SendSysMessage(SafeFormat(
                "OllamaChat: {} is in a group, instance or dead; it will replan when that ends.", bot->GetName()));
            return true;
        }

        if (ob->planPending)
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: already waiting on the model for {}.", bot->GetName()));
            return true;
        }
        const bool asked = TrySubmitPlan(bot, ai, bot->GetGUID().GetRawValue(), *row, *ob, Progress_Now(), true);
        handler->SendSysMessage(asked
            ? SafeFormat("OllamaChat: asked the model what {} should do next.", bot->GetName())
            : SafeFormat("OllamaChat: could not ask the model for {} right now (LLM off, budget spent or too many "
                         "plans in flight); it keeps its current plan.", bot->GetName()));
        return true;
    }

    // `.ollama autopilot goal <bot>` shows it; `goal <bot> clear` drops it;
    // `goal <bot> <kind> <target...>` sets it, e.g. `goal Bob reach_skill mining 150`.
    bool HandleGoal(ChatHandler* handler, std::string name, Optional<std::string> kind, Optional<Tail> target)
    {
        Player* bot = FindBot(handler, name);
        if (!bot)
            return true;

        std::lock_guard<std::mutex> lock(g_mutex);
        Row*    row = nullptr;
        Online* ob  = nullptr;
        if (!ControlTarget(handler, bot, row, ob))
            return true;

        const uint64_t guid = bot->GetGUID().GetRawValue();
        if (!kind)
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: {} goal: {}", bot->GetName(),
                row->goal.Active() ? Goal_Describe(bot, row->goal, Counters(*row)) : std::string("-")));
            handler->SendSysMessage(std::string("Kinds:\n") + Goal_KindMenu());
            return true;
        }

        if (Lower(*kind) == "clear")
        {
            row->goal  = AutopilotGoal();
            row->dirty = true;
            RecordEvent(guid, "goal", "cleared by GM");
            handler->SendSysMessage(SafeFormat("OllamaChat: {} has no goal now.", bot->GetName()));
            return true;
        }

        const std::string targetText = target ? std::string(*target) : std::string();
        AutopilotGoal goal;
        const std::string why = Goal_Resolve(bot, *kind, targetText, Lower(*kind) == "free" ? targetText : "",
                                             Counters(*row), goal);
        if (!why.empty())
        {
            handler->SendSysMessage("OllamaChat: goal not set: " + why);
            return true;
        }

        SetGoal(guid, *row, ob, std::move(goal), "gm");
        ob->urgentPlan = true;
        handler->SendSysMessage(SafeFormat("OllamaChat: {} goal: {} (the model plans around it next).",
                                           bot->GetName(), Goal_Describe(bot, row->goal, Counters(*row))));
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

    c.control                 = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.Control", true);
    c.withRealPlayer          = std::min<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.WithRealPlayer", 1));
    c.llmEnable               = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.Llm.Enable", true);
    c.llmCallsPerHour         = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.LlmCallsPerHour", 300);
    c.maxConcurrentPlans      = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.MaxConcurrentPlans", 2));
    c.decisionIntervalMinutes = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.DecisionIntervalMinutes", 15));
    c.goalRefreshMinutes      = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.GoalRefreshMinutes", 90));
    c.defaultPlanMinutes      = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.DefaultPlanMinutes", 45), 10, 180);
    c.foregroundRange         = sConfigMgr->GetOption<float>("OllamaChat.Autopilot.ForegroundRange", 100.0f);
    c.planTimeoutSeconds      = std::max<uint32_t>(30, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.PlanTimeoutSeconds", 300));
    c.promptTemplate          = sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.PromptTemplate", "");

    auto parseScope = [](const std::string& key, const char* fallback, Scope def)
    {
        const std::string v = Lower(sConfigMgr->GetOption<std::string>(key, fallback));
        if (v == "range")  return Scope::Range;
        if (v == "zone")   return Scope::Zone;
        if (v == "map")    return Scope::Map;
        if (v == "world")  return Scope::World;
        if (v == "always") return Scope::Always;
        LOG_ERROR("module.ollamachat", "[Ollama Chat] {}: '{}' is not range/zone/map/world/always.", key, v);
        return def;
    };
    auto parseTier = [](const std::string& key, const char* fallback, Tier def)
    {
        const std::string v = Lower(sConfigMgr->GetOption<std::string>(key, fallback));
        if (v == "dormant")    return Tier::Dormant;
        if (v == "background") return Tier::Background;
        if (v == "foreground") return Tier::Foreground;
        LOG_ERROR("module.ollamachat", "[Ollama Chat] {}: '{}' is not dormant/background/foreground.", key, v);
        return def;
    };

    c.foregroundScope         = parseScope("OllamaChat.Autopilot.ForegroundScope", "zone", Scope::Zone);
    c.backgroundScope         = parseScope("OllamaChat.Autopilot.BackgroundScope", "map", Scope::Map);
    c.minimumTier             = parseTier("OllamaChat.Autopilot.MinimumTier", "dormant", Tier::Dormant);
    c.selectRealPlayerGuilds  = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.Select.RealPlayerGuilds", false);
    c.realGuildTier           = parseTier("OllamaChat.Autopilot.RealPlayerGuildTier", "background", Tier::Background);
    c.realGuildRefreshMinutes = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.RealPlayerGuildRefreshMinutes", 10));

    c.guardDeaths             = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Guard.Deaths", 3);
    c.guardDeathWindowMinutes = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Guard.DeathWindowMinutes", 15));
    c.guardDurabilityPct      = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Guard.DurabilityPct", 20);
    c.guardFreeBagSlots       = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Guard.FreeBagSlots", 2);
    c.guardHoldMinutes        = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Guard.HoldMinutes", 10));

    c.goalStaleMinutes        = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.GoalStaleMinutes", 120);

    if (c.minLevel > c.maxLevel)
        std::swap(c.minLevel, c.maxLevel);

    AutopilotStrategies_Load();
    AutopilotPlanner_ConfigureBudget(c.llmCallsPerHour, c.maxConcurrentPlans);

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

    if (g_tablesOk)
    {
        // Columns changed while the table was still unreleased; a database
        // set up from an earlier draft needs the file re-applied.
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() "
                "AND table_name = 'mod_ollama_chat_autopilot' AND column_name IN ('profile', 'strategies', 'baseline')"))
            g_tablesOk = (*result)[0].Get<uint64>() == 3;
    }

    if (!g_tablesOk)
    {
        if (g_cfg.enable)
            LOG_ERROR("server.loading",
                      "[Ollama Chat] Autopilot tables are missing or out of date; drop "
                      "mod_ollama_chat_autopilot and apply data/sql/characters/base/2026_10_05_autopilot.sql. "
                      "Autopilot stays off.");
        return;
    }

    // Clear out characters deleted while we were not watching before loading,
    // so they never take a MaxEnrolled place. The lookup is synchronous (this
    // is startup); the delete is queued, so the load below skips them itself.
    std::unordered_set<uint64_t> orphans;
    {
        std::vector<uint64_t> list;
        if (QueryResult result = CharacterDatabase.Query(kOrphanQuery))
            do { list.push_back((*result)[0].Get<uint64>()); } while (result->NextRow());
        if (!list.empty())
        {
            DeleteOrphans(list);
            orphans.insert(list.begin(), list.end());
            LOG_INFO("server.loading", "[Ollama Chat] Autopilot: removed {} rows for deleted characters.",
                     list.size());
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_rows.clear();
    g_enrolledCount = 0;
    g_cappedCount   = 0;

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT bot_guid, mode, enrolled, source, style, outlook, profile, kills_total, deaths_total, "
            "quests_total, dungeons_total, doing, doing_since, decided_by, strategies, rpg, playbook, "
            "goal_kind, goal_target, goal_target_id, goal_value, goal_baseline, goal_set_at, goal_text, "
            "last_reason, enrolled_at, baseline, plan_until, last_plan_at FROM mod_ollama_chat_autopilot"))
    {
        do
        {
            Field* f = result->Fetch();
            if (orphans.count(f[0].Get<uint64>()))
                continue;

            Row row;
            row.mode          = f[1].Get<uint8>();
            row.enrolled      = f[2].Get<uint8>() != 0;
            row.source        = f[3].Get<std::string>();
            row.style         = f[4].Get<std::string>();
            row.outlook       = f[5].Get<std::string>();
            row.profile       = f[6].Get<std::string>();
            row.killsTotal    = f[7].Get<uint32>();
            row.deathsTotal   = f[8].Get<uint32>();
            row.questsTotal   = f[9].Get<uint32>();
            row.dungeonsTotal = f[10].Get<uint32>();
            row.doing         = f[11].Get<std::string>();
            row.doingSince    = f[12].Get<uint32>();
            row.decidedBy     = f[13].Get<std::string>();
            row.desired       = AutopilotStrategies_Parse(f[14].Get<std::string>());
            row.rpg           = SplitString(f[15].Get<std::string>(), ',');
            row.playbook      = ParsePlaybook(f[16].Get<std::string>());

            const std::string kind = f[17].Get<std::string>();
            row.goal.kind     = kind.empty() ? GoalKind::None : Goal_KindFromName(kind);
            row.goal.target   = f[18].Get<std::string>();
            row.goal.targetId = f[19].Get<uint32>();
            row.goal.value    = f[20].Get<uint32>();
            row.goal.baseline = f[21].Get<uint32>();
            row.goal.setAt    = f[22].Get<uint32>();
            row.goal.text     = f[23].Get<std::string>();
            row.lastReason    = f[24].Get<std::string>();
            row.enrolledAt    = f[25].Get<uint32>();
            row.baseline      = AutopilotStrategies_Parse(f[26].Get<std::string>());
            row.planUntil     = f[27].Get<uint32>();
            row.lastPlanAt    = f[28].Get<uint32>();

            if (row.enrolled)
                CountEnrolled(row, +1);
            g_rows.emplace(f[0].Get<uint64>(), std::move(row));
        } while (result->NextRow());
    }

    LOG_INFO("server.loading", "[Ollama Chat] Autopilot: loaded {} rows, {} enrolled.",
             g_rows.size(), g_enrolledCount);

    // Lands in Autopilot_Update before the first bots log in.
    RefreshRealGuilds();
}

bool Autopilot_IsActive()
{
    return g_Enable && g_cfg.enable && g_EnableChatBotSnapshotTemplate && g_tablesOk &&
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

    // History loads land here, on the world thread. Not under g_mutex: the
    // callbacks take it themselves.
    g_callbacks.ProcessReadyCallbacks();

    g_flushTimer += diff;
    if (g_flushTimer >= g_cfg.flushIntervalSeconds * 1000)
    {
        g_flushTimer = 0;
        Autopilot_SaveAll();
    }

    // Catches humans leaving guilds and guilds disbanding; joins and logins
    // are picked up immediately by the hooks.
    g_guildTimer += diff;
    if (g_guildTimer >= g_cfg.realGuildRefreshMinutes * 60 * 1000)
    {
        g_guildTimer = 0;
        RefreshRealGuilds();
    }

    // Characters deleted without a script hook (playerbots' bulk reset uses
    // raw SQL) are caught here within the hour. One indexed anti-join.
    g_orphanTimer += diff;
    if (g_orphanTimer >= kOrphanCheckMs)
    {
        g_orphanTimer = 0;
        PruneOrphansAsync();
    }

    const bool active = Autopilot_IsActive();

    // Switched off (reload with Enable = 0, the module disabled): hand every
    // bot we were steering back to playerbots rather than leaving our
    // strategies on it indefinitely.
    if (g_wasActive && !active)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ReleaseAll();
    }
    g_wasActive = active;

    // Finished plans. While inactive they are dropped, not applied.
    std::vector<AutopilotDecision> decisions = AutopilotPlanner_Drain();
    if (!decisions.empty())
    {
        const uint32_t now = Progress_Now();
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const AutopilotDecision& d : decisions)
        {
            if (active)
                ApplyDecision(d, now);
            else if (auto it = g_online.find(d.botGuid); it != g_online.end())
                it->second.planPending = false;
        }
    }

    if (!active)
        return;

    g_sweepTimer += diff;
    if (g_sweepTimer < g_cfg.sweepIntervalMs)
        return;
    g_sweepTimer = 0;

    const uint32_t now = Progress_Now();
    std::lock_guard<std::mutex> lock(g_mutex);

    // Round-robin with a fixed budget: BotsPerSweep full visits (enrolled or
    // not yet evaluated bots), plus cheap checks of the rest -- a lookup and a
    // marker test -- capped at a few times that. Per-tick cost stays flat
    // however many bots are online, and the budget goes to enrolled bots.
    const size_t limit = std::min<size_t>(g_roster.size(), size_t(g_cfg.botsPerSweep) * 8);
    size_t full = 0;
    for (size_t i = 0; i < limit && full < g_cfg.botsPerSweep; ++i)
    {
        if (g_cursor >= g_roster.size())
            g_cursor = 0;

        const uint64_t guid = g_roster[g_cursor++];
        auto it = g_online.find(guid);
        if (it != g_online.end() && VisitBot(guid, it->second, now))
            ++full;
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
        { "on",       HandleOn,       SEC_ADMINISTRATOR, Console::Yes },
        { "off",      HandleOff,      SEC_ADMINISTRATOR, Console::Yes },
        { "rules",    HandleRules,    SEC_ADMINISTRATOR, Console::Yes },
        { "status",   HandleStatus,   SEC_ADMINISTRATOR, Console::Yes },
        { "history",  HandleHistory,  SEC_ADMINISTRATOR, Console::Yes },
        { "preview",  HandlePreview,  SEC_ADMINISTRATOR, Console::Yes },
        { "identity", HandleIdentity, SEC_ADMINISTRATOR, Console::Yes },
        { "replan",   HandleReplan,   SEC_ADMINISTRATOR, Console::Yes },
        { "goal",     HandleGoal,     SEC_ADMINISTRATOR, Console::Yes },
    };
    return table;
}

// ==========================================================================
// Hooks. The progress hooks run on map threads; each touches only the player
// it was handed and module state under g_mutex (taken by WithEnrolledRow).
// ==========================================================================

AutopilotPlayerScript::AutopilotPlayerScript()
    : PlayerScript("AutopilotPlayerScript", {
          PLAYERHOOK_ON_LOGIN,
          PLAYERHOOK_ON_LOGOUT,
          PLAYERHOOK_ON_CREATURE_KILL,
          PLAYERHOOK_ON_PVP_KILL,
          PLAYERHOOK_ON_PLAYER_JUST_DIED,
          PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
          PLAYERHOOK_ON_LEVEL_CHANGED,
          PLAYERHOOK_ON_STORE_NEW_ITEM,
          PLAYERHOOK_ON_ACHI_COMPLETE,
          PLAYERHOOK_ON_DELETE_FROM_DB,
      }) { }

void AutopilotPlayerScript::OnPlayerLogin(Player* player)
{
    if (!player)
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);

    // Session test, not the AI lookup: the AI may not be attached yet (see
    // OllamaIsBotPlayer).
    if (!OllamaIsBotPlayer(player))
    {
        g_realOnline.insert(guid);
        if (player->GetGuildId() && g_realGuilds.insert(player->GetGuildId()).second)
            OnRealGuildsChanged();
        return;
    }

    // Tracked even while autopilot is off, so turning it on with a reload
    // picks up bots that are already online.
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

    g_realOnline.erase(guid);

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

        // The last bot (not yet visited this cycle) was moved into a slot the
        // cursor has already passed. Swap it with the slot just behind the
        // cursor (already visited) and step the cursor back onto it, so it is
        // visited next rather than skipped for a whole cycle.
        if (index < g_cursor && g_cursor > 0 && g_cursor <= g_roster.size())
        {
            --g_cursor;
            std::swap(g_roster[index], g_roster[g_cursor]);
        }
    }
}

void AutopilotPlayerScript::OnPlayerCreatureKill(Player* killer, Creature* /*victim*/)
{
    WithEnrolledRow(killer, [](uint64_t guid, Row& row)
    {
        ++row.killsTotal;
        if (Online* ob = OnlineOf(guid))
            if (++ob->killsPending >= 10)
            {
                NoteReward(*ob, "kills", ob->killsPending, Progress_Now());
                ob->killsPending = 0;
            }
    });
}

void AutopilotPlayerScript::OnPlayerPVPKill(Player* killer, Player* /*killed*/)
{
    WithEnrolledRow(killer, [](uint64_t guid, Row&)
    {
        if (Online* ob = OnlineOf(guid))
            NoteReward(*ob, "pvp", 1, Progress_Now());
    });
}

void AutopilotPlayerScript::OnPlayerJustDied(Player* player)
{
    WithEnrolledRow(player, [player](uint64_t guid, Row& row)
    {
        ++row.deathsTotal;
        RecordEvent(guid, "death", Progress_ZoneName(player->GetZoneId()));

        if (Online* ob = OnlineOf(guid))
            ob->deathTimes.push_back(Progress_Now());
    });
}

void AutopilotPlayerScript::OnPlayerCompleteQuest(Player* player, Quest const* quest)
{
    if (!quest)
        return;

    WithEnrolledRow(player, [quest](uint64_t guid, Row& row)
    {
        ++row.questsTotal;
        RecordEvent(guid, "quest", quest->GetTitle());
        if (Online* ob = OnlineOf(guid))
            NoteReward(*ob, "quest", 1, Progress_Now());
    });
}

void AutopilotPlayerScript::OnPlayerLevelChanged(Player* player, uint8 oldLevel)
{
    WithEnrolledRow(player, [player, oldLevel](uint64_t guid, Row&)
    {
        const uint32_t level = player->GetLevel();
        RecordEvent(guid, "level", SafeFormat("{} -> {}", uint32_t(oldLevel), level));

        Online* ob = OnlineOf(guid);
        if (!ob)
            return;

        const uint32_t now = Progress_Now();
        if (level > oldLevel)
            NoteReward(*ob, "level", level - oldLevel, now);

        // Doors that just opened.
        if (oldLevel < 10 && level >= 10)
            AddTemptation(*ob, "old enough for battlegrounds now (level 10)", now);
        if (oldLevel < 15 && level >= 15)
            AddTemptation(*ob, "old enough for dungeon-finder groups now (level 15)", now);

        // A level is a natural moment for a snapshot and a fresh look.
        ob->nextSnapshotAt = 1;
        ob->urgentPlan     = true;
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
        const std::string what = SafeFormat("{} ({})", tmpl->Name1, quality);
        RecordEvent(guid, "loot", what);

        if (Online* ob = OnlineOf(guid))
        {
            const uint32_t now = Progress_Now();
            NoteReward(*ob, "loot", 1, now);
            AddTemptation(*ob, "just found " + what, now);
        }
    });
}

void AutopilotPlayerScript::OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement)
{
    if (!achievement || !achievement->name[0])
        return;

    WithEnrolledRow(player, [achievement](uint64_t guid, Row&)
    {
        RecordEvent(guid, "achievement", achievement->name[0]);
    });
}

void AutopilotPlayerScript::OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 lowGuid)
{
    // Runs inside Player::DeleteFromDB for every real delete (client, GM,
    // playerbots' per-bot deletes). Our rows go in the same transaction as the
    // character's, so they cannot outlive it. Player guids have no high part,
    // so the raw guid we store is the low guid.
    if (!g_tablesOk)
        return;

    const uint64_t guid = lowGuid;
    for (const char* table : { "mod_ollama_chat_autopilot", "mod_ollama_chat_autopilot_snapshots",
                               "mod_ollama_chat_autopilot_events" })
        trans->Append(DeleteRowsSql(table, std::to_string(guid)));

    std::lock_guard<std::mutex> lock(g_mutex);
    ForgetRows({ guid });
}

AutopilotGuildScript::AutopilotGuildScript()
    : GuildScript("AutopilotGuildScript", { GUILDHOOK_ON_ADD_MEMBER }) { }

void AutopilotGuildScript::OnAddMember(Guild* guild, Player* player, uint8& /*plRank*/)
{
    // A human joining (or founding) a guild makes its bots eligible now
    // rather than at the next refresh.
    if (!guild || !player || OllamaIsBotPlayer(player))
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_realGuilds.insert(guild->GetId()).second)
        OnRealGuildsChanged();
}
