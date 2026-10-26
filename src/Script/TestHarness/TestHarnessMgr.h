/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTHARNESSMGR_H
#define PLAYERBOTS_TESTHARNESSMGR_H

#include "ObjectGuid.h"
#include "Position.h"
#include "TestHarnessData.h"
#include "TestHarnessLog.h"
#include "TestHarnessPrepare.h"

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class ChatHandler;
class Creature;
class GameObject;
class InstanceScript;
class Map;
class Player;
class Unit;  // [lead3]
#include "HazardZone.h"  // [lead16]
namespace PullPlanner
{
    struct Decision;  // [lead15] ChooseTargetActions.h
}
class WorldObject;
struct AreaTrigger;

namespace TestHarness
{
    // Test accounts are named <prefix>NN; the random bot manager only takes "rndbot%" accounts and the
    // addclass pool only playerbots_account_type 2, so neither ever sees them.
    constexpr char const* TEST_ACCOUNT_PREFIX = "RRTEST";
    constexpr uint32 PROTECTED_ACCOUNT_ID = 101;  // the user's own account (PLAYER)
    constexpr uint32 CHARACTERS_PER_ACCOUNT = 10;
    // Character names: "Rt<class><aa..zz>" for the level-80 pool, "Rl<class><aa..zz>" for the levelling pool
    // ("test setup <n> leveling"), which is only ever used by normal dungeons below level 80.
    constexpr char const* NAME_PREFIX_MAX_LEVEL = "Rt";
    // User rule (2026-10-24): bots walk by themselves. false: the harness never moves, teleports or holds a bot
    // during a run (no MovePoint / MoveJump / TeleportTo / SetNextCheckDelay for a walk); it gives the group intent
    // (the leader's waypoint, a portal to go through, a spot to come to) and the bots' own actions walk. What a bot
    // cannot do is logged as bot_cannot_move. true restores the harness-driven moves of waves 1-10 (each counted in
    // run_end.harnessMoves). Setup before the run (the teleport to the entrance) is not affected.
    constexpr bool HARNESS_MOVES_BOTS = false;
    constexpr char const* NAME_PREFIX_LEVELING = "Rl";

    enum class Phase : uint8
    {
        Picking,
        LoggingIn,
        Preparing,
        Grouping,
        Idle,         // prepared party, no run
        Teleporting,
        Buffing,
        ReadyCheck,
        Traveling,
        Waiting,      // members far behind
        Drinking,
        EventStart,
        Clearing,     // trash combat
        BossFight,
        Reviving,     // corpse run after a wipe
        Done,
        Failed,
        Stalled,
        Stopped
    };

    char const* PhaseName(Phase phase);
    // [hleft-bm-tank-fetch] Black Morass while the event runs (rift >= 1, shield > 0): `member` holds at Medivh (within
    // BM_MEDIVH_HOLD_RANGE) while the tank fetches the rift keepers alone (tbc-bm strategy) - no far wait, no regroup
    // and no meet for him.
    bool BlackMorassHoldsAtMedivh(Player* member);
    bool IsTerminalPhase(Phase phase);

    struct Member
    {
        ObjectGuid guid;
        std::string name;
        uint8 cls = 0;
        SpecDef const* spec = nullptr;
        CompSlot slot{};
        std::pair<uint16, uint16> professions{0, 0};
        PrepareResult prep;
        bool prepared = false;
        uint32 deadSinceMs = 0;
        uint32 awaySinceMs = 0;  // dead or outside since (AWAY_MAX_MS is per member, not "somebody away")
        // Back in through the portal (MeetMember's regroup): seen on the run's map, then off it, then on it again.
        bool seenInside = false;
        bool wasOutside = false;
        uint32 backInMs = 0;  // when it came back in (0: never during this run)
        bool ghostGuided = false;
        uint32 combatMs = 0;  // time alive and in combat during the run (sampled every harness tick)
        uint32 aggroPulled = 0;  // mobs that stayed on this non-tank member for AGGRO_LOSS_MS or longer
        uint32 unplannedStarts = 0;  // fights this non-tank member entered combat first (pull_start "unplanned")
        Position ghostLastPos;   // corpse run: where the ghost stood at the last guidance check
        uint32 ghostStillMs = 0;
        bool ghostNoPathLogged = false;
        MemberCounters pullStart;  // counters when the current pull started
        uint32 pullStartCombatMs = 0;
        // Return to the instance by flying mount (portal on a floating island: Tempest Keep, ...)
        uint32 flyCastMs = 0;     // last mount cast
        float flyCruiseZ = 0.0f;  // altitude of the current flight, 0 = not flying to the portal
        Position flyApproach;     // where the flight descends beside the portal (the portal itself without a side)
        uint8 flyStage = 0;       // 0 cruise / descent to flyApproach, 1 the last straight leg into the trigger
        bool flyRefusedLogged = false;
        bool leavingWater = false;
        // Breath estimate: Player::m_MirrorTimer is not reachable from a module, so the harness counts it the way
        // Player::HandleDrowning does (3 minutes under water, refilled 10x as fast above).
        int32 breathMs = -1;  // -1 = not sampled yet (full)
        uint32 breathSampleMs = 0;
        bool surfacing = false;
        bool lowBreathLogged = false;  // HARNESS_MOVES_BOTS off: the bot's own "breathe" surfaces, logged once
        // HARNESS_MOVES_BOTS off: intent given to this member (not the leader): its own "test runner move" walks to
        // the waypoint (dry spot, gather spot, link), its own "instance portal" into the instance.
        bool hasWaypoint = false;
        bool hasPortalGoal = false;
        uint32 noWalkSinceMs = 0;  // away and not moving since (bot_cannot_move "corpse run")
        // Alive outside, no walk back for OUTSIDE_GIVE_UP_STUCKS windows of GHOST_STUCK_MS: no longer waited for
        // (dtk-n build 10: a spirit-healed shaman stood 30 yd above the portal for 4 minutes).
        uint32 outsideStuckCount = 0;
        // A ping-pong hold timed out with this member still apart: until then it is neither apart nor far (gd-h /
        // wc-h build 17: Rtdruay 37 yd, dz -7.6, held the group 160 s over repeated holds).
        uint32 apartIgnoreUntilMs = 0;
        bool leftOutside = false;
        uint32 entryRefusedSinceMs = 0;  // on its entrance, still outside: the core refuses entry since
        std::string entryRefusal;        // the refusal last logged (entry_refused)
        Position noWalkPos;
        // A walked wing change / regroup exit: this member stays near one spot since (TickWingWalk).
        uint32 wingStillMs = 0;
        Position wingStillPos;
        // MemberStranded: no navmesh walk to the leader either way, re-checked every STRANDED_RECHECK_MS.
        bool stranded = false;
        uint32 strandedCheckMs = 0;
        uint32 strandedSinceMs = 0;
        Position strandedPos;
        // [b30_topped_up] TickStrandedReenter: no walk back to the group since (still near reenterPos), the longest
        // StuckWatchdog stuck count seen meanwhile, re-entries so far (each a death)
        uint32 reenterCheckMs = 0;
        uint32 reenterSinceMs = 0;
        uint32 reenterWatchdogSec = 0;
        uint32 strandedReentries = 0;
        Position reenterPos;
        // [b30_topped_up] wait_timeouts in a row with this member far (FAR_TIMEOUTS_DROP: no more far waits)
        uint32 farTimeouts = 0;
        // [b31_far_threshold] waited for (far wait / meet): far until within FAR_RESUME_RANGE; the apart metric at
        // the last closing in, and when
        bool farWaited = false;
        float farLastMetric = 0.0f;
        uint32 farClosingMs = 0;
        // Regen wait: the group waits for this member since (0 = not waiting), and whether its "[regen]" stall line
        // (waiting but not eating / drinking) went out for the current wait.
        uint32 regenSinceMs = 0;
        bool regenStallLogged = false;
        uint32 regenStallMs = 0;  // when regen_stall was logged (REGEN_STALL_RELEASE_MS)
        bool regenStallReleased = false;  // "regen_stall_release" logged: no longer waited for
        bool regenAboveLineLogged = false;  // regen_wait_below_line logged for this rest (reset by a fight)
        uint32 seatedSinceMs = 0;           // [queue-rest-sit-grace] when this member sat down to eat / drink
    };

