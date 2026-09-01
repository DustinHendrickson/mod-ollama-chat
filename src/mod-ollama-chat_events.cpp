#include "mod-ollama-chat_events.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_topics.h"
#include "mod-ollama-chat-utilities.h"

#include "AchievementMgr.h"
#include "Containers.h"
#include "GameObject.h"
#include "Guild.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QuestDef.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include "AiFactory.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>
#include <string>
#include <vector>

namespace
{
    OllamaBotEventChatter eventChatter;

    bool IsGuildEventType(const std::string& type)
    {
        return  type == g_GuildEventTypeGuildJoin      ||
                type == g_GuildEventTypeGuildLeave     ||
                type == g_GuildEventTypeGuildPromotion ||
                type == g_GuildEventTypeGuildDemotion  ||
                type == g_GuildEventTypeLevelUp        ||
                type == g_GuildEventTypeEpicGear       ||
                type == g_GuildEventTypeRareGear       ||
                type == g_GuildEventTypeDungeonComplete||
                type == g_GuildEventTypeGuildAchievement ||
                type == g_GuildEventTypeGuildLogin;
    }

    bool GuildHasRealPlayerOnline(uint32 guildId)
    {
        if (!guildId)
            return false;

        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* player = pair.second;
            if (!player || !player->IsInWorld())
                continue;
            if (OllamaIsBotPlayer(player))
                continue;
            if (player->GetGuildId() == guildId)
                return true;
        }
        return false;
    }

    int EventChanceFor(const std::string& type)
    {
        // Ordinary events.
        if (type == g_EventTypeLearnedSpell)    return g_EventTypeLearnedSpell_Chance;
        if (type == g_EventTypeDefeated)        return g_EventTypeDefeated_Chance;
        if (type == g_EventTypeDefeatedPlayer)  return g_EventTypeDefeatedPlayer_Chance;
        if (type == g_EventTypePetDefeated)     return g_EventTypePetDefeated_Chance;
        if (type == g_EventTypeGotItem)         return g_EventTypeGotItem_Chance;
        if (type == g_EventTypeDied)            return g_EventTypeDied_Chance;
        if (type == g_EventTypeCompletedQuest)  return g_EventTypeCompletedQuest_Chance;
        if (type == g_EventTypeRequestedDuel)   return g_EventTypeRequestedDuel_Chance;
        if (type == g_EventTypeStartedDueling)  return g_EventTypeStartedDueling_Chance;
        if (type == g_EventTypeWonDuel)         return g_EventTypeWonDuel_Chance;
        if (type == g_EventTypeLeveledUp)       return g_EventTypeLeveledUp_Chance;
        if (type == g_EventTypeAchievement)     return g_EventTypeAchievement_Chance;
        if (type == g_EventTypeUsedObject)      return g_EventTypeUsedObject_Chance;

        // Guild events.
        if (type == g_GuildEventTypeEpicGear)         return g_GuildEventTypeEpicGear_Chance;
        if (type == g_GuildEventTypeRareGear)         return g_GuildEventTypeRareGear_Chance;
        if (type == g_GuildEventTypeGuildJoin)        return g_GuildEventTypeGuildJoin_Chance;
        if (type == g_GuildEventTypeGuildLogin)       return g_GuildEventTypeGuildLogin_Chance;
        if (type == g_GuildEventTypeGuildLeave)       return g_GuildEventTypeGuildLeave_Chance;
        if (type == g_GuildEventTypeGuildPromotion)   return g_GuildEventTypeGuildPromotion_Chance;
        if (type == g_GuildEventTypeGuildDemotion)    return g_GuildEventTypeGuildDemotion_Chance;
        if (type == g_GuildEventTypeGuildAchievement) return g_GuildEventTypeGuildAchievement_Chance;
        if (type == g_GuildEventTypeLevelUp)          return g_GuildEventTypeLevelUp_Chance;
        if (type == g_GuildEventTypeDungeonComplete)  return g_GuildEventTypeDungeonComplete_Chance;

        return 0;
    }

    // Plain-language line for the witnessed-event memory that the topic engine
    // reads. This is what lets a bot say "that hawk nearly had you" instead of
    // reciting its own spell list.
    std::string MemoryLineFor(const std::string& actor, const std::string& type,
                              const std::string& detail)
    {
        if (type == g_EventTypeDefeated || type == g_EventTypePetDefeated)
            return SafeFormat("{} killed {}", actor, detail);
        if (type == g_EventTypeDefeatedPlayer)
            return SafeFormat("{} cut down {} in a fight", actor, detail);
        if (type == g_EventTypeDied)
            return SafeFormat("{} was killed", actor);
        if (type == g_EventTypeGotItem)
            return SafeFormat("{} picked up {}", actor, detail);
        if (type == g_EventTypeCompletedQuest)
            return SafeFormat("{} finished the task '{}'", actor, detail);
        if (type == g_EventTypeLeveledUp)
            return SafeFormat("{} grew stronger, now level {}", actor, detail);
        if (type == g_EventTypeAchievement)
            return SafeFormat("{} earned recognition for {}", actor, detail);
        if (type == g_EventTypeWonDuel)
            return SafeFormat("{} beat {} in a duel", actor, detail);
        if (type == g_EventTypeLearnedSpell)
            return SafeFormat("{} learned {}", actor, detail);
        return "";
    }
}

