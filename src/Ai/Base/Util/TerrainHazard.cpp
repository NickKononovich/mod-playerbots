/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TerrainHazard.h"
#include "BotPath.h"  // [lead109]

#include "Optional.h"
#include "BotMoveEvents.h"
#include "PathGenerator.h"
#include "SafeZone.h"  // [lead9] GapCloserLeavesZone
#include <array>
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GridTerrainData.h"
#include "Group.h"
#include "InstanceGuid.h"  // [rot44]
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "ObjectDefines.h"
#include "Opcodes.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Unit.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "ObjectMgr.h"  // [lead93] the exit areatriggers
#include "G3D/AABox.h"
#include "G3D/Ray.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr int TERRAIN_HAZARD_DIRECTIONS = 8;   // samples per ring
    constexpr float HAZARD_QUERY_ABOVE = 2.0f;     // liquid and ground are searched down from this far above the spot
    constexpr float HAZARD_LIQUID_BELOW = 4.0f;    // a liquid surface this far below the spot still counts (deeper:
                                                   // the drop-off test's business)
    constexpr float HAZARD_GROUND_SEARCH = 20.0f;  // no ground within this far below a sample: a void (a drop-off)
    constexpr float HAZARD_EYE = 1.5f;             // a sample hidden behind a wall at this height is no hazard here
    constexpr float HAZARD_RING_MIN_EXTRA = 0.25f;  // a search radius past the last full ring adds a ring at it
    constexpr float HAZARD_AWAY_MIN = 0.1f;         // a hazard all round: no way away
    constexpr float TERRAIN_HAZARD_CACHE_GRID = 1.0f;   // spots within the same 1 yd cell share an answer
    constexpr uint32 TERRAIN_HAZARD_CACHE_MS = 60 * IN_MILLISECONDS;  // terrain is static
    constexpr size_t TERRAIN_HAZARD_CACHE_MAX = 4096;   // entries per map thread before the cache is emptied
    constexpr float HAZARD_TWO_PI = 6.2831853f;

    struct HazardKey
    {
        uint32 mapId = 0;
        uint32 phaseMask = 0;
        int32 cellX = 0;
        int32 cellY = 0;
        int32 cellZ = 0;
        int32 radiusQuarters = 0;
        uint8 liquidMask = 0;
        bool dropOffs = false;

        bool operator==(HazardKey const& other) const
        {
            return mapId == other.mapId && phaseMask == other.phaseMask && cellX == other.cellX &&
                   cellY == other.cellY && cellZ == other.cellZ && radiusQuarters == other.radiusQuarters &&
                   liquidMask == other.liquidMask && dropOffs == other.dropOffs;
        }
    };

    struct HazardKeyHash
    {
        size_t operator()(HazardKey const& key) const
        {
            uint64 hash = 1469598103934665603ULL;
            auto mix = [&hash](uint64 value)
            {
                hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
            };
            mix(key.mapId);
            mix(key.phaseMask);
            mix(uint32(key.cellX));
            mix(uint32(key.cellY));
            mix(uint32(key.cellZ));
            mix(uint32(key.radiusQuarters));
            mix(key.liquidMask | (key.dropOffs ? 0x100u : 0u));
            return size_t(hash);
        }
    };

    struct HazardCacheEntry
    {
        TerrainHazardProbe probe;
        uint32 ms = 0;
    };

    // Bot AI runs on map threads in parallel: one cache per thread, no lock.
    thread_local std::unordered_map<HazardKey, HazardCacheEntry, HazardKeyHash> hazardCache;

    int32 HazardCell(float value) { return int32(std::floor(value / TERRAIN_HAZARD_CACHE_GRID)); }

    bool HarmfulLiquidAt(Map* map, uint32 phaseMask, float x, float y, float z, uint8 liquidMask)
    {
        LiquidData const liquid =
            map->GetLiquidData(phaseMask, x, y, z + HAZARD_QUERY_ABOVE, DEFAULT_COLLISION_HEIGHT, liquidMask);
        return (liquid.Flags & liquidMask) && liquid.Level > INVALID_HEIGHT && liquid.Level > z - HAZARD_LIQUID_BELOW;
    }

    bool SeesSample(Map* map, uint32 phaseMask, float x, float y, float z, float sx, float sy, float sz)
    {
        return map->isInLineOfSight(x, y, z + HAZARD_EYE, sx, sy, sz + HAZARD_EYE, phaseMask, LINEOFSIGHT_ALL_CHECKS,
                                    VMAP::ModelIgnoreFlags::Nothing);
    }

    TerrainHazardProbe Probe(Map* map, uint32 phaseMask, float x, float y, float z, float searchRadius,
                             uint8 liquidMask, bool dropOffs)
    {
        TerrainHazardProbe probe;
        if (HarmfulLiquidAt(map, phaseMask, x, y, z, liquidMask))
        {
            probe.distance = 0.0f;
            probe.kind = TerrainHazardKind::Liquid;
            probe.hazardX = x;
            probe.hazardY = y;
            return probe;
        }

        float rings[8];
        int ringCount = 0;
        for (float ring = TERRAIN_HAZARD_RING_STEP; ring <= searchRadius + 0.01f && ringCount < 7;
             ring += TERRAIN_HAZARD_RING_STEP)
            rings[ringCount++] = ring;

        float const lastRing = ringCount ? rings[ringCount - 1] : 0.0f;
        if (searchRadius > lastRing + HAZARD_RING_MIN_EXTRA)
            rings[ringCount++] = searchRadius;

        // Per direction: the ground of the ring before (the spot's own ground first), and whether a wall or a hit
        // already ended the walk out that way.
        float centerGround = map->GetHeight(phaseMask, x, y, z + HAZARD_QUERY_ABOVE, true, HAZARD_GROUND_SEARCH);
        if (centerGround <= INVALID_HEIGHT)
            centerGround = z;

        float previousGround[TERRAIN_HAZARD_DIRECTIONS];
        bool ended[TERRAIN_HAZARD_DIRECTIONS];
        for (int i = 0; i < TERRAIN_HAZARD_DIRECTIONS; ++i)
        {
            previousGround[i] = centerGround;
            ended[i] = false;
        }

        for (int r = 0; r < ringCount; ++r)
        {
            float const ring = rings[r];
            float awayX = 0.0f;
            float awayY = 0.0f;
            for (int i = 0; i < TERRAIN_HAZARD_DIRECTIONS; ++i)
            {
                if (ended[i])
                    continue;

                // Every other ring turned by half a step: the gaps between the directions get covered.
                float const angle = HAZARD_TWO_PI * (float(i) + (r % 2 ? 0.5f : 0.0f)) / TERRAIN_HAZARD_DIRECTIONS;
                float const dirX = std::cos(angle);
                float const dirY = std::sin(angle);
                float const sx = x + dirX * ring;
                float const sy = y + dirY * ring;

                TerrainHazardKind kind = TerrainHazardKind::None;
                float sampleGround = previousGround[i];
                if (HarmfulLiquidAt(map, phaseMask, sx, sy, z, liquidMask))
                    kind = TerrainHazardKind::Liquid;
                else if (dropOffs)
                {
                    float const from = std::max(previousGround[i], centerGround) + HAZARD_QUERY_ABOVE;
                    float const ground = map->GetHeight(phaseMask, sx, sy, from, true, HAZARD_GROUND_SEARCH);
                    if (ground <= INVALID_HEIGHT || previousGround[i] - ground > TERRAIN_HAZARD_DROP)
                        kind = TerrainHazardKind::DropOff;
                    else
                        sampleGround = ground;
                }

                if (kind == TerrainHazardKind::None)
                {
                    previousGround[i] = sampleGround;
                    continue;
                }

                // Lava or a pit behind a wall is not where the spot's occupant ends up: that way ends here.
                if (!SeesSample(map, phaseMask, x, y, centerGround, sx, sy, previousGround[i]))
                {
                    ended[i] = true;
                    continue;
                }

                if (!probe.Found())
                {
                    probe.distance = ring;
                    probe.kind = kind;
                    probe.hazardX = sx;
                    probe.hazardY = sy;
                }

                awayX -= dirX;
                awayY -= dirY;
                ended[i] = true;
            }

            if (probe.Found())
            {
                float const length = std::sqrt(awayX * awayX + awayY * awayY);
                if (length >= HAZARD_AWAY_MIN)
                {
                    probe.awayX = awayX / length;
                    probe.awayY = awayY / length;
                }

                return probe;
            }
        }

        return probe;
    }
}