    struct BossTrack
    {
        uint32 bossId = 0;
        uint8 lastState = 0;
        bool stateChanged = false;  // the state moved at least once during the run (IsPhantomSlot)
        std::string name;
        uint32 attempts = 0;
        uint32 wipes = 0;
        bool killed = false;
        bool failed = false;
        std::string failReason;  // [hleft-script-slot-names] a failed slot's own reason (ScriptSlotDef::failNote)
        uint32 attemptStartMs = 0;
        std::map<ObjectGuid, MemberCounters> attemptStart;
        std::map<ObjectGuid, uint32> attemptStartCombatMs;
        std::vector<std::string> attemptDeaths;
        bool hyperspeedUsed = false;
        bool combatPotionUsed = false;
        // boss_phantom (build 11): the boss's health while the attempt runs, and since when it has not changed.
        uint32 attemptKills = 0;  // Party::kills when the attempt was engaged (boss_attempt_loop)
        // [w24c-hor-escape] Halls of Reflection escape: walls seen up / broken (bit per ice wall), and the last
        // wall broken (a boss_attempt_loop window starts there, not at the engage).
        uint8 horWallsUp = 0;
        uint8 horWallsBroken = 0;
        uint32 loopProgressMs = 0;
        uint32 loopProgressKills = 0;
        uint32 phantomAttemptMs = 0;
        uint32 phantomHp = 0;
        uint32 phantomSinceMs = 0;
        bool phantomLogged = false;
        Position homePos;  // where the boss stood when the attempt started (blacklist zone after a FAIL)
        std::map<ObjectGuid, uint32> attemptStartAggro;

        // Identity and route order (DungeonEncounter.dbc / instance_encounters / the InstanceScript's own guids)
        uint32 entry = 0;         // boss creature entry once known
        ObjectGuid creature;      // boss creature once known
        int32 order = -1;         // DungeonEncounter.dbc encounterIndex, -1 unknown
        bool dbc = false;         // tracked from DungeonEncounter.dbc: the InstanceScript keeps no boss states
        std::vector<uint32> credits;  // instance_encounters kill-credit creature entries (dbc tracks)
        bool engaged = false;     // a party member fought it during the current IN_PROGRESS
        uint32 npcResets = 0;     // IN_PROGRESS -> reset without the party (script NPCs engaging the boss)
        uint32 scriptedResets = 0;  // [b35_urom] IN_PROGRESS -> NOT_STARTED by the boss's own script (no wipe)
        uint32 nextResolveMs = 0; // ResolveBoss retry (a dbc lookup scans the map's creatures)
        // The boss creature when the current attempt was engaged (suspicious-kill check: heroic templates missing)
        uint8 engageLevel = 0;
        uint32 engageMaxHp = 0;
    };

    // One DungeonEncounter.dbc row of the run's map and difficulty.
    struct EncounterInfo
    {
        uint32 index = 0;  // encounterIndex (completed-encounter bit, also the dungeon's boss order)
        std::string name;
        std::vector<uint32> credits;  // instance_encounters kill-credit creature entries
    };

    // A hostile that left the tanks for a non-tank member (aggro_loss metric).
    struct LooseMob
    {
        ObjectGuid victim;
        uint32 sinceMs = 0;
        bool logged = false;
        bool boss = false;
        uint32 entry = 0;
        std::string name;
        std::string victimName;
        std::string victimRole;
    };

    struct StarterTrack
    {
        EventStarterDef const* def = nullptr;
        uint32 uses = 0;
        uint32 nextAllowedMs = 0;
        std::set<ObjectGuid> usedObjects;  // UseItem: every object takes the item once (five different barrels)
        // UseItem: casts the core refused per object (no cooldown started); after STARTER_ITEM_MAX_FAILS the object
        // counts as used, so a starter never loops on one object.
        std::map<ObjectGuid, uint32> failedUses;
        uint32 doorRearms = 0;  // UseItem on an auto-closing door: armed again for a member behind it (DOOR_REARM_MAX)
        // [queue-rearm-behind-priority] re-armed for a member stranded behind the door: the starter leads the route
        // (priority) until it is used again, at most REARM_BEHIND_PRIORITY_MS.
        uint32 rearmedBehindMs = 0;
        uint32 combatRearms = 0;   // of doorRearms, the in-combat ones (DOOR_REARM_COMBAT_MAX)
        uint32 combatRearmMs = 0;  // the last in-combat re-key (DOOR_REARM_COMBAT_GAP_MS)
        std::set<ObjectGuid> steppedOut;  // UseItem: one step out of the object's model, then the use from there
        // UseItem: an object the leader's walk got stuck on (steer_stuck) is passed over until then; the starter goes
        // on with the next nearest one (Old Hillsbrad: 15 barrels, any five).
        std::map<ObjectGuid, uint32> stuckUntilMs;
        // Party::wipes when it was last used: a gossip / escort starter used up before a wipe is armed again once its
        // creature offers the gossip again (the event reset: Halls of Reflection, Violet Hold).
        uint32 wipesAtUse = 0;
        // The object whose walk ends short of it was logged (starter_partial_walk), once per object.
        ObjectGuid partialLogged;
        // [expect_door] EventStarterDef::expectDoor: when the use is checked (0: none pending), the uses that left the
        // door shut, and the last "expect_door_wait" line.
        uint32 expectDoorCheckMs = 0;
        uint32 expectDoorFails = 0;
        uint32 expectDoorWaitLogMs = 0;
        uint32 unconfirmedLogged = 0;  // [ulda-altar-confirm] uses already logged as starter_unconfirmed
        std::string blockedLogged;     // [dme-starter-blocked] last starter_blocked reason (logged on change)
        uint32 doorRearms2 = 0;        // [dme-ironbark-door] re-arms because EventStarterDef::opensDoor stayed closed
        uint32 lastUseMs = 0;          // [dme-ironbark-door] when this starter was last used (UseStarter)
        bool clearLogged = false;      // [ulda-altar-clear] starter_wait_clear logged for the current wait
        uint32 escortDeathRearms = 0;  // [ohb-escort-death-rearm] uses given back after the escort died
    };

    struct TriggerTrack
    {
        uint32 triggerId = 0;
        uint32 fires = 0;
        uint32 lastFireMs = 0;
        bool entrance = false;  // instance portal outside the map (ghosts walking back in)
    };

    struct Party
    {
        uint32 seq = 0;
        std::string id;  // run id, or "P<seq>" for a prepared party
        Composition const* comp = nullptr;
        ContentDef const* content = nullptr;
        uint8 level = 80;
        uint32 targetIlvl = 0;
        std::vector<Member> members;
        ObjectGuid leader;
        ObjectGuid groupGuid;

        Phase phase = Phase::Picking;
        uint32 phaseSinceMs = 0;
        uint32 createdMs = 0;
        uint32 runStartMs = 0;
        uint32 nextTickMs = 0;
        uint32 timeoutMs = 0;
        std::string result;
        std::string reason;
        bool fromQueue = false;

        // instance
        uint32 instanceId = 0;
        // [hleft-daily-reset] when the server's global reset of this map / difficulty comes (InstanceSaveMgr), 0: none
        time_t instanceResetAt = 0;
        std::vector<Position> entrances;
        size_t entranceIdx = 0;
        uint32 teleportTries = 0;
        // Members found inside an earlier run's instance of the content map: ported out first, then unbound from it.
        std::map<ObjectGuid, uint32> staleInstances;

