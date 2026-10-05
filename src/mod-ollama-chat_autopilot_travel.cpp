#include "mod-ollama-chat_autopilot_travel.h"
#include "mod-ollama-chat_autopilot_strategy.h"
#include "mod-ollama-chat-utilities.h"

#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
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
        uint32_t maxPlans       = 8;
    };
    TravelConfig g_tc;   // world thread only

    constexpr float kDirectWalk  = 60.0f;   // under NewRpg's own 70 yd straight walk
    constexpr float kDocked      = 40.0f;   // transport this close to its stop, and still
    constexpr float kTouch       = 4.0f;    // close enough to talk to an NPC

    // --- boats and zeppelins ----------------------------------------------

    struct Dock
    {
        uint32_t map = 0;
        float    x = 0.0f, y = 0.0f, z = 0.0f;   // where the transport stops
        float    lx = 0.0f, ly = 0.0f, lz = 0.0f; // somewhere to stand ashore
        bool     hasLand = false;
        bool     alliance = false, horde = false; // friendly NPCs nearby
    };

    struct Crossing
    {
        uint32_t    entry = 0;
        std::string name;
        size_t      from = 0, to = 0;   // indices into g_docks
    };

    std::vector<Dock>     g_docks;
    std::vector<Crossing> g_crossings;

    float Dist2D(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

    bool DockOk(const Dock& d, TeamId team)
    {
        return d.hasLand && (team == TEAM_ALLIANCE ? d.alliance : d.horde);
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

    // Maps away from `target`, by crossings usable by `team`. -1 = unreachable.
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
                const Dock& from = g_docks[c.from];
                const Dock& to   = g_docks[c.to];
                if (to.map != map || !DockOk(from, team) || !DockOk(to, team) || dist.count(from.map))
                    continue;
                dist[from.map] = dist[map] + 1;
                queue.push_back(from.map);
            }
        }
        return dist;
    }

    // The first crossing toward `target`: on a shortest chain, from the dock
    // nearest the bot.
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
            const Dock& from = g_docks[c.from];
            const Dock& to   = g_docks[c.to];
            if (from.map != bot->GetMapId() || !DockOk(from, team) || !DockOk(to, team))
                continue;
            auto next = dist.find(to.map);
            if (next == dist.end() || next->second != here->second - 1)
                continue;
            const float d = bot->GetDistance(from.lx, from.ly, from.lz);
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

    // A point on the deck: the transport's model is in the map's dynamic
    // collision, so a height probe from above finds the deck.
    AutopilotTravelPoint Deck(Player* bot, MotionTransport* t)
    {
        AutopilotTravelPoint p{ t->GetMapId(), t->GetPositionX(), t->GetPositionY(), t->GetPositionZ() };
        if (Map* map = t->GetMap())
        {
            const float h = map->GetHeight(bot->GetPhaseMask(), p.x, p.y, p.z + 30.0f, true, 60.0f);
            if (h > INVALID_HEIGHT && map->GetTransportForPos(bot->GetPhaseMask(), p.x, p.y, h + 0.5f, bot) == t)
                p.z = h + 0.5f;
        }
        return p;
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
        trip.tSampleAt    = 0;
    }

    bool PlanFlight(Player* bot, const AutopilotTrip& trip, AutopilotLeg& walk, AutopilotLeg& fly)
    {
        TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetNearestFlightMasterInfo(bot);
        if (!fm || !fm->taxiNodeId || fm->pos.GetMapId() != bot->GetMapId())
            return false;

        const float total  = DistTo(bot, trip.dest);
        const float toFm   = bot->GetExactDist2d(fm->pos.GetPositionX(), fm->pos.GetPositionY());
        const uint8 mount  = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 0;

        uint32_t bestNode = 0;
        float    bestDist = 0.0f;
        for (uint32_t i = 0; i < sTaxiNodesStore.GetNumRows(); ++i)
        {
            TaxiNodesEntry const* n = sTaxiNodesStore.LookupEntry(i);
            if (!n || n->map_id != trip.dest.map || !n->MountCreatureID[mount])
                continue;
            const float d = Dist2D(n->x, n->y, trip.dest.x, trip.dest.y);
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
        if (++trip.plans > g_tc.maxPlans)
            return "too many changes of plan on the way";

        if (bot->GetMapId() != trip.dest.map)
        {
            const Crossing* c = FirstHop(bot, trip.dest.map);
            if (!c)
                return SafeFormat("no boat or zeppelin {} can take goes from {} toward {}",
                                  bot->GetTeamId() == TEAM_ALLIANCE ? "the Alliance" : "the Horde",
                                  MapName(bot->GetMapId()), MapName(trip.dest.map));

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

        AutopilotLeg walk, fly;
        if (g_tc.flights && !trip.noFlight && !trip.flown && DistTo(bot, trip.dest) > g_tc.flightMinYards &&
            PlanFlight(bot, trip, walk, fly))
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
                if (AutopilotRoute_Next(bot, route, node))
                {
                    if (route.next != trip.issued || !AutopilotMove_IsMoving(ai))
                    {
                        AutopilotMove_To(ai, node.x, node.y, node.z, true);
                        trip.issued = route.next;
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

    LegResult Fly(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, const AutopilotLeg& leg, std::string& note)
    {
        if (trip.tookOff)
        {
            if (bot->IsInFlight() || bot->IsBeingTeleported())
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
        trip.tookOff = true;
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
        if (now - trip.legStartedAt > g_tc.boatWaitMin * 60)
        {
            note = leg.label + " never came";
            return LegResult::Fail;
        }

        MotionTransport* t = FindTransport(bot->GetMap(), leg.entry);
        if (!DockedAt(trip, t, leg.to, now))
        {
            // Wait ashore; keep playerbots from wandering off meanwhile.
            if (bot->GetExactDist2d(leg.land.x, leg.land.y) > 15.0f && !AutopilotMove_IsMoving(ai))
                AutopilotMove_To(ai, leg.land.x, leg.land.y, leg.land.z, true);
            else
                AutopilotMove_Hold(ai, 5000, false);
            return LegResult::Going;
        }

        // Docked: walk straight onto the deck. Playerbots makes the bot a
        // passenger once it stands on it; do it here too in case its probe
        // misses an odd deck.
        const AutopilotTravelPoint deck = Deck(bot, t);
        if (bot->GetExactDist2d(deck.x, deck.y) > 2.0f)
        {
            if (!AutopilotMove_IsMoving(ai))
                AutopilotMove_To(ai, deck.x, deck.y, deck.z, false);
            return LegResult::Going;
        }
        if (bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(),
                                               bot->GetPositionZ(), bot) == t)
        {
            t->AddPassenger(bot, true);
            bot->StopMovingOnCurrentPos();
        }
        return LegResult::Going;
    }

    LegResult Ride(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, const AutopilotLeg& leg, uint32_t now,
                   std::string& note)
    {
        if (bot->IsBeingTeleported())
            return LegResult::Going;

        Transport* on = bot->GetTransport();
        if (trip.stepping)
        {
            if (!on)
            {
                note       = "arrived in " + MapName(bot->GetMapId()) + " by " + leg.label;
                trip.flown = false;
                return LegResult::Replan;
            }
            if (!AutopilotMove_IsMoving(ai))
                AutopilotMove_To(ai, leg.land.x, leg.land.y, leg.land.z, false);
            return LegResult::Going;
        }

        if (!on || on->GetEntry() != leg.entry)
        {
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
}

void AutopilotTravel_LoadConfig()
{
    TravelConfig c;
    c.flights        = sConfigMgr->GetOption<bool>("OllamaChat.Autopilot.Travel.Flights", true);
    c.flightMinYards = std::max(100.0f, sConfigMgr->GetOption<float>("OllamaChat.Autopilot.Travel.FlightMinYards", 700.0f));
    c.boatWaitMin    = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Travel.BoatWaitMinutes", 20));
    c.stuckSeconds   = std::max<uint32_t>(10, sConfigMgr->GetOption<uint32_t>("OllamaChat.Autopilot.Travel.StuckSeconds", 45));
    g_tc = c;
}

void AutopilotTravel_Build()
{
    g_docks.clear();
    g_crossings.clear();

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
                if (kf.IsStopFrame())
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
                g_crossings.push_back(Crossing{ entry, tmpl.name, from, to });
            }
        }
    }

    // Where to stand ashore, and whose dock it is: the nearest creature spawn
    // to each stop, and whether any creature near it is friendly to each
    // faction (dock masters, guards, goblins at neutral ports).
    FactionTemplateEntry const* alliance = sFactionTemplateStore.LookupEntry(1);   // Human
    FactionTemplateEntry const* horde    = sFactionTemplateStore.LookupEntry(2);   // Orc
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
                if (FactionTemplateEntry const* f = sFactionTemplateStore.LookupEntry(t->faction))
                {
                    if (alliance && f->IsFriendlyTo(*alliance))
                        d.alliance = true;
                    if (horde && f->IsFriendlyTo(*horde))
                        d.horde = true;
                }

            if (std::fabs(data.posZ - d.z) < 40.0f && dist < nearest[i])
            {
                nearest[i] = dist;
                d.lx = data.posX;
                d.ly = data.posY;
                d.lz = data.posZ;
                d.hasLand = true;
            }
        }
    }

    LOG_INFO("server.loading", "[Ollama Chat] Autopilot: indexed {} boat and zeppelin crossings between {} docks.",
             g_crossings.size(), g_docks.size());
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
            case AutopilotLegType::Fly:      r = Fly(bot, ai, trip, leg, legNote);        break;
            case AutopilotLegType::Board:    r = Board(bot, ai, trip, leg, now, legNote); break;
            case AutopilotLegType::Ride:     r = Ride(bot, ai, trip, leg, now, legNote);  break;
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
    }
    return "";
}

bool AutopilotTravel_CanReach(Player* bot, uint32_t map)
{
    if (map == bot->GetMapId())
        return true;
    return HopsTo(map, bot->GetTeamId()).count(bot->GetMapId()) != 0;
}
