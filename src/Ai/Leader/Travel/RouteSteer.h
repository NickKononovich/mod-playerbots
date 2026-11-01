/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_ROUTESTEER_H
#define PLAYERBOTS_LEADER_ROUTESTEER_H

// [lead107] user: production code does not live in the test harness. The leader's route walk (Steer: the tank's
// walk to the route target, its stops, holds, pulls and skips) and the facts it reads, moved out of
// TestHarnessRunner.cpp unchanged; the harness calls it through the group view.

#include "Creature.h"
#include "G3D/Vector3.h"
#include "G3D/Vector4.h"
#include "GameObject.h"
#include "MoveSplineInitArgs.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Position.h"
#include "LeaderKnowledge.h"
#include "LeaderGroup.h"
#include "LeaderKnowledge.h"
#include "RouteEvents.h"
#include "RouteLegs.h"
#include "RoutePatrol.h"
#include "RouteSearch.h"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "RouteTargets.h"

namespace Leader::Travel
{
    constexpr float PULL_RANGE = 20.0f;

    constexpr uint32 ROUTE_BLOCKER_MAX_TRIES = 3;      // made the target this often per blocker, then as before

    constexpr uint32 HARNESS_HOLD_LOG_MS = 15 * IN_MILLISECONDS;

    bool PullAfterHoldPasses(ObjectGuid leader, ObjectGuid blocker);

    // [b33_aoepull] A required route blocker the planner called hopeless (its split search exhausted) is held
    // AOE_PULL_ASK_MS for the planner's aoe_pull_plan (AoePull::MarkRequired / Planned, ChooseTargetActions.cpp)
    // before the dead end: mgt-h 20261026-000434-mgt-h-5G-16 (Sunblade Mage Guard before Delrissa, "with it 21
    // mobs, over 18 (no split in 19 searches)") and strat-n 20261026-000434-strat-n-5A-2 (King's Square) stood
    // refused on both sides - the planner's hopeless verdict and the harness's refused walk - until stopped. While
    // the planner's issued aoe_pull_plan is fresh the route's walk-in holds AOE_PULL_WALK_MS (the tank pulls to its
    // stack spot itself); only an issued plan the tank has not started by then is walked in (via the planner ask).
    constexpr uint32 AOE_PULL_ASK_MS = 10 * IN_MILLISECONDS;

    constexpr uint32 AOE_PULL_ASK_RESET_MS = 5 * MINUTE * IN_MILLISECONDS;

    constexpr uint32 AOE_PULL_WALK_MS = 30 * IN_MILLISECONDS;

    constexpr float BODY_PULL_RANGE = 5.0f;

    constexpr float BODY_PULL_MELEE_SLACK = 1.0f;  // [b32_bm] a walk-in arrives this far past the melee range

    // [b32_bm] a walk-in this far past the melee range (in sight) is no "route end": the tank stands beside it
    constexpr float BODY_PULL_STUCK_REACH = 3.0f;

    constexpr float STARTER_RANGE = 4.0f;

    constexpr float BOSS_BUFF_DISTANCE = 70.0f;

    constexpr uint32 NOT_PULLED_SKIP_MS = 10 * MINUTE * IN_MILLISECONDS;

    constexpr uint32 PULL_WAIT_MS = 45 * IN_MILLISECONDS;

    constexpr uint32 EVENT_WAIT_FOR_SPAWNS_MS = 5 * MINUTE * IN_MILLISECONDS;

    constexpr uint32 BODY_PULL_WAIT_MS = 10 * IN_MILLISECONDS;

    // ROUTE_CAREFUL_GUARANTEE_HOLD (build 11): a pack the planner refused but will take once its stand guarantee is
    // due (CarefulPlanVerdict::GetRefusal, not hopeless) is not skipped: the group holds at it while the guarantee
    // runs, GUARANTEE_EXECUTE_MS more for the pull itself, GUARANTEE_HOLD_TOTAL_MS from the arrival at most
    // (build 14: the planner's 1.5x maxMobs tier comes after 180 s, waitMs up to ~200 s). A hopeless refusal (a
    // real dead end) is skipped at once, the planner's `why` in the skip reason, and the route goes round it.
    constexpr bool ROUTE_CAREFUL_GUARANTEE_HOLD = true;

    constexpr uint32 GUARANTEE_EXECUTE_MS = 30 * IN_MILLISECONDS;

    // [guarantee_hold_trim] 150 s (was 240): the planner's guarantee now comes after 30 s within the cap and 90 s
    // over it (ChooseTargetActions.cpp careful_guarantee_trim), plus GUARANTEE_EXECUTE_MS for the pull.
    constexpr uint32 GUARANTEE_HOLD_TOTAL_MS = 150 * IN_MILLISECONDS;

    constexpr uint32 GUARANTEE_UNKNOWN_HOLD_MS = 120 * IN_MILLISECONDS;

    constexpr uint32 PATROL_WAIT_HOLD_MS = 30 * IN_MILLISECONDS;  // [patrol_wait_spot] the stand there at most

    // [held_for_now_cap] a "held for now" with the group fully ready this long is a refusal (under the 120 s idle
    // stall watchdog)
    constexpr uint32 HELD_FOR_NOW_MAX_MS = 75 * IN_MILLISECONDS;

    // The bots' own boss hold (RaidLeader "pull hold": its ready check, PULL_HOLD_MAX_MS 150 s) is waited out
    // before a boss counts as "not pulled" (Oculus heroic 2026-10-25: Drakos, who never aggroes by sight, was
    // skipped after 10 s while the tank was held).
    constexpr uint32 BOSS_PULL_HOLD_WAIT_MS = 150 * IN_MILLISECONDS;

    constexpr uint32 WING_DONE_NO_TARGET_MS = 3 * IN_MILLISECONDS;

    // [st-guard-approach] the approach point counts as reached within this; on the guard's floor (SAME_FLOOR_DZ)
    // and no farther from it than the point + slack, the point is skipped.
    constexpr float GUARD_APPROACH_ARRIVE = 5.0f;

    constexpr float GUARD_APPROACH_SLACK = 8.0f;

    // [dmw-guard-body-pull] a pending door guard refused this often by the careful planner is body-pulled (user
    // decision 2026-10-25: approved, tag guard_body_pull).
    constexpr uint32 GUARD_REFUSALS_BEFORE_BODY_PULL = 2;

    // [ulda-ritual-hold] a summoning ritual (GAMEOBJECT_TYPE_SUMMONING_RITUAL) casts its spell 5 s after the
    // click (GameObject::Use m_cooldownTime) and only while the user still channels the anim spell.
    constexpr uint32 RITUAL_CHANNEL_HOLD_MS = 6500;


