/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PoSActions.h"
#include "DungeonHoldFire.h"
#include "DungeonInteract.h"
#include "GameObject.h"
#include "Playerbots.h"
#include "RaidBotDuty.h"
#include "TbcHazard.h"
#include "DungeonHazard.h"  // [b33_pos]

#include <cmath>
#include <vector>

#include <string>

bool IckAndKrickAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "Ick");
    if (!boss)
        return false;

    std::vector<Unit*> orbs;
    bool orb = false;

    // First gather all orbs
    GuidVector npcs1 = AI_VALUE(GuidVector, "nearest hostile npcs");
    for (auto& npc : npcs1)
    {
        Unit* unit = botAI->GetUnit(npc);
        if (unit && unit->IsAlive() && unit->HasAura(SPELL_EXPLODING_ORB_VISUAL))
        {
            orb = true;
            break;
        }
    }

    bool pursuit = bot->HasAura(SPELL_PURSUIT) || (!PlayerbotAI::IsTank(bot) && boss->HasUnitState(UNIT_STATE_CASTING) && boss->FindCurrentSpellBySpellId(SPELL_PURSUIT));
    bool poisonNova = PoSShared::IsPoisonNova(boss);
    bool explosiveBarrage = orb || (boss->HasUnitState(UNIT_STATE_CASTING) && (boss->FindCurrentSpellBySpellId(SPELL_EXPLOSIVE_BARRAGE_ICK) || boss->FindCurrentSpellBySpellId(SPELL_EXPLOSIVE_BARRAGE_KRICK)));
    bool isTank = PlayerbotAI::IsTank(bot);

    if (pursuit && Pursuit(pursuit, boss))
        return true;

    if (poisonNova && PoisonNova(poisonNova, boss))
        return true;

    if (explosiveBarrage && ExplosiveBarrage(explosiveBarrage, boss))
        return true;

    if (isTank && !pursuit && !poisonNova && !explosiveBarrage)
    {
        if (TankPosition(boss))
            return true;
    }

    return false;
}

bool IckAndKrickAction::TankPosition(Unit* boss)
{
    if (botAI->HasAggro(boss))
    {
        float distance = bot->GetExactDist2d(ICKANDKRICK_TANK_POSITION.GetPositionX(), ICKANDKRICK_TANK_POSITION.GetPositionY());
        if (distance > 7.0f)
        {
            // Calculate direction vector
            float dirX = ICKANDKRICK_TANK_POSITION.GetPositionX() - bot->GetPositionX();
            float dirY = ICKANDKRICK_TANK_POSITION.GetPositionY() - bot->GetPositionY();
            float length = sqrt(dirX * dirX + dirY * dirY);
            dirX /= length;
            dirY /= length;

            // Move in increments of 3.0f
            float moveX = bot->GetPositionX() + dirX * 3.0f;
            float moveY = bot->GetPositionY() + dirY * 3.0f;

            return MoveTo(bot->GetMapId(), moveX, moveY, bot->GetPositionZ(), false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
        }
    }

    return false;
}

bool IckAndKrickAction::Pursuit(bool pursuit, Unit* boss)
{
    // Only execute this action when pursuit is active and for non-tank players
    if (!pursuit || PlayerbotAI::IsTank(bot))
        return false;

    // Get the tank position as a reference point
    Position tankPosition = ICKANDKRICK_TANK_POSITION;

    // Calculate distance from boss
    float distanceToBoss = bot->GetExactDist2d(boss);

    // If boss is close, move in a circular pattern
    if (distanceToBoss < 20.0f)
    {
        // Current bot position
        float x = bot->GetPositionX();
        float y = bot->GetPositionY();

        // Calculate vector from tank to bot
        float dx = x - tankPosition.GetPositionX();
        float dy = y - tankPosition.GetPositionY();

        // Normalize the vector
        float distance = std::sqrt(dx * dx + dy * dy);
        if (distance > 0)
        {
            dx /= distance;
            dy /= distance;
        }

        // Rotate the vector by 90 degrees to create circular movement
        // (x,y) rotated 90 degrees becomes (-y,x)
        float rotatedX = -dy;
        float rotatedY = dx;

        // Create a target position along the circular path
        // We want to maintain a specific radius from the tank
        float targetRadius = 20.0f;  // 20 feet radius circle around tank

        Position targetPos;
        targetPos.Relocate(tankPosition.GetPositionX() + rotatedX * targetRadius,
                           tankPosition.GetPositionY() + rotatedY * targetRadius, bot->GetPositionZ());

        // Move to the calculated circular position
        botAI->Reset();
        return MoveTo(bot->GetMapId(), targetPos.GetPositionX(), targetPos.GetPositionY(), bot->GetPositionZ(),
                      false, false, false, false, MovementPriority::MOVEMENT_COMBAT);

    }

    return false;
}

// Poison Nova hits 15 yd (plus the bot's size) after 5 s. The old fixed 11 yd step from the bot's spot still ended
// inside from anywhere closer than 5 yd, and when that step was refused the multiplier left the bot with no action
// at all (wave 6: the healer stood 10.8 yd from Ick through the whole cast).
bool IckAndKrickAction::PoisonNova(bool poisonNova, Unit* boss)
{
    if (!poisonNova)
        return false;

    float const exitRadius = PoSShared::PoisonNovaDangerRadius(bot) + TbcHazard::EXIT_MARGIN;
    float const distance = bot->GetExactDist2d(boss);
    if (distance >= exitRadius)
        return true;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bot->CastStop();
    if (TbcHazard::FindExitPoint(bot, boss->GetPosition(), exitRadius, x, y, z) &&
        MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED, true))
        return true;

    return MoveAway(boss, exitRadius - distance);
}