uint8 DefaultHazardLiquids() { return MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME; }

char const* HazardKindName(TerrainHazardKind kind)
{
    switch (kind)
    {
        case TerrainHazardKind::Liquid:
            return "lava/slime";
        case TerrainHazardKind::DropOff:
            return "drop-off";
        default:
            return "none";
    }
}

TerrainHazardProbe ProbeHazard(Map* map, uint32 phaseMask, float x, float y, float z, float searchRadius,
                               uint8 liquidMask, bool dropOffs)
{
    if (!map || searchRadius < 0.0f)
        return TerrainHazardProbe();

    if (!liquidMask)
        liquidMask = DefaultHazardLiquids();

    HazardKey key;
    key.mapId = map->GetId();
    key.phaseMask = phaseMask;
    key.cellX = HazardCell(x);
    key.cellY = HazardCell(y);
    key.cellZ = HazardCell(z);
    key.radiusQuarters = int32(std::lround(searchRadius * 4.0f));
    key.liquidMask = liquidMask;
    key.dropOffs = dropOffs;

    uint32 const now = getMSTime();
    auto const found = hazardCache.find(key);
    if (found != hazardCache.end() && getMSTimeDiff(found->second.ms, now) < TERRAIN_HAZARD_CACHE_MS)
        return found->second.probe;

    if (hazardCache.size() >= TERRAIN_HAZARD_CACHE_MAX)
        hazardCache.clear();

    // The cell's centre answers for the whole cell, so every spot in it gets the same answer.
    float const cellX = (float(key.cellX) + 0.5f) * TERRAIN_HAZARD_CACHE_GRID;
    float const cellY = (float(key.cellY) + 0.5f) * TERRAIN_HAZARD_CACHE_GRID;
    TerrainHazardProbe const probe = Probe(map, phaseMask, cellX, cellY, z, searchRadius, liquidMask, dropOffs);
    hazardCache[key] = {probe, now};
    return probe;
}

float HazardDistance(Map* map, uint32 phaseMask, float x, float y, float z, float searchRadius)
{
    return ProbeHazard(map, phaseMask, x, y, z, searchRadius).distance;
}

bool NearHazard(Map* map, uint32 phaseMask, float x, float y, float z, float radius)
{
    return HazardDistance(map, phaseMask, x, y, z, radius) <= radius;
}

bool NearHarmfulLiquid(Map* map, uint32 phaseMask, float x, float y, float z, float radius)
{
    return ProbeHazard(map, phaseMask, x, y, z, radius, 0, false).distance <= radius;
}

namespace
{
    constexpr uint32 HAZARD_DOOR_LIST_MS = 30 * IN_MILLISECONDS;  // doors spawn with the map; a respawned one waits
    constexpr size_t HAZARD_DOOR_LIST_MAX = 64;                      // maps per map thread before the cache is emptied
    constexpr float HAZARD_DOOR_RAY_HEIGHT = 1.0f;  // a segment is tested this far above the path (a walking body)
    constexpr float HAZARD_DOOR_MARGIN = 2.0f;
    constexpr float HAZARD_DOOR_INSIDE = 0.75f;  // a hit this close to the walk's start: the mover stands in the door

    struct DoorList
    {
        Map const* map = nullptr;
        uint32 ms = 0;
        std::vector<ObjectGuid> doors;  // every door GameObject with a collision model, open or closed
    };

    // Bot AI runs on map threads in parallel: one list per map and thread, no lock. GUIDs, never pointers, across
    // ticks: a door is resolved by the map at every query.
    thread_local std::unordered_map<uint64, DoorList> doorLists;

    std::vector<ObjectGuid> const& MapDoors(Map* map)
    {
        uint64 const key = (uint64(map->GetId()) << 32) | map->GetInstanceId();
        uint32 const now = getMSTime();
        auto found = doorLists.find(key);
        if (found != doorLists.end() && found->second.map == map &&
            getMSTimeDiff(found->second.ms, now) < HAZARD_DOOR_LIST_MS)
            return found->second.doors;

        if (found == doorLists.end() && doorLists.size() >= HAZARD_DOOR_LIST_MAX)
            doorLists.clear();

        DoorList& list = doorLists[key];
        list.map = map;
        list.ms = now;
        list.doors.clear();
        for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
            if (go && go->GetGoType() == GAMEOBJECT_TYPE_DOOR && go->m_model)
                list.doors.push_back(go->GetGUID());

        return list.doors;
    }

    // The closed doors whose bounds come within `radius` (2d) of the segment.
    void CollectClosedDoors(Unit* mover, float fromX, float fromY, float toX, float toY, float radius,
                            std::vector<GameObject*>& closed)
    {
        Map* map = mover->GetMap();
        if (!map)
            return;

        for (ObjectGuid const& guid : MapDoors(map))
        {
            GameObject* go = map->GetGameObject(guid);
            if (!go || !go->IsInWorld() || !go->isSpawned() || go->GetGoState() != GO_STATE_READY || !go->m_model ||
                !go->m_model->isEnabled() || !mover->InSamePhase(go))
                continue;

            // The segment against the door's bounds grown by the radius (2d slabs).
            G3D::AABox const& bounds = go->m_model->GetBounds();
            float const lowX = bounds.low().x - radius;
            float const lowY = bounds.low().y - radius;
            float const highX = bounds.high().x + radius;
            float const highY = bounds.high().y + radius;
            float enter = 0.0f;
            float leave = 1.0f;
            bool overlaps = true;
            float const deltas[2] = {toX - fromX, toY - fromY};
            float const starts[2] = {fromX, fromY};
            float const lows[2] = {lowX, lowY};
            float const highs[2] = {highX, highY};
            for (int axis = 0; axis < 2 && overlaps; ++axis)
            {
                if (std::fabs(deltas[axis]) < 0.0001f)
                {
                    overlaps = starts[axis] >= lows[axis] && starts[axis] <= highs[axis];
                    continue;
                }

                float first = (lows[axis] - starts[axis]) / deltas[axis];
                float second = (highs[axis] - starts[axis]) / deltas[axis];
                if (first > second)
                    std::swap(first, second);

                enter = std::max(enter, first);
                leave = std::min(leave, second);
                overlaps = enter <= leave;
            }

            if (overlaps)
                closed.push_back(go);
        }
    }
}

