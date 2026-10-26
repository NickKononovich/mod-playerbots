/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [lead1] The pull order registry (LeaderOrders.h, C:/WoW/LEADER_REFACTOR_PLAN.md steps 1-2). Written by the tank on
// its map thread and by the leader on the world thread, read by the leader; one lock, as RouteTarget. Unlike
// RouteTarget an order has no TTL: it lives until a report or the leader closes it.

#include "LeaderOrders.h"

#include "Player.h"
#include "Timer.h"
#include "Unit.h"

#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    struct TankSlot
    {
        bool open = false;
        Leader::PullOrder order;
        std::deque<Leader::OrderReport> queue;
    };

    std::mutex orderLock;
    std::unordered_set<ObjectGuid> tracked;
    std::unordered_map<ObjectGuid, TankSlot> slots;
    uint32 nextOrderId = 0;

    uint32 NowMs()
    {
        uint32 const now = getMSTime();
        return now ? now : 1;
    }

    // caller holds orderLock
    void Queue(TankSlot& slot, Leader::OrderReport report)
    {
        if (slot.queue.size() >= Leader::MAX_QUEUED_REPORTS)
            slot.queue.pop_front();
        slot.queue.push_back(std::move(report));
    }

    Leader::OrderReport ReportOf(Leader::PullOrder const& order, std::string const& why, bool decides)
    {
        Leader::OrderReport report;
        report.orderId = order.id;
        report.tank = order.tank;
        report.target = order.target;
        report.targetName = order.targetName;
        report.targetEntry = order.targetEntry;
        report.state = order.state;
        report.method = order.method;
        report.what = "order_report";
        report.why = why;
        report.mobs = order.expected;
        report.cost = order.cost;
        report.decides = decides;
        report.at = order.changedAt;
        report.ms = order.changedMs;
        return report;
    }

    bool Opens(Leader::OrderState state)
    {
        return state == Leader::OrderState::Issued || state == Leader::OrderState::Walking ||
               state == Leader::OrderState::Tagged;
    }

    // The tank's own decisions in its reports: a commit, a cast, an abort (later the leader decides all three).
    bool TankDecides(Leader::OrderState state)
    {
        return state == Leader::OrderState::Issued || state == Leader::OrderState::Aborted;
    }
}

namespace Leader
{
    char const* MethodName(PullMethod method)
    {
        switch (method)
        {
            case PullMethod::Engage:
                return "engage";
            case PullMethod::Body:
                return "body";
            case PullMethod::PullBack:
                return "pull back";
            case PullMethod::LosPull:
                return "los pull";
            case PullMethod::One:
                return "one";
            case PullMethod::AoeStack:
                return "aoe stack";
            case PullMethod::PatrolIntercept:
                return "patrol intercept";
            case PullMethod::BossPull:
                return "boss pull";
            default:
                return "unknown";
        }
    }

    char const* StateName(OrderState state)
    {
        switch (state)
        {
            case OrderState::Issued:
                return "Issued";
            case OrderState::Walking:
                return "Walking";
            case OrderState::AtStand:
                return "AtStand";
            case OrderState::Tagged:
                return "Tagged";
            case OrderState::WalkingBack:
                return "WalkingBack";
            case OrderState::Holding:
                return "Holding";
            case OrderState::Fighting:
                return "Fighting";
            case OrderState::Done:
                return "Done";
            case OrderState::Aborted:
                return "Aborted";
            default:
                return "?";
        }
    }

    bool IsTerminal(OrderState state) { return state == OrderState::Done || state == OrderState::Aborted; }

    namespace Orders
    {
        void Track(ObjectGuid tank)
        {
            if (tank.IsEmpty())
                return;
            std::lock_guard<std::mutex> lock(orderLock);
            tracked.insert(tank);
        }

        void Untrack(ObjectGuid tank)
        {
            std::lock_guard<std::mutex> lock(orderLock);
            tracked.erase(tank);
            slots.erase(tank);
        }

