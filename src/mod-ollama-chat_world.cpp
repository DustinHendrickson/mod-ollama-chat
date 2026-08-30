#include "mod-ollama-chat_world.h"

#include "ObjectAccessor.h"
#include "Player.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

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

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        if (ai && ai->IsBotAI())
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
