/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BFMultipliers.h"
#include "BFShared.h"
#include "Playerbots.h"
#include "TbcHazard.h"

using namespace BloodFurnace;

float KelidanBurningNovaHoldMultiplier::GetValue(Action* action)
{
    if (!TbcHazard::IsRepositionAction(action))
        return 1.0f;

    Unit* kelidan = TbcHazard::FindBoss(botAI, KELIDAN_NAME);
    if (!kelidan || !kelidan->HasAura(SPELL_BURNING_NOVA))
        return 1.0f;

    return TbcHazard::ShouldHoldOutside(botAI, kelidan->GetPosition(), BurningNovaDangerRadius(bot, kelidan))
        ? 0.0f : 1.0f;
}
