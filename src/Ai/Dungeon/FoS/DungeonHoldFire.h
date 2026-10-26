/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONHOLDFIRE_H
#define PLAYERBOTS_DUNGEONHOLDFIRE_H

#include "AiObjectContext.h"
#include "AttackAction.h"
#include "CharmInfo.h"
#include "GenericSpellActions.h"
#include "Pet.h"
#include "PetDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Spell.h"
#include "SpellInfo.h"

#include <array>
#include <string>

// Shared by the ToC / Forge of Souls / Pit of Saron strategies: mechanics that reflect, mirror or copy what a
// player does (Reflective Shield, Mirrored Soul, Overlord's Brand), and priority interrupts of boss casts.
namespace DungeonHoldFire
{
// A heal, buff, cure or resurrection: it never damages anyone.
inline bool IsSupportAction(Action* action)
{
    return dynamic_cast<CastHealingSpellAction*>(action) || dynamic_cast<CastBuffSpellAction*>(action) ||
           dynamic_cast<CastCureSpellAction*>(action) || dynamic_cast<ResurrectPartyMemberAction*>(action);
}

// An action that deals damage. With a target set, only damage that can land on it: an attack or spell aimed at it,
// or any AoE. Without a target, any attack, AoE or spell aimed at a non-friendly unit. Callers whitelist their own
// encounter actions first (several derive from AttackAction for its movement helpers).
inline bool IsDamageAction(Player* bot, Action* action, Unit const* target)
{
    bool const attack = dynamic_cast<AttackAction*>(action) != nullptr;
    if (!attack && (!dynamic_cast<CastSpellAction*>(action) || IsSupportAction(action)))
        return false;

    if (action->getThreatType() == Action::ActionThreatType::Aoe)
        return true;

    Unit* actionTarget = action->GetTarget();
    if (target)
        return actionTarget == target;

    return attack || (actionTarget && actionTarget != bot && !actionTarget->IsFriendlyTo(bot));
}

// The bot or its pet is attacking target, targets it, or casts a harmful spell at it.
inline bool IsDamaging(PlayerbotAI* botAI, Unit* target)
{
    if (!target)
        return false;

    Player* bot = botAI->GetBot();
    if (bot->GetVictim() == target)
        return true;

    if (botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get() == target)
        return true;

    if (Pet* pet = bot->GetPet(); pet && pet->IsAlive() && pet->GetVictim() == target)
        return true;

    constexpr std::array<CurrentSpellTypes, 3> SPELL_SLOTS = {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL,
                                                              CURRENT_AUTOREPEAT_SPELL};
    for (CurrentSpellTypes const slot : SPELL_SLOTS)
    {
        Spell* spell = bot->GetCurrentSpell(slot);
        if (spell && spell->m_spellInfo && !spell->m_spellInfo->IsPositive() &&
            spell->m_targets.GetUnitTarget() == target)
            return true;
    }

    return false;
}

// The bot or its pet attacks anything, or the bot casts a harmful spell.
inline bool IsDamagingAnything(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->GetVictim())
        return true;

    if (Pet* pet = bot->GetPet(); pet && pet->IsAlive() && pet->GetVictim())
        return true;

    constexpr std::array<CurrentSpellTypes, 3> SPELL_SLOTS = {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL,
                                                              CURRENT_AUTOREPEAT_SPELL};
    for (CurrentSpellTypes const slot : SPELL_SLOTS)
    {
        Spell* spell = bot->GetCurrentSpell(slot);
        if (spell && spell->m_spellInfo && !spell->m_spellInfo->IsPositive())
            return true;
    }

    return false;
}

