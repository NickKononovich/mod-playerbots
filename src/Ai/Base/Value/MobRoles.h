/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MOBROLES_H
#define PLAYERBOTS_MOBROLES_H

#include "Creature.h"
#include "SmartScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <mutex>
#include <unordered_map>

// What a trash mob does in a fight, read from its SmartAI casts and its creature_template spells:
// - a standing caster: a SmartAI damage cast carries SMARTCAST_COMBAT_MOVE, so it walks only when it cannot cast;
// - a ranged caster: a paladin or mage class mob with a magic damage spell cast from range (it may walk in, but it
//   nukes on the way and keeps nuking from where it stands);
// - a healer (SmartAI heal casts).
// Cached per creature template (the heroic template has its own spells).
namespace MobRoles
{
    enum RoleFlags : uint8
    {
        ROLE_NONE = 0x00,
        ROLE_STANDING_CASTER = 0x01,
        ROLE_HEALER = 0x02,
        ROLE_RANGED_CASTER = 0x04,
        // [b33_brs] a SmartAI mob that never turns its combat movement off (no SMART_ACTION_ALLOW_COMBAT_MOVEMENT 0):
        // a cast without SMARTCAST_COMBAT_MOVE stops it only for the cast, then SmartAI chases to melee again
        ROLE_SMART_CHASES = 0x08,
    };

    // Longer than Judgement (10 yd) of a melee paladin mob, shorter than a shock (20 yd).
    constexpr float RANGED_CASTER_MIN_RANGE = 15.0f;

    inline bool IsDamageSpell(SpellInfo const* info)
    {
        return info->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE) || info->HasEffect(SPELL_EFFECT_HEALTH_LEECH) ||
               info->HasAura(SPELL_AURA_PERIODIC_DAMAGE) || info->HasAura(SPELL_AURA_PERIODIC_LEECH);
    }

    inline bool IsRangedDamageSpell(SpellInfo const* info)
    {
        return info && info->DmgClass == SPELL_DAMAGE_CLASS_MAGIC && !info->IsPositive() && IsDamageSpell(info) &&
               info->GetMaxRange(false) > RANGED_CASTER_MIN_RANGE;
    }

    inline uint8 ComputeRoles(Creature const* creature)
    {
        uint8 roles = ROLE_NONE;
        bool rangedDamage = false;
        if (creature->GetAIName() == "SmartAI")
        {
            bool stopsMoving = false;  // [b33_brs]
            for (SmartScriptHolder const& holder :
                 sSmartScriptMgr->GetScript(int32(creature->GetEntry()), SMART_SCRIPT_TYPE_CREATURE))
            {
                uint32 spellId = 0;
                uint32 castFlags = 0;
                if (holder.GetActionType() == SMART_ACTION_ALLOW_COMBAT_MOVEMENT && !holder.action.combatMove.move)
                {
                    stopsMoving = true;
                    continue;
                }
                if (holder.GetActionType() == SMART_ACTION_CAST)
                {
                    spellId = holder.action.cast.spell;
                    castFlags = holder.action.cast.castFlags;
                }
                else if (holder.GetActionType() == SMART_ACTION_CUSTOM_CAST)
                {
                    spellId = holder.action.castCustom.spell;
                    castFlags = holder.action.castCustom.flags;
                }
                else
                    continue;

                SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                if (!info)
                    continue;

                if (info->HasEffect(SPELL_EFFECT_HEAL) || info->HasEffect(SPELL_EFFECT_HEAL_PCT) ||
                    info->HasAura(SPELL_AURA_PERIODIC_HEAL))
                    roles = uint8(roles | ROLE_HEALER);

                if ((castFlags & SMARTCAST_COMBAT_MOVE) && !info->IsPositive() &&
                    (info->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE) || info->HasAura(SPELL_AURA_PERIODIC_DAMAGE)))
                    roles = uint8(roles | ROLE_STANDING_CASTER);

                rangedDamage = rangedDamage || IsRangedDamageSpell(info);
            }

            if (!stopsMoving)
                roles = uint8(roles | ROLE_SMART_CHASES);
        }

        CreatureTemplate const* cinfo = creature->GetCreatureTemplate();
        for (uint8 i = 0; i < MAX_CREATURE_SPELLS && !rangedDamage; ++i)
            rangedDamage = cinfo->spells[i] && IsRangedDamageSpell(sSpellMgr->GetSpellInfo(cinfo->spells[i]));

        // A warrior or rogue class mob with a ranged spell still fights in melee; only the mana classes nuke.
        if (rangedDamage && (cinfo->unit_class == CLASS_PALADIN || cinfo->unit_class == CLASS_MAGE))
            roles = uint8(roles | ROLE_RANGED_CASTER);

        return roles;
    }

    inline uint8 GetRoles(Unit const* unit)
    {
        Creature const* creature = unit ? unit->ToCreature() : nullptr;
        CreatureTemplate const* cinfo = creature ? creature->GetCreatureTemplate() : nullptr;
        if (!cinfo)
            return ROLE_NONE;

        static std::mutex lock;
        static std::unordered_map<uint32, uint8> cache;
        uint32 const key = cinfo->Entry;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const itr = cache.find(key);
            if (itr != cache.end())
                return itr->second;
        }

        uint8 const roles = ComputeRoles(creature);
        std::lock_guard<std::mutex> guard(lock);
        cache[key] = roles;
        return roles;
    }

    inline bool IsStandingCaster(Unit const* unit) { return (GetRoles(unit) & ROLE_STANDING_CASTER) != 0; }
    inline bool IsCaster(Unit const* unit)
    {
        return (GetRoles(unit) & (ROLE_STANDING_CASTER | ROLE_RANGED_CASTER)) != 0;
    }
    inline bool IsHealer(Unit const* unit) { return (GetRoles(unit) & ROLE_HEALER) != 0; }

    // [b33_brs] Stands and casts from range once it fights (the pull planner's KeepsRange, which draws the fight to
    // where it stands): a standing caster (SMARTCAST_COMBAT_MOVE), or a ranged caster by class whose AI does not
    // chase. A SmartAI paladin / mage class mob whose ranged damage casts carry no SMARTCAST_COMBAT_MOVE and that
    // never turns its combat movement off chases and melees: it stays a caster (crowd control, kill order, marks),
    // but it draws no fight. st-h 20261026-000434-st-h-5A-8 (Atal'ai Deathwalker 5271, measured in melee), the
    // Firebrand Dreadweaver 9263 / Pyromancer 9264 (all casts flags 0) and the Auchenai Necromancer 18702 (flags 32)
    // made "reaches #N, which keeps its range" chains across the LBRS Firebrand hall and the Auchenai Crypts bone
    // field (20-39 mobs counted per way in).
    inline bool HoldsRange(Unit const* unit)
    {
        uint8 const roles = GetRoles(unit);
        return (roles & ROLE_STANDING_CASTER) ||
               ((roles & ROLE_RANGED_CASTER) && !(roles & ROLE_SMART_CHASES));
    }
}

#endif
