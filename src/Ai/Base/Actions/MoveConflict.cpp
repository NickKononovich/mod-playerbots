/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MoveConflict.h"

#include "ActionContext.h"
#include "BotMoveEvents.h"
#include "Creature.h"
#include "ChatActionContext.h"
#include "DungeonHazard.h"
#include "EscortProtect.h"
#include "Group.h"
#include "LeaderContext.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "SelfTravelContext.h"
#include "TestRunnerContext.h"
#include "TestRunnerStrategy.h"
#include "Timer.h"
#include "WorldPacketActionContext.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace
{
    // (a) the detector
    constexpr uint32 WINDOW_MS = 20 * IN_MILLISECONDS;
    constexpr uint32 MAX_FLIPS = 2;                     // "more than twice"
    constexpr float FLIP_ANGLE = 2.0943951f;            // 120 degrees
    constexpr float RAD_TO_DEG = 57.2957795f;
    // An order this close has no direction (ohb-h 163739: "set behind" / "reach melee" 2-4 yd steps in a melee
    // fight were counted as flips).
    constexpr float MIN_ORDER_LENGTH = 5.0f;
    constexpr float SAME_TARGET = 3.0f;                 // a source's order this close to its last one is the same
    constexpr size_t MAX_ORDERS = 32;
    // (b) the waypoint lane
    constexpr bool WAYPOINT_LANE = true;
    constexpr float LANE_WIDTH = 10.0f;
    constexpr float LANE_AT_WAYPOINT = 2.0f;            // at the waypoint it has no direction
    constexpr uint32 VETO_LOG_MS = 10 * IN_MILLISECONDS;
    // (c) the escort anchor
    constexpr uint32 ESCORT_YIELD_LOG_MS = 30 * IN_MILLISECONDS;

    struct Order
    {
        uint32 ms = 0;
        std::string source;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float dirX = 0.0f;
        float dirY = 0.0f;
    };

    struct Flip
    {
        uint32 ms = 0;
        std::string first;   // the two sources, in name order
        std::string second;
    };

    struct BotOrders
    {
        std::deque<Order> orders;
        std::deque<Flip> flips;
        std::unordered_map<std::string, uint32> loggedMs;      // per source pair
        std::unordered_map<std::string, uint32> vetoLoggedMs;  // per vetoed source
        uint32 escortYieldLoggedMs = 0;
    };

    std::mutex ordersLock;
    std::unordered_map<ObjectGuid, BotOrders> botOrders;

    // (d) [b33_harn] the event hold, per group (a bot without one: its own guid)
    constexpr uint32 EVENT_HOLD_TTL_MS = 3 * IN_MILLISECONDS;
    constexpr uint32 EVENT_HOLD_LOG_MS = 15 * IN_MILLISECONDS;

    struct EventHold
    {
        uint32 ms = 0;
        uint32 mapId = 0;
        uint32 instanceId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float radius = 0.0f;
        std::string what;
        std::unordered_map<std::string, uint32> loggedMs;  // per blocked source
    };

    std::mutex eventHoldLock;
    std::unordered_map<ObjectGuid, EventHold> eventHolds;

    ObjectGuid EventHoldKey(Player* bot)
    {
        Group* group = bot->GetGroup();
        return group ? group->GetGUID() : bot->GetGUID();
    }

    std::set<std::string> const& SharedActionNames()
    {
        static std::set<std::string> const names = []
        {
            std::set<std::string> all;
            auto add = [&all](std::set<std::string> const& more) { all.insert(more.begin(), more.end()); };
            add(ActionContext().supports());
            add(ChatActionContext().supports());
            add(WorldPacketActionContext().supports());
            add(LeaderActionContext().supports());
            add(TestRunnerActionContext().supports());
            add(SelfTravelActionContext().supports());
            add(DungeonHazardActionContext().supports());
            return all;
        }();
        return names;
    }

    bool ExemptFromLane(std::string const& source)
    {
        // "event": the steps that start a dungeon event (ohb-h 163739: "dungeon event step" to the barrels was
        // vetoed 4 times by the lane).
        static std::array<char const*, 10> const exempt = {"avoid", "flee",   "escape", "away",  "dodge",
                                                           "hazard", "follow", "escort", "event", "starter"};
        if (SharedActionNames().count(source))
            return true;

        return std::any_of(exempt.begin(), exempt.end(),
                           [&source](char const* word) { return source.find(word) != std::string::npos; });
    }

    float AngleBetween(float ax, float ay, float bx, float by)
    {
        float const dot = std::clamp(ax * bx + ay * by, -1.0f, 1.0f);
        return std::acos(dot);
    }
}