    // [unseen-blocker-pull] an unseen neighbour pack this small that holds an approach becomes the target
    constexpr size_t UNSEEN_BLOCKER_PACK_MAX = 4;

    // [route_blocker_body_pull] User decision 2026-10-25 (overrides "the harness never walks the group into a pack
    // the tank refused" for this case): a pack the route's walk has to pass (ApproachStop: the leg to the target
    // enters its reach) is never skipped - neither it nor the targets behind it. It becomes the target, the tank
    // pulls it by sight when it can (its planner, the guarantee), else the group gets ready (GroupFullyReady:
    // health, healer mana, everyone here) and the leader walks into it (body pull, navmesh walk, no door
    // clipping). Only a pack that is not on the way (a side pack the route picked as a target) may be skipped.
    // st-n 2026-10-25 081744 08:56-08:57: a 5-mob Atal'ai Slave pack at the balcony stairs held the edge, then
    // Hukku, Loro, Mijan, Zul'Lor were skipped "no sight point" and Gasher, Zolo and the Witch Doctors "behind
    // skipped Atal'ai Slave"; the group stood.
    constexpr bool ROUTE_BLOCKER_BODY_PULL = true;

    constexpr uint32 ROUTE_BLOCKER_PREP_MS = 30 * IN_MILLISECONDS;   // the planner's chance before the walk in

    constexpr uint32 ROUTE_BLOCKER_SIGHT_HOLD_MS = 8 * IN_MILLISECONDS;  // the edge hold for a route blocker

    // [route_blocker_walk_through] an idle pack this close to the route blocker is part of its body pull
    constexpr float ROUTE_BLOCKER_THROUGH_RANGE = 30.0f;

    constexpr uint32 NEIGHBOUR_HOLD_MAX = 2;          // boss approach holds at one neighbour before it passes

    constexpr size_t NEIGHBOUR_PASS_PACK_MAX = 3;     // a neighbour pack this small no longer stops it

    constexpr float BEHIND_SHADOW_RANGE = 60.0f;  // ROUTE_SKIP_BEHIND_SKIPPED: the shadow skipped with a target

    constexpr float BEHIND_CONE_COS = 0.82f;      // ~35 degrees either side of the leader -> blocker line

    constexpr uint32 HOR_MAP = 668;

    // A script slot IN_PROGRESS with nothing to fight is an event between its waves (Halls of Stone: the Tribunal
    // of Ages, three 100 s phases with dialogue): no end of the run meanwhile, at most this long.
    constexpr uint32 SCRIPT_EVENT_MAX_MS = 10 * MINUTE * IN_MILLISECONDS;

    // A target whose approach a skipped neighbour pack stops is skipped at once (Steer, approach stop).
    constexpr bool ROUTE_SKIP_BEHIND_SKIPPED = true;

    // The pull wait (PULL_WAIT_MS) runs per stop, not per target: a new target at the same stop keeps it.
    constexpr bool ROUTE_STOP_KEEPS_WAIT = true;

    constexpr float STOP_SAME_RANGE = 5.0f;

    constexpr uint32 STOP_KEEP_GAP_MS = 15 * IN_MILLISECONDS;

    constexpr uint32 STEER_STUCK_MS = 20 * IN_MILLISECONDS;

    // [steer_rest_not_stuck] The leader seated with food / drink (RestState: the aura by its spell specific) out of
    // combat is no lack of progress: st-n build 23 (103125 / 103436 -2 / -3) - all 7 "route end" skips of the
    // Sunken Temple defenders were the paladin tank drinking at 29-34 % mana for 20 s (phase "traveling", the
    // group not waited for above its line), and each skip sent the walk to the next defender and back.
    constexpr bool STEER_REST_NOT_STUCK = true;

    constexpr float STEER_TWIN_RANGE = 30.0f;  // same-entry mobs skipped with an unreachable one (a ledge of them)

    constexpr uint32 STARTER_STUCK_RETRY_MS = 2 * MINUTE * IN_MILLISECONDS;

    constexpr uint32 STARTER_NO_NEARER_MS = 90 * IN_MILLISECONDS;

    constexpr uint32 STARTER_OBJECT_STUCK_SKIP_MS = 10 * MINUTE * IN_MILLISECONDS;  // StarterTrack::stuckUntilMs

    // A target on another navmesh part next to the route's end (a ramp without polygons): walked over straight.
    constexpr float GAP_CROSS_MAX_DISTANCE = 45.0f;

    constexpr uint32 GAP_CROSS_TRIES = 3;

    constexpr float DRIVE_STEP = 8.0f;

    // Farther than this from the intro's boss the leader first walks on to the event spot (Svala picks her first
    // target within 100 yd).
    constexpr float INTRO_WAIT_RANGE = 80.0f;

    // [dmw-guard-body-pull] (party seq, guard guid) -> careful refusals of a pending door guard.
    extern std::map<std::pair<uint32, ObjectGuid>, uint32> guardRefusals;

    // once-per-target / once-per-boss log keys (party seq, guid)
    extern std::set<std::pair<uint32, ObjectGuid>> guardApproachLogged;

    // [guard_approach_arrived] (party seq, guard guid) -> when its "walking" line was logged; erased on arrival
    extern std::map<std::pair<uint32, ObjectGuid>, uint32> guardApproachSinceMs;

    // [steer_rest_not_stuck] (party seq, target guid): the leader's own rest during that walk, logged once
    extern std::set<std::pair<uint32, ObjectGuid>> steerRestLogged;

    // A boss whose body pull brought no fight: tried again after this (Oculus wave 9/10: Drakos "not pulled" at
    // 372 / 415 s; the 10 minute skip left nothing to do before him on foot).
    constexpr uint32 BOSS_NOT_PULLED_SKIP_MS = 2 * MINUTE * IN_MILLISECONDS;

    constexpr bool ROUTE_APPROACH_TO_SIGHT = true;

    constexpr bool ROUTE_LOG_TANK_REJECTS = true;

    // Refused-walk pass (2026-10-24, *.bak_refusedwalk), false restores the behaviour before it of that piece:
    // - approach to sight never walks into the aggro reach (AggroReach, the margin included) of an idle mob of the
    //   target's or the blocker's pack that sees the point, unless the tank's careful planner accepted a plan for
    //   that pack (TankPlanVerdict); the leader holds at the edge instead. RFC 20:08 (user: "the tank aggroed one
    //   mob, then two more - why?"): the planner refused the 3-trogg pack, the sight point 10 yd on lay inside
    //   the trogg's real aggro range, the walk there body-pulled the pack;
    // - a boss the group stands ready for is body-pulled after BOSS_PULL_READY_MS, not after PULL_WAIT_MS (RFC
    //   20:10: Oggleflint, the tank's planner never picks a boss, 45 s stood for nothing).
    constexpr bool ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED = true;

