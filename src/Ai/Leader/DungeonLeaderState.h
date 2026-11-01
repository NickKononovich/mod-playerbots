/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_DUNGEONLEADERSTATE_H
#define PLAYERBOTS_LEADER_DUNGEONLEADERSTATE_H

// [lead107] user: production code does not live in the test harness - the dungeon leader's own decision state, moved
// out of TestHarnessMgr.h unchanged (the harness's Party holds one as `lead`; one per group leader).

#include "G3D/Vector3.h"
#include "HazardZone.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace PullPlanner
{
    struct Decision;  // ChooseTargetActions.h
}

namespace Leader
{
    // [lead3] .. [lead10] C:/WoW/LEADER_REFACTOR_PLAN.md section 9: the dungeon leader's own decision state (the
    // harness is the leader, the PullPlanner its library). World thread only.
    struct DungeonLeaderState
    {
        // [lead3] the pack the leader must clear, asked of its planner whole (the old harness body pull's
        // domain); `required`: no option for it stops the run (user decision 1)
        ObjectGuid wholePack;
        bool walkIn = false;
        bool required = false;
        uint32 wholeSinceMs = 0;
        uint32 wholeSeenMs = 0;
        std::string wholeWhy;
        // [lead3] the boss pull the leader issues at the tank's next ask
        ObjectGuid bossPull;
        uint32 bossPullMs = 0;
        std::string bossPullWhy;
        ObjectGuid bossPullIssued;  // the boss pull issued last, when (its "not pulled" end)
        uint32 bossPullIssuedMs = 0;
        // [lead4] the patrol the leader decided to pull (an intercept), and the declared wait
        ObjectGuid intercept;
        uint32 interceptMs = 0;
        uint32 interceptSinceMs = 0;
        // [lead12] patrols the planner cannot pull (a hopeless intercept): no intercept of them for a while; the
        // route crosses their lane while they are far from the leader (timed against the loop), never into them
        std::map<ObjectGuid, uint32> interceptBanned;
        std::string interceptWhy;
        ObjectGuid waitPatrol;
        uint32 waitSinceMs = 0;
        std::string waitWhy;
        // [lead4c] one wait per patrol across the forecast's flicker (a gap under LEADER_PATROL_WAIT_GAP_MS), and
        // the lane wait's own bound (LEADER_LANE_WAIT_MAX_MS: then the patrol is pulled)
        ObjectGuid lastWaitPatrol;
        uint32 lastWaitEndMs = 0;
        ObjectGuid laneWaitPatrol;
        uint32 laneWaitSinceMs = 0;
        uint32 laneWaitSeenMs = 0;
        // [lead5] the leader planner's last verdict (CarefulPlanVerdict's data, kept by the leader itself)
        uint32 verdictMs = 0;
        ObjectGuid verdictTarget;
        std::vector<ObjectGuid> verdictPack;
        bool verdictAccepted = false;
        uint32 verdictMobs = 0;
        bool verdictHopeless = false;
        uint32 verdictWaitMs = 0;
        std::string verdictWhy;
        // [lead5] its aoe_pull_plan (the old AoePull::NotePlan), and the packs the leader marked required
        std::vector<ObjectGuid> aoePlan;
        uint32 aoePlanMs = 0;
        uint32 aoePlanSinceMs = 0;
        std::map<ObjectGuid, uint32> requiredMarks;
        // [lead6] the leader's one readiness verdict, this tick; the "held for now" a ready group stands in
        bool ready = false;
        std::string notReadyWhy;
        uint32 readyCheckMs = 0;
        // [lead13] the leader's last look (it keeps a decision ready while the tank stands without a target), and
        // since when every member stands above its own rest lines (the boss pull's old [ready_hold_30s] rule)
        uint32 lastLookMs = 0;
        uint32 atLinesSinceMs = 0;
        // [lead14] the leader's last issued pull (the route view's red line): its mob, kind and stand spot
        ObjectGuid issuedMob;
        std::string issuedKind;
        Position issuedSpot;
        uint32 issuedMs = 0;
        // [lead15] the member events: the pull's assigned spots (per member), the gate's progress per member, the
        // event tick and the member wakes it caused
        std::map<ObjectGuid, Position> spots;
        std::map<ObjectGuid, std::pair<float, uint32>> spotBest;
        std::set<ObjectGuid> spotStuck;
        ObjectGuid spotsFor;
        Position spotsFight;
        uint32 spotsSinceMs = 0;
        bool spotsWaiting = false;
        uint32 spotsIssuedMs = 0;
        uint32 spotsLogMs = 0;
        uint32 eventTickMs = 0;
        uint32 wakeCount = 0;
        uint32 wakeTotal = 0;
        uint32 wakeLogMs = 0;
        std::set<ObjectGuid> following;               // walked along with the tank (the member's follow is off)
        Position tankStillPos;
        uint32 tankStillSinceMs = 0;
        uint32 healReachMs = 0;
        std::map<ObjectGuid, uint32> healReachLogMs;  // heal_reach records per member
        // [lead120] [lead121] the heal-reach decision's facts: the tank's fight anchor (fightAnchorSerial) and the
        // members' assigned spots (none: no MoveTo / StandAt) it was judged against; the spot the healer was sent from
        std::map<ObjectGuid, std::optional<Position>> healReachSpots;
        uint32 healReachAnchor = 0;
        std::optional<Position> healReachFrom;
        // [lead122] a ranged member's spot in range and sight of its attack target (target, spot), for the fight
        std::map<ObjectGuid, std::pair<ObjectGuid, Position>> rangeSpots;
        std::map<ObjectGuid, ObjectGuid> kickers;     // casting mob -> its assigned interrupter
        // [lead15] the cadence: ticks per state (calm / pre-pull / combat / hazard) and the layers' cost
        uint32 cadenceTicks[4] = {};
        uint64 cadenceFastUs = 0;
        uint64 cadenceSlowUs = 0;
        uint64 cadenceMaxUs = 0;
        uint32 slowUsTick = 0;
        bool hazardActive = false;
        // [lead15] the AoE rule's cache (the grid visit is the slow layer)
        ObjectGuid aoeTarget;
        uint32 aoeMs = 0;
        uint8 aoeRule = 0;
        std::string aoeWhy;
        // [lead15] mobs loose on a member: mob -> {victim, since}
        std::map<ObjectGuid, std::pair<ObjectGuid, uint32>> looseMobs;
        // [lead15] own_decision counts: member -> action -> count (the run report's work list)
        std::map<std::string, std::map<std::string, uint32>> ownDecisions;
        // [lead16] the hazard view per member, the members held out of a hazard, the combat spots
        std::map<ObjectGuid, std::vector<HazardZone>> hazardZones;
        uint32 hazardScanMs = 0;
        std::map<ObjectGuid, Position> hazardSpots;
        std::map<ObjectGuid, std::pair<ObjectGuid, Position>> combatSpots;  // member -> {target, spot}
        std::map<ObjectGuid, uint32> combatSpotLosMs;
        std::map<ObjectGuid, ObjectGuid> ccAssign;  // [lead16] mob -> the member that crowd-controls it
        ObjectGuid looter;          // [lead16] the member told to loot after the last fight, and since when
        uint32 lootSinceMs = 0;
        bool lastFighting = false;
        uint32 fightLenStartMs = 0;    // [lead88] the fight's start and its estimated length (-1: none)
        float fightLenEstSec = -1.0f;
        std::map<std::string, uint32> staleCommands;  // [lead16] member -> stale_command ticks
        ObjectGuid bossPrepareFor;                     // [lead16] the boss pull the tank prepares for
        std::set<ObjectGuid> trailing;                 // [lead17] members the leader waits for (far behind)
        std::map<ObjectGuid, uint32> spotReassigns;    // [lead17] pull spots re-assigned per member
        std::map<std::string, uint32> blockedMoves;    // [lead17b] "<kind>|<action>" -> blocked (10 s windows)
        // [lead18] the committed pull (LEADER_COMMIT_PULL)
        std::shared_ptr<PullPlanner::Decision> commit;
        ObjectGuid commitMob;
        Position commitPos;
        Position commitSpot;
        uint32 commitSinceMs = 0;
        uint32 commitIssuedMs = 0;
        bool commitIssued = false;
        bool commitOrderSeen = false;
        bool commitMoving = false;
        std::string commitWhy;
        bool patrolWindowOpen = true;
        std::string roomSig;  // [lead18] the last room_order logged
        // [lead18] the fight's anchor (LEADER_ANCHORED_SPOTS) and each spot's anchor serial
        Position fightAnchor;
        uint32 fightAnchorSerial = 0;
        std::map<ObjectGuid, uint32> combatSpotAnchor;
        // [lead18] the spot guard: cleared ground, idle mobs, the last checked spot per member, reject log
        std::vector<Position> clearedGround;
        std::map<ObjectGuid, Position> clearedLast;
        std::vector<ObjectGuid> idleGuids;
        uint32 idleScanMs = 0;
        std::map<ObjectGuid, std::pair<Position, uint32>> spotChecked;
        std::map<ObjectGuid, uint32> spotRejectLogMs;
        std::map<ObjectGuid, std::string> spotRejectLogged;  // [lead81] the last logged rejection (walk|reason)
        ObjectGuid routeFirstFor;  // [lead82] the engage the route walk leads to first (route_first logged)
        std::string lingeringSig;          // [lead84] the lingering combat's mob set (lingering_combat logged)
        ObjectGuid lingeringUnreachable;   // [lead84] the lingering mob the tank's walk was refused to
        bool outsideLogged = false;        // [lead93] leader_outside logged for this stay outside
        std::set<ObjectGuid> routeSkips;   // [lead96] route targets skipped while the leader refuses them
        ObjectGuid walkWakesFirst;         // [lead101] the pack the walk to a patrol wakes: the pull first
        std::map<ObjectGuid, ObjectGuid> fleeStoppers;  // [lead104] fleer -> the member the leader set to stop it
        ObjectGuid fleeFocusLogged;                     // [lead104] flee_focus logged for this fleer
        // [lead117] the pull walk's decision per member (one per order and per fact): the order, the reaching mob and
        // where it was judged, the spot, a hold (true) or the step back
        struct PullWalkSpot
        {
            uint32 orderId = 0;
            ObjectGuid mob;
            Position mobAt;
            Position spot;
            bool hold = false;
        };
        std::map<ObjectGuid, PullWalkSpot> pullWalkSpots;
        std::vector<G3D::Vector3> routePath;  // [lead97] the leg's path, taken once, walked by index
        size_t routePathIndex = 0;
        ObjectGuid routePathTarget;
        std::string routePathSource;      // [lead99] the leg path's source, logged in travel_leg
        bool routePathComplete = false;
        // [lead94] the travel of the current route leg (travel_leg)
        ObjectGuid travelTarget;
        Position travelFrom;
        uint32 travelTickMs = 0;
        float travelSec = 0.0f;
        float travelMoving = 0.0f;
        float travelFight = 0.0f;
        std::map<std::string, float> travelStops;
        struct MeleeMeet
        {
            ObjectGuid target;  // the mob the point is for
            Position meet;      // the leader's meeting point it was computed from
            Position point;     // the member's place at it
        };
        std::map<ObjectGuid, MeleeMeet> meleeMeet;  // [lead83] the melee meeting place, once per order / fight
        // [lead125] a melee member's walk to its target refused by an unpulled mob's reach (or no walk): the member
        // stays, the decision kept until a fact changes (another target, the target moves, the mob pulled / dead)
        struct MeleeBlock
        {
            ObjectGuid target;
            Position targetAt;  // where the target stood when the walk was judged
            ObjectGuid mob;     // the unpulled mob in the way (empty: no walk)
        };
        std::map<ObjectGuid, MeleeBlock> meleeBlocked;
        std::map<std::string, uint32> vetoedEvals;  // "<member>|<what>" -> count (vetoed_eval)
        // [lead18] the issued pull's whole plan for the red view (latched at the issue)
        Position issuedFrom;
        Position issuedFight;
        Position issuedTargetAt;
        uint32 healerLostTankMs = 0;  // [lead18] healer_lost_tank_s
        std::set<ObjectGuid> restingToFull;  // [lead20] Rest holds until full (no Prepare meanwhile)
        uint32 spotGateProgressMs = 0;       // [lead20] the spot gate's last progress (a sitter counts)
        std::string spotGateLast;
        uint32 commitTickMs = 0;              // [lead20] rest time does not age a commitment
        std::map<ObjectGuid, uint32> holdWaitMs;  // [lead20] combat re-spot waiting for the tank's hold
        Position meetPoint;                        // [lead25] where the tank meets the pulled mob
        std::map<std::string, uint32> leaderEventCounts;  // [lead27] leader_event per member
        std::set<ObjectGuid> engageRefused;               // [lead29] refused the engage range walk: Follow
        std::map<std::string, uint32> memberRefusals;    // [lead29] member_refusal per member (goal 0)
        std::map<ObjectGuid, uint32> noCommandLogMs;      // [lead29] no_command log throttle
        bool patrolJoinLogged = false;                    // [lead30] patrol_joins once per pull
        std::string answerSig;                            // [lead30] the last planner_answer
        std::vector<ObjectGuid> takenWalkers;             // [lead32] lone walkers taken along (no window)
        ObjectGuid patrolDangerFor;                       // [lead33] patrol_danger logged per patrol / verdict
        ObjectGuid planTarget;                            // [lead33] the leader's one target (its last plan)
        uint32 routeWalkSerial = 0;                       // [lead34] the route waypoint the leader walks
        std::optional<Position> routeWalkPoint;           // [lead91] the route point the tank walks to now
        ObjectGuid routeWalkTarget;                       // [lead91] the route target that point leads to
        ObjectGuid engageWalkFor;                         // [lead34] the engage the leader walks in for sight
        ObjectGuid patrolTarget;                          // [lead34] a patrol the leader pulls as its own pull
        std::map<ObjectGuid, std::string> bossPrepLogged;  // [lead37] the boss_prep state last logged
        std::map<ObjectGuid, std::pair<ObjectGuid, Position>> engageRangeSpots;  // [lead38] per member
        std::map<ObjectGuid, uint32> engageRangeOrder;  // [lead68] the order the range spot was computed for
        ObjectGuid keptPick;  // [lead39] the leader's pick, kept until dead / fought / refused / out of reach
        ObjectGuid pickJudgedFor;  // [lead40] the pick the reachability verdict was taken for
        Position pickJudgedAt;     // [lead41] the tank where it was judged
        ObjectGuid pickConflictFor;  // [lead43] the route target last logged as not picked
        ObjectGuid bossPullLoggedFor;  // [lead45] leader_boss_pull logged for this boss
        std::string bossWaitWhy;       // [lead45] boss_pull_waits: the readiness gap last logged
        ObjectGuid bossWaitFor;        // [lead46] the boss whose pull waits for the group's readiness
        std::string plannerRestFor;    // [lead50] the member the planner's held answer waits for (its Rest)
        ObjectGuid topUpFor;           // [lead76] the member the healer tops up (a readiness gap on health)
        std::map<ObjectGuid, std::string> restLogged;  // [lead76] the Rest line state last logged per member
        std::map<ObjectGuid, ObjectGuid> joinedPack;   // [lead76] an idle pack decided into the fight (mob)
        std::set<ObjectGuid> joinedMobs;               // [lead76] its mobs (exempt from the spot guard)
        std::optional<Position> fightMoveTo;           // [lead76] the fight moves away from a pack (the tank)
        std::map<ObjectGuid, ObjectGuid> groupBlockedBy;  // [lead77] member -> the idle mob its walk cannot pass
        float fightSecLeft = 0.0f;                        // [lead80] the fight's expected seconds
        float bossWalkSec = -1.0f;                        // [lead80] the walk to the nearest standing boss
        std::set<ObjectGuid> burnLogged;                  // [lead80] burn_decision logged in this fight
        std::string slowestPass;                       // [lead76] the slowest leader pass this minute
        uint32 slowestPassUs = 0;
        uint32 slowestPassLogMs = 0;
        // [lead51] the measured mana spend per member in fights
        struct ManaSpend
        {
            float prev = -1.0f;  // [lead69] the mana at this fight's start (-1: out of a fight)
            float spent = 0.0f;
            float sec = 0.0f;
            float fightSec = 0.0f;
        };
        std::map<ObjectGuid, ManaSpend> manaSpend;
        uint32 manaTickMs = 0;
        std::map<ObjectGuid, bool> noHeadroom;  // [lead53] no fight mob leaves this member threat headroom
        std::map<ObjectGuid, uint32> tauntLogged;  // [lead54] the pick-up spell last issued per loose mob
        std::map<ObjectGuid, uint32> threatDoneFor;  // [lead56] the pick-up cast the tank reported, per mob
        std::set<ObjectGuid> runHold;                // [lead59] an aggro-loss run holds until it arrived
        bool healStarted = false;                    // [lead61] the leader's Heal went out in this fight
        uint32 fightStartMs = 0;                     // [lead67] the fight's start (threat_lead marks)
        std::map<std::string, uint32> dupesBlocked;  // [lead69] command_dupe_blocked per member
        uint32 threatLeadLogged = 0;                 // [lead67] the last threat_lead mark logged
        std::map<ObjectGuid, std::map<std::string, float>> threatMax;  // [lead67] mob -> member -> top threat
        std::map<uint32, std::vector<float>> threatMargins;            // [lead67] mark -> tank / top margins

