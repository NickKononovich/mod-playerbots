/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADERORDERS_H
#define PLAYERBOTS_LEADERORDERS_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "SafeZone.h"  // [lead9] Leader::FightPlan / SafeZone (Ai/Base/Util)

#include <string>

class Player;
class Unit;

// [lead1] C:/WoW/LEADER_REFACTOR_PLAN.md steps 1-2: the pull order registry between the dungeon leader (the test
// harness, world thread) and the tank that executes the pull (its map thread).
//
// Step 1 (no behaviour change): the tank posts "shadow" orders. Its planner's commit (AggressiveTargetAction::Execute)
// opens an order, the pull actions report its states, the harness drains the reports into the decision log
// (TestHarness::DecisionLog, <runId>.decisions.jsonl) next to its own holds and body pulls, and counts the conflicts:
// two owners deciding about the same mob within DECISION_CONFLICT_MS, a leader waypoint given to the tank while its
// order is open.
//
// Step 2 (LEADER_ONE_TANK_MOVER): an open order is the one owner of the tank's movement. The leader gives the tank no
// waypoint while one is open (GiveWaypoint), and ownership comes back on events only: the tank's Aborted report, the
// leader seeing the order's fight end (Done), or the tank dead / gone. No clock hands it over. The one watchdog left
// (ORDER_ORPHAN_MS) is a bug report about a broken invariant ("order_orphan"), not a fallback that takes ownership.
//
// Only tanks the harness tracks (Track: a harness party's leader) post anything; every other bot's call is a no-op.
namespace Leader
{
    // [lead1] Step 1: the decision log and its conflict counts (logging only).
    constexpr bool LEADER_DECISION_LOG = true;
    // [lead2] Step 2: one owner of the tank's movement during a pull, handed over on order events only. false: the
    // old ROUTE_NO_WALK_IN_PULL heuristics (TankPullRunning, PULL_HOLD_STALL_MS / PULL_HOLD_RELEASE_MS) for the A/B.
    constexpr bool LEADER_ONE_TANK_MOVER = true;
    // [lead2] An engage order (the tank walks in / charges: no pull strategy) owns the tank's movement too. The old
    // heuristics never covered it; false narrows step 2 to the pull strategy's pulls (TankPullRunning's set) for triage.
    constexpr bool LEADER_ENGAGE_OWNS_MOVEMENT = true;

    // [lead7] Step 7 (C:/WoW/LEADER_REFACTOR_PLAN.md section 8): the careful pull planner as the leader's library
    // (PullPlanner in ChooseTargetActions.h). LEADER_PLANNER_IN_LEADER: the harness plans for its party's tank on the
    // world thread whenever the tank asks, publishes the planner's verdicts and issues the Go decisions; the tank only
    // executes them (no look of its own). LEADER_PLANNER_DIFF (used while IN_LEADER is off): the tank still decides,
    // and the planner instance looks in shadow at the same instant; disagreements become "plan_diff" decision lines
    // and the run's "plan_diff_summary" event. Groups the harness does not lead are never affected.
    // [lead3] true since steps 3-6, 8 and 10 (C:/WoW/LEADER_REFACTOR_PLAN.md section 9): the leader is the only
    // decider of its party's pulls. LEADER_PLANNER_DIFF stays for an A/B with IN_LEADER off; it is off by default
    // (it only runs while IN_LEADER is off, and doubles the planner's CPU there).
    constexpr bool LEADER_PLANNER_IN_LEADER = true;
    constexpr bool LEADER_PLANNER_DIFF = false;
    // [lead7] User decision 1: the leader's planner finds no pull at all -> "planner_no_option" bug event, once per
    // pack. [lead3] true: it stops the run when the pack is one the leader must clear (its whole-pack request: a
    // route blocker, a required way, a boss's area, an event pack); a hopeless side pack the route can leave is
    // logged and skipped by the leader (a route decision, not a refusal).
    constexpr bool LEADER_STOP_ON_NO_OPTION = true;