// --------------------------------------------------------------------------

void OllamaBotEventChatter::DispatchGameEvent(Player* source, std::string type, std::string detail)
{
    if (!g_Enable || !g_EnableEventChatter || !source || type.empty())
        return;

    if (!source->IsInWorld() || !source->GetMap())
        return;

    const bool sourceIsBot = OllamaIsBotPlayer(source);

    // Seed the witnessed-event memory before any chance roll: bots should
    // remember what they saw even when they choose not to comment on it.
    if (const std::string memory = MemoryLineFor(source->GetName(), type, detail); !memory.empty())
        Topics_BroadcastEventToNearby(source, memory, g_EventChatterRealPlayerDistance);

    const bool isGuildEvent = source->GetGuild() && g_EnableGuildEventChatter &&
                              IsGuildEventType(type) &&
                              GuildHasRealPlayerOnline(source->GetGuildId());

    bool hasNearbyRealPlayer = false;
    for (auto const& pair : source->GetMap()->GetPlayers())
    {
        Player* player = pair.GetSource();
        if (!player || player == source)
            continue;
        if (OllamaIsBotPlayer(player))
            continue;
        if (player->IsWithinDist(source, g_EventChatterRealPlayerDistance, false))
        {
            hasNearbyRealPlayer = true;
            break;
        }
    }

    if (sourceIsBot && !hasNearbyRealPlayer && !isGuildEvent)
        return;

    // The chance roll happens BEFORE any cooldown is consumed. The old code
    // stamped cooldowns during candidate filtering and then early-returned
    // here, so bots burned their event cooldown on events that were discarded.
    const int chance = EventChanceFor(type);
    if (chance <= 0)
        return;
    if (int(urand(1, 100)) > chance)
        return;

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] Event from {}: type={} detail={}",
                 source->GetName(), type, detail);

    // Gather candidates.
    std::vector<Player*> candidateBots;

    if (isGuildEvent)
    {
        const uint32 guildId = source->GetGuildId();
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* player = pair.second;
            if (!player || !player->IsInWorld() || player->GetGuildId() != guildId)
                continue;

            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
            if (ai && ai->IsBotAI())
                candidateBots.push_back(player);
        }
    }
    else
    {
        for (auto const& pair : source->GetMap()->GetPlayers())
        {
            Player* player = pair.GetSource();
            if (!player || !player->IsWithinDist(source, g_EventChatterRealPlayerDistance, false))
                continue;

            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
            if (ai && ai->IsBotAI())
                candidateBots.push_back(player);
        }
    }

    if (candidateBots.empty())
        return;

    Acore::Containers::RandomShuffle(candidateBots);

    const uint32_t maxBots = isGuildEvent ? g_GuildChatterMaxBotsPerEvent
                                          : g_EventChatterMaxBotsPerPlayer;
    uint32_t responses = 0;

    for (Player* bot : candidateBots)
    {
        if (g_DisableRepliesInCombat && bot->IsInCombat())
            continue;

        uint32_t botChance;
        if (bot == source)
            botChance = g_EventChatterBotSelfCommentChance;
        else if (isGuildEvent)
            botChance = g_GuildChatterBotCommentChance;
        else
            botChance = g_EventChatterBotCommentChance;

        if (botChance == 0 || urand(1, 100) > botChance)
            continue;

        // Cooldown is now keyed by ObjectGuid, pruned on logout, and consumed
        // only here -- at the point the bot actually commits to speaking.
        if (!Governor_TryConsumeEventCooldown(bot->GetGUID()))
            continue;

        // Was: any group at all, which let a bot spend a generation
        // commenting to a party of nothing but bots. Say is the fallback, and
        // the loop above already established a real player is in range of it.
        const bool partyAudience =
            bot->GetGroup() && !g_DisableForParty && OllamaGroupHasRealPlayer(bot);

        const ChatChannelSourceLocal source_ =
            isGuildEvent ? SRC_GUILD_LOCAL
                         : (partyAudience ? SRC_PARTY_LOCAL : SRC_SAY_LOCAL);

        const std::string scopeKey = Governor_MakeScopeKey(
            ChatChannelSourceLocalStr[source_], 0, "",
            source_ == SRC_GUILD_LOCAL ? bot->GetGuildId() : 0,
            bot->GetZoneId());

        if (!Governor_CanSend(bot->GetGUID(), scopeKey))
            continue;

        // Built here, on the world thread. The old code built the whole prompt
        // inside the worker, reading area, zone, spec and guild off-thread.
        std::string prompt = BuildPrompt(bot, g_EventChatterPromptTemplate, type, detail,
                                         source->GetName());
        if (prompt.empty())
            continue;

        OllamaChatRequest request;
        request.botGuid     = bot->GetGUID().GetRawValue();
        request.targetGuid  = (bot == source) ? 0 : source->GetGUID().GetRawValue();
        request.source      = source_;
        request.chainDepth  = 0;
        request.scopeKey    = scopeKey;
        request.prompt      = std::move(prompt);
        request.botName     = bot->GetName();
        request.kind        = OllamaRequestKind::EventChatter;
        request.triggerBotReplies = true;

        if (!OllamaDispatch_Submit(std::move(request)))
            continue;

        ++responses;
        if (maxBots > 0 && responses >= maxBots)
            break;
    }

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] Event dispatch complete, {} bots queued.", responses);
}

