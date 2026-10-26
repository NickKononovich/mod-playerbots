/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [lead1] The pull order registry (LeaderOrders.h, C:/WoW/LEADER_REFACTOR_PLAN.md steps 1-2). Written by the tank on
// its map thread and by the leader on the world thread, read by the leader; one lock, as RouteTarget. Unlike
// RouteTarget an order has no TTL: it lives until a report or the leader closes it.

#include "LeaderOrders.h"

#include "Group.h"
#include "Map.h"
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
                    uint32 mobs, float cost, bool single, bool leaderIssued)
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
                // [lead11] an abort of a leader-issued order (the walk for range would leave the healer, another
                // floor) is the executor's report by the order's abort rules (plan 2.2), not a tank decision: b55
                // counted 9 false two_owners (pull_order vs "tank: order_report Aborted decides true")
                Queue(slot, ReportOf(order, why, TankDecides(state) && !leaderIssued && !order.leaderIssued));
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
                order.origin = leaderIssued ? "leader: " + why : why;  // [lead7]
                order.leaderIssued = leaderIssued;                    // [lead11]
                order.why = why;
                order.openedMs = now;
                order.changedMs = now;
                order.changedAt = tank->GetPosition();
                slot.open = true;
                Queue(slot, ReportOf(order, why, TankDecides(state) && !leaderIssued));
                return;
            }

            // The open order of this mob. A fresh plan of the same mob refreshes it (the planner's every look while
            // the pull runs); a repeat of the current state is no change.
            if (leaderIssued)
                order.leaderIssued = true;  // [lead11] the leader's fresh issue of the same mob
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
            Queue(slot, ReportOf(order, why, TankDecides(state) && !leaderIssued && !order.leaderIssued));  // [lead11]
        }

        void Note(Player* tank, Unit* target, std::string const& what, std::string const& why, bool leaderIssued)
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
            report.decides = !leaderIssued;  // [lead7]
            report.at = tank->GetPosition();
            report.ms = NowMs();
            Queue(slot, std::move(report));
        }

        bool IsTracked(ObjectGuid tank)
        {
            std::lock_guard<std::mutex> lock(orderLock);
            return tracked.count(tank) != 0;
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

    // [lead6] Step 6: the leader's readiness verdict per group (LeaderOrders.h Readiness).
    namespace Readiness
    {
        namespace
        {
            struct Verdict
            {
                bool ready = false;
                std::string why;
                uint32 ms = 0;
            };
            std::mutex readyLock;
            std::unordered_map<ObjectGuid, Verdict> verdicts;
        }

        void Publish(ObjectGuid group, bool ready, std::string const& why)
        {
            if (group.IsEmpty())
                return;
            std::lock_guard<std::mutex> lock(readyLock);
            Verdict& verdict = verdicts[group];
            verdict.ready = ready;
            verdict.why = why;
            verdict.ms = NowMs();
        }

        void Clear(ObjectGuid group)
        {
            std::lock_guard<std::mutex> lock(readyLock);
            verdicts.erase(group);
        }

        bool Get(ObjectGuid group, bool& ready, std::string* why)
        {
            std::lock_guard<std::mutex> lock(readyLock);
            auto const found = verdicts.find(group);
            if (found == verdicts.end() || getMSTimeDiff(found->second.ms, getMSTime()) > LEADER_READY_FRESH_MS)
                return false;
            ready = found->second.ready;
            if (why)
                *why = found->second.why;
            return true;
        }
    }

    // [lead15] LedGroup / Events (LeaderOrders.h)
    bool LedGroup(Player* member)
    {
        Group* group = member ? member->GetGroup() : nullptr;
        bool ready = false;
        return group && Readiness::Get(group->GetGUID(), ready);
    }

    bool LedBot(Player* bot)
    {
        if (!LEADER_MEMBER_EVENTS || !bot || !bot->IsInWorld())
            return false;
        Map* map = bot->GetMap();
        return map && map->IsDungeon() && !map->IsRaid() && LedGroup(bot);
    }

    bool ExecutorMember(Player* member)
    {
        if (!LEADER_MEMBER_EVENTS || !LEADER_MEMBER_EXECUTOR || !member || !member->IsInWorld())
            return false;
        Map* map = member->GetMap();
        if (!map || !map->IsDungeon() || map->IsRaid() || Orders::IsTracked(member->GetGUID()))
            return false;
        return LedGroup(member);
    }

    namespace Events
    {
        namespace
        {
            struct Slot
            {
                MemberEvents events;
                std::deque<MemberAck> acks;
                std::deque<OwnDecision> owns;
                uint32 stale = 0;
                uint32 lastAckSeq = 0;
                std::string lastAckWhat;
            };
            std::mutex eventLock;
            std::unordered_map<ObjectGuid, Slot> eventSlots;
            constexpr size_t MAX_ACKS = 32;

            bool Stale(uint32 ms, uint32 now) { return !ms || getMSTimeDiff(ms, now) > LEADER_EVENT_TTL_MS; }
        }

        bool SetMove(ObjectGuid member, MoveEvent move, Position const& spot, float tolerance, std::string const& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            MemberEvents& e = eventSlots[member].events;
            bool const changed = e.move != move || e.spot.GetExactDist(spot) > 1.0f || Stale(e.moveMs, getMSTime());
            e.move = move;
            e.spot = spot;
            e.tolerance = tolerance;
            e.moveWhy = why;
            e.moveMs = NowMs();
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetAttack(ObjectGuid member, AttackEvent attack, ObjectGuid target,
                       std::vector<ObjectGuid> const& killOrder)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            MemberEvents& e = eventSlots[member].events;
            bool const changed = e.attack != attack || e.target != target || Stale(e.attackMs, getMSTime());
            e.attack = attack;
            e.target = target;
            e.killOrder = killOrder;
            e.attackMs = NowMs();
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetIntensity(ObjectGuid member, Intensity intensity)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            MemberEvents& e = eventSlots[member].events;
            bool const changed = e.intensity != intensity || Stale(e.intensityMs, getMSTime());
            e.intensity = intensity;
            e.intensityMs = NowMs();
            if (changed)
                ++e.seq;
            return changed;
        }

        // the AoE rule rides on the Attack field's freshness (set together with it)
        bool SetAoe(ObjectGuid member, AoeRule aoe, Position const& center, float radius)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            MemberEvents& e = eventSlots[member].events;
            bool const changed =
                e.aoe != aoe || (aoe == AoeRule::AllowedOnPack && e.aoeCenter.GetExactDist(center) > 3.0f);
            e.aoe = aoe;
            e.aoeCenter = center;
            e.aoeRadius = radius;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetCc(ObjectGuid member, ObjectGuid mob)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            MemberEvents& e = eventSlots[member].events;
            bool const changed = e.cc != mob || Stale(e.ccMs, getMSTime());
            e.cc = mob;
            e.ccMs = NowMs();
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetInterrupt(ObjectGuid member, ObjectGuid caster)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            MemberEvents& e = eventSlots[member].events;
            bool const changed = e.interrupt != caster || Stale(e.interruptMs, getMSTime());
            e.interrupt = caster;
            e.interruptMs = NowMs();
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetThreat(ObjectGuid member, uint32 spell, ObjectGuid target)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            MemberEvents& e = eventSlots[member].events;
            bool const changed = e.threatSpell != spell || e.threatTarget != target || Stale(e.threatMs, getMSTime());
            e.threatSpell = spell;
            e.threatTarget = target;
            e.threatMs = NowMs();
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetPreSave(ObjectGuid member, uint32 untilMs, std::string const& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            MemberEvents& e = eventSlots[member].events;
            bool const changed = e.preSaveUntilMs != untilMs || Stale(e.preSaveMs, getMSTime());
            e.preSaveUntilMs = untilMs;
            e.preSaveWhy = why;
            e.preSaveMs = NowMs();
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetLoot(ObjectGuid member, bool loot)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            MemberEvents& e = eventSlots[member].events;
            bool const changed = e.loot != loot || Stale(e.lootMs, getMSTime());
            e.loot = loot;
            e.lootMs = NowMs();
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetDontAttack(ObjectGuid member, ObjectGuid mob)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            MemberEvents& e = eventSlots[member].events;
            bool const changed = e.dontAttack != mob || Stale(e.dontAttackMs, getMSTime());
            e.dontAttack = mob;
            e.dontAttackMs = NowMs();
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetExternal(ObjectGuid member, uint32 spell, ObjectGuid target)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            MemberEvents& e = eventSlots[member].events;
            bool const changed =
                e.externalSpell != spell || e.externalTarget != target || Stale(e.externalMs, getMSTime());
            e.externalSpell = spell;
            e.externalTarget = target;
            e.externalMs = NowMs();
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetReady(ObjectGuid member, ReadyEvent ready)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            MemberEvents& e = eventSlots[member].events;
            bool const changed = e.ready != ready || Stale(e.readyMs, getMSTime());
            e.ready = ready;
            e.readyMs = NowMs();
            if (changed)
                ++e.seq;
            return changed;
        }

        void Clear(ObjectGuid member)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            eventSlots.erase(member);
        }

        bool Get(ObjectGuid member, MemberEvents& out)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end())
                return false;
            out = found->second.events;
            uint32 const now = getMSTime();
            if (Stale(out.moveMs, now))
                out.move = MoveEvent::None;
            if (Stale(out.attackMs, now))
                out.attack = AttackEvent::None;
            if (Stale(out.attackMs, now))
                out.aoe = AoeRule::Unset;
            if (Stale(out.intensityMs, now))
                out.intensity = Intensity::Normal;
            if (Stale(out.ccMs, now))
                out.cc.Clear();
            if (Stale(out.interruptMs, now))
                out.interrupt.Clear();
            if (Stale(out.threatMs, now))
            {
                out.threatSpell = 0;
                out.threatTarget.Clear();
            }
            if (Stale(out.dontAttackMs, now))
                out.dontAttack.Clear();
            if (Stale(out.lootMs, now))
                out.loot = false;
            if (Stale(out.preSaveMs, now) || (out.preSaveUntilMs && int32(now - out.preSaveUntilMs) > 0))
            {
                out.preSaveUntilMs = 0;
                out.preSaveWhy.clear();
            }
            if (Stale(out.externalMs, now))
            {
                out.externalSpell = 0;
                out.externalTarget.Clear();
            }
            if (Stale(out.readyMs, now))
                out.ready = ReadyEvent::None;
            return out.move != MoveEvent::None || out.attack != AttackEvent::None ||
                   out.intensity != Intensity::Normal || out.ready != ReadyEvent::None || !out.cc.IsEmpty() ||
                   !out.interrupt.IsEmpty() || out.externalSpell || !out.threatTarget.IsEmpty() ||
                   !out.dontAttack.IsEmpty() || out.loot || out.preSaveUntilMs;
        }

        void Ack(ObjectGuid member, uint32 seq, std::string const& what, std::string const& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end())
                return;
            Slot& slot = found->second;
            if (slot.lastAckSeq == seq && slot.lastAckWhat == what)
                return;  // one ack per event and kind
            slot.lastAckSeq = seq;
            slot.lastAckWhat = what;
            if (slot.acks.size() >= MAX_ACKS)
                slot.acks.pop_front();
            slot.acks.push_back(MemberAck{member, seq, what, why, NowMs()});
        }

        void NoteOwn(ObjectGuid member, OwnDecision const& own)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            if (slot.owns.size() >= 4 * MAX_ACKS)
                slot.owns.pop_front();
            slot.owns.push_back(own);
            slot.owns.back().ms = NowMs();
        }

        void NoteStale(ObjectGuid member)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            ++eventSlots[member].stale;
        }

        uint32 TakeStale(ObjectGuid member)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end())
                return 0;
            uint32 const stale = found->second.stale;
            found->second.stale = 0;
            return stale;
        }

        bool TakeOwn(ObjectGuid member, OwnDecision& out)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end() || found->second.owns.empty())
                return false;
            out = std::move(found->second.owns.front());
            found->second.owns.pop_front();
            return true;
        }

        bool TakeAck(ObjectGuid member, MemberAck& out)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end() || found->second.acks.empty())
                return false;
            out = std::move(found->second.acks.front());
            found->second.acks.pop_front();
            return true;
        }
    }
}