        void Report(Player* tank, Unit* target, OrderState state, PullMethod method, std::string const& why,
                    uint32 mobs, float cost, bool single)
        {
            if (!tank)
                return;
            ObjectGuid const tankGuid = tank->GetGUID();
            ObjectGuid const targetGuid = target ? target->GetGUID() : ObjectGuid::Empty;
            uint32 const now = NowMs();
            std::lock_guard<std::mutex> lock(orderLock);
            if (!tracked.count(tankGuid))
                return;

            TankSlot& slot = slots[tankGuid];
            PullOrder& order = slot.order;
            bool const sameTarget = slot.open && (targetGuid.IsEmpty() || order.target == targetGuid);

            if (IsTerminal(state))
            {
                // An abort of another mob than the open order's (or with none open) closes nothing, and neither does
                // one during the order's fight: the leader closes that one when the fight is over (Done).
                if (!sameTarget || order.state == OrderState::Fighting)
                    return;
                order.state = state;
                order.why = why;
                order.changedMs = now;
                order.changedAt = tank->GetPosition();
                slot.open = false;
                Queue(slot, ReportOf(order, why, TankDecides(state)));
                return;
            }

            if (slot.open && !sameTarget)
            {
                // A report about another mob: a new order supersedes the open one only when it opens one.
                if (!Opens(state) || !target)
                    return;
                order.state = OrderState::Aborted;
                order.changedMs = now;
                order.changedAt = tank->GetPosition();
                slot.open = false;
                Queue(slot, ReportOf(order, "superseded by " + target->GetName(), false));
            }

            if (!slot.open)
            {
                // A late report of a closed order (its fight ended, it was aborted) opens nothing.
                if (!Opens(state) || !target)
                    return;
                order = PullOrder();
                order.id = ++nextOrderId;
                order.tank = tankGuid;
                order.target = targetGuid;
                order.targetName = target->GetName();
                order.targetEntry = target->GetEntry();
                order.method = method;
                order.state = state;
                order.expected = mobs;
                order.cost = cost;
                order.singleOnly = single;
                order.origin = why;
                order.why = why;
                order.openedMs = now;
                order.changedMs = now;
                order.changedAt = tank->GetPosition();
                slot.open = true;
                Queue(slot, ReportOf(order, why, TankDecides(state)));
                return;
            }

            // The open order of this mob. A fresh plan of the same mob refreshes it (the planner's every look while
            // the pull runs); a repeat of the current state is no change.
            if (method != PullMethod::Unknown)
                order.method = method;
            if (mobs)
            {
                order.expected = mobs;
                order.cost = cost;
            }
            // Once the leader saw the order's fight (Fighting), only its end changes it: a pull step of the tank's
            // during the fight is no new state.
            if (state == order.state || state == OrderState::Issued || order.state == OrderState::Fighting)
                return;
            order.state = state;
            order.why = why;
            order.changedMs = now;
            order.changedAt = tank->GetPosition();
            Queue(slot, ReportOf(order, why, TankDecides(state)));
        }

        void Note(Player* tank, Unit* target, std::string const& what, std::string const& why)
        {
            if (!tank)
                return;
            std::lock_guard<std::mutex> lock(orderLock);
            if (!tracked.count(tank->GetGUID()))
                return;
            TankSlot& slot = slots[tank->GetGUID()];
            OrderReport report;
            report.orderId = slot.open ? slot.order.id : 0;
            report.tank = tank->GetGUID();
            if (target)
            {
                report.target = target->GetGUID();
                report.targetName = target->GetName();
                report.targetEntry = target->GetEntry();
            }
            report.state = slot.open ? slot.order.state : OrderState::Issued;
            report.method = slot.open ? slot.order.method : PullMethod::Unknown;
            report.what = what;
            report.why = why;
            report.decides = true;
            report.at = tank->GetPosition();
            report.ms = NowMs();
            Queue(slot, std::move(report));
        }

        bool GetOpen(ObjectGuid tank, PullOrder& order)
        {
            std::lock_guard<std::mutex> lock(orderLock);
            auto const it = slots.find(tank);
            if (it == slots.end() || !it->second.open)
                return false;
            order = it->second.order;
            return true;
        }

        bool IsOpen(ObjectGuid tank)
        {
            std::lock_guard<std::mutex> lock(orderLock);
            auto const it = slots.find(tank);
            return it != slots.end() && it->second.open;
        }

        bool Advance(ObjectGuid tank, OrderState state, std::string const& why, Position const& at)
        {
            std::lock_guard<std::mutex> lock(orderLock);
            auto const it = slots.find(tank);
            if (it == slots.end() || !it->second.open || it->second.order.state == state || IsTerminal(state))
                return false;
            PullOrder& order = it->second.order;
            order.state = state;
            order.why = why;
            order.changedMs = NowMs();
            order.changedAt = at;
            return true;
        }

        bool Close(ObjectGuid tank, OrderState state, std::string const& why, Position const& at, PullOrder* closed)
        {
            std::lock_guard<std::mutex> lock(orderLock);
            auto const it = slots.find(tank);
            if (it == slots.end() || !it->second.open)
                return false;
            PullOrder& order = it->second.order;
            order.state = IsTerminal(state) ? state : OrderState::Aborted;
            order.why = why;
            order.changedMs = NowMs();
            order.changedAt = at;
            it->second.open = false;
            if (closed)
                *closed = order;
            return true;
        }

        bool TakeReport(ObjectGuid tank, OrderReport& report)
        {
            std::lock_guard<std::mutex> lock(orderLock);
            auto const it = slots.find(tank);
            if (it == slots.end() || it->second.queue.empty())
                return false;
            report = std::move(it->second.queue.front());
            it->second.queue.pop_front();
            return true;
        }
    }
}