std::string OllamaBotEventChatter::BuildPrompt(Player* bot, std::string promptTemplate,
                                               std::string eventType, std::string eventDetail,
                                               std::string actorName)
{
    if (!bot || promptTemplate.empty())
        return "";

    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!ai || !ai->GetChatHelper())
        return "";

    const std::string personality       = GetBotPersonality(bot);
    const std::string personalityPrompt = GetPersonalityPromptAddition(personality);

    AreaTableEntry const* area = ai->GetCurrentArea();
    AreaTableEntry const* zone = ai->GetCurrentZone();

    std::string sentimentInfo;
    if (g_EnableSentimentTracking && !actorName.empty())
    {
        if (Player* actor = ObjectAccessor::FindPlayerByName(actorName))
            sentimentInfo = GetSentimentPromptAddition(bot, actor);
    }

    std::string prompt = SafeFormat(
        promptTemplate,
        fmt::arg("bot_name", bot->GetName()),
        fmt::arg("bot_level", bot->GetLevel()),
        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
        fmt::arg("bot_gender", bot->getGender() == GENDER_MALE ? "Male" : "Female"),
        fmt::arg("bot_role", CleanRoleForPrompt(ChatHelper::FormatClass(bot, AiFactory::GetPlayerSpecTab(bot)))),
        fmt::arg("bot_faction", bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde"),
        fmt::arg("bot_area", area ? PlayerbotAI::GetLocalizedAreaName(area) : "UnknownArea"),
        fmt::arg("bot_zone", zone ? PlayerbotAI::GetLocalizedAreaName(zone) : "UnknownZone"),
        fmt::arg("bot_map", OllamaContinentName(bot)),
        fmt::arg("bot_personality", personalityPrompt),
        fmt::arg("bot_personality_name", personality),
        fmt::arg("event_type", eventType),
        fmt::arg("event_detail", eventDetail),
        fmt::arg("actor_name", actorName),
        fmt::arg("sentiment_info", sentimentInfo));

    prompt += Memory_BuildPromptSection(bot, nullptr);
    prompt += Roleplay_BuildVoicePrompt(bot);
    return prompt;
}

// ==========================================================================
// Script hooks
// ==========================================================================

ChatOnKill::ChatOnKill()
    : PlayerScript("ChatOnKill", {
          PLAYERHOOK_ON_CREATURE_KILL,
          PLAYERHOOK_ON_PVP_KILL,
          PLAYERHOOK_ON_CREATURE_KILLED_BY_PET,
      }) { }

void ChatOnKill::OnPlayerCreatureKill(Player* killer, Creature* victim)
{
    if (killer && victim)
        eventChatter.DispatchGameEvent(killer, g_EventTypeDefeated, victim->GetName());
}

void ChatOnKill::OnPlayerPVPKill(Player* killer, Player* killed)
{
    if (killer && killed)
        eventChatter.DispatchGameEvent(killer, g_EventTypeDefeatedPlayer, killed->GetName());
}

