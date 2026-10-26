/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PoSMultipliers.h"
#include "ChooseTargetActions.h"
#include "DungeonHoldFire.h"
#include "GenericSpellActions.h"
#include "MovementActions.h"
#include "PoSActions.h"
#include "PoSTriggers.h"
#include "TbcHazard.h"
#include "Timer.h"

float IckAndKrickMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ick");
    if (!boss)
        return 1.0f;

    // Allow the IckAndKrickAction to run
    if (dynamic_cast<IckAndKrickAction*>(action))
        return 1.0f;

    if (PoSShared::IsPoisonNova(boss) &&
        bot->GetExactDist2d(boss) < PoSShared::PoisonNovaDangerRadius(bot) + TbcHazard::EXIT_MARGIN)
        return 0.0f;  // Cancel all other actions when we need to handle Poison Nova

    if (bot->GetExactDist2d(boss) < 15.0f && bot->HasAura(SPELL_PURSUIT) && !PlayerbotAI::IsTank(bot))
        return 0.0f;  // Cancel all other actions when we need to handle Pursuit

    if (!PlayerbotAI::IsHeal(bot) && boss->HasUnitState(UNIT_STATE_CASTING) && (boss->FindCurrentSpellBySpellId(SPELL_EXPLOSIVE_BARRAGE_ICK) || boss->FindCurrentSpellBySpellId(SPELL_EXPLOSIVE_BARRAGE_KRICK)))
        return 0.0f;  // Cancel all other actions when we need to handle Explosive Barrage

    return 1.0f;
}

float GarfrostMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<MovementAction*>(action) || dynamic_cast<PoSHideFromPermafrostAction*>(action) ||
        dynamic_cast<PoSDodgeSaroniteAction*>(action))
        return 1.0f;

    return IsHiding() ? 0.0f : 1.0f;
}

bool GarfrostMultiplier::IsHiding()
{
    constexpr uint32 HIDE_CHECK_MS = 300;  // the rock test is a grid search: not once per queued action

    uint32 const now = getMSTime();
    if (now >= hideCheckAt)
    {
        hiding = PoSShared::ShouldHideFromPermafrost(bot);
        hideCheckAt = now + HIDE_CHECK_MS;
    }

    return hiding;
}

float PoSOverlordsBrandMultiplier::GetValue(Action* action)
{
    if (!bot->HasAura(PoSIds::SPELL_OVERLORDS_BRAND) || dynamic_cast<PoSBrandHoldFireAction*>(action) ||
        dynamic_cast<TyrannusAction*>(action) || dynamic_cast<IckAndKrickAction*>(action))
        return 1.0f;

    if (dynamic_cast<CastHealingSpellAction*>(action))
        return 0.0f;

    if (!DungeonHoldFire::IsDamageAction(bot, action, nullptr))
        return 1.0f;

    // [b33_pos] the copies go to Tyrannus's victim: a tank he is not hitting holds fire like everyone else, and the
    // tank he hits keeps single-target threat only (each AoE hit is copied onto the tank itself once per mob)
    if (PoSShared::BrandDamageHitsAlly(bot))
        return 0.0f;

    return action->getThreatType() == Action::ActionThreatType::Aoe ? 0.0f : 1.0f;
}
