#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_WORLD_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_WORLD_H

#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// World knowledge for the autopilot's own commands.
//
// Playerbots has no "walk to the nearest repair vendor" or "walk to Feralas"
// command, so the LLM could not send a bot anywhere specific. This indexes
// every service NPC spawn once at startup (a few thousand entries, from the
// static spawn data -- no grid walks) so those destinations can be found
// cheaply on demand, and turns zone names into a walkable spot inside the
// zone: a friendly service NPC there.
//
// World thread only.
// --------------------------------------------------------------------------

enum class AutopilotService : uint8_t
{
    Repair,
    Vendor,
    Trainer,        // class trainer usable by this bot
    Profession,     // profession trainer
    Inn,
    FlightMaster,
    Bank,
    Auction,
    Count
};

const char* AutopilotService_Name(AutopilotService s);
bool        AutopilotService_FromName(const std::string& name, AutopilotService& out);

struct AutopilotPlace
{
    uint32_t    map   = 0;
    float       x     = 0.0f;
    float       y     = 0.0f;
    float       z     = 0.0f;
    uint32_t    entry = 0;
    std::string name;       // the NPC's name
    float       distance = 0.0f;
};

// Build the index. Call once at startup, after the creature spawn data loads.
void AutopilotWorld_Build();

// Nearest friendly NPC offering `service` on the bot's own map.
bool AutopilotWorld_NearestService(Player* bot, AutopilotService service, AutopilotPlace& out);

// A walkable spot in a zone: the friendly service NPC nearest the zone's
// centre. The zone may be on another continent; getting there is the travel
// planner's job (boats, zeppelins). Fills `why` on failure.
bool AutopilotWorld_ZonePlace(Player* bot, uint32_t zoneId, AutopilotPlace& out, std::string& why);

// Nearest spawn of a creature entry: on the bot's map if there is one,
// otherwise anywhere (distance 0). Any creature, not just services.
bool AutopilotWorld_NearestSpawn(Player* bot, uint32_t entry, AutopilotPlace& out);

// Creature entries that take this quest in.
const std::vector<uint32_t>& AutopilotWorld_QuestEnders(uint32_t questId);

// A top-level zone by name (exact match preferred). 0 if none.
uint32_t AutopilotWorld_FindZone(const std::string& name, std::string& display);

// For the prompt: the nearest of each service, e.g.
// "repair: Corina Steele (Goldshire, 140 yd); trainer: ...".
std::string AutopilotWorld_DescribeServices(Player* bot);

// For the prompt: zones on the bot's continent whose recommended level is
// near the bot's, e.g. "Westfall (10), Redridge Mountains (15)".
std::string AutopilotWorld_ZonesForLevel(Player* bot);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_WORLD_H