    constexpr bool ROUTE_BOSS_PULL_IMMEDIATE = true;

    // Harness walks that are no approach to their target (a body pull past its own pack, MeetMember's walk back)
    // stop at the aggro edge of every other idle pack (wave 7 scholo-h / mt-h / mt-n / lbrs-n mega-pulls: the
    // tank's last intent "test runner move", no [pull plan], refused 20-mob packs pulled by the walk).
    constexpr bool ROUTE_HARNESS_WALK_GUARD = true;

    // [hleft-hor-reentry-hold] halls_of_reflection.h: DATA_MARWYN 1, CenterPos; areatrigger_teleport 5636.
    constexpr uint32 HOR_FALRIC_ENTRY = 38112;

    constexpr uint32 HOR_MARWYN_ENTRY = 38113;

    constexpr uint32 HOR_MARWYN_SLOT = 1;

    constexpr float HOR_CENTRE_X = 5309.46f;

    constexpr float HOR_CENTRE_Y = 2006.48f;

    constexpr float HOR_ENTRANCE_X = 5239.01f;

    constexpr float HOR_ENTRANCE_Y = 1932.64f;

    constexpr float HOR_ENTRANCE_Z = 707.70f;

    constexpr float HOR_HOLD_FROM_CENTRE = 60.0f;  // the script's 40 yd start range plus a margin

    constexpr uint32 HOR_HOLD_LOG_MS = 30 * IN_MILLISECONDS;

    constexpr uint32 HOR_LICH_KING_ENTRY = 36954;  // [hleft-never-pulled]

    constexpr uint32 HOR_LICH_KING_ESCAPE_ENTRY = 37226;

    constexpr uint32 AN_HADRONOX_ENTRY = 28921;

    constexpr uint32 AK_TWILIGHT_INITIATE_ENTRY = 30114;  // [hleft-ak-jedoga-owns]

    constexpr uint32 AK_JEDOGA_ENTRY = 29310;

    // [hleft-swim-back] the swim trail (TestHarnessMgr::Steer)
    constexpr bool ROUTE_SWIM_BACK = true;

    constexpr float SWIM_TRAIL_STEP = 5.0f;

    constexpr size_t SWIM_TRAIL_MAX = 200;

    constexpr float SWIM_CLIMB_DZ = 2.0f;  // a target this far above the swimmer is no climb out of the water

    constexpr float WAYPOINT_REACHED_SWIM = 3.0f;

    constexpr uint32 SWIM_BACK_MAX_MS = 2 * MINUTE * IN_MILLISECONDS;

    constexpr bool ROUTE_TWIN_SKIP = true;

    // instance id -> shortest remaining walk to the starter
    extern std::unordered_map<uint32, float> starterNearestPath;

    constexpr bool ROUTE_REFUSE_DEAD_END = true;

    // A partial leg is judged only while the target is farther than GAP_CROSS_MAX_DISTANCE: nearer, the gap cross,
    // the own move and the "pull from here" of steer_stuck keep their say.
    // Below a cliff: the target stands this much above the end (above the water surface there, a swimmer rises)
    // and the rise is this share of the horizontal gap or more. Measured (route_backbone, 2026-10-24/25 logs):
    // Maraudon Earth Song Falls rise 71 over 95 / 113 yd (0.74 / 0.63) - dead ends; the Mechanar stairs to
    // Sepethrea 28 over 83 (0.34), Gundrak's Gal'darah altar 23 over 36 (0.64, killed in every run) and
    // Ahn'kahet's ramps (7.8 over 20.8) - walked on by the core's own move, kept by the 40 yd minimum.
    constexpr float DEAD_END_CLIFF_MIN_RISE = 40.0f;

    constexpr float DEAD_END_CLIFF_RATIO = 0.6f;

    // Ends no nearer: the end must gain this much on the walker's own distance (yd, or that share of it).
    constexpr float DEAD_END_MIN_GAIN = 5.0f;

    constexpr float DEAD_END_MIN_GAIN_SHARE = 0.1f;

    // Only when the walk to the end is this long: a leader next to the end walks it (Underbog build 30: 11.6 yd
    // from the root's foot 222.4,-154.1 Musel'ek was refused "ends no nearer", the own move there is the climb).
    constexpr float DEAD_END_WALK_MIN = 25.0f;

    // The same end of the same target where a "route end" skip (steer_stuck) already left it: no walk back to it.
    // Recorded by SkipTarget only - not every end the leader stood at (Underbog build 30: the steep knob
    // 232.3,-194.4,12.4 under the root, where the own move climbs, was refused "same dead end again" 11 times).
    constexpr float DEAD_END_SAME_END = 5.0f;

    constexpr float DEAD_END_SAME_GOAL = 10.0f;

    constexpr float DEAD_END_PROGRESS = 10.0f;

    constexpr uint32 DEAD_END_MEMORY_MS = 10 * MINUTE * IN_MILLISECONDS;

    // A known way on: an end (or goal) this near a route link's start or far end (RouteLinkDef: the Underbog root
    // to Musel'ek's terrace) is never refused.
    constexpr float DEAD_END_LINK_RANGE = 30.0f;

    // Never a stall: DEAD_END_FALLBACK_REFUSALS refusals within DEAD_END_FALLBACK_WINDOW_MS put the leader back on
    // the old walk (the partial route, the own move at its end, steer_stuck) for DEAD_END_FALLBACK_MS.
    constexpr uint32 DEAD_END_FALLBACK_REFUSALS = 3;

    constexpr uint32 DEAD_END_FALLBACK_WINDOW_MS = 3 * MINUTE * IN_MILLISECONDS;

    constexpr uint32 DEAD_END_FALLBACK_MS = 10 * MINUTE * IN_MILLISECONDS;

    struct DeadEndVisit
    {
        G3D::Vector3 end;
        G3D::Vector3 goal;
        float distance = 0.0f;  // walker to goal while it stood at the end
        uint32 ms = 0;
        ObjectGuid target;
    };

    extern std::unordered_map<ObjectGuid, std::vector<DeadEndVisit>> deadEndVisits;  // per walker, world thread only

    struct DeadEndRefusals
    {
        std::vector<uint32> ms;      // recent refusals
        uint32 fallbackUntilMs = 0;  // the old walk until then
    };

    extern std::unordered_map<ObjectGuid, DeadEndRefusals> deadEndRefusals;  // per leader, world thread only

    struct DeadEndVerdict
    {
        std::string why;  // empty: walking to the end is progress (or not judged here)
        float distance = 0.0f;
        float left = 0.0f;
        float hgap = 0.0f;
        float dz = 0.0f;
        float rise = 0.0f;
        float surface = INVALID_HEIGHT;
        bool underwater = false;
        uint32 visitSec = 0;
    };