namespace MoveConflict
{
    void NoteOrder(Player* bot, std::string const& source, float x, float y, float z)
    {
        // Dungeons and raids only: the open world's thousands of bots pay nothing.
        if (!bot || !bot->GetMap() || !bot->GetMap()->Instanceable())
            return;

        float const dx = x - bot->GetPositionX();
        float const dy = y - bot->GetPositionY();
        float const length = std::hypot(dx, dy);
        if (length < MIN_ORDER_LENGTH)
            return;

        uint32 const now = getMSTime();
        Order order;
        order.ms = now;
        order.source = source;
        order.x = x;
        order.y = y;
        order.z = z;
        order.dirX = dx / length;
        order.dirY = dy / length;

        BotMoveEvents::Event conflict;
        bool logIt = false;
        std::string otherSource;
        uint32 flipCount = 0;
        {
            std::lock_guard<std::mutex> lock(ordersLock);
            BotOrders& state = botOrders[bot->GetGUID()];
            while (!state.orders.empty() && getMSTimeDiff(state.orders.front().ms, now) > WINDOW_MS)
                state.orders.pop_front();
            while (!state.flips.empty() && getMSTimeDiff(state.flips.front().ms, now) > WINDOW_MS)
                state.flips.pop_front();

            if (!state.orders.empty())
            {
                Order const& last = state.orders.back();
                if (last.source == source && std::hypot(last.x - x, last.y - y) <= SAME_TARGET)
                    return;  // the same order again (re-issued every tick)

                if (last.source != source &&
                    AngleBetween(last.dirX, last.dirY, order.dirX, order.dirY) > FLIP_ANGLE)
                {
                    Flip flip;
                    flip.ms = now;
                    flip.first = std::min(last.source, source);
                    flip.second = std::max(last.source, source);
                    state.flips.push_back(flip);

                    uint32 const count = static_cast<uint32>(
                        std::count_if(state.flips.begin(), state.flips.end(), [&flip](Flip const& other)
                                      { return other.first == flip.first && other.second == flip.second; }));
                    std::string const pair = flip.first + "|" + flip.second;
                    uint32& loggedMs = state.loggedMs[pair];
                    if (count > MAX_FLIPS && (!loggedMs || getMSTimeDiff(loggedMs, now) > WINDOW_MS))
                    {
                        loggedMs = now ? now : 1;
                        logIt = true;
                        otherSource = last.source;
                        flipCount = count;
                        conflict.type = "move_conflict";
                        conflict.Str("name", bot->GetName())
                            .Str("sourceA", last.source)
                            .Real("toAX", last.x).Real("toAY", last.y).Real("toAZ", last.z)
                            .Str("sourceB", source)
                            .Real("toBX", x).Real("toBY", y).Real("toBZ", z)
                            .Num("flips", count)
                            .Num("windowSec", WINDOW_MS / IN_MILLISECONDS)
                            .Real("angleDeg",
                                  AngleBetween(last.dirX, last.dirY, order.dirX, order.dirY) * RAD_TO_DEG)
                            .Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                            .Real("z", bot->GetPositionZ());
                    }
                }
            }

            state.orders.push_back(order);
            if (state.orders.size() > MAX_ORDERS)
                state.orders.pop_front();
        }

        if (!logIt)
            return;

        LOG_INFO("playerbots",
                 "[move_conflict] {}: '{}' and '{}' pull it opposite ways, {} flips within {} s (at "
                 "{:.1f},{:.1f},{:.1f}, now '{}' to {:.1f},{:.1f},{:.1f})",
                 bot->GetName(), otherSource, source, flipCount,
                 WINDOW_MS / IN_MILLISECONDS, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), source, x,
                 y, z);
        BotMoveEvents::Push(bot->GetGUID(), std::move(conflict));
    }

    bool LaneApplies(Player* bot, std::string const& source)
    {
        if (!WAYPOINT_LANE || !bot || bot->IsInCombat())
            return false;

        TestRunner::Waypoint waypoint;
        return TestRunner::GetWaypoint(bot->GetGUID(), waypoint) && waypoint.mapId == bot->GetMapId() &&
               !ExemptFromLane(source);
    }

    bool VetoesOffWaypoint(Player* bot, std::string const& source, float x, float y, float z,
                           bool encounterInProgress)
    {
        if (!WAYPOINT_LANE || !bot || bot->IsInCombat() || encounterInProgress || ExemptFromLane(source))
            return false;

        TestRunner::Waypoint waypoint;
        if (!TestRunner::GetWaypoint(bot->GetGUID(), waypoint) || waypoint.mapId != bot->GetMapId())
            return false;

        float const bx = bot->GetPositionX();
        float const by = bot->GetPositionY();
        float const wx = waypoint.x - bx;
        float const wy = waypoint.y - by;
        float const waypointLength = std::hypot(wx, wy);
        if (waypointLength < LANE_AT_WAYPOINT)
            return false;

        float const tx = x - bx;
        float const ty = y - by;
        if (std::hypot(tx, ty) <= LANE_WIDTH)
            return false;  // a small step, whatever its direction

        float const along = (tx * wx + ty * wy) / waypointLength;
        float const lateral = std::fabs(tx * wy - ty * wx) / waypointLength;
        float const off = std::max(lateral, -along);
        if (off <= LANE_WIDTH)
            return false;

        uint32 const now = getMSTime();
        bool logIt = false;
        {
            std::lock_guard<std::mutex> lock(ordersLock);
            uint32& loggedMs = botOrders[bot->GetGUID()].vetoLoggedMs[source];
            if (!loggedMs || getMSTimeDiff(loggedMs, now) >= VETO_LOG_MS)
            {
                loggedMs = now ? now : 1;
                logIt = true;
            }
        }

        if (logIt)
        {
            LOG_INFO("playerbots",
                     "[waypoint_lane] {}: '{}' to ({:.1f},{:.1f},{:.1f}) vetoed, {:.1f} yd off the harness waypoint's "
                     "direction to ({:.1f},{:.1f},{:.1f}) (lane {:.0f} yd, no fight on)",
                     bot->GetName(), source, x, y, z, off, waypoint.x, waypoint.y, waypoint.z, LANE_WIDTH);
            BotMoveEvents::Push(bot->GetGUID(), BotMoveEvents::Event{"waypoint_lane_veto", {}, {}}
                                                    .Str("name", bot->GetName())
                                                    .Str("source", source)
                                                    .Real("toX", x).Real("toY", y).Real("toZ", z)
                                                    .Real("off", off).Real("lateral", lateral).Real("along", along)
                                                    .Real("waypointX", waypoint.x).Real("waypointY", waypoint.y)
                                                    .Real("waypointZ", waypoint.z)
                                                    .Real("x", bx).Real("y", by).Real("z", bot->GetPositionZ()));
        }

        return true;
    }

    bool EscortYieldsWaypoint(PlayerbotAI* botAI, float x, float y, float z)
    {
        Player* bot = botAI ? botAI->GetBot() : nullptr;
        if (!bot)
            return false;

        bool waiting = false;
        float waitReturn = 0.0f;
        Creature* escort = EscortProtect::EscortAnchor(botAI, waiting, waitReturn);
        if (!escort)
            return false;

        float const waypointToEscort = escort->GetExactDist2d(x, y);
        if (waiting && waypointToEscort <= waitReturn)
            return false;  // the walk to the waiting NPC (its gossip starts the next leg)

        // [b38_ohb] While the NPC waits, a waypoint that leads back towards it (no farther from it than the bot, but
        // for a route's detour) or a short step (the harness's stall_short_step back onto the navmesh) stands too:
        // only a walk away from it waits. ohb-h 20261026-050341-ohb-h-5A-16 1312-1480 s: the tank off the navmesh
        // at (2122.3,106.2) 110 yd from Thrall; its steps (2123.1,102.2) / (2123.7,98.7) onto the floor 4-8 yd off
        // and the route's (2127.5,78.4) up the keep ramp were all suspended, 200 s idle.
        constexpr float ESCORT_WAIT_SHORT_STEP = 10.0f;
        constexpr float ESCORT_WAIT_ROUTE_DETOUR = 15.0f;
        if (waiting && (bot->GetExactDist(x, y, z) <= ESCORT_WAIT_SHORT_STEP ||
                        waypointToEscort <= bot->GetExactDist2d(escort) + ESCORT_WAIT_ROUTE_DETOUR))
            return false;

        // [b36_ohb] The tank's waypoint near a walking escort that walks into a pack stands: its pull spot ahead of
        // the NPC (players pull ahead of Thrall). ohb-h 20261026-034959 1476 s: the sight spot (2209.3,123.8) 12 yd
        // from Thrall was suspended, the tank went back to him and the pack came onto the healer. Within Thrall's
        // walking leash (EscortProtect.cpp 20 yd), so "escort stay close" does not pull it back.
        constexpr float ESCORT_TANK_AHEAD = 20.0f;
        if (!waiting && waypointToEscort <= ESCORT_TANK_AHEAD && PlayerbotAI::IsTank(bot) &&
            EscortProtect::EscortWalksIntoPack(botAI))
            return false;

        uint32 const now = getMSTime();
        bool logIt = false;
        {
            std::lock_guard<std::mutex> lock(ordersLock);
            uint32& loggedMs = botOrders[bot->GetGUID()].escortYieldLoggedMs;
            if (!loggedMs || getMSTimeDiff(loggedMs, now) >= ESCORT_YIELD_LOG_MS)
            {
                loggedMs = now ? now : 1;
                logIt = true;
            }
        }

        if (logIt)
        {
            LOG_INFO("playerbots",
                     "[escort_anchor] {}: harness waypoint ({:.1f},{:.1f},{:.1f}) suspended, {} {} at "
                     "({:.1f},{:.1f},{:.1f}) leads ({:.0f} yd from it to the waypoint)",
                     bot->GetName(), x, y, z, escort->GetName(), waiting ? "waits" : "walks", escort->GetPositionX(),
                     escort->GetPositionY(), escort->GetPositionZ(), waypointToEscort);
            BotMoveEvents::Push(bot->GetGUID(), BotMoveEvents::Event{"escort_yield", {}, {}}
                                                    .Str("name", bot->GetName())
                                                    .Str("escort", escort->GetName())
                                                    .Num("escortEntry", escort->GetEntry())
                                                    .Str("state", waiting ? "waits" : "walks")
                                                    .Real("escortX", escort->GetPositionX())
                                                    .Real("escortY", escort->GetPositionY())
                                                    .Real("escortZ", escort->GetPositionZ())
                                                    .Real("waypointX", x).Real("waypointY", y).Real("waypointZ", z)
                                                    .Real("waypointToEscort", waypointToEscort)
                                                    .Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                                                    .Real("z", bot->GetPositionZ()));
        }

        return true;
    }
    // [b33_harn] (d) the event hold
    void NoteEventHold(Player* bot, float x, float y, float z, float radius, char const* what)
    {
        if (!bot)
            return;

        std::lock_guard<std::mutex> lock(eventHoldLock);
        EventHold& hold = eventHolds[EventHoldKey(bot)];
        hold.ms = std::max(getMSTime(), 1u);
        hold.mapId = bot->GetMapId();
        hold.instanceId = bot->GetInstanceId();
        hold.x = x;
        hold.y = y;
        hold.z = z;
        hold.radius = radius;
        hold.what = what ? what : "";
    }

    bool GetEventHold(Player* bot, float& x, float& y, float& z, float& radius)
    {
        if (!bot)
            return false;

        std::lock_guard<std::mutex> lock(eventHoldLock);
        if (eventHolds.empty())
            return false;

        auto const itr = eventHolds.find(EventHoldKey(bot));
        if (itr == eventHolds.end())
            return false;

        EventHold const& hold = itr->second;
        if (getMSTimeDiff(hold.ms, getMSTime()) > EVENT_HOLD_TTL_MS || hold.mapId != bot->GetMapId() ||
            hold.instanceId != bot->GetInstanceId())
            return false;

        x = hold.x;
        y = hold.y;
        z = hold.z;
        radius = hold.radius;
        return true;
    }

    bool EventHoldBlocksSpot(Player* bot, float x, float y, char const* source)
    {
        float hx = 0.0f;
        float hy = 0.0f;
        float hz = 0.0f;
        float radius = 0.0f;
        if (!GetEventHold(bot, hx, hy, hz, radius))
            return false;

        float const off = std::hypot(x - hx, y - hy);
        if (off <= radius)
            return false;

        uint32 const now = getMSTime();
        std::string what;
        bool logIt = false;
        {
            std::lock_guard<std::mutex> lock(eventHoldLock);
            EventHold& hold = eventHolds[EventHoldKey(bot)];
            what = hold.what;
            uint32& loggedMs = hold.loggedMs[source ? source : ""];
            if (!loggedMs || getMSTimeDiff(loggedMs, now) >= EVENT_HOLD_LOG_MS)
            {
                loggedMs = now ? now : 1;
                logIt = true;
            }
        }

        if (logIt)
        {
            LOG_INFO("playerbots",
                     "[event_hold] {}: '{}' to ({:.1f},{:.1f}) held, {:.1f} yd from the {} spot ({:.1f},{:.1f},{:.1f}),"
                     " radius {:.0f} yd",
                     bot->GetName(), source ? source : "", x, y, off, what, hx, hy, hz, radius);
            BotMoveEvents::Push(bot->GetGUID(), BotMoveEvents::Event{"event_hold_block", {}, {}}
                                                    .Str("name", bot->GetName())
                                                    .Str("source", source ? source : "")
                                                    .Str("hold", what)
                                                    .Real("toX", x).Real("toY", y)
                                                    .Real("holdX", hx).Real("holdY", hy).Real("holdZ", hz)
                                                    .Real("radius", radius).Real("off", off)
                                                    .Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                                                    .Real("z", bot->GetPositionZ()));
        }

        return true;
    }

    bool EventHoldYieldsWaypoint(Player* bot, float x, float y, float /*z*/)
    {
        return EventHoldBlocksSpot(bot, x, y, "test runner move");
    }
}