    // [lead3] Step 3: no harness body pull. Where the harness walked the group into a pack (RouteBlockerBodyPull,
    // Steer's PULL_WAIT_MS body / attack pull, the guard / boss formation / event body pulls, the give-way walk into
    // sight, the careful-refused skips) the leader asks its planner for the pack as a whole (Snapshot request:
    // allowWholePack, walk-in priced by the planner's own count) or, for a boss, issues a BossPull order. A pack that
    // still has no option is planner_no_option. false: the old harness body pulls (A/B; IN_LEADER must be on too).
    constexpr bool LEADER_NO_BODY_FALLBACK = true;
    // [lead4] Step 4: one patrol owner. The harness patrol hold / lane hold / step-back and the planner's patrol and
    // overlap waits end in one leader decision (LeaderPatrolTick): wait (a declared Wait with its forecast window) or
    // pull the patrol (an intercept request). No wait budget, no handover, no hold clocks.
    constexpr bool LEADER_ONE_PATROL_OWNER = true;
    // [lead5] Step 5: one target picker. The leader picks the look target (AggressiveTargetValue::PickFor with its
    // own route target, no RouteTarget registry), reads its own planner's verdicts (no CarefulPlanVerdict /
    // GuaranteeRequest / AoePull channel in Steer), and EncounterOrder::IsLocked is the one encounter-order rule.
    constexpr bool LEADER_ONE_PICKER = true;
    // [lead6] Step 6: one readiness judge. The leader's verdict (LeaderReadiness, published per tick) gates the
    // planner's look and replaces the RaidLeader ready check's pull hold for harness groups (user decision 3);
    // CheckStall reports instead of skipping while an order or a declared wait is open.
    constexpr bool LEADER_ONE_READINESS = true;
    // [lead8] Step 8: the pack is counted from the fight spot for every method (CollectPack's required fight
    // argument), and the leader's cleared spots are pull-back fight spot candidates.
    constexpr bool LEADER_FIGHT_SPOT_COUNT = true;
    // [lead10] Step 10: doors as planner inputs. The door rules stay the leader's (key / lever doors, the tight
    // gather with no cap, the re-open for a stranded member); the planner reads the leader's door cut (no
    // TestRunner::IsDoorCut), a plan whose stand / fight / gather walk crosses a closed door is not issued, and a key
    // pass opens only when its far side is priced within the whole-pack cap (no 60 s give-up).
    constexpr bool LEADER_DOOR_INPUTS = true;
    // A step is on only with the leader planning (IN_LEADER).
    constexpr bool Leads(bool step) { return LEADER_PLANNER_IN_LEADER && step; }

    // [lead3] The leader's whole-pack request (the old body pull's domain) lives this long without a refresh.
    constexpr uint32 LEADER_REQUEST_TTL_MS = 5000;
    // [lead3] A refusal is the request's answer only this long after the request began (the planner's 1 s plan
    // cache may still answer the look before it).
    constexpr uint32 LEADER_REQUEST_ANSWER_MS = 2500;
    // [lead3] "held for now" with the leader's readiness verdict ready this long: a planner_no_option report (the
    // hold no rest can change; the old [held_for_now_cap] turned it into a body pull).
    constexpr uint32 LEADER_HELD_READY_MAX_MS = 75000;
    // [lead10] A pull whose stand / fight / gather walk crosses a closed door: not issued; this many such looks of
    // one pack in a row with the group ready are planner_no_option.
    constexpr uint32 LEADER_DOOR_INVALID_LOOKS = 10;
    // [lead6] A published readiness verdict is fresh this long.
    constexpr uint32 LEADER_READY_FRESH_MS = 3000;
    // [lead7] The leader plans on an ask at most this old; the tank takes an issued decision at most this old.
    constexpr uint32 LEADER_ASK_FRESH_MS = 1000;
    // [lead4c] build 54 (every run stalled at 0 bosses): the tank's AI ticks every 1-3 s out of combat (Playerbots.
    // DynamicReactDelay: 10-30 x ReactDelay), so a decision issued after one tick was always older than 1 s at the
    // next and was dropped - 66 pull_orders, no pull. A decision lives LEADER_ISSUE_FRESH_MS and is consumed only by
    // the Execute that runs it (TakeLeaderDecision peeks), so a tick whose multipliers skip the action loses nothing.
    constexpr uint32 LEADER_ISSUE_FRESH_MS = 5000;
    // [lead7] The same disagreement (or the same issued order) is logged again after this long.
    constexpr uint32 PLAN_DIFF_LOG_MS = 10000;
    constexpr uint32 PULL_ORDER_LOG_MS = 5000;

