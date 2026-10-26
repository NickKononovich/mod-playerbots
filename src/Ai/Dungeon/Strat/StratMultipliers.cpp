/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "StratMultipliers.h"

#include <string>
#include <unordered_set>

#include "Action.h"
#include "Formations.h"
#include "Playerbots.h"
#include "StratShared.h"
#include "TargetValue.h"
#include "Timer.h"

using namespace Stratholme;

namespace
{
constexpr uint32 STRAT_AOE_HOLD_MS = 3000;
// The hold is sampled once per this many milliseconds (every action of a tick asks).
constexpr uint32 STRAT_AOE_SAMPLE_MS = 200;

// Area spells and the area totems: each puts threat on every mob around it before the tank has any lead.
std::unordered_set<std::string> const AOE_ACTIONS = {
    "magma totem", "fire nova", "call of the elements", "chain lightning", "blizzard", "flamestrike",
    "cone of cold", "dragon's breath", "blast wave", "volley", "multi-shot", "explosive trap", "hurricane",
    "starfall", "typhoon", "swipe (cat)", "rain of fire", "hellfire", "seed of corruption", "holy nova",
    "mind sear", "fan of knives", "death and decay", "blood boil", "pestilence", "whirlwind", "cleave",
    "bladestorm",
};
}  // namespace

float StratHoldBehindTankMultiplier::GetValue(Action* action)
{
    if (!action || action->getName() != "follow" || !InStratholme(bot) || bot->IsInCombat() ||
        PlayerbotAI::IsTank(bot))
        return 1.0f;

    Player* tank = GroupTank(bot);
    if (!tank || bot->GetExactDist2d(tank) > HOLD_MAX_TANK_DISTANCE)
        return 1.0f;

    Formation* formation = AI_VALUE(Formation*, "formation");
    if (!formation || !formation->GetTargetName().empty())
        return 1.0f;

    WorldLocation const loc = formation->GetLocation();
    if (Formation::IsNullLocation(loc) || loc.GetMapId() != bot->GetMapId())
        return 1.0f;

    std::vector<IdleMob> const mobs = CollectIdleMobs(botAI);
    if (mobs.empty())
        return 1.0f;

    bool const hereSafe =
        !SpotDrawsAggro(bot, tank, mobs, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    bool const thereSafe =
        !SpotDrawsAggro(bot, tank, mobs, loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ());
    return hereSafe && !thereSafe ? 0.0f : 1.0f;
}

float StratAoeOpenerHoldMultiplier::GetValue(Action* action)
{
    if (!action || !InStratholme(bot) || PlayerbotAI::IsTank(bot) ||
        AOE_ACTIONS.find(action->getName()) == AOE_ACTIONS.end())
        return 1.0f;

    uint32 const now = getMSTime();
    if (_checkedMs && getMSTimeDiff(_checkedMs, now) < STRAT_AOE_SAMPLE_MS)
        return _hold ? 0.0f : 1.0f;

    _checkedMs = now;
    Player* tank = GroupTank(bot);
    if (!tank || !bot->IsInCombat() || !tank->IsInCombat())
    {
        // without a tank in the fight nobody would ever take the lead: nothing to wait for
        _heldSinceMs = 0;
        _hold = tank != nullptr && !tank->IsInCombat() && bot->IsInCombat();
        return _hold ? 0.0f : 1.0f;
    }

    bool tankHoldsAll = true;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || IsCrowdControlled(unit))
            continue;

        if (unit->GetVictim() != tank)
        {
            tankHoldsAll = false;
            break;
        }
    }

    if (!tankHoldsAll)
        _heldSinceMs = 0;
    else if (!_heldSinceMs)
        _heldSinceMs = now;

    _hold = !tankHoldsAll || getMSTimeDiff(_heldSinceMs, now) < STRAT_AOE_HOLD_MS;
    return _hold ? 0.0f : 1.0f;
}
