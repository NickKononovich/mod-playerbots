/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_ROUTEPATROL_H
#define PLAYERBOTS_LEADER_ROUTEPATROL_H

// [lead107] user: production code does not live in the test harness. The leader's patrol decision - the patrol
// hold and lane waits, the step back out of a patrol's way, the intercept / patrol pull (LeaderPatrolTick) and the
// facts they share - moved out of TestHarnessRunner.cpp unchanged; the harness calls it through the group view.

#include "Creature.h"
#include "LeaderGroup.h"
#include "G3D/Vector3.h"
#include "G3D/Vector4.h"
#include "GameObject.h"
#include "MoveSplineInitArgs.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Position.h"
#include "LeaderKnowledge.h"
#include "RouteSearch.h"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct WaypointPath;  // WaypointDefines.h

namespace Leader::Travel
{
    // Idle hostile mobs (ROUTE_SKIP_BEHIND_SKIPPED's shadow).
    struct ShadowMobCheck
    {
        Player* leader;
        bool operator()(Creature* creature) const
        {
            return creature->IsAlive() && !creature->IsInCombat() && !creature->IsCritter() &&
                   !creature->IsCivilian() && creature->IsHostileTo(leader);
        }
    };

    constexpr float APPROACH_AGGRO_MARGIN = 5.0f;

    // A wanderer (RANDOM_MOTION_TYPE) walks its wander radius towards the group standing at its edge: added to
    // its reach, at most this much (Stratholme: the street cadavers came from 24 to 15.6 yd of a standing leader).
    constexpr float APPROACH_WANDER_MAX = 8.0f;

    // The followers walk behind and beside the leader, not on its eye line: a mob on the leader's floor stops the
    // walk at its real aggro range plus this spread even when it does not see the route (Stockade wave 40,
    // 17:09:59: the stop lay 12.2 yd from a Defias Captive in a cell - real aggro 19, no line of sight through
    // the cell wall - and the shaman following through the open cell door pulled it from 9.6 yd).
    constexpr float APPROACH_FOLLOW_SPREAD = 5.0f;

    // Creature::MoveInLineOfSight aggroes within GetAggroRange + m_CombatDistance (3D, no object sizes).
    float AggroReach(Creature* mob, Player* leader);

    // [patrol_wait_spot] Where the group waits for a patrol (the planner's "patrol wait"): beyond the reach (aggro
    // reach + the creature assistance radius) of every idle mob that is no patrol itself, stepped back from the
    // nearest such mob. Drak'Tharon heroic 2026-10-25 (064751-dtk-h, ~280 s): the group waited for the Wretched
    // Belcher next to the idle Ghoul Tormentor pack (-322.7,-647.6), the patrol fight there called the pack in.
    // false: the leader's spot is clear already, or no clear spot within PATROL_WAIT_STEP_MAX.
    constexpr float PATROL_WAIT_SCAN = 60.0f;

    constexpr float PATROL_WAIT_ASSIST = 10.0f;  // CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS (default 10)

    constexpr float PATROL_WAIT_TRAIL_MAX = 80.0f;   // [patrol_wait_trail_back] at most this far back the way in

    constexpr float PATROL_WAIT_TRAIL_FLOOR = 8.0f;  // a trail point this far above / below is another floor

    // The line of sight the tank's "possible targets" test (NearestUnitsValue: Player::IsWithinLOSInMap) and the
    // mob's aggro test (Creature::MoveInLineOfSight: the same line from the other end) would see with the leader
    // standing at the point: from its eyes (point + its collision height) to the mob's hit sphere. Wave 13: the old
    // check (mob->IsWithinLOS(point + 2 yd), which adds the MOB's collision height on top) cleared the crates and
    // railings a player's eye line hits - the leader stopped where the tank saw nothing (SM Armory: Scarlet
    // Guardsman 22.5 yd, possible targets empty; Arcatraz: Sentinel 24.7 yd), and the run stalled.
    bool SeesFromPoint(Player* leader, Creature* mob, G3D::Vector3 const& point);

    // The core's proximity aggro of an idle mob on the leader standing at a route point, as every approach and
    // route test of the harness asks it (wave 44, RFC 5B-1 20:30:02: a Searing Blade Cultist at z -8.2 "blocks
    // route" 20.1 yd from the backbone while the group walked at z -22, 14 yd below it). Per point:
    // MovementAction::IdleAggroBlocker (Creature::CanStartAttack - the Z gate, then the 3D range), the range
    // being `reach` (the harness's own reach with its margin: RouteReach, AggroReach, ...), then the harness's
    // eye-line sight from the point (SeesFromPoint) when `checkLos`. zGated: the point lay inside the range and
    // only the Z gate kept the mob idle.
    bool AggroesAtPoint(Creature* mob, Player* leader, G3D::Vector3 const& point, float reach, bool checkLos,
                        bool* zGated = nullptr);

