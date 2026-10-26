/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GDMultipliers.h"
#include "Action.h"
#include "ChooseTargetActions.h"
#include "DungeonTactics.h"
#include "GDActions.h"
#include "GDTriggers.h"
#include "GenericSpellActions.h"
#include "MovementActions.h"
#include "Playerbots.h"
#include "ReachTargetActions.h"

float SladranMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "slad'ran");
    if (!boss) { return 1.0f; }

    // [b35_gd] A snake or a Snake Wrap is hit without the threat rules: the snakes are on the healer anyway (a snake
    // that turns to the damage dealer is a win) and a Wrap holds a member stunned. gd-h 20261026-024524-gd-h-5H-27:
    // the snake killer waited out "[fresh add hold]" (1.5 s) on every new Constrictor on the healer, "[threat-hold]"
    // held the mage's frost nova and the cat's swipe on a Wrap (699 % / 684 %), and 7 Constrictors piled on the healer.
    // ThreatMultiplier reads (and resets) "neglect threat" for every action, as the Naxxramas multipliers set it.
    if (!PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot))
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (target && target->IsAlive() && (GundrakSladran::IsAdd(target) || target->GetEntry() == NPC_SNAKE_WRAP))
            context->GetValue<bool>("neglect threat")->Set(true);
    }

    if (boss->FindCurrentSpellBySpellId(SPELL_POISON_NOVA))
    {
        if (dynamic_cast<AvoidPoisonNovaAction*>(action)) { return 1.0f; }

        if (dynamic_cast<MovementAction*>(action))
        {
            return 0.0f;
        }
    }

    if (dynamic_cast<FleeAction*>(action) ||
        dynamic_cast<RunAwayAction*>(action) ||
        dynamic_cast<MoveRandomAction*>(action) ||
        dynamic_cast<MoveFromGroupAction*>(action))
    {
        return 0.0f;
    }

    if (PlayerbotAI::IsTank(bot))
    {
        if (dynamic_cast<TankAssistAction*>(action))
        {
            Unit* tankTarget = AI_VALUE(Unit*, "tank target");
            if (tankTarget && GundrakSladran::IsAdd(tankTarget) &&
                bot->GetExactDist2d(tankTarget) > GundrakSladran::TANK_PICKUP_YD)
            {
                return 0.0f;
            }

            // A Snake Wrap is the damage dealers' to break (attack snake wrap); wave 6 the tank hit one for 17 s
            // while Slad'ran and the snakes were loose.
            if (tankTarget && tankTarget->GetEntry() == NPC_SNAKE_WRAP)
                return 0.0f;
        }

        if (dynamic_cast<ReachTargetAction*>(action))
        {
            Unit* currentTarget = AI_VALUE(Unit*, "current target");
            if (currentTarget && GundrakSladran::IsAdd(currentTarget) &&
                bot->GetExactDist2d(currentTarget) > GundrakSladran::TANK_PICKUP_YD)
            {
                return 0.0f;
            }
        }

        return 1.0f;
    }

    if (dynamic_cast<CombatFormationMoveAction*>(action) &&
        !dynamic_cast<SetBehindTargetAction*>(action))
    {
        return 0.0f;
    }

    if (dynamic_cast<DpsAssistAction*>(action) && GundrakSladran::GetAssignedSnakeWrap(botAI))
    {
        return 0.0f;
    }

    // "dungeon kill first" (62) traded the target every tick with "attack snake wrap" (64) and between the snakes on
    // the healer (the closest one changes as they move): wave 6, 20261025-000027, a mage and a shaman cast 0 in 15 s
    // ("[idle diag] ... dungeon kill first:19 attack snake wrap:18"). Freeing the wrapped member comes first, a snake
    // on the healer is kept until dead, and only the snake killer leaves the boss for them.
    if (dynamic_cast<DungeonKillFirstAction*>(action))
    {
        if (GundrakSladran::GetAssignedSnakeWrap(botAI))
            return 0.0f;

        if (GundrakSladran::IsOnHealer(AI_VALUE(Unit*, "current target")))
            return 0.0f;

        if (boss->IsInCombat() && !GundrakSladran::IsSnakeKiller(botAI))
            return 0.0f;

        return 1.0f;
    }

    // The snake killer keeps the snake it is on until dead ("dps assist" would go back to the boss); the others are
    // sent back to the boss by "dps assist" (their "dps target" leaves the snakes out, GDStrategy.cpp).
    if (dynamic_cast<DpsAssistAction*>(action) || dynamic_cast<DpsAoeAction*>(action))
    {
        Unit* currentTarget = AI_VALUE(Unit*, "current target");
        if (currentTarget && currentTarget->IsAlive() && GundrakSladran::IsAdd(currentTarget) &&
            (!boss->IsInCombat() || GundrakSladran::IsSnakeKiller(botAI)))
            return 0.0f;
    }

    return 1.0f;
}

float GaldarahMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "gal'darah");
    if (!boss) { return 1.0f; }

    if (boss->HasAura(SPELL_WHIRLING_SLASH))
        {
            if (dynamic_cast<MovementAction*>(action) && !dynamic_cast<AvoidWhirlingSlashAction*>(action))
            {
                return 0.0f;
            }
        }
    return 1.0f;
}
