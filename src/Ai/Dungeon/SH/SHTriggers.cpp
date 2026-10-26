/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SHTriggers.h"
#include "Playerbots.h"
#include "SHActions.h"
#include "SHShared.h"
#include "TbcHazard.h"

using namespace ShatteredHalls;

bool KargathBladeDanceTrigger::IsActive()
{
    Unit* kargath = TbcHazard::FindBoss(botAI, KARGATH_NAME);
    if (!IsBladeDancing(kargath))
        return false;

    return TbcHazard::IsInside(bot, kargath->GetPosition(), BladeDanceDangerRadius(bot, kargath));
}

bool KargathRangedRingTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || (!PlayerbotAI::IsHeal(bot) && !PlayerbotAI::IsRanged(bot)))
        return false;

    Unit* kargath = TbcHazard::FindBoss(botAI, KARGATH_NAME);
    if (!kargath || !kargath->IsAlive() || !kargath->IsInCombat() || IsBladeDancing(kargath))
        return false;

    return bot->GetExactDist2d(kargath) < KargathRingFor(bot) - KARGATH_RING_SLACK;
}
