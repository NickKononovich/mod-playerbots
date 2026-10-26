/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ANTRIGGERS_H
#define PLAYERBOTS_ANTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "ObjectGuid.h"
#include "PlayerbotAIConfig.h"
#include "Position.h"
#include "Trigger.h"

#include <vector>

class Player;
class PlayerbotAI;
class Unit;

enum AzjolNerubIDs
{
    // Krik'thir the Gatewatcher
    NPC_KRIKTHIR                    = 28684,
    NPC_WATCHER_SILTHIK             = 28731,
    NPC_WATCHER_GASHRA              = 28730,
    NPC_WATCHER_NARJIL              = 28729,
    NPC_WATCHER_SKIRMISHER          = 28734,
    NPC_WATCHER_SHADOWCASTER        = 28733,
    NPC_WATCHER_WARRIOR             = 28732,
    DEBUFF_WEB_WRAP                 = 52086,
    NPC_WEB_WRAP                    = 28619,

    // Anub'arak
    // Impale Periodic on the boss places an Impale Target NPC under a player (see AnubarakImpaleTrigger).
    SPELL_IMPALE_PERIODIC           = 53456,
    SPELL_IMPALE_SPIKES             = 53457,
    SPELL_POUND_N                   = 53472,
    SPELL_POUND_H                   = 59433,
};

#define SPELL_POUND                 DUNGEON_MODE(bot, SPELL_POUND_N, SPELL_POUND_H)

namespace ANIds
{
// azjol_nerub.h
constexpr uint32 DATA_HADRONOX = 1;
constexpr uint32 DATA_ANUBARAK = 2;
constexpr uint32 NPC_HADRONOX = 28921;
// smart_scripts 29184: the spike erupts 4 s after the NPC appears under a player
constexpr uint32 NPC_IMPALE_TARGET = 29184;
constexpr float IMPALE_DANGER_RADIUS = 5.0f;
constexpr float IMPALE_SAFE_DISTANCE = 8.0f;

// boss_hadronox.cpp hadronoxSteps[2]: she webs the doors there and the fight belongs on this platform.
constexpr float HADRONOX_PLATFORM_X = 530.42f;
constexpr float HADRONOX_PLATFORM_Y = 560.003f;
constexpr float HADRONOX_PLATFORM_Z = 733.225f;
// Below this the ramp leads to her spawn ledge (z ~674), where the door swarm gathers.
constexpr float HADRONOX_PLATFORM_MIN_Z = 715.0f;
constexpr float HADRONOX_SHAFT_RADIUS = 70.0f;

// Upper (entrance, Krik'thir, Hadronox shaft) and lower (Anub'arak) navmesh parts are not connected; players drop
// through the hole in the shaft floor. The shaft has two floors over that spot (mmaps 601, tools/navmesh_probe.py):
// the web at z ~678 is closed (wave 6: the group stood on it at 533.5,549.2,676.4 for 25 minutes, mod-dungeon-clear's
// centre), the bottom floor at z ~645 has the hole (x 524-536, y 542-556). The drop is taken from that bottom floor.
constexpr float LOWER_KINGDOM_HOLE_X = 531.0f;
constexpr float LOWER_KINGDOM_HOLE_Y = 549.5f;
// The bottom floor around the hole.
constexpr float RAMP_BOTTOM_Z = 645.5f;
// Only the hole's west part falls clear into the pool (vmaps 601: open to the water at x 527.5-531.5, y 548-554;
// surface 288.5, bottom 275.9-280.3). Its east part ends on a ledge at z 385-420 (wave 15: the group landed at
// 534.9,549.5,393.9 and died of the fall). The drop is taken from the south rim, straight north over the pool.
constexpr float DROP_RIM_X = 529.5f;
constexpr float DROP_RIM_Y = 546.5f;
constexpr float DROP_POOL_X = 529.5f;
constexpr float DROP_POOL_Y = 551.0f;
constexpr float DROP_RIM_TOLERANCE = 1.0f;
// Water over the landing spot deep enough to swim in: a client entering water sends no fall damage.
constexpr float POOL_MIN_DEPTH = 2.0f;
// A bot under water this close (2D) to the landing spot is on the pool's bottom and swims up.
constexpr float POOL_SURFACE_RADIUS = 20.0f;
constexpr float UPPER_LEVEL_MIN_Z = 600.0f;
constexpr float LOWER_LEVEL_MAX_Z = 400.0f;
// Hadronox's platform hangs over the hole: only step towards it from the shaft floor.
constexpr float HOLE_FLOOR_Z_TOLERANCE = 12.0f;
constexpr float HOLE_CENTRE_TOLERANCE = 1.5f;
constexpr float HOLE_STEP_LENGTH = 2.0f;
// From the rim a short hop over the pool (a jump, as a player steps off: a straight walk over open air is cut at
// the edge by PlayerbotAI::GuardAirborneSpline), then the fall.
constexpr float HOLE_JUMP_SPEED_XY = 4.0f;
constexpr float HOLE_JUMP_SPEED_Z = 3.0f;
constexpr float HOLE_MIN_DROP = 100.0f;
// Anub'arak's Pound: a frontal cone (an-h 2026-10-25: it hit the healer for 24.7k and the hunter for 26.7k at once).
// Non-tanks stay out of this front arc and reach; to his side at their distance, within these bounds.
constexpr float POUND_REACH = 35.0f;
constexpr float POUND_SAFE_ARC = 2.0f * static_cast<float>(M_PI) / 3.0f;
constexpr float POUND_FLANK_ANGLE = 0.6f * static_cast<float>(M_PI);
constexpr float POUND_FLANK_MIN = 5.0f;
constexpr float POUND_FLANK_MAX = 25.0f;
constexpr float POUND_DODGE_RANGE = 8.0f;
constexpr float DROP_GATHER_RADIUS = 40.0f;

// Hadronox encounter running: everything below her platform is left alone so the group stays up there.
bool IsBelowHadronoxPlatform(Player* bot, Unit const* unit);
}  // namespace ANIds

