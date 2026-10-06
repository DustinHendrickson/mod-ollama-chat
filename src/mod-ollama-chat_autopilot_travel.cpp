#include "mod-ollama-chat_autopilot_travel.h"
#include "mod-ollama-chat_autopilot_strategy.h"
#include "mod-ollama-chat-utilities.h"

#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "ModelIgnoreFlags.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Transport.h"
#include "TransportMgr.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "TravelMgr.h"
#include "TravelNode.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <deque>
#include <unordered_map>

namespace
{
    struct TravelConfig
    {
        bool     flights        = true;
        float    flightMinYards = 700.0f;
        uint32_t boatWaitMin    = 20;
        uint32_t rideMaxMin     = 40;
        uint32_t stuckSeconds   = 45;
    };
    constexpr uint32_t kMaxPlans = 8;   // replans per trip: each vehicle change is one
    TravelConfig g_tc;   // world thread only

    constexpr float kDirectWalk  = 60.0f;   // under NewRpg's own 70 yd straight walk
    constexpr float kDocked      = 40.0f;   // transport this close to its stop, and still
    constexpr float kTouch       = 4.0f;    // close enough to talk to an NPC

    // --- crossings between continents ------------------------------------
    //
    // Three kinds, all the game's own mechanics:
    //   Transport  a boat or zeppelin between two docks
    //   Trigger    an area trigger that teleports whoever walks into it (the
    //              Dark Portal both ways); the core's own handling is copied
    //   Portal     a gameobject that casts a teleport on whoever uses it
    //              (city portals in Shattrath and Dalaran, the Silvermoon orb)

    struct Dock
    {
        uint32_t map = 0;
        float    x = 0.0f, y = 0.0f, z = 0.0f;   // where the transport stops
        float    lx = 0.0f, ly = 0.0f, lz = 0.0f; // somewhere to stand ashore
        bool     hasLand = false;
        uint32_t allianceOnly = 0;   // nearby NPCs hostile to the Horde only
        uint32_t hordeOnly    = 0;   // nearby NPCs hostile to the Alliance only
    };

    enum class CrossKind : uint8_t { Transport, Trigger, Portal };

    struct Crossing
    {
        CrossKind            kind  = CrossKind::Transport;
        uint32_t             entry = 0;     // transport / trigger / gameobject
        std::string          name;
        size_t               from = 0, to = 0;   // Transport: indices into g_docks
        AutopilotTravelPoint enter;         // Trigger / Portal: where to go in
        AutopilotTravelPoint exit;          // Trigger / Portal: where it leads
        bool                 alliance = true, horde = true;

        uint32_t FromMap() const;
        uint32_t ToMap() const;
    };

    std::vector<Dock>     g_docks;
    std::vector<Crossing> g_crossings;

    uint32_t Crossing::FromMap() const { return kind == CrossKind::Transport ? g_docks[from].map : enter.map; }
    uint32_t Crossing::ToMap() const   { return kind == CrossKind::Transport ? g_docks[to].map : exit.map; }

    bool IsContinent(uint32_t map) { return map == 0 || map == 1 || map == 530 || map == 571; }

