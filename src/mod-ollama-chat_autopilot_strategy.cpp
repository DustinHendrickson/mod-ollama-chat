#include "mod-ollama-chat_autopilot_strategy.h"
#include "mod-ollama-chat_autopilot_route.h"

#include "Log.h"

#include "Player.h"

#include "AiObjectContext.h"
#include "NamedObjectContext.h"
#include "Creature.h"
#include "LastMovementValue.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "PlayerbotAI.h"
#include "Action.h"
#include "Map.h"
#include "Multiplier.h"
#include "Strategy.h"

#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "DKAiObjectContext.h"
#include "DruidAiObjectContext.h"
#include "HunterAiObjectContext.h"
#include "MageAiObjectContext.h"
#include "PaladinAiObjectContext.h"
#include "PriestAiObjectContext.h"
#include "RogueAiObjectContext.h"
#include "ShamanAiObjectContext.h"
#include "WarlockAiObjectContext.h"
#include "WarriorAiObjectContext.h"

namespace
{
    // An enrolled bot earns what it has, like a player. Playerbots hands random
    // bots things through a few actions that no strategy switch reaches; a
    // zero multiplier drops them for this bot only (Engine::DoNextAction
    // discards an action whose relevance multiplies to nothing).
    std::atomic<bool> g_noHandouts{ true };

    // Bots on a trip (an errand under way), set from the world thread and read
    // by the multiplier on map threads.
    std::mutex                   g_travelMutex;
    std::unordered_set<uint64_t> g_travelling;

    bool IsTravelling(Player* bot)
    {
        std::lock_guard<std::mutex> lock(g_travelMutex);
        return g_travelling.count(bot->GetGUID().GetRawValue()) > 0;
    }

    class AutopilotHoldMultiplier : public Multiplier
    {
    public:
        explicit AutopilotHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "autopilot hold") { }

        float GetValue(Action* action) override
        {
            if (!action)
                return 1.0f;
            const std::string name = action->getName();

            // On a trip, the trip is the order: grind's pulling of whatever is
            // in reach (neutral beasts too) and its wandering when there is
            // nothing would drag the bot off the road. What attacks it is
            // still fought (that is the combat engine). Grind resumes on
            // arrival.
            if (name == "attack anything" || name == "move random")
            {
                Player* bot = botAI->GetBot();
                return bot && IsTravelling(bot) ? 0.0f : 1.0f;
            }

            if (!g_noHandouts.load(std::memory_order_relaxed))
                return 1.0f;

            // Free talents, trainer and quest spells, skills, consumables and
            // a gear upgrade at every level up; the periodic re-roll and
            // refresh (bags emptied, money set); and the dungeon-finder accept,
            // which refreshes a bot queued alone (autopilot accepts instead).
            if (name == "auto maintenance on levelup" || name == "random bot update" || name == "lfg accept")
                return 0.0f;

            // Death on its own in the open world is autopilot's corpse run:
            // playerbots' release repairs everything for free, and its spirit
            // healer skips the sickness. In a group, a dungeon or a
            // battleground, playerbots handles it.
            if (name == "auto release" || name == "release" || name == "spirit healer")
            {
                Player* bot = botAI->GetBot();
                if (bot && !bot->GetGroup() && !bot->InBattleground() && bot->GetMap() &&
                    !bot->GetMap()->Instanceable())
                    return 0.0f;
            }
            return 1.0f;
        }
    };

    class AutopilotMarkerStrategy : public Strategy
    {
    public:
        explicit AutopilotMarkerStrategy(PlayerbotAI* botAI) : Strategy(botAI) { }

        std::string const getName() override { return AUTOPILOT_STRATEGY_NAME; }
        uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }

        void InitMultipliers(std::vector<Multiplier*>& multipliers) override
        {
            multipliers.push_back(new AutopilotHoldMultiplier(botAI));
        }
    };

    class AutopilotStrategyContext : public NamedObjectContext<Strategy>
    {
    public:
        AutopilotStrategyContext()
        {
            creators[AUTOPILOT_STRATEGY_NAME] = &AutopilotStrategyContext::autopilot;
        }

    private:
        static Strategy* autopilot(PlayerbotAI* botAI) { return new AutopilotMarkerStrategy(botAI); }
    };

    bool g_registered = false;

    // Each list takes ownership of what it is given (its destructor deletes its
    // contexts), so every class gets its own instance.
    void AddTo(SharedNamedObjectContextList<Strategy>& list)
    {
        if (list.creators.count(AUTOPILOT_STRATEGY_NAME) == 0)
            list.Add(new AutopilotStrategyContext());
    }

    bool In(SharedNamedObjectContextList<Strategy> const& list)
    {
        return list.creators.count(AUTOPILOT_STRATEGY_NAME) != 0;
    }
}