        // runner
        ObjectGuid target;
        bool targetIsStarter = false;
        size_t starterIdx = 0;
        bool targetIsBoss = false;
        // [route_blocker_required_only] the route rule that chose the target (Retarget's target log: "boss",
        // "required", "blocks route", ...); empty for a target set elsewhere (a neighbour, a route blocker)
        std::string targetRouteRule;
        uint32 targetSinceMs = 0;
        uint32 arrivedMs = 0;
        uint8 approach = 0;  // 0: pull range, 1: walk into the pack (body pull)
        // [lead3] .. [lead10] C:/WoW/LEADER_REFACTOR_PLAN.md section 9: the dungeon leader's own decision state (the
        // harness is the leader, the PullPlanner its library). World thread only.
        struct LeaderState
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
            std::map<std::string, uint32> vetoedEvals;  // "<member>|<what>" -> count (vetoed_eval)
            // [lead18] the issued pull's whole plan for the red view (latched at the issue)
            Position issuedFrom;
            Position issuedFight;
            Position issuedTargetAt;
            uint32 healerLostTankMs = 0;  // [lead18] healer_lost_tank_s
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
        LeaderState lead;
        // Steering progress towards the target: where the leader stood when steerSinceMs began (Steer resets it on
        // every 3 yd of walking). No progress for STEER_STUCK_MS: the route has nothing left to walk or the leader's
        // own move refuses the waypoint (Utgarde Pinnacle wave 8: 21 minutes under a Mindless Servant on a ledge).
        ObjectGuid steerTarget;
        Position steerAnchor;
        uint32 steerSinceMs = 0;
        // [hleft-swim-back] the leader's swim since it last stood on dry ground (the dry spot first, a point every
        // SWIM_TRAIL_STEP yd), and the walk back along it once the water holds it below its target.
        std::vector<Position> swimTrail;
        Position swimDry;
        bool swimDryKnown = false;
        bool swimBack = false;
        uint32 horHoldLogMs = 0;  // [hleft-hor-reentry-hold]
        uint32 swimBackSinceMs = 0;
        uint32 steerLastMs = 0;
        float steerNearest = -1.0f;  // nearest the leader came to the steered target (STARTER_NO_NEARER_MS)
        uint32 steerNoNearerMs = 0;  // steering time since then
        bool steerReachWaived = false;  // stuck in pull range and sight: pulled from where the leader stands
        // Straight walks over a navmesh gap (a ramp without polygons) towards gapTarget.
        ObjectGuid gapTarget;
        uint32 gapTries = 0;
        // Harness drive (ROUTE_HARNESS_DRIVE): the leader's own move made no progress towards driveGoal for
        // DRIVE_AFTER_MS (its MoveTo refuses a steep-route waypoint, or a dungeon strategy action that answers OK
        // every tick outranks "test runner move": Ahn'kahet's "use nerubian device", Culling's "cos dispel crate"),
        // so the harness walks it along the route itself, one short step at a time.
        Position driveGoal;
        Position driveAnchor;
        uint32 driveSinceMs = 0;
        uint32 driveLastMs = 0;
        bool driving = false;
        uint32 nextRetargetMs = 0;
        uint32 waypointSerial = 0;
        // ROUTE_CLOSED_DOORS_ALL: the leader's last waypoint ran through this closed door and was cut short before it
        // (door_hold, SetLeaderWaypoint); Retarget then drops a target that door does not open for (DoorOpensFor).
        ObjectGuid doorHold;
        uint32 doorHoldMs = 0;
        uint32 doorHoldLogMs = 0;
        // Meet <-> route ping-pong (TickParty, REGROUP_PING_PONG_*): the last decision out of combat (1 meet: the far
        // wait walks the leader back, 2 route: Retarget / Steer), its flips, and the meet hold they started.
        uint8 decision = 0;
        uint32 decisionFlips = 0;
        uint32 flipSinceMs = 0;
        uint32 lastFlipMs = 0;
        uint32 pingPongHoldUntilMs = 0;
        uint32 pingPongCooldownUntilMs = 0;
        std::map<ObjectGuid, uint32> skipUntil;
        std::map<ObjectGuid, std::string> skipReason;  // why skipUntil holds it (the run's missing-boss report)
        // Where the leader stood when a target was skipped as unreachable: "unreachable" holds from there only. Once
        // the leader is elsewhere (dropped into Azjol-Nerub's lower kingdom, crossed a bridge, took an elevator) the
        // skip is dropped and the target checked again.
        std::map<ObjectGuid, Position> unreachableFrom;
        // A boss of the route order found unreachable first at that time: for ORDER_HOLD_MS the bosses after it in
        // the order wait (Magisters' Terrace wave 8: Selin skipped, Delrissa pulled third-first and wiped twice).
        std::map<ObjectGuid, uint32> orderHoldSinceMs;
        std::vector<StarterTrack> starters;
        std::map<size_t, ObjectGuid> starterTargets;  // starter index -> object found by the last retarget
        std::vector<TriggerTrack> triggers;
        std::set<ObjectGuid> bossCreatures;  // dungeon bosses spawned when the run entered
        // Boss-ranked creatures left out of bossCreatures ("name (entry): reason", run_start): friendly or
        // non-attackable event NPCs, triggers, summons, rank-3 creatures outside the encounter data.
        std::vector<std::string> bossCreaturesSkipped;
        // Wing runs (ContentDef::bossEntries): where the wing's bosses and entrances are, and the bosses of the
        // other wings. A hostile nearer to another wing's boss than to anything of this wing is not a target.
        std::vector<Position> wingAnchors;
        std::vector<Position> otherWingAnchors;
        std::set<ObjectGuid> prePotted;
        std::set<ObjectGuid> bossBuffed;
        bool noTargets = false;
        // Event started, nothing hostile: the group walks to the content's gather spot (Halls of Reflection waves
        // start only with everybody within 40 yd of the altar). Logged once per no-target episode.
        bool eventGatherLogged = false;
        bool eventGatherArrived = false;
        uint32 eventGatherArrivedMs = 0;  // when the leader reached the gather spot (the stall watchdog waits)
        uint32 gatherCallMs = 0;  // last time members were called in to the gather spot
        uint32 noTargetsSinceMs = 0;
        // Escort (StarterKind::Escort): the group stays with this creature and fights only what is near it, until
        // it dies, despawns or stands idle out of combat for ESCORT_IDLE_MS (the walk is over).
        ObjectGuid escort;
        uint32 escortEntry = 0;
        uint32 escortSinceMs = 0;
        uint32 escortIdleSinceMs = 0;
        Position escortLastPos;
        // [event_boss_tbcwotlk] escort_stalled: the NPC in combat but not walking (ohb-h 20261025-163739: Thrall stood
        // in combat at 2207,121,89 for 10 min after the armory, nobody knew with whom)
        uint32 escortStillSinceMs = 0;
        Position escortStillPos;
        uint32 escortStallLogMs = 0;
        std::string escortNote;
        // [ohb-escort-follow] where an escort last ended "idle" (its creature stood there): not adopted again there
        ObjectGuid escortIdleEndGuid;
        Position escortIdleEndPos;
        // Kills per creature entry this run (EventStarterDef::afterDead, EventSpotDef conditions, event drops).
        std::map<uint32, uint32> deadEntries;
        std::map<uint32, uint32> eventItemsGiven;  // item id -> how many the harness put into the bags
        // WatchEventState: the last logged event values (-1: not read yet) and when they were read.
        int32 watchedShield = -1;
        int32 watchedRift = -1;
        uint32 watchedEventMs = 0;
        // [brd_ring_*] Blackrock Depths' Ring of Law (WatchEventState): the last TYPE_RING_OF_LAW read (-1 none),
        // failed runs, and when (game time, seconds) at_ring_of_law takes the trigger again after a FAIL (its 2-minute
        // lockout).
        int32 watchedRing = -1;
        uint8 ringFails = 0;
        int64 ringRetryAtSec = 0;
        // EventSpotDef::eventData: a failed run of the spot's event is armed again; the spot holds until it runs.
        bool eventSpotRetry = false;
        uint32 eventHoldLogMs = 0;  // the last "brd_ring_hold" line
        uint32 stepRefireLogMs = 0;  // [b32_pos] the last logged re-fire of a progress step's trigger
        // [guarded_object_progress] GuardedDoorDef with guarded objects (Dire Maul West's Crystal Generators): how many
        // are no longer in their guarded GOState, and the last check (every GUARDED_OBJECT_CHECK_MS).
        uint32 guardedObjectsDown = 0;
        uint32 guardedObjectCheckMs = 0;
        // [strat_key_*] KeyDoorPassDef (TickKeyDoorPass): the auto-closing key gate just used, since when, whether the
        // members were sent through, and the last "strat_key_gather" line.
        ObjectGuid keyPassDoor;
        uint32 keyPassSinceMs = 0;
        bool keyPassSent = false;
        uint32 keyPassLogMs = 0;
        // [b32_strat_gate] the pass's door was seen open (a shut door before that is the 5 s Opening still being
        // cast, not "a member left"), and who cast the key (a member's own Key to the City: MemberKeyAtDoor).
        bool keyPassOpened = false;
        ObjectGuid keyPassCaster;
        // [b32_strat_gate] a member cut off behind a closed door (its walk to the leader runs through it;
        // TickDoorCut, every 2 s): the group holds - no wait_timeout, no route, no new pull - while the door is keyed
        // again (the member's own key, or the leader's from its side).
        ObjectGuid doorCutMember;
        ObjectGuid doorCutDoor;
        uint32 doorCutCheckMs = 0;
        uint32 doorCutSinceMs = 0;
        uint32 doorCutLogMs = 0;
        bool doorCutHold = false;  // the hold is on (a key door: until keyed; another door: DOOR_CUT_OTHER_MAX_MS)
        uint32 memberKeyMs = 0;  // the last member key cast (MEMBER_KEY_GAP_MS)
        uint32 memberKeyUses = 0;
        uint32 trapGateLogMs = 0;  // [strat_trap_gate_*] the last strat_trap_gate_* line
        // [patrol_wait_spot] the planner holds for a patrol ("patrol wait"): the leader stands at a spot out of every
        // idle pack's reach until patrolWaitUntilMs (Steer), for that target; when the wait began and why (the pull
        // that follows names "patrol proximity aggro" as its cause, PullOpenerJson).
        uint32 patrolWaitUntilMs = 0;
        ObjectGuid patrolWaitTarget;
        uint32 patrolWaitMs = 0;
        std::string patrolWaitWhy;
        // EventSpotDef index (in EventSpots order) -> 1 the leader arrived, 2 no path (the spot is given up)
        std::map<size_t, uint8> eventSpotState;
        // RouteLinkDef the group crosses (index in RouteLinks order, -1 none): the leader walks to its start and
        // over it once the group is there, then every member the same way (CrossLinks).
        int32 linkIdx = -1;
        // [b30_reenter] TickShortcutReenter: the last check, and no new shortcut re-entry before this
        uint32 shortcutCheckMs = 0;
        uint32 shortcutCooldownUntilMs = 0;
        uint32 shortcutEncounterLogMs = 0;  // [b49_hos] the last "encounter in progress, no re-entry" log
        uint32 shortcutReentries = 0;
        // [b31_far_threshold] the leader's travel pause for a trailing member: since when, and when the last one ended
        uint32 compactPauseSinceMs = 0;
        uint32 compactPauseEndMs = 0;
        uint32 compactPauseTickMs = 0;  // [b33_harn] the pause's last evaluation (a gap restarts its clock)
        uint32 linkSinceMs = 0;
        // The walk to the link's start: when it last came closer (or fought on the way), and the best distance.
        uint32 linkProgressMs = 0;
        float linkBestDist = 0.0f;
        bool linkLeaderCrossed = false;
        bool linkWalkLogged = false;
        uint32 linkCrossLogMs = 0;  // the last "link_cross" line (the leader's position while it crosses)
        uint32 linkOffLineLogMs = 0;  // the last "link_off_line" line (LINK_OFF_LINE_BACK)
        // [queue-link-off-line-cap] the leader's walks back to the link's start in this crossing, and whether it is
        // off the line now (a return counts once, on the way off).
        uint32 linkOffLineReturns = 0;
        uint32 blockerGateLogMs = 0;  // [queue-blocker-pull-gate] the last "blocker_pull_gate" line of a body pull
        // [queue-wipe-during-boss] the boss whose engaged attempt ended (wipe / reset) last, and when: a wipe
        // logged in the same tick or right after it is that boss's, not "trash".
        std::string lastAttemptEndBoss;
        uint32 lastAttemptEndMs = 0;
        // [queue-stale-idle-victim] the leader's swing / target on an idle mob outside any fight: which and since when
        ObjectGuid staleVictim;
        uint32 staleVictimSinceMs = 0;
        uint32 staleVictimReleases = 0;
        bool linkOffLineNow = false;
        std::set<ObjectGuid> linkCrossed;
        std::map<int32, uint32> linkFails;  // RouteLinkDef index -> failed crossings (EndLink); LinkTo gives it up
        // [b30_wings] the target the current link was chosen for, and the link the leader last crossed (for whom,
        // when): LinkTo takes no other link for the same target that ends farther from it (no immediate reverse).
        ObjectGuid linkFor;
        int32 lastLinkIdx = -1;
        ObjectGuid lastLinkFor;
        uint32 lastLinkMs = 0;