    float Dist2D(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

    // A faction's own dock (its guards, its dock master) is off limits to the
    // other faction; neutral ports and shared ones are open to both.
    bool DockOk(const Dock& d, TeamId team)
    {
        if (!d.hasLand)
            return false;
        return team == TEAM_ALLIANCE ? !(d.hordeOnly && !d.allianceOnly) : !(d.allianceOnly && !d.hordeOnly);
    }

    bool Usable(const Crossing& c, TeamId team)
    {
        if (c.kind == CrossKind::Transport)
            return DockOk(g_docks[c.from], team) && DockOk(g_docks[c.to], team);
        return team == TEAM_ALLIANCE ? c.alliance : c.horde;
    }

    size_t AddDock(uint32_t map, float x, float y, float z)
    {
        for (size_t i = 0; i < g_docks.size(); ++i)
            if (g_docks[i].map == map && Dist2D(g_docks[i].x, g_docks[i].y, x, y) < 30.0f)
                return i;
        Dock d;
        d.map = map;
        d.x = x;
        d.y = y;
        d.z = z;
        g_docks.push_back(d);
        return g_docks.size() - 1;
    }

    // Where the bot walks to for a crossing.
    AutopilotTravelPoint Start(const Crossing& c)
    {
        if (c.kind == CrossKind::Transport)
        {
            const Dock& d = g_docks[c.from];
            return { d.map, d.lx, d.ly, d.lz };
        }
        return c.enter;
    }

    // Maps away from `target`, by crossings usable by `team`.
    std::unordered_map<uint32_t, int> HopsTo(uint32_t target, TeamId team)
    {
        std::unordered_map<uint32_t, int> dist;
        dist[target] = 0;
        std::deque<uint32_t> queue{ target };
        while (!queue.empty())
        {
            const uint32_t map = queue.front();
            queue.pop_front();
            for (const Crossing& c : g_crossings)
            {
                if (c.ToMap() != map || dist.count(c.FromMap()) || !Usable(c, team))
                    continue;
                dist[c.FromMap()] = dist[map] + 1;
                queue.push_back(c.FromMap());
            }
        }
        return dist;
    }

    // The first crossing toward `target`: on a shortest chain, the one whose
    // start is nearest the bot.
    const Crossing* FirstHop(Player* bot, uint32_t target)
    {
        const TeamId team = bot->GetTeamId();
        const auto dist = HopsTo(target, team);
        auto here = dist.find(bot->GetMapId());
        if (here == dist.end())
            return nullptr;

        const Crossing* best = nullptr;
        float bestDist = 0.0f;
        for (const Crossing& c : g_crossings)
        {
            if (c.FromMap() != bot->GetMapId() || !Usable(c, team))
                continue;
            auto next = dist.find(c.ToMap());
            if (next == dist.end() || next->second != here->second - 1)
                continue;
            const AutopilotTravelPoint s = Start(c);
            const float d = bot->GetDistance(s.x, s.y, s.z);
            if (!best || d < bestDist)
            {
                best     = &c;
                bestDist = d;
            }
        }
        return best;
    }

    MotionTransport* FindTransport(Map* map, uint32_t entry)
    {
        if (!map)
            return nullptr;
        for (Transport* t : map->GetAllTransports())
            if (t && t->GetEntry() == entry)
                if (MotionTransport* m = t->ToMotionTransport())
                    return m;
        return nullptr;
    }

    // The transport of this entry the bot is standing on, attached or not.
    MotionTransport* StandingOn(Player* bot, uint32_t entry)
    {
        Map* map = bot->GetMap();
        if (!map)
            return nullptr;
        // No world object: that only adds a search for static transports
        // (elevators), a grid search per call that boats do not need.
        Transport* t = map->GetTransportForPos(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(),
                                               bot->GetPositionZ(), nullptr);
        return t && t->GetEntry() == entry ? t->ToMotionTransport() : nullptr;
    }

    // Docked: near its stop and not moving since the last look a second or
    // more ago (the core keeps "is moving" private).
    bool DockedAt(AutopilotTrip& trip, MotionTransport* t, const AutopilotTravelPoint& stop, uint32_t now)
    {
        if (!t || t->GetMapId() != stop.map ||
            Dist2D(t->GetPositionX(), t->GetPositionY(), stop.x, stop.y) > kDocked)
        {
            trip.tSampleAt = 0;
            return false;
        }

        const float x = t->GetPositionX(), y = t->GetPositionY();
        const bool still = trip.tSampleAt && now > trip.tSampleAt && Dist2D(x, y, trip.tx, trip.ty) < 0.5f;
        if (!trip.tSampleAt || now > trip.tSampleAt)
        {
            trip.tx        = x;
            trip.ty        = y;
            trip.tSampleAt = now;
        }
        return still;
    }

    // A point on the deck to walk onto.
    //
    // Cast rays straight down at the ship's own model -- the test the core
    // itself uses for "standing on a transport" (Map::GetTransportForPos). A
    // terrain height query does not reliably see a moving ship's model and
    // finds the sea or the pier under it instead, which is how a bot once
    // ended up riding beneath the hull.
    //
    // A ray from above meets the masts, yards and sails first, so every
    // surface under each probe is collected, and the deck is the one nearest
    // the height of the pier the bot is boarding from (`boardZ`). Probes run
    // in rings around the ship's centre, because decks differ (a zeppelin's
    // gondola is not under its centre); among equally good surfaces the one
    // nearest the bot wins.
    bool Deck(Player* bot, MotionTransport* t, float boardZ, AutopilotTravelPoint& out)
    {
        if (!t->m_model)
            return false;

        const float top    = t->GetPositionZ() + 40.0f;
        const float bottom = t->GetPositionZ() - 20.0f;

        bool  found     = false;
        float bestScore = 0.0f;
        for (float r : { 0.0f, 3.0f, 6.0f, 9.0f, 12.0f, 16.0f })
        {
            for (int k = 0; k < (r == 0.0f ? 1 : 8); ++k)
            {
                const float a = float(k) * float(M_PI) / 4.0f;
                const float x = t->GetPositionX() + r * std::cos(a);
                const float y = t->GetPositionY() + r * std::sin(a);

                float from  = top;
                float above = FLT_MAX;   // the surface over this one, if any
                for (int layer = 0; layer < 8 && from > bottom; ++layer)
                {
                    float dist = from - bottom;
                    const G3D::Ray ray(G3D::Vector3(x, y, from), G3D::Vector3(0.0f, 0.0f, -1.0f));
                    if (!t->m_model->intersectRay(ray, dist, false, bot->GetPhaseMask(), VMAP::ModelIgnoreFlags::Nothing))
                        break;
                    const float z = from - dist;
                    const float headroom = above - z;
                    above = z;
                    from  = z - 0.5f;   // look for the next surface below this one

                    // A deck has room to stand on it (floors under the deck and
                    // the hull bottom do not), sits near the pier the bot is on,
                    // and the core must count it as on this ship.
                    if (headroom < 2.2f || std::fabs(z - boardZ) > 8.0f)
                        continue;
                    if (bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), x, y, z + 0.5f, nullptr) != t)
                        continue;

                    // Height mismatch with the pier dominates; distance from
                    // the bot breaks ties between decks at the right height.
                    const float score = std::fabs(z - boardZ) * 10.0f + bot->GetExactDist2d(x, y);
                    if (!found || score < bestScore)
                    {
                        out       = { t->GetMapId(), x, y, z + 0.5f };
                        bestScore = score;
                        found     = true;
                    }
                }
            }
        }
        return found;
    }

    std::string MapName(uint32_t map)
    {
        if (MapEntry const* m = sMapStore.LookupEntry(map))
            return m->name[0];
        return std::to_string(map);
    }

    std::string TaxiNodeName(uint32_t node)
    {
        if (TaxiNodesEntry const* n = sTaxiNodesStore.LookupEntry(node))
            return n->name[0];
        return "?";
    }

    // --- planning -------------------------------------------------------------

    float DistTo(Player* bot, const AutopilotTravelPoint& p)
    {
        return bot->GetMapId() == p.map ? bot->GetExactDist2d(p.x, p.y) : FLT_MAX;
    }

    void ResetLeg(AutopilotTrip& trip, uint32_t now)
    {
        trip.legStartedAt = now;
        trip.routed       = false;
        trip.route        = AutopilotRoute();
        trip.issued       = SIZE_MAX;
        trip.best         = FLT_MAX;
        trip.bestAt       = now;
        trip.tookOff      = false;
        trip.boarded      = false;
        trip.stepping     = false;
        trip.deckProbed   = false;
        trip.deckFound    = false;
        trip.tSampleAt    = 0;
    }

    // A flight point the bot may fly to: discovered, as for a player, or any
    // when playerbots gives the bot its taxi cheat (AiPlayerbot.BotCheats) --
    // the same rule playerbots' own flight code follows.
    bool KnowsNode(Player* bot, uint32_t node)
    {
        return bot->isTaxiCheater() || bot->m_taxi.IsTaximaskNodeKnown(node);
    }

    // Walk to the nearest flight master and fly to the flight point nearest
    // `dest`, when that clearly beats walking.
    bool PlanFlight(Player* bot, const AutopilotTravelPoint& dest, AutopilotLeg& walk, AutopilotLeg& fly)
    {
        TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetNearestFlightMasterInfo(bot);
        if (!fm || !fm->taxiNodeId || fm->pos.GetMapId() != bot->GetMapId())
            return false;

        const float total  = DistTo(bot, dest);
        const float toFm   = bot->GetExactDist2d(fm->pos.GetPositionX(), fm->pos.GetPositionY());
        const uint8 mount  = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 0;

        uint32_t bestNode = 0;
        float    bestDist = 0.0f;
        for (uint32_t i = 0; i < sTaxiNodesStore.GetNumRows(); ++i)
        {
            TaxiNodesEntry const* n = sTaxiNodesStore.LookupEntry(i);
            // Only flight points the bot has discovered, as for a player.
            if (!n || n->map_id != dest.map || !n->MountCreatureID[mount] || !KnowsNode(bot, n->ID))
                continue;
            const float d = Dist2D(n->x, n->y, dest.x, dest.y);
            if (!bestNode || d < bestDist)
            {
                bestNode = n->ID;
                bestDist = d;
            }
        }

        // Only worth it when walking to the flight master and from the landing
        // is clearly shorter than walking the whole way.
        if (!bestNode || bestNode == fm->taxiNodeId || toFm + bestDist > total * 0.6f)
            return false;

        std::vector<uint32_t> path = sTravelNodeMap.FindTaxiPath(fm->taxiNodeId, bestNode);
        if (path.size() < 2)
            return false;
        // Every stop on the way must be known too; the first one is learned on
        // talking to the flight master.
        for (size_t i = 1; i < path.size(); ++i)
            if (!KnowsNode(bot, path[i]))
                return false;

        walk.type   = AutopilotLegType::Walk;
        walk.to     = { fm->pos.GetMapId(), fm->pos.GetPositionX(), fm->pos.GetPositionY(), fm->pos.GetPositionZ() };
        walk.radius = kTouch;
        walk.label  = "the flight master";

        fly.type     = AutopilotLegType::Fly;
        fly.entry    = fm->templateEntry;
        fly.to       = walk.to;
        fly.taxiPath = std::move(path);
        fly.label    = TaxiNodeName(bestNode);
        return true;
    }

    std::string Plan(Player* bot, AutopilotTrip& trip, uint32_t now)
    {
        trip.legs.clear();
        trip.leg = 0;
        ResetLeg(trip, now);
        if (++trip.plans > kMaxPlans)
            return "too many changes of plan on the way";

        if (bot->GetMapId() != trip.dest.map)
        {
            const Crossing* c = FirstHop(bot, trip.dest.map);
            if (!c)
                return SafeFormat("no boat, zeppelin or portal {} can take leads from {} toward {}",
                                  bot->GetTeamId() == TEAM_ALLIANCE ? "the Alliance" : "the Horde",
                                  MapName(bot->GetMapId()), MapName(trip.dest.map));

            // Far from the dock or portal: fly there first if it saves real
            // distance (Westfall to the Stormwind docks), then plan again.
            {
                AutopilotLeg walk, fly;
                const AutopilotTravelPoint start = Start(*c);
                if (g_tc.flights && !trip.noFlight && !trip.flown && DistTo(bot, start) > g_tc.flightMinYards &&
                    PlanFlight(bot, start, walk, fly))
                {
                    trip.legs = { walk, fly };
                    return "";
                }
            }

            if (c->kind != CrossKind::Transport)
            {
                AutopilotLeg walk;
                walk.type   = AutopilotLegType::Walk;
                walk.to     = c->enter;
                walk.radius = c->kind == CrossKind::Trigger ? 3.0f : kTouch;
                walk.label  = c->name;

                AutopilotLeg go;
                go.type   = c->kind == CrossKind::Trigger ? AutopilotLegType::Trigger : AutopilotLegType::Portal;
                go.entry  = c->entry;
                go.to     = c->enter;
                go.arrive = c->exit;
                go.label  = c->name;

                trip.legs = { walk, go };
                return "";
            }

            const Dock& from = g_docks[c->from];
            const Dock& to   = g_docks[c->to];

            AutopilotLeg walk;
            walk.type   = AutopilotLegType::Walk;
            walk.to     = { from.map, from.lx, from.ly, from.lz };
            walk.radius = 8.0f;
            walk.label  = "the dock for " + c->name;

            AutopilotLeg board;
            board.type  = AutopilotLegType::Board;
            board.entry = c->entry;
            board.to    = { from.map, from.x, from.y, from.z };
            board.land  = walk.to;
            board.label = c->name;

            AutopilotLeg ride;
            ride.type   = AutopilotLegType::Ride;
            ride.entry  = c->entry;
            ride.arrive = { to.map, to.x, to.y, to.z };
            ride.land   = { to.map, to.lx, to.ly, to.lz };
            ride.label  = c->name;

            trip.legs = { walk, board, ride };
            return "";
        }

        // A destination taken from a quest's map marker has no height. On its
        // own continent now, look one up (it works once the grid is loaded;
        // until then the route re-seats each step on the ground anyway).
        if (trip.dest.z == 0.0f)
        {
            const float h = bot->GetMap()->GetHeight(trip.dest.x, trip.dest.y, bot->GetPositionZ() + 200.0f, true,
                                                     1000.0f);
            if (h > INVALID_HEIGHT)
                trip.dest.z = h;
        }

        AutopilotLeg walk, fly;
        if (g_tc.flights && !trip.noFlight && !trip.flown && DistTo(bot, trip.dest) > g_tc.flightMinYards &&
            PlanFlight(bot, trip.dest, walk, fly))
        {
            trip.legs = { walk, fly };
            return "";
        }

        AutopilotLeg last;
        last.type   = AutopilotLegType::Walk;
        last.to     = trip.dest;
        last.radius = trip.npcEntry ? 6.0f : trip.arriveRadius;
        last.label  = "the destination";
        trip.legs.push_back(last);

        if (trip.npcEntry)
        {
            AutopilotLeg approach;
            approach.type  = AutopilotLegType::Approach;
            approach.entry = trip.npcEntry;
            trip.legs.push_back(approach);
        }
        return "";
    }

    // --- running a leg ----------------------------------------------------------

    enum class LegResult { Going, Done, Replan, Fail };

    LegResult Walk(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, const AutopilotLeg& leg, uint32_t now,
                   std::string& note)
    {
        if (bot->GetMapId() != leg.to.map)
            return LegResult::Replan;

        const float d = bot->GetExactDist2d(leg.to.x, leg.to.y);
        if (d <= leg.radius)
            return LegResult::Done;

        // Stuck: no real progress for a while. Reroute twice, then give up.
        if (d + 5.0f < trip.best)
        {
            trip.best   = d;
            trip.bestAt = now;
        }
        else if (now - trip.bestAt > g_tc.stuckSeconds)
        {
            if (trip.routed && trip.route.rebuilds < 2)
            {
                AutopilotMove_Stop(ai);
                AutopilotRoute_Rebuild(bot, trip.route);
                trip.issued = SIZE_MAX;
                trip.bestAt = now;
                note = "stuck on the way to " + leg.label + ", finding another way";
            }
            else
            {
                note = "stuck on the way to " + leg.label;
                return LegResult::Fail;
            }
        }

        if (!trip.routed && trip.issued == SIZE_MAX && d > kDirectWalk)
        {
            trip.routed = true;
            AutopilotRoute_Begin(bot, leg.to.map, leg.to.x, leg.to.y, leg.to.z, trip.route);
        }

        if (trip.routed)
        {
            AutopilotRoute& route = trip.route;
            AutopilotRoute_Extend(bot, route);
            if (route.failed)
            {
                if (route.why != "no mmaps")
                {
                    note = SafeFormat("could not find a way to {} ({})", leg.label, route.why);
                    return LegResult::Fail;
                }
                trip.routed = false;   // no navmesh: walk it directly
                trip.issued = 0;
            }
            else
            {
                AutopilotRoutePoint node;
                size_t index = 0;
                if (AutopilotRoute_Next(bot, route, node, index))
                {
                    if (index != trip.issued || !AutopilotMove_IsMoving(ai))
                    {
                        AutopilotMove_To(ai, node.x, node.y, node.z, true);
                        trip.issued = index;
                    }
                    return LegResult::Going;
                }
                if (!route.complete)
                    return LegResult::Going;   // building ahead
                trip.routed = false;           // walked it; finish directly
                trip.issued = 0;
            }
        }

        if (!AutopilotMove_IsMoving(ai))
        {
            AutopilotMove_To(ai, leg.to.x, leg.to.y, leg.to.z, true);
            trip.issued = 0;
        }
        return LegResult::Going;
    }

    LegResult Approach(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, const AutopilotLeg& leg, uint32_t now)
    {
        Creature* npc = bot->FindNearestCreature(leg.entry, 60.0f);
        if (!npc || bot->GetDistance(npc) <= kTouch)
            return LegResult::Done;
        if (now - trip.legStartedAt > g_tc.stuckSeconds)
            return LegResult::Done;   // close enough; the errand decides what that means
        if (!AutopilotMove_IsMoving(ai))
            AutopilotMove_To(ai, npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ(), true);
        return LegResult::Going;
    }

    LegResult Fly(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, const AutopilotLeg& leg, uint32_t now,
                  std::string& note)
    {
        if (trip.tookOff)
        {
            // The flight state is set on the bot's next update, not by
            // ActivateTaxiPathTo itself: give it a moment before calling it landed.
            if (bot->IsInFlight() || bot->IsBeingTeleported() || now - trip.legStartedAt < 5)
                return LegResult::Going;
            note      = "landed at " + leg.label;
            trip.flown = true;
            return LegResult::Replan;
        }

        if (bot->IsInFlight())
        {
            trip.tookOff = true;
            return LegResult::Going;
        }

        Creature* npc = bot->FindNearestCreature(leg.entry, 40.0f);
        if (!npc || !npc->IsAlive())
        {
            note          = "the flight master was not there; walking";
            trip.noFlight = true;
            return LegResult::Replan;
        }
        if (bot->GetDistance(npc) > kTouch)
        {
            if (!AutopilotMove_IsMoving(ai))
                AutopilotMove_To(ai, npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ(), true);
            return LegResult::Going;
        }

        ai->RemoveShapeshift();
        if (bot->IsMounted())
            bot->Dismount();
        bot->GetSession()->SendLearnNewTaxiNode(npc);

        if (!bot->ActivateTaxiPathTo(leg.taxiPath, npc, 0))
        {
            note          = "could not take the flight to " + leg.label + " (not enough money?); walking";
            trip.noFlight = true;
            return LegResult::Replan;
        }
        trip.tookOff      = true;
        trip.legStartedAt = now;
        note = "took a flight to " + leg.label;
        return LegResult::Going;
    }

    LegResult Board(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, const AutopilotLeg& leg, uint32_t now,
                    std::string& note)
    {
        if (Transport* on = bot->GetTransport(); on && on->GetEntry() == leg.entry)
        {
            note = "boarded " + leg.label;
            return LegResult::Done;
        }

        // On the deck but not attached yet (playerbots checks once a second):
        // attach now, before the ship sails off without the bot.
        if (MotionTransport* under = StandingOn(bot, leg.entry))
        {
            under->AddPassenger(bot, true);
            bot->StopMovingOnCurrentPos();
            note = "boarded " + leg.label;
            return LegResult::Done;
        }

        if (now - trip.legStartedAt > g_tc.boatWaitMin * 60)
        {
            note = leg.label + " never came";
            return LegResult::Fail;
        }

        MotionTransport* t = FindTransport(bot->GetMap(), leg.entry);
        if (!DockedAt(trip, t, leg.to, now))
        {
            trip.boarded    = false;   // walking on starts over at the next docking
            trip.deckProbed = false;
            trip.deckFound  = false;   // the next docking may sit a little differently
            // Wait ashore; keep playerbots from wandering off meanwhile.
            if (bot->GetExactDist2d(leg.land.x, leg.land.y) > 15.0f && !AutopilotMove_IsMoving(ai))
                AutopilotMove_To(ai, leg.land.x, leg.land.y, leg.land.z, true);
            else
                AutopilotMove_Hold(ai, 5000, false);
            return LegResult::Going;
        }

        // Docked: walk straight onto the deck (it is not on the navmesh).
        // Probed until a deck is found, then kept for this docking: the ship
        // does not move while docked.
        if (!trip.deckFound)
        {
            // The bot stands on the pier (it walked there on the navmesh), so
            // its own height is the height of the way on.
            trip.deckFound = Deck(bot, t, bot->GetPositionZ(), trip.deck);
            if (!trip.deckFound && !trip.deckProbed)
                note = "could not find a way onto the deck of " + leg.label + "; waiting for it to dock again";
            trip.deckProbed = true;
        }
        if (!trip.deckFound)
        {
            // Never board blind: attaching a bot that is not standing on the
            // deck carries it along beside or under the hull. Wait ashore;
            // BoatWaitMinutes ends the wait and the model is told.
            AutopilotMove_Hold(ai, 5000, false);
            return LegResult::Going;
        }

        const AutopilotTravelPoint& deck = trip.deck;
        if (!AutopilotMove_IsMoving(ai) || !trip.boarded)
        {
            AutopilotMove_To(ai, deck.x, deck.y, deck.z, false);
            trip.boarded = true;   // here: "on the way up the gangway"
        }
        return LegResult::Going;
    }

    LegResult Ride(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, const AutopilotLeg& leg, uint32_t now,
                   std::string& note)
    {
        if (bot->IsBeingTeleported())
            return LegResult::Going;

        Transport* on = bot->GetTransport();

        // Detached for a moment (crossing to another map, or playerbots' own
        // check ran between two steps): still standing on it means still aboard.
        if ((!on || on->GetEntry() != leg.entry) && !trip.stepping)
            if (MotionTransport* under = StandingOn(bot, leg.entry))
            {
                under->AddPassenger(bot, true);
                on = under;
            }

        if (trip.stepping)
        {
            if (!on)
            {
                note       = "arrived in " + MapName(bot->GetMapId()) + " by " + leg.label;
                trip.flown = false;
                return LegResult::Replan;
            }
            // The ship sailed before the bot got off: ride on and wait for the
            // next time it docks here.
            if (!DockedAt(trip, on->ToMotionTransport(), leg.arrive, now) && trip.tSampleAt == 0)
            {
                trip.stepping = false;
                return LegResult::Going;
            }
            if (!AutopilotMove_IsMoving(ai))
                AutopilotMove_To(ai, leg.land.x, leg.land.y, leg.land.z, false);
            return LegResult::Going;
        }

        if (!on || on->GetEntry() != leg.entry)
        {
            // Off the ship. Near the far dock that is arriving; anywhere else
            // it is falling off, and the trip is planned again from there.
            if (bot->GetMapId() == leg.arrive.map && DistTo(bot, leg.arrive) < 80.0f)
            {
                note       = "arrived in " + MapName(bot->GetMapId()) + " by " + leg.label;
                trip.flown = false;
                return LegResult::Replan;
            }
            note = "got off " + leg.label + " early";
            return LegResult::Replan;
        }
        if (now - trip.legStartedAt > g_tc.rideMaxMin * 60)
        {
            note = "never got where " + leg.label + " was going";
            return LegResult::Fail;
        }

        // Stand still on deck; nothing of playerbots' may walk it off.
        AutopilotMove_Hold(ai, 5000, true);

        MotionTransport* t = on->ToMotionTransport();
        if (bot->GetMapId() == leg.arrive.map && DockedAt(trip, t, leg.arrive, now))
        {
            trip.stepping = true;
            AutopilotMove_To(ai, leg.land.x, leg.land.y, leg.land.z, false);
        }
        return LegResult::Going;
    }

    // The Dark Portal and its like: an area trigger that teleports whoever
    // walks into it. A bot's client never reports entering one, so do what
    // the core does when a client does (WorldSession::HandleAreaTriggerOpcode).
    LegResult EnterTrigger(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, const AutopilotLeg& leg, uint32_t now,
                      std::string& note)
    {
        if (bot->GetMapId() == leg.arrive.map)
        {
            note = "went through " + leg.label;
            return LegResult::Replan;
        }
        if (trip.tookOff)
        {
            if (now - trip.legStartedAt > 30)
            {
                note = "could not go through " + leg.label;
                return LegResult::Fail;
            }
            return LegResult::Going;   // being teleported
        }

        AreaTrigger const* at = sObjectMgr->GetAreaTrigger(leg.entry);
        AreaTriggerTeleport const* tp = sObjectMgr->GetAreaTriggerTeleport(leg.entry);
        if (!at || !tp)
        {
            note = leg.label + " no longer exists";
            return LegResult::Fail;
        }

        if (!bot->IsInAreaTriggerRadius(at, 1.0f))
        {
            if (!AutopilotMove_IsMoving(ai))
                AutopilotMove_To(ai, at->x, at->y, at->z, true);
            if (now - trip.legStartedAt > g_tc.stuckSeconds * 2)
            {
                note = "could not step into " + leg.label;
                return LegResult::Fail;
            }
            return LegResult::Going;
        }

        if (sMapMgr->PlayerCannotEnter(tp->target_mapId, bot, false) != Map::CAN_ENTER)
        {
            note = "is not allowed through " + leg.label;
            return LegResult::Fail;
        }

        if (!bot->TeleportTo(tp->target_mapId, tp->target_X, tp->target_Y, tp->target_Z, tp->target_Orientation,
                             TELE_TO_NOT_LEAVE_TRANSPORT))
        {
            note = "is not allowed through " + leg.label;
            return LegResult::Fail;
        }
        trip.tookOff      = true;
        trip.legStartedAt = now;
        return LegResult::Going;
    }

    // A city portal: use the gameobject; it casts the teleport on the bot.
    LegResult UsePortal(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, const AutopilotLeg& leg, uint32_t now,
                     std::string& note)
    {
        if (bot->GetMapId() == leg.arrive.map && DistTo(bot, leg.arrive) < 200.0f)
        {
            note = "took " + leg.label;
            return LegResult::Replan;
        }
        if (trip.tookOff && now - trip.legStartedAt < 15)
            return LegResult::Going;   // casting / being teleported

        GameObject* go = bot->FindNearestGameObject(leg.entry, 30.0f);
        if (!go)
        {
            note = leg.label + " was not there";
            return LegResult::Fail;
        }
        if (bot->GetDistance(go) > kTouch)
        {
            if (!AutopilotMove_IsMoving(ai))
                AutopilotMove_To(ai, go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(), true);
            return LegResult::Going;
        }
        if (trip.tookOff)
        {
            note = "used " + leg.label + " but nothing happened";
            return LegResult::Fail;
        }

        if (bot->IsMounted())
            bot->Dismount();
        AutopilotMove_Stop(ai);
        go->Use(bot);
        trip.tookOff      = true;
        trip.legStartedAt = now;
        return LegResult::Going;
    }
}

