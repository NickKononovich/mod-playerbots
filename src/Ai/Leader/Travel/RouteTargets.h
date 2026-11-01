/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_ROUTETARGETS_H
#define PLAYERBOTS_LEADER_ROUTETARGETS_H

// [lead107] user: production code does not live in the test harness. The leader's route target choice (Retarget:
// the route rules, the boss order, the wings, the holds and skips, the score of every candidate) and the facts it
// reads, moved out of TestHarnessRunner.cpp unchanged; the harness calls it through the group view.

#include "Creature.h"
#include "InstanceGuid.h"  // [lead191] caches keyed by (instance, GUID)
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

namespace Leader::Travel
{
    using namespace Leader::Knowledge;

    constexpr uint32 MAX_PATH_CANDIDATES = 8;

    constexpr float FAILED_BOSS_RADIUS = 35.0f;

    constexpr float BOSS_ROUTE_WEIGHT = 0.25f;

    constexpr float LATER_BOSS_TRASH_RADIUS = 30.0f;

    constexpr float DEFERRED_TRASH_PENALTY = 1000.0f;

    constexpr uint32 UNREACHABLE_SKIP_MS = 5 * MINUTE * IN_MILLISECONDS;

    constexpr float UNREACHABLE_RECHECK_DISTANCE = 60.0f;  // the leader moved this far: unreachable skips end

    constexpr float UNREACHABLE_RECHECK_HEIGHT = 25.0f;    // or this much up / down (a drop, an elevator)

    // Later bosses wait this long for an unreachable earlier one; below EVENT_WAIT_FOR_SPAWNS_MS, so a run with
    // nothing else to do releases the order before it is called partial.
    constexpr uint32 ORDER_HOLD_MS = 4 * MINUTE * IN_MILLISECONDS;

    // ROUTE_ADOPT_GROUP_ESCORT (build 11): an escort a bot's own dungeon step started (ClassicActions.cpp "Old
    // Hillsbrad: Thrall", the tbc-ohb step gossiped him itself) is the harness's escort too. 2026-10-25 ohb-h /
    // ohb-n (build 10): Rtpalar "did the event step: Old Hillsbrad: Thrall", the gossip flag went off, the
    // harness's Escort starter (it needs the flag) never saw him again, the group stood 8-13 minutes in the keep
    // and stalled "Skarloc / Epoch Hunter: event not started". An escort starter's creature that is on its way
    // (moving or fighting, no gossip flag) while no escort runs and the starter's order holds is adopted.
    constexpr bool ROUTE_ADOPT_GROUP_ESCORT = true;

    // ROUTE_LIFT_REFUSED_SKIP: with nothing left, a refused pack is asked again (Retarget, no target), at most
    // LIFT_REFUSALS_MAX refusals per pack (pack_too_big is logged at 3).
    constexpr bool ROUTE_LIFT_REFUSED_SKIP = true;

    constexpr uint32 LIFT_REFUSALS_MAX = 3;

    constexpr float SAME_ENTRANCE_RANGE = 30.0f;

    // [sm-wing-done] maps whose wings are told apart by the nearest entrance; no-target wait once a wing is empty.
    constexpr bool ROUTE_WING_DONE_CHANGE = true;

    // [dme-ironbark-door]
    constexpr uint32 STARTER_DOOR_CHECK_MS = 45 * IN_MILLISECONDS;

    constexpr uint32 STARTER_DOOR_REARM_MAX = 2;

    constexpr uint32 EVENT_HOLD_LOG_MS = 30 * IN_MILLISECONDS;

    constexpr std::array<uint32, 1> ESCORT_OPENED_DOORS = {188686};  // Culling of Stratholme: Shkaf gate (Arthas)

    // [strat_trap_gate_*] Stratholme's rat-trap portcullises (instance_stratholme.cpp StartGateTrap: a player
    // within 5.5 yd of the trap spot closes both gates of its side, EVENT_GATE1_DELAY / EVENT_GATE2_DELAY open
    // them 20 s later): a wait at the gate, never a "closed door" skip (strat-h 2026-10-25: door_hold
    // Doodad_SmallPortcullis03, the bosses behind it skipped 60 s at a time).
    constexpr std::array<uint32, 4> TRAP_GATE_DOORS = {175350, 175351, 175354, 175355};

    constexpr uint32 TRAP_GATE_LOG_MS = 10 * IN_MILLISECONDS;

    bool IsTrapGate(GameObject const* door);

    // Only a target this deep below the surface is out of reach (the route may cross water, and mobs near the
    // surface are fought from it).
    constexpr float DEEP_TARGET_DEPTH = 15.0f;

    constexpr uint32 SAME_TARGET_MAX = 1;  // came back alive after a death more often than this: it respawns

    constexpr uint32 RESPAWNING_SKIP_MS = 15 * MINUTE * IN_MILLISECONDS;

    // Escorts (StarterKind::Escort): what attacks the escorted creature is fought, the leader keeps up with it.
    constexpr float ESCORT_FIGHT_RADIUS = 40.0f;

    constexpr float HOLD_TARGET_RADIUS = 90.0f;  // around an event's waiting place (Violet Hold's portals ~90 yd)

    // An EventSpotDef::guardEntries add at the spot beats ordinary trash, never a holdAlways objective (a rift
    // keeper): bm-h build 10 (-1500 for the ring adds vs -1000 for the keepers): no keeper or Deja was ever
    // attacked, the 4 rifts stayed open, Medivh's ring grew 12 -> 27 adds and the shield fell 68 -> 0.
    constexpr float HOLD_GUARD_PRIORITY = 500.0f;

    constexpr int32 HOLD_GUARD_SHIELD_PCT = 60;  // Black Morass: under this shield the ring adds come first

    constexpr float HOLD_KEEPER_DISTANT = 90.0f;     // a keeper this far from Medivh ranks below the ring adds

    constexpr float HOLD_EVENT_MOB_RANGE = 50.0f;  // event mobs this close to the spot or a live rift: targets

    constexpr uint32 BLACK_MORASS_TIME_RIFT = 17838;

    constexpr uint32 BLACK_MORASS_MAP = 269;

    constexpr float BM_KEEPER_FETCH_RANGE = 30.0f;  // [hleft-bm-tank-fetch] keepers farther out are the tank's

    // (build 18: the build-11 "keeper not engaged" pause is gone - bm-n build 17 skipped a Rift Lord with it and
    // everything behind it; a rift keeper is never skipped, ROUTE_SKIP_BEHIND_SKIPPED included.)
    constexpr float HOLD_OBJECTIVE_PRIORITY = 1000.0f;  // score bonus of a hold spot's holdAlways entry

    // [guard_target_keep] The door guard the group walks to now (a guard with an approach point, Sunken Temple's
    // defenders) keeps this much score over the other guards: one defender at a time, chosen nearest first. st-n
    // build 23 (103125 1714-1724 s): Gasher -> Zolo -> Gasher in 10 s on 7-15 points of score as the leader moved.
    constexpr float GUARD_TARGET_KEEP = 150.0f;

    // A key door that closes again by itself (door data2 autoCloseTime: Stratholme's Service Entrance Gate 175368,
    // 3 s): the key is used again while a member's way to the leader runs through it, at most this often.
    // [b51_strat] User 2026-10-26: «Кто-то застрял за воротами - открываешь»: re-keyed as often as needed (was 6
    // per starter; strat-n 014521-5C-2 ran out and held 230 s with the healer 0.7 yd from the gate).
    constexpr uint32 DOOR_REARM_MAX = 1000;

    constexpr uint32 KEY_DOOR_HOLD_FRESH_MS = 5 * IN_MILLISECONDS;  // a door_hold this recent re-arms its key

    // ReopenKeyDoorHeld: the leader this close to the closed key door.
    constexpr float KEY_DOOR_HELD_RANGE = INTERACTION_DISTANCE + 2.0f;  // the door hold stops ~5 yd from its centre

    constexpr float KEY_DOOR_REACH = 60.0f;  // a key door this close to the leader keeps the targets behind it

    constexpr uint32 DOOR_REARM_GAP_MS = 4 * IN_MILLISECONDS;  // an item starter's object, steering time

