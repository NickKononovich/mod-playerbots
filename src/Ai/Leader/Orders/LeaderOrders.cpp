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
#include "PlayerbotAI.h"  // [lead1140] IsTank
#include "Timer.h"
#include "Unit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
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
            std::unordered_map<ObjectGuid, std::vector<ObjectGuid>> held;  // [lead1140]
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
            held.erase(group);
        }

        bool Contains(Player* bot, ObjectGuid mob)
        {
            Group* group = bot ? bot->GetGroup() : nullptr;
            if (!group || mob.IsEmpty())
                return false;
            bool const tank = PlayerbotAI::IsTank(bot);
            std::lock_guard<std::mutex> lock(packLock);
            if (!tank)
                if (auto const kept = held.find(group->GetGUID());
                    kept != held.end() &&
                    std::find(kept->second.begin(), kept->second.end(), mob) != kept->second.end())
                    return false;
            auto const found = packs.find(group->GetGUID());
            return found != packs.end() &&
                   std::find(found->second.begin(), found->second.end(), mob) != found->second.end();
        }

        void SetHeld(ObjectGuid group, std::vector<ObjectGuid> const& mobs)
        {
            std::lock_guard<std::mutex> lock(packLock);
            if (mobs.empty())
                held.erase(group);
            else
                held[group] = mobs;
        }

        std::vector<ObjectGuid> Held(Player* bot)
        {
            Group* group = bot ? bot->GetGroup() : nullptr;
            if (!group)
                return {};
            std::lock_guard<std::mutex> lock(packLock);
            auto const found = held.find(group->GetGUID());
            return found != held.end() ? found->second : std::vector<ObjectGuid>{};
        }
    }

    namespace SelfLead
    {
        namespace
        {
            std::mutex selfLeadLock;
            std::unordered_set<ObjectGuid> selfLeading;  // groups whose leader is down in a fight
        }

        void Set(ObjectGuid group, bool on)
        {
            std::lock_guard<std::mutex> lock(selfLeadLock);
            if (on)
                selfLeading.insert(group);
            else
                selfLeading.erase(group);
        }

        bool On(ObjectGuid group)
        {
            std::lock_guard<std::mutex> lock(selfLeadLock);
            return selfLeading.count(group) != 0;
        }
    }

    namespace BossOutOfReach
    {
        namespace
        {
            std::mutex outOfReachLock;
            std::unordered_map<ObjectGuid, std::string> outOfReach;  // group -> why
        }

        void Note(ObjectGuid group, std::string const& why)
        {
            std::lock_guard<std::mutex> lock(outOfReachLock);
            outOfReach[group] = why;
        }

        bool Take(ObjectGuid group, std::string& why)
        {
            std::lock_guard<std::mutex> lock(outOfReachLock);
            auto const found = outOfReach.find(group);
            if (found == outOfReach.end())
                return false;
            why = found->second;
            outOfReach.erase(found);
            return true;
        }
    }

    // [lead15] LedGroup / Events (LeaderOrders.h)
    bool LedGroup(Player* member)
    {
        Group* group = member ? member->GetGroup() : nullptr;
        bool ready = false;
        return group && Readiness::Get(group->GetGUID(), ready) && !SelfLead::On(group->GetGUID());
    }

    bool LedBot(Player* bot)
    {
        if (!LEADER_MEMBER_EVENTS || !bot || !bot->IsInWorld())
            return false;
        Map* map = bot->GetMap();
        return map && map->IsDungeon() && !map->IsRaid() && LedGroup(bot);
    }

    namespace RaidBuffOrders
    {
        namespace
        {
            std::mutex raidBuffLock;
            std::unordered_map<ObjectGuid, uint32> raidBuffHeld;  // member -> its buff order's spell
        }

        void Set(ObjectGuid member, uint32 spell)
        {
            std::lock_guard<std::mutex> lock(raidBuffLock);
            raidBuffHeld[member] = spell;
        }

        uint32 Clear(ObjectGuid member)
        {
            std::lock_guard<std::mutex> lock(raidBuffLock);
            auto const found = raidBuffHeld.find(member);
            if (found == raidBuffHeld.end())
                return 0;
            uint32 const spell = found->second;
            raidBuffHeld.erase(found);
            return spell;
        }

        bool Holds(ObjectGuid member, uint32 spell)
        {
            std::lock_guard<std::mutex> lock(raidBuffLock);
            auto const found = raidBuffHeld.find(member);
            return spell && found != raidBuffHeld.end() && found->second == spell;
        }
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
            // [lead320] a member's command domains (one arbiter each)
            enum Domain : uint8
            {
                DOMAIN_MOVE,
                DOMAIN_ATTACK,
                DOMAIN_INTENSITY,
                DOMAIN_READY,
                DOMAIN_EXTERNAL,
                DOMAIN_CC,
                DOMAIN_INTERRUPT,
                DOMAIN_FLEE_STOP,
                DOMAIN_THREAT,
                DOMAIN_DONT_ATTACK,
                DOMAIN_HEAL_TARGET,
                DOMAIN_LOOT,
                DOMAIN_PRE_SAVE,
                DOMAIN_DISPEL,   // [lead400]
                DOMAIN_CONSUME,  // [lead402]
                DOMAIN_COUNT
            };
            // [lead320] one domain's commands as the arbiter sees them (an empty key: no command)
            struct Track
            {
                std::string current;
                std::string currentRule;
                uint32 currentFact = 0;
                std::string previous;  // the command before the current one
                uint32 previousFact = 0;
                uint32 assertGen = 0;  // the tick a rule last asserted the current command
                std::string start;     // the command at this tick's start (the incumbent) and its rule
                std::string startRule;
                uint32 startAssertGen = 0;  // the tick the incumbent's rule last asserted
            };

            struct Slot
            {
                MemberEvents events;
                std::deque<MemberAck> acks;
                std::deque<OwnDecision> owns;
                uint32 stale = 0;
                uint32 externalFailed = 0;  // [lead272] the External the member reported it cannot cast
                std::string externalFailedWhy;
                uint32 dispelFailed = 0;  // [lead400] the Dispel / [lead402] Consume the member reported it cannot do
                std::string dispelFailedWhy;
                uint32 consumeFailed = 0;
                std::string consumeFailedWhy;
                bool moveRefusalNew = false;  // [lead372] not taken yet
                bool moveRefused = false;     // kept until the leader clears it
                MoveRefusal moveRefusal;
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
                // [lead320] the arbiter's view per domain, and the fact the commands are judged in
                std::array<Track, DOMAIN_COUNT> tracks;
                uint32 fact = 0;
                int32 externalPrecedence = 0;
                std::map<std::string, uint32> contradictionCounts;
                std::deque<Contradiction> contradictions;
            };
            std::mutex eventLock;
            std::unordered_map<ObjectGuid, Slot> eventSlots;
            constexpr size_t MAX_ACKS = 32;

            // [lead320] the domains of a member's commands
            char const* DomainName(Domain domain)
            {
                static constexpr char const* names[DOMAIN_COUNT] = {
                    "move",     "attack", "intensity",  "ready",      "external", "cc",     "interrupt",
                    "fleeStop", "threat", "dontAttack", "healTarget", "loot",     "preSave",
                    "dispel",   "consume"};
                return names[domain];
            }

            // [lead320] the rule of a move: its purpose's text before the first ':' (a rule's own fallbacks, "X:
            // stays (...)", are the same rule)
            std::string MoveRule(std::string const& why) { return why.substr(0, why.find(':')); }

            void NoteContradiction(Slot& slot, Domain domain, std::string const& was, std::string const& current,
                                   std::string const& again, bool refused)
            {
                uint32& count =
                    slot.contradictionCounts[std::string(DomainName(domain)) + "|" + was + "|" + current + "|" + again];
                ++count;
                // logged at the first one and at every power of ten
                uint32 power = 1;
                while (power < count && power <= std::numeric_limits<uint32>::max() / 10)
                    power *= 10;
                if (power != count || slot.contradictions.size() >= MAX_ACKS)
                    return;
                slot.contradictions.push_back(
                    Contradiction{DomainName(domain), was, current, again, slot.fact, refused, count});
            }

            // [lead320] the arbiter of one domain. `key` the command (the same key: the same command asserted
            // again), `rule` the rule that asserts it, `precedence` / `currentPrecedence` the rules' ranks, `clear` a
            // rule ending its own command. False: refused, the current command stays
            bool Arbitrate(Slot& slot, Domain domain, std::string const& key, std::string const& rule,
                           int32 precedence, int32 currentPrecedence, bool clear)
            {
                Track& t = slot.tracks[domain];
                if (rule == t.startRule)
                    t.startAssertGen = slot.gen;  // the incumbent's rule asserts this tick
                if (key == t.current)
                {
                    t.assertGen = slot.gen;
                    return true;
                }
                // between equal ranks the incumbent holds while its own rule asserts it this tick
                if (!clear && !t.current.empty() && t.current == t.start && t.startAssertGen == slot.gen &&
                    rule != t.startRule && precedence <= currentPrecedence)
                {
                    NoteContradiction(slot, domain, t.startRule, t.current, key, true);
                    return false;
                }
                // a reversal within one fact: the command before the current again while the current still stands
                // (a higher current that ended gives way to the lower one again: no contradiction)
                if (currentPrecedence <= precedence)
                {
                    if (!t.previous.empty() && key == t.previous && t.previousFact == slot.fact &&
                        t.currentFact == slot.fact && t.assertGen + 1 >= slot.gen)
                        NoteContradiction(slot, domain, t.previous, t.current, key, false);
                    else if (!t.current.empty() && t.assertGen == slot.gen && rule != t.currentRule)
                        NoteContradiction(slot, domain, t.currentRule, t.current, key, false);  // two rules, one tick
                }
                t.previous = t.current;
                t.previousFact = t.currentFact;
                t.current = key;
                t.currentRule = rule;
                t.currentFact = slot.fact;
                t.assertGen = slot.gen;
                return true;
            }

            // [lead320] the leader no longer asserts the command (EndTick): it ends, no rule decided it
            void Dropped(Slot& slot, Domain domain)
            {
                Track& t = slot.tracks[domain];
                t.current.clear();
                t.currentRule.clear();
            }

            std::string GuidKey(ObjectGuid guid) { return guid.IsEmpty() ? std::string() : guid.ToString(); }

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

        bool SetMove(ObjectGuid member, MoveEvent move, Position const& spot, float tolerance, std::string const& why,
                     MoveMode mode)
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
            int32 const precedence = MovePrecedence(why);
            int32 const currentPrecedence = e.move != MoveEvent::None ? MovePrecedence(e.moveWhy) : -1;
            if (e.move != MoveEvent::None && e.moveMs + 1 >= slot.gen && currentPrecedence > precedence)
            {
                // [lead320] a reversal the precedence refuses is a contradiction too
                Track const& t = slot.tracks[DOMAIN_MOVE];
                if (why == t.previous && t.previousFact == slot.fact && t.currentFact == slot.fact)
                    NoteContradiction(slot, DOMAIN_MOVE, t.previous, t.current, why, true);
                return false;
            }
            // [lead55] user: «пожалей мой CPU» - b100: Rtdruaa 5 073 commands (813 in one second). The same command
            // is no new command: a move changes only when its point leaves the current one's arrival tolerance
            // (the walk's own, at least the old 1 yd) or its purpose changes; an unchanged one keeps its point
            float const keep = std::max(std::max(e.tolerance, tolerance), 1.0f);
            bool const changed =
                e.move != move || e.moveWhy != why || e.moveMode != mode || e.spot.GetExactDist(spot) > keep;
            // [lead69] the arbiter emits no command identical to the one before the current within the next tick
            // (member, purpose, point within tolerance): command_dupe_blocked - should be 0
            // [lead320] ... except the tick's incumbent asserted again after an equal rule replaced it this tick
            Track const& moveTrack = slot.tracks[DOMAIN_MOVE];
            bool const incumbentBack = !moveTrack.start.empty() && moveTrack.current != moveTrack.start &&
                                       MoveRule(why) == moveTrack.startRule;
            if (changed && !incumbentBack && why == slot.prevMoveWhy && slot.prevMoveSpot.GetExactDist(spot) <= keep &&
                slot.moveSetGen + 1 >= slot.gen)
            {
                ++slot.dupesBlocked;
                e.moveMs = slot.gen;
                return false;
            }
            // [lead320] the domain's arbiter (equal ranks: the incumbent holds while its rule asserts it)
            if (!Arbitrate(slot, DOMAIN_MOVE, move == MoveEvent::None ? std::string() : why, MoveRule(why),
                           precedence, currentPrecedence, move == MoveEvent::None))
                return false;
            e.move = move;
            if (changed)
            {
                slot.prevMoveWhy = e.moveWhy;
                slot.prevMoveSpot = e.spot;
                slot.moveSetGen = slot.gen;
                e.spot = spot;
                e.tolerance = tolerance;
                e.moveWhy = why;
                e.moveMode = mode;
                e.moveReachOf = ObjectGuid::Empty;  // [lead1070] a new move has its own purpose
                e.moveReachRange = 0.0f;
            }
            e.moveMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetMoveReach(ObjectGuid member, ObjectGuid unit, float range)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const it = eventSlots.find(member);
            if (it == eventSlots.end() || it->second.events.move != MoveEvent::MoveTo)
                return false;
            MemberEvents& e = it->second.events;
            bool const changed = e.moveReachOf != unit || e.moveReachRange != range;
            e.moveReachOf = unit;
            e.moveReachRange = range;
            return changed;
        }

        bool SetHealTarget(ObjectGuid member, ObjectGuid target, bool holds)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            std::string const key = GuidKey(target);
            if (!Arbitrate(slot, DOMAIN_HEAL_TARGET, key, key, 0, 0, key.empty()))
                return false;
            bool const changed = e.healTarget != target || e.healHolds != holds;  // [lead1263]
            e.healTarget = target;
            e.healHolds = holds;
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
            std::string const key =
                attack == AttackEvent::None ? std::string() : std::to_string(uint32(attack)) + ":" + GuidKey(target);
            if (!Arbitrate(slot, DOMAIN_ATTACK, key, key, 0, 0, key.empty()))
                return false;
            bool const changed = e.attack != attack || e.target != target;
            e.attack = attack;
            e.target = target;
            e.killOrder = killOrder;
            e.attackMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetIntensity(ObjectGuid member, Intensity intensity, std::vector<std::string> const& burnAllowed)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            std::string key;
            if (intensity != Intensity::Normal)
            {
                key = std::to_string(uint32(intensity));
                for (std::string const& allowed : burnAllowed)
                    key += ":" + allowed;
            }
            if (!Arbitrate(slot, DOMAIN_INTENSITY, key, key, 0, 0, key.empty()))
                return false;
            bool const changed = e.intensity != intensity || e.burnAllowed != burnAllowed;
            e.intensity = intensity;
            e.burnAllowed = burnAllowed;
            e.intensityMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        // the AoE rule rides on the Attack field's freshness (set together with it)
        bool SetAoe(ObjectGuid member, AoeRule aoe, Position const& center, float radius,
                    std::vector<ObjectGuid> const& spared)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            // [lead67] a new AoE command only when its rule changes; the centre follows the target (no re-issue)
            bool const changed = e.aoe != aoe || e.aoeSpared != spared;  // [lead415] the spared mobs too
            e.aoe = aoe;
            e.aoeCenter = center;
            e.aoeRadius = radius;
            e.aoeSpared = spared;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetCc(ObjectGuid member, ObjectGuid mob)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            std::string const key = GuidKey(mob);
            if (!Arbitrate(slot, DOMAIN_CC, key, key, 0, 0, key.empty()))
                return false;
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
            std::string const key = GuidKey(caster);
            if (!Arbitrate(slot, DOMAIN_INTERRUPT, key, key, 0, 0, key.empty()))
                return false;
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
            std::string const key = GuidKey(mob);
            if (!Arbitrate(slot, DOMAIN_FLEE_STOP, key, key, 0, 0, key.empty()))
                return false;
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
            std::string const key = target.IsEmpty() ? std::string() : std::to_string(spell) + ":" + GuidKey(target);
            if (!Arbitrate(slot, DOMAIN_THREAT, key, key, 0, 0, key.empty()))
                return false;
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
            std::string const key = untilMs ? why : std::string();  // the foreseen hit, not its time
            if (!Arbitrate(slot, DOMAIN_PRE_SAVE, key, key, 0, 0, key.empty()))
                return false;
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
            std::string const key = loot ? "loot" : "";
            if (!Arbitrate(slot, DOMAIN_LOOT, key, key, 0, 0, key.empty()))
                return false;
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
            std::string const key = GuidKey(mob);
            if (!Arbitrate(slot, DOMAIN_DONT_ATTACK, key, key, 0, 0, key.empty()))
                return false;
            bool const changed = e.dontAttack != mob;
            e.dontAttack = mob;
            e.dontAttackMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetExternal(ObjectGuid member, uint32 spell, ObjectGuid target, int32 precedence, bool unattackedOnly,
                         bool selfDefense)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            // [lead320] a lower order never replaces a fight save asserted this tick or the last
            // [lead1371] ... and no lower rule's clear ends the self-defence order asserted this tick or the last
            if ((spell || slot.externalPrecedence >= EXTERNAL_PRECEDENCE_SELF_DEFENSE) && e.externalSpell &&
                e.externalMs + 1 >= slot.gen && slot.externalPrecedence > precedence)
                return false;
            std::string const key = spell ? std::to_string(spell) + ":" + GuidKey(target) : std::string();
            if (!Arbitrate(slot, DOMAIN_EXTERNAL, key, key, precedence, e.externalSpell ? slot.externalPrecedence : -1,
                           key.empty()))
                return false;
            slot.externalPrecedence = spell ? precedence : 0;
            bool const changed =
                e.externalSpell != spell || e.externalTarget != target;
            e.externalSpell = spell;
            e.externalTarget = target;
            e.externalUnattacked = spell && unattackedOnly;  // [lead1320]
            e.externalSelfDefense = spell && selfDefense;     // [lead1371]
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
            std::string const key = ready == ReadyEvent::None ? std::string() : std::to_string(uint32(ready));
            if (!Arbitrate(slot, DOMAIN_READY, key, key, 0, 0, key.empty()))
                return false;
            bool const changed = e.ready != ready;
            e.ready = ready;
            e.readyMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetDispel(ObjectGuid member, uint32 spell, ObjectGuid target)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            std::string const key = spell ? std::to_string(spell) + ":" + GuidKey(target) : std::string();
            if (!Arbitrate(slot, DOMAIN_DISPEL, key, key, 0, 0, key.empty()))
                return false;
            bool const changed = e.dispelSpell != spell || e.dispelTarget != target;
            e.dispelSpell = spell;
            e.dispelTarget = spell ? target : ObjectGuid::Empty;
            e.dispelMs = slot.gen;
            if (changed)
                ++e.seq;
            return changed;
        }

        bool SetConsume(ObjectGuid member, uint32 item, ObjectGuid target, ObjectGuid object, std::string const& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            std::string const key = item ? std::to_string(item) + ":" + GuidKey(target)
                                         : (object.IsEmpty() ? std::string() : "object:" + GuidKey(object));
            if (!Arbitrate(slot, DOMAIN_CONSUME, key, key, 0, 0, key.empty()))
                return false;
            bool const changed = e.consumeItem != item || e.consumeTarget != target || e.consumeObject != object;
            e.consumeItem = item;
            e.consumeTarget = item ? target : ObjectGuid::Empty;
            e.consumeObject = item ? ObjectGuid::Empty : object;
            e.consumeWhy = key.empty() ? std::string() : why;
            e.consumeMs = slot.gen;
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
            if (starts("hazard") || starts("out of a cleave cone"))  // [lead320] the cleave exit is a dodge too
                return 5;
            if (starts("aggro loss"))
                return 4;
            if (starts("heal reach") || starts("sight"))
                return 3;
            // [lead156] the melee chase is its combat spot; [lead271] the trap weave's walk replaces it for a GCD
            if (starts("combat spot") || starts("chase") || starts("trap weave"))
                return 2;
            if (starts("its spot for the pull") || starts("along with the tank: to range"))
                return 1;
            return 0;
        }

        void BeginTick(ObjectGuid member)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            ++slot.gen;
            for (Track& t : slot.tracks)  // [lead320] this tick's incumbents
            {
                t.start = t.current;
                t.startRule = t.currentRule;
                t.startAssertGen = 0;
            }
        }

        void SetFact(ObjectGuid member, uint32 fact)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            eventSlots[member].fact = fact;
        }

        bool TakeContradiction(ObjectGuid member, Contradiction& out)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end() || found->second.contradictions.empty())
                return false;
            out = std::move(found->second.contradictions.front());
            found->second.contradictions.pop_front();
            return true;
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
                Dropped(slot, DOMAIN_MOVE);
                e.move = MoveEvent::None;
                changed = true;
            }
            if ((e.attack != AttackEvent::None || e.aoe != AoeRule::Unset) && drop(e.attackMs))
            {
                Dropped(slot, DOMAIN_ATTACK);
                e.attack = AttackEvent::None;
                e.aoe = AoeRule::Unset;
                changed = true;
            }
            if (e.intensity != Intensity::Normal && drop(e.intensityMs))
            {
                Dropped(slot, DOMAIN_INTENSITY);
                e.intensity = Intensity::Normal;
                e.burnAllowed.clear();
                changed = true;
            }
            if (!e.cc.IsEmpty() && drop(e.ccMs))
            {
                Dropped(slot, DOMAIN_CC);
                e.cc.Clear();
                changed = true;
            }
            if (!e.interrupt.IsEmpty() && drop(e.interruptMs))
            {
                Dropped(slot, DOMAIN_INTERRUPT);
                e.interrupt.Clear();
                changed = true;
            }
            if (!e.fleeStop.IsEmpty() && drop(e.fleeStopMs))
            {
                Dropped(slot, DOMAIN_FLEE_STOP);
                e.fleeStop.Clear();
                changed = true;
            }
            if (!e.threatTarget.IsEmpty() && drop(e.threatMs))
            {
                Dropped(slot, DOMAIN_THREAT);
                e.threatSpell = 0;
                e.threatTarget.Clear();
                changed = true;
            }
            if (!e.healTarget.IsEmpty() && drop(e.healTargetMs))
            {
                Dropped(slot, DOMAIN_HEAL_TARGET);
                e.healTarget.Clear();
                changed = true;
            }
            if (!e.dontAttack.IsEmpty() && drop(e.dontAttackMs))
            {
                Dropped(slot, DOMAIN_DONT_ATTACK);
                e.dontAttack.Clear();
                changed = true;
            }
            if (e.loot && drop(e.lootMs))
            {
                Dropped(slot, DOMAIN_LOOT);
                e.loot = false;
                changed = true;
            }
            if (e.preSaveUntilMs && drop(e.preSaveMs))
            {
                Dropped(slot, DOMAIN_PRE_SAVE);
                e.preSaveUntilMs = 0;
                e.preSaveWhy.clear();
                changed = true;
            }
            if (e.externalSpell && drop(e.externalMs))
            {
                Dropped(slot, DOMAIN_EXTERNAL);
                e.externalSpell = 0;
                e.externalTarget.Clear();
                e.externalUnattacked = false;   // [lead1320]
                e.externalSelfDefense = false;  // [lead1371]
                changed = true;
            }
            if (e.ready != ReadyEvent::None && drop(e.readyMs))
            {
                Dropped(slot, DOMAIN_READY);
                e.ready = ReadyEvent::None;
                changed = true;
            }
            if (e.dispelSpell && drop(e.dispelMs))
            {
                Dropped(slot, DOMAIN_DISPEL);
                e.dispelSpell = 0;
                e.dispelTarget.Clear();
                changed = true;
            }
            if ((e.consumeItem || !e.consumeObject.IsEmpty()) && drop(e.consumeMs))
            {
                Dropped(slot, DOMAIN_CONSUME);
                e.consumeItem = 0;
                e.consumeTarget.Clear();
                e.consumeObject.Clear();
                e.consumeWhy.clear();
                changed = true;
            }
            if (changed)
                ++e.seq;
        }

        bool SetFollow(ObjectGuid member, ObjectGuid target, uint8 index, std::string const& why, MoveMode mode)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            MemberEvents& e = slot.events;
            // [lead320] the follow is a move: the same arbiter (another rule's higher live move holds; a rule's own
            // follow fallback is that rule)
            int32 const precedence = MovePrecedence(why);
            int32 const currentPrecedence = e.move != MoveEvent::None ? MovePrecedence(e.moveWhy) : -1;
            if (e.move != MoveEvent::None && e.moveMs + 1 >= slot.gen && currentPrecedence > precedence &&
                MoveRule(e.moveWhy) != MoveRule(why))
                return false;
            if (!Arbitrate(slot, DOMAIN_MOVE, why, MoveRule(why), precedence, currentPrecedence, false))
                return false;
            bool const changed =
                e.move != MoveEvent::Follow || e.followTarget != target || e.followIndex != index || e.moveMode != mode;
            e.move = MoveEvent::Follow;
            e.moveMode = mode;
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
                   !out.dontAttack.IsEmpty() || out.loot || out.preSaveUntilMs || out.dispelSpell || out.consumeItem ||
                   !out.consumeObject.IsEmpty();
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

        void NoteExternalFailed(ObjectGuid member, uint32 spell, std::string const& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            slot.externalFailed = spell;
            slot.externalFailedWhy = why;
        }

        void NoteMoveRefused(ObjectGuid member, MoveRefusal const& refusal)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            slot.moveRefusalNew = true;
            slot.moveRefused = true;
            slot.moveRefusal = refusal;
        }

        bool TakeMoveRefused(ObjectGuid member, MoveRefusal& out)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end() || !found->second.moveRefusalNew)
                return false;
            out = found->second.moveRefusal;
            found->second.moveRefusalNew = false;
            return true;
        }

        bool MoveRefused(ObjectGuid member, MoveRefusal& out)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end() || !found->second.moveRefused)
                return false;
            out = found->second.moveRefusal;
            return true;
        }

        void ClearMoveRefused(ObjectGuid member)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found != eventSlots.end())
                found->second.moveRefused = false;
        }

        bool TakeExternalFailed(ObjectGuid member, uint32 spell, std::string& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end() || !spell || found->second.externalFailed != spell)
                return false;
            why = found->second.externalFailedWhy;
            found->second.externalFailed = 0;
            found->second.externalFailedWhy.clear();
            return true;
        }

        void NoteDispelFailed(ObjectGuid member, uint32 spell, std::string const& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            slot.dispelFailed = spell;
            slot.dispelFailedWhy = why;
        }

        bool TakeDispelFailed(ObjectGuid member, uint32 spell, std::string& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end() || !spell || found->second.dispelFailed != spell)
                return false;
            why = found->second.dispelFailedWhy;
            found->second.dispelFailed = 0;
            found->second.dispelFailedWhy.clear();
            return true;
        }

        void NoteConsumeFailed(ObjectGuid member, uint32 what, std::string const& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            Slot& slot = eventSlots[member];
            slot.consumeFailed = what;
            slot.consumeFailedWhy = why;
        }

        bool TakeConsumeFailed(ObjectGuid member, uint32 what, std::string& why)
        {
            std::lock_guard<std::mutex> lock(eventLock);
            auto const found = eventSlots.find(member);
            if (found == eventSlots.end() || !what || found->second.consumeFailed != what)
                return false;
            why = found->second.consumeFailedWhy;
            found->second.consumeFailed = 0;
            found->second.consumeFailedWhy.clear();
            return true;
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
