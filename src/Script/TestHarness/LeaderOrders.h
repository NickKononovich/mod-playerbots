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
#include <vector>  // [lead15]

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
    // [speech] the leader says what it thinks: every significant decision becomes one chat line to the GM watching
    // the run (system message, rate-limited per topic), so a user can see where the group waits and why.
    constexpr bool LEADER_SPEECH = true;
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

    // [lead15] Step 15 (user 2026-10-26: «рейд-лидер должен давать ивенты, а игроки решать, что с ними делать»): the
    // leader issues events to EVERY member (Events below), the members decide only how (rotation, own threat).
    // Each switch rolls its part back to the member's own rule. All only for a group the leader leads (LedGroup).
    constexpr bool LEADER_MEMBER_EVENTS = true;   // the channel itself (off: no event is issued or read)
    constexpr bool LEADER_ASSIGNS_SPOTS = true;   // spots before a pull, the pull only once everyone stands on his
    constexpr bool LEADER_ONE_REST_RULE = true;   // sit = the group waits; one line for sit / ready; min deficit
    constexpr bool LEADER_ATTACK_EVENTS = true;   // the pull's first target to every damage dealer at once
    constexpr bool LEADER_FAST_REACT = true;      // out of combat in a led instance group: ~300 ms ticks, not 1-3 s
    constexpr bool LEADER_OWNS_DODGE = true;      // the leader moves a member out of a cleave (the member's own
                                                  // avoid / follow / ranged moves yield to the leader's MoveTo)
    // [lead15] user correction 2026-10-26 («Бот сам не должен думать... Все команды выдаёт рейд-лидер»): in a group
    // the leader leads, a member (not the tank) is a pure EXECUTOR of the leader's current command (ExecutorMember).
    // Attack(X): the rotation on X with its own threat control; Heal(group): the heal rotation; MoveTo / StandAt:
    // walk there and stand; Rest: eat / drink to full, then "ready"; Prepare / Ready?: own buffs, forms, imbues;
    // Hold / StopDps: nothing. With no command and out of combat the strategy engine does not run at all.
    // Each switch below turns one of the member's own decisions off (false: the member's own rule again).
    constexpr bool LEADER_MEMBER_EXECUTOR = true;       // master switch of the executor model
    constexpr bool LEADER_IDLE_WITHOUT_COMMAND = true;  // no command, out of combat: the engine is skipped
    constexpr bool LEADER_NO_OWN_FOLLOW = true;         // "follow" off: the leader's MoveTo walks the member along
    constexpr bool LEADER_NO_OWN_ASSIST = true;         // "dps assist" / "dps aoe" / "attack anything" off: Attack(X)
    constexpr bool LEADER_NO_OWN_REST = true;           // "drink" / "food" only under the leader's Rest
    constexpr bool LEADER_NO_OWN_POSITIONS = true;      // ranged / melee / formation / flee moves off
    constexpr bool LEADER_NO_OWN_CLEAVE_DODGE = true;   // "dungeon avoid frontal cleave" off: the leader's MoveTo
    constexpr bool LEADER_NO_OWN_LOS_GATHER = true;     // "move to los pull spot" off: the leader's StandAt
    // [lead16] the leader assigns the in-fight crowd control (MemberEvents::cc: which mob, which member; the member
    // casts it, "leader cc"), so the members' own CC picks are off: "auto cc mark", "cast auto cc", "emergency cc"
    // and every "<spell> on cc". "pre pull cc" (it answers the tank's published pre-pull plan: Sap, a trap) and
    // "control charmed member" stay.
    constexpr bool LEADER_NO_OWN_CC = true;
    constexpr bool LEADER_CC_ASSIGN = true;  // the leader's CC assignments (off: none)
    // [lead16] Hold while the leader holds the group (a patrol wait, a harness "stay"); StopDps when no target of the
    // fight takes damage (immune: a boss phase)
    constexpr bool LEADER_HOLD_EVENTS = true;
    // [lead16] the tank executes too: in a fight its target is the leader's Attack (a mob loose on a member first,
    // else its own victim, else the kill order's head), its own target picks yield to it ("tank assist", "attack
    // anything", "dps assist", "tank defend on attacked group member", "reach healer attacker")
    constexpr bool LEADER_TANK_EXECUTES = true;
    // [lead16] after a fight the leader tells one member to loot (LEADER_LOOT_MS); nobody else loots
    constexpr bool LEADER_LOOT_EVENTS = true;
    // [lead16] rot2: a DK tank's Army of the Dead before a boss pull runs only under a fresh Prepare: the leader
    // issues it to the tank and holds the pull LEADER_BOSS_PREPARE_MS for the channel
    constexpr bool LEADER_BOSS_PREPARE = true;
    // [lead16] PreSave(member, until): the leader foresees a big hit (a heavy cast on the member or the tank, an
    // enraged boss on the tank, the tank low under a boss) and tells the member to use its personal save now; the
    // bot's own saves are for critical health only (the rotation side)
    constexpr bool LEADER_PRESAVE = true;
    // [lead17] ONE decider (user 2026-10-26: «Я ж просил убрать два решающих – оставить только один решающий»;
    // C:/WoW/LEADER_OWNERS_INVENTORY.md): for a led member the engine runs a WHITELIST - the leader's executors, the
    // harness's own waypoint for it ("test runner move", the leader's walk across a link / out of water), the
    // rotation's spells, emergency personal saves; every other movement and every attack on a target the leader did
    // not name is off (stuck recovery, pre pull cc walks, prepull sight spots, reach, flee, avoid, dungeon moves)
    constexpr bool LEADER_EXECUTOR_WHITELIST = true;
    // [lead17] one readiness / gather owner: for a led party the harness's far wait, compact pause, ping-pong meet and
    // the 40 yd readiness check read the leader's own trailing set (TickLeaderEvents: a member without a leader spot
    // farther than LEADER_TRAIL_YD from the tank); a member on its leader spot is never "far"
    constexpr bool LEADER_ONE_GATHER_OWNER = true;
    constexpr float LEADER_TRAIL_YD = 35.0f;
    constexpr float LEADER_TRAIL_BACK_YD = 20.0f;
    // [lead17] the tank's own picks off: "chain pull" (the next pack is the planner's), "move fight off hazard" (the
    // leader's hazard MoveTo moves the tank too)
    constexpr bool LEADER_TANK_OWN_PICKS_OFF = true;
    // [lead17] a pull spot not reached in LEADER_SPOT_STUCK_MS is re-assigned (a new spot) up to this often, then
    // the member is reported stuck and no longer waited for
    constexpr uint32 LEADER_SPOT_REASSIGNS = 2;
    // [lead18] slabs-h 20261026-190629: the leader re-planned every look (Familiar -> Warlock -> Guardhound ->
    // Deathsworn -> Familiar, 10 pull orders in 130 s), each reassigning every spot, so the spot gate never closed.
    // COMMITMENT: a Go decision becomes the leader's commitment; its spot gate, its patrol window and its order run
    // until the order ends or the plan is truly broken (the mob dead / moved away / in a fight, a patrol entering the
    // pull area, too old) - no re-plan meanwhile, no patrol wait by forecast
    constexpr bool LEADER_COMMIT_PULL = true;
    // [lead24] user «удали все таймеры, которые форсят»: for a led party no decision is taken because time passed;
    // each former forcing timer only logs waited_long (who waits, for what, how long);
    // C:/WoW/LEADER_OWNERS_INVENTORY.md «Forcing timers removed». The harness's 4-minute no-progress run stop stays (the test's failure detector).
    constexpr bool LEADER_NO_FORCING_TIMERS = true;
    // [lead24d] before the pull every member stays where it stands (no pull-spot search; the healer only when it
    // cannot reach / see the fight spot, and after the tank holds the mobs)
    constexpr bool LEADER_PREPULL_STAY = true;
    constexpr uint32 LEADER_COMMIT_MAX_MS = 90000;
    constexpr uint32 LEADER_COMMIT_TAKE_MS = 10000;   // an issued commitment the tank did not take
    constexpr float LEADER_COMMIT_MOVED_YD = 12.0f;
    constexpr float LEADER_PATROL_AREA_MARGIN = 5.0f;  // a patrol this far inside its reach of the pull area breaks it
    // [lead18] user: «подождал бы, покуда патруль отбежит, и запулил бы самых левых» - in a room of linked packs the
    // leader pulls from the edge (the pack with the fewest other packs beside it, nearest the group) inward
    constexpr bool LEADER_EDGE_FIRST = true;
    // [lead18] combat spots are anchored to the fight spot: kept while the member reaches every target from it (its
    // spell range, sight) and it stays safe; a target change alone never moves anyone
    constexpr bool LEADER_ANCHORED_SPOTS = true;
    constexpr float LEADER_ANCHOR_MOVE_YD = 8.0f;
    // [lead18] user «ровно 0»: a led member's engine evaluates no trigger and no default action whose every action the
    // executor model vetoes (Engine::ProcessTriggers / PushDefaultActions, RaidLeader::GateVetoes); any veto still
    // reached (a trigger fired, a value computed) counts as vetoed_eval - a blocker, the goal is 0
    constexpr bool LEADER_EXECUTOR_TRIGGER_GATE = true;
    // [lead18] a led healer only heals (and its personal saves): no offensive cast, no dps target evaluation
    constexpr bool LEADER_HEALER_HEALS_ONLY = true;
    // [lead18] every spot the leader issues and the walk to it stay out of every unpulled mob's reach (the planner's
    // IdleMobReachesSpot, plus LEADER_SPOT_AGGRO_MARGIN); none valid: the member stays (spot_rejected_aggro)
    constexpr bool LEADER_SPOT_AGGRO_GUARD = true;
    // [lead20] slabs-h 20261026-193920: every pull spot of the shaman was rejected at 24.6-29.5 yd from a Cabal
    // Deathsworn - the planner's IdleMobReachesSpot already is the real (level-adjusted) reach; the margin only
    // covers the walk's sampling now
    constexpr float LEADER_SPOT_AGGRO_MARGIN = 2.0f;
    // [lead20] a spot gate with no progress this long: spot_gate_stuck, everyone where they stand counts placed
    constexpr uint32 LEADER_SPOT_GATE_STUCK_MS = 20000;
    // [lead18] user «всегда безопасно стоять там, где мы уже были»: the group's walked ground is preferred for every
    // spot (cleared first, new ground only when no cleared spot fits)
    constexpr bool LEADER_PREFER_CLEARED = true;
    // [lead18] the healer keeps the tank: a healer spot is within LEADER_HEALER_TANK_YD of the tank's current position
    // and in its sight; the healer counts as having lost the tank beyond LEADER_HEALER_LOST_YD or out of sight
    constexpr float LEADER_HEALER_TANK_YD = 30.0f;
    constexpr float LEADER_HEALER_LOST_YD = 36.0f;
    constexpr float LEADER_PRESAVE_HIT_PCT = 35.0f;   // a cast whose damage is this share of the victim's max health
    constexpr float LEADER_PRESAVE_TANK_PCT = 40.0f;  // the tank under a boss below this health
    constexpr uint32 LEADER_BOSS_PREPARE_MS = 4500;
    constexpr uint32 LEADER_LOOT_MS = 15000;
    constexpr bool LEADER_NO_OWN_BUFF_TIMING = true;    // out of combat buffs only under Prepare / Ready? / Rest
    // step 2 / 3 of the correction, NOT built yet: the leader does not detect these hazards / tactics itself, so the
    // member's own rules stay until it does (true would leave the members standing in them)
    // [lead16] the leader dodges every ground hazard itself now (TickLeaderEvents: dynamic objects, traps, trigger
    // NPCs, area casts, DungeonHazard circles, lava / drop-offs as a spot filter): the members' own dodges are off
    constexpr bool LEADER_NO_OWN_AOE_DODGE = true;        // "avoid aoe", "avoid dungeon hazard"
    constexpr bool LEADER_HAZARD_SPOTS = true;            // the leader's hazard MoveTo (off: no leader dodge)
    // [lead16] combat positions: the leader's StandAt for every member in the fight (ranged / healer at a distance in
    // sight of the target, melee behind it, out of cleaves and hazards, in heal reach); the members' own range moves
    // ("reach melee" / "reach spell") yield to a leader spot
    constexpr bool LEADER_COMBAT_SPOTS = true;
    constexpr bool LEADER_NO_OWN_DUNGEON_MOVES = false;   // the dungeon tactics' own moves
    // [lead15] the healer reaches EVERY member (user 2026-10-26): every spot the leader issues keeps each member
    // within LEADER_HEAL_REACH_YD (40 yd heal range minus a margin) of the healer and in its sight
    constexpr bool LEADER_HEAL_REACH = true;
    // [lead15] the split's enforcement check (user: «рейд-лидер решает, что, когда и где; бот сам решает только как
    // ДД-шить, танковать, хилить»): an executed member action that moves, changes the target, sits to rest, casts an
    // external / CC / burn cooldown or a buff on another without a matching leader command is an own_decision line
    // (who member:<name>) and counts in the run's conflicts; the goal is 0
    constexpr bool LEADER_OWN_DECISION_CHECK = true;
    constexpr float LEADER_HEAL_REACH_YD = 35.0f;
    // [lead15] an event field lives this long without the leader refreshing it (a stopped leader: the members'
    // own rules again); the dodge cadence; a spot reached within this; a member not nearer for this long is stuck
    constexpr uint32 LEADER_EVENT_TTL_MS = 1500;
    constexpr uint32 LEADER_EVENT_TICK_MS = 200;
    constexpr float LEADER_SPOT_TOLERANCE = 3.0f;
    constexpr uint32 LEADER_SPOT_STUCK_MS = 15000;
    // [lead15] the one rest line (audit fix 1): before a real boss sit below 90 % (= the boss ready gate), and never
    // sit for less than this deficit to the line
    constexpr float LEADER_BOSS_REST_PCT = 90.0f;
    constexpr float LEADER_REST_MIN_DEFICIT_PCT = 10.0f;
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

    // [lead15] The member's group is led by the dungeon leader (a fresh readiness verdict for it).
    bool LedGroup(Player* member);
    // [lead15] LEADER_MEMBER_EXECUTOR: a member of a led group inside a dungeon, not the tank (the tank executes its
    // PullOrder and walks the route itself): it executes the leader's commands only.
    bool ExecutorMember(Player* member);
    // [lead15] any bot of a led 5-man dungeon group, the tank included (the command getters, LeaderCommand.h)
    bool LedBot(Player* bot);

    // [lead15] The leader -> member event channel. One record per member, written by the leader (world thread) every
    // LEADER_EVENT_TICK_MS while it holds, read by the member on its own tick (map thread): behind a lock. A field
    // older than LEADER_EVENT_TTL_MS is no event. The member reports what it executed (Ack): drained by the leader
    // into the decision log (member_ack, decides false).
    enum class MoveEvent : uint8
    {
        None,
        MoveTo,   // go to `spot` now (a dodge, a reposition): above the member's own moves
        StandAt,  // stand at `spot` (the pull's assigned spot) until told otherwise
        Hold      // do not move
    };
    enum class AttackEvent : uint8
    {
        None,
        Attack,      // attack `target` (the kill order after it)
        DontAttack,  // no new target
        StopDps,     // stop damage now
        Heal         // [lead15] the healer: the heal rotation on the whole group (whom and how: its own rotation)
    };
    enum class ReadyEvent : uint8
    {
        None,
        ReadyCheck,  // "Ready?": the leader is about to pull
        Prepare,     // [lead15] the group stands: own buffs, forms, imbues, a resurrection
        Rest         // the group rests: sit if you need it, nobody walks off
    };
    // [lead15] the responsibility split (user 2026-10-26): what needs the whole group's view is the leader's,
    // only per-GCD self-state stays with the bot (rotation, the AoE spell when AoE is allowed, own threat, personal
    // saves, the healer's whom / how). The rotation reads these through Ai/Base/Util/LeaderCommand.h.
    enum class AoeRule : uint8
    {
        Unset,          // no leader rule (the bot's own AoE choice, as before)
        Forbidden,      // a crowd-controlled mob or an unpulled pack within the AoE radius of the target
        AllowedOnPack   // AoE on the pack at `aoeCenter` within `aoeRadius`
    };
    enum class Intensity : uint8
    {
        Normal,
        Burn,  // press the cooldowns (Bloodlust / Heroism included)
        Stop   // no damage
    };
    struct MemberEvents
    {
        uint32 seq = 0;  // bumped on every change of any field
        MoveEvent move = MoveEvent::None;
        Position spot;
        float tolerance = LEADER_SPOT_TOLERANCE;
        std::string moveWhy;
        uint32 moveMs = 0;
        AttackEvent attack = AttackEvent::None;
        ObjectGuid target;
        std::vector<ObjectGuid> killOrder;
        uint32 attackMs = 0;
        AoeRule aoe = AoeRule::Unset;  // with Attack: the AoE rule around the target
        Position aoeCenter;
        float aoeRadius = 0.0f;
        Intensity intensity = Intensity::Normal;
        uint32 intensityMs = 0;
        ObjectGuid cc;  // the mob this member crowd-controls (the leader's assignment)
        uint32 ccMs = 0;
        ObjectGuid interrupt;  // the caster this member interrupts
        uint32 interruptMs = 0;
        uint32 externalSpell = 0;  // an external save / buff the leader tells this member to cast, and on whom
        ObjectGuid externalTarget;
        uint32 externalMs = 0;
        uint32 threatSpell = 0;  // the tank: take `threatTarget` back with this (a taunt, else an AoE threat spell;
        ObjectGuid threatTarget; // 0 with a target: just attack it)
        uint32 threatMs = 0;
        ObjectGuid dontAttack;  // a mob loose on a member: no damage on it until the tank has it back
        uint32 dontAttackMs = 0;
        bool loot = false;  // [lead16] loot the fight's corpses
        uint32 lootMs = 0;
        uint32 preSaveUntilMs = 0;  // [lead16] PreSave: use a personal save now, a big hit lands before this
        std::string preSaveWhy;
        uint32 preSaveMs = 0;
        ReadyEvent ready = ReadyEvent::None;
        uint32 readyMs = 0;
    };
    struct OwnDecision
    {
        std::string action;
        std::string kind;  // "move", "target", "rest", "cc", "external", "burn", "buff on others"
        std::string why;
        uint32 ms = 0;
    };
    struct MemberAck
    {
        ObjectGuid member;
        uint32 seq = 0;
        std::string what;  // "move", "stand", "hold", "attack", "stop dps"
        std::string why;
        uint32 ms = 0;
    };
    namespace Events
    {
        // Leader side (world thread). Each returns true when the field changed (a new event to log).
        bool SetMove(ObjectGuid member, MoveEvent move, Position const& spot, float tolerance, std::string const& why);
        bool SetAttack(ObjectGuid member, AttackEvent attack, ObjectGuid target,
                       std::vector<ObjectGuid> const& killOrder);
        bool SetIntensity(ObjectGuid member, Intensity intensity);
        bool SetAoe(ObjectGuid member, AoeRule aoe, Position const& center, float radius);
        bool SetCc(ObjectGuid member, ObjectGuid mob);
        bool SetInterrupt(ObjectGuid member, ObjectGuid caster);
        bool SetExternal(ObjectGuid member, uint32 spell, ObjectGuid target);
        bool SetThreat(ObjectGuid member, uint32 spell, ObjectGuid target);
        bool SetDontAttack(ObjectGuid member, ObjectGuid mob);
        bool SetLoot(ObjectGuid member, bool loot);
        bool SetPreSave(ObjectGuid member, uint32 untilMs, std::string const& why);
        bool SetReady(ObjectGuid member, ReadyEvent ready);
        void Clear(ObjectGuid member);
        // Member side: the member's fresh events (stale fields come back None); false: none at all.
        bool Get(ObjectGuid member, MemberEvents& out);
        void Ack(ObjectGuid member, uint32 seq, std::string const& what, std::string const& why);
        bool TakeAck(ObjectGuid member, MemberAck& out);
        // [lead15] the member's own decisions (member thread) and their drain (the leader)
        void NoteOwn(ObjectGuid member, OwnDecision const& own);
        bool TakeOwn(ObjectGuid member, OwnDecision& out);
        // [lead16] a member tick in a fight with no fresh Attack / Heal (the rotation's stale_command fallback)
        void NoteStale(ObjectGuid member);
        uint32 TakeStale(ObjectGuid member);
        // [lead18] a vetoed strategy's trigger / value / action evaluated by a led member (vetoed_eval, goal 0)
        void NoteVetoedEval(ObjectGuid member, std::string const& what);
        bool TakeVetoedEval(ObjectGuid member, std::string& what, uint32& count);
    }
}

#endif
