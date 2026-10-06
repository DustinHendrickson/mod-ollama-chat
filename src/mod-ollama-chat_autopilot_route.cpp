#include "mod-ollama-chat_autopilot_route.h"

#include "Config.h"
#include "Map.h"
#include "PathGenerator.h"
#include "Player.h"

#include <algorithm>
#include <cmath>

// The two-pass design and its constants come from mod-city-siege's
// CitySiegePathing.cpp, which learned them the hard way; see the notes there
// for why corridor heights are thrown away and why walkability is left to
// Detour rather than second-guessed from Z values.

namespace
{
    struct RouteConfig
    {
        uint32_t queriesPerVisit = 6;
        float    nodeSpacing     = 28.0f;
        float    steepCost       = 25.0f;
        float    waterCost       = 8.0f;
        uint32_t lookaheadNodes  = 12;
    };

    RouteConfig g_rc;   // world thread only

    constexpr float    ARRIVAL       = 6.0f;    // close enough to an aim point
    constexpr float    MIN_PROGRESS  = 2.5f;    // below this a query got nowhere
    constexpr float    LEG_MAX       = 120.0f;  // straight-line reach per smooth query
    constexpr float    LEG_MIN       = 6.0f;    // give up shortening below this
    constexpr float    LOOP_RADIUS   = 18.0f;   // de-loop a corridor
    constexpr float    TURN_COSINE   = 0.94f;   // ~20 degrees counts as a turn
    constexpr float    NODE_REACHED  = 8.0f;    // the bot is at a node
    constexpr float    TILES_LOADED  = 90.0f;   // nearer the bot than this, a failure is real
    constexpr uint32_t MAX_STALLS    = 3;
    constexpr float    HAND_AHEAD    = 55.0f;   // one pathfinding query covers it easily
    constexpr float    CORRIDOR_MAX  = 250.0f;  // how far ahead one corridor query aims
    constexpr float    CORRIDOR_MIN  = 40.0f;   // shortest reach before "not loaded" is final

