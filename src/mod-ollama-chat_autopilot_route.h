#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_ROUTE_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_ROUTE_H

#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// Long walks for autopilot errands, built on the navmesh.
//
// Playerbots' own long-distance move (NewRpg MoveFarTo) walks straight only
// under 70 yards. Past that it takes whatever one mmap query returns (capped
// around 300 yards) or a random step in the right general direction, and after
// 90 seconds without progress it teleports. Over real terrain -- a mountain
// range, a city wall, a river -- most walks of more than a few hundred yards
// end in that teleport.
//
// Autopilot does not use MoveFarTo (NewRpg is a controller the LLM replaces);
// an errand walks a route instead, the way mod-city-siege marches its
// armies (CitySiegePathing.cpp), in two passes:
//
//   corridor  findStraightPath from where the route has got to toward the
//             destination. Its corners say which way round obstacles to go;
//             their heights are not trusted.
//   walk      smooth, ground-following paths from corner to corner, at most
//             120 yards per query, halved until a query fits. Every point is
//             genuinely walkable. Thinned to nodes about 28 yards apart.
//
// The bot is then walked one node at a time (AutopilotMove_To), each a short,
// plainly pathfindable hop.
//
// Unlike a siege route, this is built lazily, a few hundred yards ahead of the
// bot, a few queries per visit: navmesh tiles are only loaded with their map
// grid, and grids load around the bot as it walks. It also keeps per-tick cost
// bounded however long the walk is.
//
// Same continent only. Other maps are reached by boat, zeppelin or portal,
// which is not a walk.
//
// World thread only (Autopilot_Update runs after MapMgr::Update has joined its
// workers, so the navmesh queries do not race a map thread).
// --------------------------------------------------------------------------

struct AutopilotRoutePoint
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct AutopilotRoute
{
    uint32_t            map = 0;
    AutopilotRoutePoint dest;

    // Pass 1: corners of the current corridor leg (XY guidance).
    std::vector<AutopilotRoutePoint> corridor;
    size_t   corner      = 0;
    uint32_t emptyLegs   = 0;   // corridor queries that got nowhere
    uint32_t stalls      = 0;   // corridors used up without progress, in a row
    float    corridorReach  = 250.0f;  // shortened while the far end's tile is not loaded
    std::vector<AutopilotRoutePoint> anchors;   // waypoints along playerbots' road network
    size_t   anchor         = 0;
    AutopilotRoutePoint goal;           // what the current corridor aims at
    bool     goalIsEnd      = true;     // that is the destination, not an anchor
    bool     corridorToDest = false;   // the current corridor ends at the destination
    AutopilotRoutePoint legStart;   // the walk cursor when the corridor was made

    // Pass 2: where the walk has got to, and the step being tried.
    AutopilotRoutePoint cursor;
    float    reach       = 0.0f;
    AutopilotRoutePoint lastKept;
    float    headingX = 0.0f, headingY = 0.0f;
    bool     haveHeading = false;

    // What the bot walks.
    std::vector<AutopilotRoutePoint> nodes;
    size_t   next        = 0;

    bool        complete = false;   // nodes reach the destination
    bool        failed   = false;
    std::string why;

    bool     waiting     = false;   // until the bot nears the cursor (tiles load)
    uint32_t queries     = 0;       // navmesh queries spent so far
    uint32_t rebuilds    = 0;

    bool Empty() const { return nodes.empty() && complete; }
};

void AutopilotRoute_LoadConfig();

class PathGenerator;

// The navmesh filter every autopilot path uses, routes and moves alike: the
// core's own filter for bots (no steep slopes, no lava, water expensive),
// never loosened; Route.WaterCost can only make water costlier.
void AutopilotRoute_Filter(Player* bot, PathGenerator& generator);

// Start a route from the bot to (map, x, y, z). Spends no queries.
void AutopilotRoute_Begin(Player* bot, uint32_t map, float x, float y, float z, AutopilotRoute& route);

// Start again from where the bot stands now (after it got stuck).
void AutopilotRoute_Rebuild(Player* bot, AutopilotRoute& route);

// Extend the route if the bot is getting close to the end of what is built,
// spending at most the configured number of navmesh queries.
void AutopilotRoute_Extend(Player* bot, AutopilotRoute& route);

// The node the bot should walk to now: the farthest built node within about
// 55 yards, so one move covers a couple of nodes (one pathfinding query
// covers that easily) and a bot visited rarely does not stop at every node.
// `index` gets its position. False while nothing is built ahead yet, or once
// the last node is reached.
bool AutopilotRoute_Next(Player* bot, AutopilotRoute& route, AutopilotRoutePoint& out, size_t& index);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_ROUTE_H