// Krik'thir's room is one timed encounter, not trash (boss_krikthir_the_gatewatcher.cpp, instance_azjol_nerub.cpp,
// smart_scripts 28729-28731, creature_formations 12758-12760 groupAI 7 = assist both ways + evade together):
// - the three Watchers (lvl 74) each lead a formation of two Anub'ar; any member pulled brings its whole pack;
// - the first Watcher aggro (SmartAI "On Aggro - Do Action 1") starts Krik'thir's timers: a Watcher is sent at
//   60 + 5 s and 120 + 5 s, and Krik'thir joins at 180 s (200 s heroic) whatever is still alive, setting every
//   Watcher left in combat with the zone;
// - when the last member of a pack dies (OnUnitDeath + 1 s) the pending sends are cancelled, the next Watcher comes
//   5 + 5 s later and the one after it 70 + 5 s later: each pack has about 65 s before the next one stacks on it,
//   the kill order inside a pack does not move the timers (only the whole formation's death does);
// - SPELL_SUBBOSS_AGGRO_TRIGGER 52343 (max 1 target, conditions: a Watcher at 100 % health) puts one random
//   untouched Watcher in combat with the zone: it runs to the group with its pack, wherever the group fights;
// - a Watcher or a pack member evading resets Krik'thir and respawns every formation.
// An expert group therefore pulls ONE whole pack to a spot the other packs cannot see (aggro reach 22 yd for a
// lvl 72), kills it there with AoE, and lets Krik'thir send the next ones to that spot, 10 s apart. Wave 2026-10-24
// 09:39: the careful planner refused the 9-mob room (best 0 mobs), pulled one Skirmisher, the fight drifted into
// the room after it and all three Watchers stacked on the tank.
namespace ANWatcherRoom
{
constexpr uint32 MAP_AZJOL_NERUB = 601;
// Krik'thir's spawn (creature 127214); the room is the floor around it.
constexpr float KRIK_X = 529.6f;
constexpr float KRIK_Y = 646.2f;
constexpr float ROOM_RADIUS = 90.0f;
constexpr float ROOM_MIN_Z = 760.0f;
constexpr float ROOM_MAX_Z = 800.0f;
constexpr float SCAN_RANGE = 150.0f;
// The foot of the entrance ramp: 42 yd from Silthik's pack, 46 from Gashra's, 54 from Narjil's, 62 from
// Krik'thir (the tank stood there in pull 2 of the wipe run, z from its position).
constexpr float RALLY_X = 543.0f;
constexpr float RALLY_Y = 707.0f;
constexpr float RALLY_Z = 777.2f;
constexpr float RALLY_ARRIVED = 4.0f;
constexpr float GATHER_RADIUS = 20.0f;
// [b34_an2] The whole Watcher phase is fought from here: out of combat every member farther than this walks back
// (between sends, to rest, after each pack), and nothing generic (the harness walk, a LoS pull) takes it off
// (MoveConflict::NoteEventHold). 10 yd round the rally keeps 29 yd to Silthik's nearest member (550,668), the
// closest idle mob (creature 12762), and stays within the waypoint lane's free step (MoveConflict LANE_WIDTH).
// an-n 20261026-014521-an-n-5G-25: the harness route walked the tank to (546.8,693.5) ("test runner move"), its
// pull_after_hold body pull opened Silthik's pack there, the group stood on at (549,680) after the pack died (28 yd
// from Gashra, outside DRIFT_RADIUS), Narjil came to it past Gashra and both packs fought the group (3 dead).
constexpr float RALLY_HOLD_RADIUS = 10.0f;
// Trash outside the formations this close to the rally is the planner's own pull first (creatures 127229/127235).
constexpr float OTHER_TRASH_RADIUS = 35.0f;
// Creature::GetAggroRange: 20 yd detection - level difference (74 vs 72: 22 yd); a margin on top.
constexpr float AGGRO_DANGER = 26.0f;
// A member this close to an untouched pack walks back to the rally.
constexpr float DRIFT_RADIUS = 24.0f;
// Where a bot ends up when it walks to its target: ranged stop at about their range, melee at the target.
constexpr float RANGED_STAND = 28.0f;
constexpr float MELEE_STAND = 3.0f;
// An area spell this close to an untouched pack may hit it.
constexpr float AOE_SAFE = 30.0f;
// The next Watcher comes 10 s after a pack died (5 s + 5 s); with none in 20 s the tank pulls the next pack itself.
constexpr uint32 SEND_GAP_MS = 20000;
// Before the first pack the group tops up; after this long the tank pulls with whoever stands gathered.
constexpr uint32 READY_WAIT_MS = 60000;
constexpr float READY_HEALTH_PCT = 85.0f;
constexpr float READY_HEALER_MANA_PCT = 80.0f;
constexpr float READY_TANK_MANA_PCT = 50.0f;
constexpr uint32 CACHE_MS = 500;

struct Snapshot
{
    uint32 ms = 0;
    bool active = false;      // the bot is in the room, Krik'thir alive and idle, a Watcher alive
    bool fighting = false;    // a pack (or the bot's group) is in combat
    bool otherTrash = false;  // trash outside the formations stands idle near the rally
    bool packFighting = false;  // [b34_an2] a Watcher's pack is in combat (the phase's clock runs)
    uint8 watchersAlive = 0;
    uint32 lastFightMs = 0;   // the last time `fighting` was seen
    ObjectGuid pullTarget;    // the member of the untouched pack nearest to the rally
    std::vector<Position> idle;  // every alive member of the untouched packs
};

// The room's state seen by this bot (cached for CACHE_MS per bot).
Snapshot const& Get(PlayerbotAI* botAI);
// The room tactic decides the group's next pull: nothing else may start one (the careful planner's hook).
bool OwnsPull(PlayerbotAI* botAI);
// A pack has died and Krik'thir is about to send the next one: the group waits at the rally.
bool WaitsForSend(PlayerbotAI* botAI);
// Walking to `target` to fight it would bring the bot into an untouched pack's reach.
bool ApproachUnsafe(PlayerbotAI* botAI, Unit* target);
// An untouched pack stands within `radius` of the spot.
bool IdleNear(Snapshot const& snapshot, float x, float y, float radius);
// The bot opens the room's pulls: the main tank, or a tank when the group has no main tank alive.
bool IsPullingTank(Player* bot);
// [b34_an2] The group holds the rally: the room tactic owns the room (OwnsPull without the fight), or the Watcher
// phase runs (a pack fights, or a pack has died).
bool HoldsRally(PlayerbotAI* botAI);
}  // namespace ANWatcherRoom

