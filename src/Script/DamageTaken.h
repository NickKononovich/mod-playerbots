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
// [lead1431] With the damage its absorbs took (Power Word: Shield, Divine Aegis, ...): the shield's amount at its
// apply less what is left at its removal (Unit::CalcAbsorbResist lowers the amount by each absorb).
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

// [lead1431] Mana a player's own casts spent inside a dungeon or raid since the server started on spells that heal
// nothing themselves - seals, blessings, Beacon of Light, Sacred Shield, Earth Shield's refresh, totems, buffs,
// dispels, damage: the cost the game took (Spell::TakePower's conditions). A heal, a heal over time, an absorb and
// the heals cast through a dummy (Holy Shock, Penance, Prayer of Mending) are not upkeep. Kept by the death recorder's
// spell cast hook.
namespace ManaSpent
{
    uint64 Upkeep(ObjectGuid player);
    // [lead1434] ... and on its heals (the same hook, the casts that heal by themselves): a healer's heal mana is the
    // sum of its heals' costs, never its pool's change (out-of-fight and in-fight regen do not net against it)
    uint64 OnHeals(ObjectGuid player);
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