uint32 InInstanceExit(uint32 mapId, float x, float y, float z, float margin)
{
    static std::mutex lock;
    static std::unordered_map<uint32, std::vector<AreaTrigger const*>> exits;
    std::vector<AreaTrigger const*> const* list = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto [itr, fresh] = exits.try_emplace(mapId);
        if (fresh)
        {
            constexpr uint32 AREATRIGGER_MAX_ID = 8000;  // AreaTrigger.dbc ids of 3.3.5 stay below it
            for (uint32 id = 1; id < AREATRIGGER_MAX_ID; ++id)
                if (AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(id); trigger && trigger->map == mapId)
                    if (AreaTriggerTeleport const* teleport = sObjectMgr->GetAreaTriggerTeleport(id);
                        teleport && teleport->target_mapId != mapId)
                        itr->second.push_back(trigger);
        }
        list = &itr->second;
    }
    for (AreaTrigger const* trigger : *list)
    {
        if (trigger->radius > 0.0f)
        {
            float const dx = x - trigger->x;
            float const dy = y - trigger->y;
            float const dz = z - trigger->z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) <= trigger->radius + margin)
                return trigger->entry;
            continue;
        }
        // a box: the point in the box's own frame (its orientation)
        float const c = std::cos(-trigger->orientation);
        float const s = std::sin(-trigger->orientation);
        float const dx = x - trigger->x;
        float const dy = y - trigger->y;
        float const along = dx * c - dy * s;
        float const across = dx * s + dy * c;
        if (std::fabs(along) <= trigger->length / 2.0f + margin &&
            std::fabs(across) <= trigger->width / 2.0f + margin &&
            std::fabs(z - trigger->z) <= trigger->height / 2.0f + margin)
            return trigger->entry;
    }
    return 0;
}

bool ClosedDoorNear(Unit* mover, float fromX, float fromY, float toX, float toY, float radius)
{
    if (!mover)
        return false;

    std::vector<GameObject*> closed;
    CollectClosedDoors(mover, fromX, fromY, toX, toY, radius, closed);
    return !closed.empty();
}

// [b44_gerst]
bool DoorNear(Unit* who, float x, float y, float z, float radius)
{
    Map* map = who ? who->GetMap() : nullptr;
    if (!map)
        return false;

    for (ObjectGuid const& guid : MapDoors(map))
    {
        GameObject* go = map->GetGameObject(guid);
        if (!go || !go->IsInWorld() || !go->isSpawned() || !who->InSamePhase(go) ||
            std::fabs(go->GetPositionZ() - z) > DOOR_NEAR_DZ)
            continue;

        if (go->GetExactDist2d(x, y) <= radius)
            return true;
    }

    return false;
}

namespace
{
    // [b41_gnomer] gnomer-n 20261026-063902-5B-21: after Grubbis (escort_end 505 s, Emi sealed the Cave In at WP11)
    // the priest's follow north from (-523.2,-116.7) "ran through Cave In 146086 at 6.1 yd" and was cut to
    // (-531.5,-107.0), 10 yd east of the plug's centre (-541.6,-105.4); every later walk from there hit it at 2 yd
    // (the leader's door_hold, 1139 skip_target "route through a closed door", boss_unreachable on every boss), while
    // four members had just walked the same corridor north with it sealed. The plugs fill side tunnels: Emi's waves
    // spawn behind them (creature_summon_groups 7998: x -547..-558 behind 146086, x -490..-495 behind 146085) and face
    // the corridor the way the door does (o 0.77 / -2.78). A walk that stays in front of the door (both ends at least
    // DOOR_PLUG_FRONT_MIN along its facing) clips the rubble's edge; a walk into the tunnel still meets it.
    constexpr std::array<uint32, 2> DOOR_PLUG_ENTRIES = {146085, 146086};  // Gnomeregan: Cave In 1 / 2
    constexpr float DOOR_PLUG_FRONT_MIN = 2.0f;
}

bool DoorEdgeClipOnly(GameObject const* door, G3D::Vector3 const& from, G3D::Vector3 const& to)
{
    if (!door || std::find(DOOR_PLUG_ENTRIES.begin(), DOOR_PLUG_ENTRIES.end(), door->GetEntry()) ==
                     DOOR_PLUG_ENTRIES.end())
        return false;

    float const facing = door->GetOrientation();
    float const nx = std::cos(facing);
    float const ny = std::sin(facing);
    auto const front = [&](G3D::Vector3 const& point)
    {
        return (point.x - door->GetPositionX()) * nx + (point.y - door->GetPositionY()) * ny >= DOOR_PLUG_FRONT_MIN;
    };
    return front(from) && front(to);
}

ClosedDoorHit PathThroughClosedDoor(Unit* mover, std::vector<G3D::Vector3> const& path)
{
    ClosedDoorHit hit;
    if (!mover || path.size() < 2)
        return hit;

    std::vector<GameObject*> closed;
    CollectClosedDoors(mover, path.front().x, path.front().y, path.back().x, path.back().y,
                       CLOSED_DOOR_SEARCH, closed);
    if (closed.empty())
        return hit;

    uint32 const phaseMask = mover->GetPhaseMask();
    G3D::Vector3 const raise(0.0f, 0.0f, HAZARD_DOOR_RAY_HEIGHT);
    G3D::Vector3 const margin(HAZARD_DOOR_MARGIN, HAZARD_DOOR_MARGIN, HAZARD_DOOR_MARGIN);
    float walked = 0.0f;
    for (size_t index = 1; index < path.size(); ++index)
    {
        G3D::Vector3 const from = path[index - 1] + raise;
        G3D::Vector3 const to = path[index] + raise;
        G3D::Vector3 const segment = to - from;
        float const length = segment.length();
        if (length <= 0.0f)
            continue;

        G3D::Vector3 const low = from.min(to) - margin;
        G3D::Vector3 const high = from.max(to) + margin;
        G3D::Ray const ray(from, segment / length);
        float nearest = length;
        for (GameObject* door : closed)
        {
            G3D::AABox const& bounds = door->m_model->GetBounds();
            if (high.x < bounds.low().x || low.x > bounds.high().x || high.y < bounds.low().y ||
                low.y > bounds.high().y || high.z < bounds.low().z || low.z > bounds.high().z)
                continue;

            float distance = nearest;
            if (!door->m_model->intersectRay(ray, distance, true, phaseMask, VMAP::ModelIgnoreFlags::Nothing))
                continue;

            if (walked + distance < HAZARD_DOOR_INSIDE)
                continue;  // the mover stands in it: it walks out

            if (DoorEdgeClipOnly(door, path.front(), path.back()))
                continue;  // [b41_gnomer] the walk stays in front of a tunnel plug: an edge clip

            if (!hit.Found() || distance < nearest)
            {
                hit.door = door;
                nearest = distance;
            }
        }

        if (hit.Found())
        {
            hit.walked = walked + nearest;
            return hit;
        }

        walked += length;
    }

    return hit;
}

namespace
{
    // Doors the instance scripts drive that the template test would let through: the click would open them before
    // their event (DoorOpensByUse). The startOpen and Doodad_ tests already cover these; the list is the explicit
    // record and the place for the next one found.
    constexpr std::array<uint32, 24> SCRIPT_DOOR_ENTRIES = {
        175570,                                          // Scholomance: Kirtonos' gate (instance_scholomance)
        177371, 177372, 177373, 177374, 177375, 177376,  // Scholomance: Darkmaster Gandling's room gates
        177377,                                          // Scholomance: Gandling, upper east
        186691, 186692, 186693,                          // Utgarde Keep: the forge walls of flame (Forge Masters)
        // [hleft-door-chase] Blackrock Depths (instance_blackrock_depths.cpp): the Ring of Law gates GO_ARENA1-4
        // (opened by Grimstone's win), the Shadowforge Lock's dummy collision doors (smart_scripts 161460), the Tomb of
        // the Seven doors and the Magmus / throne doors. lockId 0 and flags 32 like a click door, but no player opens
        // them by a click: 2026-10-25 brd-h / brd-n reached Ribbly and Hurley through ArenaGate03 and 161516.
        161522, 161523, 161524, 161525,  // ArenaGate02 / 04 / 03 / 01
        161462, 161516,                  // Giant Door Fake Collision, BigDoorDummyCollision02
        170576, 170577,                  // Tomb of the Seven: enter / exit
        170573, 170574, 170575,          // Magmus doors north / south, the throne room door
        // [door_hold_pass] Gnomeregan: the Cave Ins (type 0, lock 0, flags 32) are opened and sealed by Emi
        // Shortfuse's SAI 7998 (ids 100 / 29 / 101 / 56); a click would open them out of the event's order.
        146085, 146086,
    };

