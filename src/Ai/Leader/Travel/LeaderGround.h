/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LEADERGROUND_H
#define _PLAYERBOT_LEADERGROUND_H

#include "Map.h"
#include "TerrainHazard.h"
#include "Unit.h"

namespace Leader::Travel
{
    // [dun206] the floor at (x, y) for a spot the leader builds, searched from `seedZ` down `searchDist`
    // (Map::GetHeight, the vmap floor first). Its last answer, the map grid's raw surface (IsGridFallbackZ), is a
    // floor only where the navmesh has one at that height on dry ground (outdoor terrain); else the navmesh floor
    // near the seed, else none (INVALID_HEIGHT). b438 dm-n five 9: GetHeight's grid fallback -0.8 (the cove water
    // under the ship's deck) was taken as the floor, the tank walked a complete 346 yd route into the water under
    // the ship and its engage order on the Goblin Shipbuilder stood orphaned there
    inline float LeaderGroundZ(Unit* who, float x, float y, float seedZ, float searchDist = DEFAULT_HEIGHT_SEARCH)
    {
        Map* map = who ? who->GetMap() : nullptr;
        if (!map)
            return INVALID_HEIGHT;
        float const ground = map->GetHeight(who->GetPhaseMask(), x, y, seedZ, true, searchDist);
        float floor = 0.0f;
        if (ground > INVALID_HEIGHT && !IsGridFallbackZ(who, x, y, ground))
            return ground;
        // the grid's surface is the terrain the navmesh walks there, dry (not a liquid's surface or bed)
        float const water = map->GetWaterLevel(x, y);
        if (ground > INVALID_HEIGHT && !(water > INVALID_HEIGHT && water >= ground - 0.5f) &&
            NavmeshFloorZ(who, x, y, ground, DEST_Z_SNAP_HALF_HEIGHT, floor))
            return ground;
        if (NavmeshFloorZ(who, x, y, seedZ, DEST_Z_SNAP_HALF_HEIGHT, floor))
            return floor;
        return INVALID_HEIGHT;
    }
}

#endif