    // [lead9] Step 9: the fight's safe zone (SafeZone.h). In a fight of a harness group every bot's move (MoveTo,
    // and Follow / ChaseTo, which used to bypass MoveTo by MoveFollow / MoveChase) ends inside the zone or the bot
    // stays; avoid aoe / flee / ranged position pick their spots inside it; a tank gap closer never leaves it. It
    // replaces FightHold::RADIUS / MELEE_SLACK as the authority, ESCAPE_ROOM_FIGHT_SPOT_SLACK, [b43_move]
    // fight_hold_edge_below and the [b47_idle] stay_chase for these groups. Between pulls a follow keeps off MoveFollow
    // (the idle-mob path guard judges it) and the deadlock breaker no longer walks a member into idle reach. false:
    // today's behaviour (FightHold, the old guards) for the A/B. Groups the harness does not lead never change.
    constexpr bool LEADER_SAFE_ZONE = true;
    // [lead9] ... and between pulls the harness tank's own walks (no order open: no leader waypoint, no pull, no
    // planner commit) are judged by the idle-mob path guard as a member's are (the "who: none" starts by the tank's
    // reach melee / tank face). false narrows step 9 to fights and members.
    constexpr bool LEADER_SAFE_ZONE_IDLE_TANK = true;

    // Two owners deciding about one mob within this long count as one conflict (plan step 1 "Verify").
    constexpr uint32 DECISION_CONFLICT_MS = 2000;
    // [lead2] An open order, the tank out of combat and within ORDER_ORPHAN_RANGE of one spot, no state change and no
    // group preparation for this long: logged order_orphan (a lost report), closed, and the leader plans again.
    constexpr uint32 ORDER_ORPHAN_MS = 20000;
    constexpr float ORDER_ORPHAN_RANGE = 5.0f;
    // Reports kept per tank until the harness drains them; the oldest go first.
    constexpr size_t MAX_QUEUED_REPORTS = 64;

    enum class PullMethod : uint8
    {
        Unknown,
        Engage,    // walk in / charge
        Body,      // proximity
        PullBack,  // ranged, back to the kill spot
        LosPull,   // ranged, round a corner
        One,       // lone mob, single-target ranged
        AoeStack,
        PatrolIntercept,
        BossPull
    };

    enum class OrderState : uint8
    {
        Issued,
        Walking,
        AtStand,
        Tagged,
        WalkingBack,
        Holding,
        Fighting,
        Done,
        Aborted
    };

    char const* MethodName(PullMethod method);
    char const* StateName(OrderState state);
    bool IsTerminal(OrderState state);

