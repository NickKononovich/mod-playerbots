/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FleeManager.h"
#include "Creature.h"
#include "Group.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include <cmath>

namespace
{
    constexpr float FLEE_IDLE_AGGRO_MARGIN = 5.0f;  // a spot this close to an idle mob's aggro range is not taken
    constexpr float FLEE_IDLE_MAX_DZ = 5.0f;
    constexpr float FLEE_TANK_PREFERENCE = 5.0f;    // spots nearer the tank by more than this win

    // Wave 9 Steamvault: a healer fled 60 yd away from the tank into an unpulled Bog Overlord and died.
    bool PullsIdleMob(Player* bot, PlayerbotAI* botAI, GuidVector const& units, float x, float y, float z)
    {
        for (ObjectGuid const guid : units)
        {
            Creature* mob = botAI->GetCreature(guid);
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->HasReactState(REACT_AGGRESSIVE) ||
                !bot->IsHostileTo(mob) || std::fabs(mob->GetPositionZ() - z) > FLEE_IDLE_MAX_DZ)
                continue;

            if (mob->GetExactDist2d(x, y) <= mob->GetAggroRange(bot) + FLEE_IDLE_AGGRO_MARGIN)
                return true;
        }

        return false;
    }

    Player* NearestLivingTank(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group || PlayerbotAI::IsTank(bot))
            return nullptr;

        Player* nearest = nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId() ||
                !PlayerbotAI::IsTank(member))
                continue;

            if (!nearest || bot->GetExactDist2d(member) < bot->GetExactDist2d(nearest))
                nearest = member;
        }

        return nearest;
    }
}

FleeManager::FleeManager(Player* bot, float maxAllowedDistance, float /*followAngle*/, bool forceMaxDistance,
                         WorldPosition startPosition)
    : bot(bot),
      maxAllowedDistance(maxAllowedDistance),
      forceMaxDistance(forceMaxDistance),
      startPosition(startPosition ? startPosition : WorldPosition(bot))
{
}

void FleeManager::calculateDistanceToCreatures(FleePoint* point)
{
    point->minDistance = -1.0f;
    point->sumDistance = 0.0f;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
    {
        return;
    }
    GuidVector units = *botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los");
    for (GuidVector::iterator i = units.begin(); i != units.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (!unit)
            continue;

        float d = ServerFacade::instance().GetDistance2d(unit, point->x, point->y);
        point->sumDistance += d;
        if (point->minDistance < 0 || point->minDistance > d)
            point->minDistance = d;
    }
}

bool intersectsOri(float angle, std::vector<float>& angles, float angleIncrement)
{
    for (std::vector<float>::iterator i = angles.begin(); i != angles.end(); ++i)
    {
        float ori = *i;
        if (abs(angle - ori) < angleIncrement)
            return true;
    }

    return false;
}