bool AutopilotStrategy_Register()
{
    // Idempotent (std::call_once inside). Normally playerbots has already done
    // this in OnBeforeWorldInitialized; calling it here guarantees our entries
    // are added to built lists rather than to lists that are built afterwards.
    AiObjectContext::BuildAllSharedContexts();

    AddTo(DKAiObjectContext::sharedStrategyContexts);
    AddTo(DruidAiObjectContext::sharedStrategyContexts);
    AddTo(HunterAiObjectContext::sharedStrategyContexts);
    AddTo(MageAiObjectContext::sharedStrategyContexts);
    AddTo(PaladinAiObjectContext::sharedStrategyContexts);
    AddTo(PriestAiObjectContext::sharedStrategyContexts);
    AddTo(RogueAiObjectContext::sharedStrategyContexts);
    AddTo(ShamanAiObjectContext::sharedStrategyContexts);
    AddTo(WarlockAiObjectContext::sharedStrategyContexts);
    AddTo(WarriorAiObjectContext::sharedStrategyContexts);

    g_registered =
        In(DKAiObjectContext::sharedStrategyContexts) &&
        In(DruidAiObjectContext::sharedStrategyContexts) &&
        In(HunterAiObjectContext::sharedStrategyContexts) &&
        In(MageAiObjectContext::sharedStrategyContexts) &&
        In(PaladinAiObjectContext::sharedStrategyContexts) &&
        In(PriestAiObjectContext::sharedStrategyContexts) &&
        In(RogueAiObjectContext::sharedStrategyContexts) &&
        In(ShamanAiObjectContext::sharedStrategyContexts) &&
        In(WarlockAiObjectContext::sharedStrategyContexts) &&
        In(WarriorAiObjectContext::sharedStrategyContexts);

    if (g_registered)
        LOG_INFO("server.loading", "[Ollama Chat] Registered the '{}' playerbots strategy.",
                 AUTOPILOT_STRATEGY_NAME);
    else
        LOG_ERROR("server.loading",
                  "[Ollama Chat] Could not register the '{}' playerbots strategy in every class "
                  "context. Autopilot will stay off. mod-playerbots may have changed how "
                  "strategies are registered.", AUTOPILOT_STRATEGY_NAME);

    return g_registered;
}

bool AutopilotStrategy_IsRegistered()
{
    return g_registered;
}

void AutopilotStrategy_SetNoHandouts(bool on)
{
    g_noHandouts.store(on, std::memory_order_relaxed);
}

bool AutopilotStrategy_NoHandouts()
{
    return g_noHandouts.load(std::memory_order_relaxed);
}

void AutopilotStrategy_SetTravelling(uint64_t botGuid, bool on)
{
    std::lock_guard<std::mutex> lock(g_travelMutex);
    if (on)
        g_travelling.insert(botGuid);
    else
        g_travelling.erase(botGuid);
}

void AutopilotBot_ClearDeathCount(PlayerbotAI* ai)
{
    if (!ai)
        return;
    auto* deaths = ai->GetAiObjectContext()->GetValue<uint32>("death count");
    if (deaths && deaths->Get())
        deaths->Set(0);
}

namespace
{
    // The longest straight step taken where the navmesh has no path (onto a
    // portal or an NPC on a ledge, off a slope the filter excludes), and the
    // most it may climb or drop.
    constexpr float SHORT_STEP       = 8.0f;
    constexpr float SHORT_STEP_CLIMB = 3.0f;

    LastMovement& LastMove(PlayerbotAI* ai)
    {
        return ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
    }
}

