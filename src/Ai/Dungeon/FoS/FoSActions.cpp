/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FoSActions.h"
#include "DungeonHoldFire.h"
#include "Group.h"
#include "Playerbots.h"
#include "RaidMarkOwnership.h"

#include <algorithm>
#include <cmath>

bool MoveFromBronjahmAction::Execute(Event /*event*/)
{
    constexpr uint32 CORRUPT_SOUL_FLEE_INTERVAL_MS = 500;

    Unit* boss = AI_VALUE2(Unit*, "find target", "bronjahm");
    if (!boss || bot->GetExactDist2d(boss) >= FoSIds::CORRUPT_SOUL_FLEE_DISTANCE)
        return false;

    return FleePosition(boss->GetPosition(), FoSIds::CORRUPT_SOUL_FLEE_DISTANCE, CORRUPT_SOUL_FLEE_INTERVAL_MS);
}

bool AttackCorruptedSoulFragmentAction::Execute(Event /*event*/)
{
    GuidVector targets = AI_VALUE(GuidVector, "possible targets");

    // If no valid skull target, search for corrupted soul fragment
    for (auto i = targets.begin(); i != targets.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (!unit || !unit->IsAlive())
            continue;

        if (unit->GetEntry() == NPC_CORRUPTED_SOUL_FRAGMENT)
        {
                // Mark corrupted soul fragment with skull if in group and not already marked
                if (Group* group = bot->GetGroup())
                {
                    ObjectGuid currentSkullGuid = group->GetTargetIcon(7);
                    if (currentSkullGuid.IsEmpty() || currentSkullGuid != unit->GetGUID())
                    {
                        RaidMarkOwnership::SetIcon(bot, group, 7, unit->GetGUID());  // 7 = skull
                    }
                }
                break;
        }

    }

    return false;
}

bool BronjahmGroupPositionAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "bronjahm");
    if (!boss)
        return false;

    if (FoSShared::IsSoulstorm(boss))
        return StackInSoulstorm(boss);

    Unit* corruptedSoul = bot->FindNearestCreature(NPC_CORRUPTED_SOUL_FRAGMENT, 50.0f);
    bool activeSoulExists = corruptedSoul && corruptedSoul->IsAlive();

    if (PlayerbotAI::IsTank(bot) && botAI->HasAggro(boss))
    {
        // If any corrupted soul exists, handle positioning carefully
        if (activeSoulExists)
        {
            GuidVector npcs = AI_VALUE(GuidVector, "nearest hostile npcs");
            for (auto& npc : npcs)
            {
                Unit* unit = botAI->GetUnit(npc);
                if (unit && unit->GetEntry() == NPC_CORRUPTED_SOUL_FRAGMENT)
                {
                    // Soul exists - check positions
                    float soulToBossDist = unit->GetExactDist2d(boss);
                    float tankToBossDist = bot->GetExactDist2d(boss);
                    float soulToTankDist = unit->GetExactDist2d(bot);

                    // Handle edge case: if soul is between tank and boss
                    // This happens when: (tankToBossDist > soulToBossDist) AND (soulToTankDist < tankToBossDist)
                    if (tankToBossDist > soulToBossDist && soulToTankDist < tankToBossDist)
                    {
                        // Calculate position 5 yards behind boss from the soul's perspective
                        float angle = boss->GetAngle(unit);                                  // Angle from boss to soul
                        float oppositeAngle = Position::NormalizeOrientation(angle + M_PI);  // Opposite direction

                        // Calculate position 5 yards behind boss
                        float x = boss->GetPositionX() + 5.0f * cos(oppositeAngle);
                        float y = boss->GetPositionY() + 5.0f * sin(oppositeAngle);
                        float z = boss->GetPositionZ();

                        return MoveTo(bot->GetMapId(), x, y, z, false, false, false, true,
                                      MovementPriority::MOVEMENT_NORMAL);
                    }

                    // If soul is near boss, flee from boss
                    if (soulToBossDist < 10.0f)
                        return FleePosition(unit->GetPosition(), 13.0f, 1000U);

                    // If soul exists but none of the above conditions, don't move to tank position yet
                    bot->SetFacingToObject(boss);
                    return true;
                }
            }
        }
        else
        {
            // No souls exist, safe to move to tank position
            if (bot->GetExactDist2d(BRONJAHM_TANK_POSITION) > 5.0f)
            {
                return MoveTo(bot->GetMapId(), BRONJAHM_TANK_POSITION.GetPositionX(),
                              BRONJAHM_TANK_POSITION.GetPositionY(), BRONJAHM_TANK_POSITION.GetPositionZ(), false,
                              false, false, true, MovementPriority::MOVEMENT_NORMAL);
            }
        }
    }

    return false;
}