    // Doors with a lock any player opens by the click (Lock.dbc 86: one LOCK_KEY_SKILL case LOCKTYPE_QUICK_OPEN,
    // Skill 0; GameObject::Use opens the door, the lock is never checked) that no script, SAI, lever or event opens -
    // the instance scripts only save their state (2026-10-25 dm-h / gnomer-h: every boss behind them "behind a closed
    // door"). Other lock-86 doors (Razorfen Downs pens, Karazhan) are event doors: not listed; the Uldaman Temple
    // Doors 124368 / 141869 are click doors (wave 7 ulda-h: 127 skips "route through a closed door"). Stratholme's
    // Scarlet Bastion doors use lock 1634, byte-identical to 86, and no script opens them (instance_stratholme:
    // AllowSaveToDB only; strat-h 2026-10-25: Willey and Malor "behind a closed door (The Bastion Door 175967)").
    constexpr std::array<uint32, 8> CLICK_LOCK_DOOR_ENTRIES = {
        17153, 17154,    // Deadmines: Heavy Doors (mine -> lumber mill, lumber mill -> foundry)
        142207,          // Gnomeregan: The Final Chamber (Thermaplugg)
        124368, 141869,  // Uldaman: Temple Doors to the Hall of the Keepers / Archaedas' chamber (no script opens them)
        175967, 175968, 176194,  // Stratholme: Bastion / Hoard / High Command doors, lock 1634 (= lock 86)
    };

    constexpr char const* DOOR_ICON_POINT = "Point";  // Player::GetGameObjectIfCanInteractWith: no interaction
    constexpr char const* DOOR_DOODAD_PREFIX = "Doodad_";
    constexpr uint32 DOOR_REFUSED_MS = 60 * IN_MILLISECONDS;  // a door a click did not open is no openable door
    constexpr size_t DOOR_REFUSED_KEEP = 64;                   // entries per thread before the memo is emptied

    // A door whose click left it closed (a gossip script took the use, a cooldown): per thread, by GUID, so the
    // opener does not click it every tick and the harness treats it as a closed door again.
    // [rot44] by (instance, GUID): one thread updates several instances of a dungeon and their doors share GUIDs
    thread_local std::map<InstanceGuid, uint32> refusedDoors;

    bool DoorRefused(GameObject const* door)
    {
        auto found = refusedDoors.find(InstanceKey(door));
        if (found == refusedDoors.end())
            return false;
        if (getMSTimeDiff(found->second, getMSTime()) < DOOR_REFUSED_MS)
            return true;
        refusedDoors.erase(found);
        return false;
    }
}

bool DoorOpensByUse(GameObject const* door)
{
    if (!OPEN_DOORS_BY_USE || !door || door->GetGoType() != GAMEOBJECT_TYPE_DOOR ||
        door->GetGoState() != GO_STATE_READY)
        return false;

    GameObjectTemplate const* info = door->GetGOInfo();
    if (!info || info->door.startOpen || info->ScriptId)
        return false;
    if (info->door.lockId && std::find(CLICK_LOCK_DOOR_ENTRIES.begin(), CLICK_LOCK_DOOR_ENTRIES.end(),
                                       door->GetEntry()) == CLICK_LOCK_DOOR_ENTRIES.end())
        return false;

    if (door->HasGameObjectFlag(GO_FLAG_LOCKED) || door->HasGameObjectFlag(GO_FLAG_INTERACT_COND) ||
        door->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
        return false;

    if (info->IconName == DOOR_ICON_POINT || info->name.rfind(DOOR_DOODAD_PREFIX, 0) == 0 ||
        DoorRefused(door))
        return false;

    return std::find(SCRIPT_DOOR_ENTRIES.begin(), SCRIPT_DOOR_ENTRIES.end(), door->GetEntry()) ==
           SCRIPT_DOOR_ENTRIES.end();
}

bool OpenDoorByUse(Player* bot, GameObject* door)
{
    if (!bot || !door || !DoorOpensByUse(door) || !bot->IsAlive() || bot->IsInCombat() || !bot->IsInWorld() ||
        bot->GetMap() != door->GetMap() || !door->isSpawned() || !bot->InSamePhase(door))
        return false;

    if (InstanceScript* script = bot->GetInstanceScript(); script && script->IsEncounterInProgress())
        return false;

    if (!door->IsWithinDistInMap(bot, door->GetInteractionDistance()))
        return false;

    // Never during a fight: nobody of the group on this map is in combat either.
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource(); member && member->GetMap() == bot->GetMap() && member->IsInCombat())
                return false;

    bot->SetFacingToObject(door);
    WorldPacket use(CMSG_GAMEOBJ_USE);
    use << door->GetGUID();
    bot->GetSession()->HandleGameObjectUseOpcode(use);

    bool const opened = door->GetGoState() != GO_STATE_READY;
    if (!opened)
    {
        if (refusedDoors.size() >= DOOR_REFUSED_KEEP)
            refusedDoors.clear();
        uint32 const now = getMSTime();
        refusedDoors[InstanceKey(door)] = now ? now : 1;
    }

    LOG_INFO("playerbots", "[door] {}: opens {} {}{}", bot->GetName(), door->GetName(), door->GetEntry(),
             opened ? "" : " (still closed)");
    return opened;
}

namespace
{
    constexpr uint32 LEDGE_VERDICT_MS = 2 * IN_MILLISECONDS;
    constexpr uint32 LEDGE_LOG_MS = 5 * IN_MILLISECONDS;
    constexpr float LEDGE_SAME_DEST = 2.0f;
    constexpr float LEDGE_GROUP_RANGE = 60.0f;
    constexpr size_t LEDGE_MEMO_MAX = 256;

    struct LedgeMemo
    {
        G3D::Vector3 dest;
        uint32 ms = 0;
        LedgeDropVerdict verdict;
        uint32 loggedMs = 0;
        // [ledge_drop_false] refused verdicts in a row for (about) the same destination
        G3D::Vector3 refusedDest;
        uint32 refusals = 0;
    };
    // [ledge_drop_false] this many refused verdicts (LEDGE_VERDICT_MS apart, ~10 s) of one move, within this distance
    constexpr uint32 LEDGE_REFUSALS_FALLBACK = 5;
    constexpr float LEDGE_REFUSED_SAME_DEST = 6.0f;
    constexpr float LEDGE_SWIM_DEPTH = 2.0f;  // a landing in water this deep takes no fall damage
    // a water landing counts as harmless up to this drop (sp-h's walkway into the water ~52 yd); the Oculus islands
    // (329 yd over the floor) stay refused whatever lies below
    constexpr float LEDGE_WATER_MAX_DROP = 60.0f;
    thread_local std::map<InstanceGuid, LedgeMemo> ledgeMemos;  // [rot44] a guardian mover's GUID is per map

    // [ledge_drop_link] movers crossing a harness route link, until when (getMSTime); world thread writes, map
    // threads read
    std::mutex ledgeAllowLock;
    std::unordered_map<uint64, uint32> ledgeAllowUntil;

    bool LedgeDropAllowed(ObjectGuid mover)
    {
        std::lock_guard<std::mutex> guard(ledgeAllowLock);
        auto const found = ledgeAllowUntil.find(mover.GetRawValue());
        return found != ledgeAllowUntil.end() && int32(found->second - getMSTime()) > 0;
    }