bool AutopilotMove_To(PlayerbotAI* ai, float x, float y, float z, bool generatePath)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    if (!bot || !bot->IsInWorld() || bot->IsInFlight() || bot->IsBeingTeleported())
        return false;

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    float length = bot->GetExactDist(x, y, z);
    if (!generatePath || length < 3.0f)
    {
        // Straight on purpose: onto or off a ship's deck (not on the
        // navmesh), or a step too short to matter.
        bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, false);
    }
    else
    {
        // MovePoint with pathfinding quietly falls back to a straight line
        // when the navmesh has no path (a point off the mesh, a tile not
        // loaded) -- through walls and up cliffs. Find the path here and
        // walk it only if it is a real one; otherwise do not move, and let
        // the trip reroute or report that it is stuck.
        // With the slope check the core cuts a path at the first step it finds
        // too steep -- which includes stepping down a river bank into the
        // water. A path cut to a stub left the bot standing at the bank for
        // good. So: the slope-checked path when it gets somewhere, else the
        // plain path on the core's bot filter, which still leaves out steep
        // ground (no mountainsides either way).
        auto usable = [&](PathGenerator& p, bool built)
        {
            if (!built || (p.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH)) ||
                p.GetPath().size() < 2)
                return false;
            const float reach = (p.GetPath().back() - p.GetPath().front()).length();
            return reach >= std::min(3.0f, length * 0.5f);
        };

        PathGenerator checked(bot);
        AutopilotRoute_Filter(bot, checked);   // the core's bot filter: no steep slopes, water costly
        checked.SetSlopeCheck(true);           // drop steps too steep to walk
        bool built = checked.CalculatePath(x, y, z, false);
        PathGenerator plain(bot);
        PathGenerator* chosen = &checked;
        if (!usable(checked, built))
        {
            AutopilotRoute_Filter(bot, plain);
            built  = plain.CalculatePath(x, y, z, false) || built;
            chosen = &plain;
        }
        PathGenerator& path = *chosen;
        if (!usable(path, built))
        {
            // A short step on or off the mesh (a portal, a trigger, an NPC on
            // a ledge, the bot standing on a slope the filter excludes) is
            // walked straight -- but only in sight and without a climb, never
            // through a wall or up a cliff.
            if (!built || length > SHORT_STEP || std::fabs(z - bot->GetPositionZ()) > SHORT_STEP_CLIMB ||
                !bot->IsWithinLOS(x, y, z))
                return false;
            bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, false);
        }
        else
        {
            Movement::PointsArray points = path.GetPath();
            // The escort generator hands a two-point path to MoveTo with
            // pathfinding on, which re-paths without our filter. Split the
            // straight segment so it walks exactly this one.
            if (points.size() == 2)
                points.insert(points.begin() + 1, (points[0] + points[1]) * 0.5f);
            length = 0.0f;
            for (size_t i = 1; i < points.size(); ++i)
                length += (points[i] - points[i - 1]).length();
            const G3D::Vector3& end = points.back();
            x = end.x;
            y = end.y;
            z = end.z;
            bot->GetMotionMaster()->MoveSplinePath(&points);
        }
    }

    // Recorded the way playerbots records its own moves, so its out-of-combat
    // movement waits for this one (MOVEMENT_NORMAL); combat still outranks
    // it, so a bot that is attacked on the way fights back.
    const float speed = std::max(1.0f, bot->GetSpeed(MOVE_RUN));
    const float delay = length / speed * IN_MILLISECONDS + 500.0f;
    LastMove(ai).Set(bot->GetMapId(), x, y, z, bot->GetOrientation(), delay, MovementPriority::MOVEMENT_NORMAL);
    return true;
}

void AutopilotMove_Hold(PlayerbotAI* ai, uint32_t ms, bool overCombat)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    if (!bot)
        return;
    LastMove(ai).Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                     bot->GetOrientation(), float(ms),
                     overCombat ? MovementPriority::MOVEMENT_FORCED : MovementPriority::MOVEMENT_NORMAL);
}

bool AutopilotMove_IsMoving(PlayerbotAI* ai)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    return bot && bot->isMoving();
}

void AutopilotMove_Stop(PlayerbotAI* ai)
{
    if (Player* bot = ai ? ai->GetBot() : nullptr)
        if (bot->isMoving() && !bot->IsInFlight())
            bot->StopMovingOnCurrentPos();
}

bool AutopilotQuest_TalkTo(PlayerbotAI* ai, Creature* npc)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    if (!bot || !npc)
        return false;
    // QuestAction falls back to the bot's own target when it has no master
    // to take one from.
    bot->SetTarget(npc->GetGUID());
    return ai->DoSpecificAction("talk to quest giver", Event(), true);
}

bool AutopilotMove_Yield(PlayerbotAI* ai)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    if (!bot || bot->IsInFlight())
        return false;

    // Only autopilot's own walk (its paths run as an escort-type spline);
    // playerbots' own combat moves are point and chase movement, left alone.
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != ESCORT_MOTION_TYPE)
        return false;

    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    // Release the claim on the bot's movement at once, so nothing of
    // playerbots' waits out the rest of a walk that is not happening.
    LastMove(ai).Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                     bot->GetOrientation(), 0.0f, MovementPriority::MOVEMENT_NORMAL);
    return true;
}