        // combat
        bool inCombat = false;
        uint32 pullSeq = 0;
        uint32 pullStartMs = 0;
        uint32 lastPullEndMs = 0;  // [pull_label] when the last pull_end was logged (getMSTime, 0: none yet)
        std::string pullStarter;  // pull_start: "planned" or "unplanned: <name> <spec>/<role>"
        // [lead1] a pull the dungeon's script started, not a pull decision ("chain", "vehicle", "summon", "escort";
        // empty: none known): the decision log's pull_start names it who "event" instead of "none"
        std::string pullScript;
        bool pullIsBoss = false;
        uint32 pullKills = 0;
        // [b30_mech_pos] trash_stalemate: the pull this watch belongs to, since when nothing died and no enemy reached
        // a new lowest health, the kills then, each enemy's lowest health % seen, and whether the focus was set
        uint32 stalemateSeq = 0;
        uint32 stalemateSinceMs = 0;
        uint32 stalemateKills = 0;
        std::map<ObjectGuid, float> stalemateLowPct;
        bool stalemateFocused = false;
        std::vector<std::string> pullDeaths;
        std::vector<BossTrack> bosses;
        uint32 kills = 0;
        uint32 windowKills = 0;
        uint32 wipes = 0;
        bool wiped = false;
        uint32 wipeMs = 0;
        // [b30_st_spirit] deaths of the run (not re-entries) within DEATH_BURST_WINDOW_MS, and when they first reached
        // DEATH_BURST_COUNT: no recovery within DEATH_BURST_RECOVER_MS after that counts as a wipe
        std::deque<uint32> recentDeathMs;
        uint32 deathBurstMs = 0;
        uint32 deathBurstCount = 0;
        uint32 corpseRunMs = 0;
        uint32 combatSampleMs = 0;  // last combat-time sample (Member::combatMs)
        std::vector<EncounterInfo> encounters;  // DungeonEncounter.dbc rows, encounterIndex order
        std::map<ObjectGuid, LooseMob> looseMobs;
        uint32 aggroLosses = 0;
        uint32 lastPositionsMs = 0;  // "positions" event, every POSITIONS_MS in combat
        // last place the leader stood on dry ground out of combat inside the instance: the group rests there, never
        // in water (Underbog, Blackfathom Deeps: bots drank at the lake bottom and drowned)
        Position dryPos;
        bool hasDryPos = false;
        // members dead or outside the instance while the rest of the group is not wiped
        bool membersAway = false;
        uint32 awaySinceMs = 0;
        // Boss progress (a boss killed or failed); no progress for NO_BOSS_PROGRESS_*_MS fails the run (user rule:
        // nothing killed for that long is a failure to investigate, not a slow run).
        uint32 lastBossProgressMs = 0;
        uint32 bossProgressCount = 0;
        bool friendlyOnlyLogged = false;  // [w24c-no-done-by-exclusion] logged once per run
        uint32 killsAtBossProgress = 0;
        std::map<uint32, uint32> killsSinceBoss;  // creature entry -> kills since the last boss progress
        // Trash loop: kills go on inside TRASH_LOOP_RADIUS of loopAnchor for TRASH_LOOP_MS without boss progress
        // (reviving or endless trash: Nexus Frayers, Hadronox's adds) -> the area is skipped, the next boss is the
        // target until bossRushUntilMs.
        Position loopAnchor;
        uint32 loopSinceMs = 0;
        uint32 loopKills = 0;
        // [trash_loop_progress] the alive spawned hostiles round loopAnchor when the window began (loopAliveSinceMs:
        // the loopSinceMs it was counted for)
        uint32 loopAliveAtStart = 0;
        uint32 loopAliveSinceMs = 0;
        uint32 bossRushUntilMs = 0;
        uint32 releasedIdleSinceMs = 0;  // Violet Hold: since when a released prisoner stands unfought
        // How often each creature came back as the target after it had died (a reviving / respawning mob), and the
        // chosen creatures seen dead since (Retarget). Choices of a mob that never died do not count: dme-n
        // 2026-10-25 skipped a living Imp / Fel Lash / Whip Lasher as "respawning trash" after four refusals.
        std::map<ObjectGuid, uint32> targetCounts;
        std::set<ObjectGuid> targetDied;
        // Careful-pull refusals per pack member (heroic / level 80: deferred twice, then body-pulled).
        std::map<ObjectGuid, uint32> carefulRefused;
        // The idle mob outside the target's pack whose aggro reach ended the leader's approach (Steer), if any.
        ObjectGuid approachBlocker;
        // The last skipped neighbour pack that stopped an approach (ROUTE_SKIP_BEHIND_SKIPPED), for the idle guard.
        std::string behindSkipped;
        // ROUTE_STOP_KEEPS_WAIT: where the leader stands waiting for a pull and since when, whatever the target (a
        // retarget to another mob of the same pack, or behind the same blocker, keeps the clock).
        Position stopPos;
        uint32 stopSinceMs = 0;
        uint32 stopLastMs = 0;
        ObjectGuid assisting;  // the member the leader walks to (AssistMember), logged once per episode
        ObjectGuid meeting;    // the member the leader walks back to (MeetMember), logged once per episode
        std::set<uint32> endlessSummons;  // summoned entries no longer route targets (logged once each)
        // Combat with nothing happening: no damage done or taken and no healing by anybody of the party since
        // phantomCombatSinceMs (an unreachable mob on a ledge keeps everybody flagged, Ragefire Chasm 18 minutes).
        uint64 combatActivity = 0;
        uint32 phantomCombatSinceMs = 0;
        bool phantomCombatLogged = false;
        bool phantomCombatEnded = false;
        // [b33_sh2] when the stale refs were last ended and how often in this phantom episode: the end repeats every
        // PHANTOM_COMBAT_END_MS while the flag stays (sh-n 5B-89: one end at 1421 s, the tank flagged again, 90 s idle)
        uint32 phantomCombatEndedMs = 0;
        uint32 phantomCombatEnds = 0;
        // [b33_sh2] the last harness_hold event (Steer holds the walk with a target set), its reason, throttled
        uint32 harnessHoldLogMs = 0;
        std::string harnessHoldWhy;
        uint32 noWalkSinceMs = 0;     // [b33_sh2] target set, traveling, no waypoint and the leader standing since
        uint32 noWalkLogMs = 0;       // [b33_sh2] the last harness_hold "no walk" event
        // [phantom_mob_hunt] the live, reachable mob of a phantom combat the group was sent to, and how often
        ObjectGuid phantomHunt;
        uint32 phantomHuntTries = 0;