void AutopilotTravel_LoadConfig()
{
    TravelConfig c;
    c.flights        = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.Travel.Flights", true);
    c.flightMinYards = std::max(100.0f, sConfigMgr->GetOption<float>("OllamaChat.Autopilot.Travel.FlightMinYards", 700.0f));
    c.boatWaitMin    = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Travel.BoatWaitMinutes", 20));
    c.stuckSeconds   = std::max<uint32_t>(10, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Travel.StuckSeconds", 45));
    c.rideMaxMin     = std::max<uint32_t>(5, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Travel.RideMaxMinutes", 40));
    g_tc = c;
}

void AutopilotTravel_Build()
{
    g_docks.clear();
    g_crossings.clear();

    FactionTemplateEntry const* alliance = sFactionTemplateStore.LookupEntry(1);   // Human
    FactionTemplateEntry const* horde    = sFactionTemplateStore.LookupEntry(2);   // Orc

    // Who a faction template belongs to: hostile to exactly one side means it
    // is the other side's. Monsters hostile to both, and neutrals, are nobody's.
    auto sides = [&](uint32_t factionTemplate, bool& allianceOnly, bool& hordeOnly)
    {
        allianceOnly = hordeOnly = false;
        FactionTemplateEntry const* f = sFactionTemplateStore.LookupEntry(factionTemplate);
        if (!f || !alliance || !horde)
            return;
        const bool hostileA = f->IsHostileTo(*alliance);
        const bool hostileH = f->IsHostileTo(*horde);
        allianceOnly = hostileH && !hostileA;
        hordeOnly    = hostileA && !hostileH;
    };

    // --- boats and zeppelins ---------------------------------------------------
    size_t transports = 0;
    if (GameObjectTemplateContainer const* templates = sObjectMgr->GetGameObjectTemplates())
    {
        for (auto const& [entry, tmpl] : *templates)
        {
            if (tmpl.type != GAMEOBJECT_TYPE_MO_TRANSPORT)
                continue;
            TransportTemplate const* tt = sTransportMgr->GetTransportTemplate(entry);
            if (!tt || tt->inInstance)
                continue;

            std::vector<size_t> stops;
            for (KeyFrame const& kf : tt->keyFrames)
                if (kf.IsStopFrame() && IsContinent(kf.Node->mapid))
                {
                    const size_t dock = AddDock(kf.Node->mapid, kf.Node->x, kf.Node->y, kf.Node->z);
                    if (stops.empty() || stops.back() != dock)
                        stops.push_back(dock);
                }
            if (stops.size() > 1 && stops.front() == stops.back())
                stops.pop_back();

            for (size_t i = 0; i < stops.size() && stops.size() > 1; ++i)
            {
                const size_t from = stops[i], to = stops[(i + 1) % stops.size()];
                if (g_docks[from].map == g_docks[to].map)
                    continue;   // same continent: walking and flights cover it
                Crossing c;
                c.kind  = CrossKind::Transport;
                c.entry = entry;
                c.name  = tmpl.name;
                c.from  = from;
                c.to    = to;
                g_crossings.push_back(std::move(c));
                ++transports;
            }
        }
    }

    // Where to stand ashore, and whose dock it is: the nearest creature spawn
    // to each stop, and which side's NPCs are around it.
    std::vector<float> nearest(g_docks.size(), FLT_MAX);
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        for (size_t i = 0; i < g_docks.size(); ++i)
        {
            Dock& d = g_docks[i];
            if (d.map != data.mapid)
                continue;
            const float dist = Dist2D(d.x, d.y, data.posX, data.posY);
            if (dist > 100.0f)
                continue;

            if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(data.id))
            {
                bool a = false, h = false;
                sides(t->faction, a, h);
                d.allianceOnly += a ? 1 : 0;
                d.hordeOnly    += h ? 1 : 0;
            }

            // Where to stand ashore: a spawn on the pier, not a crab or fish in
            // the water below it (the stop is the ship's origin, about at the
            // waterline). Dock staff (anything with an NPC role) are the
            // surest sign of the pier, so the rest count as 30 yards further.
            CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(data.id);
            const float score = dist + ((ct && (ct->npcflag | data.npcflag)) ? 0.0f : 30.0f);
            if (data.posZ >= d.z - 1.0f && data.posZ - d.z < 40.0f && score < nearest[i])
            {
                nearest[i] = score;
                d.lx = data.posX;
                d.ly = data.posY;
                d.lz = data.posZ;
                d.hasLand = true;
            }
        }
    }

    // --- area-trigger portals (the Dark Portal) --------------------------------
    size_t triggers = 0;
    for (auto const& [triggerId, tp] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* at = sObjectMgr->GetAreaTrigger(triggerId);
        if (!at || !IsContinent(at->map) || !IsContinent(tp.target_mapId) || at->map == tp.target_mapId)
            continue;
        Crossing c;
        c.kind  = CrossKind::Trigger;
        c.entry = triggerId;
        c.name  = "the portal to " + MapName(tp.target_mapId);
        c.enter = { at->map, at->x, at->y, at->z };
        c.exit  = { tp.target_mapId, tp.target_X, tp.target_Y, tp.target_Z };
        g_crossings.push_back(std::move(c));
        ++triggers;
    }

    // --- city portals (gameobjects that cast a teleport) -----------------------
    size_t portals = 0;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        if (!IsContinent(data.mapid))
            continue;
        GameObjectTemplate const* tmpl = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!tmpl)
            continue;
        uint32_t spellId = 0;
        if (tmpl->type == GAMEOBJECT_TYPE_SPELLCASTER)
            spellId = tmpl->spellcaster.spellId;
        else if (tmpl->type == GAMEOBJECT_TYPE_GOOBER)
            spellId = tmpl->goober.spellId;
        SpellInfo const* spell = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!spell)
            continue;

        SpellTargetPosition const* dest = nullptr;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS && !dest; ++i)
            if (spell->Effects[i].Effect == SPELL_EFFECT_TELEPORT_UNITS)
                dest = sSpellMgr->GetSpellTargetPosition(spellId, SpellEffIndex(i));
        if (!dest || !IsContinent(dest->target_mapId) || dest->target_mapId == data.mapid)
            continue;

        Crossing c;
        c.kind  = CrossKind::Portal;
        c.entry = data.id;
        c.name  = tmpl->name.empty() ? "the portal to " + MapName(dest->target_mapId) : tmpl->name;
        c.enter = { data.mapid, data.posX, data.posY, data.posZ };
        c.exit  = { dest->target_mapId, dest->target_X, dest->target_Y, dest->target_Z };
        if (GameObjectTemplateAddon const* addon = sObjectMgr->GetGameObjectTemplateAddon(data.id))
        {
            bool a = false, h = false;
            sides(addon->faction, a, h);
            c.alliance = !h;   // a Horde portal is not for the Alliance
            c.horde    = !a;
        }
        g_crossings.push_back(std::move(c));
        ++portals;
    }

    LOG_INFO("server.loading",
             "[Ollama Chat] Autopilot: indexed {} boat/zeppelin crossings ({} docks), {} portal triggers and "
             "{} city portals between continents.",
             transports, g_docks.size(), triggers, portals);
}

