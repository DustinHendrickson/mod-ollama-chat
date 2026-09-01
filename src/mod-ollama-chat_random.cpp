#include "mod-ollama-chat_random.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_topics.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Channel.h"
#include "ChannelMgr.h"
#include "Guild.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "AiFactory.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <ctime>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    std::mutex g_scheduleMutex;
    std::unordered_map<uint64_t, time_t> g_nextRandomChatTime;

    // urand takes uint32; container sizes are size_t.
    size_t PickIndex(size_t count)
    {
        if (count <= 1)
            return 0;
        return static_cast<size_t>(urand(0, static_cast<uint32>(count - 1)));
    }

    // Decide where an ambient line would go, before spending an LLM call on it.
    bool ChooseDestination(Player* bot, const OllamaWorldSnapshot& world, bool guildTopic,
                           ChatChannelSourceLocal& outSource,
                           std::string& outChannelName,
                           uint32_t& outChannelId)
    {
        outChannelName.clear();
        outChannelId = 0;

        if (guildTopic && bot->GetGuild() && !g_DisableForGuild &&
            world.GuildHasRealPlayer(bot->GetGuildId()))
        {
            outSource = SRC_GUILD_LOCAL;
            return true;
        }

        // Was: any group at all. A party of nothing but bots is not an
        // audience, and this path had no check whatsoever -- a bot that
        // qualified for the tick because a guildmate was online could then
        // spend a generation talking to five other bots.
        if (bot->GetGroup() && !g_DisableForParty && OllamaGroupHasRealPlayer(bot))
        {
            outSource = SRC_PARTY_LOCAL;
            return true;
        }

        struct Option
        {
            ChatChannelSourceLocal source;
            std::string            channelName;
            uint32_t               channelId;
        };
        std::vector<Option> options;

        if (!g_DisableForSayYell && world.RealPlayerWithin(bot, g_SayDistance))
            options.push_back({ SRC_SAY_LOCAL, std::string(), 0 });

        // RealPlayerInZoneAndFaction is only a cheap pre-filter here. Being in
        // the bot's zone is not the same as being in the channel -- players
        // leave General -- so each resolved channel is checked for a real
        // listener below before it becomes a candidate.
        if (!g_DisableForCustomChannels && world.RealPlayerInZoneAndFaction(bot))
        {
            // Resolve real zone/city channels and carry their ACTUAL names
            // forward. Looking up "General" by name never matches (channels are
            // "General - Elwynn Forest"), and using the literal would also put
            // ambient lines in a different governor scope than replies in the
            // same channel.
            //
            // Trade and GuildRecruitment only exist in cities, so they simply
            // resolve to nullptr elsewhere and drop out of the options.
            auto addChannel = [&](uint32_t chanId, bool enabled)
            {
                if (!enabled)
                    return;

                Channel* ch = OllamaResolveZoneChannel(bot, chanId);
                if (!ch)
                    return;

                // The audience test that matters, and it happens here --
                // before any prompt is built and before the LLM is touched.
                if (!world.RealPlayerInChannel(ch))
                    return;

                options.push_back({ SRC_GENERAL_LOCAL, ch->GetName(), ch->GetChannelId() });
            };

            addChannel(ChatChannelId::GENERAL,           g_ChatterUseGeneralChannel);
            addChannel(ChatChannelId::TRADE,             g_ChatterUseTradeChannel);
            addChannel(ChatChannelId::LOOKING_FOR_GROUP, g_ChatterUseLfgChannel);
            addChannel(ChatChannelId::GUILD_RECRUITMENT, g_ChatterUseGuildRecruitmentChannel);
        }

        if (options.empty())
            return false;

        const Option& chosen = options[PickIndex(options.size())];
        outSource      = chosen.source;
        outChannelName = chosen.channelName;
        outChannelId   = chosen.channelId;
        return true;
    }

    std::string BuildRandomChatterPrompt(Player* bot, const std::string& environmentInfo)
    {
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI || !botAI->GetChatHelper())
            return "";

        const std::string personality       = GetBotPersonality(bot);
        const std::string personalityPrompt = GetPersonalityPromptAddition(personality);

        AreaTableEntry const* area = botAI->GetCurrentArea();
        AreaTableEntry const* zone = botAI->GetCurrentZone();

        std::string prompt = SafeFormat(
            g_RandomChatterPromptTemplate,
            fmt::arg("bot_name", bot->GetName()),
            fmt::arg("bot_level", bot->GetLevel()),
            fmt::arg("bot_class", botAI->GetChatHelper()->FormatClass(bot->getClass())),
            fmt::arg("bot_race", botAI->GetChatHelper()->FormatRace(bot->getRace())),
            fmt::arg("bot_gender", bot->getGender() == 0 ? "Male" : "Female"),
            fmt::arg("bot_role", CleanRoleForPrompt(ChatHelper::FormatClass(bot, AiFactory::GetPlayerSpecTab(bot)))),
            fmt::arg("bot_faction", bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde"),
            fmt::arg("bot_area", area ? PlayerbotAI::GetLocalizedAreaName(area) : "UnknownArea"),
            fmt::arg("bot_zone", zone ? PlayerbotAI::GetLocalizedAreaName(zone) : "UnknownZone"),
            fmt::arg("bot_map", OllamaContinentName(bot)),
            fmt::arg("bot_personality", personalityPrompt),
            fmt::arg("bot_personality_name", personality),
            fmt::arg("environment_info", environmentInfo));

        // Statement or question. At roleplay strictness 2 the in-character
        // lists replace the shipped ones, which otherwise push bots toward
        // out-of-world player-forum talk.
        const std::vector<std::string>& statements =
            Roleplay_UseRoleplayVariations() ? g_RoleplayPromptVariations
                                             : g_RandomChatterPromptVariations;
        const std::vector<std::string>& questions =
            Roleplay_UseRoleplayVariations() ? g_RoleplayQuestionVariations
                                             : g_RandomChatterQuestionVariations;

        const bool haveStatements = !statements.empty();
        const bool haveQuestions  = !questions.empty();

        if (haveStatements && haveQuestions)
        {
            const std::vector<std::string>& list =
                (urand(0, 99) < g_RandomChatterQuestionChance) ? questions : statements;
            prompt += " " + list[PickIndex(list.size())];
        }
        else if (haveStatements)
        {
            prompt += " " + statements[PickIndex(statements.size())];
        }
        else if (haveQuestions)
        {
            prompt += " " + questions[PickIndex(questions.size())];
        }

        prompt += Memory_BuildPromptSection(bot, nullptr);
        prompt += Roleplay_BuildVoicePrompt(bot);

        return prompt;
    }
}

