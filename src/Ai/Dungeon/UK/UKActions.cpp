/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UKActions.h"
#include "MgTShared.h"
#include "Playerbots.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

bool AttackFrostTombAction::isUseful() { return !PlayerbotAI::IsHeal(bot); }
bool AttackFrostTombAction::Execute(Event /*event*/)
{
    Unit* frostTomb = nullptr;

    // Target is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");

    for (auto& target : targets)
    {
        Unit* unit = botAI->GetUnit(target);
        if (unit && unit->GetEntry() == NPC_FROST_TOMB)
        {
            frostTomb = unit;
            break;
        }
    }
    if (!frostTomb || AI_VALUE(Unit*, "current target") == frostTomb)
    {
        return false;
    }
    return Attack(frostTomb);
}

bool AttackSkarvaldAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "skarvald the constructor");
    if (!boss || boss->GetEntry() != UKIds::NPC_SKARVALD || !boss->isTargetableForAttack())
        return false;

    if (AI_VALUE(Unit*, "current target") == boss)
        return false;

    return Attack(boss);
}

bool DalronnInterruptAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "dalronn the controller");
    if (!boss || boss->GetEntry() != UKIds::NPC_DALRONN || !boss->IsNonMeleeSpellCast(false))
        return false;

    // Urgency 3: plain interrupts only, no stun or other control.
    constexpr uint8 interruptOnly = 3;
    return !MagistersTerrace::CastInterrupt(botAI, boss, interruptOnly).empty();
}

bool IngvarAvoidSmashAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ingvar the plunderer");
    if (!UKIds::IsIngvarSmashing(bot, boss) ||
        !UKIds::InSmashDanger(bot, boss, bot->GetPositionX(), bot->GetPositionY()))
        return false;

    // He cannot turn during the cast, so behind and beside him relative to his current facing stays safe.
    constexpr std::array<float, 7> offsets = {
        static_cast<float>(M_PI),
        static_cast<float>(M_PI) * 5.0f / 6.0f, -static_cast<float>(M_PI) * 5.0f / 6.0f,
        static_cast<float>(M_PI) * 2.0f / 3.0f, -static_cast<float>(M_PI) * 2.0f / 3.0f,
        static_cast<float>(M_PI) / 2.0f, -static_cast<float>(M_PI) / 2.0f,
    };
    constexpr float extra = 1.0f;
    float const radius = UKIds::SmashBoundaryRadius(bot, boss) + extra;

    std::vector<std::pair<float, std::pair<float, float>>> candidates;  // distance from the bot, (x, y)
    for (float offset : offsets)
    {
        float const angle = boss->GetOrientation() + offset;
        float const x = boss->GetPositionX() + std::cos(angle) * radius;
        float const y = boss->GetPositionY() + std::sin(angle) * radius;
        candidates.push_back({bot->GetExactDist2d(x, y), {x, y}});
    }
    std::sort(candidates.begin(), candidates.end());

    for (auto const& candidate : candidates)
    {
        float x = candidate.second.first;
        float y = candidate.second.second;
        float z = bot->GetPositionZ();
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), x, y, z))
            continue;

        // A wall may cut the line short; the shortened spot only counts when it is out of the Smash.
        if (UKIds::InSmashDanger(bot, boss, x, y))
            continue;

        if (bot->IsNonMeleeSpellCast(false))
            bot->CastStop();

        return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED, true);
    }
    return false;
}

bool IngvarDodgeSmashAction::isUseful() { return !AI_VALUE2(bool, "behind", "current target"); }
bool IngvarDodgeSmashAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ingvar the plunderer");
    if (!boss) { return false; }

    float distance = bot->GetExactDist2d(boss->GetPosition());
    // Through him to the far side of his boundary: Smash also lands on everyone within MIN_MELEE_REACH plus both
    // bodies of his centre whatever the angle, so the old 2 yd past his midpoint was still inside it.
    float distanceExtra = UKIds::SmashBoundaryRadius(bot, boss);
    return Move(bot->GetAngle(boss), distance + distanceExtra);
}

bool IngvarSmashReturnAction::isUseful() { return AI_VALUE2(bool, "behind", "current target"); }
bool IngvarSmashReturnAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ingvar the plunderer");
    if (!boss) { return false; }

    float distance = bot->GetExactDist2d(boss->GetPosition());
    return Move(bot->GetAngle(boss), distance + bot->GetMeleeReach());
}
