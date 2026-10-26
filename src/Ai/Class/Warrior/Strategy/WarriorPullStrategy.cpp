/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WarriorPullStrategy.h"
#include "Player.h"
#include "PlayerbotAI.h"

std::string WarriorPullStrategy::GetPullActionName() const
{
    Unit* target = GetTarget();
    if (!target)
        return PullStrategy::GetPullActionName();

    if (botAI->CanCastSpell("heroic throw", target))
        return "heroic throw";

    // Shoot or Throw needs a ranged weapon; without one, Taunt (30 yd) still brings a lone mob, where the
    // plain "shoot" failed the pull and the tank walked into the pack.
    Player* bot = botAI->GetBot();
    if (bot && !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED) && botAI->CanCastSpell("taunt", target))
        return "taunt";

    return PullStrategy::GetPullActionName();
}
