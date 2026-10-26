/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_REFLECTAURAS_H
#define PLAYERBOTS_REFLECTAURAS_H

#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

// Auras that turn an attack back on the attacker: spell reflection (a reflected Deep Freeze kills the mage) and
// melee counterattacks such as Retaliation (every melee hit strikes back). Bots wait them out or switch targets.
namespace ReflectAuras
{
    constexpr uint32 SPELL_RETALIATION = 20230;           // dummy aura; spell_warrior strikes back with 22858
    constexpr float DAMAGE_SHIELD_MIN_HEALTH_PCT = 3.0f;  // a thorns-like shield this strong per hit is a counter

    // Mirrors Spell::m_canReflect: a harmful magic spell that is not an ability or marked unreflectable.
    inline bool IsReflectable(SpellInfo const* info)
    {
        return info->DmgClass == SPELL_DAMAGE_CLASS_MAGIC && !info->HasAttribute(SPELL_ATTR0_IS_ABILITY) &&
               !info->HasAttribute(SPELL_ATTR1_NO_REFLECTION) && !info->HasAttribute(SPELL_ATTR0_NO_IMMUNITIES) &&
               !info->IsPassive() && (!info->IsPositive() || info->HasEffect(SPELL_EFFECT_DISPEL));
    }

    inline bool ReflectsSpells(Unit const* target)
    {
        return target && (target->HasAuraType(SPELL_AURA_REFLECT_SPELLS) ||
                          target->HasAuraType(SPELL_AURA_REFLECT_SPELLS_SCHOOL));
    }

    // `target` would bounce `info` back at the caster.
    inline bool ReflectsSpell(Unit const* target, SpellInfo const* info)
    {
        if (!info || !ReflectsSpells(target) || !IsReflectable(info))
            return false;

        int32 const chance = target->GetTotalAuraModifier(SPELL_AURA_REFLECT_SPELLS) +
                             target->GetTotalAuraModifierByMiscMask(SPELL_AURA_REFLECT_SPELLS_SCHOOL,
                                                                    info->GetSchoolMask());
        return chance > 0;
    }

    inline bool IsWeaponStrike(SpellInfo const* info)
    {
        return info->HasEffect(SPELL_EFFECT_WEAPON_PERCENT_DAMAGE) || info->HasEffect(SPELL_EFFECT_WEAPON_DAMAGE) ||
               info->HasEffect(SPELL_EFFECT_NORMALIZED_WEAPON_DMG) ||
               info->HasEffect(SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL);
    }

    // `target` strikes back at whoever hits it in melee: a proc aura that answers a melee hit with a weapon strike
    // (Retaliation and its npc copies), or a damage shield worth several percent of the attacker's health a hit.
    inline bool CountersMelee(Unit const* target, Unit const* attacker)
    {
        if (!target)
            return false;

        if (target->HasAura(SPELL_RETALIATION))
            return true;

        for (AuraEffect const* effect : target->GetAuraEffectsByType(SPELL_AURA_PROC_TRIGGER_SPELL))
        {
            SpellInfo const* info = effect->GetSpellInfo();
            if (!info || !(info->ProcFlags & PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK))
                continue;

            SpellInfo const* strike = sSpellMgr->GetSpellInfo(info->Effects[effect->GetEffIndex()].TriggerSpell);
            if (strike && IsWeaponStrike(strike))
                return true;
        }

        if (!attacker)
            return false;

        float const threshold = float(attacker->GetMaxHealth()) * DAMAGE_SHIELD_MIN_HEALTH_PCT / 100.0f;
        for (AuraEffect const* effect : target->GetAuraEffectsByType(SPELL_AURA_DAMAGE_SHIELD))
        {
            if (float(effect->GetAmount()) >= threshold)
                return true;
        }

        return false;
    }
}

#endif