// Stops auto-attack, own harmful casts and the pet: on target only, or on anything when target is nullptr.
// Returns true when something was stopped.
inline bool StopDamage(PlayerbotAI* botAI, Unit* target)
{
    Player* bot = botAI->GetBot();
    bool stopped = false;

    if (Unit* victim = bot->GetVictim(); victim && (!target || victim == target))
    {
        bot->AttackStop();
        stopped = true;
    }

    constexpr std::array<CurrentSpellTypes, 3> SPELL_SLOTS = {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL,
                                                              CURRENT_AUTOREPEAT_SPELL};
    for (CurrentSpellTypes const slot : SPELL_SLOTS)
    {
        Spell* spell = bot->GetCurrentSpell(slot);
        if (!spell || !spell->m_spellInfo || spell->m_spellInfo->IsPositive())
            continue;

        if (target && spell->m_targets.GetUnitTarget() != target)
            continue;

        bot->InterruptSpell(slot);
        stopped = true;
    }

    if (target)
    {
        Value<Unit*>* current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target");
        if (current && current->Get() == target)
        {
            current->Set(nullptr);
            bot->SetTarget(ObjectGuid::Empty);
            stopped = true;
        }
    }

    Pet* pet = bot->GetPet();
    if (!pet || !pet->IsAlive())
        return stopped;

    Unit* petVictim = pet->GetVictim();
    if (!petVictim || (target && petVictim != target))
        return stopped;

    pet->AttackStop();
    if (CharmInfo* charmInfo = pet->GetCharmInfo())
    {
        charmInfo->SetIsCommandAttack(false);
        charmInfo->SetIsCommandFollow(true);
    }
    pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
    return true;
}

inline bool IsHealCast(Spell const* spell)
{
    SpellInfo const* info = spell ? spell->m_spellInfo : nullptr;
    return info && (info->HasEffect(SPELL_EFFECT_HEAL) || info->HasAura(SPELL_AURA_PERIODIC_HEAL));
}

inline bool IsCastingHeal(Player* bot)
{
    return IsHealCast(bot->GetCurrentSpell(CURRENT_GENERIC_SPELL)) ||
           IsHealCast(bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL));
}

// Interrupts an own heal cast in progress. Returns true when one was interrupted.
inline bool StopHealing(Player* bot)
{
    bool stopped = false;
    constexpr std::array<CurrentSpellTypes, 2> SPELL_SLOTS = {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL};
    for (CurrentSpellTypes const slot : SPELL_SLOTS)
    {
        if (!IsHealCast(bot->GetCurrentSpell(slot)))
            continue;

        bot->InterruptSpell(slot);
        stopped = true;
    }

    return stopped;
}

// True while caster is casting (or channelling) one of the given spells and the cast can still be interrupted.
template <size_t N>
inline bool IsCastingAny(Unit* caster, std::array<uint32, N> const& spellIds)
{
    if (!caster || !caster->IsAlive() || !caster->HasUnitState(UNIT_STATE_CASTING))
        return false;

    for (uint32 const spellId : spellIds)
        if (caster->FindCurrentSpellBySpellId(spellId))
            return true;

    return false;
}

// Casts the first known dedicated interrupt that is ready and in range. Returns true on a cast.
inline bool CastInterrupt(PlayerbotAI* botAI, Unit* caster)
{
    static std::array<char const*, 9> const INTERRUPTS = {"kick", "pummel", "shield bash", "wind shear",
                                                          "mind freeze", "counterspell", "spell lock",
                                                          "silencing shot", "strangulate"};
    if (!caster || !caster->IsNonMeleeSpellCast(true))
        return false;

    for (char const* name : INTERRUPTS)
    {
        std::string const spell = name;
        if (botAI->CanCastSpell(spell, caster) && botAI->CastSpell(spell, caster))
            return true;
    }

    return false;
}

// True when the bot knows any of the dedicated interrupts (cheap gate for interrupt triggers).
inline bool HasInterrupt(PlayerbotAI* botAI)
{
    static std::array<char const*, 9> const INTERRUPTS = {"kick", "pummel", "shield bash", "wind shear",
                                                          "mind freeze", "counterspell", "spell lock",
                                                          "silencing shot", "strangulate"};
    for (char const* name : INTERRUPTS)
        if (botAI->GetAiObjectContext()->GetValue<uint32>("spell id", name)->Get())
            return true;

    return false;
}
}  // namespace DungeonHoldFire

#endif