    // [incomplete_route_deadend] Is walking to the end of a partial leg progress towards goal? The why of a no
    // (Steer then skips the target as "route end", rechecked once the leader stands elsewhere), empty for a yes or
    // when the target is near enough for the gap cross / own move / "pull from here". Records the ends the walker
    // stood at (DeadEndVisit) and forgets one once the walker got nearer to the goal than it was there.
    // Every Steer tick, whatever the leg: an end the walker got past (nearer to the goal by DEAD_END_PROGRESS than
    // it stood there - the Mechanar stairs climbed by the core's own move) was no dead end.
    void ForgetPassedDeadEnds(Player* walker, G3D::Vector3 const& goal);

    DeadEndVerdict JudgeIncompleteEnd(Player* walker, G3D::Vector3 const& end, G3D::Vector3 const& goal,
                                      uint32 pathType, ObjectGuid target);

    // [incomplete_route_deadend] The never-a-stall fallback: true while the leader walks partial routes the old
    // way. A refusal counts; the DEAD_END_FALLBACK_REFUSALS-th within the window starts it (started: this call).
    bool DeadEndFallback(Player* leader, bool refusal, bool& started);

    constexpr float AGGRO_EDGE_MARGIN = 8.0f;  // one harness tick of walking (7 yd/s) stays outside aggro

    constexpr float SAME_FLOOR_DZ = 3.0f;

    // The leader stands at the aggro edge of an unengaged mob of the target's pack: on its floor, in line of
    // sight, no farther than its aggro range plus the margin. The pull logic takes over from here.
    bool AtPackAggroEdge(Player* leader, Creature* target);

    // Approach stop (route pass 4): the leader's waypoint never lies inside the aggro reach of an idle hostile
    // around the target (SV-n wave 11: the waypoint 30 yd ahead lay in a refused pack; arriving at its edge only
    // dropped the waypoint, the walk under way went on and pulled 10 mobs). A player stops outside the aggro
    // range of every idle mob, plus a margin, and pulls or skips from there.
    constexpr float APPROACH_SCAN_RADIUS = 45.0f;  // around the target: its pack and the neighbours on the way

    constexpr float APPROACH_SAMPLE_STEP = 1.0f;

    constexpr float APPROACH_ARRIVED = 3.0f;

    // AggressiveTargetValue MAX_PULL_HEIGHT_DIFF (12 yd), less a margin.
    constexpr float TANK_PULL_HEIGHT = 11.5f;

    // AggressiveTargetValue's MAX_PULL_HEIGHT_DIFF today (18 yd): beyond it the tank lists a mob as rejected
    // (TankRejects) for its height. Only for the "approach ignores" line; the registry itself decides.
    constexpr float TANK_REJECT_HEIGHT = 18.0f;

    constexpr uint32 TANK_REJECT_LOG_MS = 10 * IN_MILLISECONDS;

    // mob -> last "approach ignores" line, world thread only
    extern std::unordered_map<uint64, uint32> tankRejectLogMs;

    // Approach to sight (ROUTE_APPROACH_TO_SIGHT): from a stop that does not see its blocker, the route ahead is
    // sampled every APPROACH_SAMPLE_STEP up to APPROACH_SIGHT_PATH yd for the first point that sees it, at most
    // APPROACH_SIGHT_TRIES line-of-sight tests.
    constexpr float APPROACH_SIGHT_PATH = 15.0f;

    constexpr uint32 APPROACH_SIGHT_TRIES = 25;

    // The keep-out tests of approach to sight (ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED): line-of-sight tests from the
    // route points inside a pack mob's aggro reach, a budget of their own.
    constexpr uint32 APPROACH_SIGHT_KEEP_OUT_TRIES = 40;

    // The route's own stretch is searched too, not only the target's surroundings: around the leader, the walked
    // length plus this (the largest aggro reach of a dungeon mob). Stratholme wave of 2026-10-24 09:27 / 09:48:
    // the route to a Ravaged Cadaver 108 yd away was checked only against the mobs within 45 yd of it.
    constexpr float APPROACH_ROUTE_SCAN_MARGIN = 40.0f;

    // A mob more than this above or below the tank stands on another floor (a ledge, a gallery over the hall):
    // it stops the walk only where the route reaches its floor. RFC wave 37: a trogg 23-25 yd from the group,
    // the targets on the ledges 13-23 yd above it, held every route at (-173.4, 1.8, -52.4) and the run stalled.
    constexpr float APPROACH_OTHER_FLOOR_DZ = 5.0f;

    constexpr float PATROL_WAIT_STEP = 5.0f;

    constexpr float PATROL_WAIT_STEP_MAX = 40.0f;

    bool PatrolWaitSpot(Player* leader, G3D::Vector3& spot, Creature*& nearMob);

    // ROUTE_UNSEEN_ROUTE_END_PASS (wave 21, *.bak_w21idle): a mob skipped as "route end" while the leader could
    // not see it (no walk to its sight point) stops no walk by the follower-spread rule any more (unseenStop): the
    // spread stop held the boss approach at the edge of what nobody can see nor reach. sm-h wave 21: Scarlet
    // Monk / Diviner behind the Library shelves, "route end", then Arcanist Doan's approach stopped by them
    // (unseenStop) 64 s, the run idle 138 s. A mob that sees the route point still stops it (line of sight).
    constexpr bool ROUTE_UNSEEN_ROUTE_END_PASS = true;

    extern std::unordered_map<uint64, uint32> unseenRouteEndUntilMs;  // creature guid -> until (world thread)

    extern std::unordered_map<uint64, uint32> unseenRouteEndLogMs;

    void MarkUnseenRouteEnd(Creature* mob, uint32 untilMs);

    bool UnseenRouteEnd(Player* leader, Creature* mob);

    // [hleft-boss-own-pack] A mob that comes into `boss`'s fight by itself when he is pulled - his formation or a
    // script call for help (JoinsBossFight "formation" / "script") - is his encounter, not a neighbour pack: the
    // careful planner refuses such a pack as "brings a boss, pulled as his fight" (ChooseTargetActions), so a
    // harness that stops the boss's walk at it waits for a pull nobody makes. Razorfen Downs heroic 2026-10-25
    // (20261025-070358-rfd-h-5A-78): the Splinterbone Skeleton pack "joins boss: script" refused for Mordresh,
    // Mordresh's approach stopped at it as a neighbour ("harness_hold_refused_pack" boss approach) until the stall
    // skipped him.
    constexpr bool ROUTE_BOSS_OWN_ENCOUNTER_PACK = true;

    bool BossEncounterMob(Player* leader, Creature* boss, Creature* mob);

