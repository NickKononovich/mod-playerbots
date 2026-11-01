/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_INSTANCEGUID_H
#define PLAYERBOTS_INSTANCEGUID_H

#include "Object.h"
#include "ObjectGuid.h"

#include <utility>

// A creature / game object / guardian GUID is unique only inside its map instance: the low counters are per map, so
// two instances of the same dungeon (one map thread updates several) hand out the same GUIDs. A cache that outlives
// one call and is keyed by such a GUID keys by (instance id, GUID). Players and Pet-type pets have global GUIDs.
using InstanceGuid = std::pair<uint32, ObjectGuid>;

inline InstanceGuid InstanceKey(WorldObject const* object)
{
    return object ? InstanceGuid(object->GetInstanceId(), object->GetGUID()) : InstanceGuid(0, ObjectGuid::Empty);
}

#endif
