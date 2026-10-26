/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DamageSpikeTriggers.h"

#include "Playerbots.h"

bool DamageSpikeTrigger::IsActive()
{
    // Sample outside combat too, so the window is already filled when the pull lands.
    float const loss = AI_VALUE(float, "recent health loss");
    return bot->IsInCombat() && loss >= sPlayerbotAIConfig.TankDamageSpikePct;
}

bool TankPressureTrigger::IsActive()
{
    float const threshold = sPlayerbotAIConfig.TankCooldownHealthPct;
    if (threshold <= 0.0f || !bot->IsInCombat() || !PlayerbotAI::IsTank(bot))
        return false;

    float const health = bot->GetHealthPct();
    if (health >= threshold)
        return false;

    return health < threshold * LOW_HEALTH_SHARE ||
           AI_VALUE(float, "recent health loss") >= sPlayerbotAIConfig.TankCooldownLossPct;
}

bool TankDamageSpikeTrigger::IsActive() { return AI_VALUE(Unit*, "tank damage spike target") != nullptr; }
