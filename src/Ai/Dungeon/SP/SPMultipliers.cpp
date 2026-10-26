/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SPMultipliers.h"
#include "Playerbots.h"
#include "SPShared.h"
#include "TbcHazard.h"

using namespace SlavePens;

float MennuNovaTotemHoldMultiplier::GetValue(Action* action)
{
    if (!TbcHazard::IsRepositionAction(action))
        return 1.0f;

    if (!TbcHazard::FindBoss(botAI, MENNU_NAME))
        return 1.0f;

    float const danger = NovaDangerRadius(bot);
    float const holdRadius = danger + 1.0f + TbcHazard::HOLD_BUFFER;
    Creature* totem = bot->FindNearestCreature(NPC_CORRUPTED_NOVA_TOTEM, holdRadius);
    if (!totem)
        return 1.0f;

    return TbcHazard::ShouldHoldOutside(botAI, totem->GetPosition(), danger) ? 0.0f : 1.0f;
}
