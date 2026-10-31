/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PALADINHELPER_H
#define PLAYERBOTS_PALADINHELPER_H

#include "Player.h"
#include "Unit.h"
#include <initializer_list>

namespace ai::paladin
{
static constexpr uint32 SPELL_HAND_OF_PROTECTION = 1022;
static constexpr uint32 SPELL_HAND_OF_SALVATION = 1038;
static constexpr uint32 SPELL_HAND_OF_FREEDOM = 1044;
static constexpr uint32 SPELL_HAND_OF_SACRIFICE = 6940;

inline bool HasHandFromCaster(Unit* target, Unit* caster, std::initializer_list<uint32> spellIds)
{
    if (!target || !caster)
        return false;

    for (uint32 spellId : spellIds)
    {
        if (target->HasAura(spellId, caster->GetGUID()))
            return true;
    }

    return false;
}

inline bool HasAnyPaladinHandFromCaster(Unit* target, Unit* caster)
{
    return HasHandFromCaster(target, caster,
        { SPELL_HAND_OF_PROTECTION, SPELL_HAND_OF_SALVATION, SPELL_HAND_OF_FREEDOM, SPELL_HAND_OF_SACRIFICE });
}

// [b33_class] What Hand of Freedom can actually clear: a root or a snare (movement-impairing). PlayerbotAI::
// IsMovementImpaired also reports stuns and knockdowns (IsRooted() / zero run speed), which Hand of Freedom cannot
// remove: the paladin burned the GCD on them. A stunned target is skipped even when also snared.
inline bool HasFreedomRemovableImpairment(Unit* unit)
{
    if (!unit || unit->HasUnitState(UNIT_STATE_STUNNED) || unit->HasAuraType(SPELL_AURA_MOD_STUN))
        return false;

    return unit->HasAuraType(SPELL_AURA_MOD_ROOT) || unit->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
           unit->HasAuraWithMechanic((1ULL << MECHANIC_ROOT) | (1ULL << MECHANIC_SNARE));
}
}

#endif