    bool PathIsGround(PathType type)
    {
        return (type & PATHFIND_NORMAL) &&
               !(type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE | PATHFIND_NOT_USING_PATH));
    }

    // The highest step of `points` that rises (up) or falls (!up) more than LEDGE_DROP_DZ, steeper than 45 degrees.
    float SteepStep(std::vector<G3D::Vector3> const& points, bool up, G3D::Vector3* at = nullptr)
    {
        float highest = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
        {
            float const dz = up ? points[i].z - points[i - 1].z : points[i - 1].z - points[i].z;
            float const flat = std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
            if (dz > LEDGE_DROP_DZ && dz > flat && dz > highest)
            {
                highest = dz;
                if (at)
                    *at = points[i];
            }
        }
        return highest;
    }

    // [ledge_drop] the curated player shortcuts: a drop whose landing lies in the box is "listed". The Underbog: the
    // jump from the walkway / ramp into Ghaz'an's lake (UnderbogGhazan's lake box, surface z 49.4, bottom z 3.5;
    // map 546), the players' way down to Ghaz'an.
    struct LedgeShortcutDef
    {
        uint32 mapId;
        float minX, maxX, minY, maxY, maxZ;
        char const* note;
    };
    constexpr std::array<LedgeShortcutDef, 1> LEDGE_SHORTCUTS = {{
        {546, 150.0f, 310.0f, -530.0f, -360.0f, 60.0f, "The Underbog: the drop into Ghaz'an's lake"},
    }};

    LedgeShortcutDef const* ListedShortcut(uint32 mapId, G3D::Vector3 const& landing)
    {
        for (LedgeShortcutDef const& def : LEDGE_SHORTCUTS)
            if (def.mapId == mapId && landing.x >= def.minX && landing.x <= def.maxX && landing.y >= def.minY &&
                landing.y <= def.maxY && landing.z <= def.maxZ)
                return &def;
        return nullptr;
    }

    // Player::HandleFall: FALL_DMG_EQU_SLOPE x (height - safe fall) + FALL_DMG_EQU_INTERCEPT of the maximum health.
    float FallDamageShare(Unit* unit, float height)
    {
        float const safeFall = float(unit->GetTotalAuraModifier(SPELL_AURA_SAFE_FALL));
        return std::max(0.0f, 0.018f * (height - safeFall) - 0.2426f);
    }

    // `water`: the landing is in swimming water (no fall damage); `listed`: a curated shortcut, safe while the
    // member keeps LEDGE_LISTED_HP_MARGIN of its health after the fall.
    bool SafeFallFor(Unit* unit, float height, bool water, bool listed)
    {
        if (water || height <= LEDGE_FALL_SAFE || unit->HasAuraType(SPELL_AURA_FEATHER_FALL) ||
            unit->HasAuraType(SPELL_AURA_HOVER) || FallDamageShare(unit, height) <= 0.0f)
            return true;
        return listed && unit->GetMaxHealth() &&
               float(unit->GetHealth()) / float(unit->GetMaxHealth()) - FallDamageShare(unit, height) >=
                   LEDGE_LISTED_HP_MARGIN;
    }

    // [ledge_drop_false] the navmesh floor under an unmeasured landing (a walk with no ground path): the nearest
    // ground polygon within LEDGE_SNAP_HALF_WIDTH across and LEDGE_SNAP_HALF_HEIGHT up or down. A mob standing just
    // off the mesh at the foot of a drop (an-h 20261025-184229-36: "reach melee" 9-11 yd down) snaps to the floor it
    // stands on; a body under the world (bot-n 20261025-184229-56, 85 yd under the floor) snaps to nothing.
    constexpr float LEDGE_SNAP_HALF_WIDTH = 3.0f;
    constexpr float LEDGE_SNAP_HALF_HEIGHT = 4.0f;

    bool NavmeshLanding(Unit* mover, G3D::Vector3 const& point, G3D::Vector3& snapped)
    {
        Map* map = mover->GetMap();
        dtNavMeshQuery const* query = map ? map->GetMapCollisionData().GetMMapData().GetNavMeshQuery() : nullptr;
        if (!query)
            return false;

        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
        filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));
        // Detour works in (y, z, x).
        float const center[3] = {point.y, point.z, point.x};
        float const extents[3] = {LEDGE_SNAP_HALF_WIDTH, LEDGE_SNAP_HALF_HEIGHT, LEDGE_SNAP_HALF_WIDTH};
        float nearest[3];
        dtPolyRef ref = 0;
        if (dtStatusFailed(query->findNearestPoly(center, extents, &filter, &ref, nearest)) || !ref)
            return false;

        snapped = G3D::Vector3(nearest[2], nearest[0], nearest[1]);
        return true;
    }
}

bool HasSwimWater(Unit* mover, float x, float y, float z)
{
    Map* map = mover ? mover->GetMap() : nullptr;
    if (!map)
        return false;
    LiquidData const liquid = map->GetLiquidData(mover->GetPhaseMask(), x, y, z, mover->GetCollisionHeight(),
                                                 uint8(MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN));
    return (liquid.Status & MAP_LIQUID_STATUS_SWIMMING) && liquid.Level > INVALID_HEIGHT &&
           liquid.Level > liquid.DepthLevel && liquid.Level - liquid.DepthLevel >= mover->GetMinHeightInWater();
}

bool InHarmfulLiquid(Unit* mover, float x, float y, float z)
{
    Map* map = mover ? mover->GetMap() : nullptr;
    if (!map)
        return false;
    LiquidData const liquid =
        map->GetLiquidData(mover->GetPhaseMask(), x, y, z, mover->GetCollisionHeight(), DefaultHazardLiquids());
    return (liquid.Status & MAP_LIQUID_STATUS_IN_CONTACT) && liquid.Level > INVALID_HEIGHT;
}

