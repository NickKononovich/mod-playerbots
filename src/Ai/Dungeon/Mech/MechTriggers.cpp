/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MechTriggers.h"
#include "AiObject.h"
#include "MechShared.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "RaidLeaderState.h"
#include "Timer.h"

bool SepethreaKiteFlameTrigger::IsActive()
{
    if (!MechanarFlames::GetSepethrea(bot))
        return false;

    // Every fixated bot kites, the tank included: a tank that stands still while holding the boss gets
    // the flame parked on it, laying fire under its feet (the kite action keeps it near the boss).
    return MechanarFlames::GetFixatingFlame(bot) != nullptr;
}

bool SepethreaAvoidFlameTrigger::IsActive()
{
    if (!MechanarFlames::GetSepethrea(bot))
        return false;

    if (MechanarFlames::GetFixatingFlame(bot))
        return false;

    if (!MechanarFlames::IsFlameNearCached(bot) && !MechanarFlames::BossFlameNearCached(bot))
        return false;

    return !MechanarFlames::HealerHoldsFire(bot);
}

bool SepethreaTrailTrigger::IsActive()
{
    if (!MechanarFlames::GetSepethrea(bot))
        return false;

    if (!MechanarFlames::InTrailDangerCached(bot))
        return false;

    return !MechanarFlames::HealerHoldsFire(bot);
}

bool SepethreaFocusBossTrigger::IsActive()
{
    return MechanarFlames::GetSepethrea(bot) != nullptr;
}

bool MechanarLiftTrigger::IsActive()
{
    MechanarLift::Leg leg;
    return MechanarLift::ActiveLeg(bot, leg);
}

bool CapacitusPolarityTrigger::IsActive() { return MechanarCapacitus::PolarityDanger(bot); }

bool CapacitusReflectiveShieldTrigger::IsActive()
{
    Unit* capacitus = MechanarCapacitus::GetCapacitus(bot);
    if (!capacitus)
        return false;

    if (!PlayerbotAI::IsHeal(bot) && MechanarCapacitus::CastingIntoMagicShield(botAI, capacitus))
        return true;

    return MechanarCapacitus::MeleeHoldsForDamageShield(bot, capacitus) && bot->GetVictim() == capacitus;
}

bool MechanarBridgePullTrigger::IsActive()
{
    // The bridge scan reads the grid: once a second is plenty for a pull that waits for a gathered group.
    constexpr uint32 checkMs = 1000;

    if (bot->GetMapId() != MechanarLift::MAP_ID || bot->IsInCombat() || !MechanarBridge::IsPullingTank(bot))
        return false;

    uint32 const now = getMSTime();
    if (!_checkMs || getMSTimeDiff(_checkMs, now) >= checkMs)
    {
        _checkMs = now;
        _due = MechanarBridge::BridgePullTarget(botAI) != nullptr;
    }
    if (!_due)
        return false;

    // The group's coordinator holds every new pull (ready check, buffs, a break): so does this one.
    if (RaidLeader::IsEnabled() && AI_VALUE(bool, "pull hold"))
        return false;

    PullStrategy* strategy = PullStrategy::Get(botAI);
    return strategy && !strategy->HasTarget() && !strategy->IsPullPendingToStart() && !strategy->HasPullStarted();
}
