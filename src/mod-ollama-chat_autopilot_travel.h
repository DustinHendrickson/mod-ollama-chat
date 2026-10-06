#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_TRAVEL_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_TRAVEL_H

#include "mod-ollama-chat_autopilot_route.h"

#include <cstdint>
#include <string>
#include <vector>

class Player;
class PlayerbotAI;

// --------------------------------------------------------------------------
// Getting an autopilot bot somewhere: walking, flight masters, boats and
// zeppelins. Used by the LLM's `goto` and `quest` orders.
//
// A trip is a short list of legs, planned from where the bot is now and
// planned again after every flight or crossing (so each plan only has to see
// as far as the next change of vehicle):
//
//   other continent   the first crossing on a shortest chain (breadth-first
//                     over continents) the bot's faction may use:
//                       boat or zeppelin: walk to the dock -> board -> ride ->
//                       step off (no Alliance bot at a Horde tower)
//                       the Dark Portal: walk into it
//                       a city portal: walk up to it and use it
//                     then replan.
//   same continent,   walk to the nearest flight master -> fly to the taxi
//   far away          node nearest the destination; replan. Only when it
//                     saves real distance; if the flight cannot be bought,
//                     the bot walks.
//   otherwise         walk (navmesh route for anything over 60 yards), then
//                     step up to the NPC if there is one.
//
// Movement is the bot's own (AutopilotMove_To), not playerbots' NewRpg: the
// LLM is the bot's controller. Boarding relies on playerbots attaching a bot
// to the transport it stands on (PlayerbotAI::UpdateAI, every second); the
// core carries passengers across maps.
//
// World thread only.
// --------------------------------------------------------------------------

struct AutopilotTravelPoint
{
    uint32_t map = 0;
    float    x = 0.0f, y = 0.0f, z = 0.0f;
};

enum class AutopilotLegType : uint8_t
{
    Walk,       // to `to`, arriving within `radius`
    Fly,        // at the flight master `entry`, take `taxiPath`
    Board,      // wait at the dock for transport `entry` docked at `to`, get on
    Ride,       // stay on until it docks at `arrive` on `arriveMap`, step off at `land`
    Approach,   // step up to the nearest creature `entry`
    Trigger,    // walk into area trigger `entry` (the Dark Portal) to reach `arrive`
    Portal      // use portal gameobject `entry` to reach `arrive`
};

struct AutopilotLeg
{
    AutopilotLegType      type = AutopilotLegType::Walk;
    AutopilotTravelPoint  to;
    float                 radius = 6.0f;
    uint32_t              entry  = 0;
    std::vector<uint32_t> taxiPath;
    AutopilotTravelPoint  arrive;      // Ride: the far stop
    AutopilotTravelPoint  land;        // Ride: where to step off
    std::string           label;       // "the zeppelin to Orgrimmar"
};

struct AutopilotTrip
{
    bool                 active = false;
    AutopilotTravelPoint dest;
    float                arriveRadius = 25.0f;
    uint32_t             npcEntry = 0;          // step up to it at the end

    std::vector<AutopilotLeg> legs;
    size_t               leg = 0;
    uint32_t             legStartedAt = 0;
    uint32_t             plans = 0;              // replans so far
    bool                 noFlight = false;       // a flight failed: walk
    bool                 flown = false;          // flew since the last crossing

    // Walk leg state.
    bool                 routed = false;
    AutopilotRoute       route;
    size_t               issued = SIZE_MAX;
    float                best = 0.0f;            // nearest to the leg target so far
    size_t               lastReached = 0;         // route points reached so far
    uint32_t             bestAt = 0;

    // Vehicle state.
    bool                 tookOff = false;
    bool                 boarded = false;
    bool                 stepping = false;       // stepping off the deck
    bool                 deckProbed = false;     // looked for a deck point this docking
    bool                 deckFound  = false;
    AutopilotTravelPoint deck;

    // Where the transport was at the last look, to tell when it has stopped.
    float                tx = 0.0f, ty = 0.0f;
    uint32_t             tSampleAt = 0;

    bool                 interrupted = false;    // a fight or a death paused the trip
};

void AutopilotTravel_LoadConfig();

// Index boats, zeppelins and their docks. Once at startup, after transports
// and creature spawns load.
void AutopilotTravel_Build();

enum class AutopilotTripState : uint8_t { Going, Arrived, Failed };

// Plan a trip. Returns "" or why it cannot be made ("no boat or zeppelin goes
// there", ...).
std::string AutopilotTravel_Start(Player* bot, const AutopilotTravelPoint& dest, float arriveRadius,
                                  uint32_t npcEntry, AutopilotTrip& trip, uint32_t now, bool allowFlights = true);

// Advance the trip. `note` gets a diary line for milestones and failures
// ("boarded The Purple Princess", "landed in Booty Bay", "stuck").
AutopilotTripState AutopilotTravel_Update(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, uint32_t now,
                                          std::string& note);

// The bot got stuck or was interrupted for good: stop moving, forget the trip.
void AutopilotTravel_Stop(PlayerbotAI* ai, AutopilotTrip& trip);

// For the prompt and status: "flying to Booty Bay", "waiting for The Bravery".
std::string AutopilotTravel_Describe(Player* bot, const AutopilotTrip& trip);

// True while the trip is at a dock or aboard: these need a look every sweep,
// or a busy realm's rotation can miss the minute a ship is docked.
bool AutopilotTravel_IsTimeCritical(const AutopilotTrip& trip);

// Whether the bot can get to this map at all (same map, or a boat/zeppelin
// chain usable by its faction).
bool AutopilotTravel_CanReach(Player* bot, uint32_t map);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_TRAVEL_H
