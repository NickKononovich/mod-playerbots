/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InvalidTargetValue.h"
#include "AttackersValue.h"
#include "Group.h"
#include "KillOrder.h"
#include "LeaderOrders.h"  // [lead23]
#include "Playerbots.h"
#include "PvpValues.h"
#include "Unit.h"

#include <algorithm>

bool InvalidTargetValue::Calculate()
{
    Unit* target = AI_VALUE(Unit*, qualifier);
    if (IsUnavailableCombatTarget(bot, target))
        return true;

    // [b44_st] a charmed member of the group / team is dropped as a target, whoever chose it (AttackersValue)
    if (AttackersValue::IsCharmedAlly(target, bot))
        return true;

    // [lead23] a led member keeps the leader's Attack target (dead, evading, immune or friendly: dropped above)
    if (target && qualifier == "current target" && Leader::ExecutorMember(bot))
    {
        Leader::MemberEvents events;
        if (Leader::Events::Get(bot->GetGUID(), events) && events.attack == Leader::AttackEvent::Attack &&
            events.target == target->GetGUID())
            return !bot->IsValidAttackTarget(target) || target->IsImmunedToDamage(SPELL_SCHOOL_MASK_ALL) ||
                   (target->ToCreature() && target->ToCreature()->IsInEvadeMode());
    }

    Unit* enemy = AI_VALUE(Unit*, "enemy player target");
    if (target && enemy && target == enemy && target->IsAlive())
        return false;

    if (target && qualifier == "current target")
    {
        // Drop a fleeing critter we got into a fight with, unless it was picked on purpose (focus or skull mark).
        if (AttackersValue::IsIgnoredCritter(target, bot))
        {
            GuidVector const& prioritized = AI_VALUE(GuidVector, "prioritized targets");
            Group* group = bot->GetGroup();
            bool const marked = group && group->GetTargetIcon(7) == target->GetGUID();
            if (!marked && std::find(prioritized.begin(), prioritized.end(), target->GetGUID()) == prioritized.end())
                return true;
        }

        // 5-man dungeons: a mob that fights someone from deep in the room (a caster left at range by the tank's
        // line-of-sight pull, a mob chasing a member there) is dropped until it comes; the tank takes it, and the
        // dps do not walk into the next pack. A mob fighting nobody is kept: a "kill first" add on its way to the
        // eggs is chosen on purpose, and the dps pickers never choose one anyway.
        if (bot->IsInCombat() && target->IsAlive() && target->GetVictim() &&
            !KillOrder::IsInGroupFight(bot, target, true))
            return true;

        return target->GetMapId() != bot->GetMapId() || target->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) ||
               target->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) || target->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE_2) ||
               !target->IsVisible() || !target->IsAlive() || target->IsPolymorphed() ||
               (target->IsCharmed() && target->IsPlayer() &&  // only a mind-controlled teammate is off limits
                target->ToPlayer()->GetTeamId(true) == bot->GetTeamId(true)) ||
               target->HasFearAura() || target->HasUnitState(UNIT_STATE_ISOLATED) || target->IsFriendlyTo(bot) ||
               !AttackersValue::IsValidTarget(target, bot);
    }

    return !target;
}