LedgeDropVerdict JudgeLedgeDrop(Unit* mover, float x, float y, float z, char const* what)
{
    LedgeDropVerdict verdict;
    Map* map = mover ? mover->GetMap() : nullptr;
    if (!map || !map->IsDungeon() || mover->IsFlying() || mover->IsInWater() || mover->GetVehicle() ||
        mover->GetTransport() || mover->GetPositionZ() - z <= LEDGE_DROP_DZ)
        return verdict;

    // a harness route link's crossing: its step down is the route
    if (LedgeDropAllowed(mover->GetGUID()))
        return verdict;

    uint32 const now = getMSTime();
    G3D::Vector3 const dest(x, y, z);
    if (ledgeMemos.size() >= LEDGE_MEMO_MAX)
        ledgeMemos.clear();
    LedgeMemo& memo = ledgeMemos[InstanceKey(mover)];
    if (memo.ms && getMSTimeDiff(memo.ms, now) < LEDGE_VERDICT_MS && (memo.dest - dest).length() <= LEDGE_SAME_DEST)
        return memo.verdict;

    G3D::Vector3 const here(mover->GetPositionX(), mover->GetPositionY(), mover->GetPositionZ());
    G3D::Vector3 landing = dest;
    // [ledge_drop_false] the landing is measured only on a ground path (its steep step); with no ground path it is
    // the destination itself, unmeasured - the event says which (bot-n 20261025-184229-56: landX..Z == x..z read as
    // "landing == the bot's own spot"; it was the destination, the dead tank's body 85 yd under the floor)
    bool groundPath = false;
    bool probedWater = false;  // [ledge_drop_false] the fall under an off-mesh destination ends in deep water
    {
        BotPath const path = FindBotPath(mover, x, y, z);  // [lead109]
        groundPath = PathIsGround(path.type);
        // [b33_cos2] A long walk the navmesh query cuts short (PATHFIND_INCOMPLETE: the corridor's MAX_PATH_LENGTH
        // polygons or a stalled smooth path) is no drop of the whole height: its partial path is ground walked toward
        // the destination and the walk is judged again from where it ends. cos-h 20261026-000434-cos-h-5H-22 (and
        // 221216-cos-h-5D-23, 222618-cos-n-5G-67, 002026-cos-n-5D-68): every "cos engage event target" walk from the
        // Crusaders' Square road (2264,1191,139) to the wave spots at (2139.8,1356.3) / (2145.2,1355.3), 195-229 yd,
        // was "shortcut_drop ... navmesh under the destination (no ground path)", 5.7-7.7 yd = the plain height
        // difference of a street walked down (z 139 -> 132 over 200 yd in the positions, no ledge). The incomplete
        // path ending at a brink near the destination (dtk-n 163758-62's island) still falls through to the old test:
        // it ends within LEDGE_LONG_WALK_REST of the destination or makes no LEDGE_LONG_WALK_GAIN progress.
        constexpr float LEDGE_LONG_WALK_REST = 40.0f;
        constexpr float LEDGE_LONG_WALK_GAIN = 20.0f;
        bool longWalk = false;
        if (!groundPath)
        {
            PathType const type = path.type;
            std::vector<G3D::Vector3> const& partial = path.points;
            if ((type & PATHFIND_INCOMPLETE) &&
                !(type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH | PATHFIND_SHORT)) &&
                partial.size() >= 2)
            {
                G3D::Vector3 const& end = partial.back();
                float const endToDest = std::hypot(end.x - x, end.y - y);
                float const hereToDest = std::hypot(here.x - x, here.y - y);
                longWalk = endToDest >= LEDGE_LONG_WALK_REST && hereToDest - endToDest >= LEDGE_LONG_WALK_GAIN &&
                           SteepStep(partial, false) <= 0.0f;
                if (longWalk)
                    landing = end;
            }
        }
        if (longWalk)
        {
            verdict.height = 0.0f;  // the partial path has no steep step: no drop on this leg
            verdict.onMesh = true;
        }
        else if (groundPath)
        {
            verdict.height = SteepStep(path.points, false, &landing);
            verdict.onMesh = true;
        }
        else
        {
            // no ground path: whatever leads there drops the whole height, to the navmesh floor under the
            // destination when there is one ([ledge_drop_false]); with none the landing stays unknown
            G3D::Vector3 snapped;
            verdict.onMesh = NavmeshLanding(mover, dest, snapped);
            if (verdict.onMesh)
                landing = snapped;
            else
            {
                // [ledge_drop_false] no navmesh at the destination (a waypoint in the air over water: sp-h
                // 20261025-190607-89, the route's drop from the walkway at (-187.2,-349.1,54.8) to (-189.3,-348.7,
                // 24.8), every build-29 pass fell on into the water at z ~3): the fall ends on the surface under it,
                // water or ground; that surface's navmesh (or the bottom's, under deep water) is the landing
                float groundZ = INVALID_HEIGHT;
                float const surface = map->GetWaterOrGroundLevel(mover->GetPhaseMask(), x, y, z, &groundZ, true,
                                                                 mover->GetCollisionHeight());
                if (surface > INVALID_HEIGHT && surface <= z + 1.0f)
                {
                    landing.z = surface;
                    probedWater = groundZ > INVALID_HEIGHT && surface - groundZ >= LEDGE_SWIM_DEPTH &&
                                  HasSwimWater(mover, x, y, groundZ);  // [lead145] not lava / slime
                    verdict.onMesh = NavmeshLanding(mover, landing, snapped) ||
                                     (probedWater &&
                                      NavmeshLanding(mover, G3D::Vector3(x, y, groundZ), snapped));
                    if (verdict.onMesh && !probedWater)
                        landing = snapped;
                }
            }
            verdict.height = here.z - landing.z;
        }
    }

    verdict.drop = verdict.height > LEDGE_DROP_DZ;
    // [b33_sp] the landing and the kind of walk for MovementAction::MoveTo's water drop
    verdict.landX = landing.x;
    verdict.landY = landing.y;
    verdict.landZ = landing.z;
    verdict.groundPath = groundPath;
    if (verdict.drop)
    {
        // (a) a ground path leads from the landing spot back to where the mover stands, with no cliff to climb
        BotPath const back = FindBotPath(mover, landing.x, landing.y, landing.z, here.x, here.y, here.z);  // [lead109]
        // [lead147] ... from a landing on the route's own navmesh (the low box): the route search's tall box snapped a
        // landing on Ragefire's magma (b165 (-82.9,-37.3,-31.3)) onto the ground 11 yd above it, "joined to the rest"
        G3D::Vector3 landingPoly;
        verdict.connected = NavmeshLanding(mover, landing, landingPoly) && PathIsGround(back.type) &&
                            SteepStep(back.points, true) <= 0.0f;
        // (b) + (c) the fall is safe for the mover and for every member near that follows it: a water landing at
        // any height, a listed shortcut with the health margin, else under the fall damage height
        verdict.water = probedWater || HasSwimWater(mover, landing.x, landing.y, landing.z);  // [lead145]
        verdict.listed = ListedShortcut(map->GetId(), landing) != nullptr;
        // [lead223] b185 rfc-h 5B 615-617 s: "shortcut_drop ... dry, navmesh under the destination (no ground path)",
        // 12.7 yd "safe" onto (-209.7,116.5,-38.4) - Ragefire's magma: the tank stood in it, the healer up on the
        // ledge lost it and the group died one by one, the tank burned to death at 841 s. The navmesh polygon under
        // the lava is floor and the fall harmless; the landing is not: unsafe, no stranded drop, no fallback
        verdict.burns = InHarmfulLiquid(mover, landing.x, landing.y, landing.z);
        verdict.safe = !verdict.burns && SafeFallFor(mover, verdict.height, verdict.water, verdict.listed);
        if (Player* player = mover->ToPlayer())
            if (Group* group = player->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->GetSource(); member && member != player && member->IsAlive() &&
                                                           member->GetMap() == map &&
                                                           member->GetExactDist(mover) <= LEDGE_GROUP_RANGE &&
                                                           !SafeFallFor(member, verdict.height, verdict.water,
                                                                        verdict.listed))
                        verdict.safe = false;
        // [ledge_drop_false] a fall with no damage for the mover and the members near, onto navmesh floor, is a
        // plain step down even with no way back up (Azjol-Nerub's one-way drops to the lower level); an island is
        // refused only when the fall hurts (dtk-n 163758-62: 18-34 yd) or the landing is no floor at all
        // A landing in deep water on navmesh is as harmless up to LEDGE_WATER_MAX_DROP (the swim on is the navmesh's).
        bool const harmless = verdict.onMesh && verdict.safe &&
                              (verdict.height <= LEDGE_FALL_SAFE ||
                               (verdict.water && verdict.height <= LEDGE_WATER_MAX_DROP));
        verdict.allowed = (verdict.connected && verdict.safe) || harmless;

        // [ledge_drop_false] a mover stranded off the navmesh (no polygon within LEDGE_SNAP_HALF_HEIGHT under it: a
        // knockback put it on a perch) has no way down but a drop, and the way back up it cannot have is no test:
        // nex-n 20261025-185842-81, Telestra's knockback left the tank on a perch at (517.7,73.4,-1.9) right above
        // the group at -16.1; its walks to the containment sphere (14.2 yd down, 1.3% fall damage) were refused for
        // 250 s. Allowed onto navmesh floor joined to a living member near by a ground path, while the mover keeps
        // LEDGE_LISTED_HP_MARGIN of its health after the fall; the members up there are judged on their own.
        if (!verdict.allowed && verdict.onMesh && !verdict.listed && !verdict.burns)
        {
            G3D::Vector3 under;
            bool const stranded = !NavmeshLanding(mover, here, under) || here.z - under.z > LEDGE_DROP_DZ;
            bool const survives = mover->GetMaxHealth() &&
                                  float(mover->GetHealth()) / float(mover->GetMaxHealth()) -
                                          FallDamageShare(mover, verdict.height) >=
                                      LEDGE_LISTED_HP_MARGIN;
            bool joined = false;
            if (stranded && survives)
                if (Player* player = mover->ToPlayer())
                    if (Group* group = player->GetGroup())
                        for (GroupReference* ref = group->GetFirstMember(); ref && !joined; ref = ref->next())
                        {
                            Player* member = ref->GetSource();
                            if (!member || member == player || !member->IsAlive() || member->GetMap() != map ||
                                member->GetExactDist(mover) > LEDGE_GROUP_RANGE ||
                                std::fabs(member->GetPositionZ() - landing.z) > LEDGE_DROP_DZ)
                                continue;

                            BotPath const toMember =  // [lead109]
                                FindBotPath(mover, landing.x, landing.y, landing.z, member->GetPositionX(),
                                            member->GetPositionY(), member->GetPositionZ());
                            joined = PathIsGround(toMember.type) &&
                                     SteepStep(toMember.points, true) <= 0.0f;
                        }

            if (joined)
            {
                verdict.stranded = true;
                verdict.connected = true;
                verdict.allowed = true;
            }
        }

        // [ledge_drop_false] the general guard: the same move refused LEDGE_REFUSALS_FALLBACK times in a row falls
        // back to the walk as it was before the ledge guard (build 29), when the landing is navmesh floor (or deep
        // water) and the fall leaves the mover and every member near with LEDGE_LISTED_HP_MARGIN of its health. The
        // b30 regressions it covers: an-n (425 refusals, the one-way drops to Hadronox' level, 9-12 yd), sp-h (the
        // route's drop into the water), nex-n (a knockback's perch); no-fall-death islands only (bot-n's 85 yd and
        // occ-h's 329 yd stay refused).
        if (!verdict.allowed)
        {
            if (memo.refusals && (memo.refusedDest - dest).length() <= LEDGE_REFUSED_SAME_DEST)
                ++memo.refusals;
            else
                memo.refusals = 1;
            memo.refusedDest = dest;

            auto const survives = [&verdict](Unit* unit)
            {
                if ((verdict.water && verdict.height <= LEDGE_WATER_MAX_DROP) ||
                    unit->HasAuraType(SPELL_AURA_FEATHER_FALL) || unit->HasAuraType(SPELL_AURA_HOVER))
                    return true;
                return unit->GetMaxHealth() && float(unit->GetHealth()) / float(unit->GetMaxHealth()) -
                                                       FallDamageShare(unit, verdict.height) >=
                                                   LEDGE_LISTED_HP_MARGIN;
            };
            bool fallback =
                !verdict.burns && memo.refusals >= LEDGE_REFUSALS_FALLBACK && verdict.onMesh && survives(mover);
            if (fallback)
                if (Player* player = mover->ToPlayer())
                    if (Group* group = player->GetGroup())
                        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                            if (Player* member = ref->GetSource(); member && member != player &&
                                                                   member->IsAlive() && member->GetMap() == map &&
                                                                   member->GetExactDist(mover) <= LEDGE_GROUP_RANGE &&
                                                                   member->GetPositionZ() - landing.z >
                                                                       LEDGE_DROP_DZ &&
                                                                   !survives(member))
                                fallback = false;

            if (fallback)
            {
                verdict.fallback = true;
                verdict.allowed = true;
                LOG_INFO("playerbots",
                         "[ledge drop fallback] {}: {} to ({:.1f},{:.1f},{:.1f}) refused {} times, taken as before "
                         "the ledge guard: drop {:.1f} yd onto navmesh{} at ({:.1f},{:.1f},{:.1f})",
                         mover->GetName(), what ? what : "walk", x, y, z, memo.refusals, verdict.height,
                         verdict.water ? " (water)" : "", landing.x, landing.y, landing.z);
                memo.refusals = 0;
            }
        }
        else
            memo.refusals = 0;

        if (!memo.loggedMs || getMSTimeDiff(memo.loggedMs, now) >= LEDGE_LOG_MS)
        {
            memo.loggedMs = now ? now : 1;
            char const* const by = what ? what : "walk";
            char const* const kind = verdict.burns      ? "lava/slime"
                                     : verdict.water    ? "water"
                                     : verdict.listed   ? "listed"
                                     : verdict.stranded ? "stranded"
                                     : verdict.fallback ? "fallback"
                                                        : "dry";
            LOG_INFO("playerbots",
                     "[ledge drop] {}: {} {} to ({:.1f},{:.1f},{:.1f}): drop {:.1f} yd ({}), landing ({:.1f},{:.1f},"
                     "{:.1f}) {}, fall {}",
                     mover->GetName(), verdict.allowed ? "shortcut_drop" : "refused", by, x, y, z, verdict.height,
                     kind, landing.x, landing.y, landing.z, verdict.connected ? "joined to the rest" : "an island",
                     verdict.safe ? "safe" : "unsafe for the group");
            if (mover->IsPlayer())
            {
                BotMoveEvents::Event event;
                event.type = verdict.allowed ? "shortcut_drop" : "ledge_drop_refused";
                event.Str("by", by)
                    .Str("kind", kind)
                    .Real("height", verdict.height)
                    .Num("connected", verdict.connected ? 1.0 : 0.0)
                    .Num("safe", verdict.safe ? 1.0 : 0.0)
                    .Real("x", x)
                    .Real("y", y)
                    .Real("z", z)
                    .Real("landX", landing.x)
                    .Real("landY", landing.y)
                    .Real("landZ", landing.z)
                    // [ledge_drop_false] where the mover stands, and whether the landing was measured (a ground
                    // path's step) or is the unmeasured destination (no ground path)
                    .Real("fromX", here.x)
                    .Real("fromY", here.y)
                    .Real("fromZ", here.z)
                    .Str("landing", groundPath       ? "path step"
                                    : verdict.onMesh ? "navmesh under the destination (no ground path)"
                                                     : "unknown: no navmesh under the destination");
                BotMoveEvents::Push(mover->GetGUID(), std::move(event));
            }
        }
    }

    memo.dest = dest;
    memo.ms = now ? now : 1;
    memo.verdict = verdict;
    return verdict;
}

