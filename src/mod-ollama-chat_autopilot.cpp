#include "mod-ollama-chat_autopilot.h"
#include "mod-ollama-chat_autopilot_commands.h"
#include "mod-ollama-chat_autopilot_goals.h"
#include "mod-ollama-chat_autopilot_group.h"
#include "mod-ollama-chat_autopilot_planner.h"
#include "mod-ollama-chat_autopilot_schema.h"
#include "mod-ollama-chat_autopilot_strategy.h"
#include "mod-ollama-chat_autopilot_world.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_progress.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CharacterCache.h"
#include "CellImpl.h"
#include "Chat.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Group.h"
#include "Guild.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "GameTime.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QuestDef.h"
#include "WorldSession.h"
#include "Corpse.h"
#include "Opcodes.h"
#include "WorldPacket.h"

#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "PlayerbotRepository.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <deque>
#include <iterator>
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
        uint32_t maxLevel          = 80;
        uint32_t maxEnrolled       = 0;     // 0 = no cap

        std::unordered_set<uint32_t>    guilds;
        std::unordered_set<uint32_t>    accounts;
        std::unordered_set<std::string> includeNames;   // lowercase
        std::unordered_set<std::string> excludeNames;   // lowercase

        uint32_t sweepIntervalMs         = 1000;
        uint32_t botsPerSweep            = 25;
        uint32_t snapshotIntervalMinutes = 10;
        uint32_t flushIntervalSeconds    = 60;
        uint32_t snapshotRetention       = 500;
        uint32_t eventRetention          = 300;

        // Control.
        bool     control                 = true;    // run the model's commands at all
        uint32_t withRealPlayer          = 1;       // 0 hands off, 1 combat strategies only
        bool     noTeleport              = true;    // hold playerbots' random-bot teleports
        bool     noRandomize             = true;    // hold playerbots' periodic re-rolls
        bool     noHandouts              = true;    // earn everything: no free gear, spells, repairs, flights
        bool     keepOnline              = true;    // enrolled random bots stay logged in, across restarts too
        uint32_t corpseRunMinutes        = 10;      // then let playerbots revive it
        bool     llmEnable               = true;
        uint32_t llmCallsPerHour         = 300;
        uint32_t maxConcurrentPlans      = 2;
        uint32_t decisionIntervalMinutes = 15;      // foreground
        uint32_t backgroundMinutes       = 30;      // background
        uint32_t defaultPlanMinutes      = 30;
        uint32_t quickReplanSeconds      = 60;      // after an errand ends
        float    foregroundRange         = 100.0f;  // yards, for Scope::Range

        // Reach: who gets LLM time.
        Scope    foregroundScope         = Scope::Zone;
        Scope    backgroundScope         = Scope::Map;
        Tier     minimumTier             = Tier::Dormant;
        bool     requireRealPlayer       = true;    // false: every bot planned as if watched

        // Guilds with a human member.
        bool     selectRealPlayerGuilds  = false;
        Tier     realGuildTier           = Tier::Background;
        uint32_t realGuildRefreshMinutes = 10;
        uint32_t planTimeoutSeconds      = 300;
        bool     groups                  = true;    // the model decides who the bot groups with
        std::string promptTemplate;

        // Alerts: conditions that ask the model early. The model decides
        // what to do about them.
        uint32_t alertDeaths             = 3;
        uint32_t alertDeathWindowMinutes = 15;
        uint32_t alertDurabilityPct      = 20;
        uint32_t alertFreeBagSlots       = 2;
        uint32_t alertCooldownMinutes    = 30;
        uint32_t alertIdleMinutes        = 5;       // no trip, no fight, not moving

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
    // Strategy state as "engine:name" -> on, e.g. "nc:grind" -> true.
    using StrategyMap = std::map<std::string, bool>;

    // Everything about who the bot is and what it does comes from the model:
    // the identity, the goal, and the commands it gives. Nothing here is
    // chosen by code.
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
        AutopilotGoal    goal;
        std::string      lastReason;

        // Every strategy the model has switched with nc/co, as it last set
        // it. Playerbots resets wipe strategies constantly; these are put back
        // afterwards, as a player would re-type them.
        StrategyMap      strategies;

        // What the model's last orders did, for its next prompt.
        std::vector<std::string> lastResults;

        // Both engines' full strategy lists before the model first changed
        // anything. Persisted, so the bot can be handed back exactly --
        // playerbots' own store may have captured the model's changes.
        std::string      baseline;

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
        uint32_t planSeq         = 0;       // the plan in flight; older replies are stale
        uint32_t planSubmittedAt = 0;
        bool     urgentPlan      = false;
        bool     quickPlan       = false;   // an errand ended: the bot is waiting for orders
        Tier     tier            = Tier::Dormant;

        // Out-of-combat strategies are only the model's while the bot is its
        // own; true while they need putting back once it is again.
        bool     ncReplay        = false;
        bool     coReplay        = false;   // put the combat ones back on the next visit

        // A walk to a service or a zone, from `goto`.
        AutopilotErrand errand;
        // The model's later goto/quest orders from the same plan, run in turn
        // as each errand ends (one plan can say "turn in, then train, then hunt").
        std::deque<std::string> errandQueue;
        AutopilotTrip   corpseTrip;     // a ghost walking back to its body
        uint32_t        corpseRetryAt = 0;  // after a corpse run that could not be made
        uint32_t        nextTaxiLook = 0;   // when to look for a flight master nearby
        uint32_t        lastCombatAt = 0;   // last seen fighting or attacked
        uint32_t        castHoldUntil = 0;  // the walk waits while the bot casts
        uint32_t        walkingSince  = 0;  // continuous walking, for the upkeep pause
        std::deque<uint64_t> lootTried;     // bodies already looted (or refused), newest last
        uint32_t        groupWaitSince    = 0;  // leading: waiting for members who fell behind
        uint32_t        groupWaitOffUntil = 0;  // gave up waiting; walk on until then

        // Teleport holds (NoTeleport): when the random-bot teleport was last
        // pushed back, and the corpse run in progress.
        uint32_t teleportDeferredAt = 0;
        uint32_t deadSince          = 0;
        bool     reviveHeld         = false;

        // Conditions the model should hear about early.
        std::deque<uint32_t>     deathTimes;
        std::unordered_map<std::string, uint32_t> alertCooldown;   // kind -> may fire again at
        std::string              lastAlert;
        std::string              lastAlertKind;     // "bags", "invite"...: to tell when it is over
        uint32_t                 lastBusyAt  = 0;   // last seen on a trip, fighting or moving
        uint32_t                 lastAlertAt = 0;

        // Whispers to and from the bot, newest last, for the prompt: how it
        // asks someone to group up and hears the answer.
        struct Whisper
        {
            std::string who;
            std::string text;
            uint32_t    at       = 0;
            bool        fromThem = false;
        };
        std::deque<Whisper>      whispers;
        uint32_t                 whisperPlanAt = 0;   // a whisper may ask the model again from then

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
    // commands; map threads hold it briefly in the progress hooks. Lock order
    // is this mutex, then the progress queue's / memory's / planner's.
    //
    // Recursive on purpose: the sweep acts on the world (a quest turned in, a
    // spell learned at a trainer) and the core fires our own progress hooks
    // for it on the same thread -- level up, rare loot, an achievement --
    // which lock this again. Those hooks only update fields of entries that
    // already exist; they never insert or erase, so references the sweep
    // holds into g_rows / g_online stay valid.
    std::recursive_mutex                 g_mutex;
    std::unordered_map<uint64_t, Row>    g_rows;
    std::unordered_map<uint64_t, Online> g_online;
    std::vector<uint64_t>                g_roster;     // round-robin order
    std::unordered_set<uint64_t>         g_realOnline; // real players, for tiers
    std::unordered_set<uint32_t>         g_realGuilds; // guilds with a human member, online or not
    std::unordered_set<uint64_t>         g_aboard;     // at a dock or on a boat: visited every sweep
    std::unordered_set<uint64_t>         g_walking;    // on a trip: checked every sweep for attackers
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
    uint64_t g_statCommands = 0;   // commands run
    uint64_t g_statRefused  = 0;   // commands denied or that could not run
    uint64_t g_statAlerts   = 0;
    uint64_t g_statReplays  = 0;   // strategies put back after a playerbots reset
    uint64_t g_statArrived  = 0;   // trips that got there
    uint64_t g_statFailed   = 0;   // trips that did not
    uint64_t g_statCorpse   = 0;   // corpse runs held / given up on

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

    // --- strategy bookkeeping ---------------------------------------------

    BotState EngineOf(const std::string& key)
    {
        return key.rfind("co:", 0) == 0 ? BOT_STATE_COMBAT : BOT_STATE_NON_COMBAT;
    }

    // "nc +grind,-quest" / "co ~aoe" -> the changes it makes, by key. Toggles
    // resolve against the live bot. Resets ("!") and queries ("?") change no
    // stored choice.
    StrategyMap ParseStrategyCommand(PlayerbotAI* ai, const std::string& command)
    {
        StrategyMap out;
        const std::string lower = Lower(command);
        const std::string engine = lower.substr(0, 2);
        const BotState state = engine == "co" ? BOT_STATE_COMBAT : BOT_STATE_NON_COMBAT;
        for (std::string item : SplitString(lower.size() > 3 ? lower.substr(3) : "", ','))
        {
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            if (item.size() < 2 || (item[0] != '+' && item[0] != '-' && item[0] != '~'))
                continue;
            const std::string name = item.substr(1);
            if (name == AUTOPILOT_STRATEGY_NAME)
                continue;
            const bool on = item[0] == '+' || (item[0] == '~' && !ai->HasStrategy(name, state));
            out[engine + ":" + name] = on;
        }
        return out;
    }

    // Make one stored choice true on the bot. False when it already was.
    bool ApplyStrategy(PlayerbotAI* ai, const std::string& key, bool on)
    {
        const std::string name = key.substr(3);
        const BotState state = EngineOf(key);
        if (ai->HasStrategy(name, state) == on)
            return false;
        ai->ChangeStrategy((on ? "+" : "-") + name, state);
        return true;
    }

    // Put the model's choices back on one or both engines.
    uint32_t ReplayStrategies(PlayerbotAI* ai, const Row& row, bool nc, bool co)
    {
        uint32_t changed = 0;
        for (const auto& [key, on] : row.strategies)
        {
            const bool combat = EngineOf(key) == BOT_STATE_COMBAT;
            if ((combat ? co : nc) && ApplyStrategy(ai, key, on))
                ++changed;
        }
        return changed;
    }

    // Playerbots strategies that decide where a bot goes and what it does --
    // the model's job. Kept off; the model may not turn them on.
    constexpr const char* kControllerStrategies[] = { "nc:new rpg", "nc:rpg", "nc:travel" };

    bool IsController(const std::string& key)
    {
        return std::any_of(std::begin(kControllerStrategies), std::end(kControllerStrategies),
                           [&](const char* k) { return key == k; });
    }

    std::string FormatStrategies(const StrategyMap& m)
    {
        std::string out;
        for (const auto& [key, on] : m)
            out += SafeFormat("{}{}{}", out.empty() ? "" : ",", on ? "+" : "-", key);
        return out;
    }

    StrategyMap ParseStrategies(const std::string& text)
    {
        StrategyMap out;
        for (const std::string& item : SplitString(text, ','))
            if (item.size() > 4 && (item[0] == '+' || item[0] == '-'))
                out[item.substr(1)] = item[0] == '+';
        return out;
    }

    // "nc:a,b,c;co:d,e" -- both engines' full lists.
    std::string CaptureStrategies(PlayerbotAI* ai)
    {
        auto join = [](const std::vector<std::string>& v)
        {
            std::string s;
            for (const std::string& n : v)
                if (n != AUTOPILOT_STRATEGY_NAME)
                    s += (s.empty() ? "" : ",") + n;
            return s;
        };
        return "nc:" + join(ai->GetStrategies(BOT_STATE_NON_COMBAT)) + ";co:" +
               join(ai->GetStrategies(BOT_STATE_COMBAT));
    }

    // Make one engine's strategies exactly the recorded list.
    void RestoreEngine(PlayerbotAI* ai, const std::string& baseline, BotState state)
    {
        const std::string prefix = state == BOT_STATE_COMBAT ? "co:" : "nc:";
        std::set<std::string> want;
        bool found = false;
        for (const std::string& part : SplitString(baseline, ';'))
            if (part.rfind(prefix, 0) == 0)
            {
                found = true;
                for (const std::string& n : SplitString(part.substr(3), ','))
                    want.insert(n);
            }
        if (!found)
            return;

        for (const std::string& n : ai->GetStrategies(state))
            if (n != AUTOPILOT_STRATEGY_NAME && !want.count(n))
                ai->ChangeStrategy("-" + n, state);
        for (const std::string& n : want)
            if (!ai->HasStrategy(n, state))
                ai->ChangeStrategy("+" + n, state);
    }

    // Return a bot to what it was before the model touched it: playerbots'
    // defaults (plus, for alts, what their master saved -- ResetStrategies
    // alone does not reload it; mirrors PlayerbotHolder::OnBotLogin), then
    // exactly the lists recorded before the model's first change, which
    // undoes anything playerbots' store captured from us.
    void HandBack(PlayerbotAI* ai, Row* row, Online* ob)
    {
        ai->ResetStrategies();
        Player* bot = ai->GetBot();
        if (bot && !sRandomPlayerbotMgr.IsRandomBot(bot))
            PlayerbotRepository::instance().Load(ai);
        if (row && !row->baseline.empty())
        {
            RestoreEngine(ai, row->baseline, BOT_STATE_NON_COMBAT);
            RestoreEngine(ai, row->baseline, BOT_STATE_COMBAT);
            if (bot && !sRandomPlayerbotMgr.IsRandomBot(bot))
                PlayerbotRepository::instance().Save(ai);
        }

        // Walks and teleport holds end with control.
        if (ob)
        {
            AutopilotCommands_StopErrand(ai, ob->errand);
            ob->errand = AutopilotErrand();
            ob->errandQueue.clear();
            // A dead bot may be on a held revive from an earlier session too.
            if (bot && (ob->reviveHeld || !bot->IsAlive()) && sRandomPlayerbotMgr.IsRandomBot(bot))
                sRandomPlayerbotMgr.SetValue(bot->GetGUID().GetCounter(), "revive", 0);
            ob->reviveHeld = false;
            ob->ncReplay   = false;
        }
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
        // The dead engine too: its multipliers keep playerbots' free release
        // and revive off an enrolled bot (see the marker strategy).
        for (BotState state : { BOT_STATE_NON_COMBAT, BOT_STATE_COMBAT, BOT_STATE_DEAD })
            if (!HasMarker(ai, state))
                ai->ChangeStrategy(std::string("+") + AUTOPILOT_STRATEGY_NAME, state);
    }

    void RemoveMarker(PlayerbotAI* ai)
    {
        for (BotState state : { BOT_STATE_NON_COMBAT, BOT_STATE_COMBAT, BOT_STATE_DEAD })
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
        if (type == "plan" || type == "order" || type == "identity" || type == "identity_changed")
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

    std::string JoinLines(const std::vector<std::string>& items)
    {
        std::string out;
        for (const std::string& s : items)
            out += (out.empty() ? "" : "\n") + s;
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
        bool     leads          = false;   // leads a group with others in it
        std::string leaderName;            // whose group it follows
        uint64_t groupGuid      = 0;

        // Combat strategies are the model's everywhere -- dungeons, a human's
        // group -- except where the operator asked for hands off.
        bool CanUseCombat() const
        {
            return !dead && !(withRealPlayer && g_cfg.withRealPlayer == 0);
        }

        // Everything else only while the bot is its own: a group leader or
        // an instance owns its movement otherwise.
        bool CanUseNonCombat() const
        {
            return !dead && !withRealPlayer && !inInstance && !follower;
        }

        // For the prompt: why only some orders will be carried out.
        std::string Limits() const
        {
            if (!CanUseCombat())
                return "";
            if (withRealPlayer)
                return "They are in a group with a real player, who leads them: only co (combat), whisper and group "
                       "orders will be carried out while they are in it. Someone chose to play with them: stay, and "
                       "leave (group leave) only if they truly must go a different way.";
            if (inBattleground)
                return "They are in a battleground: only co (combat) orders will be carried out until it ends.";
            if (inInstance)
                return "They are in a dungeon: only co (combat) orders will be carried out until they leave.";
            if (follower)
                return "They are in a group led by " + (leaderName.empty() ? std::string("another bot") : leaderName) +
                       ": they follow the leader and fight alongside. Only co (combat), whisper and group orders "
                       "will be carried out while they are in it. Groups stick together: leave (group leave) only if "
                       "their goals have truly parted ways with the group's, not for an errand or on a whim.";
            return "";
        }
    };

    Situation Classify(Player* bot)
    {
        Situation s;
        s.dead       = !bot->IsAlive();
        s.inCombat   = bot->IsInCombat();
        s.travelling = bot->IsInFlight() || bot->GetTransport() != nullptr;
        s.inFlight   = bot->IsInFlight();

        if (Map* map = bot->GetMap())
        {
            s.inBattleground = map->IsBattlegroundOrArena();
            s.inInstance     = map->IsDungeon() || s.inBattleground;
        }

        if (Group* group = bot->GetGroup())
        {
            const bool leads = group->GetLeaderGUID() == bot->GetGUID();
            s.groupGuid      = group->GetGUID().GetRawValue();
            // A group the bot leads stays its own, real players in it or not:
            // they joined it. Only someone else's lead takes it out of the
            // model's hands.
            Player* leader   = leads ? bot : ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
            const bool humanLeads = !leads && leader && !OllamaIsBotPlayer(leader);
            s.withRealPlayer = humanLeads;
            s.follower       = !leads && !humanLeads;
            s.leads          = leads && group->GetMembersCount() > 1;
            if (!leads && leader)
                s.leaderName = leader->GetName();
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
        // No player needed: every enrolled bot is planned at the full rate.
        if (!g_cfg.requireRealPlayer)
            return Tier::Foreground;

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

                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (guilds != g_realGuilds)
                {
                    g_realGuilds.swap(guilds);
                    OnRealGuildsChanged();
                }
            }));
    }

    // ----------------------------------------------------------------------
    // Carrying out the model's orders. World thread, g_mutex held.
    // ----------------------------------------------------------------------

    // Put back what a playerbots reset wiped, and keep out-of-combat
    // strategies the bot's own only while it is. The marker on each engine is
    // that engine's reset detector: a reset clears it together with
    // everything else, a human toggling `co -flee` does not.
    void Reassert(PlayerbotAI* ai, Row& row, Online& ob, const Situation& sit)
    {
        const bool ncReset = !HasMarker(ai, BOT_STATE_NON_COMBAT);
        const bool coReset = !HasMarker(ai, BOT_STATE_COMBAT);
        if (ncReset || coReset || !HasMarker(ai, BOT_STATE_DEAD))
        {
            AddMarker(ai);
            ob.markerOurs = true;
            if (ncReset)
                ob.ncReplay = true;
        }

        if (!g_cfg.control)
        {
            // Diary only (`Autopilot.Control = 0`, possibly just reloaded):
            // nothing of the model's may stay on the bot.
            if (!row.baseline.empty())
            {
                HandBack(ai, &row, &ob);
                AddMarker(ai);
                row.baseline.clear();
                row.dirty = true;
            }
            return;
        }

        // Flights only between flight points it has discovered, as for a
        // player: playerbots gives random bots the taxi cheat when their AI is
        // created, and nothing sets it again.
        if (g_cfg.noHandouts && ai->GetBot()->isTaxiCheater())
            ai->GetBot()->SetTaxiCheater(false);

        // Record the bot as it was before autopilot changes anything, so it
        // can be handed back exactly.
        if (row.baseline.empty())
            row.baseline = CaptureStrategies(ai);

        // The model is the bot's controller. Playerbots' own overhead
        // controllers -- NewRpg, the old rpg wanderer, the travel planner --
        // would pick destinations and activities behind its back, so they
        // stay off while the bot is its own. Hand-back restores them.
        if (sit.CanUseNonCombat())
            for (const char* key : kControllerStrategies)
                ApplyStrategy(ai, key, false);

        if ((coReset || ob.coReplay) && sit.CanUseCombat())
        {
            g_statReplays += ReplayStrategies(ai, row, false, true);
            ob.coReplay = false;
        }

        // Joined a human's group, or walked into a dungeon: its movement is
        // someone else's now, so the out-of-combat engine goes back to what
        // the bot had before the model, and the walk in progress ends.
        //
        // The trip ends whatever else happened this visit: joining a group or
        // a dungeon usually comes with a playerbots reset too, and the trip
        // must not be left frozen to resume long after the model moved on.
        if (!sit.CanUseNonCombat() && !sit.dead)
        {
            if (ob.errand.active)
            {
                AutopilotCommands_StopErrand(ai, ob.errand);
                g_aboard.erase(ai->GetBot()->GetGUID().GetRawValue());
            }
            ob.errandQueue.clear();   // the model is asked afresh once the bot is its own again
            if (!ob.ncReplay)
            {
                // A member of a bot's group keeps what playerbots gave it on
                // joining (follow among it); the solo baseline would drop it.
                if (!row.baseline.empty() && !sit.follower)
                    RestoreEngine(ai, row.baseline, BOT_STATE_NON_COMBAT);
                ob.ncReplay = true;
            }

            // In a bot's group, stay with the leader (unless told to stay put),
            // as playerbots sets a bot up when it joins.
            if (sit.follower)
            {
                Player* self = ai->GetBot();
                if (!ai->GetMaster() && sRandomPlayerbotMgr.IsRandomBot(self) && self->GetGroup())
                    if (Player* leader = ObjectAccessor::FindConnectedPlayer(self->GetGroup()->GetLeaderGUID()))
                        ai->SetMaster(leader);
                if (!ai->HasStrategy("follow", BOT_STATE_NON_COMBAT) && !ai->HasStrategy("stay", BOT_STATE_NON_COMBAT))
                    ai->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);
            }
        }

        if (ob.ncReplay && sit.CanUseNonCombat())
        {
            g_statReplays += ReplayStrategies(ai, row, true, false);
            ob.ncReplay = false;
        }
    }

    // Run the model's orders, in order. Returns what each did.
    std::vector<std::string> RunCommands(Player* bot, PlayerbotAI* ai, Row& row, Online& ob,
                                         const std::vector<std::string>& commands, const Situation& sit,
                                         uint32_t now)
    {
        std::vector<std::string> results;

        // Errands run one at a time, in the order given. A plan that gives any
        // replaces the ones still waiting from the last plan; one that only
        // adjusts strategies leaves the errand and its queue alone.
        const bool newErrands = std::any_of(commands.begin(), commands.end(), [](const std::string& c)
                                            { return AutopilotCommands_IsErrand(AutopilotCommands_Normalize(c)); });
        if (newErrands)
            ob.errandQueue.clear();
        bool errandStarted = false;

        for (const std::string& raw : commands)
        {
            const std::string command = AutopilotCommands_Normalize(raw);
            const bool strategy = AutopilotCommands_IsStrategyChange(command);
            const bool combat   = strategy && Lower(command).rfind("co", 0) == 0;

            std::string result;
            if (AutopilotCommands_IsDenied(command))
                result = "denied by the server";
            else if (!sit.CanUseCombat())
                result = sit.dead ? "not carried out (they are dead; give it again once they are alive)"
                                  : "not carried out (hands off in a player's group)";
            else if (AutopilotGroup_IsOrder(command))
            {
                // At once, never queued behind a trip: asking someone to group
                // up and then setting off is one plan.
                std::string whisperedTo;
                // Leaving is never blocked, a real player's group included: the
                // prompt asks them to stay unless their goals part ways.
                if (!g_cfg.groups)
                    result = "not carried out (grouping is not theirs to decide on this server)";
                else
                    result = AutopilotGroup_Run(bot, ai, command, whisperedTo);
            }
            else if (!combat && !sit.CanUseNonCombat())
                result = "not carried out (only combat orders right now)";
            else
            {
                // The first change the model makes is when "before" is taken.
                if (strategy && row.baseline.empty())
                    row.baseline = CaptureStrategies(ai);

                if (strategy)
                {
                    // Directly on the engine, not through the chat command:
                    // the command would also save the change into
                    // playerbots' own store as if a master had made it.
                    const StrategyMap changes = ParseStrategyCommand(ai, command);
                    std::string unknown;
                    size_t      unknownCount = 0;
                    for (const auto& [key, on] : changes)
                    {
                        if (on && IsController(key))
                        {
                            unknown += (unknown.empty() ? "" : ", ") + key.substr(3) + " (you are their controller)";
                            ++unknownCount;
                            continue;
                        }
                        // A name playerbots does not know changes nothing;
                        // say so rather than keep replaying it.
                        ApplyStrategy(ai, key, on);
                        if (ai->HasStrategy(key.substr(3), EngineOf(key)) != on)
                        {
                            unknown += (unknown.empty() ? "" : ", ") + key.substr(3) + " (no such strategy)";
                            ++unknownCount;
                        }
                        else
                            row.strategies[key] = on;
                    }
                    if (changes.empty())
                        result = "nothing to change";
                    else if (!unknown.empty())
                        result = (unknownCount == changes.size() ? "" : "done, except ") +
                                 std::string("not changed: ") + unknown;
                    else
                        result = "done";
                }
                else if (errandStarted)
                {
                    // After a trip in the same plan, an order waits for the trip:
                    // another trip would replace it, and "goto vendor, then
                    // b vendor" means buying at the vendor, not here. Strategy
                    // changes (above) apply at once, for the way there too.
                    ob.errandQueue.push_back(command);
                    result = SafeFormat("queued ({} in line)", ob.errandQueue.size());
                }
                else
                {
                    result = AutopilotCommands_Run(bot, ai, command, ob.errand, now);
                    if (AutopilotCommands_IsErrand(command) && ob.errand.active)
                        errandStarted = true;
                }
            }

            const bool ran = result.rfind("done", 0) == 0 || result == "sent" || result.rfind("on the way", 0) == 0 ||
                             result.rfind("queued", 0) == 0;
            ++(ran ? g_statCommands : g_statRefused);
            results.push_back(command + " -> " + result);
        }

        if (!results.empty())
        {
            row.lastResults = results;
            row.dirty       = true;
        }
        return results;
    }

    // NoTeleport: playerbots moves random bots around by teleport -- every
    // hour or so "for level", and straight after a death instead of a corpse
    // run. For bots the model runs, it gets them there the old-fashioned way.
    void HoldTeleports(Player* bot, uint64_t guid, Online& ob, const Situation& sit, uint32_t now)
    {
        if (!g_cfg.control || !sRandomPlayerbotMgr.IsRandomBot(bot))
            return;
        const uint32_t low = bot->GetGUID().GetCounter();

        // The periodic teleport: keep pushing it two hours out. The periodic
        // re-roll (NoRandomize) too: it re-gears the bot, and below level 3 or
        // at the level cap rolls it a new level and a new place.
        if (!ob.teleportDeferredAt || now - ob.teleportDeferredAt >= kHour)
        {
            if (g_cfg.noTeleport)
                sRandomPlayerbotMgr.ScheduleTeleport(low, 2 * kHour);
            if (g_cfg.noRandomize)
                sRandomPlayerbotMgr.SetValue(low, "randomize", 1);
            // Stay online: the random-bot rotation logs a bot out when its
            // "add" lapses. SetValue keeps it for MaxRandomBotInWorldTime.
            if (g_cfg.keepOnline)
                sRandomPlayerbotMgr.SetValue(low, "add", 1);
            ob.teleportDeferredAt = now;
        }

        if (!g_cfg.noTeleport)
            return;

        // A death: playerbots marks it, then revives and teleports the bot to
        // a grind spot 1-5 minutes later. Mark it ourselves first, with the
        // revive held, so the dead strategy walks the ghost back.
        if (sit.dead && !bot->InBattleground())
        {
            if (!ob.deadSince)
                ob.deadSince = now;
            if (!ob.reviveHeld && now - ob.deadSince < g_cfg.corpseRunMinutes * 60)
            {
                if (!sRandomPlayerbotMgr.GetValue(low, "dead"))
                    sRandomPlayerbotMgr.SetValue(low, "dead", 1);
                sRandomPlayerbotMgr.SetValue(low, "revive", 1);
                ob.reviveHeld = true;
                ++g_statCorpse;
            }
            else if (ob.reviveHeld && now - ob.deadSince >= g_cfg.corpseRunMinutes * 60 &&
                     bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) && !sit.withRealPlayer && !sit.follower &&
                     !sit.inInstance)
            {
                // Could not make it back: take the spirit healer's offer, as a
                // player would -- resurrection sickness and the durability
                // loss included. Playerbots' own revive would instead re-roll
                // the bot (bags emptied, free consumables, money topped up).
                bot->GetSession()->SendSpiritResurrect();
                RecordEvent(guid, "corpse_run", SafeFormat("could not reach the body in {} minutes; "
                                                           "resurrected by the spirit healer", g_cfg.corpseRunMinutes));
            }
            return;
        }

        if (ob.deadSince)
        {
            // Alive again. Clear the marks, or the next death would find
            // "dead" already set and be revived (and teleported) at once.
            if (ob.reviveHeld)
            {
                sRandomPlayerbotMgr.SetValue(low, "revive", 0);
                sRandomPlayerbotMgr.SetValue(low, "dead", 0);
                RecordEvent(guid, "corpse_run", SafeFormat("made it back to the body in {}",
                                                           Span(now - ob.deadSince)));
            }
            ob.deadSince  = 0;
            ob.reviveHeld = false;
        }
    }


    // The corpse run, as a player does it: release, walk the ghost back to the
    // body along the navmesh route, and reclaim it through the same handler
    // the client's "Resurrect" button reaches (it checks distance and the
    // reclaim delay itself).
    //
    // Playerbots has its own (find corpse), but it only walks bots it counts
    // as active -- the rest wait and are teleported to the body -- and it
    // gives up ten minutes after the death. With NoTeleport holding back
    // playerbots' teleporting revive, a ghost could otherwise stand at the
    // graveyard until CorpseRunMinutes released the hold.
    void RunCorpse(Player* bot, PlayerbotAI* ai, uint64_t guid, Online& ob, const Situation& sit, uint32_t now)
    {
        // Hands off the same as when alive: with a real player, following a
        // group leader or inside an instance, the ghost is theirs to handle.
        if (!g_cfg.control || bot->InBattleground() || sit.withRealPlayer || sit.follower || sit.inInstance)
            return;

        if (!bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        {
            // Release the way the client's button does. Playerbots' own
            // release also repairs every item for free.
            WorldPacket repop(CMSG_REPOP_REQUEST, 1);
            repop << uint8(0);
            bot->GetSession()->HandleRepopRequestOpcode(repop);
            return;
        }

        Corpse* corpse = bot->GetCorpse();
        if (!corpse || corpse->GetMapId() != bot->GetMapId())
            return;   // died in an instance: playerbots' own handling

        // The core reclaims within CORPSE_RECLAIM_RADIUS (39 yd, in 3D), once
        // its delay is up; the handler simply ignores an early try, so this
        // repeats each visit until the game allows it.
        if (bot->GetExactDist(corpse) <= 35.0f)
        {
            if (ob.corpseTrip.active)
                AutopilotTravel_Stop(ai, ob.corpseTrip);
            WorldPacket packet(CMSG_RECLAIM_CORPSE);
            packet << bot->GetGUID();
            bot->GetSession()->HandleReclaimCorpseOpcode(packet);
            return;
        }

        if (!ob.corpseTrip.active)
        {
            if (now < ob.corpseRetryAt)
                return;
            const std::string why = AutopilotTravel_Start(
                bot, { corpse->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ() },
                15.0f, 0, ob.corpseTrip, now, /*allowFlights*/ false);
            if (!why.empty())
            {
                ob.corpseRetryAt = now + 30;
                return;
            }
            RecordEvent(guid, "corpse_run", SafeFormat("walking back to the body ({} yd)",
                                                       uint32_t(bot->GetExactDist2d(corpse))));
        }

        std::string note;
        const AutopilotTripState state = AutopilotTravel_Update(bot, ai, ob.corpseTrip, now, note);
        if (state != AutopilotTripState::Going && state != AutopilotTripState::Failed)
        {
            // "Arrived" over or under the body (a canyon, deep water) but not
            // within reach: walking there again would arrive again at once.
            // Wait for the spirit healer fallback instead of churning.
            ob.corpseTrip    = AutopilotTrip();
            ob.corpseRetryAt = now + 60;
            return;
        }
        if (state == AutopilotTripState::Failed)
        {
            // Stuck on the way: try again from wherever the ghost stands, in a
            // little while rather than every sweep.
            ob.corpseTrip    = AutopilotTrip();
            ob.corpseRetryAt = now + 30;
        }
    }

    // Passing a flight master, a player stops to pick up the flight point;
    // so does an autopilot bot. Trips fly only between discovered points
    // (unless playerbots gives the bot its taxi cheat), so this is how its
    // flight network fills in as it travels -- the natural way.
    void DiscoverFlightPoint(Player* bot, uint64_t guid, Online& ob, uint32_t now)
    {
        if (now < ob.nextTaxiLook || bot->isTaxiCheater())
            return;
        ob.nextTaxiLook = now + 10;

        AutopilotPlace fm;
        if (!AutopilotWorld_NearestService(bot, AutopilotService::FlightMaster, fm) || fm.distance > 30.0f)
            return;
        const uint32 node = sObjectMgr->GetNearestTaxiNode(fm.x, fm.y, fm.z, fm.map, bot->GetTeamId(true));
        if (!node || bot->m_taxi.IsTaximaskNodeKnown(node))
            return;
        Creature* npc = bot->FindNearestCreature(fm.entry, 35.0f);
        if (!npc || !npc->IsAlive())
            return;

        bot->GetSession()->SendLearnNewTaxiNode(npc);
        if (bot->m_taxi.IsTaximaskNodeKnown(node))
            if (TaxiNodesEntry const* n = sTaxiNodesStore.LookupEntry(node))
                RecordEvent(guid, "flight_point", SafeFormat("learned the flight point at {}", n->name[0]));
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
            // A flight that is part of the model's own trip is not a moment to
            // ask it anything; the trip carries on.
            if (!sit.inFlight && ob.prevInFlight && !ob.errand.active)
                boundary("landed after a flight");
        }

        ob.situationKnown = true;
        ob.prevInstance   = instance;
        ob.prevGrouped    = grouped;
        ob.prevInFlight   = sit.inFlight;
    }

    // Whether what an alert said is still so: bags sold, gear repaired, an
    // invite answered -- the prompt stops worrying about it then.
    bool AlertStillTrue(Player* bot, const Online& ob, const std::string& kind)
    {
        if (kind == "invite")
            return AutopilotGroup_Inviter(bot) != nullptr;
        if (kind == "bags")
            return bot->GetFreeInventorySpace() < std::max<uint32_t>(g_cfg.alertFreeBagSlots, 1);
        if (kind == "durability")
            return Progress_DurabilityPct(bot) < std::max<uint32_t>(g_cfg.alertDurabilityPct, 1);
        if (kind == "idle")
            return !ob.errand.active;
        if (kind == "quests")
        {
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (bot->GetQuestSlotQuestId(slot))
                    return false;
            return true;
        }
        return true;   // deaths: a fact of the last while
    }

    // Self-preservation facts the model should hear about now rather than at
    // its next scheduled look: dying again and again, gear falling apart,
    // full bags. Code never acts on them; it asks the model early and tells
    // it why. Each kind then keeps quiet for a while, since some cannot be
    // fixed quickly (a broke bot cannot repair).
    void CheckAlerts(Player* bot, uint64_t guid, Online& ob, uint32_t now)
    {
        const uint32_t window = g_cfg.alertDeathWindowMinutes * 60;
        while (!ob.deathTimes.empty() && now - ob.deathTimes.front() > window)
            ob.deathTimes.pop_front();

        auto ready = [&](const char* kind)
        {
            auto it = ob.alertCooldown.find(kind);
            return it == ob.alertCooldown.end() || now >= it->second;
        };

        std::string reason;
        const char* kind = nullptr;

        Player* inviter = g_cfg.groups ? AutopilotGroup_Inviter(bot) : nullptr;
        if (inviter && ready("invite"))
        {
            reason = SafeFormat("{} invited them to a group", inviter->GetName());
            kind   = "invite";
        }
        else if (g_cfg.alertDeaths > 0 && ob.deathTimes.size() >= g_cfg.alertDeaths && ready("deaths"))
        {
            reason = SafeFormat("died {} times in {} minutes in {}", ob.deathTimes.size(),
                                g_cfg.alertDeathWindowMinutes, Progress_ZoneName(bot->GetZoneId()));
            kind = "deaths";
            ob.deathTimes.clear();
        }
        else if (g_cfg.alertDurabilityPct > 0 && Progress_DurabilityPct(bot) < g_cfg.alertDurabilityPct &&
                 ready("durability"))
        {
            reason = SafeFormat("gear is badly damaged ({}% durability)", uint32_t(Progress_DurabilityPct(bot)));
            kind   = "durability";
        }
        else if (g_cfg.alertFreeBagSlots > 0 && bot->GetFreeInventorySpace() < g_cfg.alertFreeBagSlots &&
                 ready("bags"))
        {
            reason = "bags are full";
            kind   = "bags";
        }
        else if (g_cfg.alertIdleMinutes > 0 && !ob.errand.active && ob.lastBusyAt &&
                 now - ob.lastBusyAt >= g_cfg.alertIdleMinutes * 60 && ready("idle"))
        {
            // Nothing else steers an autopilot bot: standing around means it
            // ran out of orders, so the model hears about it.
            reason = SafeFormat("has been standing around with nothing to do for {}", Span(now - ob.lastBusyAt));
            kind   = "idle";
        }

        // An empty quest log with quests to take nearby: a player would go
        // and get them. Looked up at most every five minutes (it walks the
        // quest-starter table).
        Group* group = bot->GetGroup();
        const bool ownMover = !(bot->GetMap() && bot->GetMap()->Instanceable()) &&
                              (!group || group->GetLeaderGUID() == bot->GetGUID());
        if (!kind && ownMover && ready("quests"))
        {
            bool empty = true;
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE && empty; ++slot)
                empty = bot->GetQuestSlotQuestId(slot) == 0;
            if (empty && !AutopilotWorld_QuestsOnOffer(bot, 1200.0f).empty())
            {
                reason = "their quest log is empty, and there are quests on offer nearby";
                kind   = "quests";
            }
            else
                ob.alertCooldown["quests"] = now + 300;
        }

        if (!kind)
            return;

        // Idle repeats on its own clock: a bot still idle after another
        // IdleMinutes is worth another look, not half an hour's silence.
        // An invite waits for an answer: ask again soon if it is still there.
        const std::string_view k(kind);
        ob.alertCooldown[kind] = now + (k == "invite" ? 3 : k == "idle" ? g_cfg.alertIdleMinutes
                                                                       : g_cfg.alertCooldownMinutes) * 60;
        ob.lastAlert     = reason;
        ob.lastAlertKind = kind;
        ob.lastAlertAt   = now;
        ob.urgentPlan  = true;
        ++g_statAlerts;
        RecordEvent(guid, "alert", reason);
    }

    // ----------------------------------------------------------------------
    // Asking the model. World thread, g_mutex held.
    // ----------------------------------------------------------------------

    // "nc: grind, loot, quest | co: dps, flee" -- what is on right now.
    std::string DescribeLiveStrategies(PlayerbotAI* ai)
    {
        auto list = [ai](BotState state)
        {
            std::string s;
            for (const std::string& n : ai->GetStrategies(state))
                if (n != AUTOPILOT_STRATEGY_NAME)
                    s += (s.empty() ? "" : ", ") + n;
            return s.empty() ? std::string("-") : s;
        };
        return "nc: " + list(BOT_STATE_NON_COMBAT) + " | co: " + list(BOT_STATE_COMBAT);
    }

    // What the bot is doing on the model's orders right now.
    std::string DescribeActivity(Player* bot, const Online& ob)
    {
        if (bot->IsInCombat())
            return "fighting";
        if (!ob.errand.active)
            return "standing by: no errand under way";
        if (ob.errand.hunting && !ob.errand.trip.active)
            return ob.errand.label + " (the nearest creature it still needs, first)";
        const std::string how = AutopilotTravel_Describe(bot, ob.errand.trip);
        return how.empty() ? "on the way to " + ob.errand.label : how + ", bound for " + ob.errand.label;
    }

    // "Worn Mail Vest (mail chest, armor 98, +2 Str +1 Sta)", plus "can't use"
    // when the bot cannot -- what a player reads in the reward tooltip.
    std::string DescribeRewardItem(Player* bot, ItemTemplate const* item)
    {
        static const char* const kArmor[]  = { "misc", "cloth", "leather", "mail", "plate", "buckler", "shield",
                                               "libram", "idol", "totem", "sigil" };
        static const char* const kWeapon[] = { "one-hand axe", "two-hand axe", "bow", "gun", "one-hand mace",
                                               "two-hand mace", "polearm", "one-hand sword", "two-hand sword", "weapon",
                                               "staff", "exotic", "exotic", "fist weapon", "weapon", "dagger",
                                               "thrown", "spear", "crossbow", "wand", "fishing pole" };
        static const char* const kSlot[]   = { "", "head", "neck", "shoulder", "shirt", "chest", "waist", "legs",
                                               "feet", "wrist", "hands", "finger", "trinket", "one-hand", "shield",
                                               "ranged", "back", "two-hand", "bag", "tabard", "chest", "main hand",
                                               "off hand", "held in off hand", "ammo", "thrown", "ranged", "quiver",
                                               "relic" };

        std::string kind;
        if (item->Class == ITEM_CLASS_WEAPON && item->SubClass < std::size(kWeapon))
            kind = kWeapon[item->SubClass];
        else if (item->Class == ITEM_CLASS_ARMOR && item->SubClass < std::size(kArmor))
        {
            kind = kArmor[item->SubClass];
            if (item->InventoryType < std::size(kSlot) && item->SubClass != ITEM_SUBCLASS_ARMOR_SHIELD)
                kind += std::string(" ") + kSlot[item->InventoryType];
        }
        else if (item->InventoryType && item->InventoryType < std::size(kSlot))
            kind = kSlot[item->InventoryType];
        else
            kind = "item";

        std::string stats;
        if (item->Armor)
            stats += SafeFormat(", armor {}", item->Armor);
        if (item->Damage[0].DamageMax > 0.0f)
            stats += SafeFormat(", {:.0f}-{:.0f} damage", item->Damage[0].DamageMin, item->Damage[0].DamageMax);
        for (uint32_t i = 0; i < item->StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
        {
            const int32 value = item->ItemStat[i].ItemStatValue;
            if (!value)
                continue;
            const char* name = nullptr;
            switch (item->ItemStat[i].ItemStatType)
            {
                case ITEM_MOD_AGILITY:           name = "Agi"; break;
                case ITEM_MOD_STRENGTH:          name = "Str"; break;
                case ITEM_MOD_INTELLECT:         name = "Int"; break;
                case ITEM_MOD_SPIRIT:            name = "Spi"; break;
                case ITEM_MOD_STAMINA:           name = "Sta"; break;
                case ITEM_MOD_ATTACK_POWER:      name = "attack power"; break;
                case ITEM_MOD_SPELL_POWER:       name = "spell power"; break;
                case ITEM_MOD_CRIT_RATING:       name = "crit"; break;
                case ITEM_MOD_HIT_RATING:        name = "hit"; break;
                case ITEM_MOD_HASTE_RATING:      name = "haste"; break;
                case ITEM_MOD_DEFENSE_SKILL_RATING: name = "defense"; break;
                case ITEM_MOD_DODGE_RATING:      name = "dodge"; break;
                case ITEM_MOD_MANA_REGENERATION: name = "mp5"; break;
                default:                         name = "other"; break;
            }
            stats += SafeFormat(", +{} {}", value, name);
        }

        return SafeFormat("{} ({}{}){}", item->Name1, kind, stats,
                          bot->CanUseItem(item) == EQUIP_ERR_OK ? "" : " - can't use");
    }

    // "Grif Wildheart in Dun Morogh, 120 yd" / "... in Darnassus, on another
    // continent": who takes a finished quest and how far that is, so a turn-in
    // across the world is the model's informed choice.
    std::string DescribeQuestEnder(Player* bot, uint32_t questId)
    {
        AutopilotPlace place;
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
            return "nobody to turn it in to could be found";
        const uint32 zone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, place.map, place.x, place.y, place.z);
        const std::string where = zone ? Progress_ZoneName(zone) : std::string("an unknown place");
        if (place.map != bot->GetMapId())
            return SafeFormat("{} in {}, on another continent", place.name, where);
        return SafeFormat("{} in {}, {:.0f} yd away", place.name, where, place.distance);
    }

    // Someone, as the model should see them:
    // "Ann (level 4 Mage, a bot on autopilot, 12 yd)". g_mutex held.
    std::string WhoIs(Player* bot, PlayerbotAI* ai, Player* p)
    {
        std::string kind = "a real player";
        if (OllamaIsBotPlayer(p))
        {
            auto it = g_rows.find(p->GetGUID().GetRawValue());
            kind = it != g_rows.end() && it->second.enrolled ? "a bot on autopilot" : "a bot";
        }
        const std::string where = p->GetMap() == bot->GetMap()
            ? SafeFormat("{} yd", uint32_t(bot->GetDistance(p)))
            : "far away in " + Progress_ZoneName(p->GetZoneId());
        return SafeFormat("{} (level {} {}, {}, {})", p->GetName(), p->GetLevel(),
                          ai->GetChatHelper()->FormatClass(p->getClass()), kind, where);
    }

    std::vector<uint32_t> QuestIds(Player* p)
    {
        std::vector<uint32_t> out;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (uint32_t id = p->GetQuestSlotQuestId(slot))
                out.push_back(id);
        return out;
    }

    std::string IdList(const std::vector<uint32_t>& ids)
    {
        std::string out;
        for (size_t i = 0; i < ids.size() && i < 8; ++i)
            out += (out.empty() ? "" : ", ") + SafeFormat("[{}]", ids[i]);
        return out;
    }

    // Their group: who leads, each member, and how their quests line up
    // with the bot's (to plan together, and to know what to share).
    std::string DescribeGroup(Player* bot, PlayerbotAI* ai)
    {
        std::string out;
        Group* group = bot->GetGroup();
        if (!group)
            out = "none, they are on their own.";
        else
        {
            const bool leads = group->GetLeaderGUID() == bot->GetGUID();
            const std::vector<uint32_t> mine = QuestIds(bot);
            std::string members;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* m = ref->GetSource();
                if (!m || m == bot || !m->IsInWorld())
                    continue;
                std::string line = (group->GetLeaderGUID() == m->GetGUID() ? "the leader, " : "") + WhoIs(bot, ai, m);
                const std::vector<uint32_t> theirs = QuestIds(m);
                std::vector<uint32_t> shared, lacking;
                for (uint32_t id : mine)
                    (std::find(theirs.begin(), theirs.end(), id) != theirs.end() ? shared : lacking).push_back(id);
                if (!shared.empty())
                    line += ", has the same quests " + IdList(shared);
                if (!lacking.empty())
                    line += ", does not have " + IdList(lacking);
                auto it = g_rows.find(m->GetGUID().GetRawValue());
                if (it != g_rows.end() && it->second.enrolled && !it->second.doing.empty())
                    line += ", doing: " + it->second.doing;
                members += (members.empty() ? "" : "; ") + line;
            }
            if (members.empty())
                out = "a group with nobody else in it.";
            else if (leads)
                out = "they lead it; members follow them and fight alongside, so plan for all of them. "
                      "Members: " + members + ".";
            else
                out = "they follow its leader. Members: " + members + ".";
        }
        if (Player* inviter = AutopilotGroup_Inviter(bot))
            out += "\nInvite waiting: " + WhoIs(bot, ai, inviter) +
                   " invited them to a group (group accept or group decline).";
        return out;
    }

    // Players and bots of their faction close by, outside their group.
    std::string DescribePeople(Player* bot, PlayerbotAI* ai)
    {
        std::vector<Player*> nearby;
        Acore::AnyPlayerInObjectRangeCheck check(bot, 60.0f, true, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, nearby, check);
        Cell::VisitObjects(bot, searcher, 60.0f);
        std::sort(nearby.begin(), nearby.end(),
                  [bot](Player* a, Player* b) { return bot->GetDistance(a) < bot->GetDistance(b); });
        Group* mine = bot->GetGroup();
        std::string out;
        size_t shown = 0;
        for (Player* p : nearby)
        {
            if (p == bot || p->GetTeamId() != bot->GetTeamId() || (mine && p->GetGroup() == mine))
                continue;
            if (++shown > 6)
                break;
            std::string line = WhoIs(bot, ai, p);
            line.insert(line.size() - 1, p->GetGroup() ? ", in a group" : ", alone");
            out += (out.empty() ? "" : "; ") + line;
        }
        return out;
    }

    // The last whispers, both ways, from the last quarter of an hour.
    std::string DescribeWhispers(const Online& ob, uint32_t now)
    {
        std::string out;
        for (const Online::Whisper& w : ob.whispers)
        {
            if (now - w.at > 15 * 60)
                continue;
            std::string text = w.text;
            std::replace(text.begin(), text.end(), '{', '(');
            std::replace(text.begin(), text.end(), '}', ')');
            out += SafeFormat("\n- {} {} ({} ago): \"{}\"", w.fromThem ? "from" : "to", w.who, Span(now - w.at), text);
        }
        return out;
    }

    // "- [783] A Threat Within (level 1): ready to turn in"
    std::string DescribeQuestLog(Player* bot)
    {
        std::string out;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            const uint32 id = bot->GetQuestSlotQuestId(slot);
            Quest const* q = id ? sObjectMgr->GetQuestTemplate(id) : nullptr;
            if (!q)
                continue;
            const QuestStatus status = bot->GetQuestStatus(id);
            const char* state = status == QUEST_STATUS_COMPLETE ? "ready to turn in"
                              : status == QUEST_STATUS_FAILED   ? "failed"
                                                                : "in progress";
            out += SafeFormat("{}- [{}] {} (level {}): {}", out.empty() ? "" : "\n", id, q->GetTitle(),
                              q->GetQuestLevel() > 0 ? q->GetQuestLevel() : int32(bot->GetLevel()), state);

            if (!bot->SatisfyQuestClass(q, false) || !bot->SatisfyQuestRace(q, false))
            {
                out += " - NOT FOR THEIR CLASS OR RACE: it cannot be finished; abandon it";
                continue;
            }
            if (status == QUEST_STATUS_COMPLETE)
                out += " - turn in to " + DescribeQuestEnder(bot, id);

            // The choice a player makes at the quest giver; theirs to make.
            if (status == QUEST_STATUS_COMPLETE && q->GetRewChoiceItemsCount() > 1)
            {
                out += SafeFormat("\n  rewards to choose from (quest {} reward <n>):", id);
                for (uint32_t i = 0; i < q->GetRewChoiceItemsCount(); ++i)
                    if (ItemTemplate const* item = sObjectMgr->GetItemTemplate(q->RewardChoiceItemId[i]))
                        out += SafeFormat(" {}) {};", i + 1, DescribeRewardItem(bot, item));
            }
        }
        return out;
    }

    AutopilotPromptContext BuildPromptContext(Player* bot, PlayerbotAI* ai, const Row& row,
                                              const Online& ob, Tier tier, uint32_t now)
    {
        AutopilotPromptContext ctx;
        ctx.botName = bot->GetName();
        ctx.level   = bot->GetLevel();
        ctx.race    = ai->GetChatHelper()->FormatRace(bot->getRace());
        ctx.gender  = bot->getGender() == GENDER_FEMALE ? "female" : "male";
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

        ctx.commandReference = AutopilotCommands_Reference();
        ctx.strategiesOn     = DescribeLiveStrategies(ai);
        ctx.activity         = DescribeActivity(bot, ob);
        if (ob.errand.active)
            ctx.errand = SafeFormat("(for {})", Span(now - ob.errand.startedAt));
        if (!ob.errandQueue.empty())
        {
            std::string queued;
            for (const std::string& q : ob.errandQueue)
                queued += (queued.empty() ? "" : "; ") + q;
            ctx.errand += (ctx.errand.empty() ? "" : " ") + std::string("then, in order: ") + queued;
        }
        ctx.questLog    = DescribeQuestLog(bot);
        ctx.services    = AutopilotWorld_DescribeServices(bot);
        ctx.zones       = AutopilotWorld_ZonesForLevel(bot);
        ctx.lastResults = row.lastResults;

        const Situation sit = Classify(bot);
        ctx.concerns = sit.Limits();
        if (!ob.lastAlert.empty() && now - ob.lastAlertAt < kHour && AlertStillTrue(bot, ob, ob.lastAlertKind))
            ctx.concerns += SafeFormat("{}Worry ({}): {}.", ctx.concerns.empty() ? "" : "\n",
                                       Ago(ob.lastAlertAt, now), ob.lastAlert);
        if (ob.reviveHeld)
            ctx.concerns += SafeFormat("{}They are dead, running back to their body as a ghost.",
                                       ctx.concerns.empty() ? "" : "\n");

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

        // The full surroundings scan is for bots someone can see. A choice for
        // an unwatched bot needs only the macro state.
        const ProgressSnapshot& cur = window.back();
        ctx.state = SafeFormat(
            "Now: in {}, {} gold, gear at {}% durability, {} free bag slots, {} quests in the log.",
            Progress_ZoneName(cur.zoneId), cur.money / 10000, uint32_t(cur.durabilityPct),
            uint32_t(cur.freeBagSlots), uint32_t(cur.questsActive));
        if (!cur.professions.empty())
            ctx.state += " Professions: " + Progress_DescribeProfessions(cur.professions) + ".";
        ctx.state += SafeFormat(" Free primary profession slots: {}.", bot->GetFreePrimaryProfessionPoints());
        if (bot->GetLevel() >= 10)
        {
            std::string specs;
            for (int specNo = 0; specNo < MAX_SPECNO; ++specNo)
            {
                const std::string& name = sPlayerbotAIConfig.premadeSpecName[bot->getClass()][specNo];
                if (name.empty())
                    break;
                specs += (specs.empty() ? "" : ", ") + name;
            }
            ctx.state += SafeFormat(" Unspent talent points: {}.", bot->GetFreeTalentPoints());
            if (!specs.empty())
                ctx.state += " Talent specs (talents spec <name>): " + specs + ".";
        }
        // What a player would notice and be tempted by: what they could make
        // right now, and what is lying around to loot, gather, open or fish.
        if (const std::string craft = AutopilotCommands_DescribeCraftable(bot); !craft.empty())
            ctx.state += " They could craft now, from their own bags (craft <name> [count|all]): " + craft + ".";
        if (const std::string around = AutopilotCommands_DescribeSurroundings(bot); !around.empty())
            ctx.state += "\nAround them: " + around + ".";
        // What a player sees as "!" over heads: the quests they could take.
        if (const std::string offers = AutopilotWorld_QuestsOnOffer(bot, 1200.0f); !offers.empty())
            ctx.state += "\nQuests on offer nearby that they could take now (goto <name> walks up and takes "
                         "them): " + offers + ".";
        else
            ctx.state += "\nNo quests on offer nearby for their level: the zones for their level (listed) "
                         "have quest givers with more.";
        if (g_cfg.groups)
        {
            ctx.state += "\nTheir group: " + DescribeGroup(bot, ai);
            if (const std::string people = DescribePeople(bot, ai); !people.empty())
                ctx.state += "\nPeople nearby, not in their group: " + people + ".";
            if (const std::string w = DescribeWhispers(ob, now); !w.empty())
                ctx.state += "\nWhispers (oldest first):" + w;
        }
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
        // A waiting invite needs an answer whoever is around.
        const bool invited = g_cfg.groups && bot->GetGroupInvite();
        if (ob.tier == Tier::Dormant && !force && !invited)
            return false;

        // The tier's interval is a minimum gap between plans for one bot; the
        // model's own `minutes` says when it wants to be asked again. Events
        // (a level, a goal done) may ask sooner, but never more than once per
        // kUrgentGapSeconds, so a burst of events is one plan, not several.
        const uint32_t sinceLast = row.lastPlanAt ? now - row.lastPlanAt : UINT32_MAX;
        const uint32_t gap = (ob.tier == Tier::Foreground ? g_cfg.decisionIntervalMinutes
                                                          : g_cfg.backgroundMinutes) * 60;
        // Near a player (foreground), the model's own `minutes` is honoured;
        // further away the tier's interval is the floor, to spare the budget.
        const bool asked = ob.tier == Tier::Foreground && row.planUntil && now >= row.planUntil &&
                           sinceLast >= kUrgentGapSeconds;
        const bool allowed = force || sinceLast >= gap || asked || ((ob.urgentPlan || invited) && sinceLast >= kUrgentGapSeconds) ||
                             (ob.quickPlan && sinceLast >= g_cfg.quickReplanSeconds);
        if (!allowed)
            return false;

        // Cheap checks before the prompt is built: the budget, and room in
        // the shared request queue (chat comes first).
        if (!AutopilotPlanner_CanSubmit(force || ob.tier == Tier::Foreground) ||
            !OllamaDispatch_BackgroundHasRoom())
            return false;

        std::string prompt = AutopilotPlanner_BuildPrompt(BuildPromptContext(bot, ai, row, ob, ob.tier, now),
                                                          g_cfg.promptTemplate);
        if (!AutopilotPlanner_Submit(guid, ob.planSeq + 1, std::move(prompt)))
            return false;

        ob.planPending     = true;
        ++ob.planSeq;
        ob.planSubmittedAt = now;
        ob.urgentPlan      = false;
        ob.quickPlan       = false;

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
            // A reply to a plan that timed out, after another was sent: the
            // newer one is still pending, and this one is out of date.
            if (d.seq && d.seq != ob->planSeq)
                return;
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

        // Goal: resolved against the live bot so progress can be measured.
        std::string goalRefused;
        if (bot && !d.goalKind.empty())
        {
            AutopilotGoal goal;
            const std::string why = Goal_Resolve(bot, d.goalKind, d.goalTarget, d.goalText, Counters(row), goal);
            if (!why.empty())
            {
                if (g_cfg.debug)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: goal for {} not taken: {}", name, why);
                goalRefused = "goal " + d.goalKind + (d.goalTarget.empty() ? "" : " " + d.goalTarget) +
                              " -> not taken: " + why;
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

        const uint32_t minutes = d.minutes ? std::clamp<uint32_t>(d.minutes, 5, 180) : g_cfg.defaultPlanMinutes;
        row.planUntil = now + minutes * 60;

        // The orders. Only a bot that is online, and only while the model is
        // allowed to give them; otherwise they are recorded as not carried out
        // so the next prompt says so.
        std::vector<std::string> results;

        // "Carry on" ([]): the results shown are an earlier plan's; say so,
        // or they read as what the latest orders did.
        static const std::string kEarlier = "(the last plan gave no new orders; these are from an earlier one)";
        if (d.commands.empty() && !row.lastResults.empty() && row.lastResults.front() != kEarlier)
        {
            row.lastResults.insert(row.lastResults.begin(), kEarlier);
            row.dirty = true;
        }

        if (ob && bot && ai && g_cfg.control)
        {
            const Situation sit = Classify(bot);
            results = RunCommands(bot, ai, row, *ob, d.commands, sit, now);

            // An order failed and the bot has no trip: it would stand there
            // until the plan's minutes run out. Let the model try something
            // else soon (it sees what failed in the results). Not for orders
            // the situation refused (a follower, a dungeon, dead): asking
            // again would only be refused again, once a minute.
            if (!ob->errand.active && sit.CanUseNonCombat() &&
                std::any_of(results.begin(), results.end(), [](const std::string& r)
                {
                    const size_t arrow = r.find(" -> ");
                    const std::string what = arrow == std::string::npos ? r : r.substr(arrow + 4);
                    return what.rfind("done", 0) != 0 && what != "sent" && what.rfind("on the way", 0) != 0 &&
                           what.rfind("queued", 0) != 0 && what.rfind("not carried out", 0) != 0;
                }))
                ob->quickPlan = true;
        }
        else if (!d.commands.empty())
        {
            for (const std::string& c : d.commands)
                results.push_back(c + " -> not carried out (" + (g_cfg.control ? "offline" : "control is off") + ")");
            row.lastResults = results;
        }
        // A goal it gave that was not taken: say so, or it repeats it.
        if (!goalRefused.empty())
        {
            results.push_back(goalRefused);
            row.lastResults = results;
            row.dirty       = true;
        }

        std::string orders;
        for (const std::string& c : d.commands)
            orders += (orders.empty() ? "" : "; ") + c;
        const std::string summary = SafeFormat(
            "{} for {}m{}: {}", row.doing.empty() ? "?" : row.doing, minutes,
            orders.empty() ? "" : " [" + orders + "]", d.reason.empty() ? "-" : d.reason);
        RecordEvent(d.botGuid, "plan", summary);
        for (const std::string& r : results)
            RecordEvent(d.botGuid, "order", r);

        if (g_cfg.debug)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: {} -> {}", name, summary);
            for (const std::string& r : results)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Autopilot: {}   {}", name, r);
        }

        if (ob)
            PushCapped(ob->decisions, summary, kDecisionRing);
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

                std::lock_guard<std::recursive_mutex> lock(g_mutex);
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

                std::lock_guard<std::recursive_mutex> lock(g_mutex);
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

            // Login, a reload or a re-enrollment: put the model's choices back.
            ob.ncReplay = true;
            ob.coReplay = true;

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

        // Hand the bot back to playerbots exactly as it was before the model.
        // Its choices are kept in the row: enrolling it again resumes them.
        if (it != g_rows.end() && (!it->second.baseline.empty() || ob.errand.active || ob.reviveHeld || !bot->IsAlive()))
        {
            HandBack(ai, &it->second, &ob);
            it->second.baseline.clear();
            it->second.dirty = true;
        }
        ob.planPending = false;

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

    // A trip the model sent the bot on: keep it going, and do the job on
    // arrival. Finishing or failing is the moment to ask the model what next
    // -- soon, since without orders the bot stands idle.
    struct LootableBodyCheck
    {
        Player* bot;
        float   range;
        bool operator()(Creature* c) const
        {
            return c && !c->IsAlive() && bot->IsWithinDistInMap(c, range) && bot->isAllowedToLoot(c) &&
                   !c->loot.isLooted();
        }
    };

    // Leading a group: wait for a member who fell behind, as a player looks
    // back for the party. A while at a time, so one stuck member cannot hold
    // the group for good. Members much further off are not waited for.
    bool WaitForGroup(Player* bot, PlayerbotAI* ai, Online& ob, uint32_t now)
    {
        if (now < ob.groupWaitOffUntil || AutopilotGroup_Straggler(bot, 30.0f, 150.0f) <= 0.0f)
        {
            ob.groupWaitSince = 0;
            return false;
        }
        if (!ob.groupWaitSince)
            ob.groupWaitSince = now;
        if (now - ob.groupWaitSince > 25)
        {
            ob.groupWaitSince    = 0;
            ob.groupWaitOffUntil = now + 60;
            return false;
        }
        if (bot->isMoving())
            AutopilotMove_Stop(ai);
        return true;
    }

    // Loot the nearest body the bot may loot, the way the client does: walk up,
    // open it, take the coin and every item it can carry, close it. Each body is
    // tried once (full bags would leave it unlooted for good). True while busy.
    bool LootBodies(Player* bot, PlayerbotAI* ai, Online& ob)
    {
        std::list<Creature*> bodies;
        LootableBodyCheck check{ bot, 25.0f };
        Acore::CreatureListSearcher<LootableBodyCheck> searcher(bot, bodies, check);
        Cell::VisitObjects(bot, searcher, 25.0f);

        Creature* body = nullptr;
        float best = 0.0f;
        for (Creature* c : bodies)
        {
            if (std::find(ob.lootTried.begin(), ob.lootTried.end(), c->GetGUID().GetRawValue()) != ob.lootTried.end())
                continue;
            const float d = bot->GetDistance(c);
            if (!body || d < best)
            {
                body = c;
                best = d;
            }
        }
        if (!body)
            return false;

        if (best > 3.0f)
        {
            if (!AutopilotMove_IsMoving(ai))
                AutopilotMove_To(ai, body->GetPositionX(), body->GetPositionY(), body->GetPositionZ(), true);
            return true;
        }

        AutopilotMove_Stop(ai);
        const ObjectGuid guid = body->GetGUID();
        PushCapped(ob.lootTried, guid.GetRawValue(), size_t(32));
        WorldSession* session = bot->GetSession();

        WorldPacket open(CMSG_LOOT, 8);
        open << guid;
        session->HandleLootOpcode(open);
        if (bot->GetLootGUID() != guid)
            return true;   // refused (too far, not theirs): tried, move on next visit

        if (body->loot.gold)
        {
            WorldPacket money(CMSG_LOOT_MONEY, 0);
            session->HandleLootMoneyOpcode(money);
        }
        const uint32 slots = body->loot.GetMaxSlotInLootFor(bot);
        for (uint32 slot = 0; slot < slots; ++slot)
        {
            WorldPacket take(CMSG_AUTOSTORE_LOOT_ITEM, 1);
            take << uint8(slot);
            session->HandleAutostoreLootItemOpcode(take);
        }
        WorldPacket release(CMSG_LOOT_RELEASE, 8);
        release << guid;
        session->HandleLootReleaseOpcode(release);
        return true;
    }

    void StepErrand(Player* bot, PlayerbotAI* ai, uint64_t guid, Row& row, Online& ob, uint32_t now)
    {
        const AutopilotErrandUpdate u = AutopilotCommands_UpdateErrand(bot, ai, ob.errand, now);
        if (!u.note.empty())
            RecordEvent(guid, u.finished ? "errand" : "travel", u.note);
        if (u.finished)
        {
            ++(u.note.rfind("never reached", 0) == 0 ? g_statFailed : g_statArrived);
            row.lastResults.push_back(u.note);
            if (row.lastResults.size() > AUTOPILOT_MAX_COMMANDS + 2)
                row.lastResults.erase(row.lastResults.begin());
            row.dirty = true;

            // The model's next order in line, if any; ask it again only once
            // they are all done (or none of the rest could start).
            while (!ob.errand.active && !ob.errandQueue.empty())
            {
                const std::string next = ob.errandQueue.front();
                ob.errandQueue.pop_front();
                const std::string result = next + " -> " + AutopilotCommands_Run(bot, ai, next, ob.errand, now);
                RecordEvent(guid, "order", result);
                row.lastResults.push_back(result);
                if (row.lastResults.size() > AUTOPILOT_MAX_COMMANDS + 2)
                    row.lastResults.erase(row.lastResults.begin());
            }
            if (!ob.errand.active)
                ob.quickPlan = true;
        }

        if (ob.errand.active && AutopilotTravel_IsTimeCritical(ob.errand.trip))
            g_aboard.insert(guid);
        else
            g_aboard.erase(guid);

        if (ob.errand.active)
            g_walking.insert(guid);
        else
            g_walking.erase(guid);
        AutopilotStrategy_SetTravelling(guid, ob.errand.active);
    }

    void Control(Player* bot, PlayerbotAI* ai, uint64_t guid, Row& row, Online& ob, uint32_t now)
    {
        const Situation sit = Classify(bot);

        // Cheap (real players online only), and keeps status honest even for a
        // bot that is not being planned for right now (dead, say).
        ob.tier = ComputeTier(bot);

        if (!ob.historyRequested)
            RequestHistory(guid, ob);

        // Teleport holds run while dead too: that is when the revive teleport
        // would happen.
        HoldTeleports(bot, guid, ob, sit, now);
        // Grind holds only while the bot is on its own and on a trip; never
        // in a group, a dungeon, or after the errand ended some other way.
        AutopilotStrategy_SetTravelling(guid, g_cfg.control && sit.CanUseNonCombat() && ob.errand.active);

        // Five deaths and playerbots revives the bot through its re-roll.
        if (g_cfg.control && g_cfg.noHandouts)
            AutopilotBot_ClearDeathCount(ai);
        if (sit.dead)
        {
            ob.errand.trip.interrupted = true;
            ob.lastBusyAt = now;   // dead is not idle
            RunCorpse(bot, ai, guid, ob, sit, now);
            return;
        }
        if (ob.corpseTrip.active)
            ob.corpseTrip = AutopilotTrip();   // alive again
        ob.corpseRetryAt = 0;

        // Busy: on a trip, in a fight, or moving under its own strategies --
        // or not its own to direct (a real player's group, a follower, an
        // instance), which is not standing idle either.
        if (!ob.lastBusyAt || ob.errand.active || sit.inCombat || bot->isMoving() || !sit.CanUseNonCombat())
            ob.lastBusyAt = now;

        CheckBoundaries(bot, guid, row, ob, sit, now);
        UpdateFacts(bot, ob, now);
        CheckGoal(bot, guid, row, ob, now);
        CheckAlerts(bot, guid, ob, now);

        // Put back what a playerbots reset wiped; keep out-of-combat
        // strategies the bot's own only while it is.
        Reassert(ai, row, ob, sit);

        if (g_cfg.control && !sit.inCombat)
            DiscoverFlightPoint(bot, guid, ob, now);

        // Attacked: get out of the way of playerbots' combat AI immediately.
        // After the fight, give it a few seconds (loot, a heal), and never
        // walk off while the bot sits to eat or drink.
        const bool threatened = sit.inCombat || !bot->getAttackers().empty();

        // Casting (a pet summoned, a buff renewed): let it finish, as a player
        // would, and give the next one in a chain a moment to start. A move
        // order now would cancel it, and playerbots never starts a spell with
        // a cast time while the bot is moving -- so its upkeep happens only in
        // the pauses the walk leaves it.
        if (bot->IsNonMeleeSpellCast(false) || bot->HasUnitState(UNIT_STATE_CASTING))
            ob.castHoldUntil = now + 2;

        // The walk hands on the next route point before the bot reaches the
        // last, so it never stands still on its own. Every half minute of
        // walking, stop for a couple of seconds -- the moment a player takes
        // to summon a pet or renew a buff -- and wait out any cast it starts.
        // Only on foot on open ground, never at a dock or aboard.
        if (ob.errand.active && bot->isMoving() && !AutopilotTravel_IsTimeCritical(ob.errand.trip))
        {
            if (!ob.walkingSince)
                ob.walkingSince = now;
            else if (now - ob.walkingSince >= 30)
            {
                AutopilotMove_Stop(ai);
                ob.castHoldUntil = now + 2;
                ob.walkingSince  = 0;
            }
        }
        else if (!bot->isMoving())
            ob.walkingSince = 0;

        if (threatened)
        {
            ob.lastCombatAt = now;
            AutopilotCommands_HuntUnderAttack(bot, ai, ob.errand);
            AutopilotMove_Yield(ai);
            ob.errand.trip.interrupted = true;
        }
        else if (g_cfg.control && sit.CanUseNonCombat() && !bot->IsSitState() && now >= ob.castHoldUntil)
        {
            // Loot the kill before moving on, as a player does -- quest items
            // drop on bodies. Then the errand (a quest hunt picks its next
            // target, a trip walks on).
            if (now - ob.lastCombatAt >= 1 && LootBodies(bot, ai, ob))
                ob.lastBusyAt = now;
            else if (sit.leads && ob.errand.active && !AutopilotTravel_IsTimeCritical(ob.errand.trip) &&
                     WaitForGroup(bot, ai, ob, now))
            {
                ob.lastBusyAt = now;
                ob.errand.trip.interrupted = true;   // waiting is not being stuck
            }
            else if (now - ob.lastCombatAt >= 4)
                StepErrand(bot, ai, guid, row, ob, now);
        }

        // A plan that never came back (provider down, server restarted the
        // dispatcher) must not block the next one forever.
        if (ob.planPending && now - ob.planSubmittedAt > g_cfg.planTimeoutSeconds)
            ob.planPending = false;

        if (!g_cfg.control || !sit.CanUseCombat() || sit.inFlight)
            return;

        // Ask the model when it wanted to be asked again, when something
        // happened that warrants it, or when the bot has no identity yet.
        // In a group or a dungeon it is still asked -- only its combat orders
        // are carried out there. When it cannot be asked (no budget, nobody
        // around), the bot keeps doing what it was last told.
        const bool due = !row.HasIdentity() || ob.urgentPlan || ob.quickPlan || now >= row.planUntil;
        if (due)
            TrySubmitPlan(bot, ai, guid, row, ob, now, false);
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
                    std::lock_guard<std::recursive_mutex> lock(g_mutex);
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
                auto row = g_rows.find(guid);
                if (row != g_rows.end() && (!row->second.baseline.empty() || ob.errand.active || ob.reviveHeld || !ai->GetBot()->IsAlive()))
                {
                    HandBack(ai, &row->second, &ob);
                    row->second.baseline.clear();
                    row->second.dirty = true;
                }
                RemoveMarker(ai);
            }
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

    // Playerbots' level brackets (RandomBotLevelMgr) move random bots to
    // another level by re-rolling them, but leave alone any bot on someone's
    // friend list (character_social, flags 1, with
    // AiPlayerbot.LevelBrackets.IgnoreFriendListed on). A row owned by guid 0 -- no character, so nobody's list shows
    // it -- with our note puts an enrolled bot there without touching those
    // modules.
    constexpr const char* kSocialNote = "ollama autopilot";

    void SyncBracketShield(const std::vector<uint32_t>& add, const std::vector<uint32_t>& remove)
    {
        auto join = [](const std::vector<uint32_t>& lows, bool tuples)
        {
            std::string out;
            for (uint32_t low : lows)
            {
                if (!out.empty())
                    out += ',';
                out += tuples ? SafeFormat("(0, {}, 1, '{}')", low, kSocialNote) : std::to_string(low);
            }
            return out;
        };
        if (!add.empty())
            CharacterDatabase.Execute("INSERT IGNORE INTO character_social (guid, friend, flags, note) VALUES " +
                                      join(add, true));
        if (!remove.empty())
            CharacterDatabase.Execute(SafeFormat("DELETE FROM character_social WHERE guid = 0 AND note = '{}' "
                                                 "AND friend IN ({})", kSocialNote, join(remove, false)));
    }

    void SaveRowsLocked()
    {
        std::vector<uint32_t> shield, unshield;
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
                "quests_total, dungeons_total, doing, doing_since, decided_by, strategies, last_results, "
                "goal_kind, goal_target, goal_target_id, goal_value, goal_baseline, goal_set_at, goal_text, "
                "last_reason, baseline, plan_until, last_plan_at, enrolled_at, updated_at) VALUES " + values +
                " ON DUPLICATE KEY UPDATE mode = VALUES(mode), enrolled = VALUES(enrolled), "
                "source = VALUES(source), style = VALUES(style), outlook = VALUES(outlook), "
                "profile = VALUES(profile), kills_total = VALUES(kills_total), "
                "deaths_total = VALUES(deaths_total), quests_total = VALUES(quests_total), "
                "dungeons_total = VALUES(dungeons_total), doing = VALUES(doing), "
                "doing_since = VALUES(doing_since), decided_by = VALUES(decided_by), "
                "strategies = VALUES(strategies), last_results = VALUES(last_results), "
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
                "({}, {}, {}, '{}', '{}', '{}', '{}', {}, {}, {}, {}, '{}', {}, '{}', '{}', '{}', "
                "'{}', '{}', {}, {}, {}, {}, '{}', '{}', '{}', {}, {}, {}, {})",
                guid, uint32_t(row.mode), row.enrolled ? 1 : 0, esc(row.source, 32),
                esc(row.style, 64), esc(row.outlook, 32), esc(row.profile, 1000),
                row.killsTotal, row.deathsTotal, row.questsTotal, row.dungeonsTotal,
                esc(row.doing, 64), row.doingSince, esc(row.decidedBy, 16),
                esc(FormatStrategies(row.strategies), 2000), esc(JoinLines(row.lastResults), 2000),
                g.Active() ? Goal_KindName(g.kind) : "", esc(g.target, 64), g.targetId, g.value,
                g.baseline, g.setAt, esc(g.text, 255), esc(row.lastReason, 255),
                esc(row.baseline, 4000), row.planUntil, row.lastPlanAt,
                row.enrolledAt, now);
            row.dirty = false;
            (row.enrolled && g_cfg.noRandomize ? shield : unshield).push_back(ObjectGuid(guid).GetCounter());

            if (++count >= 200)
                flush();
        }
        flush();
        SyncBracketShield(shield, unshield);
    }

    // Dungeon-finder proposals seen for enrolled bots, answered on the world
    // tick: bot guid -> proposal id. Filled from OnPacketSent (any thread).
    std::mutex                             g_lfgMutex;
    std::unordered_map<uint64_t, uint32_t> g_lfgPending;
    std::unordered_map<uint64_t, uint32_t> g_lfgAnswered;   // world thread only

    void AnswerLfgProposals()
    {
        std::unordered_map<uint64_t, uint32_t> pending;
        {
            std::lock_guard<std::mutex> lock(g_lfgMutex);
            pending.swap(g_lfgPending);
        }
        for (auto const& [guid, id] : pending)
        {
            if (g_lfgAnswered[guid] == id)
                continue;   // the proposal is updated as others answer; one reply each
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(guid));
            if (!bot || !bot->IsInWorld() || !bot->GetSession())
                continue;
            g_lfgAnswered[guid] = id;
            // As playerbots answers: yes, unless fighting or dead.
            const bool accept = bot->IsAlive() && !bot->IsInCombat();
            WorldPacket* packet = new WorldPacket(CMSG_LFG_PROPOSAL_RESULT, 5);
            *packet << id << accept;
            bot->GetSession()->QueuePacket(packet);
            std::lock_guard<std::recursive_mutex> lock(g_mutex);   // RecordEvent's rule
            RecordEvent(guid, "dungeon", accept ? "accepted a dungeon finder group"
                                                : "declined a dungeon finder group (in a fight or dead)");
        }
    }

    // Shared shape of every progress hook: find the enrolled row, under lock.
    template <typename Fn>
    void WithEnrolledRow(Player* player, Fn&& fn)
    {
        if (!player || !Autopilot_IsActive())
            return;

        const uint64_t guid = player->GetGUID().GetRawValue();
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
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
        std::lock_guard<std::recursive_mutex> lock(g_mutex);

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
        for (const std::string& why : Autopilot_InactiveReasons())
            handler->SendSysMessage("  - " + why);
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
            std::lock_guard<std::recursive_mutex> lock(g_mutex);

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
                "  plans applied {} | unusable replies {} | orders run {}, refused {} | alerts {} | "
                "strategies put back after resets {} | trips arrived {}, failed {} | corpse runs {}",
                g_statPlans, g_statInvalid, g_statCommands, g_statRefused, g_statAlerts, g_statReplays,
                g_statArrived, g_statFailed, g_statCorpse));
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
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
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
        handler->SendSysMessage("  model's strategies: " +
                                (row.strategies.empty() ? std::string("-") : FormatStrategies(row.strategies)));
        if (ai)
        {
            handler->SendSysMessage("  live: " + DescribeLiveStrategies(ai));
            handler->SendSysMessage("  now: " + DescribeActivity(bot, ob) +
                                    (ob.reviveHeld ? " | corpse run" : ""));

            // What fighting looks like from the server's side: whether the
            // core counts the bot in combat, who is attacking it, and which
            // of playerbots' engines is running.
            const BotState state = ai->GetState();
            std::string attackers;
            for (Unit* u : bot->getAttackers())
                if (u)
                    attackers += (attackers.empty() ? "" : ", ") + u->GetName();
            handler->SendSysMessage(SafeFormat(
                "  combat: {} | attacked by: {} | target: {} | playerbots engine: {} | moving: {}",
                bot->IsInCombat() ? "yes" : "no", attackers.empty() ? std::string("nobody") : attackers,
                bot->GetVictim() ? bot->GetVictim()->GetName() : std::string("none"),
                state == BOT_STATE_COMBAT ? "combat" : state == BOT_STATE_DEAD ? "dead" : "non-combat",
                bot->isMoving() ? "yes" : "no"));
        }
        for (const std::string& r : row.lastResults)
            handler->SendSysMessage("  order: " + r);
        handler->SendSysMessage("  goal: " + (row.goal.Active() ? Goal_Describe(bot, row.goal, Counters(row))
                                                                : std::string("-")));
        handler->SendSysMessage("  last reason: " + (row.lastReason.empty() ? std::string("-") : row.lastReason));
        const std::string rewards = RewardSummary(ob, now);
        handler->SendSysMessage("  last hour: " + (rewards.empty() ? std::string("nothing rewarding") : rewards));
        for (const auto& [at, text] : ob.temptations)
            if (now - at <= kHour)
                handler->SendSysMessage(SafeFormat("  tempted ({}): {}", Ago(at, now), text));
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

        std::lock_guard<std::recursive_mutex> lock(g_mutex);
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
            tiers[1] * 60 / std::max<uint32_t>(1, g_cfg.backgroundMinutes);
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

        std::lock_guard<std::recursive_mutex> lock(g_mutex);
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

        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        Row*    row = nullptr;
        Online* ob  = nullptr;
        if (!ControlTarget(handler, bot, row, ob))
            return true;

        PlayerbotAI* ai = BotAI(bot);
        if (!Classify(bot).CanUseCombat())
        {
            ob->urgentPlan = true;
            handler->SendSysMessage(SafeFormat(
                "OllamaChat: {} is dead or in a hands-off group; it will replan when that ends.", bot->GetName()));
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

        std::lock_guard<std::recursive_mutex> lock(g_mutex);
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
    c.maxLevel          = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Select.MaxLevel", 80);
    c.maxEnrolled       = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.MaxEnrolled", 0);
    c.guilds            = ParseIds(sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.Select.Guilds", ""));
    c.accounts          = ParseIds(sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.Select.Accounts", ""));
    c.includeNames      = ParseNames(sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.Select.Include", ""));
    c.excludeNames      = ParseNames(sConfigMgr->GetOption<std::string>("OllamaChat.Autopilot.Select.Exclude", ""));

    c.sweepIntervalMs         = std::max<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.SweepIntervalMs", 1000));
    c.botsPerSweep            = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.BotsPerSweep", 25));
    c.snapshotIntervalMinutes = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.SnapshotIntervalMinutes", 10));
    c.flushIntervalSeconds    = std::max<uint32_t>(5, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.FlushIntervalSeconds", 60));
    c.snapshotRetention       = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.SnapshotRetention", 500);
    c.eventRetention          = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.EventRetention", 300);

    c.control                 = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.Control", true);
    c.withRealPlayer          = std::min<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.WithRealPlayer", 1));
    c.noTeleport              = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.NoTeleport", true);
    c.noRandomize             = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.NoRandomize", true);
    c.noHandouts              = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.NoHandouts", true);
    c.keepOnline              = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.KeepOnline", true);
    c.corpseRunMinutes        = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.CorpseRunMinutes", 10));
    c.llmEnable               = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.Llm.Enable", true);
    c.llmCallsPerHour         = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.LlmCallsPerHour", 300);
    c.maxConcurrentPlans      = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.MaxConcurrentPlans", 2));
    c.decisionIntervalMinutes = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.DecisionIntervalMinutes", 15));
    // GoalRefreshMinutes is the old name, from when the LLM only revisited
    // goals; still honoured if a conf sets it and not the new one.
    c.backgroundMinutes       = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>(
        "OllamaChat.Autopilot.BackgroundIntervalMinutes",
        sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.GoalRefreshMinutes", 30, false)));
    c.defaultPlanMinutes      = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.DefaultPlanMinutes", 30), 5, 180);
    c.quickReplanSeconds      = std::max<uint32_t>(10, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.QuickReplanSeconds", 60));
    c.foregroundRange         = sConfigMgr->GetOption<float>("OllamaChat.Autopilot.ForegroundRange", 100.0f);
    c.planTimeoutSeconds      = std::max<uint32_t>(30, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.PlanTimeoutSeconds", 300));
    c.groups                  = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.Groups", true);
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
    c.requireRealPlayer       = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.RequireRealPlayer", true);
    c.minimumTier             = parseTier("OllamaChat.Autopilot.MinimumTier", "dormant", Tier::Dormant);
    c.selectRealPlayerGuilds  = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.Select.RealPlayerGuilds", false);
    c.realGuildTier           = parseTier("OllamaChat.Autopilot.RealPlayerGuildTier", "background", Tier::Background);
    c.realGuildRefreshMinutes = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.RealPlayerGuildRefreshMinutes", 10));

    c.alertDeaths             = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Alert.Deaths", 3);
    c.alertDeathWindowMinutes = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Alert.DeathWindowMinutes", 15));
    c.alertDurabilityPct      = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Alert.DurabilityPct", 20);
    c.alertFreeBagSlots       = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Alert.FreeBagSlots", 2);
    c.alertCooldownMinutes    = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Alert.CooldownMinutes", 30));
    c.alertIdleMinutes        = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Alert.IdleMinutes", 5);

    c.goalStaleMinutes        = sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.GoalStaleMinutes", 120);

    if (c.minLevel > c.maxLevel)
        std::swap(c.minLevel, c.maxLevel);

    AutopilotCommands_Load();
    AutopilotPlanner_ConfigureBudget(c.llmCallsPerHour, c.maxConcurrentPlans);

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_cfg = std::move(c);
    AutopilotStrategy_SetNoHandouts(g_cfg.noHandouts);
    AutopilotStrategy_SetModelGroups(g_cfg.enable && g_cfg.control && g_cfg.groups && g_cfg.llmEnable);

    // Rules may have changed: re-evaluate every online bot on its next visit.
    for (auto& [guid, ob] : g_online)
        ob.evaluated = false;

    // With NoHandouts the whole level-up maintenance, its teleport included,
    // is dropped for enrolled bots by the marker strategy.
    if (g_cfg.enable && g_cfg.noTeleport && !g_cfg.noHandouts && sPlayerbotAIConfig.autoTeleportForLevel)
        LOG_WARN("module.ollamachat",
                 "[Ollama Chat] AiPlayerbot.AutoTeleportForLevel is on: playerbots will still teleport "
                 "autopilot bots on level-up. Set it to 0 for OllamaChat.Autopilot.NoTeleport to cover them.");
}

void Autopilot_Load()
{
    AutopilotStrategy_Register();
    AutopilotWorld_Build();
    AutopilotTravel_Build();

    // Bring the tables to the current layout, whatever draft made them (the
    // core's updater only ever runs the SQL file's CREATE TABLE IF NOT EXISTS).
    std::string problem;
    g_tablesOk = AutopilotSchema_Ensure(problem);
    if (!g_tablesOk)
    {
        LOG_ERROR("server.loading", "[Ollama Chat] Autopilot tables could not be set up: {}. Autopilot stays off.",
                  problem);
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

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_rows.clear();
    g_enrolledCount = 0;
    g_cappedCount   = 0;

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT bot_guid, mode, enrolled, source, style, outlook, profile, kills_total, deaths_total, "
            "quests_total, dungeons_total, doing, doing_since, decided_by, strategies, last_results, "
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
            row.strategies    = ParseStrategies(f[14].Get<std::string>());
            for (const std::string& r : SplitString(f[15].Get<std::string>(), '\n'))
                if (!r.empty())
                    row.lastResults.push_back(r);

            const std::string kind = f[16].Get<std::string>();
            row.goal.kind     = kind.empty() ? GoalKind::None : Goal_KindFromName(kind);
            row.goal.target   = f[17].Get<std::string>();
            row.goal.targetId = f[18].Get<uint32>();
            row.goal.value    = f[19].Get<uint32>();
            row.goal.baseline = f[20].Get<uint32>();
            row.goal.setAt    = f[21].Get<uint32>();
            row.goal.text     = f[22].Get<std::string>();
            row.lastReason    = f[23].Get<std::string>();
            row.enrolledAt    = f[24].Get<uint32>();
            row.baseline      = f[25].Get<std::string>();
            row.planUntil     = f[26].Get<uint32>();
            row.lastPlanAt    = f[27].Get<uint32>();

            if (row.enrolled)
                CountEnrolled(row, +1);
            g_rows.emplace(f[0].Get<uint64>(), std::move(row));
        } while (result->NextRow());
    }

    LOG_INFO("server.loading", "[Ollama Chat] Autopilot: loaded {} rows, {} enrolled.",
             g_rows.size(), g_enrolledCount);

    // Before any bot logs in: playerbots schedules a re-roll and a refresh a
    // few seconds after a random bot logs in if their timers have lapsed (they
    // run on real time, so any downtime past eight hours lapses them), and it
    // picks which random bots to log in afresh at every start. Hold the timers
    // now, and ask for enrolled random bots to be logged in. Random-bot
    // accounts only: an "add" row would have playerbots log an alt in as a
    // random bot.
    if (g_cfg.enable && g_cfg.control)
    {
        uint32_t held = 0;
        for (auto const& [guid, row] : g_rows)
        {
            if (!row.enrolled)
                continue;
            const ObjectGuid og(guid);
            const uint32 account = sCharacterCache->GetCharacterAccountIdByGuid(og);
            if (!account || !sRandomPlayerbotMgr.IsAccountType(account, 1))
                continue;
            const uint32 low = og.GetCounter();
            if (g_cfg.noRandomize)
                sRandomPlayerbotMgr.SetValue(low, "randomize", 1);
            if (g_cfg.noTeleport)
                sRandomPlayerbotMgr.ScheduleTeleport(low, 2 * kHour);
            if (g_cfg.keepOnline)
                sRandomPlayerbotMgr.SetValue(low, "add", 1);
            ++held;
        }
        if (held)
            LOG_INFO("server.loading", "[Ollama Chat] Autopilot: held playerbots' re-roll and teleport for {} enrolled random bots{}.",
                     held, g_cfg.keepOnline ? " and asked for them to be logged in" : "");
    }

    // The level-bracket shield, rebuilt from the table: ours out, enrolled
    // bots back in (if NoRandomize still wants them kept where they are).
    CharacterDatabase.DirectExecute(SafeFormat("DELETE FROM character_social WHERE guid = 0 AND note = '{}'",
                                               kSocialNote));
    if (g_cfg.enable && g_cfg.noRandomize)
    {
        std::vector<uint32_t> shield;
        for (auto const& [guid, row] : g_rows)
            if (row.enrolled)
                shield.push_back(ObjectGuid(guid).GetCounter());
        SyncBracketShield(shield, {});
        if (!shield.empty())
            LOG_INFO("server.loading", "[Ollama Chat] Autopilot: {} enrolled bots kept out of level-bracket re-rolls "
                     "(friend-list rows owned by guid 0).", shield.size());
    }

    // Lands in Autopilot_Update before the first bots log in.
    RefreshRealGuilds();
}

bool Autopilot_IsActive()
{
    return g_Enable && g_cfg.enable && g_tablesOk && AutopilotStrategy_IsRegistered();
}

std::vector<std::string> Autopilot_InactiveReasons()
{
    std::vector<std::string> out;
    if (!g_Enable)
        out.push_back("OllamaChat.Enable is 0");
    if (!g_cfg.enable)
        out.push_back("OllamaChat.Autopilot.Enable is 0");
    if (!g_tablesOk)
        out.push_back("autopilot tables could not be set up at startup (see \"Autopilot tables\" in the server log)");
    if (!AutopilotStrategy_IsRegistered())
        out.push_back("the 'autopilot' playerbots strategy failed to register (see the startup log)");
    return out;
}

void Autopilot_Update(uint32_t diff)
{
    if (!g_tablesOk)
        return;

    // History loads land here, on the world thread. Not under g_mutex: the
    // callbacks take it themselves.
    g_callbacks.ProcessReadyCallbacks();
    AnswerLfgProposals();

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
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        ReleaseAll();
    }
    g_wasActive = active;

    // Finished plans. While inactive they are dropped, not applied.
    std::vector<AutopilotDecision> decisions = AutopilotPlanner_Drain();
    if (!decisions.empty())
    {
        const uint32_t now = Progress_Now();
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
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
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

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

    // Bots on a trip get a cheap look every sweep for attackers, so a walk
    // never runs on while the bot is being hit (the full visit may be seconds
    // away on a busy realm). Only the yield; the trip resumes on a full visit.
    const std::vector<uint64_t> walking(g_walking.begin(), g_walking.end());
    for (uint64_t guid : walking)
    {
        auto on = g_online.find(guid);
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(guid));
        PlayerbotAI* ai = BotAI(bot);
        if (on == g_online.end() || !ai || !bot->IsInWorld() || !on->second.errand.active)
        {
            g_walking.erase(guid);
            continue;
        }
        if (bot->IsInCombat() || !bot->getAttackers().empty())
        {
            on->second.lastCombatAt = now;
            on->second.errand.trip.interrupted = true;
            AutopilotMove_Yield(ai);
        }
    }

    // Bots waiting at a dock or aboard a ship get a look every sweep: a ship
    // is docked for well under a minute, and the rotation can be slower than
    // that on a busy realm. Only the trip is advanced here.
    const std::vector<uint64_t> aboard(g_aboard.begin(), g_aboard.end());
    for (uint64_t guid : aboard)
    {
        auto on  = g_online.find(guid);
        auto row = g_rows.find(guid);
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(guid));
        PlayerbotAI* ai = BotAI(bot);
        if (on == g_online.end() || row == g_rows.end() || !row->second.enrolled || !ai || !bot->IsInWorld() ||
            !bot->IsAlive() || bot->IsInCombat() || !g_cfg.control)
        {
            g_aboard.erase(guid);
            continue;
        }
        StepErrand(bot, ai, guid, row->second, on->second, now);
    }
}

void Autopilot_SaveAll()
{
    if (!g_tablesOk)
        return;

    uint32_t snapshotRetention;
    uint32_t eventRetention;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
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

void Autopilot_NoteWhisper(Player* from, Player* to, const std::string& text)
{
    if (!from || !to || from == to || text.empty() || !Autopilot_IsActive())
        return;
    const uint32_t now = Progress_Now();
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    // Existing entries only: this can run on a map thread.
    auto note = [&](Player* self, Player* other, bool fromThem) -> Online*
    {
        const uint64_t guid = self->GetGUID().GetRawValue();
        auto row = g_rows.find(guid);
        auto on  = g_online.find(guid);
        if (row == g_rows.end() || !row->second.enrolled || on == g_online.end())
            return nullptr;
        Online& ob = on->second;
        ob.whispers.push_back({ other->GetName(), text.substr(0, 200), now, fromThem });
        while (ob.whispers.size() > 8)
            ob.whispers.pop_front();
        return &ob;
    };

    note(from, to, false);
    Online* ob = note(to, from, true);
    if (!ob)
        return;

    // Ask the model soon when it is about grouping, or it answers something
    // the bot asked. Ordinary chat is the chat system's to answer. At most
    // every two minutes, so two bots whispering cannot spin each other.
    const std::string lower = Lower(text);
    bool about = false;
    for (const char* word : { "group", "party", "invite", "inv ", "join" })
        if (lower.find(word) != std::string::npos || lower == "inv")
            about = true;
    bool asked = false;
    for (const Online::Whisper& w : ob->whispers)
        if (!w.fromThem && w.who == from->GetName() && now - w.at <= 600)
            asked = true;
    if (!about && !asked)
        return;
    // A plain field, not an alertCooldown key: this can run on a map thread,
    // which must never insert into module maps.
    if (now < ob->whisperPlanAt)
        return;
    ob->whisperPlanAt = now + 120;
    ob->quickPlan = true;
}

std::string Autopilot_ChatContext(Player* bot)
{
    if (!bot || !Autopilot_IsActive())
        return "";
    const uint64_t guid = bot->GetGUID().GetRawValue();
    const uint32_t now  = Progress_Now();

    std::string doing, goal, heading, queued;
    uint32_t    doingMinutes = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        auto row = g_rows.find(guid);
        if (row == g_rows.end() || !row->second.enrolled)
            return "";
        doing = row->second.doing;
        goal  = row->second.goal.text;
        if (row->second.doingSince && now > row->second.doingSince)
            doingMinutes = (now - row->second.doingSince) / 60;
        if (auto on = g_online.find(guid); on != g_online.end())
        {
            const Online& ob = on->second;
            if (ob.errand.active && !ob.errand.label.empty())
                heading = ob.errand.label;
            if (!ob.errandQueue.empty())
                queued = SafeFormat("{} more thing{} after that", ob.errandQueue.size(),
                                    ob.errandQueue.size() == 1 ? "" : "s");
        }
    }

    std::string out;
    auto line = [&out](const std::string& text) { out += "- " + text + "\n"; };
    if (!doing.empty())
        line(SafeFormat("What you are doing: {}{}", doing,
                        doingMinutes ? SafeFormat(" (for {} minutes now)", doingMinutes) : std::string()));
    if (!goal.empty())
        line("What you are aiming for: " + goal);
    if (!heading.empty())
        line("Where you are headed right now: " + heading + (queued.empty() ? "" : ", then " + queued));

    std::string quests;
    size_t count = 0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE && count < 5; ++slot)
        if (uint32_t id = bot->GetQuestSlotQuestId(slot))
            if (Quest const* q = sObjectMgr->GetQuestTemplate(id))
            {
                const bool ready = bot->GetQuestStatus(id) == QUEST_STATUS_COMPLETE;
                quests += (quests.empty() ? "" : "; ") + q->GetTitle() + (ready ? " (done, to hand in)" : "");
                ++count;
            }
    line(quests.empty() ? std::string("Your quest log: empty") : "Your quests: " + quests);

    if (Group* group = bot->GetGroup())
    {
        Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
        line(group->GetLeaderGUID() == bot->GetGUID()
                 ? SafeFormat("You lead a group of {}", group->GetMembersCount())
                 : "You are in a group led by " + (leader ? leader->GetName() : std::string("someone")));
    }

    if (out.empty())
        return "";
    return "\nWhat you are up to (your own plans: mention them naturally if it fits the conversation, "
           "never as a list, never as orders or commands):\n" + out;
}

bool Autopilot_MonitorCommand(Player* gm, const std::string& sub, const std::string& name)
{
    if (!gm || !gm->GetSession())
        return false;
    ChatHandler handler(gm->GetSession());
    if (sub == "on")     return HandleOn(&handler, name), true;
    if (sub == "off")    return HandleOff(&handler, name), true;
    if (sub == "replan") return HandleReplan(&handler, name), true;
    if (sub == "status") return HandleStatus(&handler, name.empty() ? Optional<std::string>() : Optional<std::string>(name)), true;
    return false;
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
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

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
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    g_realOnline.erase(guid);
    g_aboard.erase(guid);
    g_walking.erase(guid);
    AutopilotStrategy_SetTravelling(guid, false);

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

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
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

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_realGuilds.insert(guild->GetId()).second)
        OnRealGuildsChanged();
}

// ==========================================================================
// Monitor addon pages. World thread (the addon bridge answers from the chat
// handler, which runs in the session update).
// ==========================================================================

namespace
{
    const char* LegTypeName(AutopilotLegType t)
    {
        switch (t)
        {
            case AutopilotLegType::Walk:     return "walk";
            case AutopilotLegType::Fly:      return "fly";
            case AutopilotLegType::Board:    return "board";
            case AutopilotLegType::Ride:     return "ride";
            case AutopilotLegType::Approach: return "approach";
            case AutopilotLegType::Trigger:  return "trigger";
            case AutopilotLegType::Portal:   return "portal";
        }
        return "?";
    }

    const char* ErrandKindName(AutopilotErrandKind k)
    {
        switch (k)
        {
            case AutopilotErrandKind::Place:          return "place";
            case AutopilotErrandKind::Service:        return "service";
            case AutopilotErrandKind::QuestTurnIn:    return "quest turn-in";
            case AutopilotErrandKind::QuestObjective: return "quest objective";
            case AutopilotErrandKind::Craft:          return "craft";
            case AutopilotErrandKind::Open:           return "open";
            case AutopilotErrandKind::Mailbox:        return "mailbox";
            case AutopilotErrandKind::Npc:            return "person";
        }
        return "?";
    }

    const char* MotionName(MovementGeneratorType t)
    {
        switch (t)
        {
            case IDLE_MOTION_TYPE:     return "idle";
            case RANDOM_MOTION_TYPE:   return "random";
            case WAYPOINT_MOTION_TYPE: return "waypoint";
            case CONFUSED_MOTION_TYPE: return "confused";
            case CHASE_MOTION_TYPE:    return "chase";
            case HOME_MOTION_TYPE:     return "home";
            case FLIGHT_MOTION_TYPE:   return "flight";
            case POINT_MOTION_TYPE:    return "point";
            case FLEEING_MOTION_TYPE:  return "fleeing";
            case FOLLOW_MOTION_TYPE:   return "follow";
            case EFFECT_MOTION_TYPE:   return "effect";
            case ESCORT_MOTION_TYPE:   return "escort (autopilot walk)";
            default:                   return "other";
        }
    }

    void Head(std::vector<std::string>& out, const std::string& title) { out.push_back("# " + title); }
    void Kv(std::vector<std::string>& out, const std::string& key, const std::string& value)
    {
        out.push_back(key + ": " + (value.empty() ? std::string("-") : value));
    }
    std::string YesNo(bool b) { return b ? "yes" : "no"; }

    std::string PointText(const AutopilotTravelPoint& p)
    {
        return SafeFormat("map {} ({:.0f}, {:.0f}, {:.0f})", p.map, p.x, p.y, p.z);
    }

    std::string DistanceText(Player* bot, const AutopilotTravelPoint& p)
    {
        if (bot->GetMapId() != p.map)
            return "another map";
        return SafeFormat("{:.0f} yd", bot->GetExactDist(p.x, p.y, p.z));
    }

    // Splits multi-line text (a prompt, a reply) into page lines.
    void TextBlock(std::vector<std::string>& out, const std::string& text)
    {
        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            out.push_back("  " + line);
            start = end + 1;
        }
    }

    void TripLines(Player* bot, const AutopilotTrip& t, uint32_t now, std::vector<std::string>& out)
    {
        Kv(out, "active", YesNo(t.active));
        if (!t.active)
            return;
        Kv(out, "now", AutopilotTravel_Describe(bot, t));
        Kv(out, "destination", PointText(t.dest) + " - " + DistanceText(bot, t.dest) +
                                   SafeFormat(", arrive within {:.0f} yd", t.arriveRadius));
        Kv(out, "plans", SafeFormat("{} (no flights: {}, flew since last crossing: {}, interrupted: {})",
                                    t.plans, YesNo(t.noFlight), YesNo(t.flown), YesNo(t.interrupted)));
        Kv(out, "this leg", SafeFormat("{} of {}, for {}", t.leg + 1, t.legs.size(),
                                       t.legStartedAt ? Span(now > t.legStartedAt ? now - t.legStartedAt : 0)
                                                      : std::string("-")));
        for (size_t i = 0; i < t.legs.size(); ++i)
        {
            const AutopilotLeg& l = t.legs[i];
            out.push_back(SafeFormat("  {} {}. {} {}{}{}", i == t.leg ? ">" : " ", i + 1, LegTypeName(l.type),
                                     l.label.empty() ? PointText(l.to) : l.label,
                                     l.entry ? SafeFormat(" [entry {}]", l.entry) : std::string(),
                                     l.taxiPath.empty() ? std::string()
                                                        : SafeFormat(" [{} taxi nodes]", l.taxiPath.size())));
        }

        const AutopilotRoute& r = t.route;
        out.push_back("  walk:");
        Kv(out, "    routed", YesNo(t.routed));
        if (t.routed)
        {
            std::string next = "-";
            if (r.next < r.nodes.size())
            {
                const AutopilotRoutePoint& n = r.nodes[r.next];
                next = SafeFormat("({:.0f}, {:.0f}, {:.0f}) {:.0f} yd away", n.x, n.y, n.z,
                                  bot->GetExactDist(n.x, n.y, n.z));
            }
            Kv(out, "    route", SafeFormat("node {} of {} built | {}{}{}", r.next, r.nodes.size(),
                                            r.complete ? "complete" : r.failed ? "FAILED" : "building",
                                            r.waiting ? " | waiting for tiles to load" : "",
                                            r.why.empty() ? std::string() : " | " + r.why));
            Kv(out, "    next node", next);
            Kv(out, "    walking", t.issued == SIZE_MAX ? std::string("NO - no usable path to the next node")
                                                         : SafeFormat("to node {}", t.issued));
            Kv(out, "    road anchors", SafeFormat("{} of {} ({})", std::min(r.anchor, r.anchors.size()),
                                                   r.anchors.size(), r.anchorNote.empty() ? "-" : r.anchorNote));
            Kv(out, "    corridor", SafeFormat("corner {} of {} | reach {:.0f} yd | stalls {} | steps off slopes {}",
                                               r.corner, r.corridor.size(), r.corridorReach, r.stalls, r.stepOffs));
            Kv(out, "    cost", SafeFormat("{} navmesh queries | {} rebuilds", r.queries, r.rebuilds));
            Kv(out, "    progress", SafeFormat("{} points reached | nearest {:.0f} yd, {}", t.lastReached, t.best,
                                               t.bestAt ? Ago(t.bestAt, now) : std::string("-")));
        }
        Kv(out, "  vehicle", SafeFormat("boarded {} | deck found {} | stepping off {} | took off {}",
                                        YesNo(t.boarded), YesNo(t.deckFound), YesNo(t.stepping),
                                        YesNo(t.tookOff)));
    }
}

std::vector<AutopilotMonitorRow> Autopilot_MonitorList()
{
    std::vector<AutopilotMonitorRow> out;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (uint64_t guid : g_roster)
    {
        auto rowIt = g_rows.find(guid);
        if (rowIt == g_rows.end() || !rowIt->second.enrolled)
            continue;
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
        if (!bot || !bot->IsInWorld())
            continue;

        const Online* ob = OnlineOf(guid);
        AutopilotMonitorRow m;
        m.guid  = guid;
        m.name  = bot->GetName();
        m.level = bot->GetLevel();
        m.cls   = bot->getClass();
        m.zone  = bot->GetZoneId();
        m.tier  = ob ? TierName(ob->tier) : "-";
        m.doing = rowIt->second.doing;

        if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            m.state = "corpse run";
        else if (!bot->IsAlive())
            m.state = "dead";
        else if (bot->IsInCombat())
            m.state = "fighting";
        else if (bot->IsInFlight())
            m.state = "flying";
        else if (bot->GetTransport())
            m.state = "aboard";
        else if (ob && ob->errand.active)
            m.state = "travelling";
        else if (ob && ob->planPending)
            m.state = "waiting on the model";
        else
            m.state = "idle";
        out.push_back(std::move(m));
    }
    return out;
}

bool Autopilot_MonitorPage(Player* bot, const std::string& page, std::vector<std::string>& out)
{
    const uint64_t guid = bot->GetGUID().GetRawValue();
    const uint32_t now  = Progress_Now();
    Row    row;
    Online ob;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        auto it = g_rows.find(guid);
        if (it == g_rows.end())
            return false;
        row = it->second;
        if (const Online* on = OnlineOf(guid))
            ob = *on;
    }
    PlayerbotAI* ai = BotAI(bot);

    if (page == "overview")
    {
        Head(out, "Bot");
        if (ai)
            Kv(out, "character", SafeFormat("level {} {} {} {}", bot->GetLevel(),
                                            bot->getGender() == GENDER_FEMALE ? "female" : "male",
                                            ai->GetChatHelper()->FormatRace(bot->getRace()),
                                            ai->GetChatHelper()->FormatClass(bot->getClass())));
        Kv(out, "where", SafeFormat("{} - map {} ({:.1f}, {:.1f}, {:.1f}){}", Progress_ZoneName(bot->GetZoneId()),
                                    bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                    bot->GetTransport() ? " - on a transport" : ""));
        Kv(out, "health", SafeFormat("{}% | power {}/{} | gold {}", uint32_t(bot->GetHealthPct()),
                                     bot->GetPower(bot->getPowerType()), bot->GetMaxPower(bot->getPowerType()),
                                     bot->GetMoney() / 10000));
        std::string attackers;
        for (Unit* u : bot->getAttackers())
            if (u)
                attackers += (attackers.empty() ? "" : ", ") + u->GetName();
        Kv(out, "combat", SafeFormat("{} | attacked by {} | target {}", YesNo(bot->IsInCombat()),
                                     attackers.empty() ? std::string("nobody") : attackers,
                                     bot->GetVictim() ? bot->GetVictim()->GetName() : std::string("none")));
        Kv(out, "body", SafeFormat("{}{}{}{}", bot->IsAlive() ? "alive" : "dead",
                                   bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? " (ghost)" : "",
                                   bot->IsSitState() ? " | sitting" : "", bot->IsInFlight() ? " | in flight" : ""));
        Kv(out, "around", AutopilotCommands_DescribeSurroundings(bot));
        Kv(out, "can craft", AutopilotCommands_DescribeCraftable(bot));
        Kv(out, "movement", SafeFormat("{} | moving {}", MotionName(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()),
                                       YesNo(bot->isMoving())));
        if (ai)
        {
            const BotState state = ai->GetState();
            Kv(out, "playerbots engine", state == BOT_STATE_COMBAT ? "combat" : state == BOT_STATE_DEAD ? "dead" : "non-combat");
            // Playerbots idles bots with no real player near (AiPlayerbot.BotActiveAlone):
            // only urgent actions run, so no grinding, looting or buffing.
            Kv(out, "playerbots activity", ai->AllowActivity(ALL_ACTIVITY)
                ? "active"
                : "idle: no real player near (AiPlayerbot.BotActiveAlone and ForceWhenIn*)");
            Kv(out, "live strategies", DescribeLiveStrategies(ai));
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);   // reads other bots' rows
                Kv(out, "group", DescribeGroup(bot, ai));
            }
            Kv(out, "now", DescribeActivity(bot, ob));
        }

        Head(out, "Autopilot");
        Kv(out, "enrolled", SafeFormat("{} | mode {} | source {} | tier {}", YesNo(row.enrolled), ModeName(row.mode),
                                       row.source.empty() ? "-" : row.source, TierName(ob.tier)));
        Kv(out, "planning", SafeFormat("{}{}{} | last plan {} | next due {}",
                                       ob.planPending ? "waiting on the model since " + Ago(ob.planSubmittedAt, now)
                                                      : std::string("idle"),
                                       ob.quickPlan ? " | quick replan asked" : "",
                                       ob.urgentPlan ? " | urgent" : "",
                                       row.lastPlanAt ? Ago(row.lastPlanAt, now) : std::string("never"),
                                       row.planUntil > now ? "in " + Span(row.planUntil - now) : std::string("now")));
        Kv(out, "identity", row.HasIdentity() ? SafeFormat("{} ({})", row.style, row.outlook) : std::string());
        Kv(out, "profile", row.profile);
        Kv(out, "doing", row.doing.empty() ? std::string()
                                           : row.doing + (row.doingSince ? " (since " + Ago(row.doingSince, now) + ")" : ""));
        Kv(out, "decided by", row.decidedBy);
        Kv(out, "goal", row.goal.Active() ? Goal_Describe(bot, row.goal, Counters(row)) : std::string());
        Kv(out, "last reason", row.lastReason);
        Kv(out, "model's strategies", row.strategies.empty() ? std::string() : FormatStrategies(row.strategies));
        Kv(out, "errand now", ob.errand.active ? DescribeActivity(bot, ob) : std::string("none"));
        for (size_t i = 0; i < ob.errandQueue.size(); ++i)
            Kv(out, SafeFormat("queued {}", i + 1), ob.errandQueue[i]);
        // As reported when given; the errand line above is live.
        for (const std::string& r : row.lastResults)
            Kv(out, "ordered", r);

        Head(out, "Watchdogs");
        Kv(out, "last alert", ob.lastAlert.empty() ? std::string()
                                                   : ob.lastAlert + " (" + Ago(ob.lastAlertAt, now) + ")");
        Kv(out, "last busy", ob.lastBusyAt ? Ago(ob.lastBusyAt, now) : std::string());
        Kv(out, "last fight", ob.lastCombatAt ? Ago(ob.lastCombatAt, now) : std::string());
        Kv(out, "walk held", ob.castHoldUntil > now ? "yes - waiting out a cast or an upkeep pause" : "no");
        Kv(out, "teleport hold", ob.teleportDeferredAt ? "pushed back " + Ago(ob.teleportDeferredAt, now) : std::string());
        Kv(out, "death", ob.deadSince ? SafeFormat("dead {} | revive held {} | corpse run {}",
                                                   Ago(ob.deadSince, now), YesNo(ob.reviveHeld),
                                                   YesNo(ob.corpseTrip.active))
                                      : std::string());
        if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            if (Corpse* corpse = bot->GetCorpse())
            {
                // The core refuses a reclaim until the ghost time plus a delay
                // that grows with recent deaths (30 s, 60 s, 120 s) has passed.
                const time_t ready = corpse->GetGhostTime() +
                                     bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP);
                const time_t gameNow = GameTime::GetGameTime().count();
                const std::string where = corpse->GetMapId() != bot->GetMapId()
                    ? std::string("body on another map")
                    : SafeFormat("body {:.0f} yd away", bot->GetExactDist2d(corpse));
                Kv(out, "resurrect", ready > gameNow
                    ? SafeFormat("{} | the game allows it in {}s (delay grows with recent deaths)", where,
                                 uint32_t(ready - gameNow))
                    : where + " | allowed now");
            }
        Kv(out, "last hour", RewardSummary(ob, now));
        for (const auto& [at, text] : ob.temptations)
            Kv(out, "tempted " + Ago(at, now), text);
        Kv(out, "since enrollment", SafeFormat("{} kills, {} deaths, {} quests, {} dungeons", row.killsTotal,
                                               row.deathsTotal, row.questsTotal, row.dungeonsTotal));
        return true;
    }

    if (page == "travel")
    {
        Head(out, "Errand");
        Kv(out, "active", YesNo(ob.errand.active));
        if (ob.errand.active)
        {
            Kv(out, "kind", ErrandKindName(ob.errand.kind));
            Kv(out, "bound for", ob.errand.label);
            Kv(out, "started", ob.errand.startedAt ? Ago(ob.errand.startedAt, now) : std::string());
            if (ob.errand.npcEntry)
                Kv(out, "npc entry", std::to_string(ob.errand.npcEntry));
            if (ob.errand.questId)
                Kv(out, "quest", std::to_string(ob.errand.questId));
        }
        for (size_t i = 0; i < ob.errandQueue.size(); ++i)
            Kv(out, SafeFormat("then {}", i + 1), ob.errandQueue[i]);
        Head(out, "Trip");
        TripLines(bot, ob.errand.trip, now, out);
        if (ob.corpseTrip.active || ob.corpseRetryAt > now)
        {
            Head(out, "Corpse run");
            TripLines(bot, ob.corpseTrip, now, out);
            if (ob.corpseRetryAt > now)
                Kv(out, "retry", "in " + Span(ob.corpseRetryAt - now));
        }
        return true;
    }

    if (page == "planner")
    {
        Head(out, "Last exchange with the model");
        AutopilotExchange ex;
        if (!AutopilotPlanner_LastExchange(guid, ex))
        {
            out.push_back("none kept - the model has not been asked about this bot recently");
            return true;
        }
        Kv(out, "asked", Ago(ex.submittedAt, now));
        Kv(out, "answered", ex.answeredAt ? SafeFormat("{} ({} ms)", Ago(ex.answeredAt, now), ex.latencyMs)
                                          : std::string("still waiting"));
        if (!ex.error.empty())
            Kv(out, "error", ex.error);
        Head(out, "Reply (raw)");
        TextBlock(out, ex.reply.empty() ? std::string("(none)") : ex.reply);
        Head(out, "Prompt");
        TextBlock(out, ex.prompt);
        return true;
    }

    if (page == "events")
    {
        Head(out, "Orders and what they did");
        for (const std::string& r : row.lastResults)
            out.push_back("  " + r);
        Head(out, "Recent decisions (newest last)");
        for (const std::string& d : ob.decisions)
            out.push_back("  " + d);
        Head(out, "Diary (newest last)");
        for (const ProgressEvent& e : ob.events)
            out.push_back(SafeFormat("  [{}] {}: {}", Ago(e.at, now), e.type, e.detail));
        return true;
    }

    out.push_back("unknown page: " + page);
    return true;
}

AutopilotServerScript::AutopilotServerScript()
    : ServerScript("AutopilotServerScript", { SERVERHOOK_ON_PACKET_SENT })
{
}

void AutopilotServerScript::OnPacketSent(WorldSession* session, WorldPacket const& packet)
{
    if (packet.GetOpcode() != SMSG_LFG_PROPOSAL_UPDATE || !session || !AutopilotStrategy_NoHandouts())
        return;
    Player* bot = session->GetPlayer();
    if (!bot || !Autopilot_IsActive())
        return;

    const uint64_t guid = bot->GetGUID().GetRawValue();
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        auto it = g_rows.find(guid);
        if (it == g_rows.end() || !it->second.enrolled)
            return;
    }

    // uint32 dungeon, uint8 state, uint32 proposal id, ...
    WorldPacket p(packet);
    p.rpos(0);
    uint32 dungeonId = 0, id = 0;
    uint8 state = 0;
    p >> dungeonId >> state >> id;
    if (!id)
        return;
    std::lock_guard<std::mutex> lock(g_lfgMutex);
    g_lfgPending[guid] = id;
}
