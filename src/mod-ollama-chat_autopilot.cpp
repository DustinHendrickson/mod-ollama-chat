#include "mod-ollama-chat_autopilot.h"
#include "mod-ollama-chat_autopilot_goals.h"
#include "mod-ollama-chat_autopilot_planner.h"
#include "mod-ollama-chat_autopilot_presets.h"
#include "mod-ollama-chat_autopilot_strategy.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_playstyle.h"
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
#include "Random.h"
#include "WorldSession.h"

#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "PlayerbotRepository.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <deque>
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
        bool     control                 = true;    // apply activities and dispositions at all
        uint32_t withRealPlayer          = 1;       // 0 hands off, 1 dispositions only
        bool     llmEnable               = true;
        uint32_t llmCallsPerHour         = 300;
        uint32_t maxConcurrentPlans      = 2;
        uint32_t decisionIntervalMinutes = 15;      // foreground
        uint32_t goalRefreshMinutes      = 90;      // background
        float    foregroundRange         = 100.0f;  // yards, for Scope::Range

        // Reach: who gets LLM time. Defaults keep it to bots near people.
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

        // Feelings and goals.
        float    boredomThreshold        = 70.0f;
        float    satisfiedScore          = 10.0f;   // reward score per hour that feels "enough"
        uint32_t goalStaleMinutes        = 120;
        bool     policyGoals             = true;    // the policy sets goals when the model has not
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

        std::string activity;
        uint32_t    activitySince = 0;
        std::string decidedBy;
        std::vector<std::pair<std::string, std::string>> dispositions;   // axis -> option
        std::vector<std::pair<std::string, std::string>> playbook;       // situation -> option
        AutopilotGoal goal;
        std::string lastReason;
        uint32_t    dungeonsTotal = 0;

        bool        dirty       = false;
    };

    GoalCounters Counters(const Row& row)
    {
        GoalCounters c;
        c.questsTotal   = row.questsTotal;
        c.dungeonsTotal = row.dungeonsTotal;
        return c;
    }

    // Reward channels for the mood model. Amounts are normalised by `unit`
    // so a kill and a quest are not weighed one for one.
    struct RewardChannel { const char* name; float unit; };
    constexpr RewardChannel kRewardChannels[] = {
        { "level", 1.0f }, { "quest", 1.0f }, { "kills", 10.0f }, { "pvp", 1.0f },
        { "loot", 1.0f },  { "gold", 5.0f },  { "skill", 5.0f },  { "discovery", 1.0f },
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

        // Control.
        uint32_t activityUntil   = 0;
        bool     planPending     = false;
        uint32_t planSubmittedAt = 0;
        uint32_t lastPlanAt      = 0;
        bool     urgentPlan      = false;
        Tier     tier            = Tier::Dormant;
        bool     controlled      = false;   // we have applied presets this session
        std::string       appliedActivity;   // activity whose strategies are live on the bot
        AutopilotBaseline baseline;          // strategy states before autopilot touched them

        // Strategies a human changed by hand while grouped with the bot, and
        // the group that lock belongs to.
        std::set<std::string> locked;
        uint64_t              lockGroup = 0;

        std::deque<uint32_t> deathTimes;
        std::unordered_map<std::string, uint32_t> guardCooldown;   // guard kind -> may fire again at
        std::string          lastGuard;
        uint32_t             lastGuardAt = 0;

        // Recent history, for prompts without a DB round trip.
        bool                          historyRequested = false;
        std::deque<ProgressSnapshot>  snapshots;   // oldest first
        std::deque<ProgressEvent>     events;      // oldest first
        std::deque<std::string>       decisions;   // oldest first

        // Dispositions as last applied, after the playbook for the situation
        // the bot was in. Diffed against what is wanted now, so moving from
        // the open world into a dungeon reverts one option and applies another.
        std::vector<std::pair<std::string, std::string>> effective;

        // Mood.
        float                  boredom     = 0.0f;   // 0..100
        uint32_t               moodAt      = 0;
        bool                   boredNoted  = false;
        std::deque<Reward>     rewards;              // last hour
        uint32_t               lastMoney   = 0;
        uint32_t               lastSkillSum = 0;
        uint32_t               killsPending = 0;     // kills since the last reward entry
        std::unordered_set<uint32_t> zonesSeen;      // this session
        std::unordered_set<uint32_t> skillCapsNoted;
        std::deque<std::pair<uint32_t, std::string>> temptations;

        // Goal stall detection.
        uint32_t goalProgressValue = 0;
        uint32_t goalProgressAt    = 0;

        // Situation on the previous visit, for boundary decisions.
        bool     situationKnown = false;
        uint8_t  prevInstance   = 0;     // 0 none, 1 dungeon, 2 battleground
        bool     prevGrouped    = false;
        bool     prevTravelling = false;
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
    uint64_t g_statPolicy  = 0;
    uint64_t g_statLlm     = 0;
    uint64_t g_statInvalid = 0;
    uint64_t g_statGuard   = 0;
    uint64_t g_statSteer   = 0;
    uint64_t g_statLocks   = 0;

    constexpr uint32_t kLogoutSnapshotMinGap = 300;
    constexpr uint32_t kPlanGraceSeconds     = 180;

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

    // Return a bot to exactly what playerbots would give it without us: its
    // defaults, plus -- for alts -- the strategies its master saved, which
    // ResetStrategies alone does not reload (its Load call is commented out).
    // Mirrors PlayerbotHolder::OnBotLogin.
    void HandBack(PlayerbotAI* ai)
    {
        ai->ResetStrategies();
        if (Player* bot = ai->GetBot())
            if (!sRandomPlayerbotMgr.IsRandomBot(bot))
                PlayerbotRepository::instance().Load(ai);
    }

    bool HasMarker(PlayerbotAI* ai)
    {
        return ai->HasStrategy(AUTOPILOT_STRATEGY_NAME, BOT_STATE_NON_COMBAT);
    }

    void AddMarker(PlayerbotAI* ai)
    {
        ai->ChangeStrategy(std::string("+") + AUTOPILOT_STRATEGY_NAME, BOT_STATE_NON_COMBAT);
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

    std::string SerializeDispositions(const std::vector<std::pair<std::string, std::string>>& d)
    {
        std::string out;
        for (const auto& [axis, option] : d)
            out += (out.empty() ? "" : ",") + axis + ":" + option;
        return out;
    }

    std::vector<std::pair<std::string, std::string>> ParseDispositions(const std::string& text)
    {
        std::vector<std::pair<std::string, std::string>> out;
        for (const std::string& entry : SplitString(text, ','))
        {
            size_t colon = entry.find(':');
            if (colon != std::string::npos)
                out.emplace_back(Lower(entry.substr(0, colon)), Lower(entry.substr(colon + 1)));
        }
        return out;
    }

    std::string DispositionOf(const Row& row, const AutopilotDispositionAxis& axis)
    {
        for (const auto& [a, option] : row.dispositions)
            if (a == axis.name && AutopilotPresets_Disposition(a, option))
                return option;
        return axis.neutral;
    }

    // Fill every configured axis: the row's choice if still valid, else the
    // playstyle's default, else the axis neutral. Drops axes that no longer
    // exist. Returns true when anything changed.
    bool NormalizeDispositions(Row& row)
    {
        const PlaystyleProfile* profile = Playstyle_Profile(row.playstyle);

        std::vector<std::pair<std::string, std::string>> out;
        for (const AutopilotDispositionAxis& axis : AutopilotPresets_Axes())
        {
            std::string option;
            for (const auto& [a, o] : row.dispositions)
                if (a == axis.name && AutopilotPresets_Disposition(a, o))
                    option = o;

            if (option.empty() && profile)
                for (const auto& [a, o] : profile->dispositions)
                    if (a == axis.name && AutopilotPresets_Disposition(a, o))
                        option = o;

            out.emplace_back(axis.name, option.empty() ? axis.neutral : option);
        }

        const bool changed = out != row.dispositions;
        row.dispositions = std::move(out);
        return changed;
    }

    // ----------------------------------------------------------------------
    // Situation: what is allowed to change right now. World thread.
    // ----------------------------------------------------------------------

    struct Situation
    {
        bool     dead           = false;
        bool     inCombat       = false;
        bool     travelling     = false;
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

        // Dispositions hold everywhere -- combat, dungeons, a human's group --
        // except where the operator asked us to keep hands off entirely.
        bool CanDispose() const
        {
            return !dead && !(withRealPlayer && g_cfg.withRealPlayer == 0);
        }

        // The activity layer stands down wherever something else already
        // owns the bot's movement.
        bool CanSteerActivity() const
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
    //
    // The defaults keep the LLM for bots near people. The scopes and floors
    // let an operator with the hardware or the API budget widen that, up to
    // every enrolled bot planning in the foreground.
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
    // Applying presets. World thread, g_mutex held.
    // ----------------------------------------------------------------------

    // The axis a disposition option belongs to, or null.
    const AutopilotDispositionAxis* AxisOfOption(const std::string& option)
    {
        for (const AutopilotDispositionAxis& axis : AutopilotPresets_Axes())
            for (const AutopilotPreset& p : axis.options)
                if (p.name == option)
                    return &axis;
        return nullptr;
    }

    // Dispositions wanted right now: the bot's own, with the playbook entry
    // for the current situation laid over the axis it belongs to.
    std::vector<std::pair<std::string, std::string>> WantedDispositions(const Row& row, const Situation& sit)
    {
        const std::string key = sit.PlaybookKey();
        std::vector<std::pair<std::string, std::string>> out;
        for (const AutopilotDispositionAxis& axis : AutopilotPresets_Axes())
        {
            std::string option = DispositionOf(row, axis);
            if (!key.empty())
                for (const auto& [situation, o] : row.playbook)
                    if (situation == key && AxisOfOption(o) == &axis)
                        option = o;
            out.emplace_back(axis.name, option);
        }
        return out;
    }

    void ReapplyDispositions(PlayerbotAI* ai, Online& ob)
    {
        for (const auto& [axis, option] : ob.effective)
            if (const AutopilotPreset* p = AutopilotPresets_Disposition(axis, option))
                AutopilotPresets_Apply(ai, *p, ob.locked, ob.baseline);
    }

    void ReapplyActivity(PlayerbotAI* ai, Online& ob)
    {
        if (!ob.appliedActivity.empty())
            if (const AutopilotPreset* p = AutopilotPresets_Activity(ob.appliedActivity))
                AutopilotPresets_Apply(ai, *p, ob.locked, ob.baseline);
    }

    // Move the bot from the dispositions it has (ob.effective) to `wanted`.
    // Each replaced option is reverted to what the bot had before autopilot
    // touched it, then the live activity is re-applied in case the two share a
    // strategy (greedy and gather both want `gather`).
    void ApplyEffective(PlayerbotAI* ai, Online& ob,
                        const std::vector<std::pair<std::string, std::string>>& wanted)
    {
        for (const auto& [axis, option] : wanted)
        {
            const AutopilotPreset* now = AutopilotPresets_Disposition(axis, option);
            if (!now)
                continue;

            for (const auto& [a, o] : ob.effective)
                if (a == axis && o != option)
                    if (const AutopilotPreset* old = AutopilotPresets_Disposition(a, o))
                        AutopilotPresets_Revert(ai, *old, now, ob.locked, ob.baseline);

            AutopilotPresets_Apply(ai, *now, ob.locked, ob.baseline);
        }
        ob.effective  = wanted;
        ob.controlled = true;
        ReapplyActivity(ai, ob);
    }

    // Make `preset` the live activity: put back whatever the previous one
    // changed (unless the new one manages it too), apply the new one, then
    // re-apply dispositions for any strategy the two share. Without the
    // revert, one dungeon activity would leave `lfg` on for good.
    void ApplyActivity(PlayerbotAI* ai, Online& ob, const AutopilotPreset& preset)
    {
        if (!ob.appliedActivity.empty() && ob.appliedActivity != preset.name)
            if (const AutopilotPreset* old = AutopilotPresets_Activity(ob.appliedActivity))
                AutopilotPresets_Revert(ai, *old, &preset, ob.locked, ob.baseline);

        AutopilotPresets_Apply(ai, preset, ob.locked, ob.baseline);
        ob.appliedActivity = preset.name;
        ob.controlled      = true;
        ReapplyDispositions(ai, ob);
    }

    void SteerRpg(PlayerbotAI* ai, const AutopilotPreset& preset, const Situation& sit)
    {
        if (preset.rpg.empty() || sit.inCombat || sit.travelling)
            return;
        if (!ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
            return;

        const int status = AutopilotRpg_CurrentStatus(ai);
        if (status != AutopilotRpg_StatusFromName("idle") &&
            std::find(preset.rpg.begin(), preset.rpg.end(), status) != preset.rpg.end())
            return;

        if (AutopilotRpg_Steer(ai, preset.rpg))
            ++g_statSteer;
    }

    // Restore whatever a playerbots reset wiped, and recognise a human's hand
    // on the controls. The marker is the reset detector: a reset clears it
    // together with everything else, a human toggling `co -flee` does not.
    void Reassert(PlayerbotAI* ai, Row& row, Online& ob, const Situation& sit)
    {
        const bool reset = !HasMarker(ai);
        if (reset)
        {
            AddMarker(ai);
            ob.markerOurs = true;
        }

        if (!g_cfg.control)
            return;

        if (reset)
        {
            // Playerbots put the bot back to its defaults: everything we had
            // applied is gone, and the baseline must be re-learned from them.
            ob.baseline.clear();
            ob.effective.clear();
            ob.appliedActivity.clear();
        }

        if (sit.CanDispose())
        {
            const auto wanted = WantedDispositions(row, sit);

            if (reset)
            {
                ApplyEffective(ai, ob, wanted);
            }
            else if (wanted != ob.effective)
            {
                // New situation (playbook) or new choice: move across.
                ApplyEffective(ai, ob, wanted);
            }
            else
            {
                for (const auto& [axis, option] : wanted)
                {
                    const AutopilotPreset* p = AutopilotPresets_Disposition(axis, option);
                    if (!p)
                        continue;

                    std::vector<std::string> drift = AutopilotPresets_Drift(ai, *p, ob.locked);
                    if (drift.empty())
                        continue;

                    if (sit.withRealPlayer && ob.controlled)
                    {
                        // Someone in the group changed it by hand. Theirs now,
                        // until the group breaks up.
                        for (const std::string& name : drift)
                        {
                            ob.locked.insert(name);
                            ++g_statLocks;
                            RecordEvent(ai->GetBot()->GetGUID().GetRawValue(), "locked",
                                        name + " (changed by a player in the group)");
                        }
                        continue;
                    }
                    AutopilotPresets_Apply(ai, *p, ob.locked, ob.baseline);
                }
            }
        }

        if (sit.CanSteerActivity() && !row.activity.empty())
        {
            if (const AutopilotPreset* preset = AutopilotPresets_Activity(row.activity))
            {
                // Chosen while the bot could not act on it (a group, an
                // instance), or drifted: catch up now.
                if (ob.appliedActivity != preset->name)
                    ApplyActivity(ai, ob, *preset);
                else if (!AutopilotPresets_Drift(ai, *preset, ob.locked).empty())
                    AutopilotPresets_Apply(ai, *preset, ob.locked, ob.baseline);
            }
        }
    }

    uint32_t PickMinutes(const Row& row)
    {
        const PlaystyleProfile* p = Playstyle_Profile(row.playstyle);
        if (!p)
            return 30;
        return urand(p->spanMinMinutes, p->spanMaxMinutes);
    }

    // ----------------------------------------------------------------------
    // Feelings: rewards, boredom, temptations. g_mutex held.
    //
    // Deliberately simple and visible. Each playstyle weighs reward channels
    // differently (an explorer lives for new zones, a crafter for skill-ups).
    // The weighted rewards of the last hour give a satisfaction score.
    // Boredom climbs at the playstyle's rate while the bot stays in one
    // activity, faster the less rewarding it is, and drops when the activity
    // changes or a goal is met. Crossing the threshold ends the activity
    // early and asks for a fresh plan.
    // ----------------------------------------------------------------------

    constexpr uint32_t kHour           = 3600;
    constexpr size_t   kRewardCap      = 256;
    constexpr size_t   kTemptationCap  = 4;

    void NoteReward(Online& ob, const char* channel, uint32_t amount, uint32_t now)
    {
        if (amount == 0)
            return;
        while (!ob.rewards.empty() && now - ob.rewards.front().at > kHour)
            ob.rewards.pop_front();
        PushCapped(ob.rewards, Reward{ now, channel, amount }, kRewardCap);
    }

    float RewardScore(const Row& row, const Online& ob, uint32_t now)
    {
        const PlaystyleProfile* profile = Playstyle_Profile(row.playstyle);
        if (!profile)
            return 0.0f;

        float score = 0.0f;
        for (const Reward& r : ob.rewards)
        {
            if (now - r.at > kHour)
                continue;
            for (const RewardChannel& c : kRewardChannels)
            {
                if (std::string_view(c.name) != r.channel)
                    continue;
                for (const auto& [name, weight] : profile->rewards)
                    if (name == c.name)
                        score += static_cast<float>(r.amount) / c.unit * static_cast<float>(weight);
            }
        }
        return score;
    }

    std::string RewardSummary(const Online& ob, uint32_t now)
    {
        std::string out;
        for (const RewardChannel& c : kRewardChannels)
        {
            uint32_t total = 0;
            for (const Reward& r : ob.rewards)
                if (now - r.at <= kHour && std::string_view(c.name) == r.channel)
                    total += r.amount;
            if (total)
                out += SafeFormat("{}{} {}", out.empty() ? "" : ", ", total, c.name);
        }
        return out;
    }

    void AddTemptation(Online& ob, std::string text, uint32_t now)
    {
        while (!ob.temptations.empty() && now - ob.temptations.front().first > kHour)
            ob.temptations.pop_front();
        PushCapped(ob.temptations, std::make_pair(now, std::move(text)), kTemptationCap);
    }

    std::string MoodText(const Row& row, const Online& ob, uint32_t now)
    {
        const float b = ob.boredom;
        const char* label = b < 25.0f ? "engaged" : b < 50.0f ? "content" : b < g_cfg.boredomThreshold ? "restless" : "bored";
        const float score = RewardScore(row, ob, now);
        const char* feeling = score >= g_cfg.satisfiedScore        ? "it has felt worthwhile"
                            : score >= g_cfg.satisfiedScore / 2.0f ? "it has been slow going"
                                                                   : "it has felt unrewarding";
        const std::string summary = RewardSummary(ob, now);
        const uint32_t minutes = row.activitySince && now > row.activitySince ? (now - row.activitySince) / 60 : 0;
        return SafeFormat("{} (boredom {}/100) after {} minutes of {}; in the last hour {}, and {}.",
                          label, static_cast<uint32_t>(b), minutes, row.activity.empty() ? "nothing" : row.activity,
                          summary.empty() ? "nothing rewarding happened" : "they got " + summary, feeling);
    }

    void UpdateMood(Player* bot, uint64_t guid, Row& row, Online& ob, const Situation& sit, uint32_t now)
    {
        // Gold earned. Income arrives in coppers and silvers between visits a
        // few seconds apart, so the baseline only moves when a whole gold is
        // booked (by exactly that much) or when the bot spends.
        const uint32_t money = bot->GetMoney();
        if (ob.lastMoney == 0 || money < ob.lastMoney)
            ob.lastMoney = money;
        else if (money >= ob.lastMoney + 10000)
        {
            const uint32_t gold = (money - ob.lastMoney) / 10000;
            NoteReward(ob, "gold", gold, now);
            ob.lastMoney += gold * 10000;
        }

        // Profession skill-ups, and a nudge when a skill hits its cap.
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

        if (ob.moodAt == 0)
        {
            ob.moodAt = now;
            return;
        }

        const float minutes = std::min(30.0f, static_cast<float>(now - ob.moodAt) / 60.0f);
        ob.moodAt = now;

        if (!sit.CanSteerActivity())
        {
            // In a group, a dungeon or a battleground something else is
            // driving the evening; the bot is not bored of its own choice.
            ob.boredom = std::max(0.0f, ob.boredom - minutes);
            return;
        }

        const PlaystyleProfile* profile = Playstyle_Profile(row.playstyle);
        const float rate = profile ? profile->boredomRate : 1.5f;
        const float satisfaction = std::min(1.0f, RewardScore(row, ob, now) / std::max(0.1f, g_cfg.satisfiedScore));
        ob.boredom = std::clamp(ob.boredom + minutes * rate * (1.2f - satisfaction), 0.0f, 100.0f);

        if (ob.boredom >= g_cfg.boredomThreshold && !ob.boredNoted && !row.activity.empty())
        {
            ob.boredNoted    = true;
            ob.urgentPlan    = true;
            ob.activityUntil = now;
            RecordEvent(guid, "bored", SafeFormat("of {} after {} minutes", row.activity,
                                                  row.activitySince ? (now - row.activitySince) / 60 : 0));
        }
    }

    // ----------------------------------------------------------------------
    // Goals. g_mutex held.
    // ----------------------------------------------------------------------

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
            row.goal         = AutopilotGoal();
            row.dirty        = true;
            ob.boredom       = std::max(0.0f, ob.boredom - 30.0f);
            ob.urgentPlan    = true;
            ob.activityUntil = now;
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

    // A goal for a bot the model has not given one, from its playstyle. Keeps
    // dormant bots pointed somewhere. Phrased for the bot's awareness.
    void PolicyGoal(Player* bot, uint64_t guid, Row& row, Online& ob)
    {
        if (!g_cfg.policyGoals || row.goal.Active())
            return;

        const bool immersed = row.awareness == "immersed";
        const uint32_t level = bot->GetLevel();
        const std::string& style = row.playstyle;

        std::string kind, target, text;
        auto levelGoal = [&](uint32_t to)
        {
            kind = "reach_level"; target = std::to_string(to);
            text = immersed ? "Grow stronger and more capable." : SafeFormat("Hit level {}.", to);
        };

        if (style == "quester" || style == "casual")
        {
            kind = "complete_quests"; target = style == "quester" ? "5" : "3";
            text = immersed ? "Help the people who need it." : SafeFormat("Finish {} quests.", target);
        }
        else if (style == "crafter")
        {
            uint32_t best = 0;
            for (uint32_t skill : { uint32_t(SKILL_HERBALISM), uint32_t(SKILL_MINING), uint32_t(SKILL_SKINNING) })
                if (bot->HasSkill(skill) && (!best || bot->GetSkillValue(skill) < bot->GetSkillValue(best)))
                    best = skill;
            if (best && bot->GetSkillValue(best) < bot->GetPureMaxSkillValue(best))
            {
                const uint32_t to = std::min<uint32_t>(bot->GetSkillValue(best) + 25, bot->GetPureMaxSkillValue(best));
                kind = "reach_skill"; target = SafeFormat("{} {}", Progress_SkillName(best), to);
                text = immersed ? SafeFormat("Get better at {}.", Progress_SkillName(best))
                                : SafeFormat("Raise {} to {}.", Progress_SkillName(best), to);
            }
            else
            {
                kind = "earn_gold"; target = "20";
                text = immersed ? "Put some coin aside." : "Save up 20 more gold.";
            }
        }
        else if (style == "dungeonrunner")
        {
            if (level >= 15)
            {
                kind = "run_dungeon"; target = "1";
                text = immersed ? "Find companions brave enough for the depths." : "Run a dungeon.";
            }
            else
                levelGoal(level + 1);
        }
        else if (style == "explorer")
        {
            kind = "free";
            text = immersed ? "See lands I have never walked." : "See somewhere new.";
        }
        else if (style == "roleplayer")
        {
            kind = "free";
            text = "Live up to who I am and where I come from.";
        }
        else if (style == "pvper")
        {
            kind = "free";
            text = immersed ? "Prove myself against the enemy." : "Get some PvP kills.";
        }
        else
        {
            levelGoal(level + 1);
        }

        AutopilotGoal goal;
        if (Goal_Resolve(bot, kind, target, text, Counters(row), goal).empty())
            SetGoal(guid, row, &ob, std::move(goal), "policy");
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
                ob.urgentPlan    = true;
                ob.activityUntil = now;
            };

            if (instance == 0 && ob.prevInstance != 0)
                boundary(ob.prevInstance == 2 ? "the battleground is over" : "left the dungeon");
            if (!grouped && ob.prevGrouped)
                boundary("left the group");
            if (!sit.travelling && ob.prevTravelling && row.activity == "travel")
                ob.activityUntil = now;   // landed: the trip was the whole activity
        }

        ob.situationKnown = true;
        ob.prevInstance   = instance;
        ob.prevGrouped    = grouped;
        ob.prevTravelling = sit.travelling;
    }

    void SetActivity(Player* bot, PlayerbotAI* ai, uint64_t guid, Row& row, Online& ob,
                     const Situation& sit, const AutopilotPreset& preset, const std::string& by,
                     const std::string& reason, uint32_t minutes, uint32_t now)
    {
        const bool changed = row.activity != preset.name;
        if (changed)
        {
            // Something new takes the edge off.
            ob.boredom    *= 0.4f;
            ob.boredNoted  = false;
        }
        row.activity   = preset.name;
        row.decidedBy  = by;
        row.lastReason = reason;
        if (changed || row.activitySince == 0)
            row.activitySince = now;
        row.dirty = true;

        ob.activityUntil = now + std::max<uint32_t>(1, minutes) * 60;

        PushCapped(ob.decisions,
                   SafeFormat("{} ({}, {}m): {}", preset.name, by, minutes, reason.empty() ? "-" : reason),
                   kDecisionRing);
        RecordEvent(guid, "activity", SafeFormat("{} for {}m ({}): {}", preset.name, minutes, by, reason));

        if (g_cfg.debug)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: {} -> {} for {}m ({}): {}",
                     bot->GetName(), preset.name, minutes, by, reason);

        if (g_cfg.control && sit.CanSteerActivity())
        {
            ApplyActivity(ai, ob, preset);
            SteerRpg(ai, preset, sit);
        }
    }

    // Deterministic self-preservation, checked before anyone is asked.
    // Returns the activity to switch to, or null.
    const AutopilotPreset* CheckGuards(Player* bot, const Row& row, Online& ob, uint32_t now,
                                       std::string& reason)
    {
        const uint32_t window = g_cfg.guardDeathWindowMinutes * 60;
        while (!ob.deathTimes.empty() && now - ob.deathTimes.front() > window)
            ob.deathTimes.pop_front();

        const AutopilotPreset* rest = AutopilotPresets_Activity("rest");
        const AutopilotPreset* town = AutopilotPresets_Activity("town");

        // A guard that fired keeps quiet for a while. Some conditions cannot
        // be fixed by the activity it picks (a broke bot cannot repair, bags
        // can be full of unsellable items), and re-firing after every hold
        // would churn activities and fill the diary forever.
        auto ready = [&](const char* kind)
        {
            auto it = ob.guardCooldown.find(kind);
            return it == ob.guardCooldown.end() || now >= it->second;
        };
        auto fire = [&](const char* kind, const AutopilotPreset* preset)
        {
            ob.guardCooldown[kind] = now + g_cfg.guardHoldMinutes * 60 * 4;
            return preset;
        };

        if (g_cfg.guardDeaths > 0 && ob.deathTimes.size() >= g_cfg.guardDeaths && rest &&
            row.activity != "rest" && ready("deaths"))
        {
            reason = SafeFormat("died {} times in {} minutes in {}", ob.deathTimes.size(),
                                g_cfg.guardDeathWindowMinutes, Progress_ZoneName(bot->GetZoneId()));
            ob.deathTimes.clear();
            return fire("deaths", rest);
        }

        if (town && row.activity != "town")
        {
            const uint8_t durability = Progress_DurabilityPct(bot);
            if (g_cfg.guardDurabilityPct > 0 && durability < g_cfg.guardDurabilityPct && ready("durability"))
            {
                reason = SafeFormat("gear is badly damaged ({}% durability)", uint32_t(durability));
                return fire("durability", town);
            }
            if (g_cfg.guardFreeBagSlots > 0 && bot->GetFreeInventorySpace() < g_cfg.guardFreeBagSlots &&
                ready("bags"))
            {
                reason = "bags are full";
                return fire("bags", town);
            }
        }
        return nullptr;
    }

    // The deterministic policy: playstyle weights, a damper on whatever the
    // bot is already doing (people get bored), and availability.
    const AutopilotPreset* PolicyPick(Player* bot, const Row& row)
    {
        const PlaystyleProfile* profile = Playstyle_Profile(row.playstyle);

        std::vector<std::pair<const AutopilotPreset*, uint32_t>> candidates;
        uint32_t total = 0;

        auto consider = [&](const std::string& name, uint32_t weight)
        {
            const AutopilotPreset* preset = AutopilotPresets_Activity(name);
            if (!preset || weight == 0 || !AutopilotPresets_Unavailable(*preset, bot).empty())
                return;
            if (preset->name == row.activity)
                weight = std::max<uint32_t>(1, weight / 4);
            candidates.emplace_back(preset, weight);
            total += weight;
        };

        // A goal pulls toward the activities that serve it, on top of taste.
        const auto bias = Goal_ActivityBias(bot, row.goal);
        auto boost = [&](const std::string& name, uint32_t weight)
        {
            for (const auto& [activity, factor] : bias)
                if (activity == name)
                    return weight * factor;
            return weight;
        };

        if (profile)
            for (const auto& [name, weight] : profile->activityWeights)
                consider(name, boost(name, weight * 4));

        // Activities the goal needs but the playstyle never lists still get a
        // look in: an explorer with a dungeon goal has to queue sometime.
        for (const auto& [name, factor] : bias)
            if (std::none_of(candidates.begin(), candidates.end(),
                             [&](const auto& c) { return c.first->name == name; }))
                consider(name, 2 * factor);

        if (candidates.empty())
            for (const AutopilotPreset& p : AutopilotPresets_Activities())
                consider(p.name, 4);

        if (total == 0)
            return nullptr;

        uint32_t roll = urand(1, total);
        for (const auto& [preset, weight] : candidates)
        {
            if (roll <= weight)
                return preset;
            roll -= weight;
        }
        return candidates.back().first;
    }

    AutopilotPromptContext BuildPromptContext(Player* bot, PlayerbotAI* ai, const Row& row,
                                              const Online& ob, Tier tier, uint32_t now)
    {
        AutopilotPromptContext ctx;
        ctx.botName = bot->GetName();
        ctx.level   = bot->GetLevel();
        ctx.race    = ai->GetChatHelper()->FormatRace(bot->getRace());
        ctx.cls     = ai->GetChatHelper()->FormatClass(bot->getClass());

        ctx.playstyle = row.playstyle;
        if (const PlaystyleProfile* p = Playstyle_Profile(row.playstyle))
            ctx.playstyleDescription = p->description;
        ctx.awareness            = row.awareness;
        ctx.awarenessDescription = Awareness_Description(row.awareness);

        if (g_EnableRPPersonalities)
        {
            const std::string key = GetBotPersonality(bot);
            if (!key.empty() && key != "default")
                ctx.personality = key + " - " + GetPersonalityPromptAddition(key);
        }

        ctx.activity        = row.activity;
        ctx.activityMinutes = row.activitySince && now > row.activitySince ? (now - row.activitySince) / 60 : 0;
        ctx.goal            = Goal_Describe(bot, row.goal, Counters(row));
        if (row.goal.Active() && ob.goalProgressAt && now - ob.goalProgressAt > 1800)
            ctx.goal += SafeFormat(" (no progress for {})", Span(now - ob.goalProgressAt));
        ctx.mood            = MoodText(row, ob, now);

        for (const auto& [situation, option] : row.playbook)
            ctx.playbook += (ctx.playbook.empty() ? "" : ", ") + situation + ": " + option;

        for (const auto& [at, text] : ob.temptations)
            if (now - at <= kHour)
                ctx.temptations.push_back(text);

        std::string unavailable;
        for (const AutopilotPreset& p : AutopilotPresets_Activities())
        {
            std::string why = AutopilotPresets_Unavailable(p, bot);
            if (!why.empty())
                unavailable += (unavailable.empty() ? "" : ", ") + p.name + " (" + why + ")";
        }
        ctx.unavailable = unavailable;

        ctx.decisions.assign(ob.decisions.begin(), ob.decisions.end());
        for (const ProgressEvent& e : ob.events)
            ctx.events.push_back(SafeFormat("[{}] {}: {}", Ago(e.at, now), e.type, e.detail));

        std::vector<ProgressSnapshot> window(ob.snapshots.begin(), ob.snapshots.end());
        window.push_back(Progress_Capture(bot));
        window.back().killsTotal  = row.killsTotal;
        window.back().deathsTotal = row.deathsTotal;
        window.back().questsTotal = row.questsTotal;
        ctx.progress = Progress_Summarize(window);

        if (!ob.lastGuard.empty() && now - ob.lastGuardAt < 3600)
            ctx.guards = SafeFormat("Recently ({}): had to stop because {}.", Ago(ob.lastGuardAt, now), ob.lastGuard);

        // The full surroundings scan is for bots someone can see. A strategic
        // choice for an unwatched bot needs only the macro state.
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

    // Ask the LLM, within the budget. Returns true when a plan was submitted.
    bool TrySubmitPlan(Player* bot, PlayerbotAI* ai, uint64_t guid, Row& row, Online& ob,
                       Tier tier, uint32_t now, bool force)
    {
        if (!g_cfg.llmEnable || ob.planPending)
            return false;
        if (tier == Tier::Dormant && !force)
            return false;

        const uint32_t cadence = (tier == Tier::Foreground ? g_cfg.decisionIntervalMinutes
                                                           : g_cfg.goalRefreshMinutes) * 60;
        const bool due = force || ob.urgentPlan || ob.lastPlanAt == 0 || now - ob.lastPlanAt >= cadence;
        if (!due || !AutopilotPlanner_CanSubmit(force || tier == Tier::Foreground))
            return false;

        std::string prompt = AutopilotPlanner_BuildPrompt(BuildPromptContext(bot, ai, row, ob, tier, now),
                                                          g_cfg.promptTemplate);
        if (!AutopilotPlanner_Submit(guid, std::move(prompt)))
            return false;

        ob.planPending     = true;
        ob.planSubmittedAt = now;
        ob.urgentPlan      = false;

        // Keep doing what we were doing while the model thinks.
        ob.activityUntil = std::max(ob.activityUntil, now + kPlanGraceSeconds);

        if (g_cfg.debug)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: asked the model about {} ({}).",
                     bot->GetName(), TierName(tier));
        return true;
    }

    void Decide(Player* bot, PlayerbotAI* ai, uint64_t guid, Row& row, Online& ob,
                const Situation& sit, uint32_t now, bool forceLlm)
    {
        ob.tier = ComputeTier(bot);

        if (TrySubmitPlan(bot, ai, guid, row, ob, ob.tier, now, forceLlm))
            return;
        if (ob.planPending)
            return;   // an answer is on its way; the grace period covers it

        // No model this time. Give the bot a direction if it has none, so
        // the policy's pick serves something.
        PolicyGoal(bot, guid, row, ob);

        const AutopilotPreset* pick = PolicyPick(bot, row);
        if (!pick)
            return;

        ++g_statPolicy;
        SetActivity(bot, ai, guid, row, ob, sit, *pick, "policy",
                    Playstyle_Profile(row.playstyle) ? "fits their playstyle" : "something to do",
                    PickMinutes(row), now);
    }

    void ApplyDecision(const AutopilotDecision& d, uint32_t now)
    {
        auto rowIt = g_rows.find(d.botGuid);
        auto onIt  = g_online.find(d.botGuid);
        if (onIt != g_online.end())
        {
            onIt->second.planPending = false;
            onIt->second.lastPlanAt  = now;
        }
        if (rowIt == g_rows.end() || !rowIt->second.enrolled)
            return;

        Row& row = rowIt->second;
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(d.botGuid));
        PlayerbotAI* ai = BotAI(bot);

        auto reject = [&](const std::string& why)
        {
            ++g_statInvalid;
            if (g_cfg.debug)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: plan for {} rejected: {}",
                         bot ? bot->GetName() : std::to_string(d.botGuid), why);
            // Let the policy decide on the next visit instead.
            if (onIt != g_online.end())
                onIt->second.activityUntil = now;
        };

        if (!d.ok)
            return reject(d.error);

        const AutopilotPreset* preset = AutopilotPresets_Activity(d.activity);
        if (!preset)
            return reject("unknown activity '" + d.activity + "'");
        if (bot)
        {
            std::string why = AutopilotPresets_Unavailable(*preset, bot);
            if (!why.empty())
                return reject(d.activity + " is not available: " + why);
        }

        ++g_statLlm;

        // Dispositions: take each axis the model named validly; keep the rest.
        const auto previous = row.dispositions;
        for (const auto& [axis, option] : d.disposition)
        {
            if (!AutopilotPresets_Disposition(axis, option))
                continue;
            for (auto& [a, o] : row.dispositions)
                if (a == axis)
                    o = option;
        }
        NormalizeDispositions(row);
        if (row.dispositions != previous)
            RecordEvent(d.botGuid, "disposition", SerializeDispositions(row.dispositions));

        // Playbook: replace it when the model sent one; every entry must name
        // a known situation and a real disposition option.
        if (!d.playbook.empty())
        {
            std::vector<std::pair<std::string, std::string>> playbook;
            for (const auto& [situation, option] : d.playbook)
                if ((situation == "dungeon" || situation == "battleground" || situation == "with_player") &&
                    AxisOfOption(option))
                    playbook.emplace_back(situation, option);
            if (playbook != row.playbook)
            {
                row.playbook = std::move(playbook);
                std::string text;
                for (const auto& [s, o] : row.playbook)
                    text += (text.empty() ? "" : ", ") + s + ": " + o;
                RecordEvent(d.botGuid, "playbook", text.empty() ? "cleared" : text);
            }
        }

        // Goal: resolved against the live bot so progress can be measured. A
        // goal the game cannot resolve does not sink the rest of the plan.
        if (bot && !d.goalKind.empty())
        {
            AutopilotGoal goal;
            const std::string why = Goal_Resolve(bot, d.goalKind, d.goalTarget, d.goalText, Counters(row), goal);
            if (!why.empty())
            {
                if (g_cfg.debug)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: goal for {} not taken: {}",
                             bot->GetName(), why);
            }
            else
            {
                // The prompt shows the current goal, so models echo it back.
                // Same kind and same target is the same aim: keep its baseline
                // and age. Otherwise "earn 50 gold" (target = money now + 50)
                // would move its goalposts on every plan and never complete.
                const bool same = goal.kind == row.goal.kind && Lower(goal.target) == Lower(row.goal.target);
                if (same)
                {
                    if (!goal.text.empty())
                        row.goal.text = goal.text;
                }
                else
                {
                    SetGoal(d.botGuid, row, onIt != g_online.end() ? &onIt->second : nullptr, std::move(goal), "llm");
                }
            }
        }

        uint32_t minutes = d.minutes;
        if (const PlaystyleProfile* p = Playstyle_Profile(row.playstyle))
            minutes = minutes ? std::clamp<uint32_t>(minutes, std::max<uint32_t>(1, p->spanMinMinutes / 2),
                                                     p->spanMaxMinutes * 2)
                              : urand(p->spanMinMinutes, p->spanMaxMinutes);
        if (minutes == 0)
            minutes = 30;
        row.dirty = true;

        if (!bot || !ai || onIt == g_online.end())
        {
            // Logged out while the model was thinking: keep the choice for later.
            row.activity   = preset->name;
            row.decidedBy  = "llm";
            row.lastReason = d.reason;
            return;
        }

        Online& ob = onIt->second;
        const Situation sit = Classify(bot, ai);
        if (g_cfg.control && sit.CanDispose())
        {
            const auto wanted = WantedDispositions(row, sit);
            if (wanted != ob.effective)
                ApplyEffective(ai, ob, wanted);
        }

        SetActivity(bot, ai, d.botGuid, row, ob, sit, *preset, "llm", d.reason, minutes, now);
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
                if (row.playstyle.empty())
                    row.playstyle = Playstyle_Assign(guid, bot->GetGuildId());
                if (row.awareness.empty())
                    row.awareness = Awareness_Assign(guid);
                if (firstEver)
                    row.enrolledAt = now;
                CountEnrolled(row, +1);

                RecordEvent(guid, "enrolled", SafeFormat("{} ({} / {})", source, row.playstyle, row.awareness));
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
            NormalizeDispositions(row);
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
                ob.nextSnapshotAt = now + static_cast<uint32_t>(OllamaStableHash(guid) % interval);
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
        if (ob.controlled || !ob.appliedActivity.empty())
        {
            HandBack(ai);
            ob.controlled      = false;
            ob.appliedActivity.clear();
            ob.baseline.clear();
            ob.effective.clear();
        }
        ob.planPending   = false;
        ob.activityUntil = 0;

        if (HasMarker(ai))
            ai->ChangeStrategy(std::string("-") + AUTOPILOT_STRATEGY_NAME, BOT_STATE_NON_COMBAT);
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
        ob.evaluated = true;
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

        // Joined someone's group with an activity still live: `+new rpg` and
        // `-grind` would fight `follow`. Random bots are usually reset by
        // playerbots on regroup anyway; alts are not. Hand the bot back to its
        // defaults -- Reassert below then restores the marker and the
        // dispositions, which do hold in a group.
        if ((sit.withRealPlayer || sit.follower) && !ob.appliedActivity.empty())
        {
            HandBack(ai);
            ob.appliedActivity.clear();
            ob.baseline.clear();
            ob.controlled      = false;
            ob.effective.clear();
            RecordEvent(guid, "grouped", "activity paused while following the group");
        }

        Reassert(ai, row, ob, sit);

        if (!ob.historyRequested)
            RequestHistory(guid, ob);

        CheckBoundaries(bot, guid, row, ob, sit, now);
        UpdateMood(bot, guid, row, ob, sit, now);
        CheckGoal(bot, guid, row, ob, now);

        // A plan that never came back (provider down, server restarted the
        // dispatcher) must not freeze the bot forever.
        if (ob.planPending && now - ob.planSubmittedAt > g_cfg.planTimeoutSeconds)
        {
            ob.planPending   = false;
            ob.activityUntil = now;
        }

        if (!g_cfg.control || !sit.CanSteerActivity())
            return;

        std::string guardReason;
        if (const AutopilotPreset* guard = CheckGuards(bot, row, ob, now, guardReason))
        {
            ++g_statGuard;
            // No urgent plan: the reason reaches the model through the prompt
            // at its normal cadence, so a guard never spends budget by itself.
            ob.lastGuard   = guardReason;
            ob.lastGuardAt = now;
            RecordEvent(guid, "guard", guardReason);
            SetActivity(bot, ai, guid, row, ob, sit, *guard, "guard", guardReason, g_cfg.guardHoldMinutes, now);
            return;
        }

        const AutopilotPreset* current = row.activity.empty() ? nullptr : AutopilotPresets_Activity(row.activity);
        const bool due = !current || now >= ob.activityUntil ||
                         !AutopilotPresets_Unavailable(*current, bot).empty();
        if (due)
        {
            Decide(bot, ai, guid, row, ob, sit, now, false);
            return;
        }

        SteerRpg(ai, *current, sit);
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
                if (ob.controlled || !ob.appliedActivity.empty())
                    HandBack(ai);
                if (HasMarker(ai))
                    ai->ChangeStrategy(std::string("-") + AUTOPILOT_STRATEGY_NAME, BOT_STATE_NON_COMBAT);
            }
            ob.controlled = false;
            ob.appliedActivity.clear();
            ob.baseline.clear();
            ob.effective.clear();
            ob.markerOurs    = false;
            ob.evaluated     = false;
            ob.planPending   = false;
            ob.activityUntil = 0;
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
                "(bot_guid, mode, enrolled, source, playstyle, awareness, kills_total, "
                "deaths_total, quests_total, dungeons_total, activity, activity_since, decided_by, "
                "dispositions, playbook, goal_kind, goal_target, goal_target_id, goal_value, "
                "goal_baseline, goal_set_at, goal_text, last_reason, enrolled_at, updated_at) VALUES " + values +
                " ON DUPLICATE KEY UPDATE mode = VALUES(mode), enrolled = VALUES(enrolled), "
                "source = VALUES(source), playstyle = VALUES(playstyle), "
                "awareness = VALUES(awareness), kills_total = VALUES(kills_total), "
                "deaths_total = VALUES(deaths_total), quests_total = VALUES(quests_total), "
                "dungeons_total = VALUES(dungeons_total), "
                "activity = VALUES(activity), activity_since = VALUES(activity_since), "
                "decided_by = VALUES(decided_by), dispositions = VALUES(dispositions), "
                "playbook = VALUES(playbook), goal_kind = VALUES(goal_kind), "
                "goal_target = VALUES(goal_target), goal_target_id = VALUES(goal_target_id), "
                "goal_value = VALUES(goal_value), goal_baseline = VALUES(goal_baseline), "
                "goal_set_at = VALUES(goal_set_at), "
                "goal_text = VALUES(goal_text), last_reason = VALUES(last_reason), "
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
                "({}, {}, {}, '{}', '{}', '{}', {}, {}, {}, {}, '{}', {}, '{}', '{}', '{}', "
                "'{}', '{}', {}, {}, {}, {}, '{}', '{}', {}, {})",
                guid, uint32_t(row.mode), row.enrolled ? 1 : 0, esc(row.source, 32),
                esc(row.playstyle, 32), esc(row.awareness, 32), row.killsTotal, row.deathsTotal,
                row.questsTotal, row.dungeonsTotal, esc(row.activity, 32), row.activitySince,
                esc(row.decidedBy, 16), esc(SerializeDispositions(row.dispositions), 255),
                esc(SerializeDispositions(row.playbook), 255),
                g.Active() ? Goal_KindName(g.kind) : "", esc(g.target, 64), g.targetId, g.value,
                g.baseline, g.setAt, esc(g.text, 255), esc(row.lastReason, 255), row.enrolledAt, now);
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

            uint32_t onlineEnrolled = 0;
            std::unordered_map<std::string, uint32_t> byActivity;
            uint32_t tiers[3] = { 0, 0, 0 };
            for (uint64_t guid : g_roster)
            {
                auto it = g_rows.find(guid);
                if (it == g_rows.end() || !it->second.enrolled)
                    continue;
                ++onlineEnrolled;
                ++byActivity[it->second.activity.empty() ? "-" : it->second.activity];
                auto on = g_online.find(guid);
                if (on != g_online.end())
                    ++tiers[static_cast<int>(on->second.tier)];
            }

            handler->SendSysMessage(SafeFormat("OllamaChat Autopilot: {}",
                                               Autopilot_IsActive() ? "active" : "INACTIVE"));
            if (!Autopilot_IsActive())
                SendInactiveReasons(handler);
            handler->SendSysMessage(SafeFormat(
                "  enrolled: {} total, {} online | bots tracked online: {} | rule-enrolled {} of cap {}",
                g_enrolledCount, onlineEnrolled, g_roster.size(), g_cappedCount,
                g_cfg.maxEnrolled ? std::to_string(g_cfg.maxEnrolled) : std::string("none")));

            std::string activities;
            for (const auto& [activity, n] : byActivity)
                activities += SafeFormat("{}{} {}", activities.empty() ? "" : ", ", n, activity);
            handler->SendSysMessage("  activities: " + (activities.empty() ? std::string("-") : activities));
            handler->SendSysMessage(SafeFormat(
                "  tiers (as last computed): {} foreground, {} background, {} dormant",
                tiers[2], tiers[1], tiers[0]));

            const AutopilotPlannerStats ps = AutopilotPlanner_GetStats();
            handler->SendSysMessage(SafeFormat(
                "  decisions: {} llm, {} policy, {} guard, {} rejected | rpg steers {} | human locks {}",
                g_statLlm, g_statPolicy, g_statGuard, g_statInvalid, g_statSteer, g_statLocks));
            handler->SendSysMessage(SafeFormat(
                "  llm: {} | budget {:.1f}/{:.0f} tokens ({}/h) | in flight {} | submitted {}, refused {}, "
                "parsed {}, failed {}{}",
                g_cfg.llmEnable ? "on" : "off", ps.tokens, ps.capacity, g_cfg.llmCallsPerHour, ps.inFlight,
                ps.submitted, ps.refused, ps.parsed, ps.failed,
                ps.lastError.empty() ? "" : " | last error: " + ps.lastError));
            handler->SendSysMessage(SafeFormat(
                "  sweep: {} bots every {} ms (full cycle ~{}s) | snapshots every {} min",
                g_cfg.botsPerSweep, g_cfg.sweepIntervalMs,
                (g_roster.size() / std::max<uint32_t>(1, g_cfg.botsPerSweep) + 1) * g_cfg.sweepIntervalMs / 1000,
                g_cfg.snapshotIntervalMinutes));
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
            auto it = g_rows.find(guid);
            if (it != g_rows.end())
            {
                row    = it->second;
                hasRow = true;
            }
            auto on = g_online.find(guid);
            if (on != g_online.end())
                ob = on->second;
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
            "  playstyle: {} | awareness: {} | dispositions: {} | enrolled since: {}",
            row.playstyle, row.awareness, SerializeDispositions(row.dispositions),
            row.enrolledAt ? Ago(row.enrolledAt, now) : "-"));
        handler->SendSysMessage(SafeFormat(
            "  activity: {} (since {}, by {}, reconsider in {}) | rpg status: {} | tier: {}{}",
            row.activity.empty() ? "-" : row.activity,
            row.activitySince ? Ago(row.activitySince, now) : "-",
            row.decidedBy.empty() ? "-" : row.decidedBy,
            ob.activityUntil > now ? Span(ob.activityUntil - now) : "now",
            ai ? AutopilotRpg_StatusName(AutopilotRpg_CurrentStatus(ai)) : "-", TierName(ob.tier),
            ob.planPending ? " | waiting on the model" : ""));
        handler->SendSysMessage("  goal: " + (row.goal.Active() ? Goal_Describe(bot, row.goal, Counters(row))
                                                                : std::string("-")));
        handler->SendSysMessage("  mood: " + MoodText(row, ob, now));
        if (!row.playbook.empty())
            handler->SendSysMessage("  playbook: " + SerializeDispositions(row.playbook));
        for (const auto& [at, text] : ob.temptations)
            if (now - at <= kHour)
                handler->SendSysMessage(SafeFormat("  tempted ({}): {}", Ago(at, now), text));
        handler->SendSysMessage("  last reason: " + (row.lastReason.empty() ? std::string("-") : row.lastReason));
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
        if (snaps.empty())
        {
            handler->SendSysMessage("  no snapshots in the last 24h yet.");
            return true;
        }

        const ProgressSnapshot& last = snaps.back();
        handler->SendSysMessage(SafeFormat(
            "  latest snapshot ({}): level {}, {}, {} | durability {}% | {} free bag slots | {} quests in log",
            Ago(last.takenAt, now), uint32_t(last.level), Progress_FormatMoney(last.money),
            Progress_ZoneName(last.zoneId), uint32_t(last.durabilityPct), uint32_t(last.freeBagSlots),
            uint32_t(last.questsActive)));
        if (!last.professions.empty())
            handler->SendSysMessage("  professions: " + Progress_DescribeProfessions(last.professions));
        std::string summary = Progress_Summarize(snaps);
        if (!summary.empty())
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

        const uint32_t interval = std::max<uint32_t>(1, g_cfg.snapshotIntervalMinutes);
        handler->SendSysMessage(SafeFormat(
            "  diary load: ~{} snapshot rows/hour; history settles at <= {} snapshots and {} events per bot.",
            wouldEnroll * 60 / interval, g_cfg.snapshotRetention, g_cfg.eventRetention));

        // What the matched bots would ask for at the configured cadences,
        // against what the budget lets through.
        handler->SendSysMessage(SafeFormat(
            "  tiers right now: {} foreground, {} background, {} dormant (dormant bots never call the LLM).",
            tiers[2], tiers[1], tiers[0]));
        if (g_cfg.llmEnable)
        {
            const uint32_t wanted =
                tiers[2] * 60 / std::max<uint32_t>(1, g_cfg.decisionIntervalMinutes) +
                tiers[1] * 60 / std::max<uint32_t>(1, g_cfg.goalRefreshMinutes);
            handler->SendSysMessage(SafeFormat(
                "  LLM: these bots would ask ~{} plans/hour; the budget allows {}/hour{}.",
                wanted, g_cfg.llmCallsPerHour,
                wanted > g_cfg.llmCallsPerHour ? " - the rest fall back to the playstyle policy" : ""));
        }
        else
        {
            handler->SendSysMessage("  LLM planning is off (Autopilot.Llm.Enable = 0): policy only, no LLM calls.");
        }

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
        RecordEvent(guid, "playstyle", it->second.playstyle + " (set by GM)");

        // Its next decision should reflect the new playstyle.
        auto on = g_online.find(guid);
        if (on != g_online.end())
        {
            on->second.urgentPlan    = true;
            on->second.activityUntil = 0;
        }
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
        RecordEvent(guid, "awareness", it->second.awareness + " (set by GM)");
        handler->SendSysMessage(SafeFormat("OllamaChat: {} awareness set to {}.", bot->GetName(), it->second.awareness));
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
        const Situation sit = Classify(bot, ai);
        const uint32_t now = Progress_Now();
        if (!sit.CanSteerActivity())
        {
            ob->urgentPlan = true;
            handler->SendSysMessage(SafeFormat(
                "OllamaChat: {} is in a group, instance or dead; it will replan when that ends.", bot->GetName()));
            return true;
        }

        ob->planPending = false;
        Decide(bot, ai, bot->GetGUID().GetRawValue(), *row, *ob, sit, now, /*forceLlm*/ true);
        handler->SendSysMessage(ob->planPending
            ? SafeFormat("OllamaChat: asked the model what {} should do next.", bot->GetName())
            : SafeFormat("OllamaChat: {} -> {} (policy; the model was unavailable or over budget).",
                         bot->GetName(), row->activity));
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
        handler->SendSysMessage(SafeFormat("OllamaChat: {} goal: {}", bot->GetName(),
                                           Goal_Describe(bot, row->goal, Counters(*row))));
        return true;
    }

    bool HandleActivity(ChatHandler* handler, std::string name, Optional<std::string> activity,
                        Optional<uint32> minutes)
    {
        Player* bot = FindBot(handler, name);
        if (!bot)
            return true;

        std::string all;
        for (const AutopilotPreset& p : AutopilotPresets_Activities())
            all += (all.empty() ? "" : ", ") + p.name;

        std::lock_guard<std::mutex> lock(g_mutex);
        Row*    row = nullptr;
        Online* ob  = nullptr;
        if (!ControlTarget(handler, bot, row, ob))
            return true;

        if (!activity)
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: {} activity: {} (available: {})", bot->GetName(),
                                               row->activity.empty() ? "-" : row->activity, all));
            return true;
        }

        const AutopilotPreset* preset = AutopilotPresets_Activity(*activity);
        if (!preset)
        {
            handler->SendSysMessage(SafeFormat("OllamaChat: unknown activity '{}'. Available: {}", *activity, all));
            return true;
        }

        PlayerbotAI* ai = BotAI(bot);
        const Situation sit = Classify(bot, ai);
        const uint32_t mins = std::clamp<uint32_t>(minutes.value_or(PickMinutes(*row)), 1, 600);
        SetActivity(bot, ai, bot->GetGUID().GetRawValue(), *row, *ob, sit, *preset, "gm", "set by a GM", mins,
                    Progress_Now());
        handler->SendSysMessage(SafeFormat("OllamaChat: {} -> {} for {} minutes{}.", bot->GetName(), preset->name, mins,
                                           sit.CanSteerActivity() ? "" : " (applies when its group/instance ends)"));
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

    c.boredomThreshold        = std::clamp(sConfigMgr->GetOption<float>("OllamaChat.Autopilot.BoredomThreshold", 70.0f), 1.0f, 100.0f);
    c.satisfiedScore          = std::max(0.1f, sConfigMgr->GetOption<float>("OllamaChat.Autopilot.SatisfiedScore", 10.0f));
    c.goalStaleMinutes        = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.GoalStaleMinutes", 120);
    c.policyGoals             = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.PolicyGoals", true);

    if (c.minLevel > c.maxLevel)
        std::swap(c.minLevel, c.maxLevel);

    Playstyle_LoadConfig();
    AutopilotPresets_Load();
    AutopilotPlanner_ConfigureBudget(c.llmCallsPerHour, c.maxConcurrentPlans);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_cfg = std::move(c);

    // Rules and presets may have changed: re-evaluate every online bot on its
    // next visit, and re-check every row's dispositions against the new axes.
    for (auto& [guid, ob] : g_online)
        ob.evaluated = false;
    for (auto& [guid, row] : g_rows)
        if (row.enrolled && NormalizeDispositions(row))
            row.dirty = true;

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
        // Columns were added while the table was still unreleased; a database
        // set up from an earlier draft needs the file re-applied.
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() "
                "AND table_name = 'mod_ollama_chat_autopilot' AND column_name = 'goal_kind'"))
            g_tablesOk = (*result)[0].Get<uint64>() == 1;
    }

    if (!g_tablesOk)
    {
        if (g_cfg.enable)
            LOG_ERROR("server.loading",
                      "[Ollama Chat] Autopilot tables are missing or out of date; apply "
                      "data/sql/characters/base/2026_10_05_autopilot.sql. Autopilot stays off.");
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
            "SELECT bot_guid, mode, enrolled, source, playstyle, awareness, kills_total, "
            "deaths_total, quests_total, enrolled_at, activity, activity_since, decided_by, "
            "dispositions, goal_text, last_reason, dungeons_total, playbook, goal_kind, "
            "goal_target, goal_target_id, goal_value, goal_baseline, goal_set_at "
            "FROM mod_ollama_chat_autopilot"))
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
            row.playstyle     = f[4].Get<std::string>();
            row.awareness     = f[5].Get<std::string>();
            row.killsTotal    = f[6].Get<uint32>();
            row.deathsTotal   = f[7].Get<uint32>();
            row.questsTotal   = f[8].Get<uint32>();
            row.enrolledAt    = f[9].Get<uint32>();
            row.activity      = f[10].Get<std::string>();
            row.activitySince = f[11].Get<uint32>();
            row.decidedBy     = f[12].Get<std::string>();
            row.dispositions  = ParseDispositions(f[13].Get<std::string>());
            row.lastReason    = f[15].Get<std::string>();
            row.dungeonsTotal = f[16].Get<uint32>();
            row.playbook      = ParseDispositions(f[17].Get<std::string>());

            row.goal.kind     = Goal_KindFromName(f[18].Get<std::string>());
            if (f[18].Get<std::string>().empty())
                row.goal.kind = GoalKind::None;
            row.goal.text     = f[14].Get<std::string>();
            row.goal.target   = f[19].Get<std::string>();
            row.goal.targetId = f[20].Get<uint32>();
            row.goal.value    = f[21].Get<uint32>();
            row.goal.baseline = f[22].Get<uint32>();
            row.goal.setAt    = f[23].Get<uint32>();
            if (row.enrolled)
            {
                CountEnrolled(row, +1);
                if (NormalizeDispositions(row))
                    row.dirty = true;
            }
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
    // however many bots are online, and the budget goes to enrolled bots
    // rather than being spread across everyone.
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
        { "on",        HandleOn,        SEC_ADMINISTRATOR, Console::Yes },
        { "off",       HandleOff,       SEC_ADMINISTRATOR, Console::Yes },
        { "rules",     HandleRules,     SEC_ADMINISTRATOR, Console::Yes },
        { "status",    HandleStatus,    SEC_ADMINISTRATOR, Console::Yes },
        { "history",   HandleHistory,   SEC_ADMINISTRATOR, Console::Yes },
        { "preview",   HandlePreview,   SEC_ADMINISTRATOR, Console::Yes },
        { "playstyle", HandlePlaystyle, SEC_ADMINISTRATOR, Console::Yes },
        { "awareness", HandleAwareness, SEC_ADMINISTRATOR, Console::Yes },
        { "replan",    HandleReplan,    SEC_ADMINISTRATOR, Console::Yes },
        { "activity",  HandleActivity,  SEC_ADMINISTRATOR, Console::Yes },
        { "goal",      HandleGoal,      SEC_ADMINISTRATOR, Console::Yes },
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

// The hooks below run on map threads. Each touches only the player it was
// handed and module state under g_mutex (taken by WithEnrolledRow).

// Online state for a bot, if it is tracked. g_mutex held.
static Online* OnlineOf(uint64_t guid)
{
    auto it = g_online.find(guid);
    return it == g_online.end() ? nullptr : &it->second;
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

        // Whatever this level opened up is a temptation.
        for (const AutopilotPreset& p : AutopilotPresets_Activities())
            if (p.minLevel > oldLevel && p.minLevel <= level)
                AddTemptation(*ob, SafeFormat("{} is open to them now (level {}): {}", p.name, p.minLevel,
                                              p.description), now);

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
            NoteReward(*ob, "loot", tmpl->Quality >= ITEM_QUALITY_EPIC ? 3 : 1, now);
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
