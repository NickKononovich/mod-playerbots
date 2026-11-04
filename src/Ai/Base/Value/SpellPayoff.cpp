/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SpellPayoff.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Spell.cpp TriggerGlobalCooldown: a global cooldown stays within 1-1.5 s
    constexpr int32 GCD_MIN_MS = 1000;
    constexpr int32 GCD_MAX_MS = 1500;
    constexpr uint32 GCD_HASTED_CATEGORY = 133;

    // Spell::TriggerGlobalCooldown: the spell mods, then cast speed for a 1.5 s spell of category 133
    float GlobalCooldownMs(Player* bot, SpellInfo const* info)
    {
        int32 gcd = static_cast<int32>(info->StartRecoveryTime);
        if (gcd < GCD_MIN_MS || gcd > GCD_MAX_MS)
            return static_cast<float>(std::max(gcd, 0));

        bot->ApplySpellMod(info->Id, SPELLMOD_GLOBAL_COOLDOWN, gcd);
        if (info->StartRecoveryCategory == GCD_HASTED_CATEGORY && info->StartRecoveryTime == 1500 &&
            info->DmgClass != SPELL_DAMAGE_CLASS_MELEE && info->DmgClass != SPELL_DAMAGE_CLASS_RANGED &&
            !info->HasAttribute(SPELL_ATTR0_USES_RANGED_SLOT) && !info->HasAttribute(SPELL_ATTR0_IS_ABILITY))
            gcd = static_cast<int32>(static_cast<float>(gcd) * bot->GetFloatValue(UNIT_MOD_CAST_SPEED));

        return static_cast<float>(std::clamp(gcd, GCD_MIN_MS, GCD_MAX_MS));
    }

    // Spell::prepare for a channel: SPELLMOD_DURATION, then cast speed where haste affects its ticks
    float ChannelMs(Player* bot, SpellInfo const* info)
    {
        int32 duration = info->GetDuration();
        if (duration <= 0)
            return 0.0f;

        bot->ApplySpellMod(info->Id, SPELLMOD_DURATION, duration);
        if (bot->HasAuraTypeWithAffectMask(SPELL_AURA_PERIODIC_HASTE, info) ||
            info->HasAttribute(SPELL_ATTR5_SPELL_HASTE_AFFECTS_PERIODIC))
            duration = static_cast<int32>(static_cast<float>(duration) * bot->GetFloatValue(UNIT_MOD_CAST_SPEED));
        return static_cast<float>(duration);
    }

    // The expected value of a hit: SpellDoneCritChance (percent) x the core's crit bonus on it
    float WithCrit(Player* bot, Unit* target, SpellInfo const* info, float damage, bool periodic)
    {
        if (damage <= 0.0f || info->HasAttribute(SPELL_ATTR2_CANT_CRIT))
            return damage;
        // AuraEffect::PeriodicTick: a tick crits only under an SPELL_AURA_ABILITY_PERIODIC_CRIT aura on the spell
        if (periodic && !bot->HasAuraTypeWithAffectMask(SPELL_AURA_ABILITY_PERIODIC_CRIT, info))
            return damage;

        WeaponAttackType const attack = info->DmgClass == SPELL_DAMAGE_CLASS_RANGED ? RANGED_ATTACK : BASE_ATTACK;
        float const chance =
            std::clamp(bot->SpellDoneCritChance(target, info, info->GetSchoolMask(), attack, periodic), 0.0f, 100.0f);
        float const crit =
            static_cast<float>(Unit::SpellCriticalDamageBonus(bot, info, static_cast<uint32>(damage), target));
        return damage + chance / 100.0f * std::max(0.0f, crit - damage);
    }

    float DirectDamage(Player* bot, Unit* target, SpellInfo const* info)
    {
        float damage = 0.0f;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect != SPELL_EFFECT_SCHOOL_DAMAGE)
                continue;
            int32 const base = std::max(effect.CalcValue(bot), 0);
            damage += static_cast<float>(
                bot->SpellDamageBonusDone(target, info, static_cast<uint32>(base), SPELL_DIRECT_DAMAGE,
                                          effect.EffectIndex));
        }
        return WithCrit(bot, target, info, damage, false);
    }
}

namespace SpellPayoff
{
    bool Read(Player* bot, Unit* target, uint32 spellId, Cast& out)
    {
        out = Cast();
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!bot || !info)
            return false;

        out.channel = info->IsChanneled();
        out.direct = DirectDamage(bot, target, info);

        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (!effect.Amplitude ||
                (effect.Effect != SPELL_EFFECT_APPLY_AURA && effect.Effect != SPELL_EFFECT_PERSISTENT_AREA_AURA))
                continue;