    // [b33_dme] A boss whose wander or aggro reach covers `mob`'s stand (the careful planner's "wanders to the
    // spot" / "aggro reaches spot" for an idle boss): pulling `mob` is his fight, the planner refuses its pack as
    // "brings a boss, pulled as his fight once the group is ready for him". Dire Maul East 5H (bear tank)
    // 20261026-000434-dme-n-5H-40: the Wildspawn Hellcaller (a caster, no lone-mob pull) 25 yd from Hydrospawn's
    // pool stopped every walk to Zevrim; boss_on_the_way_first fired only for a blocker that IS a boss, the body
    // pull was held at the refused Hellcaller ("harness_hold_refused_pack" body pull) every 10 s, 0/4 at 24 min.
    constexpr bool ROUTE_BOSS_BROUGHT_BY_PACK = true;  // [b33_dme] false: the build-32 behaviour

    bool BossBroughtBy(Player* leader, Creature* boss, Creature* mob);

    // [b33_harn] The boss's own room pack: an idle mob on the boss's floor within BOSS_ROOM_TRASH_RADIUS of him
    // that he sees but the leader does not (no pull of its own from here: the planner never sees it, verdict
    // "none", 0 mobs) is part of the boss pull - the walk to the boss does not stop at it. scholo-h
    // 20261026-000434-scholo-h-5A-3 (2453-2572 s): every body pull of Vectus held at a Scholomance Student 31.8 yd
    // off ("harness_hold_refused_pack", verdict none, realPack 3-4, no line of sight from the tank), then
    // pull_after_hold targeted the unseen Student, "route end", Vectus skipped 300 s. A footprint mob the leader
    // DOES see stays a stop: it is pulled first when cheap (TickBossAreaFirst / boss-area trash).
    constexpr bool ROUTE_BOSS_ROOM_PACK = true;  // false: the build-33 behaviour

    bool BossRoomPack(Player* leader, Creature* boss, Creature* mob);

    constexpr bool ROUTE_STOP_ONLY_REAL_AGGRO = true;  // [stop_only_real_aggro] false: the margin stops too

    constexpr float STOP_REAL_SLACK = 1.5f;

    // [b34_petramp] The Z gate (Creature::CanStartAttack: no proximity aggro on a unit more than
    // CREATURE_Z_ATTACK_RANGE + m_CombatDistance above / below the mob, object sizes off) keeps the TANK idle on a
    // ramp, never the group: the followers and pets stand lower or further ahead, and a flier has no gate at all.
    // scholo-h 20261026-014521-scholo-h-5H-3 (116-133 s): the stop on the Reliquary ramp at (221.1,51.6,122.1)
    // stood 6.3 yd above the hall floor, 20.8 yd from a Scholomance Neophyte (real aggro 21.0, approach_stop
    // insideRealAggro true) - the gated ramp points counted as out of reach, so the stop lay where the gate first
    // let the mob see the leader; the hunter's Cat by the tank opened the 33-mob pull 5 (wipe). A point inside the
    // real 3D range of a mob that sees it is no stop whatever the Z gate says: the stop goes back up the stairs,
    // out of the floor's sight (a line-of-sight pull from there), or back along the tank's trail when the leader
    // stands inside already (STOP_TRAIL_BACK_MAX).
    constexpr bool ROUTE_STOP_IGNORES_Z_GATE = true;  // [b34_petramp] false: the build-33 gated stop

    constexpr float STOP_TRAIL_BACK_MAX = 30.0f;

    constexpr float STOP_TRAIL_FLOOR = 8.0f;  // a trail point this far above / below the leader is another floor

    bool ApproachStop(Player* leader, WorldObject* target, Movement::PointsArray const& points, float length,
                      G3D::Vector3& stop, Creature*& blocker, bool& unseenStop, Creature* exemptPack = nullptr,
                      Leader::LeaderGroup* group = nullptr);

    // ROUTE_HARNESS_WALK_GUARD: the stop of a harness walk before the aggro reach of the idle mobs on its route
    // (ApproachStop around the leader, or around exemptPack, whose own pack is exempt). True: `point` is the stop,
    // `held`: the leader stands there already.
    bool HarnessWalkStop(Player* leader, Movement::PointsArray const& points, float length, Creature* exemptPack,
                         G3D::Vector3& point, Creature*& blocker, bool& held, Leader::LeaderGroup* group = nullptr);

    // blocker -> last harness_hold_refused_pack, world thread
    extern std::unordered_map<uint64, uint32> harnessHoldLogMs;

    // [pull_after_hold] User 2026-10-25 (scholo-h 153805, «тупят, излишне ждут»): every approach stop, guarantee
    // hold and careful-pull refusal of a pack on the way or on the required route ends PULL_AFTER_HOLD_MS after
    // the group stands fully ready (GroupFullyReady) at it: the deliberate pull - the blocker behind an openable
    // closed door has its door opened first (a lattice Iron Gate, 15:42-15:44: no line of sight, nobody opened it,
    // the 120 s idle event ended the stand), then the route blocker target (its sight pull, else its walk in) or
    // the body pull - instead of the 45 s wait, the planner's stand guarantee or a skip. A patrol / overlap wait
    // of the planner keeps its own short clock. Per leader, world thread.
    constexpr bool PULL_AFTER_HOLD = true;

    constexpr uint32 PULL_AFTER_HOLD_MS = 10 * IN_MILLISECONDS;

    constexpr uint32 PULL_AFTER_HOLD_GATE_MS = 20 * IN_MILLISECONDS;  // the body pull's gate lets it through

    struct HoldReady
    {
        ObjectGuid target;
        uint32 sinceMs = 0;  // since when the group stands ready at this target (0: not ready)
        Position at;         // where the leader stood when the clock began (a new target there keeps it)
    };

    extern std::unordered_map<ObjectGuid, HoldReady> holdReady;

    struct PullAfterHoldGate
    {
        ObjectGuid blocker;
        uint32 untilMs = 0;
    };

    extern std::unordered_map<ObjectGuid, PullAfterHoldGate> pullAfterHoldGates;  // by leader

    // [b33_bodypull] The classes of a planner refusal (CarefulPlanVerdict's why, ChooseTargetActions.cpp) the
    // harness reads before a walk-in. A READINESS refusal (the healer not with the tank / the group, a member
    // missing, dead, sitting below its line or buffing, not topped up) is a wait the walk-in can never end: no
    // pull_after_hold clock nor the planner's time-out overrides it, the group holds. An OVER-CAP refusal (the
    // pack does not fit, over the cap, a split search still running) is the planner's own [b32_smpull] split /
    // guarantee to take: the harness never walks the whole group into it (strat-h 20261026-000434-5D-1: a 4-6 mob
    // Ravaged Cadaver blocker the planner held as "does not fit" chained to 17 on the walk-in). Only the rest -
    // the stuck route / no plan from range class ("no pull of X from range ... the route's body pull decides",
    // "given up for range", a within-cap pack the stand's guarantee takes) - is the harness's walk-in to make.
    bool RefusalIsReadiness(std::string const& why);

