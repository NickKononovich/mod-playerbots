/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TERRAINHAZARD_H
#define PLAYERBOTS_TERRAINHAZARD_H

#include "Define.h"
#include "G3D/Vector3.h"
#include "ObjectGuid.h"

#include <vector>

class GameObject;
class Map;
class Player;
class Unit;

// User, Ragefire Chasm 2026-10-24: «Стоят просто на краю лавы. Зачем? … Столько места там. Стойте где-нибудь еще.»
// The whole group fought one step from the lava. Terrain that hurts or kills whoever stands or is knocked there: lava
// or slime (MAP_LIQUID_TYPE_MAGMA / SLIME) whose surface is near the spot's height, and a drop-off (the ground falls
// more than TERRAIN_HAZARD_DROP within TERRAIN_HAZARD_RING_STEP). One shared test for every positioning code path.
//
// Cost: a ring of TERRAIN_HAZARD_DIRECTIONS samples every TERRAIN_HAZARD_RING_STEP yd up to the search radius (one
// liquid query and one height query per sample, a LoS query only for a sample that hits), innermost ring first, done
// at the first ring that hits. Results are cached per map thread (thread_local) by map, phase and the spot rounded to
// TERRAIN_HAZARD_CACHE_GRID yd for TERRAIN_HAZARD_CACHE_MS: terrain does not change, so repeated asks are a lookup.

constexpr float TERRAIN_HAZARD_NONE = 1000.0f;      // HazardDistance: no hazard within the search radius
constexpr float TERRAIN_HAZARD_RING_STEP = 2.0f;    // the rings are 2, 4, 6 ... yd round the spot
constexpr float TERRAIN_HAZARD_FIGHT_RADIUS = 6.0f;  // a fight this close to a hazard is moved off it
constexpr float TERRAIN_HAZARD_DROP = 4.0f;         // the ground this much lower one ring further out is a drop-off

enum class TerrainHazardKind : uint8
{
    None = 0,
    Liquid,   // lava or slime (or the liquids asked for)
    DropOff,  // the ground falls away (a ledge, a pit, a void)
};

struct TerrainHazardProbe
{
    float distance = TERRAIN_HAZARD_NONE;  // the nearest ring that hit (0: the spot itself)
    TerrainHazardKind kind = TerrainHazardKind::None;
    float hazardX = 0.0f;  // the nearest sample that hit
    float hazardY = 0.0f;
    float awayX = 0.0f;  // unit 2d direction away from the hazard samples of the nearest ring that hit (0, 0: none)
    float awayY = 0.0f;

    bool Found() const { return kind != TerrainHazardKind::None; }
};

// "lava/slime", "drop-off" or "none", for log lines.
char const* HazardKindName(TerrainHazardKind kind);

// The liquid flags that count as a hazard by default. Deep water is no hazard by itself (bots swim); a caller that
// needs it passes MAP_LIQUID_TYPE_WATER / OCEAN in `liquidMask`.
uint8 DefaultHazardLiquids();

// The distance (yd, rounded to the ring) from (x, y, z) to the nearest hazard within `searchRadius`, or
// TERRAIN_HAZARD_NONE. `z` is the height of whoever stands there (a bot's or a mob's position z).
float HazardDistance(Map* map, uint32 phaseMask, float x, float y, float z, float searchRadius);

// As HazardDistance, with what was found and the way away from it. `liquidMask` 0: DefaultHazardLiquids();
// `dropOffs` false: liquids only.
TerrainHazardProbe ProbeHazard(Map* map, uint32 phaseMask, float x, float y, float z, float searchRadius,
                               uint8 liquidMask = 0, bool dropOffs = true);

// A hazard within `radius` of the spot.
bool NearHazard(Map* map, uint32 phaseMask, float x, float y, float z, float radius);

// Liquid only (no drop-off test): lava or slime within `radius` of the spot. For spot pickers that test the footing
// themselves (RangedPositionActions' FootingRejection).
bool NearHarmfulLiquid(Map* map, uint32 phaseMask, float x, float y, float z, float radius);