class KrikthirWebWrapTrigger : public Trigger
{
public:
    KrikthirWebWrapTrigger(PlayerbotAI* botAI) : Trigger(botAI, "krik'thir web wrap") {}
    bool IsActive() override;
};

class KrikthirWatchersTrigger : public Trigger
{
public:
    KrikthirWatchersTrigger(PlayerbotAI* botAI) : Trigger(botAI, "krik'thir watchers") {}
    bool IsActive() override;
};

class AnubarakImpaleTrigger : public Trigger
{
public:
    AnubarakImpaleTrigger(PlayerbotAI* botAI) : Trigger(botAI, "anub'arak impale") {}
    bool IsActive() override;
};

class HadronoxPlatformTrigger : public Trigger
{
public:
    HadronoxPlatformTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hadronox platform") {}
    bool IsActive() override;
};

class AzjolNerubLowerKingdomTrigger : public Trigger
{
public:
    AzjolNerubLowerKingdomTrigger(PlayerbotAI* botAI) : Trigger(botAI, "azjol-nerub lower kingdom") {}
    bool IsActive() override;
};

class AnubarakPoundTrigger : public Trigger
{
public:
    AnubarakPoundTrigger(PlayerbotAI* botAI) : Trigger(botAI, "anub'arak pound") {}
    bool IsActive() override;
};

// A non-tank in front of Anub'arak, where Pound (a frontal cone) lands: to his side before it comes.
class AnubarakFrontalTrigger : public Trigger
{
public:
    AnubarakFrontalTrigger(PlayerbotAI* botAI) : Trigger(botAI, "anub'arak frontal") {}
    bool IsActive() override;
};

// The tank out of combat in Krik'thir's room with an untouched Watcher pack and no fight or send pending.
class KrikthirWatcherPullTrigger : public Trigger
{
public:
    KrikthirWatcherPullTrigger(PlayerbotAI* botAI) : Trigger(botAI, "krik'thir watcher pull") {}
    bool IsActive() override;
};

// [b34_an2] Out of combat while the group holds the rally (ANWatcherRoom::HoldsRally): a member farther than
// RALLY_HOLD_RADIUS from it (not the tank on its own pull's walk). Every member evaluating it notes the rally as the
// group's event hold.
class KrikthirWatcherRallyTrigger : public Trigger
{
public:
    KrikthirWatcherRallyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "krik'thir watcher rally") {}
    bool IsActive() override;
};

// A member stands in an untouched pack's reach (chasing, following, looting).
class KrikthirWatcherDriftTrigger : public Trigger
{
public:
    KrikthirWatcherDriftTrigger(PlayerbotAI* botAI) : Trigger(botAI, "krik'thir watcher drift") {}
    bool IsActive() override;
};

#endif
