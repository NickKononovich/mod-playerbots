/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_GROUPROUTESTATE_H
#define PLAYERBOTS_LEADER_GROUPROUTESTATE_H

// [lead107] user: production code does not live in the test harness. The group's route state - the route target and
// the rule that chose it, the walk towards it (steer, swim, gap, drive), the skips and path verdicts, the boss / wing
// facts, the escort, the event and door states, the links, the pauses - moved out of the harness's Party unchanged.
// The harness's Party is one (it derives from it); the leader reads it through its group view (LeaderGroup::Route).

#include "LeaderKnowledge.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Leader
{
    // User rule (2026-10-24): bots walk by themselves. false: the harness never moves, teleports or holds a bot
    // during a run (no MovePoint / MoveJump / TeleportTo / SetNextCheckDelay for a walk); it gives the group intent
    // (the leader's waypoint, a portal to go through, a spot to come to) and the bots' own actions walk. What a bot
    // cannot do is logged as bot_cannot_move. true restores the harness-driven moves of waves 1-10 (each counted in
    // run_end.harnessMoves). Setup before the run (the teleport to the entrance) is not affected.
    constexpr bool HARNESS_MOVES_BOTS = false;

    struct StarterTrack
    {
        Knowledge::EventStarterDef const* def = nullptr;
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

    // [lead107] the facts of a boss of the run the leader reads (the harness's BossTrack is one)
    struct BossFacts
    {
        uint32 bossId = 0;
        uint8 lastState = 0;      // its encounter state (InstanceScript), last read
        bool killed = false;
        bool failed = false;
        Position homePos;         // where the boss stood when the attempt started (blacklist zone after a FAIL)
        uint32 entry = 0;         // boss creature entry once known
        ObjectGuid creature;      // boss creature once known
        int32 order = -1;         // DungeonEncounter.dbc encounterIndex, -1 unknown
        std::vector<uint32> credits;  // instance_encounters kill-credit creature entries (dbc tracks)
        uint32 wipes = 0;             // [lead107] the group's wipes at it
    };

    // [lead107] a member's water facts (LeaveWater), moved out of the harness's member record
    struct MemberWater
    {
        bool leavingWater = false;
        // Breath estimate: Player::m_MirrorTimer is not reachable from a module, so the harness counts it the way
        // Player::HandleDrowning does (3 minutes under water, refilled 10x as fast above).
        int32 breathMs = -1;  // -1 = not sampled yet (full)
        uint32 breathSampleMs = 0;
        bool surfacing = false;
        bool lowBreathLogged = false;  // HARNESS_MOVES_BOTS off: the bot's own "breathe" surfaces, logged once
    };

    // [lead107 stage 5] a member's rest / far-wait facts (RestWait), moved out of the harness's member record
    struct MemberRest
    {
        // [b31_far_threshold] waited for (far wait / meet): far until within FAR_RESUME_RANGE
        bool farWaited = false;
        // Regen wait: the group waits for this member since (0 = not waiting), and whether its "[regen]" stall line
        // (waiting but not eating / drinking) went out for the current wait.
        uint32 regenSinceMs = 0;
        bool regenStallLogged = false;
        uint32 regenStallMs = 0;  // when regen_stall was logged
        bool regenAboveLineLogged = false;  // regen_wait_below_line logged for this rest (reset by a fight)
    };

    struct GroupRouteState
    {
        std::vector<StarterTrack> starters;
        std::vector<TriggerTrack> triggers;
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
        uint32 nextRetargetMs = 0;  // [lead129] a retarget asked for at this time (0: none); RetargetDue
        uint32 retargetKills = 0;     // the kills (deadEntries) and the last pull end at the last retarget
        uint32 retargetPullEndMs = 0;
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
        std::map<ObjectGuid, uint32> skipUntil;
        // [lead41] the leader's on-the-path verdict per idle candidate (the route rules of Retarget: blocks route,
        // patrol crosses route, required, boss area, joins the boss ...; off the path: its route distance and reach)
        // the leader's on-the-path verdict per idle candidate (the route rules: blocks route, patrol crosses route,
            // required, boss area, joins the boss ...; off the path: its route distance and reach)
            struct PathVerdict
            {
                bool on = false;
                std::string why;
                float routeDist = -1.0f;
                float reach = 0.0f;
            };
        std::map<ObjectGuid, PathVerdict> pathVerdicts;
        std::map<ObjectGuid, bool> pathVerdictLogged;  // the verdict last logged per mob (path_verdict)
        std::map<ObjectGuid, std::string> skipReason;  // why skipUntil holds it (the run's missing-boss report)
        // Where the leader stood when a target was skipped as unreachable: "unreachable" holds from there only. Once
        // the leader is elsewhere (dropped into Azjol-Nerub's lower kingdom, crossed a bridge, took an elevator) the
        // skip is dropped and the target checked again.
        std::map<ObjectGuid, Position> unreachableFrom;
        // A boss of the route order found unreachable first at that time: for ORDER_HOLD_MS the bosses after it in
        // the order wait (Magisters' Terrace wave 8: Selin skipped, Delrissa pulled third-first and wiped twice).
        std::map<ObjectGuid, uint32> orderHoldSinceMs;
        std::map<size_t, ObjectGuid> starterTargets;  // starter index -> object found by the last retarget
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
        // [lead107] moved from the Party with the patrol stage
        // The idle mob outside the target's pack whose aggro reach ended the leader's approach (Steer), if any.
        ObjectGuid approachBlocker;
        // ROUTE_STOP_KEEPS_WAIT: where the leader stands waiting for a pull and since when, whatever the target (a
        // retarget to another mob of the same pack, or behind the same blocker, keeps the clock).
        Position stopPos;
        uint32 stopSinceMs = 0;
        uint32 stopLastMs = 0;
        uint32 lastStarterUseMs = 0;
        ObjectGuid neighbourTarget;  // boss_neighbour_target: kept as the target (Retarget) while it lives
        bool guaranteeHolding = false;
        std::string lastEvent;
        // [lead107] the route's holds (moved with the event / starter stage)
        // The leader's waypoint stays while a starter step-out is walked (the no-target branch would clear it).
        uint32 waypointHoldUntilMs = 0;
        // The leader casts an event item on its object (Old Hillsbrad's 5 s bomb): no retarget and no waypoint until
        // then, a new walk would interrupt the cast.
        uint32 castHoldUntilMs = 0;
        // [b51_strat] TickGateTrapGather: the group gathers inside a Stratholme rat-trap alley before anyone walks over
        // the armed trap point; no retarget and no leader waypoint until then (refreshed every tick while it holds).
        uint32 trapHoldUntilMs = 0;
        bool doorGatherHeld = false;  // [b51_strat] the last DoorGatherHolds held the use (no progress)
        // [lead107] moved with the route target stage: the instance's entrances (the wing walk's), the boss rush,
        // the target counts and deaths (a reviving / respawning mob), the careful refusals, the endless summons,
        // the detour fallback, the wing walk
        std::vector<Position> entrances;
        size_t entranceIdx = 0;
        uint32 bossRushUntilMs = 0;
        std::map<ObjectGuid, uint32> targetCounts;
        std::set<ObjectGuid> targetDied;
        std::map<ObjectGuid, uint32> carefulRefused;
        std::set<uint32> endlessSummons;  // summoned entries no longer route targets (logged once each)
        uint32 detourFallbackUntilMs = 0;  // the last event_start (an event the group works through: CoS crates)
        bool wingWalk = false;
        uint32 wingWalkSinceMs = 0;
        // [lead107] moved with the steer stage: the skipped neighbour behind a stop, the harness-hold log, the
        // neighbour holds and the route blocker, the idle / event-wait time, the guarantee hold, the held-for-now
        // The last skipped neighbour pack that stopped an approach (ROUTE_SKIP_BEHIND_SKIPPED), for the idle guard.
        std::string behindSkipped;
        // [b33_sh2] the last harness_hold event (Steer holds the walk with a target set), its reason, throttled
        uint32 harnessHoldLogMs = 0;
        std::string harnessHoldWhy;
        // Boss approach held at a neighbour pack (neighbourStop): the boss and how often in a row.
        ObjectGuid neighbourHoldBoss;
        uint32 neighbourHolds = 0;
        // route_blocker_body_pull: the pack that stands on the way to the route's target (its walk enters the pack's
        // reach) is the target, and pulled - by sight, else walked in - never skipped while it is on the way.
        std::vector<ObjectGuid> routeBlockerPack;
        std::map<ObjectGuid, uint32> routeBlockerTries;  // by blocker: how often it was made the target
        uint32 idleMs = 0;
        uint32 eventWaitMs = 0;
        // ROUTE_CAREFUL_GUARANTEE_HOLD: the pack the tank refused (not hopeless) the group holds at for the planner's
        // stand guarantee (CarefulPlanVerdict::GetRefusal), and when that guarantee came due (0: not yet).
        ObjectGuid guaranteeTarget;
        uint32 guaranteeDueMs = 0;
        // [held_for_now_cap] the target the planner answers "held for now" for, and since when (0: none)
        ObjectGuid heldForNowTarget;
        uint32 heldForNowSinceMs = 0;
        // [lead107] moved with the meet stage: the group wiped (until the revive), the member the leader walks back
        // to (MeetMember), logged once per episode
        bool wiped = false;
        ObjectGuid meeting;
        // [lead107] moved with the water stage: the last place the leader stood on dry ground out of combat inside
        // the instance: the group rests there, never in water (Underbog, Blackfathom Deeps: bots drank at the lake
        // bottom and drowned); each member's water facts
        Position dryPos;
        bool hasDryPos = false;
        std::map<ObjectGuid, MemberWater> water;
        // [lead107 stage 5] moved with the phantom-combat / rest / buffing stage.
        // Phantom combat (TickPhantomCombat): flagged in combat while nothing in the combat refs attacks - the episode
        // logged (and since when, for the logs), the refs ended, when and how often, and the set of refs last ended
        // (a new set after the end is ended again: sh-n 5B-89, the tank flagged again after the one end)
        uint32 phantomCombatSinceMs = 0;
        bool phantomCombatLogged = false;
        bool phantomCombatEnded = false;
        uint32 phantomCombatEndedMs = 0;
        uint32 phantomCombatEnds = 0;
        std::set<ObjectGuid> phantomEndedRefs;
        // [phantom_mob_hunt] the live, reachable mob of a phantom combat the group was sent to, and how often
        ObjectGuid phantomHunt;
        std::set<ObjectGuid> phantomHunted;  // the mobs hunted in this phantom episode (once each)
        // [b33_sh2] target set, traveling, no walk point and the leader standing since; the last "no walk" log
        uint32 noWalkSinceMs = 0;
        uint32 noWalkLogMs = 0;
        // buffing
        bool needBuff = true;
        uint8 buffStep = 0;
        uint32 buffStepMs = 0;
        uint32 buffSinceMs = 0;  // the buffing pass started (for the logs)
        std::string buffReason = "run start";
        // waits
        uint32 waitSinceMs = 0;
        uint32 waitCooldownUntilMs = 0;
        std::string waitingFor;
        std::map<ObjectGuid, MemberRest> rest;  // each member's rest / far-wait facts
    };
}

#endif
