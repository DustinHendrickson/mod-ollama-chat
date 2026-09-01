#include "mod-ollama-chat_world.h"

#include "AreaDefines.h"
#include "Channel.h"
#include "Map.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

bool OllamaIsBotPlayer(Player* player)
{
    if (!player)
        return false;

    // Correct from session construction, unlike the AI lookup below.
    if (WorldSession* session = player->GetSession())
    {
        if (session->IsBot())
            return true;
    }

    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
    return ai && ai->IsBotAI();
}

std::string OllamaContinentName(Player* player)
{
    Map* map = player ? player->GetMap() : nullptr;
    if (!map)
        return "UnknownMap";

    // The Burning Crusade starting zones sit on Outland's map but belong to
    // the old continents in the fiction, which is what a character would say.
    if (map->GetId() == MAP_OUTLAND)
    {
        switch (player->GetZoneId())
        {
            case AREA_EVERSONG_WOODS:
            case AREA_GHOSTLANDS:
            case AREA_SILVERMOON_CITY:
            case AREA_ISLE_OF_QUEL_DANAS:
                return "Eastern Kingdoms";

            case AREA_AZUREMYST_ISLE:
            case AREA_BLOODMYST_ISLE:
            case AREA_THE_EXODAR:
                return "Kalimdor";

            default:
                break;
        }
    }

    return map->GetMapName();
}

void OllamaWorldSnapshot::Build()
{
    realPlayers.clear();
    guildsWithRealPlayer.clear();

    auto const& all = ObjectAccessor::GetPlayers();
    realPlayers.reserve(16);

    for (auto const& pair : all)
    {
        Player* player = pair.second;
        if (!player || !player->IsInWorld())
            continue;

        if (OllamaIsBotPlayer(player))
            continue;

        realPlayers.push_back(player);

        if (uint32_t guildId = player->GetGuildId())
            guildsWithRealPlayer.insert(guildId);
    }
}

bool OllamaWorldSnapshot::RealPlayerWithin(Player* who, float distance) const
{
    if (!who || distance <= 0.0f || !who->IsInWorld())
        return false;

    for (Player* player : realPlayers)
    {
        if (player == who)
            continue;
        if (player->GetMapId() != who->GetMapId())
            continue;
        if (who->GetDistance(player) <= distance)
            return true;
    }
    return false;
}

bool OllamaWorldSnapshot::RealPlayerInChannel(Channel* channel) const
{
    if (!channel)
        return false;

    for (Player* player : realPlayers)
        if (player->IsInChannel(channel))
            return true;

    return false;
}

bool OllamaGroupHasRealPlayer(Player* who)
{
    Group* group = who ? who->GetGroup() : nullptr;
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        if (OllamaIsRealPlayer(ref->GetSource()))
            return true;

    return false;
}

bool OllamaWorldSnapshot::RealPlayerInZoneAndFaction(Player* who) const
{
    if (!who)
        return false;

    for (Player* player : realPlayers)
    {
        if (player == who)
            continue;
        if (player->GetTeamId() == who->GetTeamId() &&
            player->GetZoneId() == who->GetZoneId())
            return true;
    }
    return false;
}
