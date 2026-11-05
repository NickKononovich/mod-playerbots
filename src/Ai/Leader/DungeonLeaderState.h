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
#include "LeaderHazardVerdict.h"  // [lead820]
#include "ObjectGuid.h"
#include "Position.h"
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>  // [lead975]
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
        bool bossPullOrderSeen = false;  // [lead131] the tank opened the boss pull's order
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
        std::vector<ObjectGuid> aoeSpared;  // [lead415] the mobs every member's AoE spares
        std::string innervateLogged;        // [lead416] the last Innervate judgement logged
        // [lead15] mobs loose on a member: mob -> {victim, since}
        std::map<ObjectGuid, std::pair<ObjectGuid, uint32>> looseMobs;
        // [lead15] own_decision counts: member -> action -> count (the run report's work list)
        std::map<std::string, std::map<std::string, uint32>> ownDecisions;
        // [lead16] the hazard view per member, the members held out of a hazard, the combat spots
        std::map<ObjectGuid, std::vector<HazardZone>> hazardZones;
        uint32 hazardScanMs = 0;
        std::map<ObjectGuid, Position> hazardSpots;
        // [lead780] the stay-or-leave verdict per member and hazard it stands in (logged on a change), and the
        // numbers of the member's last verdict (its hazard MoveTo carries them)
        std::map<ObjectGuid, std::map<std::string, bool>> hazardStays;
        std::map<ObjectGuid, std::string> hazardVerdictWhy;
        // [lead820] the hazard casts seen in this fight (their cadence, measured) and the heal each member's stay asks
        // of the main healer per hazard (every hit to come): the healer's mana is judged on them all
        HazardVerdict::CastLog hazardCasts;
        HazardVerdict::StayHeals hazardStayHeals;
        std::map<ObjectGuid, std::pair<ObjectGuid, Position>> combatSpots;  // member -> {target, spot}
        // [lead137] the combat spots a member left in this fight, for their targets: not chosen again for that target
        // (A -> B -> A); [lead239] every one of the fight, not only the last (A -> B -> A came back over B)
        std::map<ObjectGuid, std::vector<std::pair<ObjectGuid, Position>>> combatSpotLeft;
        // [lead152] a fallback combat spot on the tank's own fight ground (its trail / its spot): no idle mob's reach
        std::map<ObjectGuid, Position> tankGroundSpots;
        std::map<ObjectGuid, ObjectGuid> tankGroundFor;  // [lead156] the target the fallback point was chosen for
        std::map<ObjectGuid, uint32> combatSpotLosMs;
        std::map<ObjectGuid, ObjectGuid> ccAssign;  // [lead16] mob -> the member that crowd-controls it
        ObjectGuid looter;          // [lead16] the member told to loot after the last fight, and since when
        uint32 lootSinceMs = 0;
        bool lastFighting = false;
        uint32 peaceSinceMs = 0;  // [lead940] the last fight's end (getMSTime; 0: none fought yet)
        uint32 fightLenStartMs = 0;    // [lead88] the fight's start and its estimated length (-1: none)
        float fightLenEstSec = -1.0f;
        std::map<std::string, uint32> staleCommands;  // [lead16] member -> stale_command ticks
        ObjectGuid armyHoldFor;                        // [lead270] the boss pull a prepull Army holds (logged)
        std::set<ObjectGuid> trailing;                 // [lead17] members the leader waits for (far behind)
        std::set<ObjectGuid> trailUnmovedLogged;       // [lead1297] trailing members named standing on another order
        std::map<ObjectGuid, uint32> spotReassigns;    // [lead17] pull spots re-assigned per member
        // [lead930] pull spots the issue guard refused per member (no candidate again while the pull stands)
        std::map<ObjectGuid, std::vector<Position>> spotRefused;
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
        std::string commitWaitKey;  // [lead1270] the last commit_wait logged (its reason without numbers)
        bool patrolWindowOpen = true;
        std::string roomSig;  // [lead18] the last room_order logged
        // [lead18] the fight's anchor (LEADER_ANCHORED_SPOTS) and each spot's anchor serial
        Position fightAnchor;
        uint32 fightAnchorSerial = 0;
        bool fightAnchorAhead = false;  // [lead466] the anchor is the tank's stop point it still walks / charges to
        std::map<ObjectGuid, uint32> combatSpotAnchor;
        std::map<ObjectGuid, Position> combatSpotStoppedAt;  // [lead397] where "casts from where it walks" stopped
        // [lead18] the spot guard: cleared ground, idle mobs, the last checked spot per member, reject log
        std::vector<Position> clearedGround;
        std::map<ObjectGuid, Position> clearedLast;
        std::vector<ObjectGuid> idleGuids;
        uint32 idleScanMs = 0;
        // [lead143] the last verdict per member: a refusal is kept until a fact changes (the spot, the member, the
        // blocking mob moved), an accepted spot is re-judged on its cadence
        struct SpotVerdict
        {
            Position spot;
            Position from;
            uint32 ms = 0;
            std::string reject;
            bool noPath = false;
            ObjectGuid mob;
            Position mobAt;
        };
        std::map<ObjectGuid, SpotVerdict> spotChecked;
        std::map<ObjectGuid, uint32> spotRejectLogMs;
        std::map<ObjectGuid, std::string> spotRejectLogged;  // [lead81] the last logged rejection (walk|reason)
        ObjectGuid routeFirstFor;  // [lead82] the engage the route walk leads to first (route_first logged)
        std::string lingeringSig;          // [lead84] the lingering combat's mob set (lingering_combat logged)
        ObjectGuid lingeringUnreachable;   // [lead84] the lingering mob the tank's walk was refused to
        bool outsideLogged = false;        // [lead93] leader_outside logged for this stay outside
        bool leaderDownCleared = false;    // [lead227] the members' commands ended for this death / stay outside
        std::set<ObjectGuid> routeSkips;   // [lead96] route targets skipped while the leader refuses them
        ObjectGuid walkWakesFirst;         // [lead101] the pack the walk to a patrol wakes: the pull first
        std::map<ObjectGuid, ObjectGuid> meleeChase;  // [lead190] member -> the walked-off target it chases
        // [lead720] member -> the target its side was weighed for and the verdict (true: its front, false: behind)
        std::map<ObjectGuid, std::pair<ObjectGuid, bool>> meleeSide;
        // [lead801] member -> the chase verdict for its target (DryFollow): the target, refused or not, the idle mob
        // it would wake and where the target stood when it was judged. Kept until one of those facts changes
        struct ChaseVerdict
        {
            ObjectGuid target;
            bool refused = false;
            ObjectGuid blocker;
            Position targetAt;
        };
        std::map<ObjectGuid, ChaseVerdict> chaseVerdicts;
        std::set<std::pair<ObjectGuid, ObjectGuid>> sideHeldLogged;   // [lead800] logged once per member and target
        std::set<std::pair<ObjectGuid, ObjectGuid>> chaseHeldLogged;  // [lead801]
        std::set<std::pair<ObjectGuid, ObjectGuid>> walkEndHeldLogged;  // [lead802]
        ObjectGuid walkWakesReleased;      // [lead167] the pull released once for walkWakesFirst (one release per fact)
        std::map<ObjectGuid, ObjectGuid> fleeStoppers;  // [lead104] fleer -> the member the leader set to stop it
        ObjectGuid fleeFocusLogged;                     // [lead104] flee_focus logged for this fleer
        // [lead1345] a fight mob that flees for help near its flee health -> the member with a stop at range the
        // leader keeps in its range and sight (this tick's), and the flee_stopper_ready state logged per mob
        struct FleeReady
        {
            ObjectGuid mob;
            char const* spell = nullptr;
            float range = 0.0f;
        };
        std::map<ObjectGuid, FleeReady> fleeReady;             // member -> its fleer
        std::map<ObjectGuid, std::string> fleeReadyLogged;     // fleer -> "member/state" logged
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
            bool follows = false;  // [lead1031] its walk there is blocked short of the pull: along with the tank
            Position from;         // [lead1031] where the member stood when that was judged
        };
        // [lead730] the melee wait before the pull: the last point of its walk to the pull's stop point outside
        // every unpulled mob's reach (target: the pull's mob, meet: the stop point it was computed for)
        std::map<ObjectGuid, MeleeMeet> meleeWait;
        // [lead730] the melee walk in at the tank's opener: the predicted meeting of the running target and the tank
        // (target: that mob, meet: the stop point, point: the member's walk's end outside every unpulled reach)
        std::map<ObjectGuid, MeleeMeet> meleeStop;
        // [lead731] the arrival a member's Attack target was judged by (lead413 / lead432): target, the mob where it
        // stood then (meet), the arrival point (point) - kept while the mob stays there, not re-read from the
        // member's own walk
        std::map<ObjectGuid, MeleeMeet> attackArrive;
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
        Position idleScanAt;  // [lead1121] where the tank stood at the last idle-mob scan (lead18)
        std::map<ObjectGuid, uint32> engageRangeOrder;  // [lead68] the order the range spot was computed for
        // [lead1030] the order and the member's place a range spot search found none from: not searched again
        // until the order changes or the member moved off that place (the search's own spot tolerance)
        std::map<ObjectGuid, std::pair<uint32, Position>> engageRangeNone;
        ObjectGuid keptPick;  // [lead39] the leader's pick, kept until dead / fought / refused / out of reach
        ObjectGuid pickJudgedFor;  // [lead40] the pick the reachability verdict was taken for
        Position pickJudgedAt;     // [lead41] the tank where it was judged
        ObjectGuid pickConflictFor;  // [lead43] the route target last logged as not picked
        ObjectGuid bossPullLoggedFor;  // [lead45] leader_boss_pull logged for this boss
        std::string bossWaitWhy;       // [lead45] boss_pull_waits: the readiness gap last logged
        ObjectGuid bossWaitFor;        // [lead46] the boss whose pull waits for the group's readiness
        std::string plannerRestFor;    // [lead50] the member the planner's held answer waits for (its Rest)
        float plannerRestLine = -1.0f;  // [lead221] the mana line that answer names for it ("needs N %"), -1: none
        ObjectGuid topUpFor;           // [lead76] the member the healer tops up (a readiness gap on health)
        std::map<ObjectGuid, std::string> restLogged;  // [lead76] the Rest line state last logged per member
        std::set<ObjectGuid> restHurtLogged;  // [lead226] no Rest while it takes damage: logged once per stretch
        std::map<ObjectGuid, float> restEndMana;  // [lead230] its mana when its last Rest ended (out of a fight)
        std::map<ObjectGuid, float> restExtra;    // [lead230] the mana it spent standing between two Rests
        // [lead1021] the boss line (mana %) a member's boss-wait Rest last read while it held: the Rest ends at it
        std::map<ObjectGuid, float> restBossLine;
        std::set<ObjectGuid> restBossHoldLogged;  // [lead1021] its hold over an empty gap logged (once per stretch)
        std::map<ObjectGuid, std::string> manaWayLogged;  // [lead955] the warlock mana way last logged
        std::set<ObjectGuid> lifeTapRefusedLogged;  // [lead1310] no Life Tap while mobs attack it: once per stretch
        std::map<ObjectGuid, ObjectGuid> joinedPack;   // [lead76] an idle pack decided into the fight (mob)
        std::set<ObjectGuid> joinedMobs;               // [lead76] its mobs (exempt from the spot guard)
        std::set<ObjectGuid> joinedTankOpens;          // [lead1140] joined for a member's blocked spot / walk
        std::set<ObjectGuid> joinedHeld;               // [lead1140] of those: idle, the tank opens on them from here
        std::set<ObjectGuid> fieldJudged;  // [lead1100] walkers into a ground field judged this fight
        std::set<ObjectGuid> fieldTaken;   // [lead1100] their packs the field may take (spared no longer)
        uint32 chainLoggedFight = 0;                                 // [lead1170] the fight (start ms) logged below
        std::set<std::pair<ObjectGuid, std::string>> chainLogged;   // [lead1170] chain_pull verdicts (pack, kind)
        std::optional<Position> fightMoveTo;           // [lead76] the fight moves away from a pack (the tank)
        std::string fightMoveWhy;                      // [lead909] its purpose (empty: away from a pack)
        // [lead1190] where the tank will stand: a chain pull's walk end (planAheadMob empty) or a tank meet's landing
        // (its mob) - the fight anchor while the tank goes there
        std::optional<Position> planAhead;
        ObjectGuid planAheadMob;
        std::map<ObjectGuid, ObjectGuid> groupBlockedBy;  // [lead77] member -> the idle mob its walk cannot pass
        float fightSecLeft = 0.0f;                        // [lead80] the fight's expected seconds
        float groupDps = 0.0f;  // [lead700] the estimated group dps the fight's seconds were judged by
        float bossWalkSec = -1.0f;                        // [lead80] the walk to the nearest standing boss
        float bossPullSec = -1.0f;  // [lead178] seconds to the next boss pull: this fight, the on-path fights, the walk
        // [lead1083] a living boss not in the fight is ahead (false: no boss left - the estimate's own state; while
        // true bossPullSec >= 0 on every leader tick, in a fight and out of it)
        bool bossLeft = false;
        ObjectGuid bossOrderFirst;  // [lead1085] the computed first mob of a fight with a boss (Empty: none)
        // [lead1083] the route plan's estimate table, built once per plan (its boss, stop count and length): from each
        // stop the straight walk on to the boss, the plan's path length over its straight legs, the health of the
        // stops from each one on
        struct BossPullTable
        {
            ObjectGuid boss;
            size_t stops = 0;
            float length = -1.0f;
            float pathFactor = 1.0f;
            std::vector<float> walkFrom;
            std::vector<float> healthFrom;
        };
        BossPullTable bossPullTable;
        std::string bossPullParts;  // [lead724] bossPullSec's parts, logged with each burn_decision
        // [lead771] bossPullSec from its parts alone (no stretch floor): what a trash verdict's "back before" reads
        float bossPullPartsSec = -1.0f;
        // [lead852] [lead1200] a burn cooldown allowed in a fight stays allowed for that fight: member -> cooldown ->
        // the fight (its fightStartMs)
        std::map<ObjectGuid, std::map<std::string, uint32>> burnKept;
        // [lead274] every living boss judged like any pull once per fight (JudgePack's overload, his length over the
        // group's dps): the burn cooldowns go to the boss that needs them most; the fight itself (a critical pull)
        struct BossNeed
        {
            std::string name;
            float overload = 0.0f;
            float fightSec = 0.0f;
            std::string judged;
        };
        std::map<ObjectGuid, BossNeed> bossNeeds;
        float fightOverload = 0.0f;
        std::string fightJudged;
        std::string nextBossName;  // the nearest standing boss
        ObjectGuid nextBossGuid;   // [lead421] the next boss (the route plan's, else the nearest standing)
        // [lead421] the run's measured stretches between bosses (the run's first tick / a boss fight's end to the
        // next boss pull): their sum and count, the start of the one under way, a boss fight on
        float stretchSecSum = 0.0f;
        uint32 stretches = 0;
        uint32 stretchFromMs = 0;
        bool bossFightOn = false;
        // [lead513] the run's measured trash fights: the fight mobs of the fight under way (their full health), and
        // over the finished trash fights the full health of the mobs killed and the fights' seconds
        std::map<ObjectGuid, float> fightMobHealth;
        float trashKilledHealth = 0.0f;
        float trashFightSec = 0.0f;
        uint32 bossJudgedFor = 0;  // the fightStartMs it was judged for
        std::map<std::string, std::string> burnLogged;  // [lead274] burn_decision: the verdict logged per member
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
            uint32 fights = 0;         // [lead232] fights counted
            bool boss = false;         // [lead232] a boss fought in this fight
            float bossSpent = 0.0f;    // [lead232] the same over boss fights only
            float bossSec = 0.0f;
            float fullSpent = 0.0f;  // [lead234] the same over the fights at least as long as the mean fight
            float fullSec = 0.0f;
            float fightCost = 0.0f;  // [lead393] a healer: this fight's planned cost (tank healths), its order's
            float costSpent = 0.0f;  // [lead393] the net use over the fights with a planned cost
            float costSum = 0.0f;    // [lead393] ... and their planned costs
            float dryPct = -1.0f;    // [lead417] its mana when it first ran dry in this fight (-1: not dry)
            float drySec = 0.0f;     // [lead417] ... and the fight's seconds until then
            float lastPct = -1.0f;   // [lead480] its mana at the last fight tick
            float toolGain = 0.0f;   // [lead480] the mana its tools and potions returned in this fight (until dry)
            float toolSec = 0.0f;    // [lead921] the fight seconds a tool returned in (out of the measured rate)
            // [lead990] the fight seconds it wanded (its rotation out of mana: no spend) and the mana they changed,
            // both out of the measured rate (until dry)
            float wandSec = 0.0f;
            float wandGain = 0.0f;
            bool potion = false;     // [lead480] a potion drunk in this combat (Player::GetLastPotionId) is counted
            // [lead520] the fight seconds it acted (casting, channelling or on its global cooldown) until dry, and over
            // its fights the net use per acting second: the rate of its whole rotation, a boss fight's
            float actingSec = 0.0f;
            float actSpent = 0.0f;
            float actSec = 0.0f;
            float bossActSpent = 0.0f;  // [lead610] the same over boss fights only: its boss rotation's rate
            float bossActSec = 0.0f;
            bool gemReady = false;  // [lead610] a mana gem ready at the last fight tick (RaidConsumables::FindManaGem)
        };
        std::map<ObjectGuid, ManaSpend> manaSpend;
        std::map<ObjectGuid, float> fightBosses;  // [lead234] the bosses of this fight, their max health
        uint32 manaTickMs = 0;
        std::map<ObjectGuid, bool> noHeadroom;  // [lead53] no fight mob leaves this member threat headroom
        std::map<ObjectGuid, std::string> attackWaitWhy;  // [lead413] its mobs stand in an unpulled mob's reach
        // [lead571] member -> the mob its last Attack change left and when (the aggro-loss cause: a switch in flight)
        std::map<ObjectGuid, std::pair<ObjectGuid, uint32>> attackLeft;
        // [lead412] the threat redirect the leader told a member (Misdirection / Tricks): onto whom, and the share of
        // its threat the spell's SPELL_EFFECT_REDIRECT_THREAT moves (its base points, %)
        std::map<ObjectGuid, std::pair<ObjectGuid, float>> redirectTo;
        struct ThreatSeen  // [lead243] a member's / the tank's threat on a fight mob since its first threat there
        {
            uint32 firstMs = 0;
            uint32 lastMs = 0;
            float threat = 0.0f;
            uint32 riseMs = 0;  // [lead432] the last time its threat there grew
            // [lead561] threat it did not build at its own rate: a taunt's copy of the top threat (the tank's rate
            // there counts from it)
            float base = 0.0f;
            bool taunted = false;  // [lead561] its taunt aura was on the mob at the last look
        };
        std::map<ObjectGuid, std::map<ObjectGuid, ThreatSeen>> threatSeen;  // [lead243] fight mob -> who -> seen
        std::map<ObjectGuid, std::string> stopProjection;        // [lead246] the headroom projection a Stop was read on
        std::map<ObjectGuid, std::pair<float, uint32>> openTps;  // [lead243] its threat / s on each mob it hit: sum, n
        // [lead1080] the pre-cast (user 2026-11-04 Q58): a caster's first damage cast started so it lands just after
        // the tank's predicted first hit on an engaged mob. `opener` is the cast-time damage spell the member opened
        // its last fight with (read from its own cast, no list); `mob` / `spell` the pre-cast told now
        struct Precast
        {
            uint32 opener = 0;
            ObjectGuid mob;
            uint32 spell = 0;
            uint32 toldMs = 0;
            bool started = false;
            ObjectGuid toldOn;  // [lead1091] the mob its last pre-cast was told on (kept after it landed)
            std::string skip;   // [lead1091] the gate that held its pre-cast on this tick's pull mob
        };
        std::map<ObjectGuid, Precast> precast;
        ObjectGuid precastMob;  // [lead1091] the pull mob the pre-cast window was open on last tick
        std::set<ObjectGuid> precastRead;  // [lead1080] members whose first cast of this fight was read
        std::set<ObjectGuid> precastRotationRead;  // [lead1101] members whose rotation opener was read
        std::map<ObjectGuid, uint32> tauntLogged;  // [lead54] the pick-up spell last issued per loose mob
        std::map<ObjectGuid, uint32> threatDoneFor;  // [lead56] the pick-up cast the tank reported, per mob
        std::set<ObjectGuid> runHold;                // [lead59] an aggro-loss run holds until it arrived
        std::set<std::pair<ObjectGuid, ObjectGuid>> looseRunRefused;  // [lead1300] (member, mob) logged
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
        // [lead1011] ... or another fact it was judged under changed: the tank's own state (a stun / root / loss of
        // control), its fight state, a fight that ended since, the mob's fight state (LeaderUnreachableChanged)
        struct Unreachable
        {
            Position tankAt;
            Position mobAt;
            std::string why;
            bool tankHeld = false;
            bool tankFighting = false;
            uint32 peaceSinceMs = 0;
            bool mobFighting = false;
        };
        std::map<ObjectGuid, Unreachable> unreachable;
        std::string keptPickWhy;
        bool looseIsTarget = false;  // [lead38] every fight mob is loose: the loose one is the Attack target
        // [lead42] fight targets a member's Attack was refused on (who refused), until the fight ends
        std::map<ObjectGuid, std::set<ObjectGuid>> attackRefused;
        std::string fightStateSig;  // [lead42] the fight_attackers line last logged
        // [lead1332] a fight mob nobody can hit from where they stand: the sight spot a member walks to (mob ->
        // member, spot), the spots that did not give sight once reached, and the mobs no member has any spot for
        // (where each stood at that verdict)
        std::map<ObjectGuid, std::pair<ObjectGuid, Position>> sightSpots;
        std::set<std::pair<ObjectGuid, ObjectGuid>> sightSpotSpent;
        std::map<ObjectGuid, Position> unhittableNoSpot;
        // [lead1332] fight mobs that cannot come to their victim (standing, out of its melee reach, no navmesh walk
        // to it - PathGenerator - and no cast of their own): they deal nobody damage where they stand
        std::set<ObjectGuid> harmlessMobs;
        // [lead1420] the in-fight deadlock guarantee: the leader's last tick ended with no member acting (none walking
        // or with a walk ordered, none damaging a fight mob from where it stands, none on a new target, no fight mob
        // coming) - a state the next tick judges its own decision by; the mob the break hands the no-place resolver,
        // each member's attack target at the last tick and the break last logged
        bool fightIdle = false;
        std::string fightIdleWhy;
        ObjectGuid fightDeadlockMob;
        std::map<ObjectGuid, ObjectGuid> fightIdleTargets;
        std::string fightDeadlockLogged;
        // [lead1451] the same guarantee per member: the members whose last leader tick in the fight ended without an
        // action of their own (memberIdle), those that ended two ticks in a row so (memberNoAction: the why, read by
        // the member_no_action invariant) and the member break last logged per member
        std::set<ObjectGuid> memberIdle;
        std::map<ObjectGuid, std::string> memberNoAction;
        std::map<ObjectGuid, std::string> memberBreakLogged;
        ObjectGuid patrolDangerLoggedAlong;              // [lead34] patrol_target "along" logged once
        std::vector<ObjectGuid> planMobs;
        bool patrolDangerFits = false;
        std::map<ObjectGuid, std::string> refusedPicks;  // [lead33] targets the planner refused / held: skipped
        std::map<std::string, std::vector<float>> walkDistances;  // [lead29] member -> tank on the walk
        std::vector<ObjectGuid> extraLogged;       // [lead25] extra_aggro logged per pull
        uint32 lostCheckMs = 0;
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
        // [lead270] the prepull Army of the Dead per death knight (LeaderPrepull.h)
        struct ArmyPrepull
        {
            ObjectGuid boss;
            uint32 orderedMs = 0;        // the leader told the DK to channel (0: not yet)
            float orderedRemainSec = 0.0f;  // the preparation left (computed) when it was told
            uint32 predictedEndMs = 0;   // when the preparation was computed to end
            uint32 prepEndMs = 0;        // when the preparation really ended (the pull gate open but for the Army)
            uint32 startedMs = 0;        // the channel seen running
            int32 channelLeftMs = 0;     // [lead272] the channel's remainder when last seen, and when
            uint32 channelSeenMs = 0;
            bool paused = false;         // [lead272] the order is off while the DK walks
            bool done = false;
            std::string blockedLogged;   // [lead803] the last logged reason it could not be told (per boss)
            std::string waitLogged;      // [lead881] the last logged reason the gate waits for it (per boss)
            // [lead910] the tank knight's step out of the boss's own reach to channel: where to and where from
            std::optional<Position> stepTo;
            Position stepFrom;
            bool stepNone = false;  // no such spot found from stepFrom (searched once per place)
            bool stepBack = false;  // walked back to stepFrom after the channel
        };
        std::map<ObjectGuid, ArmyPrepull> armyPrepull;
        // [lead773] the assassination rogue's pre-pull Stealth for Overkill per rogue (LeaderPrepull.h StealthHolds):
        // the boss pull it is for, the pull gate waits for it (ordered), done once stealthed / refused / fighting
        struct StealthPrepull
        {
            ObjectGuid boss;
            bool ordered = false;
            bool done = false;
        };
        std::map<ObjectGuid, StealthPrepull> stealthPrepull;
        // [lead271] the trap weave per hunter (LeaderTrapWeave.h)
        struct TrapWeave
        {
            ObjectGuid target;
            uint32 spell = 0;
            Position spot;         // where the hunter drops the trap
            float triggerRadius = 0.0f;
            uint8 step = 0;        // 0 walk, 1 drop
            uint32 orderedMs = 0;
        };
        std::map<ObjectGuid, TrapWeave> trapWeave;
        std::map<ObjectGuid, std::string> trapWeaveLogged;  // the last verdict logged per hunter
        // [lead276] a hunter's refused trap walk (its target and spot): no new weave until either changes
        std::map<ObjectGuid, std::pair<ObjectGuid, Position>> trapWeaveRefused;
        // [lead400] .. [lead405] the leader's support decisions (Fight/LeaderSupport.h)
        struct Support
        {
            std::map<std::pair<ObjectGuid, uint32>, ObjectGuid> dispellers;  // (member, debuff spell) -> dispeller
            std::set<std::pair<ObjectGuid, uint32>> dispelFailed;  // (dispeller, debuff spell): it reported a refusal
            std::set<std::pair<ObjectGuid, uint32>> dispelNoneLogged;  // (member, debuff spell) nobody can remove
            std::map<ObjectGuid, ObjectGuid> resurrecters;  // dead member -> its resurrecter (out of a fight)
            std::set<std::pair<ObjectGuid, ObjectGuid>> resFailed;  // (resurrecter, dead) refused for this death
            std::set<ObjectGuid> resNoneLogged;  // dead members nobody can raise, logged once per death
            std::set<ObjectGuid> resVerdicts;    // [lead1367] dead members with a published resurrection verdict
            ObjectGuid prepotBoss;                // the boss the pre-pull potions are for
            std::set<ObjectGuid> prepotDone;      // members done (used it, have none, or refused) for that boss
            ObjectGuid prepopBoss;                // [lead774] the boss the cat's pre-pull Berserk is for
            std::set<ObjectGuid> prepopDone;      // [lead774] cats done (cast it or refused) for that boss
            std::map<ObjectGuid, std::string> potionLogged;  // the last in-fight potion verdict logged per member
            uint32 fightStartMs = 0;               // the fight's start (the members' measured damage taken rate)
            std::map<ObjectGuid, uint64> takenAtStart;
            std::set<std::pair<ObjectGuid, ObjectGuid>> wellTried;  // (member, Soulwell) it used without a stone
            std::set<ObjectGuid> wellClose;  // members told to use the well that stood in its reach last tick
            uint32 soulstonePass = 0;  // the buff pass (its start) the soulstone was given or given up in
            uint32 ritualPass = 0;     // the buff pass the Ritual of Souls was given up in (refused)
            // [lead1360] the buff pass's ritual portal (its pass) and the members whose click it did not take
            uint32 portalPass = 0;
            ObjectGuid ritualPortal;
            std::set<ObjectGuid> portalFailed;
            std::map<ObjectGuid, std::string> battleResLogged;  // the last battle resurrection verdict per dead
            std::string buffWork;  // what the soulwell / soulstone orders still do (the buff pass waits for it)
            std::string supportLogged;  // the last buff-pass support verdict logged
            // [lead975] the buff pass's buff orders (step 1): TickBuffing queues every missing buff a caster gives
            // (each harness tick), the leader tick (TickBuffPass) asserts one order per caster at a time and takes
            // the next once the buff is on or the caster reported it cannot cast it
            struct BuffOrder
            {
                ObjectGuid caster;
                uint32 spell = 0;
                ObjectGuid target;
                std::string what;  // "<caster> <spell name> on <target>"
                // [lead1251] the ms left of the caster's own aura on the target when the order was told (0: none):
                // the order is cast when the aura is on again with more left (a recast), not the old one still on
                int32 leftMs = 0;
            };
            using BuffKey = std::tuple<ObjectGuid, uint32, ObjectGuid>;  // caster, spell, target
            std::vector<BuffOrder> buffQueue;            // the orders the step still needs, in the step's order
            uint32 buffQueuePass = 0;                    // the buff pass (its start) the queue belongs to
            std::map<ObjectGuid, BuffOrder> buffTold;    // caster -> the order it holds now (one at a time)
            std::map<BuffKey, std::string> buffFailed;   // the casters' reports in this pass: the buff is not given
            std::set<BuffKey> buffIssuedKeys;            // every order issued in this pass
            uint32 buffCast = 0;                         // orders that ended with the buff on its target
            std::string buffOrderWork;                   // the orders standing / waiting (the step waits for it)
            // [lead1133] caster -> the mana its buff orders still standing in the pass cost, while it has less
            std::map<ObjectGuid, uint32> buffManaNeed;
        };
        Support support;
    };
}

#endif