bool UnderWalkableFloor(Unit* mover, float x, float y, float z)
{
    Map* map = mover ? mover->GetMap() : nullptr;
    if (!map || mover->GetTransport() || mover->IsFlying())
        return false;

    // the first static surface down from UNDER_FLOOR_PROBE_UP above the spot (a ceiling that low is no room to
    // walk in either)
    float const above = map->GetHeight(mover->GetPhaseMask(), x, y, z + UNDER_FLOOR_PROBE_UP, true,
                                       UNDER_FLOOR_PROBE_UP);
    return above > INVALID_HEIGHT && above > z + UNDER_FLOOR_DZ;
}

bool GapCloserLeavesGround(Unit* caster, Unit* target, char const* what)
{
    if (!caster || !target || caster->GetMap() != target->GetMap() || caster->GetVehicle() || caster->IsFlying())
        return false;

    // [lead9] a harness tank's Charge / Intercept / Feral Charge never leaves the fight's safe zone and never opens a
    // pull of its own between the leader's orders (SafeZone.h, C:/WoW/LEADER_REFACTOR_PLAN.md step 9)
    if (Leader::FightZone::GapCloserLeavesZone(caster, target, what))
        return true;

    // a target in the air: the charge ends on whatever ground lies under it
    float level = target->GetMapWaterOrGroundLevel(target->GetPositionX(), target->GetPositionY(),
                                                   target->GetPositionZ());
    // [b35_z0] the grid fallback (0.0 on a WMO-only tile) is no ground: a target standing on navmesh is on its floor
    if (IsGridFallbackZ(caster, target->GetPositionX(), target->GetPositionY(), level))
    {
        float floor = 0.0f;
        if (NavmeshFloorZ(caster, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
                          GAP_CLOSER_AIR_HEIGHT, floor) &&
            floor <= target->GetPositionZ() + 1.0f)
            level = floor;
    }
    if (level <= INVALID_HEIGHT || target->GetPositionZ() - level > GAP_CLOSER_AIR_HEIGHT)
        return true;

    // a target below: the charge is a walk off the ledge to it
    if (caster->GetPositionZ() - level > LEDGE_DROP_DZ &&
        !JudgeLedgeDrop(caster, target->GetPositionX(), target->GetPositionY(), level, what).allowed)
        return true;

    // [b40_strat] through an open door that shuts by itself (GAMEOBJECT_TYPE_DOOR, autoCloseTime: Stratholme's key
    // gates stand open 3 s): the charge lands on the far side and the door shuts behind it, the group (and the
    // healer's sight) on the other side. strat-h 20261026-060218-strat-h-5H-1 (1793 s): the bear tank's "feral charge -
    // bear on group member attacker" onto a Mangled Cadaver 7 yd past King's Square Gate 175353 while it stood open
    // after the key; the gate shut, the healer cut off at 7-12 yd, the tank dead at 1812 s, two more by 1820 s.
    if (Map* map = caster->GetMap())
    {
        constexpr float GAP_CLOSER_DOOR_LINE = 4.0f;  // the door's centre this close (2d) to the charge's line
        float const ax = caster->GetPositionX();
        float const ay = caster->GetPositionY();
        float const bx = target->GetPositionX();
        float const by = target->GetPositionY();
        float const len2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
        for (ObjectGuid const& guid : MapDoors(map))
        {
            GameObject* go = map->GetGameObject(guid);
            if (!go || !go->IsInWorld() || !go->isSpawned() || go->GetGoType() != GAMEOBJECT_TYPE_DOOR ||
                go->GetGoState() == GO_STATE_READY || !go->GetGOInfo()->door.autoCloseTime ||
                !caster->InSamePhase(go))
                continue;
            float const px = go->GetPositionX();
            float const py = go->GetPositionY();
            float const t =
                len2 > 0.0001f ? std::clamp(((px - ax) * (bx - ax) + (py - ay) * (by - ay)) / len2, 0.0f, 1.0f) : 0.0f;
            float const cx = ax + (bx - ax) * t;
            float const cy = ay + (by - ay) * t;
            if ((px - cx) * (px - cx) + (py - cy) * (py - cy) <= GAP_CLOSER_DOOR_LINE * GAP_CLOSER_DOOR_LINE)
            {
                LOG_DEBUG("playerbots", "[b40_strat] {}: no {} onto {}: through {} {}, open, it shuts by itself",
                          caster->GetName(), what ? what : "gap closer", target->GetName(), go->GetName(),
                          go->GetEntry());
                return true;
            }
        }
    }

    return false;
}