    constexpr float SHORTCUT_REENTER_MIN_GAIN = 200.0f;

    // Retarget: a summoned non-boss entry killed this often is no longer walked to (endless summons).
    constexpr uint32 ENDLESS_SUMMON_KILLS = 20;

    // Retarget guards (pass 3). An event chain (a starter of the map used, another one still pending with its
    // conditions met): ordinary trash farther than this from every pending starter object and event spot is no
    // target (Culling wave 10: after Chromie the route chose Risen Zombies 970 yd away in the city).
    constexpr float EVENT_CHAIN_TRASH_RADIUS = 150.0f;

    // [event_chain_narrow] The radius above only bounds the candidates; route-only trash calls a mob "event
    // chain" (required) only when it is the chain's own: summoned (the event's spawns), standing within
    // EVENT_CHAIN_JOIN_RANGE of a pending anchor with sight of it (it joins when the step starts), or inside the
    // event spot's area (its holdRadius). The way to the nearest pending starter is a backbone leg already
    // ("blocks route"). brd-h 20261025-163739-brd-h-5A-20 18:15:55: a 6-mob Shadowforge Citizen pack in a dead-end
    // alcove ~96 yd from the bar's anchors was "event chain" and pulled. false: every mob within the radius.
    constexpr bool ROUTE_CHAIN_NARROW = true;

    constexpr float EVENT_CHAIN_JOIN_RANGE = 35.0f;

    // A non-boss, non-starter target with a path longer than this is taken only when nothing nearer is valid
    // and no starter is pending.
    constexpr float FAR_TARGET_PATH = 400.0f;

    // A walk-in event spot first (ROUTE_EVENT_SPOT_FIRST): idle trash is a target only when it stands no farther
    // from the spot than the leader plus this margin (it lies on the way), or this close to the leader.
    constexpr float LEAD_SPOT_AHEAD_MARGIN = 15.0f;

    constexpr float LEAD_SPOT_NEAR_RANGE = 25.0f;

    // A scripted intro (ROUTE_INTRO_WAIT) is waited out at most this long (Svala's talk: 72 s).
    constexpr uint32 INTRO_WAIT_MAX_MS = 2 * MINUTE * IN_MILLISECONDS;

    // When each instance's intro wait began (Steer, world thread only).
    extern std::unordered_map<uint32, uint32> introWaitSinceMs;

    // [ulda-ritual-hold] instance id -> end of the ritual channel hold (world thread only).
    extern std::unordered_map<uint32, uint32> ritualHoldUntilMs;

    // [guard_target_keep] (party seq, kept guard, the guard it was kept over), logged once
    extern std::set<std::tuple<uint32, ObjectGuid, ObjectGuid>> guardKeepLogged;

    // [target_commit] Target hysteresis: an idle target the group walks to is kept until it is reached, dies or is
    // skipped (unreachable, closed door: then it is no longer scored), unless a challenger scores better by
    // max(TARGET_COMMIT_MARGIN, TARGET_COMMIT_SHARE x the kept target's path), fights already, is a boss or an
    // objective; at most TARGET_COMMIT_MAX_MS after the choice. brd-h 20261025-163739-brd-h-5A-20
    // 18:12:04-18:12:32: "full clear" flipped Peasant -> Medic -> Bloodhound -> Officer -> Medic -> Footman every 4-8 s (scores
    // 1055 -> 794), the tank ran up and down a staircase. false: the best score every retarget (before).
    constexpr bool ROUTE_TARGET_COMMIT = true;

    constexpr float TARGET_COMMIT_MARGIN = 40.0f;

    constexpr float TARGET_COMMIT_SHARE = 0.25f;

    constexpr uint32 TARGET_COMMIT_MAX_MS = 90 * IN_MILLISECONDS;

    // [target_commit] (party seq, kept target, challenger), logged once
    extern std::set<std::tuple<uint32, ObjectGuid, ObjectGuid>> targetCommitLogged;

    // [b30_overpull] party seq -> (hold until, door): the door spin hold of Retarget (world thread)
    extern std::unordered_map<uint32, std::pair<uint32, ObjectGuid>> doorSpinHolds;

    // [b30_overpull] door spin guard: the targets behind one closed door were skipped one per retarget (strat-n
    // 20261025-184229-strat-n-5D-6: "leader held at a closed door" / "behind skipped ... (leader held at a closed
    // door ...)" one a second for 7.5 min, 0 pulls). From the DOOR_SPIN_SKIPS-th such skip within
    // DOOR_SPIN_WINDOW_MS the retarget holds DOOR_SPIN_HOLD_MS, logged once per DOOR_SPIN_LOG_MS.
    constexpr uint32 DOOR_SPIN_SKIPS = 4;

    constexpr uint32 DOOR_SPIN_WINDOW_MS = 20 * IN_MILLISECONDS;

    constexpr uint32 DOOR_SPIN_HOLD_MS = 8 * IN_MILLISECONDS;

    constexpr uint32 DOOR_SPIN_LOG_MS = 30 * IN_MILLISECONDS;

    struct DoorSpinCount
    {
        uint32 sinceMs = 0;
        uint32 skips = 0;
        uint32 loggedMs = 0;
    };

    extern std::unordered_map<uint32, DoorSpinCount> doorSpinCounts;  // party seq (world thread)

    // One door skip counted; true: the hold starts now (doorSpinHolds set), `log`: its line is due.
    bool CountDoorSpin(uint32 seq, ObjectGuid door, uint32 now, bool& log, uint32& skips, uint32& windowMs);

    extern std::set<std::pair<uint32, ObjectGuid>> gateKeyHeldLogged;

    extern std::set<std::pair<uint32, ObjectGuid>> phasedBossLogged;

    extern std::set<std::pair<uint32, ObjectGuid>> starterKeyLegLogged;

    extern std::set<std::pair<uint32, ObjectGuid>> pullAloneLogged;  // [sv-bog-overlord-alone]

    extern std::set<std::pair<uint32, ObjectGuid>> fledSurvivorLogged;  // [fled_survivor]

    extern std::set<std::pair<uint32, ObjectGuid>> lockoutClearLogged;  // [b44_ring] "brd_ring_lockout_clear" lines

    // [sv-bog-overlord-alone] Trash pulled alone and before what stands within its reach: The Steamvault's Bog
    // Overlord 21694 / heroic 21914 (aggro reach ~30 yd: sv-h 2026-10-24 it stopped three approaches to the
    // Coilfang packs round it as their blocker, then came with them - four deaths).
    constexpr std::array<uint32, 2> PULL_ALONE_FIRST = {21694, 21914};

    constexpr float PULL_ALONE_BONUS = 200.0f;

    // [fled_survivor] a fled, damaged mob on the trail (FledMobs) is the next target before anything further on
    constexpr float FLED_SURVIVOR_BONUS = 400.0f;

    constexpr float FLED_SURVIVOR_BEHIND = 60.0f;  // within this of the leader, trail or not

    constexpr float FLED_SURVIVOR_TRAIL = 30.0f;   // within this of a point of the tank's recorded trail

    constexpr float PULL_ALONE_ASSIST = 10.0f;  // CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS (default 10)

    // Rest hold (2026-10-24, *.bak_approach2), false restores the walk right after a pull: no new target and no
    // walk while a member close by sits eating / drinking (RestState::IsEating / IsDrinking: the food / drink
    // aura, not a passive MOD_POWER_REGEN like Blessing of Wisdom) or, for REST_HOLD_SIT_GRACE_MS, while one is
    // below the rest line (RestState::REST_MANA_PCT / REST_HEALTH_PCT) with water / food in the bags - the time
    // the bot's own "rest mana" / "rest health" takes to sit down. Stockade wave 40, 17:09:57: the target came
    // with pull_end and the leader walked off while the members were about to drink. The regen wait in
    // TestHarnessMgr holds the same way before Retarget / Steer run; this holds what gets past it (the pull_end
    // tick, a member sitting down later). At most REST_HOLD_MAX_MS, then REST_HOLD_COOLDOWN_MS without it.
    constexpr bool ROUTE_REST_HOLD = true;