        bool tankCannotTake = false;               // [lead54] no taunt / AoE threat ready for the loose mob
        // [lead55] the move command before the current one, per member
        std::map<ObjectGuid, std::pair<Position, std::string>> prevMoves;
        std::map<ObjectGuid, uint32> prevMovePhase;  // [lead65] the phase the command before was issued in
        uint32 movePhase = 0;                        // [lead65] fight start / end, a new order: a new phase
        std::string movePhaseKey;
        std::map<std::string, uint32> commandRepeats;                      // [lead55] member|why -> repeats
        ObjectGuid dragFor;            // [lead50] fight_drag logged for this loose mob
        // [lead41] unreachable / detour verdicts: kept until the tank or the mob moved off where it was judged
        struct Unreachable
        {
            Position tankAt;
            Position mobAt;
            std::string why;
        };
        std::map<ObjectGuid, Unreachable> unreachable;
        std::string keptPickWhy;
        bool looseIsTarget = false;  // [lead38] every fight mob is loose: the loose one is the Attack target
        // [lead42] fight targets a member's Attack was refused on (who refused), until the fight ends
        std::map<ObjectGuid, std::set<ObjectGuid>> attackRefused;
        std::string fightStateSig;  // [lead42] the fight_attackers line last logged
        ObjectGuid patrolDangerLoggedAlong;               // [lead34] patrol_target "along" logged once
        std::vector<ObjectGuid> planMobs;
        bool patrolDangerFits = false;
        std::map<ObjectGuid, std::string> refusedPicks;  // [lead33] targets the planner refused / held: skipped
        std::map<std::string, std::vector<float>> walkDistances;  // [lead29] member -> tank on the walk
        std::vector<ObjectGuid> extraLogged;       // [lead25] extra_aggro logged per pull
        uint32 lostCheckMs = 0;
        uint32 bossPrepareSinceMs = 0;
        ObjectGuid heldReadyMob;
        uint32 heldReadySinceMs = 0;
        // [lead8] where the group fought and cleared this run (the tank at each fight's end)
        std::vector<Position> cleared;
        // [lead10] a Go decision whose walk crosses a closed door: not issued, counted per pack
        ObjectGuid doorInvalidMob;
        uint32 doorInvalidLooks = 0;
        // [lead11] the door the leader opens before the plan's walk (its key / lever starter leads the route)
        uint32 doorStepEntry = 0;
        uint32 doorStepMs = 0;
        uint32 doorStepLogMs = 0;
        // [lead3] packs reported planner_no_option this run
        std::set<ObjectGuid> noOption;
    };
}

#endif
