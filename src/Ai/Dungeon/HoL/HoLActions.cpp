/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HoLActions.h"
#include "Playerbots.h"
#include <algorithm>

bool BjarngrimTargetAction::Execute(Event /*event*/)
{
    Unit* target = nullptr;

    // Target is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    GuidVector npcs = AI_VALUE(GuidVector, "possible targets");

    for (auto& npc : npcs)
    {
        Unit* unit = botAI->GetUnit(npc);
        if (unit && unit->GetEntry() == NPC_STORMFORGED_LIEUTENANT)
        {
            target = unit;
            break;
        }
    }

    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    // There are two, we don't want to ping-pong between them if we're attacking one already
    if (target && currentTarget && currentTarget->GetEntry() == NPC_STORMFORGED_LIEUTENANT)
    {
        return false;
    }

    if (AI_VALUE(Unit*, "current target") == target)
    {
        return false;
    }

    return Attack(target);
}

bool AvoidWhirlwindAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "general bjarngrim");
    if (!boss) { return false; }

    float distance = bot->GetExactDist2d(boss->GetPosition());
    float radius = 8.0f;
    float distanceExtra = 2.0f;

    if (distance < radius + distanceExtra)
    {
        return MoveAway(boss, radius + distanceExtra - distance);
    }

    return false;
}

bool VolkhanTargetAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "volkhan");
    if (!boss || AI_VALUE(Unit*, "current target") == boss)
    {
        return false;
    }

    return Attack(boss);
}

bool AvoidBrittleGolemAction::Execute(Event /*event*/)
{
    Unit* golem = HoLShared::BrittleGolemThreat(bot, AI_VALUE2(Unit*, "find target", "volkhan"));
    if (!golem)
        return false;

    // Small steps so the next tick can steer away from a second golem.
    constexpr float extra = 2.0f;
    return MoveAway(golem, HoLIds::SHATTER_SAFE_DISTANCE + extra - bot->GetExactDist2d(golem));
}

bool StaticOverloadSpreadAction::Execute(Event /*event*/)
{
    float radius = 8.0f;
    float distanceExtra = 2.0f;

    GuidVector members = AI_VALUE(GuidVector, "group members");
    // The carrier moves off the group too: the knock-back 53337 (8 yd, 2.3-2.7k) lands on everybody around it.
    if (bot->HasAura(SPELL_STATIC_OVERLOAD))
    {
        Unit* nearest = nullptr;
        for (ObjectGuid const& member : members)
        {
            Unit* unit = botAI->GetUnit(member);
            if (unit && unit != bot && unit->IsAlive() && bot->GetExactDist2d(unit) < radius + distanceExtra &&
                (!nearest || bot->GetExactDist2d(unit) < bot->GetExactDist2d(nearest)))
                nearest = unit;
        }
        if (nearest)
            return MoveAway(nearest, radius + distanceExtra - bot->GetExactDist2d(nearest));
    }
    for (auto& member : members)
    {
        if (bot->GetGUID() == member)
        {
            continue;
        }

        Unit* unit = botAI->GetUnit(member);
        if (unit && unit->HasAura(SPELL_STATIC_OVERLOAD)
            && bot->GetExactDist2d(unit) < radius)
        {
            return MoveAway(unit, radius + distanceExtra - bot->GetExactDist2d(unit));
        }
    }
    return false;
}

bool BallLightningSpreadAction::Execute(Event /*event*/)
{
    float radius = 6.0f;
    float distanceExtra = 1.0f;

    GuidVector members = AI_VALUE(GuidVector, "group members");
    for (auto& member : members)
    {
        if (bot->GetGUID() == member)
        {
            continue;
        }
        Unit* unit = botAI->GetUnit(member);
        if (unit && bot->GetExactDist2d(unit) < radius + distanceExtra)
        {
            return MoveAway(unit, radius + distanceExtra - bot->GetExactDist2d(unit));
        }
    }
    return false;
}

bool IonarTankPositionAction::isUseful() { return bot->GetExactDist2d(IONAR_TANK_POSITION) > 10.0f; }
bool IonarTankPositionAction::Execute(Event /*event*/)
{
    return MoveTo(bot->GetMapId(), IONAR_TANK_POSITION.GetPositionX(), IONAR_TANK_POSITION.GetPositionY(), IONAR_TANK_POSITION.GetPositionZ(),
                  false, false, false, true, MovementPriority::MOVEMENT_COMBAT);
}

namespace
{
// Minimum hunter range is 5, but hunters at 5 drift into melee between shots; 6.5 keeps them shooting.
constexpr float LOKEN_STACK_HUNTER = 6.5f;
constexpr float LOKEN_STACK_OTHER = 3.0f;
constexpr float LOKEN_STACK_TOLERANCE = 1.5f;

float LokenStackDistance(Player* bot)
{
    return bot->getClass() == CLASS_HUNTER ? LOKEN_STACK_HUNTER : LOKEN_STACK_OTHER;
}
}  // namespace

// Pulsing Shockwave hits everybody for 100 x distance every 2 s. The bot's distance to Loken himself counts here:
// the "current target" of a ranged bot is not always Loken, which left the action useless at range.
bool LokenStackAction::isUseful()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "loken");
    if (!boss || HoLShared::IsLightningNova(bot, boss))
        return false;

    return bot->GetExactDist2d(boss) > LokenStackDistance(bot) + LOKEN_STACK_TOLERANCE;
}

bool LokenStackAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "loken");
    if (!boss)
        return false;

    // UNIT_STATE_CASTING is no test here: Loken channels the Pulsing Shockwave aura (59414) the whole fight, so
    // Spell::finish never clears that state after his first Lightning Nova and the old check froze every ranged.
    if (HoLShared::IsLightningNova(bot, boss))
        return false;

    float const maxMovement = 10.0f;
    float const distance = bot->GetExactDist2d(boss) - LokenStackDistance(bot);
    if (distance <= 0.0f)
        return false;

    return Move(bot->GetAngle(boss), std::min(distance, maxMovement));
}

bool AvoidLightningNovaAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "loken");
    if (!boss) { return false; }

    float distance = bot->GetExactDist2d(boss->GetPosition());
    float radius = 20.0f;
    float distanceExtra = 2.0f;

    if (distance < radius + distanceExtra)
    {
        return MoveAway(boss, radius + distanceExtra - distance);
    }

    return false;
}