    constexpr uint32 REST_HOLD_SIT_GRACE_MS = 10 * IN_MILLISECONDS;

    constexpr uint32 REST_HOLD_MAX_MS = 60 * IN_MILLISECONDS;

    constexpr uint32 REST_HOLD_COOLDOWN_MS = 45 * IN_MILLISECONDS;

    struct RestHold
    {
        uint32 sinceMs = 0;
        uint32 graceSinceMs = 0;  // the first out-of-combat look since the last fight: the sit-down grace
        uint32 cooldownUntilMs = 0;
        bool logged = false;    // rest_hold "waiting" logged, the end is logged too
        bool timedOut = false;  // REST_HOLD_MAX_MS ran out (Steer logs it)
        std::string members;
    };

    // Per instance (Retarget / Steer, world thread only).
    extern std::unordered_map<uint32, RestHold> restHolds;

    // [regen_wait_below_line] ROUTE_REST_HOLD as the regen wait (TestHarnessMgr): a sitter above its own line and
    // a standing member above the hard lines hold no new target (DOWNTIME_AUDIT 2026-10-25).
    constexpr bool REST_HOLD_BELOW_LINE = true;

    // Food (spell category 11) or water (59) in the bags: TestHarnessMgr's CountRestItems (file local there).
    bool HasRestItem(Player* bot, uint32 spellCategory);

    // Utgarde Keep (ROUTE_FORGE_ORDER): the trash of a locked forge (Metalworkers 14.8-19.3 yd from their Forge
    // Master, acore_world map 574) waits with its master.
    constexpr float FORGE_STATION_RADIUS = 22.0f;

    // Retarget: the next boss waits while idle trash stands in his room (ROUTE_BOSS_ROOM_FIRST).
    constexpr float BOSS_ROOM_TRASH_RADIUS = 40.0f;

    constexpr float BOSS_ROOM_TRASH_DZ = 10.0f;

    constexpr float BOSS_ROOM_TRASH_PENALTY = 5000.0f;

    // Boss-room trash = trash that joins the boss fight by the core's own mechanics (JoinsBossFight: assistance,
    // aggro at the fight spot, formation, script), no distance-only room. RFC 2026-10-24: an Earthborer on the
    // ledge 9.6 yd above Oggleflint, 34 yd from him in a straight line (inside the old 40 yd / dz 10 room), walks
    // 370 yd round the lower cave to him - the tank ran back 150 yd for it. Aggro at the fight spot needs a walk to
    // the boss of at most BOSS_WALK_RATIO x the straight distance + BOSS_WALK_EXTRA (the same floor).
    constexpr float BOSS_WALK_RATIO = 1.5f;

    constexpr float BOSS_WALK_EXTRA = 10.0f;

    // Cached walks to the boss (map, boss spot, mob spot rounded to this): the spawns stand still, a patrol
    // fills a few cells more. Cleared when it holds BOSS_ROOM_CACHE_MAX.
    constexpr float BOSS_ROOM_CACHE_CELL = 2.0f;

    // A patrol node's line of sight to the boss is tested from this height above the node.
    constexpr float PATROL_EYE_HEIGHT = 2.0f;

    constexpr size_t BOSS_ROOM_CACHE_MAX = 20000;

    // A detour (ROUTE_SKIP_DETOUR): idle trash whose walk from the leader is longer than DETOUR_RATIO x its
    // straight distance and longer than that + DETOUR_EXTRA stands on another floor or ledge - it reaches the
    // group only over that walk, so it neither blocks the route nor joins the fight here. RFC 2026-10-24: the
    // tank at Oggleflint's door took an Earthborer 12 yd away, 11 yd up on a ledge, over a 206 yd route back
    // west through the lower cave. Detours are evaluated past MAX_PATH_CANDIDATES up to DETOUR_EXTRA_PATHS more.
    constexpr bool ROUTE_SKIP_DETOUR = true;

    // Everything left was a detour (wave 8/10: ulda-h, ohb-h, ubrs-n stood 2+ minutes with "no target" and dozens
    // of detour skips): the detours are targets for DETOUR_FALLBACK_MS - the long way is still honest walking.
    constexpr bool ROUTE_DETOUR_FALLBACK = true;

    constexpr uint32 DETOUR_FALLBACK_MS = 3 * MINUTE * IN_MILLISECONDS;

    constexpr uint32 DETOUR_EXTRA_PATHS = 4;

    // Trash no farther than the boss's own walk + this is on the boss's way, no detour (RFC wave 38).
    constexpr float DETOUR_BOSS_WAY_MARGIN = 60.0f;

    constexpr uint32 DETOUR_LOG_MS = MINUTE * IN_MILLISECONDS;

    constexpr bool ROUTE_BACKBONE_COMPLETE_ONLY = true;

    // Pass 3 kill switches (wave 10), false restores the pass 2 behaviour of that piece:
    // - the harness drive (removed in [lead2]: it never ran, LeaderOwnWalkStalled keeps its stall watch);
    // - the event chain radius and the far-target guard in Retarget;
    // - the same-entry twins skipped with an unreachable mob (steer_stuck);
    // - Oculus "complete walks only" restricted to the drake phase (after Drakos), a short retry for a boss that
    //   was not pulled;
    // - an event hold spot: no careful-pull deferral there and a short pull wait (Black Morass: the refused rift
    //   pack broke Medivh's shield), and a spot with fightOnlyAttackers takes no idle targets (Azjol-Nerub);
    // - UseItem starters: dismount, step out of the object, a failed cast is no use;
    // - the boss room cleared before its boss (BOSS_ROOM_TRASH_PENALTY).
    constexpr bool ROUTE_BOSS_ROOM_FIRST = true;

    constexpr uint32 MECHANAR_MAP_ID = 554;  // [hleft-mech-bridge]

    // - a used-up gossip / escort starter armed again after a wipe once its gossip is offered again.
    constexpr bool ROUTE_STARTER_REARM = true;

    constexpr bool ROUTE_CHAIN_GUARD = true;

    constexpr bool ROUTE_OCULUS_DRAKE_ONLY = true;

    constexpr bool ROUTE_HOLD_SPOT_FIGHT = true;

    // Event pass (2026-10-24, *.bak_events), false restores the behaviour before it:
    // - a walk-in event spot whose boss is the next one of the route and not spawned yet is walked to first: idle
    //   trash counts only on the way there (Utgarde Pinnacle: Svala, areatrigger 5140);
    // - a scripted intro that keeps its boss immune (Svala's Arthas talk): no new pulls, the group waits;
    // - a gossip / escort starter is not talked to while its creature walks a scripted point path (Zul'Farrak:
    //   Bly's crew, Creature::PauseMovementForInteraction);
    // - Utgarde Keep: a locked Dragonflayer Forge Master (previous forge not engaged yet) is no route blocker, its
    //   forge's trash waits and a route through its aggro reach scores as crossing a skipped pack.
    constexpr bool ROUTE_EVENT_SPOT_FIRST = true;

    constexpr bool ROUTE_INTRO_WAIT = true;

    constexpr bool ROUTE_FORGE_ORDER = true;

    // Wave 21 (*.bak_w21idle), false restores the behaviour before it: a usable starter comes first while the next
    // boss is skipped behind a closed door ("starter_door_priority", once per DOOR_STARTER_LOG_MS and instance).
    constexpr bool ROUTE_DOOR_STARTER_FIRST = true;

    constexpr uint32 DOOR_STARTER_LOG_MS = 60 * IN_MILLISECONDS;

    extern std::unordered_map<uint32, uint32> doorStarterLogMs;  // instance id -> last "starter_door_priority" line

    extern std::unordered_map<uint32, uint32> leadSpotFirstLogMs;  // instance id -> last "brd_ring_lead" line

    extern std::unordered_map<uint32, uint32> eventBossWaitLogMs;  // instance id -> last "event_boss_waits" line

    extern std::unordered_map<uint32, uint32> eventStartHoldLogMs;  // [event_spot_hold] instance id -> last line

