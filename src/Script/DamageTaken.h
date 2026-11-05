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
    uint64 Spell(ObjectGuid player);  // [lead256] the part of Total armor does not reduce (magic schools)
    // [lead1344] the part of Total one creature dealt (DealDamage's attacker), this instance only
    uint64 From(ObjectGuid player, ObjectGuid attacker);
}

// [lead256] Health a player's heals restored inside a dungeon or raid since the server started, overheal cut off.
namespace HealingDone
{
    uint64 Effective(ObjectGuid healer);
}

// [lead1400] Mana a player took inside a dungeon or raid since the server started from the returns a boss gate's mana
// line credits on top of the measured use (FightManaLine): its own Evocation, Divine Plea, Innervate, Shadowfiend's
// Mana Leech, and a consumable's use (a mana potion, a mana gem). Kept by the death recorder's power gain hook.
namespace ManaReturned
{
    uint64 FromTools(ObjectGuid player);
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
