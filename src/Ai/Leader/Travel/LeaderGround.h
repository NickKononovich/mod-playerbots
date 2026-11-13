/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LEADERGROUND_H
#define _PLAYERBOT_LEADERGROUND_H

#include "Log.h"
#include "Map.h"
#include "TerrainHazard.h"
#include "Unit.h"

#include <source_location>  // [navz10] the builder that got no floor
#include <string_view>
#include <unordered_set>

namespace Leader::Travel
{
    // [dun206] the floor at (x, y) for a spot the leader builds, searched from `seedZ` down `searchDist`
    // (Map::GetHeight, the vmap floor first). Its last answer, the map grid's raw surface (IsGridFallbackZ), is a
    // floor only where the navmesh has one at that height on dry ground (outdoor terrain); else the navmesh floor
    // near the seed, else none (INVALID_HEIGHT). b438 dm-n five 9: GetHeight's grid fallback -0.8 (the cove water
    // under the ship's deck) was taken as the floor, the tank walked a complete 346 yd route into the water under
    // the ship and its engage order on the Goblin Shipbuilder stood orphaned there
    //
    // [navz10] A rejection is named, never silent. Before this the three gates could all fail and the builder got
    // INVALID_HEIGHT with nothing in the log: the spot simply did not exist, and the missing order looked like a
    // leader that never decided. The line below says which builder asked (file:line), where, and what the map
    // answered, so the next instance is one grep. It changes no decision - the verdict is the same
    // INVALID_HEIGHT this function already returned.
    inline float LeaderGroundZ(Unit* who, float x, float y, float seedZ, float searchDist = DEFAULT_HEIGHT_SEARCH,
                               std::source_location const where = std::source_location::current())
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
        // [navz10] no honest floor here: say so once per builder and place (the set is bounded like [lag350]'s,
        // a memory bound on a log, not a rule any decision reads)
        thread_local std::unordered_set<uint64> saidNoFloor;
        if (saidNoFloor.size() > 512)
            saidNoFloor.clear();
        uint64 const place = uint64(uint32(where.line())) * 1000003ull
                             + uint64(uint32(int32(x)) * 7919u) + uint64(uint32(int32(y)) * 104729u);
        if (saidNoFloor.insert(place).second)
        {
            std::string_view file = where.file_name();
            if (size_t const slash = file.find_last_of("/\\"); slash != std::string_view::npos)
                file.remove_prefix(slash + 1);
            LOG_INFO("playerbots",
                     "[navz10] {}: no honest floor at ({:.1f},{:.1f}) for a spot seeded at {:.1f} on map {}: "
                     "Map::GetHeight {:.1f}, grid {:.1f} (grid fallback {}), water {:.1f}, and no navmesh floor "
                     "within {:.1f} yd of either - no spot built, from {}:{}",
                     who->GetName(), x, y, seedZ, map->GetId(), ground, map->GetGridHeight(x, y),
                     IsGridFallbackZ(who, x, y, ground) ? "true" : "false", water,
                     DEST_Z_SNAP_HALF_HEIGHT, file, where.line());
        }
        return INVALID_HEIGHT;
    }
}

#endif