            float tick = 0.0f;
            // Devouring Plague is a periodic leech (Spell.dbc aura 53)
            if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE || effect.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH)
            {
                int32 const base = std::max(effect.CalcValue(bot), 0);
                tick = WithCrit(bot, target, info,
                                static_cast<float>(bot->SpellDamageBonusDone(target, info, static_cast<uint32>(base),
                                                                             DOT, effect.EffectIndex)),
                                true);
            }
            // Blizzard, Rain of Fire, Mind Sear: the channel triggers a damage spell every amplitude
            else if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL && effect.TriggerSpell)
            {
                if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                    tick = DirectDamage(bot, target, triggered);
            }
            // Mind Flay: every tick casts its damage spell with this effect's value as base points
            // (AuraEffect::HandlePeriodicTriggerSpellWithValueAuraTick)
            else if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE && effect.TriggerSpell)
            {
                if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                {
                    int32 const base = std::max(effect.CalcValue(bot), 0);
                    tick = WithCrit(bot, target, triggered,
                                    static_cast<float>(bot->SpellDamageBonusDone(
                                        target, triggered, static_cast<uint32>(base), SPELL_DIRECT_DAMAGE, EFFECT_0)),
                                    false);
                }
            }
            if (tick <= 0.0f)
                continue;

            int32 duration = out.channel ? static_cast<int32>(ChannelMs(bot, info)) : info->GetMaxDuration();
            if (!out.channel && duration > 0)
                bot->ApplySpellMod(info->Id, SPELLMOD_DURATION, duration);
            uint32 amplitude = effect.Amplitude;
            // a hasted channel ticks as much faster as it is shorter
            if (out.channel && info->GetDuration() > 0)
                amplitude = static_cast<uint32>(static_cast<float>(amplitude) * static_cast<float>(duration) /
                                                static_cast<float>(info->GetDuration()));
            if (duration <= 0 || !amplitude)
                continue;

            out.tick += tick;
            out.amplitudeMs = out.amplitudeMs ? std::min(out.amplitudeMs, amplitude) : amplitude;
            out.ticks = std::max(out.ticks, static_cast<uint32>(duration) / amplitude);
        }

        if (out.direct <= 0.0f && out.tick <= 0.0f)
            return false;

        out.castMs = out.channel ? 0.0f : static_cast<float>(info->CalcCastTime(bot));
        float const channelMs = out.channel ? ChannelMs(bot, info) : 0.0f;
        out.executeMs = std::max({out.castMs, channelMs, GlobalCooldownMs(bot, info)});
        return out.executeMs > 0.0f;
    }

    float FillerDps(Player* bot, Unit* target, uint32 fillerSpellId)
    {
        Cast filler;
        if (!Read(bot, target, fillerSpellId, filler))
            return 0.0f;
        // a channel's ticks land within it; a cast's own DoT part (Fireball) is not what the next filler repeats
        float const damage = filler.channel ? filler.Total() : filler.direct;
        return damage * static_cast<float>(IN_MILLISECONDS) / filler.executeMs;
    }

    DotVerdict DotPays(Player* bot, Unit* target, uint32 dotSpellId, uint32 fillerSpellId, float lifetimeSec,
                       float upfrontShare)
    {
        DotVerdict verdict;
        Cast dot;
        float const fillerDps = FillerDps(bot, target, fillerSpellId);
        if (fillerDps <= 0.0f || !Read(bot, target, dotSpellId, dot) || !dot.amplitudeMs)
            return verdict;

        // the ticks after it lands (the first one amplitude after the landing) and before the death
        float const afterLandMs = lifetimeSec * static_cast<float>(IN_MILLISECONDS) - dot.castMs;
        uint32 const landing =
            afterLandMs > 0.0f
                ? std::min(dot.ticks, static_cast<uint32>(afterLandMs / static_cast<float>(dot.amplitudeMs)))
                : 0;
        verdict.ticks = landing;
        verdict.dot = (afterLandMs >= 0.0f ? dot.direct + upfrontShare * dot.tick * static_cast<float>(dot.ticks)
                                           : 0.0f) +
                      dot.tick * static_cast<float>(landing);
        verdict.filler = fillerDps * dot.executeMs / static_cast<float>(IN_MILLISECONDS);
        verdict.pays = verdict.dot >= verdict.filler;
        return verdict;
    }

    uint32 AoeBreakEvenTargets(Player* bot, Unit* target, uint32 aoeSpellId, uint32 fillerSpellId,
                               float fillerScale)
    {
        Cast area;
        float const fillerDps = FillerDps(bot, target, fillerSpellId) * fillerScale;
        if (fillerDps <= 0.0f || !Read(bot, target, aoeSpellId, area))
            return 0;

        float const perTargetDps = area.Total() * static_cast<float>(IN_MILLISECONDS) / area.executeMs;
        if (perTargetDps <= 0.0f)
            return 0;
        return static_cast<uint32>(std::max(1.0f, std::ceil(fillerDps / perTargetDps)));
    }

    float AreaRadius(Player* bot, uint32 aoeSpellId)
    {
        SpellInfo const* info = aoeSpellId ? sSpellMgr->GetSpellInfo(aoeSpellId) : nullptr;
        if (!info)
            return 0.0f;

        float radius = 0.0f;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL && effect.TriggerSpell)
            {
                if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                    for (SpellEffectInfo const& inner : triggered->GetEffects())
                        if (inner.Effect == SPELL_EFFECT_SCHOOL_DAMAGE && inner.HasRadius())
                            radius = std::max(radius, inner.CalcRadius(bot));
                continue;
            }
            if ((effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE || effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA) &&
                effect.HasRadius())
                radius = std::max(radius, effect.CalcRadius(bot));
        }
        return radius;
    }
}