bool BronjahmGroupPositionAction::StackInSoulstorm(Unit* boss)
{
    // Wave 8 (normal): the old move in started only after the 4 s cast, stopped 6.5 yd from the boss (inside the
    // 8.25 yd melee-danger gap of the ranged position), ran at normal priority and gave up at 10 yd. The ranged
    // position walked the hunter and the healer back to 22 yd, a combat-priority walk the normal move could not
    // cut short; fear sent them out as well. The hunter swung 7-21 yd for 25 s and died at 11 yd.
    bool const melee = PlayerbotAI::IsTank(bot) || !PlayerbotAI::IsRanged(bot);
    float const minDistance = melee ? 0.0f : FoSIds::SOULSTORM_RANGED_MIN;
    float const maxDistance = melee ? FoSIds::SOULSTORM_MELEE_MAX : FoSIds::SOULSTORM_RANGED_MAX;
    float const stackDistance = melee ? FoSIds::SOULSTORM_MELEE_STACK : FoSIds::SOULSTORM_RANGED_STACK;

    Position const centre = FoSShared::GetSoulstormCentre(boss);
    float const distance = bot->GetExactDist2d(centre);
    if (distance >= minDistance && distance <= maxDistance)
        return false;

    // Already walking to a spot in the eye: keep walking. Returning true keeps lower actions from re-routing the
    // bot; cast-time spells are refused while moving anyway.
    LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
    float const lastMoveDistance = centre.GetExactDist2d(lastMove.lastMoveToX, lastMove.lastMoveToY);
    bool const lastMoveInEye = lastMove.lastMoveToMapId == bot->GetMapId() && lastMoveDistance >= minDistance &&
                               lastMoveDistance <= maxDistance;
    if (lastMoveInEye && bot->isMoving())
        return true;

    // A forced order still pending (an earlier dodge, the walk in broken off by fear or a cast) must not hold the
    // bot out, and neither may the duplicate-move guard when the same spot is ordered again after a stop.
    if (lastMove.priority == MovementPriority::MOVEMENT_FORCED)
        lastMove.priority = MovementPriority::MOVEMENT_COMBAT;

    if (!bot->isMoving())
        lastMove.setShort(WorldPosition());

    // Straight in along the bot's own bearing: the shortest way out of the storm, and the group spreads round
    // the boss instead of piling onto one spot.
    float const angle = centre.GetAngle(bot);
    float x = centre.GetPositionX() + std::cos(angle) * stackDistance;
    float y = centre.GetPositionY() + std::sin(angle) * stackDistance;
    float z = centre.GetPositionZ();
    bot->UpdateAllowedPositionZ(x, y, z);

    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED);
}

bool BronjahmGroupPositionAction::isUseful() { return true; }

bool DevourerOfSoulsAction::Execute(Event /*event*/)
{
    Unit* boss = FoSShared::GetMirroringDevourer(botAI);
    return boss && DungeonHoldFire::StopDamage(botAI, boss);
}

