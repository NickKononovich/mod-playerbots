/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SLMultipliers.h"
#include "Playerbots.h"
#include "SLShared.h"
#include "ShamanActions.h"
#include "TbcHazard.h"

using namespace ShadowLabyrinth;

float MurmurSonicBoomHoldMultiplier::GetValue(Action* action)
{
    if (!TbcHazard::IsRepositionAction(action))
        return 1.0f;

    Unit* murmur = TbcHazard::FindBoss(botAI, MURMUR_NAME);
    if (!IsCastingSonicBoom(murmur))
        return 1.0f;

    return TbcHazard::ShouldHoldOutside(botAI, murmur->GetPosition(), SonicBoomDangerRadius(bot)) ? 0.0f : 1.0f;
}

float HellmawTremorTotemMultiplier::GetValue(Action* action)
{
    if (bot->getClass() != CLASS_SHAMAN || botAI->GetState() == BOT_STATE_NON_COMBAT)
        return 1.0f;

    if (!dynamic_cast<CastStrengthOfEarthTotemAction*>(action) &&
        !dynamic_cast<CastStoneskinTotemAction*>(action) &&
        !dynamic_cast<CastStoneclawTotemAction*>(action) &&
        !dynamic_cast<CastEarthbindTotemAction*>(action))
    {
        return 1.0f;
    }

    return TbcHazard::FindBoss(botAI, HELLMAW_NAME) ? 0.0f : 1.0f;
}