    // [lead5] LEADER_ONE_PICKER: Steer reads the leader planner's own verdict memory (Party::lead, written by
    // TickLeaderPlanner from the look's Decision), not the CarefulPlanVerdict registry; the same semantics (one
    // latest verdict, its target or any mob of its pack, fresh LEADER_VERDICT_TTL_MS).
    constexpr uint32 LEADER_VERDICT_TTL_MS = 10 * IN_MILLISECONDS;  // CarefulPlanVerdict's TTL

    // The path a patrol walks: its own (creature_addon path, or the one its SmartAI started), else its formation
    // leader's, which it follows - the lookup of ChooseTargetActions' PatrolPath (FindPatrolRoute), uncached:
    // Retarget runs every few seconds on the world thread.
    WaypointPath const* PatrolWalkPath(Creature* creature);

    // [b47_patrol] PATROL_WAIT_BUDGET: the waits for one patrol (ROUTE_PATROL_HOLD "it comes to the group" and
    // PATROL_STEP_BACK) add up to at most PATROL_WAIT_BUDGET_MS, or PATROL_WAIT_BUDGET_STARTS hold / step-back
    // starts; then the patrol is handed to the tank's planner as the route target (TestHarnessMgr::PatrolStepBack:
    // the careful pull decides, a pull back / los pull to the cleared spot, or the meet) and neither waits for it
    // for PATROL_HANDOVER_MS. sm-arm-n 20261026-104908-sm-arm-n-5B-19 (759-1147 s, build 46, stopped): patrol_hold
    // "patrol Scarlet Evoker ... it comes to the group" ~17 s, then patrol_step_back "it walks beside idle Scarlet
    // Defender, out of its walk, 30 yd back" ~6-18 s, "walked on", and again, 16 times; the Evoker was never
    // pulled.




    struct PatrolWaitBudget
    {
        uint32 spentMs = 0;
        uint32 lastMs = 0;
        uint8 starts = 0;
        uint32 handedUntilMs = 0;
    };

    // per instance, per patrol (world thread); the patrol handed over and not taken yet, per instance
    extern std::unordered_map<uint32, std::map<ObjectGuid, PatrolWaitBudget>> patrolWaitBudgets;

    extern std::unordered_map<uint32, ObjectGuid> patrolHandOvers;

    bool PatrolHandedOver(uint32 instanceId, ObjectGuid patrol, uint32 now);

    // One tick of waiting for `patrol` (start: a hold or step-back begins). True when its budget is spent: the
    // patrol is handed over now (patrolHandOvers) or already was.
    bool ChargePatrolWait(uint32 instanceId, ObjectGuid patrol, uint32 now, bool start);

    // [b43_harn] ROUTE_PATROL_HOLD: no route walk while a patrol comes to the group. hos-h
    // 20261026-071042-hos-h-5A-21 (283-289 s): the planner had logged "[pull patrol] Rtpalaj: patrol Dark Rune
    // Theurgist comes to the group (22.1 yd, eta 0 s): pulls it first" and planned a los pull on it, yet the
    // harness's "test runner move" walked the tank 11 yd toward that patrol (the route target, "patrol crosses
    // route"); the los pull was dropped, 7-8 mobs came, three deaths. The leader holds while an idle patrol
    // (PatrolWalkPath) within PATROL_HOLD_SCAN of it either has the tank's fresh accepted verdict
    // (CarefulPlanVerdict::Get: the planner pulls it now) or is within its aggro reach + PATROL_HOLD_MARGIN of a
    // member (it reaches the group in a moment, the tank's planner meets it where the group stands). At most
    // PATROL_HOLD_MAX_MS per hold, then PATROL_HOLD_COOLDOWN_MS without it (a patrol circling the group).
    constexpr bool ROUTE_PATROL_HOLD = true;

    constexpr float PATROL_HOLD_SCAN = 45.0f;

    constexpr float PATROL_HOLD_MARGIN = 12.0f;  // the walk of a harness tick plus the patrol's own walk



