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
#include "Log.h"
#include "Map.h"
#include "MoveSpline.h"
#include "Player.h"
#include "Timer.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
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
            // [lead27] no age: the verdict holds until the leader publishes another or the run ends (Clear)
            if (found == verdicts.end())
                return false;
            ready = found->second.ready;
            if (why)
                *why = found->second.why;
            return true;
        }
    }

    namespace PullPack
    {
        namespace
        {
            std::mutex packLock;
            std::unordered_map<ObjectGuid, std::vector<ObjectGuid>> packs;
        }

        void Set(ObjectGuid group, std::vector<ObjectGuid> const& mobs)
        {
            std::lock_guard<std::mutex> lock(packLock);
            packs[group] = mobs;
        }

        void Clear(ObjectGuid group)
        {
            std::lock_guard<std::mutex> lock(packLock);
            packs.erase(group);
        }

        bool Contains(Player* bot, ObjectGuid mob)
        {
            Group* group = bot ? bot->GetGroup() : nullptr;
            if (!group || mob.IsEmpty())
                return false;
            std::lock_guard<std::mutex> lock(packLock);
            auto const found = packs.find(group->GetGUID());
            return found != packs.end() &&
                   std::find(found->second.begin(), found->second.end(), mob) != found->second.end();
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

    bool WalksOffStand(Player* member, MemberEvents const& events)
    {
        if ((events.move != MoveEvent::StandAt && events.move != MoveEvent::Hold) || !ExecutorMember(member) ||
            !member->isMoving() || member->movespline->Finalized())
            return false;
        G3D::Vector3 const end = member->movespline->FinalDestination();
        return events.spot.GetExactDist2d(end.x, end.y) > std::max(events.tolerance, 1.0f);
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
                std::map<std::string, uint32> vetoedEvals;
                uint32 lastAckSeq = 0;
                std::string lastAckWhat;
                // [lead69] the command before the current one, and the tick the current one was set (ping-pong guard)
                std::string prevMoveWhy;
                Position prevMoveSpot;
                uint32 moveSetGen = 0;
                uint32 dupesBlocked = 0;
                uint32 holdPhase = 0;  // [lead74] the move phase the current move was issued in
                uint32 gen = 1;  // [lead27] the leader's tick generation: a field asserted in it is current
            };
            std::mutex eventLock;
            std::unordered_map<ObjectGuid, Slot> eventSlots;
            constexpr size_t MAX_ACKS = 32;

        }

        namespace
        {
            // [lead127] b159 20261101-113309 127.5-128.4 s: Rtshaab's position had three owners in one fight ("in
            // reach of its target", the follow rule's "along with the tank: to the meeting point" 136 yd off and its
            // stay, the combat spot). A member under the leader's Attack fights: its position belongs to the fight's
            // rules; the out-of-fight follow ("along with the tank ...", its stays included) is no move for it
            bool Barred(Slot const& slot, std::string const& why)
            {
                MemberEvents const& e = slot.events;
                return e.attack == AttackEvent::Attack && e.attackMs + 1 >= slot.gen &&
                       why.rfind("along with the tank", 0) == 0;
            }
        }

        bool AtCommandedSpot(Player* member)
        {
            if (!member || !ExecutorMember(member))
                return true;
            if (member->isMoving())
                return false;
            MemberEvents events;
            if (!Get(member->GetGUID(), events))
                return true;
            if (events.move != MoveEvent::MoveTo && events.move != MoveEvent::StandAt &&
                events.move != MoveEvent::Hold)
                return true;
            return member->GetExactDist2d(events.spot.GetPositionX(), events.spot.GetPositionY()) <=
                   std::max(events.tolerance, LEADER_SPOT_TOLERANCE);
        }

        bool MoveBarred(ObjectGuid member, std::string const& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            return found != eventSlots.end() && Barred(found->second, why);
        }

        bool SetMove(ObjectGuid member, MoveEvent move, Position const& spot, float tolerance, std::string const& why)
        {
            // [lead161] the leader never issues a spot that is no number
            if (!std::isfinite(spot.GetPositionX()) || !std::isfinite(spot.GetPositionY()) ||
                !std::isfinite(spot.GetPositionZ()))
            {
                LOG_ERROR("playerbots", "[lead161] non_finite_spot: {} for {}: ({},{},{}) not issued", why,
                          member.ToString(), spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ());
                return false;
            }
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            if (Barred(slot, why))
                return false;
            // [lead64] the position arbiter: a move asserted this tick by a rule of higher precedence holds - a lower
            // one cannot overwrite it in the same tick (b109: "aggro loss: to the tank" then "combat spot" at 32.6 s)
            // [lead69] ... nor one it asserted last tick (b114: heal reach <-> combat spot flipped every tick, 204
            // commands in one second - the lower rule came first in the next tick)
            if (e.move != MoveEvent::None && e.moveMs + 1 >= slot.gen &&
                MovePrecedence(e.moveWhy) > MovePrecedence(why))
                return false;
            // [lead55] user: «пожалей мой CPU» - b100: Rtdruaa 5 073 commands (813 in one second). The same command
            // is no new command: a move changes only when its point leaves the current one's arrival tolerance
            // (the walk's own, at least the old 1 yd) or its purpose changes; an unchanged one keeps its point
            float const keep = std::max(std::max(e.tolerance, tolerance), 1.0f);
            bool const changed = e.move != move || e.moveWhy != why || e.spot.GetExactDist(spot) > keep;
            e.move = move;
            // [lead69] the arbiter emits no command identical to the one before the current within the next tick
            // (member, purpose, point within tolerance): command_dupe_blocked - should be 0
            if (changed && why == slot.prevMoveWhy && slot.prevMoveSpot.GetExactDist(spot) <= keep &&
                slot.moveSetGen + 1 >= slot.gen)
            {
                ++slot.dupesBlocked;
                e.moveMs = slot.gen;
                return false;
            }
            if (changed)
            {
                slot.prevMoveWhy = e.moveWhy;
                slot.prevMoveSpot = e.spot;
                slot.moveSetGen = slot.gen;
                e.spot = spot;
                e.tolerance = tolerance;
                e.moveWhy = why;
            }
            e.moveMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetHealTarget(ObjectGuid member, ObjectGuid target)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed = e.healTarget != target;
            e.healTarget = target;
            e.healTargetMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetAttack(ObjectGuid member, AttackEvent attack, ObjectGuid target,
                       std::vector<ObjectGuid> const& killOrder)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed = e.attack != attack || e.target != target;
            e.attack = attack;
            e.target = target;
            e.killOrder = killOrder;
            e.attackMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetIntensity(ObjectGuid member, Intensity intensity)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed = e.intensity != intensity;
            e.intensity = intensity;
            e.intensityMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        // the AoE rule rides on the Attack field's freshness (set together with it)
        bool SetAoe(ObjectGuid member, AoeRule aoe, Position const& center, float radius)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            // [lead67] a new AoE command only when its rule changes; the centre follows the target (no re-issue)
            bool const changed = e.aoe != aoe;
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
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed = e.cc != mob;
            e.cc = mob;
            e.ccMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetInterrupt(ObjectGuid member, ObjectGuid caster)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed = e.interrupt != caster;
            e.interrupt = caster;
            e.interruptMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetFleeStop(ObjectGuid member, ObjectGuid mob)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed = e.fleeStop != mob;
            e.fleeStop = mob;
            e.fleeStopMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetThreat(ObjectGuid member, uint32 spell, ObjectGuid target)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed = e.threatSpell != spell || e.threatTarget != target;
            e.threatSpell = spell;
            e.threatTarget = target;
            e.threatMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetPreSave(ObjectGuid member, uint32 untilMs, std::string const& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed = e.preSaveUntilMs != untilMs;
            e.preSaveUntilMs = untilMs;
            e.preSaveWhy = why;
            e.preSaveMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetLoot(ObjectGuid member, bool loot)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed = e.loot != loot;
            e.loot = loot;
            e.lootMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetDontAttack(ObjectGuid member, ObjectGuid mob)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed = e.dontAttack != mob;
            e.dontAttack = mob;
            e.dontAttackMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetExternal(ObjectGuid member, uint32 spell, ObjectGuid target)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed =
                e.externalSpell != spell || e.externalTarget != target;
            e.externalSpell = spell;
            e.externalTarget = target;
            e.externalMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetReady(ObjectGuid member, ReadyEvent ready)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed = e.ready != ready;
            e.ready = ready;
            e.readyMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        void StampPhase(ObjectGuid member, uint32 phase)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            eventSlots[member].holdPhase = phase;
        }

        void HoldMove(ObjectGuid member, Position const& at, uint32 phase)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end())
                return;
            Slot& slot = found->second;
            MemberEvents& e = slot.events;
            // its purpose still holds (the same move phase: no fight start / end, no new order or commitment, the
            // same fight target alive) and the member is still on its way: the stored decision stands this tick too
            if ((e.move == MoveEvent::MoveTo || e.move == MoveEvent::StandAt) && e.moveMs != slot.gen &&
                slot.holdPhase == phase &&
                at.GetExactDist2d(e.spot.GetPositionX(), e.spot.GetPositionY()) > e.tolerance)
                e.moveMs = slot.gen;
        }

        uint32 TakeDupesBlocked(ObjectGuid member)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end())
                return 0;
            uint32 const dupes = found->second.dupesBlocked;
            found->second.dupesBlocked = 0;
            return dupes;
        }

        int32 MovePrecedence(std::string const& why)
        {
            // hazard / puddle > aggro-loss run > heal reach / sight > combat spot > the pull's spot > walk-along
            auto const starts = [&why](char const* prefix) { return why.rfind(prefix, 0) == 0; };
            if (starts("hazard"))
                return 5;
            if (starts("aggro loss"))
                return 4;
            if (starts("heal reach") || starts("sight"))
                return 3;
            if (starts("combat spot") || starts("chase"))  // [lead156] the melee chase is its combat spot
                return 2;
            if (starts("its spot for the pull") || starts("along with the tank: to range"))
                return 1;
            return 0;
        }

        void BeginTick(ObjectGuid member)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            ++eventSlots[member].gen;
        }

        void EndTick(ObjectGuid member)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end())
                return;
            Slot& slot = found->second;
            MemberEvents& e = slot.events;
            uint32 const gen = slot.gen;
            bool changed = false;
            auto const drop = [&](uint32 stamp) { return stamp != gen; };
            if (e.move != MoveEvent::None && drop(e.moveMs))
            {
                e.move = MoveEvent::None;
                changed = true;
            }
            if ((e.attack != AttackEvent::None || e.aoe != AoeRule::Unset) && drop(e.attackMs))
            {
                e.attack = AttackEvent::None;
                e.aoe = AoeRule::Unset;
                changed = true;
            }
            if (e.intensity != Intensity::Normal && drop(e.intensityMs))
            {
                e.intensity = Intensity::Normal;
                changed = true;
            }
            if (!e.cc.IsEmpty() && drop(e.ccMs))
            {
                e.cc.Clear();
                changed = true;
            }
            if (!e.interrupt.IsEmpty() && drop(e.interruptMs))
            {
                e.interrupt.Clear();
                changed = true;
            }
            if (!e.fleeStop.IsEmpty() && drop(e.fleeStopMs))
            {
                e.fleeStop.Clear();
                changed = true;
            }
            if (!e.threatTarget.IsEmpty() && drop(e.threatMs))
            {
                e.threatSpell = 0;
                e.threatTarget.Clear();
                changed = true;
            }
            if (!e.healTarget.IsEmpty() && drop(e.healTargetMs))
            {
                e.healTarget.Clear();
                changed = true;
            }
            if (!e.dontAttack.IsEmpty() && drop(e.dontAttackMs))
            {
                e.dontAttack.Clear();
                changed = true;
            }
            if (e.loot && drop(e.lootMs))
            {
                e.loot = false;
                changed = true;
            }
            if (e.preSaveUntilMs && drop(e.preSaveMs))
            {
                e.preSaveUntilMs = 0;
                e.preSaveWhy.clear();
                changed = true;
            }
            if (e.externalSpell && drop(e.externalMs))
            {
                e.externalSpell = 0;
                e.externalTarget.Clear();
                changed = true;
            }
            if (e.ready != ReadyEvent::None && drop(e.readyMs))
            {
                e.ready = ReadyEvent::None;
                changed = true;
            }
            if (changed)
                ++e.seq;
        }

        bool SetFollow(ObjectGuid member, ObjectGuid target, uint8 index, std::string const& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            bool const changed = e.move != MoveEvent::Follow || e.followTarget != target || e.followIndex != index;
            e.move = MoveEvent::Follow;
            e.followTarget = target;
            e.followIndex = index;
            e.tolerance = 2.5f;
            e.moveWhy = why;
            e.moveMs = slot.gen;
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
            // [lead27] a command stays current until the leader issues another (EndTick clears what it no longer
            // asserts): no age, no TTL. The PreSave's hit time is the game's
            if (out.preSaveUntilMs && int32(now - out.preSaveUntilMs) > 0)
            {
                out.preSaveUntilMs = 0;
                out.preSaveWhy.clear();
            }
            return out.move != MoveEvent::None || out.attack != AttackEvent::None ||
                   out.intensity != Intensity::Normal || out.ready != ReadyEvent::None || !out.cc.IsEmpty() ||
                   !out.interrupt.IsEmpty() || !out.fleeStop.IsEmpty() || out.externalSpell ||
                   !out.threatTarget.IsEmpty() ||
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

        void NoteVetoedEval(ObjectGuid member, std::string const& what)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            ++eventSlots[member].vetoedEvals[what];
        }

        bool TakeVetoedEval(ObjectGuid member, std::string& what, uint32& count)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end() || found->second.vetoedEvals.empty())
                return false;
            auto const first = found->second.vetoedEvals.begin();
            what = first->first;
            count = first->second;
            found->second.vetoedEvals.erase(first);
            return true;
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
