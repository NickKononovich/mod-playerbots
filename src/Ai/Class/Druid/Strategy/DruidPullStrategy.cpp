/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DruidPullStrategy.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

std::string DruidPullStrategy::GetPullActionName() const
{
    std::string const pullActionName = PullStrategy::GetPullActionName();
    std::string const actionName =
        botAI->HasSpell("faerie fire (feral)") &&
            (botAI->HasStrategy("bear", BOT_STATE_COMBAT) || botAI->HasStrategy("cat", BOT_STATE_COMBAT))
            ? "faerie fire (feral)" : pullActionName;

    Unit* target = GetTarget();
    if (!target)
        return actionName;

    if (!botAI->CanCastSpell(actionName, target) && botAI->CanCastSpell("growl", target))
        return "growl";

    return actionName;
}

// [b32_tank_aoe] O(1): the strategy flag, two aura lookups and the spell id value (cached). Empty when the druid is no
// bear tank or already in a bear form; the action's own isUseful() / isPossible() decide the rest (mana, GCD).
std::string DruidPullStrategy::GetTankFormActionName() const
{
    Player* bot = botAI->GetBot();
    if (bot->IsInCombat() || !PlayerbotAI::IsTank(bot) || !botAI->HasStrategy("bear", BOT_STATE_COMBAT))
        return "";

    if (botAI->HasAnyAuraOf(bot, "dire bear form", "bear form", nullptr))
        return "";

    return botAI->HasSpell("dire bear form") ? "dire bear form" : "bear form";
}

std::string DruidPullStrategy::GetPreActionName() const
{
    if (GetPullActionName() == "faerie fire")
        return "";

    return PullStrategy::GetPreActionName();
}
