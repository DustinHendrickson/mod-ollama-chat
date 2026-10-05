#include "mod-ollama-chat_autopilot_strategy.h"

#include "Log.h"

#include "Player.h"

#include "AiObjectContext.h"
#include "NamedObjectContext.h"
#include "Creature.h"
#include "LastMovementValue.h"
#include "MotionMaster.h"
#include "PlayerbotAI.h"
#include "Strategy.h"

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
    class AutopilotMarkerStrategy : public Strategy
    {
    public:
        explicit AutopilotMarkerStrategy(PlayerbotAI* botAI) : Strategy(botAI) { }

        std::string const getName() override { return AUTOPILOT_STRATEGY_NAME; }
        uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
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

namespace
{
    LastMovement& LastMove(PlayerbotAI* ai)
    {
        return ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
    }
}

void AutopilotMove_To(PlayerbotAI* ai, float x, float y, float z, bool generatePath)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    if (!bot || !bot->IsInWorld() || bot->IsInFlight() || bot->IsBeingTeleported())
        return;

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, generatePath, false);

    // Recorded the way playerbots records its own moves, so its out-of-combat
    // movement waits for this one (MOVEMENT_NORMAL); combat still outranks
    // it, so a bot that is attacked on the way fights back.
    const float speed = std::max(1.0f, bot->GetSpeed(MOVE_RUN));
    const float delay = bot->GetExactDist(x, y, z) / speed * IN_MILLISECONDS + 500.0f;
    LastMove(ai).Set(bot->GetMapId(), x, y, z, bot->GetOrientation(), delay, MovementPriority::MOVEMENT_NORMAL);
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
