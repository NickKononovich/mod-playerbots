/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_ROUTEEVENTS_H
#define PLAYERBOTS_LEADER_ROUTEEVENTS_H

// [lead107] user: production code does not live in the test harness. The leader's event route steps - the event
// starters (item, NPC, object use), the doors they open and their gathers, the escort the group stays with, the
// event spots and the event gather, the route links (walked, jumped) the group crosses - moved out of
// TestHarnessRunner.cpp unchanged; the harness calls them through the group view.

#include "Creature.h"
#include "G3D/Vector3.h"
#include "G3D/Vector4.h"
#include "GameObject.h"
#include "MoveSplineInitArgs.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Position.h"
#include "LeaderGroup.h"
#include "LeaderKnowledge.h"
#include "RouteLegs.h"
#include "RouteSearch.h"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace Leader::Travel
{
    using namespace Leader::Knowledge;

    constexpr float WAYPOINT_STEP = 30.0f;

    // On a route over steep slopes the leader's own MoveTo (headless filter) has no polygon there and goes in a
    // straight line: short steps keep that line on the slope.
    constexpr float STEEP_WAYPOINT_STEP = 8.0f;

    constexpr float EVENT_GATHER_ARRIVE = 6.0f;

    constexpr float DRY_SPOT_SPREAD = 3.0f;

    constexpr float ESCORT_FOLLOW_RANGE = 10.0f;

    constexpr uint32 ESCORT_IDLE_MS = 2 * MINUTE * IN_MILLISECONDS;  // standing, out of combat: the walk is over

    constexpr uint32 ESCORT_MAX_MS = 30 * MINUTE * IN_MILLISECONDS;

    // [ohb-escort-death-rearm] an escort that died (Old Hillsbrad: Thrall respawns at his last checkpoint and
    // offers his gossip again) gives its use back, at most this often per starter.
    constexpr uint32 ESCORT_DEATH_REARM_MAX = 6;

    constexpr float EVENT_SPOT_HEIGHT = 15.0f;

    constexpr uint32 VIOLET_HOLD_MAP_ID = 608;  // prisoners released in a random order (see the next-boss loop)

    // Steering without progress (Party::steerAnchor): a walk covers 3 yd in well under a second.
    constexpr float STEER_PROGRESS_RANGE = 3.0f;


    constexpr float KEY_DOOR_CAST_RANGE = 3.0f;  // a key door cast without line of sight: from here, in front of it

    // Harness drive (Party::driving, pass 3): no progress towards the same goal (the leader stands on its walk point, LeaderWalkStands), and
    // the harness walks the leader itself in short straight steps along its route (each step lies on the route's
    // polyline, so on the navmesh corridor), the bot AI held for the step. Wave 10: Utgarde Pinnacle (a 97-polygon
    // ledge joined to the hall only over NAV_GROUND_STEEP, "test runner move - FAILED" for 4 minutes),
    // Azjol-Nerub (the shaft ledge above the hole, same), Ahn'kahet ("use nerubian device - OK" every tick
    // before Nadox), Culling ("cos dispel crate - OK" every tick) - the leader stood, steer_stuck skipped
    // target after target.

    constexpr float DRIVE_SAME_GOAL = 10.0f;

    // [lead130] the leader's walk does not move it: it was given a walk point and stands (no move, no cast) with the
    // same walk point at the same spot as at the last look (a hold clears the walk point: no stand);
    // `seenAt` / `seenSerial` are the caller's last look
    bool LeaderWalkStands(LeaderGroup& group, Player* leader, Position& seenAt, uint32& seenSerial);

    // A gossip starter found on a scripted walk at the click (ROUTE_STARTER_NOT_WALKING): tried again after this.
    constexpr uint32 STARTER_WALKING_RETRY_MS = 3 * IN_MILLISECONDS;

    // Event item starters (UseItem): standing this close to the object the leader is inside its model (Culling's
    // crates: the CoS strategy walks to the crate's centre); it steps out to STARTER_ITEM_STEP_OUT first.
    constexpr float STARTER_ITEM_MIN_RANGE = 2.0f;

    constexpr float STARTER_ITEM_STEP_OUT = 3.5f;

    constexpr uint32 STARTER_ITEM_MAX_FAILS = 3;

    // WalkToEventGather: members farther than this from the gather spot are called in (HoR's start radius is 40 yd
    // 2D from the altar centre, the spot 17 yd from it), at most every GATHER_CALL_MS.
    constexpr float EVENT_GATHER_MEMBER_RANGE = 15.0f;

    constexpr uint32 GATHER_CALL_MS = 5 * IN_MILLISECONDS;

    constexpr uint32 STARTER_ITEM_RETRY_MS = 5 * IN_MILLISECONDS;

    // [expect_door] EventStarterDef::expectDoor (Deadmines' Fire Cannon 6250: 2 s cast, range 5 yd, InterruptFlags
    // 0x0F).
    constexpr uint32 EXPECT_DOOR_MS = 5 * IN_MILLISECONDS;  // cast 2 s + SAI 500 ms + margin

    constexpr uint32 EXPECT_DOOR_MAX_RETRIES = 4;

    constexpr float EXPECT_DOOR_CAST_RANGE = 3.5f;  // to the object's centre (spell range 5 yd)

    constexpr float EXPECT_DOOR_STEP = 2.5f;        // a step in: this far from the centre, toward the leader

    constexpr uint32 EXPECT_DOOR_WAIT_MS = 1500;

    constexpr uint32 EXPECT_DOOR_LOG_MS = 10 * IN_MILLISECONDS;

    // [strat_key_*] KeyDoorPassDef (Stratholme's auto-closing key gates: Opening 3366 5 s cast, the gate shuts 3 s
    // after it opens).
    constexpr float KEY_PASS_LEADER_RANGE = 1.5f;  // the leader at the use point

    constexpr float KEY_PASS_GATHER_RANGE = 5.0f;  // every member this close (2D) to the use point

    constexpr float KEY_PASS_SPREAD = 2.0f;        // members called to points this far round the use point

    constexpr uint32 KEY_PASS_RETRY_MS = 1500;

    constexpr uint32 KEY_PASS_LOG_MS = 5 * IN_MILLISECONDS;

    // [b30_overpull] strat-n 20261025-184229-strat-n-5D-6: the door hold left the leader 1.7 yd from the use point,
    // the test runner counts a waypoint within 2.5 yd as reached (TestRunnerStrategy WAYPOINT_REACHED_DISTANCE) and
    // never moved it - "leader to the use point" for 7.5 min, the key never used, 0 pulls. Within the runner's
    // reach the leader keys from where it stands when the gate's centre is in the Opening's range and sight.
    constexpr float KEY_PASS_RUNNER_REACH = 2.5f;     // = TestRunnerStrategy WAYPOINT_REACHED_DISTANCE

    constexpr float KEY_PASS_CAST_CENTRE = 4.5f;      // Opening 3366: 5 yd to the object

    // [b30_overpull] the gather wait retries at KEY_PASS_RETRY_MS, not the starter's 60 s cooldown (the 60 s gap
    // let the retarget spin through the trash behind the gate one per second); at most this long, then the key is
    // used with whoever stands at the use point (the re-key covers the rest).
    constexpr uint32 KEY_PASS_GATHER_MAX_MS = 45 * IN_MILLISECONDS;

    // [starter_wait_group] EventStarterDef::gatherRadius (Scholomance: Kirtonos' gate closes on the brazier use).
    constexpr uint32 STARTER_GATHER_RETRY_MS = 2 * IN_MILLISECONDS;

    constexpr uint32 STARTER_GATHER_MAX_MS = 60 * IN_MILLISECONDS;

    constexpr uint32 STARTER_GATHER_LOG_MS = 15 * IN_MILLISECONDS;

    // - at an event gather spot (Halls of Reflection) members left farther than EVENT_GATHER_MEMBER_RANGE walk in.
    constexpr bool ROUTE_GATHER_MEMBERS = true;

    constexpr bool ROUTE_STARTER_ITEM_CHECK = true;

    constexpr bool ROUTE_STARTER_NOT_WALKING = true;

    extern std::unordered_map<uint32, std::pair<uint32, uint32>> starterGatherMs;  // instance -> (wait since, last log)

    // [b35_ss2] Where a key gate's bars stand along its facing, measured from its spawn point. The bars are not on
    // the spawn's plane: strat-h 20261026-024524-strat-h-5C-1, King's Square Gate 175352, the leader held at
    // +0.2 yd from the plane (3572.5,-3450.6) and the healer at +1.7 yd (3573.6,-3447.6) with the bars between
    // them (door_cut "holding" 1294-1525 s, the leader's walk "walked 2.5" into them). The plane test called
    // both "through": rearm_check "the whole group is through it" every 15 s, strat_key_pass "sent: []" after
    // the healer's own key, 260 s stalled. Measured once while the gate is shut (its collision is off when open):
    // a ray from the use point to the far point and one back, the middle of the two hits. 0 until measured.
    float KeyGateBarsOffset(GameObject const* door, KeyDoorPassDef const& pass);

    // Where a started event waits for the group before its waves come; false: no such spot on that map.
    bool EventGatherSpot(uint32 mapId, Position& spot);

    // The event is at the stage where the group belongs on the spot. HoR: from the Jaina / Sylvanas gossip until
    // Marwyn is dead - the group watches the intro at the altar, as players do; the waves start only with
    // everybody within 40 yd of it (instance_halls_of_reflection.cpp Update). Waiting for IsWaveEventActive
    // (intro persistent data) left the group at the entrance in wave 6: nobody ever walked to the altar.
    bool EventGatherReady(uint32 mapId, Player* leader);

    // A straight walk (no navmesh path) only over ground: every 2 yd of the line has a floor at most
    // DIRECT_MAX_DROP below it. A living member walking off a ledge or a floating platform falls to its death;
    // a ghost takes no damage and may still go straight.
    constexpr float DIRECT_STEP = 2.0f;

    constexpr float DIRECT_MAX_DROP = 6.0f;

    // [incomplete_route_deadend] DirectLineOverGround's test on a line from any point (a route link's own start).
    bool LineOverGround(Map* map, uint32 phaseMask, G3D::Vector3 const& from, G3D::Vector3 const& to);

    bool DirectLineOverGround(Player* bot, float x, float y, float z);

    bool RefusalIsOverCap(std::string const& why);

    bool IsDoorKey(DoorEventDef const& def, Creature const* creature);

    // The walk to `target` opens this closed door: the door is the target (a key used on it: Scholomance's
    // Viewing Room Door), or the target is the key creature of its DoorEventDef (the pull opens it).
    // 2026-10-25: or the door opens by use (TerrainHazard.h DoorOpensByUse: Scholomance's Iron Gates 175611-175618
    // and Door 175610, lockId 0, flags 32) and is no DoorEventDef door (the Utgarde Keep forge walls stay shut
    // until their Forge Master): the leader walks up and clicks it (LeaderDoorStop), for any target.
    bool DoorOpensFor(GameObject const* door, WorldObject const* target);

    // [starter_chest_open] The opening spell a client casts on a locked chest: the lock's LOCK_KEY_SKILL case the
    // caster meets (its skill; SKILL_NONE for the generic types), then the first OPEN_LOCK spell of that lock type
    // the caster knows, or one that needs no spellbook entry (TARGET_FLAG_GAMEOBJECT_ITEM: HandleCastSpellOpcode
    // skips HasActiveSpell for it). UBRS Father Flame 175245, lock 93 = LOCKTYPE_OPEN_TINKERING 12 -> 6477
    // "Opening". 0: none.
    uint32 ChestOpeningSpell(Player* caster, GameObject const* go);

    // The closed doors a route to `target` must not run through (the ones that do not open for it).
    std::vector<GameObject*> DoorsClosedFor(std::vector<GameObject*> const& doors, WorldObject const* target);

    // A creature on a scripted point walk (MovePoint in the active or controlled slot: an event NPC on its way, or
    // paused on it). A gossip hello runs Creature::PauseMovementForInteraction, which pauses exactly that walk
    // for the creature's InteractionPauseTimer (default CreatureStopForPlayer, 3 min; Bly / Weegli 3 s since AC
    // #27731), and every click arms it again: Zul'Farrak's crew never reaches the stairs, ARRIVED_AT_STAIR never
    // comes and the pyramid waves never start (AC issue #26436). Same guard as AC PR #26443 by lipeter1982
    // ("prevent gossip from interrupting stairs event"), which took the crew's gossip flag away for the walk: a
    // player does not click an NPC that is walking somewhere for its event.
    // [b33_cos2] The Culling's Arthas (26499) offers his gossip at the event waypoints, and his escort
    // (npc_escortAI: MoveSplinePath, ESCORT_MOTION_TYPE) or its return to the combat start (POINT_MOTION_TYPE)
    // may still run when the flag is on: a hello then pauses him for Creature.MovingStopTimeForPlayer (180 s).
    // cos-h 20261026-000434-cos-h-5H-22: the escort stood 3 min 15 s at (2428.5,1121.5) after a gossip, the
    // Guardian of Time ran out. As [b33_cos]'s CoSArthasGossipTrigger: no hello while he moves (a spline, an
    // escort or point walk in the active slot), at most ARTHAS_STARTER_MOVING_WAIT_MS, then the hello goes anyway.
    constexpr uint32 ARTHAS_STARTER_MOVING_WAIT_MS = 30 * IN_MILLISECONDS;

    constexpr uint32 ARTHAS_STARTER_SEEN_GAP_MS = 5 * IN_MILLISECONDS;  // a wait unseen this long starts anew

    struct ArthasMovingWait
    {
        uint32 sinceMs = 0;
        uint32 seenMs = 0;
    };

    extern std::unordered_map<uint32, ArthasMovingWait> arthasMovingWaits;  // instance id -> wait, world thread

    bool ArthasStarterMoving(Creature* creature);

    bool WalksScriptedPath(Creature* creature);

    // Route links (RouteLinkDef): arrival at either end, the walk over it.
    constexpr float LINK_ARRIVE_RANGE = 3.0f;

    constexpr float LINK_ARRIVE_HEIGHT = 4.0f;

    constexpr float LINK_DONE_RANGE = 6.0f;

    constexpr float LINK_GATHER_RANGE = 30.0f;

    constexpr uint32 LINK_MAX_MS = 4 * MINUTE * IN_MILLISECONDS;

    constexpr uint32 LINK_CROSS_LOG_MS = 5 * IN_MILLISECONDS;  // "link_cross": the leader's position, crossing

    // The walk to a link's start times out after LINK_MAX_MS without coming LINK_PROGRESS_STEP closer (fights on
    // the way count as progress), LINK_WALK_MAX_MS at most. sh-h wave 11: the walk to the sewer jump took ~230 s
    // with its fights in waves 7-10 (4 minutes flat), the build-11 guarantee holds added 30 s each, it timed out
    // twice at 241 / 247 s and was given up (396 "closed door" skips, Nethekurse never reached).
    constexpr float LINK_PROGRESS_STEP = 3.0f;

    constexpr uint32 LINK_WALK_MAX_MS = 10 * MINUTE * IN_MILLISECONDS;

    constexpr uint32 LINK_HOLD_MS = 2 * IN_MILLISECONDS;  // bot AI paused while the harness moves the member

    // A link that failed this often (timeout, no ground) is given up for the run (Mechanar 2026-10-25: the lift
    // timed out with nobody over and was chosen again at once, 2.6 yd away, for another 4 minutes).
    constexpr uint32 LINK_MAX_FAILS = 2;

    constexpr uint32 LIFT_MAX_MS = 90 * IN_MILLISECONDS;  // a lift crossing from the gathered landing

    bool AtLinkPoint(Player* bot, float x, float y, float z, float range);

    // A crossing member this far (3D) from the link's line is off it (LINK_OFF_LINE_BACK: back to the start).
    constexpr float LINK_OFF_LINE_RANGE = 6.0f;

    // The 3D distance from the bot to the segment from the link's start to its far end.
    float LinkLineDistance(Player* bot, RouteLinkDef const& link);

    // [w24b-under-end] Under the far end on another layer (Ahn'kahet 2026-10-25: 4.2 yd under the step onto the
    // Volazj ramp, 4.4 yd from the line, inside LINK_OFF_LINE_RANGE): off the line too, back to the start and over
    // the line again.
    bool LinkOffLine(Player* bot, RouteLinkDef const& link);

    // A link over a lift (RouteLinkDef::goEntry of GAMEOBJECT_TYPE_TRANSPORT: The Mechanar's GO 183788).
    bool LinkIsLift(RouteLinkDef const& link);

    // The bot's waypoint (the intent; its own "test runner move" walks there).
    // ROUTE_NO_WALK_IN_PULL (build 12): the tank's own pull is running - a pull target or a pull pending / started
    // (PullStrategy), or its group's line-of-sight pull plan (LosPull) - so the harness gives it no waypoint and
    // drops the one it has. sv-n wave 11 03:20: "[los pull] starts ..., group to (40.0,-230.2)", then the
    // approach walked the tank 12 yd toward the pack, the pull aborted out of range and the walk dragged the group
    // into the whole 24-mob pack (wipe).
    constexpr bool ROUTE_NO_WALK_IN_PULL = true;

    extern std::unordered_map<ObjectGuid, uint32> pullWalkLogMs;  // tank -> last "[approach] pull running" line

    bool TankPullRunning(Player* tank);

    struct PullHoldTrack
    {
        uint32 sinceMs = 0;
        uint32 seenMs = 0;
        uint32 releaseUntilMs = 0;
        bool logged = false;
        bool stallLogged = false;
        Position anchor;
        uint32 orderId = 0;  // [lead2] the order its pull_hold line was logged for
    };

    extern std::unordered_map<ObjectGuid, PullHoldTrack> pullHolds;  // leader -> its current pull hold (world thread)

    bool PullHoldReleased(ObjectGuid leader);

    // A jump link lands as a fall does (a client's MSG_MOVE_FALL_LAND): Player::HandleFall from the start's
    // height. MoveJump is a controlled move, PlayerbotAI::UpdateFall never sees it. No damage in water.
    void LandLinkJump(Player* bot, float fromZ);

    bool LeaderOwnWalkStalled(LeaderGroup& group, Player* leader, G3D::Vector3 const& goal);

    bool PartyRefusal(LeaderGroup& group, ObjectGuid mob, bool& hopeless, uint32& waitMs, std::string& why);
    // [lead133] the group stands at its patrol wait spot while the planner still holds the target for the patrol
    // ("patrol wait"), not for PATROL_WAIT_HOLD_MS
    bool PatrolWaitOn(LeaderGroup& group);

    // [b32_pos] A progress step (EventSpotDef::progressData) that was reached (eventSpotState 1, not 2 "no path")
    // and whose instance progress is still below progressMin: the group stays and its trigger re-fires.
    bool ProgressStepPending(LeaderGroup& group, EventSpotDef const& spot);

    // [brd_ring_hold] A spot that holds the group now: a hold spot, or a step whose event runs
    // (EventSpotDef::eventData IN_PROGRESS) or waits for its retry after a FAIL (Party::eventSpotRetry,
    // WatchEventState).
    bool SpotHolds(LeaderGroup& group, EventSpotDef const& spot);

    // Over the link as a player walks it: a straight walk (no navmesh on a game-object bridge). Bots walk by
    // themselves: the far end is the bot's waypoint, its own "test runner move" walks straight where the navmesh
    // ends (MovementAction::MoveStraight).
    void StartLinkCross(LeaderGroup& group, Player* bot, RouteLinkDef const& link);

    // The leader's waypoint is withheld for its own running pull (GiveWaypoint's ROUTE_NO_WALK_IN_PULL rule).
    // [lead2] LEADER_ONE_TANK_MOVER (C:/WoW/LEADER_REFACTOR_PLAN.md step 2): the tank's open pull order (Leader::
    // Orders) is the one owner of its movement, a boss target's too; the leader gets the tank back on the order's
    // events only (the tank's Aborted, the order's fight over, the tank gone, order_orphan). TankPullRunning's
    // clocks (GetMaxPullTime, the RecentlyAborted window) and the PULL_HOLD_STALL_MS / PULL_HOLD_RELEASE_MS
    // release are the A/B's old path. Violet Hold's released prisoners stay the harness's walk in both.
    bool WaypointHeldForPull(LeaderGroup& group, Player* bot);

    bool CanOpenLock(LeaderGroup& group, GameObject const* go);

    bool EntriesDead(LeaderGroup& group, std::vector<uint32> const& entries);

    Creature* ActiveEscort(LeaderGroup& group, Map* map);

    bool StayWithEscort(LeaderGroup& group, Player* leader);

    int32 EventSpotIndex(LeaderGroup& group);

    bool WalkToEventSpot(LeaderGroup& group, Player* leader);

    bool LinkUsable(LeaderGroup& group, Map* map, RouteLinkDef const& link);

    int32 LinkTo(LeaderGroup& group, Player* leader, WorldObject* target);

    void EndLink(LeaderGroup& group, char const* reason);

    bool WalkToLink(LeaderGroup& group, Player* leader);

    void CrossLinks(LeaderGroup& group, std::vector<Player*> const& players, Player* leader);

    // [b51_strat] User 2026-10-26: «Ключ есть - открыл ворота, пустил»; before the door is opened the WHOLE group
    // stands right next to the opener («совсем рядом», ~3-4 yd), out of combat, and passes in the same opening. Every
    // door that closes again by itself: GameObjectTemplate::GetAutoCloseTime() on a DOOR / BUTTON (world DB, 5-man
    // maps: Stratholme 175368 / 175352 / 175353 / 175369, Dire Maul 177211-177215 Doors, BRD 170555-170557 / 170572 /
    // 161461, UBRS 175185, Deadmines / SFK / Gnomeregan / SM levers and buttons, ...) or a KeyDoorPassDef gate. No cap
    // (user correction 2026-10-26: "never open early"): a member who cannot come is a stall for the stall recovery; the
    // wait is logged every DOOR_GATHER_LOG_MS. A member whose walk to the opener runs through this closed door is on
    // its far side (it is the one the door is opened for) and the door-cut member of this door too: not waited for.
    // Dead members do not count.
    bool DoorGatherHolds(LeaderGroup& group, Player* opener, GameObject* door);

    bool UseStarter(LeaderGroup& group, Player* leader, StarterTrack& starter, WorldObject* object);

    bool UseStarterItem(LeaderGroup& group, Player* leader, StarterTrack& starter, GameObject* go);

    // [brd_bar_buy] [brd_bar_turnin] A vendor or quest step at its creature, as the client does it: CMSG_BUY_ITEM's
    // Player::BuyItemFromVendorSlot (money, reputation, the vendor's stock and reach are checked by the core), or
    // CMSG_QUESTGIVER_ACCEPT_QUEST + CMSG_QUESTGIVER_CHOOSE_REWARD (the creature AI's sQuestReward runs from the reward
    // handler: Blackrock Depths' npc_rocknot counts the ale, TYPE_BAR SPECIAL at three). A step counts only if it
    // changed the bags (bought items in, quest items out).
    bool UseStarterAtNpc(LeaderGroup& group, Player* leader, StarterTrack& starter, Creature* creature);

    bool StartersUsed(LeaderGroup& group, std::vector<uint32> const& entries);

    void MarkStartersDoneByGroup(LeaderGroup& group, Player* leader);

    bool WalkToEventGather(LeaderGroup& group, Player* leader);

    bool EventGatherApplies(LeaderGroup& group);
}

#endif
