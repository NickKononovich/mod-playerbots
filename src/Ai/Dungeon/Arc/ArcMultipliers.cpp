/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArcMultipliers.h"
#include "ArcActions.h"
#include "ArcShared.h"
#include "Playerbots.h"
#include "TbcHazard.h"

using namespace Arcatraz;

float DalliahWhirlwindHoldMultiplier::GetValue(Action* action)
{
    if (!TbcHazard::IsRepositionAction(action))
        return 1.0f;

    Unit* dalliah = TbcHazard::FindBoss(botAI, DALLIAH_NAME);
    if (!dalliah || !dalliah->HasAura(SPELL_DALLIAH_WHIRLWIND))
        return 1.0f;

    return TbcHazard::ShouldHoldOutside(botAI, dalliah->GetPosition(), WhirlwindDangerRadius(bot, dalliah))
        ? 0.0f : 1.0f;
}

float AbyssalMeteorStackMultiplier::GetValue(Action* action)
{
    if (!TbcHazard::IsRepositionAction(action) || dynamic_cast<AbyssalMeteorStackAction*>(action))
        return 1.0f;

    Position point;
    if (!FindMeteorPoint(botAI, point) || bot->GetExactDist2d(point) <= METEOR_STACK_DISTANCE)
        return 1.0f;

    return 0.0f;
}