bool IckAndKrickAction::ExplosiveBarrage(bool /*explosiveBarrage*/, Unit* boss)
{
    std::vector<Unit*> orbs;
    Unit* closestOrb = nullptr;
    float closestDistance = std::numeric_limits<float>::max();

    // First gather all orbs
    GuidVector npcs1 = AI_VALUE(GuidVector, "nearest hostile npcs");
    for (auto& npc : npcs1)
    {
        Unit* unit = botAI->GetUnit(npc);
        if (unit && unit->IsAlive() && unit->HasAura(SPELL_EXPLODING_ORB_VISUAL))
        {
            orbs.push_back(unit);
            float dist = bot->GetDistance(unit);
            if (dist < closestDistance)
            {
                closestDistance = dist;
                closestOrb = unit;
            }
        }
    }

    if (!orbs.empty() && closestOrb && bot->GetExactDist2d(closestOrb) < 7.0f)
    {
        // Get player positions to avoid moving toward them
        std::vector<Position> playerPositions;
        Group* group = bot->GetGroup();
        if (group)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                if (Player* member = ref->GetSource())
                {
                    if (member != bot && member->IsAlive() && bot->GetMap() == member->GetMap() &&
                        bot->GetExactDist2d(member) < 20.0f)
                    {
                        playerPositions.push_back(member->GetPosition());
                    }
                }
            }
        }

        // Calculate several potential escape positions
        const int NUM_ANGLES = 16;  // Check 16 different directions
        const float SAFE_DISTANCE = 7.0f;
        const float MAX_BOSS_DISTANCE = 30.0f;  // Maximum allowed distance from boss
        const float ANGLE_INCREMENT = 2 * M_PI / NUM_ANGLES;

        Position bestPos;
        bool foundValidPos = false;
        float bestScore = -1.0f;

        for (int i = 0; i < NUM_ANGLES; i++)
        {
            float angle = i * ANGLE_INCREMENT;
            Position potentialPos;
            potentialPos.Relocate(bot->GetPositionX() + SAFE_DISTANCE * cos(angle),
                                  bot->GetPositionY() + SAFE_DISTANCE * sin(angle),
                                  bot->GetPositionZ());

            // Check if position is valid (not in a wall)
            if (!bot->IsWithinLOS(potentialPos.GetPositionX(), potentialPos.GetPositionY(),
                                  potentialPos.GetPositionZ()))
                continue;

            // Check if position is within maximum allowed distance from boss
            if (boss && boss->GetDistance2d(potentialPos.GetPositionX(), potentialPos.GetPositionY()) > MAX_BOSS_DISTANCE)
                continue;

            // Score this position based on:
            // 1. Distance from all orbs (farther is better)
            // 2. Distance from other players (farther is better)
            // 3. Distance from boss (closer is better, but still safe)
            float score = 0.0f;

            // Check distance from all orbs
            float minOrbDist = std::numeric_limits<float>::max();
            for (Unit* orb : orbs)
            {
                float orbDist = orb->GetDistance2d(potentialPos.GetPositionX(), potentialPos.GetPositionY());
                minOrbDist = std::min(minOrbDist, orbDist);
            }
            score += minOrbDist * 2.0f;  // Weight orb distance more heavily

            // Check distance from other players
            for (Position const& playerPos : playerPositions)
            {
                float playerDist = sqrt(pow(potentialPos.GetPositionX() - playerPos.GetPositionX(), 2) +
                                        pow(potentialPos.GetPositionY() - playerPos.GetPositionY(), 2));
                score += std::min(playerDist, 10.0f);  // Cap player distance contribution
            }

            // Factor in proximity to boss (closer is better, as long as we're safe from orbs)
            if (boss)
            {
                float bossDist = boss->GetDistance2d(potentialPos.GetPositionX(), potentialPos.GetPositionY());
                // Add points for being closer to boss (inverse relationship)
                // but only if we're safely away from orbs
                if (minOrbDist > SAFE_DISTANCE)
                {
                    score += (MAX_BOSS_DISTANCE - bossDist) * 0.5f;
                }
            }

            // If this is the best position so far, remember it
            if (score > bestScore)
            {
                bestScore = score;
                bestPos = potentialPos;
                foundValidPos = true;
            }
        }

        // If we found a valid position, move there
        if (foundValidPos)
        {
            botAI->Reset();
            // Use MoveTo instead of FleePosition since we already calculated the exact position
            return MoveTo(bot->GetMapId(), bestPos.GetPositionX(), bestPos.GetPositionY(), bot->GetPositionZ(), false,
                          false,
                          false, true, MovementPriority::MOVEMENT_COMBAT);
        }
        else
        {
            return FleePosition(closestOrb->GetPosition(), 7.0f, 250U);
        }
    }

    return false;
}

