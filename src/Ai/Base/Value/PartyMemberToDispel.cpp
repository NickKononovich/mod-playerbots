/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PartyMemberToDispel.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include <algorithm>

namespace
{
constexpr uint32 SPELL_ABOLISH_POISON = 2893;  // removes one poison every 3 s by itself
constexpr uint32 SPELL_ABOLISH_DISEASE = 552;
constexpr float URGENT_DOT_DAMAGE_LEFT_PCT = 3.0f;  // damage the DoT still deals, in % of max health
constexpr int32 URGENT_HEALING_TAKEN_PCT = -20;  // Mortal Strike style healing cuts
constexpr int32 URGENT_TANK_DAMAGE_TAKEN_PCT = 10;

enum class DispelUrgency : uint8
{
    NONE,
    HARMLESS,  // not worth a GCD: a slow on a ranged member, an attack speed cut on a caster
    NORMAL,
    URGENT
};

struct DispelVictimRole
{
    bool tank = false;
    bool healer = false;
    bool melee = true;  // pets fight in melee
    bool mana = false;
};

DispelVictimRole GetVictimRole(Unit* target)
{
    DispelVictimRole role;
    role.mana = target->getPowerType() == POWER_MANA;
    if (Player* player = target->ToPlayer())
    {
        role.tank = PlayerbotAI::IsTank(player);
        role.healer = PlayerbotAI::IsHeal(player);
        role.melee = PlayerbotAI::IsMelee(player);
    }

    return role;
}

int32 TicksLeft(AuraEffect const* effect)
{
    return std::max<int32>(1, effect->GetTotalTicks() - int32(effect->GetTickNumber()));
}

DispelUrgency GetEffectUrgency(AuraEffect const* effect, Unit* target, DispelVictimRole const& role)
{
    int32 const amount = effect->GetAmount();
    switch (effect->GetAuraType())
    {
        case SPELL_AURA_PERIODIC_DAMAGE:
        case SPELL_AURA_PERIODIC_LEECH:
        {
            float const damageLeft = float(amount) * TicksLeft(effect);
            return damageLeft * 100.0f >= URGENT_DOT_DAMAGE_LEFT_PCT * float(target->GetMaxHealth())
                       ? DispelUrgency::URGENT
                       : DispelUrgency::NORMAL;
        }
        case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
            return float(amount * TicksLeft(effect)) >= URGENT_DOT_DAMAGE_LEFT_PCT ? DispelUrgency::URGENT
                                                                                   : DispelUrgency::NORMAL;
        case SPELL_AURA_PERIODIC_MANA_LEECH:
        case SPELL_AURA_POWER_BURN:
            return role.mana ? DispelUrgency::URGENT : DispelUrgency::NORMAL;
        case SPELL_AURA_MOD_STUN:
        case SPELL_AURA_MOD_FEAR:
        case SPELL_AURA_MOD_CONFUSE:
        case SPELL_AURA_MOD_CHARM:
        case SPELL_AURA_MOD_POSSESS:
        case SPELL_AURA_MOD_PACIFY_SILENCE:
            return DispelUrgency::URGENT;
        case SPELL_AURA_MOD_SILENCE:
            return role.mana ? DispelUrgency::URGENT : DispelUrgency::NORMAL;
        case SPELL_AURA_MOD_POWER_COST_SCHOOL_PCT:  // Glutinous Poison: +25% spell costs
        case SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK:
        case SPELL_AURA_HASTE_SPELLS:
            return role.healer ? DispelUrgency::URGENT : DispelUrgency::NORMAL;
        case SPELL_AURA_MOD_HEALING_PCT:
            return amount <= URGENT_HEALING_TAKEN_PCT ? DispelUrgency::URGENT : DispelUrgency::NORMAL;
        case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
            return role.tank && amount >= URGENT_TANK_DAMAGE_TAKEN_PCT ? DispelUrgency::URGENT
                                                                        : DispelUrgency::NORMAL;
        case SPELL_AURA_MOD_DECREASE_SPEED:
            return role.tank ? DispelUrgency::URGENT : DispelUrgency::HARMLESS;
        case SPELL_AURA_MOD_ROOT:
            if (role.tank)
                return DispelUrgency::URGENT;
            return role.melee ? DispelUrgency::NORMAL : DispelUrgency::HARMLESS;
        case SPELL_AURA_MOD_ATTACKSPEED:
        case SPELL_AURA_MOD_MELEE_HASTE:
        case SPELL_AURA_MOD_MELEE_RANGED_HASTE:
            return role.melee || role.tank ? DispelUrgency::NORMAL : DispelUrgency::HARMLESS;
        default:
            return DispelUrgency::NORMAL;
    }
}

// Same aura filter as PlayerbotAI::HasAuraToDispel, graded by what the debuff does to this member.
DispelUrgency GetDispelUrgency(PlayerbotAI* botAI, Unit* target, uint32 dispelType)
{
    if (!target || !target->IsInWorld() || !target->IsAlive())
        return DispelUrgency::NONE;

    // An Abolish already ticking clears the poison or disease without another GCD.
    if ((dispelType == DISPEL_POISON && target->HasAura(SPELL_ABOLISH_POISON)) ||
        (dispelType == DISPEL_DISEASE && target->HasAura(SPELL_ABOLISH_DISEASE)))
        return DispelUrgency::NONE;

    Unit::VisibleAuraMap const* visibleAuras = target->GetVisibleAuras();
    if (!visibleAuras)
        return DispelUrgency::NONE;

    bool const isFriend = botAI->GetBot()->IsFriendlyTo(target);
    bool roleKnown = false;
    DispelVictimRole role;
    DispelUrgency best = DispelUrgency::NONE;
    for (auto const& [slot, application] : *visibleAuras)
    {
        if (!application)
            continue;

        Aura* aura = application->GetBase();
        if (!aura || aura->IsPassive() || aura->IsRemoved())
            continue;

        if (sPlayerbotAIConfig.DispelAuraDuration && aura->GetDuration() &&
            aura->GetDuration() < (int32)sPlayerbotAIConfig.DispelAuraDuration)
            continue;

        SpellInfo const* spellInfo = aura->GetSpellInfo();
        if (!spellInfo || spellInfo->IsPositive() == isFriend || !botAI->canDispel(spellInfo, dispelType))
            continue;

        if (!roleKnown)
        {
            role = GetVictimRole(target);
            roleKnown = true;
        }

        DispelUrgency auraUrgency = DispelUrgency::NONE;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (AuraEffect const* effect = aura->GetEffect(i))
                auraUrgency = std::max(auraUrgency, GetEffectUrgency(effect, target, role));
        }

        // A debuff without aura effects is still a debuff the encounter put there on purpose.
        if (auraUrgency == DispelUrgency::NONE)
            auraUrgency = DispelUrgency::NORMAL;

        best = std::max(best, auraUrgency);
        if (best == DispelUrgency::URGENT)
            break;
    }

    return best;
}

class PartyMemberToDispelPredicate : public FindPlayerPredicate, public PlayerbotAIAware
{
public:
    PartyMemberToDispelPredicate(PlayerbotAI* botAI, uint32 dispelType, DispelUrgency minUrgency)
        : FindPlayerPredicate(), PlayerbotAIAware(botAI), dispelType(dispelType), minUrgency(minUrgency)
    {
    }

    bool Check(Unit* unit) override { return GetDispelUrgency(botAI, unit, dispelType) >= minUrgency; }

private:
    uint32 dispelType;
    DispelUrgency minUrgency;
};
}

Unit* PartyMemberToDispel::Calculate()
{
    uint32 const dispelType = atoi(qualifier.c_str());

    if (Unit* urgent = AI_VALUE2(Unit*, "party member to dispel urgent", dispelType))
        return urgent;

    PartyMemberToDispelPredicate predicate(botAI, dispelType, DispelUrgency::NORMAL);
    return FindPartyMember(predicate);
}

Unit* PartyMemberToDispelUrgent::Calculate()
{
    uint32 const dispelType = atoi(qualifier.c_str());

    PartyMemberToDispelPredicate predicate(botAI, dispelType, DispelUrgency::URGENT);
    return FindPartyMember(predicate);
}