void FleeManager::calculatePossibleDestinations(std::vector<FleePoint*>& points)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
    {
        return;
    }
    Unit* target = *botAI->GetAiObjectContext()->GetValue<Unit*>("current target");

    float botPosX = startPosition.GetPositionX();
    float botPosY = startPosition.GetPositionY();
    float botPosZ = startPosition.GetPositionZ();

    FleePoint start(botPosX, botPosY, botPosZ);
    calculateDistanceToCreatures(&start);

    Player* tank = NearestLivingTank(bot);
    std::vector<float> enemyOri;
    GuidVector units = *botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los");
    for (GuidVector::iterator i = units.begin(); i != units.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (!unit)
            continue;

        float ori = bot->GetAngle(unit);
        enemyOri.push_back(ori);
    }

    float distIncrement = std::max(sPlayerbotAIConfig.FollowDistance,
                                   (maxAllowedDistance - sPlayerbotAIConfig.TooCloseDistance) / 10.0f);
    for (float dist = maxAllowedDistance; dist >= sPlayerbotAIConfig.TooCloseDistance; dist -= distIncrement)
    {
        float angleIncrement = std::max(M_PI / 20, M_PI / 4 / (1.0 + dist - sPlayerbotAIConfig.TooCloseDistance));
        for (float add = 0.0f; add < M_PI / 4 + angleIncrement; add += angleIncrement)
        {
            for (float angle = add; angle < add + 2 * static_cast<float>(M_PI) + angleIncrement;
                 angle += static_cast<float>(M_PI) / 4)
            {
                if (intersectsOri(angle, enemyOri, angleIncrement))
                    continue;

                float x = botPosX + cos(angle) * maxAllowedDistance, y = botPosY + sin(angle) * maxAllowedDistance,
                      z = botPosZ + CONTACT_DISTANCE;
                if (forceMaxDistance &&
                    ServerFacade::instance().IsDistanceLessThan(ServerFacade::instance().GetDistance2d(bot, x, y),
                                                      maxAllowedDistance - sPlayerbotAIConfig.TooCloseDistance))
                    continue;

                bot->UpdateAllowedPositionZ(x, y, z);

                Map* map = startPosition.getMap();
                if (map && map->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
                    continue;

                if (!bot->IsWithinLOS(x, y, z) || (target && !target->IsWithinLOS(x, y, z)))
                    continue;

                if (PullsIdleMob(bot, botAI, units, x, y, z))
                    continue;

                FleePoint* point = new FleePoint(x, y, z);
                calculateDistanceToCreatures(point);
                if (tank)
                    point->tankDistance = tank->GetExactDist2d(x, y);

                if (ServerFacade::instance().IsDistanceGreaterOrEqualThan(point->minDistance - start.minDistance,
                                                                sPlayerbotAIConfig.FollowDistance))
                    points.push_back(point);
                else
                    delete point;
            }
        }
    }
}

void FleeManager::cleanup(std::vector<FleePoint*>& points)
{
    for (std::vector<FleePoint*>::iterator i = points.begin(); i != points.end(); i++)
    {
        delete *i;
    }

    points.clear();
}

bool FleeManager::isBetterThan(FleePoint* point, FleePoint* other)
{
    // A healer or damage dealer runs towards its tank, where the mob it pulls gets taken back.
    if (point->tankDistance >= 0.0f && other->tankDistance >= 0.0f &&
        std::fabs(point->tankDistance - other->tankDistance) > FLEE_TANK_PREFERENCE)
        return point->tankDistance < other->tankDistance;

    return point->sumDistance - other->sumDistance > 0;
}

FleePoint* FleeManager::selectOptimalDestination(std::vector<FleePoint*>& points)
{
    FleePoint* best = nullptr;
    for (std::vector<FleePoint*>::iterator i = points.begin(); i != points.end(); i++)
    {
        FleePoint* point = *i;
        if (!best || isBetterThan(point, best))
            best = point;
    }

    return best;
}

bool FleeManager::CalculateDestination(float* rx, float* ry, float* rz)
{
    std::vector<FleePoint*> points;
    calculatePossibleDestinations(points);

    FleePoint* point = selectOptimalDestination(points);
    if (!point)
    {
        cleanup(points);
        return false;
    }

    *rx = point->x;
    *ry = point->y;
    *rz = point->z;

    cleanup(points);
    return true;
}

bool FleeManager::isUseful()
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
    {
        return false;
    }
    GuidVector units = *botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los");
    for (GuidVector::iterator i = units.begin(); i != units.end(); ++i)
    {
        Creature* creature = botAI->GetCreature(*i);
        if (!creature)
            continue;

        if (startPosition.sqDistance(WorldPosition(creature)) <
            creature->GetAttackDistance(bot) * creature->GetAttackDistance(bot))
            return true;

        // float d = ServerFacade::instance().GetDistance2d(unit, bot);
        // if (ServerFacade::instance().IsDistanceLessThan(d, sPlayerbotAIConfig.AggroDistance)) return true;
    }

    return false;
}
