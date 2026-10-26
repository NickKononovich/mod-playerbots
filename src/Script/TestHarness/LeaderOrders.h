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
        std::string why;     // the last state change's reason
        uint32 openedMs = 0;
        uint32 changedMs = 0;  // the last state change
        Position changedAt;    // the tank at the last state change
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
        void Report(Player* tank, Unit* target, OrderState state, PullMethod method, std::string const& why,
                    uint32 mobs = 0, float cost = 0.0f, bool single = false);
        // A decision of the tank that is no order state (logged, counted, no order change).
        void Note(Player* tank, Unit* target, std::string const& what, std::string const& why);

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
}

#endif
