/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MTMultipliers.h"
#include "AttackAction.h"
#include "GenericSpellActions.h"
#include "MTShared.h"
#include "Playerbots.h"
#include "TbcHazard.h"

using namespace ManaTombs;

float PandemoniusDarkShellMultiplier::GetValue(Action* action)
{
    bool const isAttack = dynamic_cast<AttackAction*>(action) != nullptr;
    bool const isHostileCast =
        dynamic_cast<CastSpellAction*>(action) && !dynamic_cast<CastHealingSpellAction*>(action);
    if (!isAttack && !isHostileCast)
        return 1.0f;

    Unit* pandemonius = TbcHazard::FindBoss(botAI, PANDEMONIUS_NAME);
    if (!HasDarkShell(pandemonius))
        return 1.0f;

    return action->GetTarget() == pandemonius ? 0.0f : 1.0f;
}