    // [b35_brd2] Key door chains: a closed door the walk of a promoted key starter runs through (instance id ->
    // door entry -> last ms seen). A key starter of that door counts as one for the blocking door (below). Fresh
    // for longer than STARTER_STUCK_RETRY_MS: the blocked starter's object may sit out a stuck skip meanwhile.
    constexpr uint32 KEY_DOOR_CHAIN_FRESH_MS = 3 * MINUTE * IN_MILLISECONDS;

    extern std::unordered_map<uint32, std::unordered_map<uint32, uint32>> keyDoorChain;

    extern std::unordered_map<uint32, uint32> fallbackFloorLogMs;  // [b34_arc] instance id -> last fallback line

    // [brd_seven_fast] A creature in an active hold spot's area (holdAlways, or within holdRadius) seen NOT
    // attackable (immune to PC / friendly / non-attackable) and attackable later: a scripted wave (Tomb of the
    // Seven: every 30 s the next dwarf gets FACTION_DARK_IRON_DWARVES + SetImmuneToPC(false), and attacks only if
    // its SelectNearestPlayer(130) - GM watchers included - passes CanCreatureAttack). It is a target at once:
    // never dropped by fightOnlyAttackers, sorted as an objective, no boss buff / ready check before it.
    // brd-h 20261025-163739 18:25:46 Hate'rel stood hostile and idle 37 s ("no_targets", hostilesSeen 0), the
    // hold walk pulled the tank back off him; the core resets the whole event after 15 s with no dwarf in combat.
    // instance id -> guid -> 0 seen not attackable, else the ms it became attackable.
    extern std::unordered_map<uint32, std::unordered_map<ObjectGuid, uint32>> eventWaveFlipMs;

    bool EventWaveFresh(uint32 instanceId, ObjectGuid const& guid);

    extern std::unordered_map<uint32, uint32> passageDetourLogMs;  // instance id -> last "passage_door_detour" line

    constexpr uint32 PASSAGE_DETOUR_LOG_MS = 60 * IN_MILLISECONDS;

    // Route-only trash (2026-10-24, *.bak_routeonly), false restores the full clear: the group clears a ROUTE,
    // not the whole instance. User rule: a mob standing in water may be left unless it is a boss; not everything
    // everywhere has to die - optimise the way and kill only what really has to be killed. The backbone is the
    // navmesh route from the leader to the next boss (Retarget's nextRoute, no second search; over an open link
    // its two legs), plus the walk to a walk-in event spot (leadSpot) and to the nearest pending starter. An idle
    // trash mob is a target only when (1) its aggro reach (AggroReach, the approach margin included) covers that
    // route where it sees it (BlocksRoute) or it would join the next boss's fight (JoinsBossFight: assistance,
    // aggro at the fight spot, formation, script); (2) it is required (a route link's or
    // a closed area's afterDead, a pending starter's afterDead; the event chain's trash within
    // EVENT_CHAIN_TRASH_RADIUS); (3) it patrols across the route or the boss room (its waypoint path); or (4) it
    // fights already, or is the target the group walks to. Everything else is left standing - a mob in water
    // (MAP_LIQUID_STATUS_SWIMMING at its position) also under the boss-room, chain and patrol rules: only when it
    // blocks the route at reach, joins the boss or an objective needs it. Bosses are never left. Not applied
    // without a backbone (no next boss / spot), at a hold spot or an escort (they fight only what comes) and in
    // raids (trash gates the harness does not model); nothing left on the route and no boss to walk to: the full
    // clear for that retarget (an event the harness does not model may need the rest).
    constexpr bool ROUTE_ONLY_TRASH = true;

    // Line-of-sight tests per mob on the route stretch inside its reach (SeesFromPoint, the aggro test).
    constexpr uint32 ROUTE_ONLY_LOS_TRIES = 12;

    // A starter of several objects measures the walks to this many of the nearest ones (Retarget).
    constexpr uint32 STARTER_PATH_CANDIDATES = 4;

    constexpr float STARTER_PATH_SORT_RANGE = 150.0f;  // farther ones are not measured after the nearest

    // A walk that is not complete still arrives when it ends this close to the object (its model, a floor
    // polygon next to it): the leader stands within STARTER_RANGE of it there.
    constexpr float STARTER_PATH_ARRIVE = 5.0f;

    bool StarterPathArrives(PathResult const& path, WorldObject const* object);

    // StarterTrack::stuckUntilMs: the leader's walk to this object got stuck a short while ago.
    bool StarterObjectStuck(StarterTrack const& starter, ObjectGuid const& guid, Player* leader);
    // [lead132] the doors of the instance standing open (a change ends the route's skips)
    uint32 OpenDoorCount(Map* map);

    float NearestDist(std::vector<Position> const& anchors, WorldObject const* object);

    // Summoned creatures (event bosses and their waves: Tuten'kash, Gahz'rilla, Grubbis, Mutanus, Vazruden and
    // Nazan, the Hellfire Sentries, Sezz'ziz / Nekrum) have no spawn id, so Map::GetCreatureBySpawnIdStore never
    // lists them and the route never chose them (wave 4/5 Ramparts: no target with both sentries standing).
    // The ones near the leader come from a grid search.
    constexpr float SUMMON_SCAN_RANGE = 150.0f;

    constexpr float CHAIN_SUMMON_SCAN_RANGE = 300.0f;  // a chain anchor's summons (Culling waves round Arthas)

    struct SummonedCreatureCheck
    {
        bool operator()(Creature* creature) const { return creature->IsInWorld() && !creature->GetSpawnId(); }
    };

    // A pending gossip / escort starter summoned by the instance far from the leader (Culling of Stratholme:
    // Chromie 27915 appears at the city entrance once the five crates are done, ~215 yd from where the last crate
    // left the leader, outside SUMMON_SCAN_RANGE - cos-h build 15 stood 4 minutes with "no target" while the
    // bots' "cos chromie middle" trigger fired): found by entry within STARTER_SUMMON_SCAN_RANGE.
    constexpr float STARTER_SUMMON_SCAN_RANGE = 500.0f;

    struct SummonedEntryCheck
    {
        std::vector<uint32> const& entries;
        bool operator()(Creature* creature) const
        {
            return creature->IsInWorld() && !creature->GetSpawnId() &&
                   std::find(entries.begin(), entries.end(), creature->GetEntry()) != entries.end();
        }
    };

    std::vector<Creature*> RouteCreatures(Map* map, Player* leader);

    // [b32_smpull] `mob` is a mob of Commander Mograine's call while he stands idle with his chapel
    // (ClassicShared.h): the boss's linked trash, cleared before him.
    bool LinkedChapelTrash(Player* leader, Creature* mob);

    // [b46_strat] Wing order: in a dungeon whose wings stand on their own (Stratholme: the living side Timmy ..
    // Balnazzar, the undead side behind the Gauntlet Gates Baroness .. Baron), a next boss whose route runs into a
    // pack the planner refused as hopeless (hopelessPacks) with no way round it (closed doors and those packs kept
    // out) for HOPELESS_WING_HOLD_MS is deferred: the other wing's bosses come first, and the deferred ones come
    // back once nothing else stands (or after HOPELESS_WING_DEFER_MAX_MS). strat-n 20261026-093620-strat-n-5A-3:
    // Timmy's only way (Market Row north) ran through a 30-mob Patchwork Horror / Skeletal Berserker chain
    // ("careful pull hopeless") from 1380 s; Timmy stayed the next boss, the Gauntlet Gate 175357 key stayed a
    // non-priority starter (no boss "behind a closed door"), and everything behind the gate was "route through a
    // closed door (Gauntlet Gate 175357)" until the stop at 1788 s.
    constexpr uint32 HOPELESS_WING_HOLD_MS = MINUTE * IN_MILLISECONDS;

    constexpr uint32 HOPELESS_WING_CHECK_MS = 10 * IN_MILLISECONDS;

    constexpr uint32 HOPELESS_WING_DEFER_MAX_MS = 45 * MINUTE * IN_MILLISECONDS;

    bool HopelessWingMap(uint32 mapId);

