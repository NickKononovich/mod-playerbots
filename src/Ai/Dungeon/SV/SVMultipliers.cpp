/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SVMultipliers.h"
#include "GenericSpellActions.h"
#include "Playerbots.h"
#include "SVShared.h"
#include "TbcHazard.h"

using namespace Steamvault;

float KalithreshSpellReflectionMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<CastSpellAction*>(action) || dynamic_cast<CastHealingSpellAction*>(action))
        return 1.0f;

    Unit* kalithresh = TbcHazard::FindBoss(botAI, KALITHRESH_NAME);
    if (!kalithresh || !kalithresh->HasAura(SPELL_KALITHRESH_REFLECTION))
        return 1.0f;

    return action->GetTarget() == kalithresh ? 0.0f : 1.0f;
}