bool TyrannusAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "scourgelord tyrannus");
    if (!boss)
        return false;

    bool rangedSpread = false;

    if (PlayerbotAI::IsRanged(bot) && boss->HealthBelowPct(99))
        rangedSpread = true;

    if (rangedSpread && RangedSpread(rangedSpread))
        return true;

    return false;
}

bool TyrannusAction::RangedSpread(bool rangedSpread)
{
    float radius = 10.0f;
    float moveIncrement = 3.0f;

    GuidVector members = AI_VALUE(GuidVector, "group members");
    if (PlayerbotAI::IsRanged(bot) && rangedSpread)
    {
        // Ranged: spread from other members
        for (auto& member : members)
        {
            Unit* unit = botAI->GetUnit(member);
            if (!unit || !unit->IsAlive() || unit == bot)
                continue;

            float dist = bot->GetExactDist2d(unit);
            if (dist < radius)
            {
                float moveDistance = std::min(moveIncrement, radius - dist + 1.0f);
                return FleePosition(unit->GetPosition(), moveDistance, 250U);
            }
        }
    }

    return false;
}

bool PoSSendAreaTriggerAction::Execute(Event /*event*/)
{
    uint32 const triggerId = PoSShared::PendingAreaTrigger(bot);
    if (!triggerId || !DungeonInteract::SendAreaTrigger(bot, triggerId))
        return false;

    RaidBotDuty::LogWorkaroundOnce(bot, "pos-at-" + std::to_string(triggerId),
                                   "Pit of Saron: sent area trigger " + std::to_string(triggerId) +
                                       " of the Tyrannus event from inside its radius");
    return true;
}

bool PoSHideFromPermafrostAction::Execute(Event /*event*/)
{
    Unit* garfrost = PoSShared::GetGarfrost(bot);
    if (!garfrost || PoSShared::GetCoveringRock(bot, garfrost))
        return false;

    Position spot;
    if (!PoSShared::FindHidingSpot(bot, garfrost, spot))
    {
        RaidBotDuty::LogWorkaroundOnce(bot, "pos-permafrost-no-rock",
                                       "Pit of Saron: needs honest tactic: Permafrost stacks high but no Saronite "
                                       "Rock to hide behind yet");
        return false;
    }

    float x = spot.GetPositionX();
    float y = spot.GetPositionY();
    float z = spot.GetPositionZ();
    bot->UpdateAllowedPositionZ(x, y, z);
    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
}

