/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_DAMAGETAKEN_H
#define _PLAYERBOT_DAMAGETAKEN_H

#include "ObjectGuid.h"

// Damage a player took from creatures inside a dungeon or raid since the server started, as DealDamage saw it (after
// armor, resistances, absorbs and avoidance). Kept by the death recorder's DealDamage hook; read as a difference
// over a fight (the careful pull planner's measured calibration).
namespace DamageTaken
{
    uint64 Total(ObjectGuid player);
}

#include <string>

class Player;

// [b30_reenter] A re-entry of the instance written to <LogsDir>/deaths.jsonl as a death (the death recorder's
// record, its killer `killer`, and `context`: a JSON object, the "reentry" field). Call before the teleport.
namespace DeathRecorder
{
    void WriteReentry(Player* player, std::string const& killer, std::string const& context);
}

#endif
