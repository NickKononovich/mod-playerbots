/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_HAZARDZONE_H
#define _PLAYERBOT_HAZARDZONE_H

#include "ObjectGuid.h"
#include "Position.h"

#include <string>

// [lead16] One ground hazard as "avoid aoe" sees it (a damage trap, an invisible trigger NPC's pulsing damage, a
// mob's area cast with a cast bar, an area spell's dynamic object, a DungeonHazard circle): a circle to stay out of.
// The dungeon leader's hazard view (TestHarnessMgr::TickLeaderEvents) and AvoidAoeAction::CollectZones share it.
struct HazardZone
{
    Position centre;
    float radius = 0.0f;
    std::string what;
    ObjectGuid source;  // [lead460] the mob whose area cast it is (empty for every other kind)
};

#endif