void ChatOnKill::OnPlayerCreatureKilledByPet(Player* owner, Creature* victim)
{
    if (owner && victim)
        eventChatter.DispatchGameEvent(owner, g_EventTypePetDefeated, victim->GetName());
}

// --------------------------------------------------------------------------

ChatOnLoot::ChatOnLoot()
    : PlayerScript("ChatOnLoot", { PLAYERHOOK_ON_STORE_NEW_ITEM }) { }

void ChatOnLoot::OnPlayerStoreNewItem(Player* player, Item* item, uint32 /*count*/)
{
    if (!player || !item || !item->GetTemplate())
        return;

    ItemTemplate const* tmpl = item->GetTemplate();

    if (tmpl->Quality >= ITEM_QUALITY_UNCOMMON)
        eventChatter.DispatchGameEvent(player, g_EventTypeGotItem, tmpl->Name1);

    if (!player->GetGuild() || !g_EnableGuildEventChatter)
        return;

    if (tmpl->Quality == ITEM_QUALITY_EPIC && !g_GuildEventTypeEpicGear.empty())
    {
        eventChatter.DispatchGameEvent(player, g_GuildEventTypeEpicGear, tmpl->Name1);
    }
    else if (tmpl->Quality == ITEM_QUALITY_RARE && !g_GuildEventTypeRareGear.empty())
    {
        if (tmpl->Class == ITEM_CLASS_WEAPON || tmpl->Class == ITEM_CLASS_ARMOR)
            eventChatter.DispatchGameEvent(player, g_GuildEventTypeRareGear, tmpl->Name1);
    }
}

// --------------------------------------------------------------------------

ChatOnDeath::ChatOnDeath()
    : PlayerScript("ChatOnDeath", { PLAYERHOOK_ON_PLAYER_JUST_DIED }) { }

void ChatOnDeath::OnPlayerJustDied(Player* player)
{
    if (player)
        eventChatter.DispatchGameEvent(player, g_EventTypeDied, "");
}

// --------------------------------------------------------------------------

ChatOnQuest::ChatOnQuest()
    : PlayerScript("ChatOnQuest", { PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST }) { }

void ChatOnQuest::OnPlayerCompleteQuest(Player* player, Quest const* quest)
{
    if (!player || !quest)
        return;

    eventChatter.DispatchGameEvent(player, g_EventTypeCompletedQuest, quest->GetTitle());

    if (player->GetGuild() && g_EnableGuildEventChatter &&
        !g_GuildEventTypeDungeonComplete.empty() &&
        player->GetMap() && player->GetMap()->IsDungeon())
    {
        eventChatter.DispatchGameEvent(
            player, g_GuildEventTypeDungeonComplete,
            SafeFormat("{} in {}", quest->GetTitle(), OllamaContinentName(player)));
    }
}

// --------------------------------------------------------------------------

ChatOnLearn::ChatOnLearn()
    : PlayerScript("ChatOnLearn", { PLAYERHOOK_ON_LEARN_SPELL }) { }

void ChatOnLearn::OnPlayerLearnSpell(Player* player, uint32 spellID)
{
    if (!player)
        return;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
    if (spellInfo && spellInfo->SpellName[0] && *spellInfo->SpellName[0])
        eventChatter.DispatchGameEvent(player, g_EventTypeLearnedSpell, spellInfo->SpellName[0]);
}

// --------------------------------------------------------------------------

ChatOnDuel::ChatOnDuel()
    : PlayerScript("ChatOnDuel", {
          PLAYERHOOK_ON_DUEL_REQUEST,
          PLAYERHOOK_ON_DUEL_START,
          PLAYERHOOK_ON_DUEL_END,
      }) { }

void ChatOnDuel::OnPlayerDuelRequest(Player* target, Player* challenger)
{
    if (challenger && target)
        eventChatter.DispatchGameEvent(challenger, g_EventTypeRequestedDuel, target->GetName());
}

void ChatOnDuel::OnPlayerDuelStart(Player* player1, Player* player2)
{
    if (player1 && player2)
        eventChatter.DispatchGameEvent(player1, g_EventTypeStartedDueling, player2->GetName());
}

void ChatOnDuel::OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType /*type*/)
{
    if (winner && loser)
        eventChatter.DispatchGameEvent(winner, g_EventTypeWonDuel, loser->GetName());
}

// --------------------------------------------------------------------------

ChatOnLevelUp::ChatOnLevelUp()
    : PlayerScript("ChatOnLevelUp", { PLAYERHOOK_ON_LEVEL_CHANGED }) { }