void AllowLedgeDrop(ObjectGuid mover, uint32 forMs)
{
    std::lock_guard<std::mutex> guard(ledgeAllowLock);
    if (ledgeAllowUntil.size() > 256)
        ledgeAllowUntil.clear();
    uint32 const until = getMSTime() + forMs;
    ledgeAllowUntil[mover.GetRawValue()] = until ? until : 1;
}

bool IsGridFallbackZ(Unit* mover, float x, float y, float z)
{
    if (z <= INVALID_HEIGHT)
        return true;

    Map* map = mover ? mover->GetMap() : nullptr;
    if (!map)
        return false;

    // Map::GetHeight's last answer: the raw .map surface, which a WMO-only tile holds flat at gridHeight
    float const grid = map->GetGridHeight(x, y);
    return grid > INVALID_HEIGHT && std::fabs(z - grid) < 0.05f;
}

bool NavmeshFloorZ(Unit* mover, float x, float y, float z, float halfHeight, float& floorZ)
{
    Map* map = mover ? mover->GetMap() : nullptr;
    dtNavMeshQuery const* query = map ? map->GetMapCollisionData().GetMMapData().GetNavMeshQuery() : nullptr;
    if (!query)
        return false;

    dtQueryFilter filter;
    filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
    filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));
    // Detour works in (y, z, x).
    float const center[3] = {y, z, x};
    float const extents[3] = {DEST_Z_SNAP_HALF_WIDTH, halfHeight, DEST_Z_SNAP_HALF_WIDTH};
    float nearest[3];
    dtPolyRef ref = 0;
    if (dtStatusFailed(query->findNearestPoly(center, extents, &filter, &ref, nearest)) || !ref)
        return false;

    // the polygon's own height right at (x, y) when (x, y) lies over it, else its nearest point's
    float height = nearest[1];
    float const at[3] = {y, nearest[1], x};
    if (dtStatusSucceed(query->getPolyHeight(ref, at, &height)))
        floorZ = height;
    else
        floorZ = nearest[1];
    return true;
}

bool SafeFloorZ(Unit* mover, float x, float y, float& z)
{
    if (!mover || !mover->GetMap())
        return false;

    float floor = 0.0f;
    if (NavmeshFloorZ(mover, x, y, z, DEST_Z_SNAP_HALF_HEIGHT, floor))
    {
        z = floor;
        return true;
    }

    // no navmesh (a map without mmaps, a spot just off it): the collision floor, when it is a real one near the seed
    floor = mover->GetMapHeight(x, y, z);
    if (floor > INVALID_HEIGHT && !IsGridFallbackZ(mover, x, y, floor) && std::fabs(floor - z) <= LEDGE_DROP_DZ)
    {
        z = floor;
        return true;
    }

    return false;
}

bool SnapGridFallbackZ(Unit* mover, float x, float y, float& z, char const* what)
{
    if (!mover || !mover->IsInWorld() || mover->IsFlying() || mover->IsInWater() || mover->GetTransport() ||
        mover->GetVehicle())
        return false;

    float const here = mover->GetPositionZ();
    if (std::fabs(z - here) <= LEDGE_DROP_DZ || !IsGridFallbackZ(mover, x, y, z))
        return false;

    // navmesh at that height: a real floor (outdoor terrain at the grid's height, a lower level), the guards judge it
    float floor = 0.0f;
    if (z > INVALID_HEIGHT && NavmeshFloorZ(mover, x, y, z, DEST_Z_SNAP_HALF_HEIGHT, floor))
        return false;

    if (!NavmeshFloorZ(mover, x, y, here, LEDGE_DROP_DZ, floor))
        return false;

    thread_local std::unordered_map<uint64, uint32> loggedMs;
    if (loggedMs.size() > 512)
        loggedMs.clear();
    uint32 const now = getMSTime();
    uint32& at = loggedMs[mover->GetGUID().GetRawValue()];
    if (!at || getMSTimeDiff(at, now) >= 10 * IN_MILLISECONDS)
    {
        at = now ? now : 1;
        LOG_INFO("playerbots",
                 "[b35_z0] [dest z] {}: {} to ({:.1f},{:.1f},{:.1f}): z is the map grid's fallback {:.1f} yd from "
                 "the bot (z {:.1f}), no floor there: navmesh floor z {:.1f}",
                 mover->GetName(), what ? what : "walk", x, y, z, here - z, here, floor);
    }

    z = floor;
    return true;
}
