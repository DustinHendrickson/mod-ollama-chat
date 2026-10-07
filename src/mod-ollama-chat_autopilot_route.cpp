#include "mod-ollama-chat_autopilot_route.h"
#include "mod-ollama-chat-utilities.h"

#include "Config.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "PathGenerator.h"

#include "TravelMgr.h"
#include "TravelNode.h"
#include "Player.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <shared_mutex>

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
        float    waterCost       = 20.0f;
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
    constexpr uint32_t MAX_STEP_OFFS = 4;     // straight steps off a slope, per route
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

    void Filter(Player* bot, PathGenerator& generator) { AutopilotRoute_Filter(bot, generator); }

    // A corner's XY is the useful part; its Z is whatever surface Detour
    // snapped to, often the hillside above a road. Search down from just above
    // the height we are walking at instead.
    // Is the navmesh tile under (x, y) loaded? The core answers a query that
    // ends on an unloaded tile and one that ends off the mesh (inside a wall,
    // in rock under a building) the same way, with a NOT_USING_PATH shortcut;
    // only the first is fixed by waiting.
    bool TileLoaded(Map* map, float x, float y)
    {
        dtNavMesh const* mesh = map ? map->GetMapCollisionData().GetMMapData().GetNavMesh() : nullptr;
        if (!mesh)
            return false;
        const float point[3] = { y, 0.0f, x };
        int tx = -1, ty = -1;
        mesh->calcTileLoc(point, &tx, &ty);
        return tx >= 0 && ty >= 0 && mesh->getTileAt(tx, ty, 0) != nullptr;
    }

    // Move an aim point onto the nearest walkable navmesh polygon within the
    // box -- the bot's own filter's ground, no steep slopes. A point aimed
    // straight ahead from inside a building lands in its walls.
    bool SnapToMesh(Map* map, AutopilotRoutePoint& p, float horizontal, float vertical)
    {
        dtNavMeshQuery const* query = map ? map->GetMapCollisionData().GetMMapData().GetNavMeshQuery() : nullptr;
        if (!query || !TileLoaded(map, p.x, p.y))
            return false;
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        const float center[3]  = { p.y, p.z, p.x };
        const float extents[3] = { horizontal, vertical, horizontal };
        dtPolyRef ref = 0;
        float nearest[3] = { 0.0f, 0.0f, 0.0f };
        if (dtStatusFailed(query->findNearestPoly(center, extents, &filter, &ref, nearest)) || !ref)
            return false;
        p = { nearest[2], nearest[0], nearest[1] };
        return true;
    }

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

    // A long walk follows playerbots' own road network (TravelNodeMap: hand
    // placed nodes with stored walking paths between them, the same data its
    // travel planner uses), so the bot takes the roads and the passes instead
    // of aiming straight at a destination behind a mountain range. Only a
    // route that walks every link on this continent is used; anything else (a
    // tram, a flight, a portal) is the travel planner's business, and the
    // corridors fall back to aiming at the destination.
    //
    // Not TravelNodeMap::getFullPath: it returns with the map's shared lock
    // still held when no route is found (freezing playerbots' node editing for
    // good) and allocates a node per call for the hearthstone leg. The
    // node-to-node A* below takes the lock itself, without waiting for it.
    void Anchor(Player* bot, AutopilotRoute& r)
    {
        if (D2(r.cursor, r.dest) < 150.0f)
        {
            r.anchorNote = "short walk: no road needed";
            return;
        }

        TravelNodeMap& nodeMap = TravelNodeMap::instance();
        std::shared_lock<std::shared_timed_mutex> lock(nodeMap.m_nMapMtx, std::try_to_lock);
        if (!lock.owns_lock())
        {
            r.anchorNote = "road network busy (playerbots is editing it)";
            return;
        }

        // The road network's nodes sit at towns, crossroads and the like, often
        // several hundred yards apart in open country: look that far.
        const WorldPosition from(bot);
        const WorldPosition to(r.map, r.dest.x, r.dest.y, r.dest.z);
        std::vector<TravelNode*> starts = nodeMap.getNodes(from, 600.0f);
        std::vector<TravelNode*> ends   = nodeMap.getNodes(to, 600.0f);
        if (starts.empty() || ends.empty())
        {
            r.anchorNote = starts.empty() ? "no road node within 600 yd of the bot"
                                          : "no road node within 600 yd of the destination";
            return;
        }
        if (starts.size() > 3) starts.resize(3);
        if (ends.size() > 3)   ends.resize(3);

        std::vector<TravelNode*> nodes;
        for (TravelNode* s : starts)
        {
            for (TravelNode* e : ends)
            {
                if (s == e)
                    continue;
                // No bot: with one, getRoute allocates a hearthstone portal
                // node on every call (and routes through it, which is no road).
                TravelNodeRoute found = nodeMap.getRoute(s, e, nullptr);
                if (!found.isEmpty())
                {
                    nodes = found.getNodes();
                    break;
                }
            }
            if (!nodes.empty())
                break;
        }
        if (nodes.size() < 2)
        {
            r.anchorNote = "no road route between the nearest nodes";
            return;
        }

        std::vector<AutopilotRoutePoint> points;
        auto keep = [&](const WorldPosition& w)
        {
            const AutopilotRoutePoint q{ w.GetPositionX(), w.GetPositionY(), w.GetPositionZ() };
            // One anchor every ~120 yards is plenty: the navmesh walks between.
            if (points.empty() || D2(points.back(), q) >= 120.0f)
                points.push_back(q);
        };
        // Follow the road as far as it walks on this map; a flight, boat or
        // portal link ends it there (the travel planner handles those).
        for (size_t i = 0; i + 1 < nodes.size(); ++i)
        {
            if (nodes[i]->getMapId() != r.map || nodes[i + 1]->getMapId() != r.map)
                break;
            auto* links = nodes[i]->getLinks();
            auto link = links->find(nodes[i + 1]);
            if (link == links->end() || link->second->getPathType() != TravelNodePathType::walk)
                break;
            keep(*nodes[i]->getPosition());
            for (const WorldPosition& w : link->second->getPath())
                keep(w);
            if (i + 2 == nodes.size())
                keep(*nodes.back()->getPosition());
        }
        if (points.size() < 2)
        {
            r.anchorNote = "the road route is not a walk from here";
            return;
        }
        r.anchorNote = SafeFormat("following the road: {} nodes", nodes.size());
        r.anchors = std::move(points);
        r.anchor  = 0;
    }

    enum class Step { Progress, Exhausted, Done, Fail };

    // Walkable ground within a few yards of the cursor, reached by a straight
    // step: the way off a slope the navmesh filter excludes.
    bool StepOff(Player* bot, AutopilotRoute& r)
    {
        if (r.stepOffs >= MAX_STEP_OFFS)
            return false;
        Map* map = bot->GetMap();
        for (float radius : { 4.0f, 7.0f })
        {
            for (int i = 0; i < 8; ++i)
            {
                const float angle = float(i) * float(M_PI) / 4.0f;
                const AutopilotRoutePoint c = Reseat(map, { r.cursor.x + std::cos(angle) * radius,
                                                            r.cursor.y + std::sin(angle) * radius, r.cursor.z },
                                                     r.cursor.z);
                if (std::fabs(c.z - r.cursor.z) > 3.0f ||
                    !map->isInLineOfSight(r.cursor.x, r.cursor.y, r.cursor.z + 2.0f, c.x, c.y, c.z + 2.0f,
                                          bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                    continue;

                ++r.queries;
                PathGenerator probe(bot);
                Filter(bot, probe);
                probe.CalculatePath(c.x, c.y, c.z, c.x + 2.0f, c.y, c.z, false);
                if (probe.GetPathType() & (PATHFIND_NOT_USING_PATH | PATHFIND_NOPATH))
                    continue;

                ++r.stepOffs;
                r.nodes.push_back(c);
                r.lastKept      = c;
                r.haveHeading   = false;
                r.cursor        = c;
                r.retryCorridor = true;
                return true;
            }
        }
        return false;
    }

    // Pass 1: one corridor leg from the walk cursor toward the destination.
    Step CorridorLeg(Player* bot, AutopilotRoute& r)
    {
        ++r.queries;
        PathGenerator generator(bot);
        generator.SetUseStraightPath(true);
        Filter(bot, generator);

        r.corridor.clear();
        r.corner = 0;

        // Aim at most corridorReach ahead, never straight at a far destination:
        // the core answers any query whose end lies on a navmesh tile that is
        // not loaded (most of the map, away from players) with a straight-line
        // shortcut marked NOT_USING_PATH -- which used to read as "no mmaps"
        // and sent the bot in a beeline over the mountains.
        // Head for the next road anchor (skipping ones reached or passed), or
        // the destination once there are none left.
        while (r.anchor < r.anchors.size() &&
               (D2(r.cursor, r.anchors[r.anchor]) < 25.0f ||
                (r.anchor + 1 < r.anchors.size() &&
                 D2(r.cursor, r.anchors[r.anchor + 1]) < D2(r.cursor, r.anchors[r.anchor]))))
            ++r.anchor;
        const bool viaAnchor = r.anchor < r.anchors.size();
        const AutopilotRoutePoint goal = viaAnchor ? r.anchors[r.anchor] : r.dest;
        r.goal      = goal;
        r.goalIsEnd = !viaAnchor;

        AutopilotRoutePoint target = goal;
        const float toGoal = D2(r.cursor, goal);
        r.corridorToDest = !viaAnchor && toGoal <= r.corridorReach;
        if (toGoal > r.corridorReach)
        {
            const float t = r.corridorReach / toGoal;
            target = Reseat(bot->GetMap(), { r.cursor.x + (goal.x - r.cursor.x) * t,
                                             r.cursor.y + (goal.y - r.cursor.y) * t, r.cursor.z }, r.cursor.z);
        }

        // Aim at walkable ground: a point straight ahead from inside a
        // building (the Exodar, a mine) is in its walls, and the query then
        // fails exactly as if the tile were not loaded.
        SnapToMesh(bot->GetMap(), target, 20.0f, 30.0f);

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
                // The cursor may just stand where the filter will not walk (a
                // steep slope): the query cannot start there, so it is no
                // proof the map has no navmesh. Step off onto walkable ground
                // nearby, straight, in sight and without a climb.
                if (StepOff(bot, r))
                    return Step::Progress;
                r.why = "no mmaps";
                return Step::Fail;
            }

            // The far end's tile is not loaded yet: aim nearer and try again.
            if (r.corridorReach > CORRIDOR_MIN)
            {
                r.corridorReach = std::max(CORRIDOR_MIN, r.corridorReach * 0.5f);
                r.retryCorridor = true;
                return Step::Progress;   // corridor still empty: the next query retries
            }
            r.why = TileLoaded(bot->GetMap(), target.x, target.y)
                ? "no walkable ground found toward it from here"
                : "the way ahead is not loaded yet";
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

    // One smooth, ground-following query along a corridor corner. Slope
    // checked first; where that stops short (a river bank counts as too steep
    // a step), the same query without it -- still on the core's bot filter,
    // which leaves out steep ground.
    bool SmoothQuery(Player* bot, AutopilotRoute& r, const AutopilotRoutePoint& to, bool slopeCheck,
                     bool onlyWetClimbs = false)
    {
        ++r.queries;
        PathGenerator generator(bot);
        generator.SetUseStraightPath(false);
        Filter(bot, generator);
        generator.SetSlopeCheck(slopeCheck);

        if (!generator.CalculatePath(r.cursor.x, r.cursor.y, r.cursor.z, to.x, to.y, to.z, false))
            return false;
        if (generator.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH))
            return false;

        const Movement::PointsArray& points = generator.GetPath();
        if (points.size() < 2)
            return false;
        // Without the slope check, a step too steep to walk is allowed only
        // where water is involved (a river bank); never up a mountainside.
        if (onlyWetClimbs && !AutopilotRoute_ClimbsOnlyWhereWet(bot, points))
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

    bool SmoothLeg(Player* bot, AutopilotRoute& r, const AutopilotRoutePoint& to)
    {
        return SmoothQuery(bot, r, to, true) || SmoothQuery(bot, r, to, false, true);
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
        // The destination keeps its own floor: seated from the walker's height,
        // an NPC upstairs became the floor beneath them.
        const AutopilotRoutePoint aim = toDest ? Reseat(bot->GetMap(), r.dest, r.dest.z)
                                               : Reseat(bot->GetMap(), r.corridor[r.corner], r.cursor.z);
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
            SnapToMesh(bot->GetMap(), step, 8.0f, 15.0f);   // a shortened step can land in a wall too
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
    c.waterCost       = std::clamp(sConfigMgr->GetOption<float>("OllamaChat.Autopilot.Route.WaterCost", 20.0f), 1.0f, 1000.0f);
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
    Anchor(bot, route);
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
        Step step = Step::Progress;
        if (r.corridor.empty())
        {
            // A corridor used up where it began got the walk nowhere. Aimed at
            // a road anchor, give that anchor up and head past it (the road
            // point may sit off the mesh); aimed at the destination, it is a
            // stall, and a few in a row end the route.
            if (r.legOpen && !r.retryCorridor)
            {
                if (D2(r.legStart, r.cursor) >= MIN_PROGRESS)
                    r.stalls = 0;
                else if (!r.goalIsEnd && r.anchor < r.anchors.size())
                    ++r.anchor;
                else if (++r.stalls >= MAX_STALLS)
                {
                    step = Step::Fail;
                    if (r.why.empty())
                        r.why = "no walkable way found";
                }
            }
            r.retryCorridor = false;

            if (step != Step::Fail)
            {
                r.legStart = r.cursor;
                r.legOpen  = true;
                step = CorridorLeg(bot, r);
                // No corridor: aim the walk at the current goal -- the next road
                // anchor when there is one, the destination only after the last.
                // Smooth queries often get round what the straight query could not.
                if (step == Step::Exhausted)
                {
                    r.corridor.push_back(r.goal);
                    r.corridorToDest = r.goalIsEnd;
                    step = Step::Progress;
                }
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
            // one from here (the check above counts it if it got nowhere).
            r.corridor.clear();
            r.corner = 0;
            continue;
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
                r.stalls  = 0;
                r.legOpen = false;
                r.why.clear();
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

void AutopilotRoute_Filter(Player* bot, PathGenerator& generator)
{
    // This core already gives bot (headless) sessions the filter a traveller
    // wants: steep slopes excluded outright, lava and slime excluded, water at
    // twenty times the cost of ground (PathGenerator::CreateFilter). Never
    // loosen it -- replacing its exclude flags once let bots climb
    // mountainsides and swim far more readily. Only a stricter water cost is
    // applied on top.
    if (bot->GetSession() && bot->GetSession()->IsHeadless())
    {
        if (g_rc.waterCost > 20.0f)
            generator.SetNavTerrainCost(NAV_WATER, g_rc.waterCost);
        return;
    }

    generator.SetExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP));
    generator.SetNavTerrainCost(NAV_WATER, std::max(20.0f, g_rc.waterCost));
}

bool AutopilotRoute_ClimbsOnlyWhereWet(Player* bot, const Movement::PointsArray& points)
{
    Map* map = bot->GetMap();
    if (!map)
        return false;
    const float height = bot->GetCollisionHeight();
    const uint32 phase = bot->GetPhaseMask();
    auto wet = [&](const G3D::Vector3& p)
    {
        // In the water, or the bank just above it.
        return map->IsInWater(phase, p.x, p.y, p.z, height) || map->IsInWater(phase, p.x, p.y, p.z - 1.5f, height);
    };
    for (size_t i = 1; i < points.size(); ++i)
    {
        const G3D::Vector3& a = points[i - 1];
        const G3D::Vector3& b = points[i];
        if (PathGenerator::IsWalkableClimb(a.x, a.y, a.z, b.x, b.y, b.z, height))
            continue;
        if (!wet(a) && !wet(b))
            return false;   // a steep step on dry ground: a mountainside
    }
    return true;
}

bool AutopilotRoute_WalkableNear(Player* bot, float x, float y, AutopilotRoutePoint& out)
{
    Map* map = bot->GetMap();
    if (!map)
        return false;
    AutopilotRoutePoint p = Reseat(map, { x, y, bot->GetPositionZ() }, bot->GetPositionZ());
    if (!SnapToMesh(map, p, 25.0f, 40.0f))
        return false;
    out = p;
    return true;
}