        // buffing
        bool needBuff = true;
        uint8 buffStep = 0;
        uint32 buffStepMs = 0;
        uint32 buffSinceMs = 0;  // the buffing pass started (BUFF_TOTAL_MAX_MS)
        std::string buffReason = "run start";

        // waits
        uint32 waitSinceMs = 0;
        uint32 waitCooldownUntilMs = 0;
        std::string waitingFor;

        // heartbeat / stall
        uint32 lastHeartbeatMs = 0;
        Position heartbeatPos;
        uint32 lastProgressMs = 0;
        Position progressPos;
        uint32 progressMapId = 0;
        uint8 stallLevel = 0;
        uint32 stuckEvents = 0;
        // Idle guard (CheckIdle, user rule 2026-10-25: two minutes without a fight is a blocker): time without a fight
        // outside rest / buffing (idleMs) and in genuine event waits (eventWaitMs), each logged once per episode.
        uint32 idleTickMs = 0;
        uint32 lastStarterUseMs = 0;
        // ROUTE_DETOUR_FALLBACK: until then detours are targets too (everything left was a detour: the long way).
        uint32 detourFallbackUntilMs = 0;  // the last event_start (an event the group works through: CoS crates)
        uint32 rearmCheckLogMs = 0;  // ReopenKeyDoorHeld: the last "rearm_check" line (a failed re-key, its reason)
        // trash_phantom (build 18): the health of the mobs the party fights (no boss attempt), and since when no mob
        // lost any (or the set changed).
        uint64 trashHp = 0;
        uint32 trashMobs = 0;
        // Boss approach held at a neighbour pack (neighbourStop): the boss and how often in a row.
        ObjectGuid neighbourHoldBoss;
        uint32 neighbourHolds = 0;
        ObjectGuid neighbourTarget;  // boss_neighbour_target: kept as the target (Retarget) until NEIGHBOUR_TARGET_MS
        uint32 neighbourTargetUntilMs = 0;
        // route_blocker_body_pull: the pack that stands on the way to the route's target (its walk enters the pack's
        // reach) is the target, and pulled - by sight, else walked in - never skipped while it is on the way.
        std::vector<ObjectGuid> routeBlockerPack;
        uint32 routeBlockerUntilMs = 0;
        std::map<ObjectGuid, uint32> routeBlockerTries;  // by blocker: how often it was made the target
        uint32 trashFlatSinceMs = 0;
        bool trashPhantomLogged = false;
        uint32 idleMs = 0;
        uint32 eventWaitMs = 0;
        // ROUTE_CAREFUL_GUARANTEE_HOLD: the pack the tank refused (not hopeless) the group holds at for the planner's
        // stand guarantee (CarefulPlanVerdict::GetRefusal), and when that guarantee came due (0: not yet).
        ObjectGuid guaranteeTarget;
        uint32 guaranteeDueMs = 0;
        bool guaranteeHolding = false;
        // [held_for_now_cap] the target the planner answers "held for now" for, and since when (0: none)
        ObjectGuid heldForNowTarget;
        uint32 heldForNowSinceMs = 0;
        bool idleLogged = false;
        bool eventWaitLogged = false;
        std::string lastEvent;
        // Bots walk by themselves (HARNESS_MOVES_BOTS): moves the harness still made (0 when off), and what the
        // bots' own movement could not do (bot_cannot_move, throttled per bot, capability and reason).
        uint32 harnessMoves = 0;
        uint32 cannotMoveEvents = 0;
        std::map<std::string, uint32> cannotMoveLoggedMs;
        // The leader's waypoint stays while a starter step-out is walked (the no-target branch would clear it).
        uint32 waypointHoldUntilMs = 0;
        // The leader casts an event item on its object (Old Hillsbrad's 5 s bomb): no retarget and no waypoint until
        // then, a new walk would interrupt the cast.
        uint32 castHoldUntilMs = 0;
        // [b51_strat] TickGateTrapGather: the group gathers inside a Stratholme rat-trap alley before anyone walks over
        // the armed trap point; no retarget and no leader waypoint until then (refreshed every tick while it holds).
        uint32 trapHoldUntilMs = 0;
        bool doorGatherHeld = false;  // [b51_strat] the last DoorGatherHolds held the use (no progress)
        uint32 ghostPosMs = 0;  // last "ghost_pos" event
        // A wing change walked (Scarlet Monastery, Dire Maul, Maraudon): out through the exit, in through the next
        // entrance, every member on its own (SelfTravel portal goal).
        bool wingWalk = false;
        uint32 wingWalkSinceMs = 0;
        bool gapsLogged = false;

        // Results that cannot be trusted (a boss far below the group's level, killed much faster than its health
        // allows): one line per boss, "suspicious" event + run_end.suspicious.
        std::vector<std::string> suspicious;
    };

