/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BFTriggers.h"
#include "BFShared.h"
#include "Playerbots.h"
#include "TbcHazard.h"

using namespace BloodFurnace;

bool KelidanBurningNovaTrigger::IsActive()
{
    Unit* kelidan = TbcHazard::FindBoss(botAI, KELIDAN_NAME);
    if (!kelidan || !kelidan->HasAura(SPELL_BURNING_NOVA))
        return false;

    return TbcHazard::IsInside(bot, kelidan->GetPosition(), BurningNovaDangerRadius(bot, kelidan));
}

bool BroggokLooseAddTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsMelee(bot))
        return false;

    return FindLooseCellOrc(botAI) != nullptr;
}