// --------------------------------------------------------------------------

void OllamaRandomChatter_ForgetBot(ObjectGuid botGuid)
{
    std::lock_guard<std::mutex> lock(g_scheduleMutex);
    g_nextRandomChatTime.erase(botGuid.GetRawValue());
}

OllamaBotRandomChatter::OllamaBotRandomChatter() : WorldScript("OllamaBotRandomChatter") { }

void OllamaBotRandomChatter::OnUpdate(uint32 diff)
{
    if (!g_Enable)
        return;

    // The module's world tick. These run before any feature toggle can return
    // early, because pending replies still have to be delivered even when
    // random chatter itself is switched off.
    OllamaDispatch_Update(diff);

    static uint32 maintenanceTimer = 0;
    if (maintenanceTimer <= diff)
    {
        maintenanceTimer = 30000;
        Governor_Update();
        Topics_Update();
    }
    else
    {
        maintenanceTimer -= diff;
    }

    if (g_ConversationHistorySaveInterval > 0)
    {
        const time_t now = time(nullptr);
        if (difftime(now, g_LastHistorySaveTime) >= g_ConversationHistorySaveInterval * 60)
        {
            SaveBotConversationHistoryToDB();
            g_LastHistorySaveTime = now;
        }
    }

    if (g_MemoryEnable && g_MemorySaveInterval > 0)
    {
        const time_t now = time(nullptr);
        if (difftime(now, g_LastMemorySaveTime) >= g_MemorySaveInterval * 60)
        {
            Memory_SaveAll();
            g_LastMemorySaveTime = now;
        }
    }

    if (g_EnableSentimentTracking && g_SentimentSaveInterval > 0)
    {
        const time_t now = time(nullptr);
        if (difftime(now, g_LastSentimentSaveTime) >= g_SentimentSaveInterval * 60)
        {
            SaveBotPlayerSentimentsToDB();
            g_LastSentimentSaveTime = now;
        }
    }

    if (!g_EnableRandomChatter)
        return;

    static uint32 chatterTimer = 0;
    if (chatterTimer <= diff)
    {
        chatterTimer = 30000;
        HandleRandomChatter();
    }
    else
    {
        chatterTimer -= diff;
    }
}

