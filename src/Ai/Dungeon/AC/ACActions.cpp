/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ACActions.h"
#include "ACTriggers.h"
#include "EncounterHelpers.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "TbcHazard.h"

using namespace EncounterHelpers;

// Tank will position Shirrak at the specified coordinates, up the stairs
bool ShirrakTankPositionBossAction::Execute(Event /*event*/)
{
    Unit* shirrak = AI_VALUE2(Unit*, "find target", "shirrak the dead watcher");
    if (!shirrak)
        return false;

    if (bot->GetVictim() != shirrak)
        return Attack(shirrak);

    if (shirrak->GetVictim() != bot || !bot->IsWithinMeleeRange(shirrak) ||
        bot->GetHealthPct() < 30.0f)
    {
        return false;
    }

    constexpr float arrivalDist = 3.0f;
    float moveX;
    float moveY;
    bool backwards;
    if (!GetStepToPosition(
            bot, SHIRRAK_TANK_POSITION, arrivalDist, shirrak, moveX, moveY, backwards))
    {
        return false;
    }

    return MoveTo(
        bot->GetMapId(), moveX, moveY, bot->GetPositionZ(), false, false, false, false,
        MovementPriority::MOVEMENT_COMBAT, true, backwards);
}

bool ShirrakFleeFocusFireAction::Execute(Event /*event*/)
{
    Creature* flare = bot->FindNearestCreature(NPC_FOCUS_FIRE, FLARE_SEARCH_RADIUS);
    if (!flare)
        return false;

    float currentDistance = bot->GetExactDist2d(flare);
    constexpr float safeDistance = 12.0f;
    if (currentDistance >= safeDistance)
        return false;

    bot->CastStop();
    float distanceToMove = safeDistance - currentDistance;
    return MoveAway(flare, distanceToMove);
}

// Ranged back off along the line from Shirrak to the bot until they stand at their band (see ACTriggers.h).
// The old fixed spot down the stairs was out of casting range, so the reach-spell action pulled them straight
// back in and they shuttled to and fro with 3-4 Inhibit Magic stacks.
bool ShirrakRangedKeepDistanceAction::Execute(Event /*event*/)
{
    Unit* shirrak = AI_VALUE2(Unit*, "find target", "shirrak the dead watcher");
    if (!shirrak)
        return false;

    float const edgeToCenter = shirrak->GetCombatReach() + bot->GetCombatReach();
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    // Healers try the 1-stack ring first, but never leave the tank out of heal range for it.
    if (PlayerbotAI::IsHeal(bot))
    {
        Player* tank = GetGroupMainTank(bot);
        constexpr float healSlack = 2.0f;
        if (TbcHazard::FindRingPoint(bot, shirrak->GetPosition(), SHIRRAK_HEALER_DISTANCE + edgeToCenter, x, y, z) &&
            (!tank || tank->GetExactDist2d(x, y) < sPlayerbotAIConfig.HealDistance - healSlack))
        {
            return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT,
                          true);
        }

        if (bot->GetDistance(shirrak) >= sPlayerbotAIConfig.SpellDistance - SHIRRAK_DISTANCE_TOLERANCE)
            return false;
    }

    // [b35_lock] A damage dealer with 36 yd spells holds the 1-stack band (ShirrakRangedDpsDistance, ACTriggers.h).
    float const ring = PlayerbotAI::IsHeal(bot) ? sPlayerbotAIConfig.SpellDistance : ShirrakRangedDpsDistance(botAI);
    if (!TbcHazard::FindRingPoint(bot, shirrak->GetPosition(), ring + edgeToCenter, x, y, z))
        return false;

    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT, true);
}