std::string AutopilotTravel_Start(Player* bot, const AutopilotTravelPoint& dest, float arriveRadius,
                                  uint32_t npcEntry, AutopilotTrip& trip, uint32_t now)
{
    trip              = AutopilotTrip();
    trip.dest         = dest;
    trip.arriveRadius = arriveRadius;
    trip.npcEntry     = npcEntry;
    const std::string why = Plan(bot, trip, now);
    trip.active = why.empty();
    return why;
}

AutopilotTripState AutopilotTravel_Update(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, uint32_t now,
                                          std::string& note)
{
    if (!trip.active)
        return AutopilotTripState::Failed;
    if (bot->IsBeingTeleported())
        return AutopilotTripState::Going;

    // Back from a fight or a death: progress so far says nothing about being
    // stuck, and the route was built from somewhere else (a chase, a corpse
    // run) and may now lie behind the bot. Only real interruptions count --
    // on a busy realm the time between visits alone can be long.
    if (trip.interrupted)
    {
        trip.interrupted = false;
        trip.best   = FLT_MAX;
        trip.bestAt = now;
        // Rebuild only if the interruption pulled the bot off the route.
        const AutopilotRoute& r = trip.route;
        const bool offRoute = r.next < r.nodes.size() &&
                              bot->GetExactDist2d(r.nodes[r.next].x, r.nodes[r.next].y) > 60.0f;
        if (trip.routed && offRoute)
        {
            const uint32_t rebuilds = trip.route.rebuilds;
            AutopilotRoute_Rebuild(bot, trip.route);
            trip.route.rebuilds = rebuilds;   // not a "stuck" rebuild
            trip.issued = SIZE_MAX;
        }
    }

    for (int step = 0; step < 4; ++step)   // a leg may finish and the next start in one visit
    {
        if (trip.leg >= trip.legs.size())
        {
            if (DistTo(bot, trip.dest) <= std::max(trip.arriveRadius, 30.0f) || trip.npcEntry)
            {
                trip.active = false;
                return AutopilotTripState::Arrived;
            }
            if (std::string why = Plan(bot, trip, now); !why.empty())
            {
                note        = why;
                trip.active = false;
                return AutopilotTripState::Failed;
            }
        }

        const AutopilotLeg& leg = trip.legs[trip.leg];
        std::string legNote;
        LegResult r = LegResult::Going;
        switch (leg.type)
        {
            case AutopilotLegType::Walk:     r = Walk(bot, ai, trip, leg, now, legNote);  break;
            case AutopilotLegType::Approach: r = Approach(bot, ai, trip, leg, now);       break;
            case AutopilotLegType::Fly:      r = Fly(bot, ai, trip, leg, now, legNote);   break;
            case AutopilotLegType::Board:    r = Board(bot, ai, trip, leg, now, legNote); break;
            case AutopilotLegType::Ride:     r = Ride(bot, ai, trip, leg, now, legNote);  break;
            case AutopilotLegType::Trigger:  r = EnterTrigger(bot, ai, trip, leg, now, legNote); break;
            case AutopilotLegType::Portal:   r = UsePortal(bot, ai, trip, leg, now, legNote);  break;
        }
        if (!legNote.empty())
            note = legNote;

        switch (r)
        {
            case LegResult::Going:
                return AutopilotTripState::Going;
            case LegResult::Fail:
                AutopilotTravel_Stop(ai, trip);
                return AutopilotTripState::Failed;
            case LegResult::Done:
                ++trip.leg;
                ResetLeg(trip, now);
                break;
            case LegResult::Replan:
                if (std::string why = Plan(bot, trip, now); !why.empty())
                {
                    note = why;
                    AutopilotTravel_Stop(ai, trip);
                    return AutopilotTripState::Failed;
                }
                break;
        }
    }
    return AutopilotTripState::Going;
}

