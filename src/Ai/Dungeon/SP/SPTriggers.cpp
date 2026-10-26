/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SPTriggers.h"
#include "Playerbots.h"
#include "SPShared.h"
#include "TbcHazard.h"

using namespace SlavePens;

bool MennuNovaTotemTrigger::IsActive()
{
    if (!TbcHazard::FindBoss(botAI, MENNU_NAME))
        return false;

    float const danger = NovaDangerRadius(bot);
    Creature* totem = bot->FindNearestCreature(NPC_CORRUPTED_NOVA_TOTEM, danger + 1.0f);
    return totem && TbcHazard::IsInside(bot, totem->GetPosition(), danger);
}

bool MennuTotemsUpTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return false;

    return TbcHazard::FindBoss(botAI, MENNU_NAME) != nullptr;
}