    // The raid leader layer (src/Ai/Base/Leader) pre-pots and uses raid consumables itself in raids.
    bool LeaderLayerHandlesConsumables(Party const& party);
    // What the boss of that track needs to appear (TestHarnessData.cpp encounterRequirements), nullptr if nothing.
    char const* TrackRequirement(uint32 mapId, BossTrack const& boss, char const** note = nullptr);
    // User rule: a boss only one class can unlock (ClassGatedBossDef) is the only one a run may leave alive; its
    // reason, nullptr for every other boss. Such a track is not required for "done" and is listed as excluded.
    char const* TrackClassGate(Party const& party, BossTrack const& boss);
    // A script slot that is no boss: EncounterCount above the real boss count (Blood Furnace 4 for 3 bosses). No
    // dbc row at its index, never named, no creature credit, state never moved. Not required for "done"; listed in
    // run_start.phantomSlots. A state change makes it a normal required track again.
    bool IsPhantomSlot(Party const& party, BossTrack const& boss);
    // Bosses the run must kill: every track but phantom slots and class-gated bosses (bossesTotal in the logs).
    uint32 RequiredBossCount(Party const& party);
    bool HasFlaskAura(Player* bot);
    bool HasWellFed(Player* bot);
    // Combat-time fields of one member between two snapshots: activeSec, activeDps, activeHps, activeDtps.
    JsonObject CombatStatsJson(MemberCounters const& now, MemberCounters const& start, uint32 combatMs);
    // Sum of ExpectedMemberDps over the party (each member's own level and item level).
    float ExpectedGroupDps(Party const& party);
    // [b30_reenter] the member's last recorded intention ("doing", "goingTo" with its movement source, "why"),
    // a JSON object, or "null"
    std::string MemberIntentJson(ObjectGuid guid);

    struct PoolCharacter
    {
        ObjectGuid guid;
        std::string name;
        uint8 cls = 0;
        uint32 account = 0;
        uint8 level = 1;
        bool leveling = false;  // "Rl" name: kept for content below level 80
        bool horde = false;     // a Horde race (Horde compositions only)
    };

    class TestHarnessMgr
    {
    public:
        static TestHarnessMgr& Instance();

        void Update(uint32 diff);
        void Shutdown();

        bool HandleSetup(ChatHandler* handler, uint32 groups, bool leveling, bool variants, bool horde = false);
        bool HandlePrepare(ChatHandler* handler, std::string const& compId, uint8 level, uint32 ilvl);
        bool HandleRun(ChatHandler* handler, std::string const& contentKey, uint32 count, std::string const& compId);
        void SetRunAllRandomComp(bool on) { _runAllRandomComp = on; }
        bool HandleRunAll(ChatHandler* handler, std::string const& set, uint32 parallel, bool variants,
                          uint32 window = 10);
        bool HandleStatus(ChatHandler* handler);
        bool HandleStop(ChatHandler* handler, std::string const& id);
        bool HandleList(ChatHandler* handler);

        bool IsTestCharacter(ObjectGuid guid) const;

        // World thread, read only ("playerbots watch"): every party, terminal phases included.
        std::vector<std::unique_ptr<Party>> const& Parties() const { return _parties; }

        // Route visualisation ("playerbots route show|hide", TestHarnessRunner.cpp): GM-only waypoint markers
        // (VISUAL_WAYPOINT) along the route backbone Retarget built last, the leader's walk, the target's route and
        // columns on the waypoint, the target and the next boss, in the run's instance; redrawn every
        // ROUTE_VIZ_REFRESH_MS for <seconds>. World thread only: commands and TickRouteViz run from World::Update
        // after the map updates finished (MapMgr::Update waits for its threads), so no map thread runs meanwhile.
        bool HandleRouteShow(ChatHandler* handler, uint32 seq, uint32 seconds);
        bool HandleRouteHide(ChatHandler* handler, uint32 seq);
        void TickRouteViz();

    private:
        TestHarnessMgr() = default;

        struct QueueItem
        {
            ContentDef const* content;
            Composition const* comp;
        };

        struct SetupJob
        {
            bool active = false;
            uint32 stage = 0;
            uint32 startedMs = 0;
            std::map<uint8, uint32> missing;  // class -> characters to create
            std::vector<std::string> accounts;  // accounts that must exist before characters are created
            bool leveling = false;              // new characters go to the levelling pool ("Rl" names)
            bool horde = false;                 // new characters get Horde races (RaceForClass)
        };

        // pool
        bool LoadPool(std::string& error);
        void TickSetup();
        bool CreateCharacter(uint32 accountId, uint8 cls, std::string& error);
        std::string NextName(uint8 cls, char const* prefix);
        // Characters per class for <groups> parallel groups of every listed size (the maximum over the
        // compositions of a size; only the optimal ones unless variants).
        std::map<uint8, uint32> RequiredPerClass(uint32 groups, std::vector<uint8> const& sizes,
                                                 bool variants) const;
        // Free pool character of that class for a party of that level: never one above the level; for level 80
        // the level-80 pool only (an 80 first), below 80 the highest level first (levelling pool on a tie).
        PoolCharacter const* PickCharacter(uint8 cls, uint8 level, std::set<ObjectGuid> const& picked,
                                           bool horde = false) const;

        // parties
        Party* CreateParty(Composition const* comp, ContentDef const* content, uint8 level, uint32 ilvl,
                           std::string& error);
        void TickParty(Party& party);
        void SetPhase(Party& party, Phase phase);
        bool TickLogin(Party& party);
        bool TickPrepare(Party& party);
        bool TickGrouping(Party& party);
        bool TickTeleport(Party& party);
        void TickRun(Party& party);
        void Finish(Party& party, std::string const& result, std::string const& reason);
        void ReleaseParty(Party& party);
        void AssignProfessions(Party& party);
        void ApplyStrategies(Party& party);
        void GrantAccess(Party& party, Player* bot);
        void EnterInstance(Party& party, Player* leader);