void AutopilotTravel_Stop(PlayerbotAI* ai, AutopilotTrip& trip)
{
    if (trip.active && ai)
    {
        Player* bot = ai->GetBot();
        // Never stop a bot mid-flight or on a deck at sea; just let go.
        if (bot && !bot->IsInFlight() && !bot->GetTransport())
            AutopilotMove_Stop(ai);
    }
    trip.active = false;
}

std::string AutopilotTravel_Describe(Player* bot, const AutopilotTrip& trip)
{
    if (!trip.active || trip.leg >= trip.legs.size())
        return "";
    const AutopilotLeg& leg = trip.legs[trip.leg];
    switch (leg.type)
    {
        case AutopilotLegType::Walk:
            return SafeFormat("walking to {} ({} yd)", leg.label,
                              bot->GetMapId() == leg.to.map ? uint32_t(bot->GetExactDist2d(leg.to.x, leg.to.y)) : 0);
        case AutopilotLegType::Approach: return "stepping up to the NPC";
        case AutopilotLegType::Fly:      return trip.tookOff ? "flying to " + leg.label : "taking a flight to " + leg.label;
        case AutopilotLegType::Board:    return "waiting at the dock for " + leg.label;
        case AutopilotLegType::Ride:     return "aboard " + leg.label;
        case AutopilotLegType::Trigger:  return "going through " + leg.label;
        case AutopilotLegType::Portal:   return "taking " + leg.label;
    }
    return "";
}

bool AutopilotTravel_CanReach(Player* bot, uint32_t map)
{
    if (map == bot->GetMapId())
        return true;
    return HopsTo(map, bot->GetTeamId()).count(bot->GetMapId()) != 0;
}

bool AutopilotTravel_IsTimeCritical(const AutopilotTrip& trip)
{
    if (!trip.active || trip.leg >= trip.legs.size())
        return false;
    const AutopilotLegType t = trip.legs[trip.leg].type;
    return t == AutopilotLegType::Board || t == AutopilotLegType::Ride;
}
