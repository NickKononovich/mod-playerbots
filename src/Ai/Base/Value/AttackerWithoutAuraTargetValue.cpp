/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AttackerWithoutAuraTargetValue.h"
#include "KillOrder.h"
#include "Playerbots.h"
#include "RtiValue.h"
#include "Strategy.h"
#include "TargetValue.h"

Unit* AttackerWithoutAuraTargetValue::Calculate()
{
    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    GuidSet const dynamicExclusions = GatherStrategyTargetExclusions(botAI, TargetValueExclusionType::DebuffTarget);
    // Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    uint32 max_health = 0;
    Unit* result = nullptr;
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || dynamicExclusions.find(guid) != dynamicExclusions.end())
            continue;

        // a DoT tick breaks the crowd control; [rot_b31_cc] also the one its crowd control icon waits for (a DoT put
        // on the mob before the Polymorph lands broke it again at the next tick)
        if (IsCrowdControlled(unit) || HasCrowdControlIcon(bot, unit))
            continue;

        if (!bot->IsWithinCombatRange(unit, botAI->GetRange(range)))
            continue;

        // 5-man dungeons: no DoT on a mob deep in the room the group does not fight yet (KillOrder::IsInGroupFight)
        if (!KillOrder::IsInGroupFight(bot, unit))
            continue;

        if (unit->GetHealth() < max_health)
            continue;

        if (!botAI->HasAura(qualifier, unit, false, true))
        {
            max_health = unit->GetHealth();
            result = unit;
        }
    }

    return result;
}

Unit* MeleeAttackerWithoutAuraTargetValue::Calculate()
{
    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    GuidSet const dynamicExclusions = GatherStrategyTargetExclusions(botAI, TargetValueExclusionType::DebuffTarget);
    // Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    uint32 max_health = 0;
    Unit* result = nullptr;
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || dynamicExclusions.find(guid) != dynamicExclusions.end())
            continue;

        // a DoT tick breaks the crowd control; [rot_b31_cc] also the one its crowd control icon waits for (a DoT put
        // on the mob before the Polymorph lands broke it again at the next tick)
        if (IsCrowdControlled(unit) || HasCrowdControlIcon(bot, unit))
            continue;

        if (!bot->IsWithinMeleeRange(unit))
            continue;

        if (checkArc && !bot->HasInArc(CAST_ANGLE_IN_FRONT, unit))
            continue;

        if (unit->GetHealth() < max_health)
            continue;

        if (!botAI->HasAura(qualifier, unit, false, true))
        {
            max_health = unit->GetHealth();
            result = unit;
        }
    }

    return result;
}