    struct HopelessWingState
    {
        ObjectGuid checkedBoss;
        uint32 checkedMs = 0;
        bool checkedHeld = false;
        std::string heldBy;
        uint32 heldSinceMs = 0;
        std::set<ObjectGuid> deferred;
        uint32 deferredSinceMs = 0;
    };

    extern std::unordered_map<uint32, HopelessWingState> hopelessWings;  // Party::seq -> state, world thread only

    bool HopelessWingDeferred(uint32 seq, ObjectGuid const& boss);

    // The route to `boss` enters the reach of an idle hopeless-refused mob (or the boss stands in it), and no
    // complete way round keeps out of those reaches and of the closed doors. heldBy: that mob's name.
    bool RouteHeldByHopelessPack(Player* leader, Creature* boss, Movement::PointsArray const& route,
                                 std::string& heldBy);

    // Creature::MoveInLineOfSight aggroes within GetAggroRange + m_CombatDistance (3D, no object sizes).
    float RealAggroRange(Creature* mob, Player* leader);

    // Walks the route from the leader up to `length` yd: the last point before the first one inside an idle
    // mob's aggro reach (the leader's own position when it stands in the margin already); when that mob is out of
    // sight from there (a corner), the first point that sees it, so the tank can pull what stopped it - but never
    // a point inside the real aggro range (no margin) of a mob that sees it: then the hidden point before the
    // corner. False: the walk stays out of every reach.
    //
    // Every idle mob counts, whichever pack the route is heading for and whether the route skipped it (a refused
    // pack in the street is the one that must stop the walk): the target's surroundings and the route's stretch.
    //
    // unseenStop: the blocker stopped the walk without seeing the route (the follower spread on its floor,
    // APPROACH_FOLLOW_SPREAD); the stop is then the last point before its spread reach.
    // ROUTE_FAST_RESPAWN_PASS (build 11): trash whose own spawn comes back within seconds (respawn delay
    // FAST_RESPAWN_MIN_S..FAST_RESPAWN_MAX_S) is no route target and stops no walk: the group walks past it and
    // fights it only when it attacks. 2026-10-25 hol-h (build 10): the 14 Slag 28585 of Volkhan's hall (respawn
    // 20 s, no spawner - DB spawns) were chosen "blocks route" 19 times and stopped every approach to Volkhan,
    // 147 Slag kills and two trash_loop windows in 25 minutes, Volkhan never engaged. The only such dungeon trash
    // in the DB (the 5 s Shadow Labyrinth Cabal adds / Shattered Halls Oozelings and the 0 s BRD Reservists are
    // event spawns, below the minimum).
    constexpr bool ROUTE_FAST_RESPAWN_PASS = true;

    constexpr uint32 FAST_RESPAWN_MIN_S = 10;

    constexpr uint32 FAST_RESPAWN_MAX_S = 60;

    // [b32_sh_ooze] ROUTE_RESPAWN_IGNORE: the DB minimum above leaves out the 5 s spawns. sh-h
    // 20261025-223237-sh-h-5B-71: the four Creeping Oozeling 17357 sewer patrols (creature 151085-151088,
    // spawntimesecs 5, MovementType 2, paths 1510850-1510880 over x 116-206 y 154-258 - no summoner, no SmartAI
    // summon) were "patrol crosses route" / "blocks route" from 23:17 to 23:27, ~45 kills, the tank turning back
    // to each new one. User rule 2026-10-25 («мы же знаем время респауна моба»): decided up front from the DB
    // respawn delay (creature.spawntimesecs, FastSpawns, cached per map), no kill counting. A hostile non-boss
    // DB spawn with a respawn of 1..RespawnIgnored::RESPAWN_IGNORE_MAX_S (120 s: shorter than the group needs to
    // clear an area, rest and move on, so it would be back before the group is gone) is a fast respawner from
    // the start (respawn_ignored, logged once per entry at the run's first target scan): the group passes its
    // area and does not come back - no route target, no "patrol crosses route", no walk stop, no tank pick while
    // idle, on any pass; fought when it aggroes (or stands in the corridor and aggroes on the walk). Required
    // objective entries stay targets. Summons stay ENDLESS_SUMMON_KILLS's (an-n Hadronox's Anub'ar waves are
    // summons, untouched). Its spawn points (and where its patrols are seen) are the respawn zone
    // (RespawnZoneOnWalk): a cost for re-entries and the reason a re-entered member is met by the group
    // (TickStrandedReenter); RespawnIgnored::Any widens the dead's resurrecter range (ReleaseSpiritAction.cpp).
    constexpr bool ROUTE_RESPAWN_IGNORE = true;

    constexpr float RESPAWN_ZONE_MERGE = 15.0f;      // zone points closer than this are one

    constexpr size_t RESPAWN_ZONE_MAX_POINTS = 256;  // per instance

    extern std::unordered_map<uint32, std::vector<G3D::Vector3>> respawnZones;  // instance id -> points (world thread)

    bool FastRespawner(Creature const* creature);

    // [b32_sh_ooze] The fast-respawn spawns of `mapId` from the creature table: spawntimesecs
    // 1..RESPAWN_IGNORE_MAX_S, hostile to players, no boss / civilian / trigger / NPC service (cached per map;
    // world thread).
    struct FastSpawn
    {
        uint32 entry = 0;
        uint32 respawnSec = 0;
        G3D::Vector3 pos;
    };

    std::vector<FastSpawn> const& FastSpawns(uint32 mapId);

    void AddRespawnZonePoint(uint32 instanceId, G3D::Vector3 const& point);

    // [b30_mech_pos] [mech_bridge_far] skip lifts per bridge mob while the pack keeps its size (party seq ->)
    struct MechBridgeLifts
    {
        uint32 packAlive = 0;
        std::map<ObjectGuid, uint32> count;
        bool cappedLogged = false;
    };

    extern std::unordered_map<uint32, MechBridgeLifts> mechBridgeLifts;  // world thread only

    // Approach to sight (ROUTE_APPROACH_TO_SIGHT): the stop the leader walks to instead of the one ApproachStop
    // chose, because that one did not show the blocker to the tank. Per leader, for one target and blocker.
    struct SightStop
    {
        ObjectGuid target;
        ObjectGuid blocker;
        G3D::Vector3 point;
        bool intoReach = false;  // found without a keep-out pack: the planner had accepted the pack's plan
    };

    extern std::unordered_map<ObjectGuid, SightStop> sightStops;  // world thread only

    struct SightHold
    {
        ObjectGuid target;
        ObjectGuid blocker;
        uint32 checkedMs = 0;
        uint32 loggedMs = 0;
        uint32 unknownSinceMs = 0;     // held at the edge of this blocker since then
        std::vector<ObjectGuid> pack;  // the keep-out pack: not marked as unseen by the wait's skip
        // [lead132] the facts the hold was searched for: the blocker's and the leader's spot, the planner's verdict
        Position blockerAt;
        Position leaderAt;
        uint8 verdict = 0;
    };

    extern std::unordered_map<ObjectGuid, SightHold> sightHolds;  // world thread only

    struct BossReady
    {
        ObjectGuid target;
        uint32 sinceMs = 0;
    };

    extern std::unordered_map<ObjectGuid, BossReady> bossReady;  // per leader, world thread only

    // Room trash that brings the idle boss into its fight: the same faction within the call for assistance
    // (Creature::CallAssistance: CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS, object sizes included, repeated every
    // assistance period; a margin for the steps it takes when pulled). Magisters' Terrace waves 10/11: the
    // Wretched pair 10.6 yd from Selin was the route's pick, pulled her each time and wiped.
    constexpr float ASSIST_LINK_MARGIN = 3.0f;

    // Creature::CanAssistTo(caller, enemy, checkFaction) as the core runs it when the caller calls for help,
    // without its last test (the helper sees the enemy: the group is not there yet) - the enemy only has to be
    // hostile to it (no enemy: not tested).
    bool CanAnswerAssist(Creature* helper, Creature* caller, Player* enemy, bool checkFaction);

    // The pulled trash brings the idle boss (Creature::CallAssistance from the trash: AnyAssistCreatureInRangeCheck
    // - the boss can assist, within the radius of the trash, the trash sees him).
    bool CallsBossForHelp(Creature* trash, Creature* boss);

