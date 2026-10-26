/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MechMultipliers.h"
#include "ChooseTargetActions.h"
#include "DungeonHoldFire.h"
#include "FollowActions.h"
#include "GenericSpellActions.h"
#include "MechActions.h"
#include "MechShared.h"
#include "MovementActions.h"
#include "Playerbots.h"
#include "ReachTargetActions.h"
#include "TbcHazard.h"

float SepethreaKiteFlameMultiplier::GetValue(Action* action)
{
    if (dynamic_cast<SepethreaKiteFlameAction*>(action) ||
        dynamic_cast<SepethreaAvoidTrailAction*>(action) ||
        dynamic_cast<SepethreaAvoidFlameAction*>(action) ||
        dynamic_cast<SepethreaFocusBossAction*>(action))
        return 1.0f;

    bool const isMover =
        dynamic_cast<MovementAction*>(action) || dynamic_cast<CastReachTargetSpellAction*>(action);
    if (!isMover)
        return 1.0f;

    // [enc-mech-healer] A healer does not walk to Sepethrea for spell range: at the pull she stands on her dais where
    // the Raging Flames appear (mech-n 2026-10-25 115805 12:18:18: Rldruaf "reach spell" 34 yd from 287.7,45.5 to
    // 319.8,21.4 into the flames, dead 6 s later). It heals from where it stands or by reach party member to heal.
    if (PlayerbotAI::IsHeal(bot) && dynamic_cast<ReachSpellAction*>(action) && MechanarFlames::GetSepethrea(bot))
        return 0.0f;

    if (MechanarFlames::GetFixatingFlame(bot))
        return 0.0f;

    if (!MechanarFlames::HealerHoldsFire(bot) &&
        (MechanarFlames::InTrailDangerCached(bot) || MechanarFlames::IsFlameNearCached(bot)))
        return 0.0f;

    return 1.0f;
}

float MechanarLiftHoldMultiplier::GetValue(Action* action)
{
    if (dynamic_cast<MechanarRideLiftAction*>(action))
        return 1.0f;

    bool const isMover =
        dynamic_cast<MovementAction*>(action) || dynamic_cast<CastReachTargetSpellAction*>(action);
    if (!isMover)
        return 1.0f;

    MechanarLift::Leg leg;
    return MechanarLift::ActiveLeg(bot, leg) ? 0.0f : 1.0f;
}

float SepethreaFocusBossMultiplier::GetValue(Action* action)
{
    bool const suppressed =
        dynamic_cast<DpsAssistAction*>(action) ||
        dynamic_cast<TankAssistAction*>(action) ||
        dynamic_cast<CastDebuffSpellOnAttackerAction*>(action) ||
        (action->getThreatType() == Action::ActionThreatType::Aoe && PlayerbotAI::IsDps(bot) &&
         !dynamic_cast<CastHealingSpellAction*>(action));
    if (!suppressed)
        return 1.0f;

    if (!MechanarFlames::GetSepethrea(bot))
        return 1.0f;

    return 0.0f;
}

float CapacitusMultiplier::GetValue(Action* action)
{
    if (dynamic_cast<CapacitusPolarityAction*>(action) || dynamic_cast<CapacitusHoldFireAction*>(action))
        return 1.0f;

    Unit* capacitus = MechanarCapacitus::GetCapacitus(bot);
    if (!capacitus)
        return 1.0f;

    if (!PlayerbotAI::IsHeal(bot) && MechanarCapacitus::ReflectedSpell(botAI, action, capacitus))
        return 0.0f;

    if (MechanarCapacitus::MeleeHoldsForDamageShield(bot, capacitus) &&
        DungeonHoldFire::IsDamageAction(bot, action, capacitus))
        return 0.0f;

    bool const mover = TbcHazard::IsRepositionAction(action) || dynamic_cast<FollowAction*>(action) ||
                       dynamic_cast<CombatFormationMoveAction*>(action);
    if (mover && MechanarCapacitus::OppositeOfTank(bot))
        return 0.0f;

    return 1.0f;
}