    // [pull_after_hold] (4) the walk to a closed door that opens by use, between the leader and a blocker it
    // cannot see: kept over the ticks until it is open (the stop would walk the leader back).
    struct DoorWalk
    {
        ObjectGuid door;
        ObjectGuid blocker;
        G3D::Vector3 point;
        uint32 untilMs = 0;
    };

    extern std::unordered_map<ObjectGuid, DoorWalk> doorWalks;  // by leader

    // [body_pull_via_planner] plan29 follow-up 4 (build 29: harness body pulls / walks were 13 % of all pulls,
    // and the unplanned rest died five times as often as the planned pulls): before the harness walks the
    // leader into a trash pack, the tank's planner is asked to take it (GuaranteeRequest: its guarantee pull of
    // the pack as it stands, at range when it has a pull, with its readiness check) for BODY_PULL_PLANNER_MS;
    // only then the walk in. Bosses keep their body pull (the planner never picks a boss). Per leader.
    constexpr bool BODY_PULL_VIA_PLANNER = true;

    constexpr uint32 BODY_PULL_PLANNER_MS = 8 * IN_MILLISECONDS;

    struct PlannerAsk
    {
        ObjectGuid target;
        uint32 sinceMs = 0;
    };

    extern std::unordered_map<ObjectGuid, PlannerAsk> plannerAsks;

    extern std::unordered_map<ObjectGuid, PlannerAsk> aoePullAsks;  // by leader

    bool AoePullAskRuns(ObjectGuid leaderGuid, ObjectGuid blocker, uint32 now);

    // True while the planner still has its time (the caller holds the leader); `first`: the ask began now.
    bool AskPlannerFirst(ObjectGuid leaderGuid, Creature* target, uint32 now, bool& first);

    // [behind_root_pull] the skipped pack a walk stopped at is retargeted at most this often (world thread)
    constexpr bool BEHIND_ROOT_PULL = true;

    constexpr uint32 BEHIND_ROOT_TRIES = 2;

    constexpr uint32 BEHIND_ROOT_WINDOW_MS = 5 * MINUTE * IN_MILLISECONDS;

    struct BehindRootTry
    {
        uint32 firstMs = 0;
        uint32 count = 0;
    };

    extern std::unordered_map<ObjectGuid, BehindRootTry> behindRootTries;

    // [pull_estimate_vs_actual] the last PullEstimate result the leader's JSONL has
    extern std::unordered_map<ObjectGuid, uint32> pullEstimateSeen;

    // The first point of the route ahead (from the leader, up to APPROACH_SIGHT_PATH yd) from which the tank's eye
    // line reaches the blocker on a pullable height (TANK_PULL_HEIGHT) - the corner the stop lay before. A point
    // inside the real aggro range of the blocker is fine only for a pack the tank's planner accepted (keepOut
    // empty): walking into its sight is the pull. keepOut (ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED): the idle mobs of the
    // target's and the blocker's packs while no plan for them is accepted - a point inside the aggro reach
    // (AggroReach, the margin included) of one that sees it is a body pull of a pack the planner did not take:
    // the search ends there, heldAtEdge (the route only goes deeper from there). For a blocker outside the
    // target's pack, no point from which the target sees the leader inside its real aggro range (one pack at a
    // time). At most APPROACH_SIGHT_TRIES line-of-sight tests.
    bool FindSightPoint(Player* leader, Creature* blocker, Creature* target, bool ownPack,
                        Movement::PointsArray const& points, std::vector<Creature*> const& keepOut,
                        G3D::Vector3& sight, bool& heldAtEdge);

    // What the tank's careful planner said about a pack, as far as the harness reads it without the bot's AI
    // context (no cross-thread GetValue): the group's pre-pull crowd control plan (PrePullCc, behind its lock)
    // names the tank's planned pack - accepted, or refused as it stands (`heavy`, also a fallback plan: no
    // accepted one) - and the harness's own count of the pack's careful refusals (the PULL_WAIT_MS skips).
    // The planner's published verdict (CarefulPlanVerdict) is read first. Unknown otherwise: approach to sight
    // treats it as "not accepted" and keeps out of the pack's reach.
    enum class PlanVerdict
    {
        Unknown,
        Accepted,
        Refused
    };

    // The edge hold's give-way (SIGHT_UNKNOWN_HOLD_MS): the target's own pack may be walked into sight of now, no
    // other one. BRS heroic 22:10:30: the give-way cleared the whole keep-out, the sight point 15 yd on lay in the
    // Smolderthorn and Firebrand packs' aggro - 26 attackers, wipe. The idle hostiles within
    // SIGHT_GIVE_WAY_SCAN of the leader outside the target's pack (PACK_RADIUS around it), bosses included (a
    // neighbour boss is a boss pull nobody chose); FindSightPoint keeps every point out of their aggro
    // (IdleAggroBlocker) as it does for a refused pack.
    constexpr float SIGHT_GIVE_WAY_SCAN = 50.0f;  // APPROACH_SIGHT_PATH plus the widest idle aggro reach

    struct OtherPackMobCheck
    {
        Player* leader;
        Creature* target;
        bool operator()(Creature* creature) const
        {
            return creature->IsAlive() && !creature->IsInCombat() && !creature->IsCritter() &&
                   !creature->IsCivilian() && creature->IsHostileTo(leader) &&
                   creature->GetExactDist(target) > PACK_RADIUS;
        }
    };

    std::vector<Creature*> SightKeepOutOthers(Player* leader, Creature* target);

    // A leader held at the edge by approach to sight: the search is not run again for SIGHT_HOLD_RECHECK_MS (it
    // costs up to APPROACH_SIGHT_TRIES + APPROACH_SIGHT_KEEP_OUT_TRIES line-of-sight tests), the line is logged
    // once per SIGHT_HOLD_LOG_MS. Per leader, for one target and blocker. The search runs only with the leader at
    // its stop; 2.5 s, not 5 s: Utgarde Keep heroic 22:26:01 held at the edge, 22:26:11 skip at once, the pull at
    // 22:26:16 - two rechecks of idling the user noticed (*.bak_doorgate: 5 s).
    constexpr uint32 SIGHT_HOLD_RECHECK_MS = 2500;

    constexpr uint32 SIGHT_HOLD_LOG_MS = 10 * IN_MILLISECONDS;