    float D2(const AutopilotRoutePoint& a, const AutopilotRoutePoint& b)
    {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    AutopilotRoutePoint At(Player* bot)
    {
        return { bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() };
    }

    // Prefer the ground a traveller would actually take: roads over
    // hillsides, bridges over rivers, never lava. Costs, not exclusions, so a
    // ramp that is the only way up still works.
    void Filter(PathGenerator& generator)
    {
        generator.SetNavTerrainCost(NAV_GROUND_STEEP, g_rc.steepCost);
        generator.SetNavTerrainCost(NAV_WATER, g_rc.waterCost);
        generator.SetExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));
    }

    // A corner's XY is the useful part; its Z is whatever surface Detour
    // snapped to, often the hillside above a road. Search down from just above
    // the height we are walking at instead.
    AutopilotRoutePoint Reseat(Map* map, const AutopilotRoutePoint& p, float walkingZ)
    {
        if (map)
        {
            const float ground = map->GetHeight(p.x, p.y, walkingZ + 6.0f, true, 80.0f);
            if (ground > INVALID_HEIGHT)
                return { p.x, p.y, ground };
        }
        return { p.x, p.y, walkingZ };
    }

    void RemoveLoops(std::vector<AutopilotRoutePoint>& points)
    {
        for (size_t i = 0; i + 2 < points.size(); ++i)
            for (size_t j = points.size() - 1; j > i + 1; --j)
                if (D2(points[i], points[j]) <= LOOP_RADIUS)
                {
                    points.erase(points.begin() + i + 1, points.begin() + j + 1);
                    break;
                }
    }

    // Thin the dense smooth path into nodes as it arrives, keeping turns.
    void AddDense(AutopilotRoute& r, const AutopilotRoutePoint& p)
    {
        const float travelled = D2(r.lastKept, p);
        if (travelled < 0.01f)
            return;

        const float fx = (p.x - r.lastKept.x) / travelled;
        const float fy = (p.y - r.lastKept.y) / travelled;
        const bool turned = r.haveHeading && (fx * r.headingX + fy * r.headingY) < TURN_COSINE;

        if (travelled >= g_rc.nodeSpacing || (turned && travelled >= 8.0f))
        {
            r.nodes.push_back(p);
            r.lastKept    = p;
            r.headingX    = fx;
            r.headingY    = fy;
            r.haveHeading = true;
        }
    }

    enum class Step { Progress, Exhausted, Done, Fail };

    // Pass 1: one corridor leg from the walk cursor toward the destination.
    Step CorridorLeg(Player* bot, AutopilotRoute& r)
    {
        ++r.queries;
        PathGenerator generator(bot);
        generator.SetUseStraightPath(true);
        Filter(generator);

        r.corridor.clear();
        r.corner = 0;

        // Aim at most corridorReach ahead, never straight at a far destination:
        // the core answers any query whose end lies on a navmesh tile that is
        // not loaded (most of the map, away from players) with a straight-line
        // shortcut marked NOT_USING_PATH -- which used to read as "no mmaps"
        // and sent the bot in a beeline over the mountains.
        AutopilotRoutePoint target = r.dest;
        const float toDest = D2(r.cursor, r.dest);
        r.corridorToDest = toDest <= r.corridorReach;
        if (!r.corridorToDest)
        {
            const float t = r.corridorReach / toDest;
            target = Reseat(bot->GetMap(), { r.cursor.x + (r.dest.x - r.cursor.x) * t,
                                             r.cursor.y + (r.dest.y - r.cursor.y) * t, r.cursor.z }, r.cursor.z);
        }

        if (!generator.CalculatePath(r.cursor.x, r.cursor.y, r.cursor.z, target.x, target.y, target.z, false))
        {
            r.why = "invalid coordinates";
            return Step::Fail;
        }

        const PathType type = generator.GetPathType();
        if (type & PATHFIND_NOT_USING_PATH)
        {
            // Truly no navmesh only if even a step beside the cursor has none.
            PathGenerator probe(bot);
            probe.CalculatePath(r.cursor.x, r.cursor.y, r.cursor.z, r.cursor.x + 2.0f, r.cursor.y, r.cursor.z, false);
            ++r.queries;
            if (probe.GetPathType() & PATHFIND_NOT_USING_PATH)
            {
                r.why = "no mmaps";
                return Step::Fail;
            }

            // The far end's tile is not loaded yet: aim nearer and try again.
            if (r.corridorReach > CORRIDOR_MIN)
            {
                r.corridorReach = std::max(CORRIDOR_MIN, r.corridorReach * 0.5f);
                return Step::Progress;   // corridor still empty: the next query retries
            }
            r.why = "the way ahead is not loaded yet";
            return Step::Exhausted;
        }
        r.corridorReach = CORRIDOR_MAX;
        if (type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT))
        {
            r.why = "no path on the navmesh";
            return Step::Exhausted;
        }

        for (const G3D::Vector3& p : generator.GetPath())
        {
            AutopilotRoutePoint c{ p.x, p.y, p.z };
            if (D2(c, r.cursor) >= MIN_PROGRESS)
                r.corridor.push_back(c);
        }
        RemoveLoops(r.corridor);

        if (r.corridor.empty())
        {
            r.why = "the navmesh leads nowhere from here";
            return Step::Exhausted;
        }
        return Step::Progress;
    }

    // One smooth, ground-following query along a corridor corner.
    bool SmoothLeg(Player* bot, AutopilotRoute& r, const AutopilotRoutePoint& to)
    {
        ++r.queries;
        PathGenerator generator(bot);
        generator.SetUseStraightPath(false);
        Filter(generator);

        if (!generator.CalculatePath(r.cursor.x, r.cursor.y, r.cursor.z, to.x, to.y, to.z, false))
            return false;
        if (generator.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH))
            return false;

        const Movement::PointsArray& points = generator.GetPath();
        if (points.size() < 2)
            return false;

        const G3D::Vector3& end = generator.GetActualEndPosition();
        const AutopilotRoutePoint reached{ end.x, end.y, end.z };
        if (D2(r.cursor, reached) < MIN_PROGRESS)
            return false;

        for (size_t i = 1; i < points.size(); ++i)
            AddDense(r, { points[i].x, points[i].y, points[i].z });
        r.cursor = reached;
        return true;
    }

    // Pass 2: walk toward the current corner (or the destination once the
    // corridor is used up), shortening the reach until a query fits.
    Step WalkStep(Player* bot, AutopilotRoute& r)
    {
        // A corridor that stops short of the destination is followed by the
        // next one, not by a straight line at a destination far away.
        if (r.corner >= r.corridor.size() && !r.corridorToDest)
        {
            r.corridor.clear();
            r.corner = 0;
            return Step::Progress;
        }

        const bool toDest = r.corner >= r.corridor.size();
        const AutopilotRoutePoint aim = Reseat(bot->GetMap(), toDest ? r.dest : r.corridor[r.corner], r.cursor.z);
        const float remaining = D2(r.cursor, aim);

        if (remaining <= ARRIVAL)
        {
            if (toDest)
                return Step::Done;
            ++r.corner;
            r.reach = 0.0f;
            return Step::Progress;
        }

        if (r.reach <= 0.0f)
            r.reach = std::min(remaining, LEG_MAX);

        AutopilotRoutePoint step = aim;
        if (r.reach < remaining)
        {
            const float t = r.reach / remaining;
            step = Reseat(bot->GetMap(), { r.cursor.x + (aim.x - r.cursor.x) * t,
                                           r.cursor.y + (aim.y - r.cursor.y) * t,
                                           r.cursor.z + (aim.z - r.cursor.z) * t }, r.cursor.z);
        }

        if (SmoothLeg(bot, r, step))
        {
            r.reach = 0.0f;
            return Step::Progress;
        }

        r.reach *= 0.5f;
        if (r.reach >= LEG_MIN)
            return Step::Progress;

        // No way toward this aim. Skip a corner; at the destination, the
        // corridor is used up.
        r.reach = 0.0f;
        if (!toDest)
        {
            ++r.corner;
            return Step::Progress;
        }
        return Step::Exhausted;
    }
}

