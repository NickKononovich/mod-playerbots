/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "StratTriggers.h"

#include "Playerbots.h"
#include "StratShared.h"

using namespace Stratholme;

bool StratAggroEdgeTrigger::IsActive()
{
    if (!InStratholme(bot) || !bot->IsAlive() || bot->IsInCombat() || PlayerbotAI::IsTank(bot))
        return false;

    Player* tank = GroupTank(bot);
    if (!tank || tank->isMoving() || bot->GetExactDist2d(tank) > HOLD_MAX_TANK_DISTANCE)
        return false;

    std::vector<IdleMob> const mobs = CollectIdleMobs(botAI);
    return !mobs.empty() &&
           SpotDrawsAggro(bot, tank, mobs, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
}

bool StratMemberSwarmedTrigger::IsActive()
{
    if (!InStratholme(bot) || !bot->IsAlive() || bot->IsInCombat() || !PlayerbotAI::IsTank(bot))
        return false;

    Player* victim = nullptr;
    return FindSwarmer(bot, victim) != nullptr;
}