void ChatOnLevelUp::OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/)
{
    if (!player)
        return;

    const std::string level = std::to_string(player->GetLevel());
    eventChatter.DispatchGameEvent(player, g_EventTypeLeveledUp, level);

    if (player->GetGuild() && g_EnableGuildEventChatter && !g_GuildEventTypeLevelUp.empty())
        eventChatter.DispatchGameEvent(player, g_GuildEventTypeLevelUp, level);
}

// --------------------------------------------------------------------------

ChatOnAchievement::ChatOnAchievement()
    : PlayerScript("ChatOnAchievement", { PLAYERHOOK_ON_ACHI_COMPLETE }) { }

void ChatOnAchievement::OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement)
{
    if (!player || !achievement || !achievement->name[0])
        return;

    eventChatter.DispatchGameEvent(player, g_EventTypeAchievement, achievement->name[0]);

    if (player->GetGuild() && g_EnableGuildEventChatter &&
        !g_GuildEventTypeGuildAchievement.empty() &&
        OllamaIsRealPlayer(player))
    {
        eventChatter.DispatchGameEvent(player, g_GuildEventTypeGuildAchievement, achievement->name[0]);
    }
}

// --------------------------------------------------------------------------

ChatOnGameObjectUse::ChatOnGameObjectUse() : AllGameObjectScript("ChatOnGameObjectUse") { }

bool ChatOnGameObjectUse::CanGameObjectGossipHello(Player* player, GameObject* go)
{
    if (player && go && go->GetGOInfo())
        eventChatter.DispatchGameEvent(player, g_EventTypeUsedObject, go->GetGOInfo()->name);

    // false = we did not handle the interaction; normal processing continues.
    return false;
}

// --------------------------------------------------------------------------

ChatOnGuild::ChatOnGuild()
    : GuildScript("ChatOnGuild", {
          GUILDHOOK_ON_ADD_MEMBER,
          GUILDHOOK_ON_REMOVE_MEMBER,
          GUILDHOOK_ON_EVENT,
      }) { }

void ChatOnGuild::OnAddMember(Guild* guild, Player* player, uint8& /*plRank*/)
{
    if (!guild || !player || !g_EnableGuildEventChatter || g_GuildEventTypeGuildJoin.empty())
        return;

    eventChatter.DispatchGameEvent(player, g_GuildEventTypeGuildJoin, guild->GetName());
}

void ChatOnGuild::OnRemoveMember(Guild* guild, Player* player, bool /*isDisbanding*/, bool /*isKicked*/)
{
    if (!guild || !player || !g_EnableGuildEventChatter || g_GuildEventTypeGuildLeave.empty())
        return;

    eventChatter.DispatchGameEvent(player, g_GuildEventTypeGuildLeave, guild->GetName());
}

void ChatOnGuild::OnEvent(Guild* guild, uint8 eventType, ObjectGuid::LowType playerGuid1,
                          ObjectGuid::LowType /*playerGuid2*/, uint8 newRank)
{
    if (!guild || !g_EnableGuildEventChatter)
        return;

    if (eventType != GUILD_EVENT_LOG_PROMOTE_PLAYER && eventType != GUILD_EVENT_LOG_DEMOTE_PLAYER)
        return;

    Player* player = ObjectAccessor::FindConnectedPlayer(
        ObjectGuid::Create<HighGuid::Player>(playerGuid1));
    if (!player)
        return;

    const bool promoted = (eventType == GUILD_EVENT_LOG_PROMOTE_PLAYER);
    const std::string& type = promoted ? g_GuildEventTypeGuildPromotion
                                       : g_GuildEventTypeGuildDemotion;
    if (type.empty())
        return;

    eventChatter.DispatchGameEvent(player, type, std::to_string(newRank));
}

// --------------------------------------------------------------------------

ChatOnGuildLogin::ChatOnGuildLogin()
    : PlayerScript("ChatOnGuildLogin", { PLAYERHOOK_ON_LOGIN }) { }

void ChatOnGuildLogin::OnPlayerLogin(Player* player)
{
    if (!player || !g_EnableGuildEventChatter || g_GuildEventTypeGuildLogin.empty())
        return;

    Guild* guild = player->GetGuild();
    if (!guild)
        return;

    // Only real players; a wave of bot logins would spam the guild channel.
    // Must be the session test: the bot's AI is not attached yet at this point.
    if (OllamaIsBotPlayer(player))
        return;

    eventChatter.DispatchGameEvent(player, g_GuildEventTypeGuildLogin, guild->GetName());
}