// User, Utgarde Keep heroic 2026-10-24: «Как они через огонь прошли? Надо идти через правую сторону.» and «Даже с
// правами ГМ не могу пройти через эту границу». The forge room's walls of flame (Doodad_VR_ForgeFire_First/Second/
// Third, 186692/186693/186691) are GAMEOBJECT_TYPE_DOOR spawned GO_STATE_READY: their collision is on and a player
// (a GM too) cannot pass until instance_utgarde_keep opens each with its Forge Master's pull. The navmesh is static
// and runs through every door, and a bot's spline ignores collision: the group walked through the closed walls.
// A closed door (GAMEOBJECT_TYPE_DOOR in GO_STATE_READY, collision model enabled) is a barrier for a walk.
//
// Cost: the map's door GameObjects are listed once per HAZARD_DOOR_LIST_MS per map thread (thread_local); a query
// resolves those GUIDs, keeps the closed ones whose bounds come within the search radius of the walk, and only then
// casts one ray per path segment that overlaps a door's bounds, against that door's own model.
constexpr float CLOSED_DOOR_SEARCH = 40.0f;  // doors this far from the walk's start or end are not looked at

struct ClosedDoorHit
{
    GameObject* door = nullptr;
    float walked = 0.0f;  // the walk along the path up to the door

    bool Found() const { return door != nullptr; }
};

// Any closed door whose bounds come within `radius` (2d) of the segment from (fromX, fromY) to (toX, toY): the cheap
// gate before a path is built for PathThroughClosedDoor.
bool ClosedDoorNear(Unit* mover, float fromX, float fromY, float toX, float toY, float radius);
// [sfk241] Any closed door on the mover's map: the gate before a path is built for PathThroughClosedDoor where the
// walk's path is not known yet (a path may run far off the straight line, round a door ClosedDoorNear never sees).
bool AnyClosedDoor(Unit* mover);
// [sfk241] logs once per mover and door a closed door the walk's path meets that ClosedDoorNear (the straight line
// to `to` within `lineRadius`) did not see
void NoteDoorOffLine(Unit* mover, GameObject const* door, G3D::Vector3 const& to, float lineRadius, char const* walk);
// [lead93] the map's exit areatriggers (AreaTrigger.dbc entries on `mapId` whose areatrigger_teleport leads to another
// map): a point within one (its sphere radius or its box, plus `margin`) ports whoever stands there out of the
// instance. Returns the trigger's id, 0 when none. Cached per map (built once from the DBC)
uint32 InInstanceExit(uint32 mapId, float x, float y, float z, float margin);

// [b44_gerst] A door (GAMEOBJECT_TYPE_DOOR with a collision model, open or closed) whose centre stands within `radius`
// (2d) of (x, y) on that floor (DOOR_NEAR_DZ): no fight spot in a doorway (a boss dragged there fights in the gap
// with the next room behind it, or the door shuts on half the group).
constexpr float DOOR_NEAR_DZ = 8.0f;
bool DoorNear(Unit* who, float x, float y, float z, float radius);

// The first closed door `path` (from the mover's position) passes through: a ray along each segment at a walking
// body's height against the door's collision model. A hit within HAZARD_DOOR_INSIDE of the walk's start is ignored (the
// mover already stands in it: it walks out).
ClosedDoorHit PathThroughClosedDoor(Unit* mover, std::vector<G3D::Vector3> const& path);

// [lead1570] A door the leader holds shut while it stands open: the opening of an auto-closing key gate that wakes the
// pack behind it is that pack's pull on the use side - no member walk goes through it (the pack comes through: a
// creature's path ignores the gate) until the leader lets it go. Such a door counts closed for ClosedDoorNear and
// PathThroughClosedDoor (the walk against its model's bounds: an open door's model takes no ray). Any thread.
void HoldDoorShut(GameObject const* door, bool hold);
bool DoorHeldShut(GameObject const* door);