void AutopilotRoute_LoadConfig()
{
    RouteConfig c;
    c.queriesPerVisit = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Route.QueriesPerVisit", 6));
    c.nodeSpacing     = std::clamp(sConfigMgr->GetOption<float>("OllamaChat.Autopilot.Route.NodeSpacing", 28.0f), 8.0f, 60.0f);
    c.steepCost       = std::clamp(sConfigMgr->GetOption<float>("OllamaChat.Autopilot.Route.SteepCost", 25.0f), 1.0f, 1000.0f);
    c.waterCost       = std::clamp(sConfigMgr->GetOption<float>("OllamaChat.Autopilot.Route.WaterCost", 8.0f), 1.0f, 1000.0f);
    g_rc = c;
}

void AutopilotRoute_Begin(Player* bot, uint32_t map, float x, float y, float z, AutopilotRoute& route)
{
    const uint32_t rebuilds = route.rebuilds;
    route          = AutopilotRoute();
    route.map      = map;
    route.dest     = { x, y, z };
    route.cursor   = At(bot);
    route.lastKept = route.cursor;
    route.rebuilds = rebuilds;
}

void AutopilotRoute_Rebuild(Player* bot, AutopilotRoute& route)
{
    const AutopilotRoutePoint dest = route.dest;
    const uint32_t map = route.map;
    ++route.rebuilds;
    AutopilotRoute_Begin(bot, map, dest.x, dest.y, dest.z, route);
}

void AutopilotRoute_Extend(Player* bot, AutopilotRoute& r)
{
    if (r.complete || r.failed || bot->GetMapId() != r.map)
        return;
    if (r.nodes.size() - r.next >= g_rc.lookaheadNodes)
        return;
    if (r.waiting)
    {
        if (D2(At(bot), r.cursor) > TILES_LOADED)
            return;
        r.waiting = false;
    }

    for (uint32_t spent = 0; spent < g_rc.queriesPerVisit; ++spent)
    {
        Step step;
        if (r.corridor.empty())
        {
            r.legStart = r.cursor;
            step = CorridorLeg(bot, r);
            // No corridor: aim the walk straight at the destination. Smooth
            // queries often get round what the straight query could not.
            if (step == Step::Exhausted)
            {
                r.corridor.push_back(r.dest);
                r.corridorToDest = true;
                step = Step::Progress;
            }
        }
        else
        {
            step = WalkStep(bot, r);
        }

        if (step == Step::Done)
        {
            if (r.nodes.empty() || D2(r.nodes.back(), r.cursor) > 1.0f)
                r.nodes.push_back(r.cursor);
            r.complete = true;
            return;
        }

        if (step == Step::Exhausted)
        {
            // The corridor is used up short of the destination: ask for a new
            // one from here, unless the last one got us nowhere.
            r.stalls = D2(r.legStart, r.cursor) < MIN_PROGRESS ? r.stalls + 1 : 0;
            r.corridor.clear();
            r.corner = 0;
            if (r.stalls < MAX_STALLS)
                continue;
            step = Step::Fail;
            if (r.why.empty())
                r.why = "no walkable way found";
        }

        if (step == Step::Fail)
        {
            // Out ahead of the bot the navmesh tiles may simply not be loaded
            // yet (they load with their grid). Wait until the bot catches up
            // and try again; only a failure close to the bot is real.
            if (r.why != "no mmaps" && r.why != "invalid coordinates" &&
                D2(At(bot), r.cursor) > TILES_LOADED)
            {
                r.corridor.clear();
                r.corner  = 0;
                r.waiting = true;
                return;
            }
            r.failed = true;
            return;
        }
    }
}

bool AutopilotRoute_Next(Player* bot, AutopilotRoute& r, AutopilotRoutePoint& out, size_t& index)
{
    const AutopilotRoutePoint here = At(bot);
    while (r.next < r.nodes.size() && D2(here, r.nodes[r.next]) <= NODE_REACHED)
        ++r.next;

    // Cut a corner the bot has already rounded.
    if (r.next + 1 < r.nodes.size() && D2(here, r.nodes[r.next + 1]) < D2(here, r.nodes[r.next]) &&
        D2(here, r.nodes[r.next + 1]) <= g_rc.nodeSpacing)
        ++r.next;

    if (r.next >= r.nodes.size())
        return false;

    index = r.next;
    while (index + 1 < r.nodes.size() && D2(here, r.nodes[index + 1]) <= HAND_AHEAD)
        ++index;
    out = r.nodes[index];
    return true;
}