        // runner
        void Retarget(Party& party, Player* leader);
        void Steer(Party& party, Player* leader);
        // [b46_patrol] the group steps back out of a patrol's way (TestHarnessRunner.cpp PATROL_STEP_BACK): true while
        // the leader walks there or stands there
        bool PatrolStepBack(Party& party, Player* leader);
        void SkipTarget(Party& party, ObjectGuid guid, uint32 ms, std::string const& reason);
        // route_blocker_body_pull: `blocker`'s pack stands on the way to `behind`: it becomes the target (false when
        // it may not: a boss of it, tried too often, nothing alive).
        bool TargetRouteBlocker(Party& party, Player* leader, Creature* blocker, WorldObject const* behind,
                                std::string const& why);
        bool IsRouteBlocker(Party const& party, ObjectGuid guid) const;
        // [route_blocker_required_only] `behind` is a required target (a boss or his encounter mob, an event starter
        // or keeper, a door guard, a gate / link / starter key): only a pack on the way to one is never skipped.
        // `why` names the rule.
        bool RouteBlockerBehindRequired(Party const& party, Map* map, WorldObject const* behind,
                                        std::string& why) const;
        // The route blocker that is the target is pulled by walking into it once the group is fully ready.
        void RouteBlockerBodyPull(Party& party, Player* leader, Creature* target, std::string const& why);
        // [queue-blocker-pull-gate] One gate in front of the unseen-blocker pull, TargetRouteBlocker and the body
        // pull: "" when the pull may go on; else why not, with `hold` true when the group only has to get ready.
        std::string BlockerPullGate(Party& party, Player* leader, Creature* blocker, WorldObject const* behind,
                                    bool bodyPull, bool& hold) const;
        bool UseStarter(Party& party, Player* leader, StarterTrack& starter, WorldObject* object);
        void ReopenKeyDoorInCombat(Party& party, Player* leader);
        void ReopenKeyDoorHeld(Party& party, Player* leader);
        // [expect_door] EventStarterDef::expectDoor: a use whose door is still shut EXPECT_DOOR_MS later is no use -
        // armed again.
        void CheckExpectedDoors(Party& party, Player* leader);
        // [strat_key_pass] KeyDoorPassDef: the group through an auto-closing key gate at once (Stratholme).
        void TickKeyDoorPass(Party& party, Player* leader);
        // [b32_strat_gate] who is cut off behind a closed key door (Party::doorCutMember), and the recovery: the
        // cut-off member uses its own key on the door from its side when nothing hits it.
        void TickDoorCut(Party& party, Player* leader);
        void MemberKeyAtDoor(Party& party, Player* leader);
        void EmulateAreaTriggers(Party& party, std::vector<Player*> const& players);
        void GuideGhosts(Party& party, std::vector<Player*> const& players);
        // An event was started and nothing hostile is left: walks the leader to the content's gather spot. True
        // while walking (the caller does nothing else), false at the spot or when no gather spot applies.
        bool WalkToEventGather(Party& party, Player* leader);
        bool EventGatherApplies(Party const& party) const;
        // A walked (HARNESS_MOVES_BOTS: teleported) change to entrance `next`: every member's portal goal, the skips
        // cleared, wing_change logged with the reason.
        void StartWingChange(Party& party, size_t next, char const* why);
        // Proximity-started events (EventSpotDef): with nothing hostile reachable, the leader walks to the first spot
        // whose conditions hold. True while walking; false at the spot or when no spot applies.
        bool WalkToEventSpot(Party& party, Player* leader);
        // Index of the event spot that applies now (EventSpots order), -1 for none.
        int32 EventSpotIndex(Party const& party) const;
        // Route links (RouteLinkDef): the link over which target is reachable from the leader (it becomes the
        // party's link), -1 for none. The leader walks to it and over it (WalkToLink, true while it does); every
        // other member follows over the same link (CrossLinks, every tick).
        int32 LinkTo(Party& party, Player* leader, WorldObject* target);
        bool LinkUsable(Party const& party, Map* map, RouteLinkDef const& link) const;
        bool WalkToLink(Party& party, Player* leader);
        void CrossLinks(Party& party, std::vector<Player*> const& players, Player* leader);
        void EndLink(Party& party, char const* reason);
        // The leader (the tank) out of combat while a member fights far away or on another floor: true while it
        // walks to that member (the caller then sets no other waypoint).
        bool AssistMember(Party& party, std::vector<Player*> const& players, Player* leader);
        // Out of combat, a living member far behind (back from a corpse run): true while the leader (and with it the
        // group) walks to meet it, instead of letting it cross the idle packs alone.
        bool MeetMember(Party& party, std::vector<Player*> const& players, Player* leader);
        // Out of combat, farther than MEET_RANGE, no complete navmesh walk to the leader nor back, and staying put:
        // nobody waits for it or walks to it (re-checked, it rejoins when it can walk).
        bool MemberStranded(Party& party, Player* bot, Player* leader);
        // [b30_topped_up] true: the member was put back at the instance entrance (counted as a death)
        bool TickStrandedReenter(Party& party, Member& member, Player* bot, Player* leader);
        // [b30_reenter] re-entry as a route choice (no death): true when the group or a member was put at an entrance
        bool TickShortcutReenter(Party& party, std::vector<Player*> const& players, Player* leader);
        void ShortcutReenter(Party& party, std::vector<Player*> const& movers, size_t entrance, char const* why,
                             float walk, float entranceWalk, std::string const& goal);
        // ROUTE_HARNESS_WALK_GUARD: a harness walk held at the aggro edge of an idle pack (harness_hold_refused_pack),
        // with the tank's careful plan verdict on it; once per 15 s per blocker.
        void LogHarnessHold(Party& party, Player* leader, Creature* blocker, char const* walk, float x, float y,
                            float z);
        // The escorted creature while the escort runs (nullptr ends it, logged once as "escort_end").
        Creature* ActiveEscort(Party& party, Map* map);
        // With an escort running: true while the leader walks back to the escorted creature (the caller then does
        // nothing else).
        bool StayWithEscort(Party& party, Player* leader);
        // Creatures of all these entries are dead (killed by the party, or a boss track naming them is done).
        bool EntriesDead(Party const& party, std::vector<uint32> const& entries) const;
        // Puts the event items of the content (EventItems) into the leader's bags, logged as "event_item": the
        // carried ones at the start (atStart), the dungeon drops once their creatures are dead (every tick).
        void PrepareEventItems(Party& party, Player* leader, bool atStart);
        // The starters naming these entries are used up (EventStarterDef::afterUsed).
        bool StartersUsed(Party const& party, std::vector<uint32> const& entries) const;
        // A gossip / escort starter the bots' own dungeon strategy already did counts as used up: the item that only
        // its gossip hands out (the UseItem starter after it names it in afterUsed) is in the leader's bags and the
        // harness never put it there (Culling of Stratholme: the CoS strategy's own Chromie talk gave the Arcane
        // Disruptor, the harness chain waited for its own Chromie use). Logged as "starter_done_by_group".
        void MarkStartersDoneByGroup(Party& party, Player* leader);
        // Black Morass: Medivh's shield and the rift counter (instance data), logged when they change, so a failed
        // event (shield 0: Medivh dies, everything despawns, cleanup 5 minutes later) is not read as "not started".
        void WatchEventState(Party& party, Map* map);
        // The leader can open the game object honestly: a lock that needs a key item needs that item in the bags
        // of a member, a lock that needs a skill (lockpicking, herbalism, ...) the skill. Generic locks are open.
        bool CanOpenLock(Party const& party, GameObject const* go) const;
        bool UseStarterItem(Party& party, Player* leader, StarterTrack& starter, GameObject* go);
        // [b51_strat] User 2026-10-26: before a door that closes again by itself (door / button autoCloseTime, or a
        // KeyDoorPassDef gate) is opened, every living member who can come stands within DOOR_GATHER_RANGE of the
        // opener, out of combat; true while it holds (the members are walked to the opener). No cap: a member who
        // cannot come is a stall for the stall recovery, the door is never opened early.
        bool DoorGatherHolds(Party& party, Player* opener, GameObject* door);
        // [b51_strat] the rat-trap alleys of Stratholme: the group walks over an armed trap point together.
        void TickGateTrapGather(Party& party, Player* leader);
        // [brd_bar_buy] [brd_bar_turnin] StarterKind::VendorBuy / QuestTurnIn at its creature; false: not done.
        bool UseStarterAtNpc(Party& party, Player* leader, StarterTrack& starter, Creature* creature);
        // Bots walk by themselves: the event for what a bot's own movement could not do (throttled), the reports the
        // bots' own actions left (SelfTravel::TakeReport), intent for a member, and the walked wing change.
        void LogCannotMove(Party& party, Player* bot, std::string const& capability, std::string const& reason,
                           float toX, float toY, float toZ);
        void DrainMoveReports(Party& party, std::vector<Player*> const& players);
        // [lead1] The tank's pull order reports (Leader::Orders) into the decision log, the order states the leader
        // observes (Fighting, Done), the pull_start owner line; [lead2] order_orphan.
        void DrainOrderReports(Party& party, Player* leader);
        // [lead7] C:/WoW/LEADER_REFACTOR_PLAN.md step 7, LEADER_PLANNER_IN_LEADER: the leader plans the tank's pull
        // (PullPlanner::Decide on the world thread, every world update, when the tank asked) and issues the decision;
        // a hopeless refusal is the planner_no_option bug event.
        void TickLeaderPlanner(Party& party);
        // [lead3] .. [lead10] C:/WoW/LEADER_REFACTOR_PLAN.md section 9: the leader's own decisions around its planner.
        // The leader decides this party's pulls (LEADER_PLANNER_IN_LEADER, a tracked 5-man tank; raids keep the old
        // harness pulls).
        bool LeaderDecides(Party const& party) const;
        // [lead6] the leader's one readiness verdict ("" = ready): the group here and topped up, nobody buffing,
        // sitting or preparing (PullReadiness::GroupPreparing)
        std::string LeaderReadyWhy(Party& party, Player* leader);
        // [lead3] the pack the leader must clear: asked of its planner whole (walk-in priced by the planner); the
        // leader holds where it stands meanwhile. `required`: no option for it is planner_no_option with a stop.
        void LeaderWholePack(Party& party, Player* leader, Creature* pack, std::string const& why, bool walkIn,
                             bool required);
        // [lead3] a boss pull: a BossPull order at the tank's next ask (PullPlanner::BossPullDecision)
        void LeaderBossPull(Party& party, Player* leader, Creature* boss, std::string const& why);
        // [lead3] the pack must be cleared (a route blocker, the only way to a boss / event / required target, a
        // boss's area trash); `why` names the rule
        bool LeaderNeedsPack(Party const& party, Map* map, WorldObject const* object, std::string& why) const;
        // [lead3] user decision 1: planner_no_option, once per pack; `stops` ends the run (LEADER_STOP_ON_NO_OPTION)
        // true: reported now (a pack reported before is not again)
        bool LeaderNoOption(Party& party, Player* leader, Unit* mob, std::vector<ObjectGuid> const& pack,
                            std::string const& why, bool stops, JsonObject fields);
        // [lead4] the one patrol owner: the harness's hold / lane hold / step-back and the planner's patrol waits as
        // one decision, a declared wait or an intercept; true: the leader holds this tick
        bool LeaderPatrolTick(Party& party, Player* leader);
        // [lead15] the leader's events to every member (LeaderOrders.h Events), every LEADER_EVENT_TICK_MS
        void TickLeaderEvents(Party& party, Player* tank);
        // [lead15] the spots of a pull (false: no gate for it) and what the pull still waits for ("" = everyone there)
        bool LeaderAssignSpots(Party& party, Player* tank, PullPlanner::Decision const& decision);
        std::string LeaderSpotsWait(Party& party, Player* tank);
        void LogHealReach(Party& party, Player* member, Player* healer, std::string const& why,
                          std::string const& fix);
        // [lead5] the leader planner's own verdict memory, aoe plan and required marks, the tank's view with the
        // leader's route target: free helpers in TestHarnessRunner.cpp (PartyRefusal, PartyVerdict, PartyAoePlanned,
        // PartyMarkRequired, PartyTankView).
        void SetMemberWaypoint(Party& party, Member& member, Player* bot, float x, float y, float z);
        // The leader's waypoint, cut before a closed door its walk runs through (ROUTE_CLOSED_DOORS_ALL: door_hold).
        // straight: a walked route link's far end (TestRunner::Waypoint::straight).
        void SetLeaderWaypoint(Party& party, Player* leader, float x, float y, float z, bool straight = false);
        void ClearMemberWaypoint(Member& member);
        void ClearMemberIntent(Party& party);
        bool TickWingWalk(Party& party, std::vector<Player*> const& players, Player* leader);
        // Diagnostic at a pull start: no alive healer within PULL_HEALER_RANGE of the tank (pull_without_healer).
        void CheckPullHealer(Party& party, std::vector<Player*> const& players);
        // An alive member outside the instance with no walkable route to the portal flies there on its own flying
        // mount (riding and mount come from the preparation). False when it cannot fly here.
        bool FlyToPortal(Party& party, Member& member, Player* bot, AreaTrigger const* portal);
        // Breathing: a member under water whose breath is low swims straight up to the surface (in a fight too) and
        // goes on once it has breathed; the group rests on the last dry spot, never in the water (resting).
        void LeaveWater(Party& party, std::vector<Player*> const& players, bool resting);
        void LoadEncounters(Party& party, Map* map);
        void ResolveBoss(Party& party, BossTrack& boss, Map* map, InstanceScript* script);
        // [b31_dg] a script slot whose encounter opens with a gauntlet of adds (no wave-encounter fallback)
        bool GauntletSlot(Party const& party, BossTrack const& boss) const;
        // Removes the DungeonEncounter.dbc tracks a resolved script track already covers (same credit creature,
        // encounter order or name).
        void DropDbcTwins(Party& party);
        int32 BossOrder(Party const& party, Creature const* creature) const;
        // [sm-wing-done] Several entrances (Scarlet Monastery, Dire Maul): true while a living boss stands in the wing
        // of Party::entranceIdx (its nearest entrance); nextWing: the entrance of the lowest-order boss left elsewhere.
        bool WingBossesLeft(Party const& party, Map* map, Player* leader, int32& nextWing) const;
        void TrackAggro(Party& party, std::vector<Player*> const& players);
        void CloseLooseMob(Party& party, ObjectGuid guid, LooseMob& mob, char const* end);