    // The order the tank executes (today the tank's own commit, a "shadow" order; later the leader issues it).
    struct PullOrder
    {
        uint32 id = 0;  // monotonic; every report names it
        ObjectGuid tank;
        ObjectGuid target;  // the mob that is tagged
        std::string targetName;
        uint32 targetEntry = 0;
        PullMethod method = PullMethod::Unknown;
        OrderState state = OrderState::Issued;
        uint32 expected = 0;  // mobs the plan expects
        float cost = 0.0f;
        bool singleOnly = false;
        std::string origin;  // what opened it: the planner's commit, the pull start, the cast
        // [lead11] the leader issued it (PullPlanner::Issue): every state the tank reports of it - its steps, its
        // aborts by the order's own abort conditions - is the execution's report, no decision of the tank's
        bool leaderIssued = false;
        std::string why;     // the last state change's reason
        uint32 openedMs = 0;
        uint32 changedMs = 0;  // the last state change
        Position changedAt;    // the tank at the last state change
        // [lead9] The order's fight plan (plan 2.2): fight spot, safe zone, role spots. fight.zone.valid false: none
        // published, the bots build the default zone round FightHold's anchor (FightZone::GetPlan).
        FightPlan fight;
    };

    // One line for the decision log: a state change of the tank's order, or a decision-only note (`what` set).
    struct OrderReport
    {
        uint32 orderId = 0;  // 0: no order (a note)
        ObjectGuid tank;
        ObjectGuid target;
        std::string targetName;
        uint32 targetEntry = 0;
        OrderState state = OrderState::Issued;
        PullMethod method = PullMethod::Unknown;
        std::string what;  // "order_report", or the note's kind ("commit_failed", "approach_walk", "chain_pull")
        std::string why;
        uint32 mobs = 0;
        float cost = 0.0f;
        bool decides = false;  // the tank decided here (a commit, an abort, a walk of its own): counted for conflicts
        Position at;
        uint32 ms = 0;
    };

    namespace Orders
    {
        // World thread: the harness party's leader posts from now on / no longer (its open order and queue dropped).
        void Track(ObjectGuid tank);
        void Untrack(ObjectGuid tank);

        // Executor side (the tank's map thread). `target` nullptr on a terminal state: whatever order is open.
        // Issued / Walking / Tagged open an order when none is open (a different target supersedes the open one);
        // the other states only advance the open order of the same target. A repeat of the current state is no
        // report.
        // [lead7] `leaderIssued`: the tank executes the leader's decision (PullPlanner::Issue): no decision of its own.
        void Report(Player* tank, Unit* target, OrderState state, PullMethod method, std::string const& why,
                    uint32 mobs = 0, float cost = 0.0f, bool single = false, bool leaderIssued = false);
        // A decision of the tank that is no order state (logged, counted, no order change); [lead7] with
        // `leaderIssued` the line is a report of the leader's decision being executed.
        void Note(Player* tank, Unit* target, std::string const& what, std::string const& why,
                  bool leaderIssued = false);
        // [lead7] The harness leads this tank (Track ... Untrack).
        bool IsTracked(ObjectGuid tank);

        // Leader side (world thread).
        bool GetOpen(ObjectGuid tank, PullOrder& order);
        bool IsOpen(ObjectGuid tank);
        // A state the leader observes (Fighting); false: no order open or already in it.
        bool Advance(ObjectGuid tank, OrderState state, std::string const& why, Position const& at);
        // Done / Aborted by the leader (the fight ended, the tank died, order_orphan); false: none open.
        bool Close(ObjectGuid tank, OrderState state, std::string const& why, Position const& at, PullOrder* closed);
        // The oldest queued report of the tank; false when none.
        bool TakeReport(ObjectGuid tank, OrderReport& report);
    }

    // [lead6] Step 6: the leader's one readiness verdict for its group, published by the harness every tick (world
    // thread) and read by the bot layers that used to judge it themselves (RaidLeader's ready-check pull hold).
    // Keyed by the group's guid; a verdict older than LEADER_READY_FRESH_MS is no verdict (the group is not led).
    namespace Readiness
    {
        void Publish(ObjectGuid group, bool ready, std::string const& why);
        void Clear(ObjectGuid group);
        // false: no fresh verdict (not a harness group, or the run ended)
        bool Get(ObjectGuid group, bool& ready, std::string* why = nullptr);
    }
}

#endif