    // Route-only trash (ROUTE_ONLY_TRASH): the polylines the group walks to its next objectives (the next boss,
    // a walk-in event spot, the nearest pending starter), each from the leader's position.
    using RouteBackbone = std::vector<Movement::PointsArray>;

    // A backbone leg ending this far from its goal without reaching the goal's polygon is incomplete
    // (ROUTE_BACKBONE_COMPLETE_ONLY); its "route backbone incomplete" line once per leader and leg kind this often.
    constexpr float ROUTE_BACKBONE_END_LEFT = 5.0f;

    constexpr uint32 ROUTE_BACKBONE_LOG_MS = 30 * IN_MILLISECONDS;

    extern std::unordered_map<uint64, uint32> backboneIncompleteLogMs;  // world thread only

    // Route visualisation ("playerbots route show"): the backbone Retarget built last, per party seq. Written by
    // Retarget, read by TickRouteViz; both on the world thread.
    struct RouteSnapshot
    {
        RouteBackbone legs;     // the backbone legs (complete ones)
        RouteBackbone dropped;  // legs left out as incomplete (ROUTE_BACKBONE_COMPLETE_ONLY)
        std::string routeType = "none";
        bool routeOnly = false;
        ObjectGuid nextBoss;
        std::string nextBossName;
        uint32 builtMs = 0;
    };

    extern std::unordered_map<uint32, RouteSnapshot> routeSnapshots;  // world thread only

    G3D::Vector3 ClosestOnSegment(G3D::Vector3 const& point, G3D::Vector3 const& from, G3D::Vector3 const& to);

    // Route-only reach (2026-10-24, *.bak_approach2): the real aggro range against the group's level (the leader
    // is of it), ROUTE_REACH_MARGIN and the wander radius - not AggroReach's approach margin, which keeps the
    // leader's stop outside and is no reason to fight a mob. Stockade wave 40, 17:08:38: a Defias Inmate in a
    // cell (149.8,-69.5,-34.9) that bothered nobody was the target on the way back to the centre (19 route
    // candidates, 22 off the route). Patrols keep AggroReach (they walk into the route).
    constexpr float ROUTE_REACH_MARGIN = 3.0f;

    float RouteReach(Creature* mob, Player* leader);

    // What BlocksRoute found, for the target log: the mob's nearest distance to the backbone (3D; -1: no segment
    // within reach of it in x / y) and whether a route point inside its reach was in its line of sight.
    struct RouteCheck
    {
        float routeDist = -1.0f;
        float routeDz = 0.0f;   // the mob's height over (+) / under (-) the backbone at routeDist
        bool routeLos = false;
        bool zGated = false;    // a route point inside the reach that only the core's Z gate keeps idle
    };

    // The mob aggroes on the route: a route point inside its reach (3D, as Creature::MoveInLineOfSight measures)
    // and its Z gate (AggroesAtPoint) from which it sees the leader (SeesFromPoint) - the open space between
    // them, not the distance alone. Per
    // segment its nearest point, then the points of the segment's stretch inside the reach (half and nine
    // tenths of the way to the reach's edge, both sides): a mob in a Stockade cell sees the corridor only through
    // its door, and the nearest point lies in front of the cell wall. At most ROUTE_ONLY_LOS_TRIES line-of-sight
    // tests: a cave beside the corridor behind rock stays out.
    bool BlocksRoute(Player* leader, Creature* mob, float reach, RouteBackbone const& backbone, RouteCheck& check);

    float SegmentsDistance2d(G3D::Vector2 const& a, G3D::Vector2 const& b, G3D::Vector2 const& c,
                             G3D::Vector2 const& d);

    // The walk from a spot to the boss's spot with the walker's filters (RoutePath from the spot: complete, or
    // ending beside a gap the leader walks over), -1 when it does not get there. Cached per map, boss spot and
    // spot (BOSS_ROOM_CACHE_CELL).
    struct BossRoomKey
    {
        uint32 mapId;
        int32 bossX, bossY, bossZ, x, y, z;
        bool operator<(BossRoomKey const& other) const
        {
            return std::tie(mapId, bossX, bossY, bossZ, x, y, z) <
                   std::tie(other.mapId, other.bossX, other.bossY, other.bossZ, other.x, other.y, other.z);
        }
    };

    extern std::map<BossRoomKey, float> bossWalkCache;  // world thread only

    // ROUTE_SKIP_DETOUR: when a detour was last logged per party (seq) and mob (DETOUR_LOG_MS).
    extern std::map<std::pair<uint32, ObjectGuid>, uint32> detourLoggedMs;  // world thread only

    float BossWalk(Player* walker, Creature* boss, G3D::Vector3 const& at);

    // On the boss's floor: the walk to him is about the straight distance (BOSS_WALK_RATIO / _EXTRA).
    bool WalkableToBoss(Player* walker, Creature* boss, G3D::Vector3 const& at);

    // C++ boss scripts that call for help when engaged (me->CallForHelp(radius) in JustEngagedWith): every idle
    // creature friendly to the boss within the radius (3D, object sizes included) that sees the group joins
    // (CallOfHelpCreatureInRangeDo / CanAssistTo(.., false): not engaged, aggressive, not immune to NPCs, hostile
    // to the group). The core tests the helper's line of sight to the boss's victim; here: to the boss.
    // Shadow Labyrinth: boss_blackheart_the_inciter.cpp:77 (wave 6: 18 adds came with Blackheart). Not listed
    // (yet): Emperor Dagran Thaurissan (boss_emperor_dagran_thaurissan.cpp:51, VISIBLE_RANGE 166 yd - most of the
    // Imperial Seat, Moira is friendly to him), Trial of the Champion's champions (phase 2, no trash in the
    // arena), Zul'Farrak's pyramid crew (friendly NPCs).
    struct ScriptHelpDef
    {
        uint32 mapId;
        uint32 bossEntry;  // the normal entry (Creature::InitEntry keeps it on heroic)
        float radius;
    };

    constexpr ScriptHelpDef SCRIPT_HELP_BOSSES[] = {
        {555, 18667, 100.0f},  // Blackheart the Inciter
    };

    // The boss's C++ call for help (SCRIPT_HELP_BOSSES) reaches this idle trash.
    bool ScriptCallForHelpReaches(Player* leader, Creature* boss, Creature* trash);

    // The boss's own SmartAI call for help (SMART_ACTION_CALL_FOR_HELP on himself: Creature::CallForHelp), the
    // largest radius of his spawn's and his entry's scripts; 0: none. Per entry and spawn.
    extern std::unordered_map<uint64, float> smartHelpRanges;  // world thread only

    float SmartCallForHelpRange(Creature* boss);

    // Trash that joins the next boss's fight (ROUTE_ONLY_TRASH, ROUTE_BOSS_ROOM_FIRST: his room is cleared before
    // him), by the core's mechanics: the reason, nullptr when it does not join (however near: a ledge, a floor
    // above or below him).
    // - "assist": the engaged boss calls for assistance (Creature::CallAssistance at combat start and every
    //   assistance period: AnyAssistCreatureInRangeCheck - the trash can assist him (CanAnswerAssist, same
    //   faction), within CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS of him (+ ASSIST_LINK_MARGIN for the steps of
    //   the pull) and the boss sees it);
    // - "aggro": the group fights at the boss's spot - the trash's aggro reach (AggroReach, the leader's level)
    //   covers that spot, it sees him (Creature::MoveInLineOfSight) and stands on his floor (WalkableToBoss);
    // - "formation": the boss's creature_formations group (members join their leader's fight);
    // - "script": the trash calls the idle boss when pulled (CallsBossForHelp), or the boss's SmartAI or C++
    //   (SCRIPT_HELP_BOSSES) call for help reaches it (CallOfHelpCreatureInRangeDo: friendly, within the radius,
    //   sees the boss / the group at him).
    // Not modelled: boss summons (the adds do not stand there before the pull).
    char const* JoinsBossFight(Player* leader, Creature* boss, Creature* trash);