bool AvoidWailingSoulsAction::Execute(Event /*event*/)
{
    // Every spot lies outside the boundary circle (FoSIds::WAILING_SOULS_BOUNDARY_EXTRA), which the beam hits from
    // any angle: melee wait just outside it, out of melee range, until the sweep is over.
    constexpr float MELEE_BEHIND_EXTRA = 1.0f;
    constexpr float RANGED_BEHIND_EXTRA_MAX = 5.0f;
    constexpr float MAX_LANE_SPREAD = 0.2f;  // radians between group lanes behind the boss

    Unit* boss = FoSShared::GetWailingDevourer(botAI);
    if (!boss)
        return false;

    float const danger = FoSShared::GetWailingDangerAngle(boss);
    if (FoSShared::IsSafeFromWailing(boss, bot->GetPositionX(), bot->GetPositionY(), danger))
        return false;

    // Already running to a spot the rest of the sweep cannot reach: keep running. Returning true keeps
    // lower actions from re-routing the bot; cast-time spells are refused while moving anyway.
    LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
    bool const lastMoveSafe =
        lastMove.lastMoveToMapId == bot->GetMapId() &&
        FoSShared::IsSafeFromWailing(boss, lastMove.lastMoveToX, lastMove.lastMoveToY, danger);
    if (lastMoveSafe && bot->isMoving())
        return true;

    // A forced order into the beam (an earlier dodge, another mechanic) must not hold the bot there until it expires.
    if (!lastMoveSafe && lastMove.priority == MovementPriority::MOVEMENT_FORCED)
        lastMove.priority = MovementPriority::MOVEMENT_COMBAT;

    // the run from the front goes through the beam: a near spot behind the boss is the shortest way out
    float const boundary = boss->GetCombatReach() + FoSIds::WAILING_SOULS_BOUNDARY_EXTRA;
    float distance = bot->GetExactDist2d(boss);
    if (PlayerbotAI::IsRanged(bot))
        distance = std::clamp(distance, boundary + MELEE_BEHIND_EXTRA, boundary + RANGED_BEHIND_EXTRA_MAX);
    else
        distance = boundary + MELEE_BEHIND_EXTRA;

    int32 lane = 0;
    if (Group* group = bot->GetGroup())
    {
        int32 index = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource(); member && member->GetGUID() < bot->GetGUID())
                ++index;

        lane = index % 5 - 2;
    }

    // Straight behind the current facing, lanes kept inside the safe arc: a spot past the danger angle now stays
    // past it for the rest of the sweep, whichever way the boss turns, so the bot never has to move again.
    float const safeArc = static_cast<float>(M_PI) - danger;
    float const laneSpread = std::clamp(safeArc / 3.0f, 0.0f, MAX_LANE_SPREAD);
    float const angle = boss->GetOrientation() + static_cast<float>(M_PI) + lane * laneSpread;
    float x = boss->GetPositionX() + std::cos(angle) * distance;
    float y = boss->GetPositionY() + std::sin(angle) * distance;

    // Units are no obstacle to the path, so a straight run would cross his boundary circle, where every beam
    // tick hits: go round it instead, a quarter turn at a time towards the destination side.
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const dx = x - botX;
    float const dy = y - botY;
    float const len2 = dx * dx + dy * dy;
    float t = len2 > 0.01f ? ((boss->GetPositionX() - botX) * dx + (boss->GetPositionY() - botY) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    float const passDist = std::hypot(botX + t * dx - boss->GetPositionX(), botY + t * dy - boss->GetPositionY());
    if (passDist < boundary)
    {
        float const botBearing = boss->GetAngle(bot);
        float turn = Position::NormalizeOrientation(angle - botBearing);
        if (turn > static_cast<float>(M_PI))
            turn -= 2.0f * static_cast<float>(M_PI);

        // Already inside: mostly straight out first (an eighth turn), the shortest way out of the beam.
        float const maxStep = bot->GetExactDist2d(boss) < boundary ? static_cast<float>(M_PI) / 4.0f
                                                                   : static_cast<float>(M_PI) / 2.0f;
        float const side = turn >= 0.0f ? 1.0f : -1.0f;
        float const step = std::min(std::fabs(turn), maxStep);
        float const waypoint = botBearing + side * step;
        float const radius = boundary + MELEE_BEHIND_EXTRA;
        x = boss->GetPositionX() + std::cos(waypoint) * radius;
        y = boss->GetPositionY() + std::sin(waypoint) * radius;
    }

    float z = boss->GetPositionZ();
    bot->UpdateAllowedPositionZ(x, y, z);

    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED);
}

bool FoSInterruptAction::Execute(Event /*event*/)
{
    return DungeonHoldFire::CastInterrupt(botAI, FoSShared::GetPhantomBlastCaster(botAI));
}