        // buffs and consumables
        void TickBuffing(Party& party, std::vector<Player*> const& players, Player* leader);
        bool UseConsumables(Party& party, Player* bot, bool food);  // true: it used a flask / food
        void PrePot(Party& party, std::vector<Player*> const& players);
        bool UseItem(Player* bot, uint32 itemId, uint32 spellOverride = 0);
        bool UseEquipped(Player* bot, uint8 slot, uint32 spellId);

        // logging
        void LogEvent(Party& party, std::string const& type, JsonObject fields);
        void LogRunStart(Party& party);
        void LogPull(Party& party, std::vector<Player*> const& players, bool boss, std::string const& bossName);
        void LogAttemptEnd(Party& party, BossTrack& boss, std::string const& result);
        std::string PullMembersJson(Party& party);
        // Who started the fight (StatsRegistry::Opener): pull_start, and again in pull_end with every entry.
        std::string PullOpenerJson(Party& party, std::vector<Player*> const& players, bool atStart);
        void LogPullStart(Party& party, std::vector<Player*> const& players);
        void Heartbeat(Party& party, std::vector<Player*> const& players, Player* leader);
        void LogPositions(Party& party, std::vector<Player*> const& players);
        void CheckStall(Party& party, std::vector<Player*> const& players, Player* leader, bool anyInCombat);
        // The idle guard: no fight for IDLE_GUARD_MS outside rest / buffing (IDLE_EVENT_WAIT_MS in a genuine event
        // wait) logs "[idle] <run>: no fight for N s: <components holding>" once, with a diagnostic snapshot.
        void CheckIdle(Party& party, std::vector<Player*> const& players, Player* leader, bool anyInCombat);
        // What holds the group now (the idle guard's text): phase, waits, link, target, skips, holds.
        std::string IdleHolders(Party& party, Player* leader);
        // The genuine event wait the group stands in (an event hold spot between waves, the Halls of Reflection
        // gather, Svala's intro, an escort), empty if none.
        std::string IdleEventWait(Party& party, Player* leader);
        std::string MemberBuffJson(Party& party, Player* bot);
        std::string MemberDiagnosisJson(Party& party, Player* bot, Player* leader);
        std::string PhaseLabel(Party const& party) const;

        void StartQueued();
        Party* FindParty(std::string const& id);
        bool AllBossesResolved(Party& party, Map* map);
        void WatchHorEscapeWalls(Party& party, Map* map, BossTrack& boss, Player* leader, uint32 now);
        // Every boss of the run that is not dead, with the reason (bossesMissing, boss_missing).
        JsonArray MissingBosses(Party& party, std::vector<std::string>* lines);

        // Clean-run streaks per content key (<LogsDir>/tests/streaks.json, user rule 2026-10-24): runall puts a
        // content with STREAK_CLEAN_RUNS consecutive clean runs (done, no boss missing, no death, no wipe) at the end
        // of the queue, untested or not-yet-clean contents first. Every finished run but "stopped" updates it; a
        // missing file is seeded once from the run logs since STREAK_SEED_FROM.
        struct StreakInfo
        {
            uint32 streak = 0;
            std::string lastResult;
            std::string lastTime;
        };
        void LoadStreaks();
        void SaveStreaks() const;
        void RecordStreak(std::string const& contentKey, bool clean, std::string const& result,
                          std::string const& time);
        std::map<std::string, StreakInfo> _streaks;
        bool _streaksLoaded = false;

        std::vector<std::unique_ptr<Party>> _parties;
        std::deque<QueueItem> _queue;
        uint32 _queueParallel = 8;
        bool _runAllRandomComp = false;  // [runall-random-comp]
        uint32 _nextSeq = 1;
        std::vector<PoolCharacter> _pool;
        bool _poolLoaded = false;
        std::set<ObjectGuid> _inUse;
        SetupJob _setup;
        uint32 _setupTimer = 0;
        uint32 _queueTimer = 0;
    };
}

#endif
