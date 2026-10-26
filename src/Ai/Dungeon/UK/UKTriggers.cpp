/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UKTriggers.h"
#include "AiObjectContext.h"
#include "Playerbots.h"

bool KelesethFrostTombTrigger::IsActive()
{
    GuidVector members = AI_VALUE(GuidVector, "group members");
    for (auto& member : members)
    {
        Unit* unit = botAI->GetUnit(member);
        if (unit && unit->HasAura(SPELL_FROST_TOMB))
        {
            return true;
        }
    }
    return false;
}

bool UKIds::IsIngvarSmashing(Player* bot, Unit* boss)
{
    return boss && boss->HasUnitState(UNIT_STATE_CASTING) &&
           (boss->FindCurrentSpellBySpellId(SPELL_SMASH) || boss->FindCurrentSpellBySpellId(SPELL_DARK_SMASH));
}

float UKIds::SmashBoundaryRadius(Player* bot, Unit* boss)
{
    return SMASH_ALWAYS_HIT_REACH + boss->GetCombatReach() + bot->GetCombatReach() + SMASH_MARGIN;
}

bool UKIds::InSmashDanger(Player* bot, Unit* boss, float x, float y)
{
    float const dist = boss->GetExactDist2d(x, y);
    if (dist < SmashBoundaryRadius(bot, boss))
        return true;

    Position const point(x, y, boss->GetPositionZ());
    float const coneReach = SMASH_CONE_RADIUS + boss->GetCombatReach() + bot->GetCombatReach() + SMASH_MARGIN;
    return dist < coneReach && boss->HasInArc(SMASH_SAFE_ARC, &point);
}

bool SkarvaldPriorityTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return false;

    // The ghost carries the same name but is unselectable, so it never passes isTargetableForAttack.
    Unit* boss = AI_VALUE2(Unit*, "find target", "skarvald the constructor");
    return boss && boss->GetEntry() == UKIds::NPC_SKARVALD && boss->IsAlive() && boss->isTargetableForAttack();
}

bool DalronnInterruptTrigger::IsActive()
{
    if (PlayerbotAI::IsHeal(bot))
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "dalronn the controller");
    return boss && boss->GetEntry() == UKIds::NPC_DALRONN && boss->IsAlive() && boss->IsNonMeleeSpellCast(false) &&
           bot->GetExactDist2d(boss) <= UKIds::DALRONN_INTERRUPT_RANGE;
}

bool IngvarSmashDangerTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot))
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "ingvar the plunderer");
    return UKIds::IsIngvarSmashing(bot, boss) &&
           UKIds::InSmashDanger(bot, boss, bot->GetPositionX(), bot->GetPositionY());
}

bool IngvarDreadfulRoarTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ingvar the plunderer");
    if (!boss) { return false; }

    if (boss->FindCurrentSpellBySpellId(SPELL_DREADFUL_ROAR))
    {
        return true;
    }
    return false;
}

bool IngvarSmashTankTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ingvar the plunderer");
    if (!boss || !PlayerbotAI::IsTank(bot)) { return false; }

    if (boss->FindCurrentSpellBySpellId(SPELL_SMASH) ||
        boss->FindCurrentSpellBySpellId(SPELL_DARK_SMASH))
        {
            return true;
        }
    return false;
}

bool IngvarSmashTankReturnTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ingvar the plunderer");
    // if (!boss || !PlayerbotAI::IsTank(bot) || boss->HasUnitState(UNIT_STATE_CASTING))
    // Ignore casting state as Ingvar will sometimes chain-cast a roar after a smash..
    // We don't want this to prevent our tank from repositioning properly.
    if (!boss || !PlayerbotAI::IsTank(bot)) { return false; }

    return true;
}

bool NotBehindIngvarTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ingvar the plunderer");
    if (!boss || PlayerbotAI::IsTank(bot) || !PlayerbotAI::IsMelee(bot)) { return false; }

    // Melee damage dealers stand behind him (this used to fire when they already were). Ranged stay where they are:
    // "set behind" would pull them into melee range.
    if (AI_VALUE(Unit*, "current target") != boss)
        return false;

    return !AI_VALUE2(bool, "behind", "current target");
}