// [b41_gnomer] A closed "plug" door that seals a side tunnel beside a corridor (Gnomeregan's Cave Ins 146085 /
// 146086): a walk whose start and end both lie in front of it (the corridor side, the door's own facing) only clips
// the rubble's edge, it does not go through it. True: ignore this hit. Every other door: false.
bool DoorEdgeClipOnly(GameObject const* door, G3D::Vector3 const& from, G3D::Vector3 const& to);

// Doors a player opens by right-clicking them (2026-10-25). Scholomance normal/heroic ended "no reachable targets
// left": the Iron Gates (175611-175615, 175617, 175618) and the Door 175610 are closed type 0 DOORs with lockId 0
// and template flags 32 (NODESPAWN only) that instance_scholomance never opens - a player clicks them (GameObject::Use,
// case GAMEOBJECT_TYPE_DOOR -> UseDoorOrButton). The server's CMSG_GAMEOBJ_USE handler checks only the spawn state and
// the distance, never the lock (the client does not send it for a locked door), so DoorOpensByUse is what keeps
// locked and script doors shut. false restores the closed-door barrier for every door.
constexpr bool OPEN_DOORS_BY_USE = true;
constexpr float DOOR_OPEN_BACK = 1.0f;  // a walk an openable door stops ends this far before it (the opener's spot)

// A closed door whose opening is a player's click: GAMEOBJECT_TYPE_DOOR in GO_STATE_READY whose template neither
// starts open (a startOpen door that is closed now was closed by a script: Kirtonos' gate 175570, Gandling's gates)
// nor has a lock (lockId 0; the Viewing Room Door 175167, lock 1199, stays the key's job), no LOCKED / INTERACT_COND /
// NOT_SELECTABLE flag now, no own C++ script, no "Point" icon, no "Doodad_" wall (Utgarde Keep's forge fires
// 186691-186693, opened by instance_utgarde_keep on the Forge Master pull) and no entry of the script-door list.
bool DoorOpensByUse(GameObject const* door);

// The bot opens the door the way a player does (CMSG_GAMEOBJ_USE through its own session) when DoorOpensByUse holds,
// the bot is alive, within the door's interaction distance, neither it nor a group member on its map is in combat,
// and no encounter of the instance is in progress. Logs "[door] <bot>: opens <name> <entry>". true: the door is open
// now. A click that leaves the door closed marks it refused for a minute (per thread): DoorOpensByUse is false then.
bool OpenDoorByUse(Player* bot, GameObject* door);

// [ledge_drop] plan29 follow-up 3 (dtk-n 20261025-163758-62, 1284 s: the tank walked off the upper level after a Risen
// Drakkari Death Knight 18-34 yd below and landed on a navmesh island at (-292,-676,55) nothing joins; the mage fell
// after it, the run stalled). User 2026-10-25: «сокращения разрешаются, но они должны быть оправданы» - a drop is a
// justified shortcut only when (a) the landing spot is navmesh-joined to the rest (a ground path leads from it back to
// where the mover stands, no cliff to climb), (b) the fall is under LEDGE_FALL_SAFE yd (or every member near has a
// slow fall / hover), and (c) the group can follow the same way (the same tests hold for every member near).
// Otherwise the normal ground path, or no walk. A walk to a spot no more than LEDGE_DROP_DZ below is no drop.
// A ground path with a step steeper than 45 degrees and higher than LEDGE_DROP_DZ is a drop; a walk the navmesh
// cannot build (no path, a shortcut, incomplete) to a spot below is one too, of the whole height.
// User refinement (2026-10-25): an established player shortcut is allowed with some fall damage when it really saves
// time (the Underbog drop into Ghaz'an's lake). A landing in water deep enough to swim counts as a safe fall at any
// height ("water"); a drop of the curated list (LedgeShortcutDef, per map: the landing box) is safe when every
// member near keeps at least LEDGE_LISTED_HP_MARGIN of its health after the fall damage ("listed"). The landing must
// still be joined to the rest and the group able to follow.
constexpr float LEDGE_DROP_DZ = 5.0f;
constexpr float LEDGE_FALL_SAFE = 13.4f;  // Player::HandleFall: MIN_FALL_DMG_DIST 13.48, no damage below it
constexpr float LEDGE_LISTED_HP_MARGIN = 0.25f;