    // [b48_scholo] scholo-h 20261026-111724-scholo-h-5C-4 (378-398 s, wipe 443-453 s): the hold "patrol Necrofiend
    // 37.1 yd from ... (reach 38.0): it comes to the group" ended at 391 s ("done", no timeout) because the
    // Necrofiend stood at the far end of its walk (280.9,92.8), 38.1 yd off; in that one gap the harness walked the
    // tank Rtdekaa to its approach stop (267.5,74.9) by the refused Scholomance Necrolyte ("test runner move"
    // 392 s). The hold came back at 393 s, but ClearWaypoint does not stop a walk already under way: the tank
    // went on 12 yd more, the group followed into the Necrofiend's walk (y 91-93, 17 yd off; its reach 22.5),
    // and it took the priest at 398 s ("unplanned: Rtpriab"), next to the Dark Summoner pack over the Aberration
    // pit. Now:
    // - PATROL_HOLD_LINGER_MS: a hold lasts this long after its patrol walked out of the hold's reach while the
    //   patrol is still near (its turn at the end of the walk).
    // - PATROL_LANE_HOLD: the harness walk (the leader's waypoint) that takes the leader nearer than its aggro
    //   reach to the patrol's walk (PatrolWalkPath, every leg on the walk's floor, in sight), nearer than it
    //   stands now, holds too, whatever the patrol's position. Not subject to the cooldown or PATROL_HOLD_MAX_MS:
    //   the PATROL_WAIT_BUDGET hands the patrol to the tank's planner after 60 s.
    // - HaltLeaderWalk: a hold stops the leader's walk to the harness waypoint under way.

    constexpr bool PATROL_LANE_HOLD = true;

    constexpr float PATROL_LANE_DEEPER = 1.0f;     // the walk must come this much nearer to the walk than now

    constexpr float PATROL_LANE_SAMPLE = 2.0f;     // the walk's line sampled this often

    constexpr float HALT_WALK_SAME_DEST = 3.0f;    // the spline's end this near the waypoint: the harness walk

    struct PatrolHold
    {
        uint32 sinceMs = 0;
        uint32 cooldownUntilMs = 0;
        bool logged = false;
        bool timedOut = false;
        std::string why;
        // [b48_scholo] the hold's patrol, its last match, its kind; a lane hold's walk (and the route target then)
        ObjectGuid patrol;
        uint32 lastMatchMs = 0;
        bool comes = false;
        bool lane = false;
        bool lingerLogged = false;
        bool wander = false;  // [lead1600] a wanderer's area reaches a member (no linger: a fact of where it stands)
        G3D::Vector3 laneTo;
        ObjectGuid laneTarget;
    };

    extern std::unordered_map<uint32, PatrolHold> patrolHolds;  // per instance (Steer, world thread)

    float PointSegmentDistance2d(G3D::Vector2 const& point, G3D::Vector2 const& from, G3D::Vector2 const& to);

    // [b48_scholo] PATROL_LANE_HOLD: the leader's walk from where it stands to `to`, sampled every
    // PATROL_LANE_SAMPLE yd, against every leg of the patrol's walk (PatrolWalkPath, closed, the legs within
    // PATROL_WAIT_TRAIL_FLOOR of the sample's height). walkGap: the walk's nearest 2D approach to it; nowGap: the
    // leader's own. True when the walk comes within `reach` of the walk, PATROL_LANE_DEEPER nearer than the leader
    // stands now, and the patrol on its walk sees that sample (its eyes to the leader's).
    bool WalkEntersPatrolLane(Player* leader, Creature* patrol, G3D::Vector3 const& to, float reach,
                              float& walkGap, float& nowGap);

    // [b48_scholo] A hold (ROUTE_PATROL_HOLD, ROUTE_REST_HOLD) clears the leader's waypoint, which stops no walk
    // already under way: the leader's "test runner move" spline still ending at that waypoint (not a jump, a
    // link's straight walk or a gate pass) is stopped here. True: stopped; `to` is where it went.
    bool HaltLeaderWalk(LeaderGroup& group, Player* leader, G3D::Vector3& to);

