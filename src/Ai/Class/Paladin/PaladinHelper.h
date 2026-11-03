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
static constexpr uint32 SPELL_GUARDED_BY_THE_LIGHT_R1 = 53583;
static constexpr uint32 SPELL_GUARDED_BY_THE_LIGHT_R2 = 53585;

// [rot150] Guarded by the Light (Spell.dbc 53583 / 53585: effect 2 PROC_TRIGGER_SPELL 63521 on a melee swing or melee
// spell hit, 50 / 100 %; spell_pal_guarded_by_the_light refreshes Divine Plea, whose 5 % of max mana ticks every 3 s go
// on). Rawr ClassTalents.cs:2037-2041. A tank with it keeps one Divine Plea up for the whole fight by hitting.
inline bool KeepsDivinePleaUpByHits(Player* bot)
{
    return bot && (bot->HasSpell(SPELL_GUARDED_BY_THE_LIGHT_R1) || bot->HasSpell(SPELL_GUARDED_BY_THE_LIGHT_R2));
}

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