struct LedgeDropVerdict
{
    bool allowed = true;
    bool drop = false;      // the walk drops (a ledge, or a straight line down)
    float height = 0.0f;    // the drop's height
    bool connected = true;  // (a) the landing spot is joined to the rest
    bool safe = true;       // (b) + (c) the fall is safe for the mover and the members near
    bool water = false;     // the landing is in water deep enough to swim: no fall damage
    bool listed = false;    // a curated player shortcut (LedgeShortcutDef)
    // [ledge_drop_false] the landing is navmesh floor: a ground path's step, or with no ground path the polygon
    // nearest under the destination. A drop under LEDGE_FALL_SAFE onto navmesh floor is allowed with no way back up.
    bool onMesh = false;
    // [ledge_drop_false] the mover stands off the navmesh (a knockback's perch) and drops to floor joined to a member
    bool stranded = false;
    // [ledge_drop_false] allowed by the general guard: LEDGE_REFUSALS_FALLBACK refusals of one move, a survivable
    // fall onto navmesh (JudgeLedgeDrop logs "[ledge drop fallback]")
    bool fallback = false;
    // [b33_sp] the landing JudgeLedgeDrop measured (a ground path's step, the navmesh / water under the destination)
    // and whether a ground path leads there (false: the own walk ends at the brink, MovementAction::MoveTo jumps)
    float landX = 0.0f;
    float landY = 0.0f;
    float landZ = 0.0f;
    bool groundPath = false;
    bool burns = false;  // [lead223] the landing is in lava or slime: never a drop
    // [nav221] the landing is under a liquid surface the mover cannot reach from it: the drop drowns it, so the
    // dungeon island landing [nav24] allows as "harmless" is not harmless and is refused
    bool noSurface = false;
};

// Judged per mover and destination (cached LEDGE_VERDICT_MS per map thread for a destination within 2 yd). A taken
// drop logs "[ledge drop] <mover>: shortcut_drop ..." and pushes the move event "shortcut_drop"; a refused one
// "[ledge drop] <mover>: refused ..." (throttled) and "ledge_drop_refused". `what` names the walk.
LedgeDropVerdict JudgeLedgeDrop(Unit* mover, float x, float y, float z, char const* what);

// [lead145] Map::HasEnoughWater for water and ocean only: it counts every liquid (MAP_ALL_LIQUIDS), so a fall into
// lava or slime was a "water landing, safe" (rfc-h b165: "drop 6.0 yd (water) ... fall safe" into Ragefire's magma)
bool HasSwimWater(Unit* mover, float x, float y, float z);

// [lead223] (x, y, z) stands in (or on) lava or slime (DefaultHazardLiquids): a landing there burns whoever drops
bool InHarmfulLiquid(Unit* mover, float x, float y, float z);

// [ledge_drop_link] A harness route link the mover is crossing (RouteLinkDef: Immol'thar's pit step, the Shattered
// Halls sewer drop) is a deliberate step down: while allowed (renewed by the harness each tick of the crossing,
// for `forMs`) JudgeLedgeDrop lets every drop of the mover pass ("shortcut_drop ... (link)"). Thread-safe.
void AllowLedgeDrop(ObjectGuid mover, uint32 forMs);