    // ROUTE_BOSS_PULL_IMMEDIATE: a boss the group stands ready for (GroupFullyReady, the boss buffs done, no
    // crowd control hold of the tank's) is body-pulled after this, not after PULL_WAIT_MS: the tank's careful
    // planner never picks a boss ("[pull hold] no aggressive target"), the wait was always spent for nothing.
    constexpr uint32 BOSS_PULL_READY_MS = 3 * IN_MILLISECONDS;

    // [pull_option] The tank's two ways to pull a pack, cheapest first (user 2026-10-25, MgT heroic Selin room):
    // (1) the sight pull - a spot on the arc round the pack (MovementAction::SightArcCandidates, the sight-arc
    // search the ranged / healers use) that sees it within the pull spell's range and stands outside every idle
    // mob's real aggro distance (GetAggroRange + combat distance + SIGHT_IDLE_AGGRO_MARGIN), with a walk there
    // that stays out of them too; (2) the body pull - the walk into the pack. For both the mobs they touch are
    // counted: every idle mob whose real aggro distance the walk or the spot enters (and that sees it), the pack
    // itself, and their links (formation, the core's call for assistance: same faction within the assistance
    // radius, in sight) - a boss among them is a boss pull nobody chose (Selin and her Wretched on AzerothCore).
    constexpr float PULL_SIGHT_RANGE = 30.0f;          // Hand of Reckoning, Avenger's Shield, Shoot / Throw

    constexpr float PULL_SIGHT_RANGE_SLACK = 1.5f;     // the spot stands this far inside it

    constexpr uint32 PULL_SIGHT_MAX_LOS = 120;         // target sight tests of the arc candidates

    constexpr uint32 PULL_SIGHT_MAX_PATHS = 8;         // candidates that get a path

    constexpr float PULL_SIGHT_WALK_RATIO = 2.5f;      // a walk longer than this x straight + extra is a detour

    constexpr float PULL_SIGHT_WALK_EXTRA = 25.0f;

    constexpr float PULL_SIGHT_PATH_END = 2.0f;        // the walk ends this close to its spot

    constexpr float PULL_TOUCH_STEP = 1.5f;            // the walk is tested every this many yards

    constexpr float PULL_IDLE_SCAN = 60.0f;            // idle mobs looked at, round the leader and the pack

    constexpr uint32 PULL_OPTION_MOB_CAP = 6;          // a pull that touches more is "over the cap"

    struct PullTouch
    {
        std::vector<Creature*> mobs;  // every mob the option brings
        uint32 extra = 0;             // of them, outside the intended pack (and its links)
        bool boss = false;            // a boss among them
        std::string bossName;
        std::string firstExtra;       // the first one outside the intended pack (for the log)
    };

    // The idle hostiles (IdleAggroCheck) within PULL_IDLE_SCAN of the leader or of the pack's mob.
    std::vector<Creature*> PullIdleMobs(Player* leader, Creature* target);

    // The pack itself and its links: the target's pack (PACK_RADIUS), its formation, and what answers their
    // call for assistance (CanAnswerAssist with the faction test, within the assistance radius of a touched mob
    // plus ASSIST_LINK_MARGIN, in its sight), grown until nothing new comes.
    // build 27 (20261025-150737-mgt-h-5A-14): the assistance test had ASSIST_LINK_MARGIN on top (13 yd + both
    // sizes) and linked Selin's side packs to the centre pair (15.1 yd apart; the core's 10 yd + sizes = 13.3 does
    // not), so the "own pack" grew to the pair and every arc spot was "inside the pack's aggro". The core's own
    // radius now (AnyAssistCreatureInRangeCheck: IsWithinDistInMap, sizes included), and the own pack (`assist`
    // false: pack radius and formation) is told apart from the links it may call (`assist` true).
    void PullTouchClosure(Player* leader, std::vector<Creature*> const& idle, std::vector<Creature*>& touched,
                          bool withAssist = true);

    // The mobs a pull of `target` brings when the leader walks `walk` (the spot at its end): the target and its
    // links, every idle mob whose real aggro distance (+ SIGHT_IDLE_AGGRO_MARGIN) a walk point enters and that
    // sees the point (AggroesAtPoint: the core's Z gate, 3D range, the eye line), and their links. `intended`
    // gets the target with its links alone; `isBoss` tells a boss.
    PullTouch PullTouches(Player* leader, Creature* target, Movement::PointsArray const& walk,
                          std::vector<Creature*> const& idle, std::function<bool(Creature*)> const& isBoss,
                          std::vector<Creature*>& intended);

    struct PullSightCandidate
    {
        G3D::Vector3 at;
        float straight = 0.0f;
        float radius = 0.0f;
        float angleOffDeg = 0.0f;
        float walk = -1.0f;  // the path's length (-1: no path searched / none)
        std::string verdict;  // "ok" or why not
        PullTouch touch;
    };

    struct PullSightSearch
    {
        bool found = false;
        PullSightCandidate best;           // the chosen sight spot (found)
        bool hasFallback = false;          // a pathable spot that sees the pack but touches more
        PullSightCandidate fallback;       // the least dangerous of them (no boss first, then the fewest mobs)
        uint32 candidates = 0;             // arc spots on the floor that see the pack
        uint32 inOtherAggro = 0;
        uint32 inOwnAggro = 0;
        uint32 noPath = 0;
        uint32 touchesMore = 0;
        uint32 touchesBoss = 0;
        float minRadius = 0.0f;
        float maxRadius = 0.0f;
        float margin = 0.0f;
        std::vector<PullSightCandidate> top;  // the first three looked at, with their verdicts
    };

    bool PullTouchBetter(PullTouch const& a, PullTouch const& b);

    // [pull_option] (1) the sight pull spot for `target` (see above). The candidates nearest to the leader first;
    // of the clean ones (the spot and the walk touch the intended pack alone, no boss) the shortest walk.
    // A tight second pass: the core's aggro test is an exact 3D distance (Creature::CanStartAttack), so half a yard
    // is still safe; Selin's room on heroic (real aggro 21) has its side-pack spots only there (PATCH_selin.md).
    constexpr float PULL_SIGHT_TIGHT_MARGIN = 0.5f;

    constexpr size_t PULL_SIGHT_MAX_CENTRES = 6;  // pack members the arc is searched round

    void FindPullSightSpotWith(Player* leader, Creature* target, std::vector<Creature*> const& idle,
                               std::function<bool(Creature*)> const& isBoss, float margin, PullSightSearch& out);

    // The usual margin first, then the tight one; the fallback (a spot that touches more) of either pass.
    void FindPullSightSpot(Player* leader, Creature* target, std::vector<Creature*> const& idle,
                           std::function<bool(Creature*)> const& isBoss, PullSightSearch& out);

    // The JSON of a search's candidates (sight_point_search / pull_option events).
    std::string PullCandidatesJson(std::vector<PullSightCandidate> const& top);

