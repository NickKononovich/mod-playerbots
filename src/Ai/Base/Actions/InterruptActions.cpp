/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InterruptActions.h"
#include "Event.h"
#include "InterruptValues.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

Unit* InterruptAssignedCastAction::GetCaster()
{
    InterruptAssignment const assignment = AI_VALUE(InterruptAssignment, "interrupt assignment");
    Unit* caster = botAI->GetUnit(assignment.caster);
    if (!caster || !caster->IsAlive() || !caster->IsInWorld() || caster->GetMap() != bot->GetMap())
        return nullptr;

    return caster;
}

SpellInfo const* InterruptAssignedCastAction::GetAssignedSpell()
{
    InterruptAssignment const assignment = AI_VALUE(InterruptAssignment, "interrupt assignment");
    if (assignment.spell.empty())
        return nullptr;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", assignment.spell);
    return spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
}

bool InterruptAssignedCastAction::isUseful()
{
    Unit* caster = GetCaster();
    if (!caster || !caster->IsNonMeleeSpellCast(false, false, true))
        return false;

    // the cached assignment may lag a hand-off to the next member by a tick: the registry has the last word
    InterruptAssignment const assignment = AI_VALUE(InterruptAssignment, "interrupt assignment");
    return InterruptCoordination::Allows(botAI, caster, assignment.spell);
}

bool InterruptAssignedCastAction::isPossible()
{
    Unit* caster = GetCaster();
    SpellInfo const* spellInfo = GetAssignedSpell();
    if (!caster || !spellInfo || bot->HasUnitState(UNIT_STATE_LOST_CONTROL) || bot->HasSpellCooldown(spellInfo->Id))
        return false;

    // An own channel is stopped in Execute; CanCastSpell would refuse while it runs.
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return true;

    Unit* target = spellInfo->NeedsExplicitUnitTarget() ? caster : bot;
    return botAI->CanCastSpell(spellInfo->Id, target, true);
}

bool InterruptAssignedCastAction::Execute(Event /*event*/)
{
    Unit* caster = GetCaster();
    SpellInfo const* spellInfo = GetAssignedSpell();
    if (!caster || !spellInfo)
        return false;

    // Stop-cast to kick, as a player would; a healer is never assigned while it casts.
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) && !PlayerbotAI::IsHeal(bot))
        bot->InterruptSpell(CURRENT_CHANNELED_SPELL);

    if (Spell* own = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        bot->InterruptSpell(own->GetCurrentContainer());

    Unit* target = spellInfo->NeedsExplicitUnitTarget() ? caster : bot;
    InterruptAssignment const assignment = AI_VALUE(InterruptAssignment, "interrupt assignment");
    if (!botAI->CastSpell(assignment.spell, target))
        return false;

    InterruptCoordination::MarkFired(botAI, caster);
    // the assignment is spent; recompute it at once instead of re-firing on a cached answer
    if (UntypedValue* value = context->GetUntypedValue("interrupt assignment"))
        value->Reset();
    return true;
}