// [ledge_drop_false] A ground spot under the walkable floor: the static collision (vmap) has a floor within
// UNDER_FLOOR_PROBE_UP above (x, y, z), more than UNDER_FLOOR_DZ over it. CheckCollisionAndGetValidCoords takes its
// end height from the navmesh polygon and probes the ground from there (collision height up), so a navmesh layer
// under a floor model gives a spot under that floor: bot-n 20261025-184229-56, the tank's "move to group member
// attackers" step to (-10.9,286.2,-2.9) where the floor is about -0.4; it fell through to -87 and died. Map thread.
constexpr float UNDER_FLOOR_PROBE_UP = 3.0f;
constexpr float UNDER_FLOOR_DZ = 1.0f;
bool UnderWalkableFloor(Unit* mover, float x, float y, float z);

// [b30_oculus] A charge-type gap closer (Charge, Intercept, Feral Charge) runs the core's path to the ground under
// its target: onto a target in the air (no ground support under it) it carries the caster down to that ground, and
// onto one below it is a drop. occ-h 20261025-184229-26 (430 s): the tank on Ring 1's island (z 436) charged an Azure
// Ring Guardian flying 30 yd away; the charge's spline ended at (1194,1054,107), the instance floor 329 yd below,
// the group stood above for 90 s. True: the gap closer must not be cast (a walk / ranged pull instead).
constexpr float GAP_CLOSER_AIR_HEIGHT = 6.0f;  // a target this far over its ground is in the air
bool GapCloserLeavesGround(Unit* caster, Unit* target, char const* what);

// [b35_z0] The map grid's fallback height. A WMO-only instance tile (Violet Hold, Azjol-Nerub, Gundrak, Scarlet
// Monastery ...: the .map header MAP_HEIGHT_NO_HEIGHT, flat gridHeight 0.0) has no terrain; Map::GetHeight returns
// that 0.0 whenever the vmap ray (from z + Z_OFFSET_FIND_HEIGHT down) finds no floor - a seed z a little under the
// WMO floor, a point past a wall or over a pit. UpdateAllowedPositionZ then clamps z DOWN to it (z > max_z), and so
// does PathGenerator::NormalizePath on every path point: 210 ledge_drop_refused of 20261026 ("vh rally",
// "anub'arak flank", "reach melee", "reach spell", "cast auto cc", "test runner move", "aggressive target") walked
// to z 0.0 from 6-224 yd above it. True: z is that fallback (or invalid), not a floor.
bool IsGridFallbackZ(Unit* mover, float x, float y, float z);

// [b35_z0] The height of the navmesh polygon nearest to (x, y, z): Detour findNearestPoly within
// DEST_Z_SNAP_HALF_WIDTH across and halfHeight up or down, the polygon's height at (x, y) (its nearest point off it).
// One query, no node pool. Map thread.
constexpr float DEST_Z_SNAP_HALF_WIDTH = 2.0f;
constexpr float DEST_Z_SNAP_HALF_HEIGHT = 4.0f;
bool NavmeshFloorZ(Unit* mover, float x, float y, float z, float halfHeight, float& floorZ);

// [b35_z0] A ground floor for a destination built from a seed height (a fixed spot's z, the boss's z): the navmesh
// polygon within DEST_Z_SNAP_HALF_HEIGHT of the seed, else the collision floor (GetMapHeight) when it is a real one
// (not IsGridFallbackZ) within LEDGE_DROP_DZ of the seed. False: no floor there (z unchanged) - the spot is rejected.
bool SafeFloorZ(Unit* mover, float x, float y, float& z);

// [b35_z0] The central guard (MovementAction::MoveTo): a destination z that is the grid fallback (IsGridFallbackZ),
// more than LEDGE_DROP_DZ from the mover, with no navmesh near it, snaps to the navmesh polygon at (x, y) within
// LEDGE_DROP_DZ of the mover's own height ("[b35_z0] [dest z]" log, throttled). True: z was snapped. Without such a
// polygon z stays and the ledge guard judges (refuses) it as before.
bool SnapGridFallbackZ(Unit* mover, float x, float y, float& z, char const* what);

#endif