    // [b33_mech2] A boss's body pull waits for his killable area trash (BossAreaBodyPullHold) at most this long in
    // all, from its first hold; then it goes, logged "body pull: given up" (no deadlock on trash nothing pulls).
    constexpr uint32 BOSS_AREA_BODY_PULL_HOLD_MS = 4 * MINUTE * IN_MILLISECONDS;

    extern std::map<std::pair<uint32, ObjectGuid>, uint32> bossAreaBodyPullHoldMs;  // {party seq, boss} -> first hold

    // The next boss of the party's last route (Retarget's RouteSnapshot), nullptr when none is alive.
    Creature* RouteNextBoss(uint32 seq, Map* map);

    // [b31_ubrs_door] Party::seq -> since when the leader's walk out of a closed area (AreaGateDef exit) holds at a
    // door that stays shut for good (world thread only).
    extern std::unordered_map<uint32, uint32> lockedInSince;

    constexpr uint32 LOCKED_IN_FAIL_MS = 2 * MINUTE * IN_MILLISECONDS;

    // [dmw-guard-body-pull] A living guard of a guarded door that is still closed (PendingDoorGuards).
    bool IsPendingDoorGuard(Map* map, Creature const* creature);

    // The tank picks its next pull itself (AggressiveTargetValue: the nearest hostile in sight within 40 yd). A
    // mob of a closed part is no pull for it either: such mobs near the tank are marked as careful-pull skips
    // every tick (CarefulPullSkip keeps an entry 60 s, at most 32 per tank). Blackrock Spire, LBRS waves
    // 2026-10-24: every target was LBRS trash, yet the tank walked from the Legionnaires' room into the UBRS
    // Dragonspire Hall after a Blackhand niche it saw through the open Dragonspine Door.
    constexpr float CLOSED_AREA_SKIP_RANGE = 60.0f;

    constexpr size_t CLOSED_AREA_SKIP_MAX = 16;

    struct ClosedAreaMobCheck
    {
        Player* tank;
        std::vector<AreaGateDef const*> const& gates;

        bool operator()(Creature* creature) const
        {
            return creature->IsAlive() && !creature->IsInCombat() && creature->IsHostileTo(tank) &&
                   InAnyAreaGate(gates, creature->GetPositionX(), creature->GetPositionY(),
                                 creature->GetPositionZ());
        }
    };

    void KeepTankOutOfClosedAreas(Player* tank, std::vector<AreaGateDef const*> const& gates);

    // [queue-pull-hold-not-stuck] ROUTE_NO_WALK_IN_PULL withholds the leader's waypoint while its own pull runs;
    // the steering / drive watchdogs then saw "own walk made no progress" although the harness itself gave no
    // walk: bot_cannot_move "route" every ~40 s and a steer_stuck "route end" skip of the target every 20 s
    // (brs-h 20261025-115805-brs-h-5A-32: the tank at (64.2, -441.6) from 303 s to 527 s, "pull start" 106
    // executed every tick, 83 line-of-sight pull plans on the Spellbinder / Warlock packs never cast, 10 route
    // targets skipped as "route end" from a spot it never left; fos-n 123018-fos-n-5A-84: "Soul Horror out of pull
    // range" 310 times, 3x bot_cannot_move at (5138.4, 2129.0); bfd-h 115805-bfd-h-5A-23 Rtpalad 2880 s). A
    // pull hold is no route stall for PULL_HOLD_STALL_MS; past it the hold is the pull planner's deadlock: logged
    // once (pull_hold_stall, with the pull's state), the waypoint is no longer withheld for PULL_HOLD_RELEASE_MS
    // and the steer clock runs again.
    constexpr uint32 PULL_HOLD_STALL_MS = 60 * IN_MILLISECONDS;

    constexpr uint32 PULL_HOLD_RELEASE_MS = 30 * IN_MILLISECONDS;

    // The bots' own raid-leader pull hold (LeaderValues PullHoldValue) holds the leader now.
    bool LeaderPullHeld(Player* leader);

    bool IsRouteBlocker(LeaderGroup& group, ObjectGuid guid);

    bool RouteBlockerBehindRequired(LeaderGroup& group, Map* map, WorldObject const* behind,
                                    std::string& why);

    // [queue-blocker-pull-gate] WIPE_ANALYSIS_w24_25: scholo-n w24 walked the tank to 17 yd of a 20 yd aggro radius
    // into a pack the planner had REFUSED ("refused, 97 mobs") once the edge hold "gave way" after 8 s; the body pull
    // and the blocker retarget had their own, different checks. One gate now:
    // - the planner's chain count did not refuse the pack as hopeless (CarefulPlanVerdict::GetRefusal: its smallest
    //   pull is over the over-cap scale - "no safe pull: X brings N mobs, over M");
    // - the group is ready (GroupFullyReady: everyone here, health and healer mana up) - else hold, no skip;
    // - a body pull only for a blocker on the only way to a boss / event / required target (IsRouteBlocker, or
    //   RouteBlockerBehindRequired - [boss_area_clear] trash included).
    // The sight pull ([pull_option] (1), FindPullSightSpot) runs before any of these and is not gated here.
    std::string BlockerPullGate(LeaderGroup& group, Player* leader, Creature* blocker,
                                WorldObject const* behind, bool bodyPull, bool& hold);

    bool TargetRouteBlocker(LeaderGroup& group, Player* leader, Creature* blocker, WorldObject const* behind,
                            std::string const& why);

    void RouteBlockerBodyPull(LeaderGroup& group, Player* leader, Creature* target, std::string const& why);

    void LogHarnessHold(LeaderGroup& group, Player* leader, Creature* blocker, char const* walk, float x,
                        float y, float z);

    // [lead5] the tank's view of `unit` (AggressiveTargetValue::Explain) with the leader's own route target
    std::string PartyTankView(LeaderGroup& group, Player* leader, Unit* unit);

    PlanVerdict TankPlanVerdict(LeaderGroup& group, Player* leader, std::vector<Creature*> const& pack);

    // The idle mobs of the target's pack and of the blocker's (PackMobCheck, PACK_RADIUS around each), bosses
    // left out (their body pull is the boss pull): what approach to sight keeps out of while no plan is accepted.
    std::vector<Creature*> SightKeepOutPack(LeaderGroup& group, Player* leader, Creature* target, Creature* blocker);

    // [lead5] the leader planner's aoe_pull_plan (its own memory, Decision::aoePlan) and the leader's required
    // marks it hands the planner (LeaderInputs::required), else the AoePull registry
    bool PartyAoePlanned(LeaderGroup& group, ObjectGuid mob, uint32* sinceMs = nullptr);

    // the route walk of the group's tank: its stops, holds, pulls and skips, and the walk on to the route target
    void Steer(LeaderGroup& group, Player* leader);
}

#endif