bool PoSDodgeSaroniteAction::Execute(Event /*event*/)
{
    constexpr float SARONITE_DODGE_DISTANCE = 10.0f;  // boulder impact 6 yd (SpellRadius index 29)

    if (bot->isMoving())
        return true;

    Unit* garfrost = PoSShared::GetGarfrost(bot);
    return garfrost && MoveAway(garfrost, SARONITE_DODGE_DISTANCE);
}

bool PoSDodgeIcicleAction::Execute(Event /*event*/)
{
    constexpr float ICICLE_FLEE_DISTANCE = PoSIds::ICICLE_RADIUS + 1.5f;
    constexpr uint32 ICICLE_FLEE_INTERVAL_MS = 200;

    Unit* icicle = PoSShared::FindFallingIcicle(bot);
    if (!icicle)
        return false;

    return FleePosition(icicle->GetPosition(), ICICLE_FLEE_DISTANCE, ICICLE_FLEE_INTERVAL_MS);
}

bool PoSBrandHoldFireAction::Execute(Event /*event*/)
{
    if (!bot->HasAura(PoSIds::SPELL_OVERLORDS_BRAND))
        return false;

    bool stopped = DungeonHoldFire::StopHealing(bot);
    if (PoSShared::BrandDamageHitsAlly(bot))  // [b33_pos] also a tank Tyrannus is not hitting
        stopped = DungeonHoldFire::StopDamage(botAI, nullptr) || stopped;

    return stopped;
}

bool PoSAvoidHoarfrostAction::Execute(Event /*event*/)
{
    constexpr uint32 HOARFROST_FLEE_INTERVAL_MS = 500;

    Player* marked = PoSShared::FindMarkedMember(bot, PoSIds::HOARFROST_SPREAD);
    if (!marked)
        return false;

    if (marked == bot)
        return !PlayerbotAI::IsTank(bot) && MoveFromGroup(PoSIds::HOARFROST_SPREAD);

    return FleePosition(marked->GetPosition(), PoSIds::HOARFROST_SPREAD, HOARFROST_FLEE_INTERVAL_MS);
}

// [b33_pos]
bool PoSTyrannusPositionAction::Execute(Event /*event*/)
{
    Position spot;
    if (!PoSShared::TyrannusSpotFor(bot, spot))
        return false;

    // Hoarfrost and the Icy Blast puddles come first (higher priorities); a step through a puddle on the way is
    // cheaper than a Fallen Warrior on a caster, but the bot does not stop inside one: the first free point of two
    // rings round the spot (up to 12 puddles of 7 yd lie on the platform at once)
    constexpr float RING_STEP = 6.0f;
    constexpr uint32 RING_POINTS = 8;
    constexpr float PUDDLE_SAFETY = 1.0f;  // DungeonHazard.cpp HAZARD_SAFETY
    std::vector<DungeonHazard::Circle> const& circles =
        context->GetValue<std::vector<DungeonHazard::Circle>>("dungeon hazard circles")->RefGet();
    auto const isFree = [&circles, PUDDLE_SAFETY](float px, float py)
    {
        for (DungeonHazard::Circle const& circle : circles)
            if (std::hypot(px - circle.x, py - circle.y) < circle.radius + PUDDLE_SAFETY)
                return false;
        return true;
    };

    float x = spot.GetPositionX();
    float y = spot.GetPositionY();
    bool found = isFree(x, y);
    for (uint32 ring = 1; !found && ring <= 2; ++ring)
        for (uint32 i = 0; !found && i < RING_POINTS; ++i)
        {
            float const angle = 2.0f * float(M_PI) * i / RING_POINTS;
            float const px = spot.GetPositionX() + std::cos(angle) * RING_STEP * ring;
            float const py = spot.GetPositionY() + std::sin(angle) * RING_STEP * ring;
            if (isFree(px, py))
            {
                x = px;
                y = py;
                found = true;
            }
        }

    if (!found)
        return false;

    float z = spot.GetPositionZ();
    bot->UpdateAllowedPositionZ(x, y, z);
    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
}