    // [boss_area_clear] User rule 2026-10-25, every content (dungeons and raids): the idle trash round the next
    // boss is cleared before his pull, and a pack there is never skipped for want of a sight point - the body
    // pull (group ready first) is the last fallback. The trash counted: what joins his fight by the core's
    // mechanics (JoinsBossFight: assistance, aggro at his spot, script) and what stands in his room - within
    // BOSS_ROOM_TRASH_RADIUS of him on his floor (BOSS_ROOM_TRASH_DZ), in his sight and walkable to him on that
    // floor (WalkableToBoss; RFC: no ledge 370 yd round). Left out (part of the encounter by design, or no
    // clearing them): his formation (fought with him), trash that calls him when pulled (CallsBossForHelp: no
    // pull without him), summons / temporary spawns (no spawn id: they come back or belong to an event), and
    // BOSS_AREA_ENCOUNTER_ADDS.
    constexpr bool ROUTE_BOSS_AREA_CLEAR = true;

    // Encounter adds the tactic fights with the boss (not "trash round him"): Priestess Delrissa's companions
    // (Magisters' Terrace; she is the boss of their fight).
    constexpr uint32 BOSS_AREA_ENCOUNTER_ADDS[] = {
        24553, 24554, 24555, 24556, 24557, 24558, 24559, 24561,  // Delrissa's retinue (MgTShared.cpp)
        24552, 24656,                                            // their pets Sliver, Fizzle
        24722,                                                   // Selin's Fel Crystals (not trash)
    };

    // Skip reasons that say the mob cannot be reached or fought now: they stay skipped.
    bool ReachabilitySkip(std::string const& reason);

    // An unskipped boss-area mob is skipped again at most this often, BOSS_AREA_MAX_UNSKIPS times in all.
    constexpr uint32 BOSS_AREA_UNSKIP_MS = 60 * IN_MILLISECONDS;

    constexpr uint32 BOSS_AREA_MAX_UNSKIPS = 5;

    struct BossAreaUnskip
    {
        uint32 lastMs = 0;
        uint32 count = 0;
    };

    extern std::map<std::pair<uint32, ObjectGuid>, BossAreaUnskip> bossAreaUnskips;  // world thread only

    extern std::unordered_map<uint32, uint32> bossAreaHoldLogMs;  // party seq -> last line

    // [b33_mech2] Boss-area trash is looked for this far from the boss (BOSS_AREA_EXTRA reaches ~64 yd).
    constexpr float BOSS_AREA_SCAN_RANGE = 100.0f;

    // [b33_harn] The encounter footprint: where a boss's fight spreads besides his own spot - the stands of the
    // adds / summons his script puts down when he is engaged (not there while he stands idle, so data). A cheap
    // (BOSS_FOOTPRINT_PACK_MAX mobs at most) idle hostile mob inside it on his floor is his area trash, cleared
    // before him as his room is. ok-h 20261026-000434-ok-h-5C-36: a lone Savage Cave Beast (guid 131948, wanders
    // 5 yd round 370.4,-846.9) stood 44 yd from Amanitar (past BOSS_ROOM_TRASH_RADIUS) inside her mushroom field
    // (boss_amanitar.cpp MushroomPositions: 32 stands, x 328-401, y -842..-908); target_commit took her (92.7 over
    // the beast's 80.9) at 1592 s, the beast joined at +36 s and killed the group after she died.
    constexpr bool ROUTE_BOSS_FOOTPRINT = true;  // false: the build-33 behaviour

    constexpr uint32 BOSS_FOOTPRINT_PACK_MAX = 3;

    struct BossFootprintDef
    {
        uint32 bossEntry;
        float x;
        float y;
        float radius;  // 2D, round the centre of the add stands, margin included
    };

    constexpr BossFootprintDef BOSS_FOOTPRINTS[] = {
        {30258, 365.0f, -872.0f, 44.0f},  // Amanitar: mushrooms 30391 / 30435 (boss_amanitar MushroomPositions)
    };

    bool InBossFootprint(Player* leader, Creature* boss, Creature* mob);

    // Why `mob` is trash round `boss` that is cleared before him, nullptr when it is not.
    char const* BossAreaTrashOf(Player* leader, Creature* boss, Creature* mob, bool bossCreature);

    // A patrol whose walk passes within `reach` (2D) of the route on its floor (the height ranges of the two
    // segments overlap within BOSS_ROOM_TRASH_DZ), or within its reach of the next boss where it sees him on his
    // floor (it aggroes on the group fighting him: JoinsBossFight "aggro").
    bool PatrolCrossesRoute(Player* walker, WaypointPath const& path, float reach, RouteBackbone const& backbone,
                            Creature* boss);

    // A route through the aggro reach of an idle mob the route skipped (a pack the tank refused, one the tank did
    // not see): the walk would stop at that pack's edge (ApproachStop) and wait for a pull the tank already
    // refused. Such a target comes after the ones reached around it; chosen anyway, the leader stops at the
    // refused pack and the tank plans it again from its edge (a lone mob of it first). Stratholme 09:27 / 09:48:
    // after the refusals the next targets lay behind the street packs.
    constexpr float ROUTE_SKIPPED_PACK_PENALTY = 400.0f;

    constexpr float ROUTE_SKIPPED_SAMPLE_STEP = 2.0f;

    // Closed parts of the instance (AreaGateDef): the gate is meant for this run (onlyContent), a point is inside.
    bool AreaGateForContent(AreaGateDef const& gate, char const* contentKey);  // [lead107] the content's key

    bool InAreaGate(AreaGateDef const& gate, float x, float y, float z);

    // The closed parts that are a wing of another content (onlyContent set): kept out of the route and the tank's
    // own pulls too. The other gates (an event's area: the Ring of Law stands, Stratholme's acolytes) only keep
    // their mobs from being targets - a route past them stays a route.
    std::vector<AreaGateDef const*> KeepOutAreas(std::vector<AreaGateDef const*> const& gates);

    bool InAnyAreaGate(std::vector<AreaGateDef const*> const& gates, float x, float y, float z);

    // A route that walks through a closed part: the target is not reached that way (Blackrock Spire, LBRS waves
    // 2026-10-24: the UBRS hall lies between the Legionnaires' room and the lower city).
    bool RouteCrossesClosedArea(Movement::PointsArray const& points, std::vector<AreaGateDef const*> const& gates);

    constexpr uint32 CLOSED_DOOR_SKIP_MS = 60 * IN_MILLISECONDS;

    // [hleft-passage-door-route] The walk to `target` goes round `door` (a closed door that opens on a boss's
    // death, PassageDoorBoss) without running through any of `shut`: memoised per target for
    // PASSAGE_ROUND_MEMO_MS (the long search is not run on every retarget).
    constexpr uint32 PASSAGE_ROUND_MEMO_MS = 15 * IN_MILLISECONDS;

    struct PassageRoundMemo
    {
        ObjectGuid door;
        uint32 ms = 0;
        bool round = false;
    };

    extern std::map<InstanceGuid, PassageRoundMemo> passageRoundMemos;  // world thread only

    bool GoesRoundPassageDoor(Player* leader, InstanceScript const* script, WorldObject* target, GameObject* door,
                              std::vector<GameObject*> const& shut);

    constexpr uint32 DOOR_HOLD_FRESH_MS = 3 * IN_MILLISECONDS;  // Retarget reacts to a door_hold this recent

    // A DoorEventDef key creature whose earlier step has not started (GetData(afterData) 0): it evades when
    // engaged, so it is no target (Utgarde Keep: Forge Master 2 before forge 1, 3 before 2).
    bool DoorKeyLocked(Creature const* creature, InstanceScript const* script);

    // GuardedDoorDef: the alive guards of the objects still guarded for this closed door (empty: none left or no
    // such event for it).
    std::vector<Creature*> PendingDoorGuards(Map* map, GameObject const* door);

    // [st-guard-approach] The approach point of a guard of a door still closed (GuardedDoorDef::approaches).
    GuardApproachDef const* PendingGuardApproach(Map* map, Creature const* guard);

