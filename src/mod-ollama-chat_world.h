#ifndef MOD_OLLAMA_CHAT_WORLD_H
#define MOD_OLLAMA_CHAT_WORLD_H

#include <cstdint>
#include <unordered_set>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// One pass over the online player list, reused for the whole of a message or
// a chatter tick.
//
// The module repeatedly asked the same three questions -- "is a real player
// near this bot", "does this bot's guild have a real player online", "is a
// real player in this bot's zone and faction" -- and each one walked
// ObjectAccessor::GetPlayers() from scratch. Asked once per candidate bot,
// inside a loop that was itself over every online player, that is quadratic in
// online characters on every single chat message.
//
// Build one of these once, then answer all three in O(real players), which on
// a bot-heavy realm is a very small number.
//
// WORLD THREAD ONLY, and do not hold one across ticks: the Player pointers are
// only guaranteed valid for the tick that built it.
// --------------------------------------------------------------------------
struct OllamaWorldSnapshot
{
    std::vector<Player*>        realPlayers;            // non-bot, in world
    std::unordered_set<uint32_t> guildsWithRealPlayer;

    void Build();

    bool Empty() const { return realPlayers.empty(); }

    bool GuildHasRealPlayer(uint32_t guildId) const
    {
        return guildId != 0 && guildsWithRealPlayer.count(guildId) != 0;
    }

    // True when a real player is within `distance` of `who`, on the same map.
    bool RealPlayerWithin(Player* who, float distance) const;

    // True when a real player shares this bot's zone and faction, which is
    // what the zone-scoped General channel requires.
    bool RealPlayerInZoneAndFaction(Player* who) const;
};

#endif // MOD_OLLAMA_CHAT_WORLD_H