    // [b46_patrol] PATROL_STEP_BACK: a patrol walks in (PatrolHoldWhy's "comes to the group") while a member rests
    // or wants to, or while it walks beside an idle pack: the group steps back along the tank's own trail (the way
    // in, cleared) instead of standing in its way. strat-h 20261026-093620-strat-h-5E-2 (1520-1571 s): after a
    // 13-kill pull the patrol hold waited for the Patchwork Horror "it comes to the group" (26.1 yd) and the group
    // sat down to drink; the Horror aggroed on the rogue 21.7 yd from the idle Plague Ghoul of the 16-30 mob
    // Skeletal Berserker chain, the tank would not walk there ("[tank reach] ... aggro reach of idle Plague
    // Ghoul"), the Ghoul, then its chain's Skeletal Guardians / Berserkers / Cadavers joined ("[far engage]" at
    // 43 yd), three died. The spot: the first trail point PATROL_STEP_BACK_MIN or more back, on the same floor,
    // out of the patrol's reach (aggro reach + PATROL_STEP_BACK_SPREAD) of its position and of every leg of its
    // walk, and out of every idle non-patrol mob's reach + assistance ("out of its walk"); else the first one where
    // a patrol that reaches the group still fights clear of those packs ("where it meets the group clear of the
    // packs", the tank's planner pulls it there). false: no such spot within PATROL_WAIT_TRAIL_MAX.
    constexpr bool PATROL_STEP_BACK = true;

    constexpr float PATROL_STEP_BACK_SPREAD = 10.0f;  // the followers' spread round the leader plus a margin

    constexpr float PATROL_STEP_BACK_MIN = 4.0f;      // a trail point closer than this is no step

    constexpr float PATROL_STEP_BACK_ARRIVED = 3.0f;

    constexpr float PATROL_STEP_BACK_GONE = 10.0f;    // the patrol this far past its reach of the spot walked on




    struct PatrolStepBackState
    {
        ObjectGuid patrol;
        G3D::Vector3 spot;
        float reach = 0.0f;
        uint32 sinceMs = 0;
        uint32 cooldownUntilMs = 0;
        uint32 noSpotMs = 0;
        bool arrived = false;
        std::string how;
        std::vector<G3D::Vector3> hops;  // [b48_patrol] the trail walked back, hop by hop, the spot last
        size_t hop = 0;
        bool wander = false;  // [lead1600] the step back out of a refused wanderer's area (the leader's own)
    };

    extern std::unordered_map<uint32, PatrolStepBackState> patrolStepBacks;  // per instance (world thread)

    // An idle mob that is no patrol (no walk path of its own or of its formation leader) and not `patrol`'s.
    bool StaticPackMob(Creature* mob, Creature* patrol);

    // [b48_patrol] ohb-h 20261026-111724-ohb-h-5C-16 (653-720 s, wipe): the step back to (2122.5,124.4,64.7), 51 yd
    // along the way in, was a plain harness walk to that point; its path dipped to z 56-57 into the yard where the
    // Durnholde Sentry / Tracking Hound patrol walked up ("[follow path] ... would wake Durnholde Tracking Hound
    // (11.2 yd of reach 13.7)"), the mage following the tank was hit ("[defend member] Durnholde Sentry hits
    // Rtmagag") and the Sentry's flight for help brought the yard's Riflemen / Warden ("[far engage]" at dz 12):
    // five deaths. Only the spot was checked, never the walk. Now the walk back is the trail itself (hops of
    // PATROL_STEP_BACK_HOP yd, walked one by one), and every trail point up to the spot must be clear as a route
    // leg is for the followers: out of every idle static mob's aggro reach + assistance + APPROACH_FOLLOW_SPREAD
    // (AggroesAtPoint, the core's Z gate), out of the patrol's reach of where it stands, on the leader's floor.
    // The first point that is not ends the search (the way back runs through it).
    constexpr float PATROL_STEP_BACK_HOP = 6.0f;

    bool PatrolStepBackSpot(Player* leader, Creature* patrol, float reach, G3D::Vector3& spot, std::string& how,
                            std::vector<G3D::Vector3>& hops);

    bool LeaderPicksFor(Leader::LeaderGroup& group);

    bool LeaderVerdictCovers(Leader::LeaderGroup& group, ObjectGuid mob);

    bool PartyVerdict(Leader::LeaderGroup& group, ObjectGuid mob, bool& accepted, uint32& mobs);

    // [lead4] LEADER_ONE_PATROL_OWNER: the leader owns the patrol decision (LeaderPatrolTick); the harness's
    // patrol hold is its forecast only (no wait budget, no handover, no hold clock or cooldown).
    bool LeaderOwnsPatrols(Leader::LeaderGroup& group);

    // The patrol that holds the walk, with why ("planner pulls it" / "<yd> from <member>"); empty: no hold.
    std::string PatrolHoldWhy(Leader::LeaderGroup& group, Player* leader);

    // [lead4] the patrol decision of the leader
    bool PatrolStepBack(Leader::LeaderGroup& group, Player* leader);

    // [lead4] the patrol decision of the leader
    bool LeaderPatrolTick(Leader::LeaderGroup& group, Player* leader);
}

#endif