    // [hleft-cos-starters] Culling of Stratholme gossip starters held by the event's own state:
    // - Arthas 26499 while the heroic Infinite Corruptor detour runs (CoSHelpers::GetCorruptorDetourTarget: slot 4
    //   open, the Guardian of Time timer still long enough): his last stage sends him to Mal'Ganis mid-detour;
    // - Chromie 27915 (the middle one, summoned 20 s after the fifth crate) until the leader sees her (the
    //   second, early use of the old starter came before she was there).
    // [hleft-gd-galdarah-last] gundrak.h DATA_SLAD_RAN 0, DATA_MOORABI 1, DATA_DRAKKARI_COLOSSUS 2.
    constexpr uint32 GUNDRAK_MAP_ID = 604;

    constexpr uint32 GUNDRAK_GALDARAH_ENTRY = 29306;

    constexpr uint32 GUNDRAK_SLADRAN_SLOT = 0;

    constexpr uint32 GUNDRAK_MOORABI_SLOT = 1;

    constexpr uint32 GUNDRAK_COLOSSUS_SLOT = 2;

    constexpr uint32 COS_MAP_ID = 595;

    constexpr uint32 COS_ARTHAS_ENTRY = 26499;

    constexpr uint32 COS_CHROMIE_MIDDLE_ENTRY = 27915;

    bool GossipStarterHeld(Player* leader, Creature* creature);

    // EventStarterDef::afterBossDone: the script's boss slot is DONE (or no slot is named).
    bool ScriptBossDone(InstanceScript const* script, int32 bossId);

    bool RouteCrossesSkipped(Player* walker, Movement::PointsArray const& points, Creature* target,
                             std::vector<std::pair<Creature*, float>> const& skipped);

    // [b35_up] Utgarde Pinnacle: Skadi's gauntlet runs (areatrigger 4991 -> boss_skadi ACTION_START_ENCOUNTER sets
    // DATA_SKADI_THE_RUTHLESS IN_PROGRESS). Skadi rides Grauf NOT_SELECTABLE and out of combat meanwhile, so his
    // dbc BossTrack reads NOT_STARTED; spell_skadi_reset_check evades him (waves despawn, the event starts over)
    // once no player in the hall keeps Gauntlet Effect 47547. up-h 20261026-024524-up-h-5D-35: waves cleared at
    // 1231 s, the fallback target was a Mindless Servant 846 yd off, shortcut_reenter walked the group out at
    // 1232 s, Skadi reset (boss_rebound 1264 s); the second gauntlet (1402 s) began with harpoons still in hand,
    // the DPS ran ahead of the tank to the launchers and the healer, 30 yd behind, died first (death burst).
    bool SkadiGauntletRunning(Player* leader);

    // [b35_up] Inside Skadi's hall (the gauntlet lane, UPIds::HALL_*) with a margin: the waves spawn at
    // boss_skadi.cpp SpawnLoc (477.6,-484.6), just past HALL_Y_MAX.
    bool InSkadiHall(WorldObject const* object);

    // The boss of a scripted intro that keeps it immune while it runs (UPIds::IsSvalaIntroRunning: Svala's Arthas
    // talk after areatrigger 5140), nullptr when none runs.
    Creature* RunningIntroBoss(Player* leader);

    // The event spot's boss (an untilDead entry of a boss track) is the lowest open one of the route and its
    // creature is not known yet: it appears only through this event (Utgarde Pinnacle: Svala Sorrowgrave 26668,
    // order 0, comes from Svala 29281 after the intro).
    // EventSpotDef::guardEntries: this creature attacks the protected NPC of the hold spot.
    bool AtHoldGuard(EventSpotDef const& spot, Creature const* creature);

    // [brd_bar_buy] [brd_bar_turnin] Starters done at a creature (gossip, escort, vendor, quest giver) and the NPC
    // flag the creature shows while the step can be done.
    bool CreatureStarterKind(StarterKind kind);

    NPCFlags StarterNpcFlag(StarterKind kind);

    // [brd_bar_turnin] QuestTurnIn: every item the quest asks for is in the leader's bags (Rocknot's Ale 4295:
    // two Dark Iron Ale Mugs 11325).
    bool QuestItemsReady(Player* leader, EventStarterDef const* def);

    // Oculus after Drakos: Varos, Urom and Eregos are reached and fought from drakes (Ai/Dungeon/OC: essence
    // gossip, mount, pilot route, landing on the boss's island). The rings float apart with nothing walkable in
    // between, and bots no longer walk straight over open air (PlayerbotAI::GuardAirborneSpline), so the harness
    // does not route the group to another ring: while a member sits on a drake the OC strategy drives, and on foot
    // only what is walkable from the island counts (RoutePath).
    constexpr uint32 OCULUS_MAP_ID = 578;

    constexpr uint32 OCULUS_DATA_DRAKOS = 0;  // oculus.h DATA_DRAKOS, instance_oculus answers it in GetData

    bool OculusDrakePhase(Player* leader);

    // A living creature summoned by `summoner` (its retinue: Delrissa's lackeys).
    constexpr float BOSS_SUMMON_SEARCH = 200.0f;

    struct BossSummonCheck
    {
        ObjectGuid summoner;
        bool operator()(Creature* creature) const
        {
            if (!creature || !creature->IsAlive() || !creature->IsSummon())
                return false;
            TempSummon const* summon = creature->ToTempSummon();
            return summon && summon->GetSummonerGUID() == summoner;
        }
    };

    bool AnyMemberOnVehicle(LeaderGroup& group);

    // [boss_dead_encounter_open] the script slot of a boss creature, as the boss_state event knows it (BossTrack:
    // its bound creature, else its entry); -1 when the run tracks no slot for it.
    int32 BossSlotOf(LeaderGroup& group, Creature const* boss);

    // The door_spin_hold line: the door, the skips, and its opener (a key starter of the group, or none).
    JsonObject DoorSpinFields(LeaderGroup& group, Player* leader, GameObject const* door, uint32 skips,
              uint32 windowMs, char const* how, std::string& opener);

    // [lead5] LEADER_ONE_PICKER: EncounterOrder::IsLocked is the one encounter-order rule (the planner's and the
    // pick's: the Utgarde Keep forge order with its formations, Blackheart's room), not the harness's own copy of
    // the forge rule (UKIds::IsForgeMasterLocked) for the route's target choice.
    bool EncounterLockedFor(LeaderGroup& group, Creature* creature, InstanceScript const* script);

    // [b32_strat_gate] An auto-closing key gate (KeyDoorPassDef) the whole living group is through: every member on
    // the far side of the gate plane (away from its use point). A door_hold there is a walk BACK to what stands
    // behind it - no reason to key it again (strat-h 221216-5C-1: 175352 re-armed at 1040 s for trash behind it).
    bool GroupThroughKeyGate(LeaderGroup& group, GameObject const* door);

    // Wing runs (Scarlet Monastery, Dire Maul, Blackrock Spire): a hostile closer to a boss of another wing than
    // to every boss and entrance of this one belongs to the other wing.
    bool InThisWing(LeaderGroup& group, WorldObject const* object);

    // The entrance nearest (2D) to an object: the wing it stands in (Scarlet Monastery, Dire Maul).
    int32 NearestEntrance(LeaderGroup& group, WorldObject const* object);

    // ROUTE_REST_HOLD: the members the group waits for ("name(drinking)", ...); empty: no hold. A fight ends
    // it (the bots fight, the harness has nothing to hold), and so does an escort (the group keeps up with it).
    std::string RestHoldMembers(LeaderGroup& group, Player* leader);

    bool SpotBossIsNext(LeaderGroup& group, EventSpotDef const& spot, Player* leader);

    void Retarget(LeaderGroup& group, Player* leader);

    void StartWingChange(LeaderGroup& group, size_t next, char const* why);

    // [sm-wing-done] true while a living boss of the run stands in the wing of Party::entranceIdx (its nearest
    // entrance); nextWing: the entrance of the lowest-order boss left in another wing (-1: none).
    bool WingBossesLeft(LeaderGroup& group, Map* map, Player* leader, int32& nextWing);
}

#endif
