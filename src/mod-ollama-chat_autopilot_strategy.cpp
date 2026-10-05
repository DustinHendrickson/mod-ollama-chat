#include "mod-ollama-chat_autopilot_strategy.h"

#include "Log.h"

#include "Player.h"

#include "AiObjectContext.h"
#include "NamedObjectContext.h"
#include "NewRpgBaseAction.h"
#include "NewRpgInfo.h"
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

    // Not registered anywhere and never executed as an action: it exists only
    // to reach NewRpgBaseAction's protected status-selection helpers.
    class AutopilotRpgSteer : public NewRpgBaseAction
    {
    public:
        explicit AutopilotRpgSteer(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "autopilot rpg steer") { }

        // Weighted pick among the allowed statuses, trying the next one when a
        // status has no target right now. Mirrors RandomChangeStatus but
        // without its fallback of sitting the bot down when nothing fits, and
        // without checking each target twice (Check* then Select* again).
        bool Steer(const std::vector<int>& allowed)
        {
            std::vector<std::pair<NewRpgStatus, uint32>> candidates;
            for (int s : allowed)
            {
                if (s <= RPG_IDLE || s >= RPG_STATUS_END)
                    continue;
                const NewRpgStatus status = static_cast<NewRpgStatus>(s);
                const int32 weight = sPlayerbotAIConfig.RpgStatusProbWeight[status];
                if (weight > 0)
                    candidates.emplace_back(status, static_cast<uint32>(weight));
            }

            while (!candidates.empty())
            {
                uint32 total = 0;
                for (const auto& c : candidates)
                    total += c.second;

                uint32 roll = urand(1, total);
                size_t pick = 0;
                for (; pick < candidates.size(); ++pick)
                {
                    if (roll <= candidates[pick].second)
                        break;
                    roll -= candidates[pick].second;
                }
                pick = std::min(pick, candidates.size() - 1);

                if (Enter(candidates[pick].first))
                    return true;
                candidates.erase(candidates.begin() + pick);
            }
            return false;
        }

    private:
        bool Enter(NewRpgStatus status)
        {
            NewRpgInfo& info = botAI->rpgInfo;
            switch (status)
            {
                case RPG_WANDER_RANDOM:
                    if (!CheckRpgStatusAvailable(status))
                        return false;
                    info.ChangeToWanderRandom();
                    return true;
                case RPG_WANDER_NPC:
                    if (!CheckRpgStatusAvailable(status))
                        return false;
                    info.ChangeToWanderNpc();
                    return true;
                case RPG_GO_GRIND:
                {
                    WorldPosition pos = SelectRandomGrindPos(bot);
                    if (pos == WorldPosition())
                        return false;
                    info.ChangeToGoGrind(pos);
                    return true;
                }
                case RPG_GO_CAMP:
                {
                    WorldPosition pos = SelectRandomCampPos(bot);
                    if (pos == WorldPosition())
                        return false;
                    info.ChangeToGoCamp(pos);
                    return true;
                }
                case RPG_DO_QUEST:
                {
                    std::vector<uint32> quests;
                    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                    {
                        const uint32 questId = bot->GetQuestSlotQuestId(slot);
                        if (!questId || botAI->lowPriorityQuest.count(questId))
                            continue;
                        std::vector<POIInfo> poi;
                        if (GetQuestPOIPosAndObjectiveIdx(questId, poi, true))
                            quests.push_back(questId);
                    }
                    if (quests.empty())
                        return false;
                    const uint32 questId = quests[urand(0, static_cast<uint32>(quests.size()) - 1)];
                    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                    if (!quest)
                        return false;
                    info.ChangeToDoQuest(questId, quest);
                    return true;
                }
                case RPG_TRAVEL_FLIGHT:
                {
                    uint32 entry = 0;
                    WorldPosition pos;
                    std::vector<uint32> path;
                    if (!SelectRandomFlightTaxiNode(entry, pos, path))
                        return false;
                    info.ChangeToTravelFlight(entry, pos, std::move(path));
                    return true;
                }
                case RPG_REST:
                    info.ChangeToRest();
                    bot->SetStandState(UNIT_STAND_STATE_SIT);
                    return true;
                case RPG_OUTDOOR_PVP:
                    if (!CheckRpgStatusAvailable(status))
                        return false;
                    info.ChangeToOutdoorPvp();
                    return true;
                default:
                    return false;
            }
        }
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

int AutopilotRpg_StatusFromName(const std::string& name)
{
    NewRpgStatus status = NewRpgInfo::StatusFromString(name);
    return status == RPG_STATUS_END ? -1 : static_cast<int>(status);
}

std::string AutopilotRpg_StatusName(int status)
{
    switch (status)
    {
        case RPG_IDLE:          return "idle";
        case RPG_GO_GRIND:      return "go grind";
        case RPG_GO_CAMP:       return "go camp";
        case RPG_WANDER_RANDOM: return "wander random";
        case RPG_WANDER_NPC:    return "wander npc";
        case RPG_DO_QUEST:      return "do quest";
        case RPG_TRAVEL_FLIGHT: return "travel flight";
        case RPG_REST:          return "rest";
        case RPG_OUTDOOR_PVP:   return "outdoor pvp";
        default:                return "unknown";
    }
}

int AutopilotRpg_CurrentStatus(PlayerbotAI* ai)
{
    return ai ? static_cast<int>(ai->rpgInfo.GetStatus()) : -1;
}

bool AutopilotRpg_IsTravelling(PlayerbotAI* ai)
{
    if (!ai)
        return false;
    if (Player* bot = ai->GetBot())
        if (bot->IsInFlight())
            return true;
    return ai->rpgInfo.GetStatus() == RPG_TRAVEL_FLIGHT;
}

bool AutopilotRpg_Steer(PlayerbotAI* ai, const std::vector<int>& allowed)
{
    if (!ai || allowed.empty())
        return false;

    AutopilotRpgSteer steer(ai);
    return steer.Steer(allowed);
}
