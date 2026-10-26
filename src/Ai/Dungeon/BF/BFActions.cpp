/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BFActions.h"
#include "BFShared.h"
#include "Playerbots.h"

using namespace BloodFurnace;

bool KelidanLeaveBurningNovaAction::Execute(Event /*event*/)
{
    Unit* kelidan = TbcHazard::FindBoss(botAI, KELIDAN_NAME);
    if (!kelidan || !kelidan->HasAura(SPELL_BURNING_NOVA))
        return false;

    return LeaveCircle(kelidan->GetPosition(), BurningNovaDangerRadius(bot, kelidan), true);
}

bool BroggokBackOffAction::Execute(Event /*event*/)
{
    Unit* orc = FindLooseCellOrc(botAI);
    if (!orc)
        return false;

    // Out to just past the trigger radius; the tank's taunts and threat do the rest.
    return LeaveCircle(orc->GetPosition(), BROGGOK_LOOSE_ADD_RADIUS, false);
}
