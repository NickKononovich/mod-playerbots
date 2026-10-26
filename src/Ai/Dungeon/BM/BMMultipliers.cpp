/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BMMultipliers.h"
#include "AttackAction.h"
#include "BMActions.h"
#include "BMShared.h"
#include "BMTriggers.h"
#include "ChooseTargetActions.h"
#include "GenericSpellActions.h"
#include "MovementActions.h"
#include "NonCombatActions.h"  // [b32_bm] DrinkAction, EatAction
#include "Playerbots.h"
#include "ReachTargetActions.h"
#include "StringFormat.h"

namespace
{
bool IsBlackMorassMove(Action* action)
{
    return dynamic_cast<BlackMorassHoldNearMedivhAction*>(action) ||
           dynamic_cast<BlackMorassDefendMedivhAction*>(action) ||
           dynamic_cast<BlackMorassKillRiftGuardianAction*>(action) ||
           dynamic_cast<BlackMorassFetchRiftGuardianAction*>(action) ||
           dynamic_cast<BlackMorassDragRiftGuardianAction*>(action) ||
           dynamic_cast<BlackMorassChronoBeaconFromSaatAction*>(action);
}
bool IsTargetChooser(Action* action)
{
    return dynamic_cast<DpsAssistAction*>(action) || dynamic_cast<DpsAoeAction*>(action) ||
           dynamic_cast<TankAssistAction*>(action) || dynamic_cast<AggressiveTargetAction*>(action) ||
           dynamic_cast<AttackRtiTargetAction*>(action) || dynamic_cast<AttackLeastHpTargetAction*>(action) ||
           dynamic_cast<AttackAnythingAction*>(action);
}
}  // namespace

float BlackMorassDefendFocusMultiplier::GetValue(Action* action)
{
    if (!IsTargetChooser(action) || !BMShared::IsEventInProgress(bot))
        return 1.0f;

    Unit* current = AI_VALUE(Unit*, "current target");
    if (!current || !current->IsAlive())
        return 1.0f;

    // The tank lets go once the channel broke: tank assist then takes the add off whoever it turned on.
    bool const hold = PlayerbotAI::IsTank(bot)
                          ? BMShared::IsCorruptingMedivh(BMShared::Medivh(bot), current)
                          : BMShared::IsThreat(botAI, current) || current == BMShared::GuardianAtMedivh(botAI);
    if (!hold)
        return 1.0f;

    BMShared::Log(bot, "bm guard", current->GetGUID().ToString() + ":focus:" + action->getName(),
                  Acore::StringFormat("stays on {} at Medivh instead of {}", current->GetName(), action->getName()));
    return 0.0f;
}

float BlackMorassStayAtMedivhMultiplier::GetValue(Action* action)
{
    // Walks only: an attack action (dps assist) may switch the bot off the far keeper.
    bool const reachSpell = dynamic_cast<CastReachTargetSpellAction*>(action) != nullptr;
    bool const walk = dynamic_cast<MovementAction*>(action) && !dynamic_cast<AttackAction*>(action);
    if ((!reachSpell && !walk) || IsBlackMorassMove(action))
        return 1.0f;

    if (PlayerbotAI::IsTank(bot) || !BMShared::IsEventInProgress(bot))
        return 1.0f;

    Unit* current = AI_VALUE(Unit*, "current target");
    if (!current || !BMShared::IsFarGuardian(bot, current) || !BMShared::HasLivingTank(bot))
        return 1.0f;

    BMShared::Log(bot, "bm guardian", current->GetGUID().ToString() + ":stay",
                  Acore::StringFormat("stays at Medivh instead of walking to {} ({})", current->GetName(),
                                      action->getName()));
    return 0.0f;
}

float BlackMorassDragRiftGuardianMultiplier::GetValue(Action* action)
{
    bool const reachSpell = dynamic_cast<CastReachTargetSpellAction*>(action) != nullptr;
    if ((!reachSpell && !dynamic_cast<MovementAction*>(action)) ||
        dynamic_cast<BlackMorassDragRiftGuardianAction*>(action))
        return 1.0f;

    if (!PlayerbotAI::IsTank(bot) || !BMShared::GuardianToDrag(botAI))
        return 1.0f;

    return 0.0f;
}

// [b32_bm]
float BlackMorassNoRestForRiftBossMultiplier::GetValue(Action* action)
{
    if ((!dynamic_cast<DrinkAction*>(action) && !dynamic_cast<EatAction*>(action)) ||
        !BMShared::IsEventInProgress(bot))
        return 1.0f;

    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "bm rift guardians"))
    {
        Unit* boss = botAI->GetUnit(guid);
        if (!boss || !boss->IsAlive() || !BMShared::IsBossKeeperEntry(boss->GetEntry()))
            continue;

        BMShared::Log(bot, "bm rest", guid.ToString(),
                      Acore::StringFormat("no {}: {} is up ({:.0f} yd from Medivh)", action->getName(),
                                          boss->GetName(),
                                          BMShared::Medivh(bot) ? boss->GetExactDist2d(BMShared::Medivh(bot)) : -1.0f),
                      30 * IN_MILLISECONDS);
        return 0.0f;
    }
    return 1.0f;
}

float BlackMorassTemporusReflectMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<CastSpellAction*>(action) || action->getName() == "spellsteal")
        return 1.0f;

    if (bot->getPowerType() != POWER_MANA || bot->getClass() == CLASS_HUNTER || PlayerbotAI::IsTank(bot) ||
        PlayerbotAI::IsHeal(bot) || bot->GetMapId() != BMShared::MAP_ID)
        return 1.0f;

    Unit* current = AI_VALUE(Unit*, "current target");
    if (!current || (current->GetEntry() != BMShared::NPC_TEMPORUS &&
                     current->GetEntry() != BMShared::NPC_INFINITE_TIMEREAVER) ||
        !current->HasAura(BMShared::SPELL_TEMPORUS_REFLECT))
        return 1.0f;

    BMShared::Log(bot, "bm reflect", current->GetGUID().ToString(),
                  Acore::StringFormat("holds {} while {} reflects spells", action->getName(), current->GetName()),
                  5 * IN_MILLISECONDS);
    return 0.0f;
}