void OllamaBotRandomChatter::HandleRandomChatter()
{
    // One pass for the whole tick. GuildHasRealPlayerOnline() used to be a
    // full player walk per bot, so this was O(bots x players) every 30s.
    OllamaWorldSnapshot world;
    world.Build();

    if (world.Empty())
        return;   // nobody to talk to; do not burn LLM calls on an empty world

    auto const& allPlayers = ObjectAccessor::GetPlayers();

    const time_t now = time(nullptr);

    for (auto const& itr : allPlayers)
    {
        Player* bot = itr.second;
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
            continue;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai || !ai->IsBotAI())
            continue;

        if (g_DisableRepliesInCombat && bot->IsInCombat())
            continue;

        const uint64_t rawGuid = bot->GetGUID().GetRawValue();

        // Guild bots with a guildmate online may talk without anyone standing
        // next to them; everyone else needs an audience in range.
        const bool guildAudience = world.GuildHasRealPlayer(bot->GetGuildId());
        const bool nearRealPlayer =
            world.RealPlayerWithin(bot, g_RandomChatterRealPlayerDistance);

        if (!guildAudience && !nearRealPlayer)
            continue;

        // Schedule.
        {
            std::lock_guard<std::mutex> lock(g_scheduleMutex);
            auto it = g_nextRandomChatTime.find(rawGuid);
            if (it == g_nextRandomChatTime.end())
            {
                g_nextRandomChatTime[rawGuid] = now + urand(g_MinRandomInterval, g_MaxRandomInterval);
                continue;
            }
            if (now < it->second)
                continue;
        }

        auto reschedule = [&]()
        {
            std::lock_guard<std::mutex> lock(g_scheduleMutex);
            g_nextRandomChatTime[rawGuid] = now + urand(g_MinRandomInterval, g_MaxRandomInterval);
        };

        if (urand(0, 99) >= g_RandomChatterBotCommentChance)
        {
            reschedule();
            continue;
        }

        // Pick what to talk about. Weighted toward the people and the world
        // around the bot rather than its own inventory.
        TopicPick topic = Topics_Pick(bot, world);
        if (!topic.valid)
        {
            reschedule();
            continue;
        }

        ChatChannelSourceLocal source = SRC_SAY_LOCAL;
        std::string channelName;
        uint32_t channelId = 0;
        if (!ChooseDestination(bot, world, topic.isGuildTopic, source, channelName, channelId))
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} has nowhere to speak; skipping ambient line.",
                         bot->GetName());
            reschedule();
            continue;
        }

        // Same inputs the reply path uses, so ambient lines and replies in the
        // same channel share one cooldown, rate limit and repetition history.
        const std::string scopeKey = Governor_MakeScopeKey(
            ChatChannelSourceLocalStr[source],
            channelId, channelName,
            source == SRC_GUILD_LOCAL ? bot->GetGuildId() : 0,
            bot->GetZoneId());

        if (!Governor_CanSend(bot->GetGUID(), scopeKey))
        {
            reschedule();
            continue;
        }

        std::string prompt = BuildRandomChatterPrompt(bot, topic.text);
        if (prompt.empty())
        {
            reschedule();
            continue;
        }

        OllamaChatRequest request;
        request.botGuid     = rawGuid;
        request.targetGuid  = 0;
        request.source      = source;
        request.channelName = channelName;
        request.channelId   = channelId;
        request.chainDepth  = 0;
        request.scopeKey    = scopeKey;
        request.prompt      = std::move(prompt);
        request.botName     = bot->GetName();
        request.kind        = OllamaRequestKind::RandomChatter;
        request.triggerBotReplies = true;

        if (OllamaDispatch_Submit(std::move(request)))
            Topics_NoteUsed(bot->GetGUID(), topic.key);

        reschedule();
    }
}
