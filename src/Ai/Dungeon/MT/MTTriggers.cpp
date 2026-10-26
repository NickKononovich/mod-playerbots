/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MTTriggers.h"
#include "MTShared.h"
#include "Playerbots.h"
#include "Spell.h"
#include "TbcHazard.h"

using namespace ManaTombs;

bool ShaffarBeaconUpTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return false;

    if (!TbcHazard::FindBoss(botAI, SHAFFAR_NAME))
        return false;

    return TbcHazard::FindAttackerByEntry(botAI, { NPC_ETHEREAL_BEACON, NPC_ETHEREAL_APPRENTICE }) != nullptr;
}

bool PandemoniusDarkShellTrigger::IsActive()
{
    Unit* pandemonius = TbcHazard::FindBoss(botAI, PANDEMONIUS_NAME);
    if (!HasDarkShell(pandemonius))
        return false;

    if (bot->GetVictim() == pandemonius)
        return true;

    Spell* spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!spell)
        spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);

    return spell && spell->m_targets.GetUnitTarget() == pandemonius;
}
