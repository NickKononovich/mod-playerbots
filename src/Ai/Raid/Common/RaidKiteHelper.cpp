/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidKiteHelper.h"
#include "EncounterHelpers.h"
#include "Player.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace RaidKiteHelper
{

namespace
{

constexpr uint8 KITE_SAMPLE_ANGLES = 16;
// Score penalty per radian away from the preferred angle, in yards.
constexpr float KITE_PREFERRED_ANGLE_WEIGHT = 2.0f;
// A route leg may not pass this much closer to a threat than the bot already is.
constexpr float KITE_ROUTE_LEG_SLACK = 2.0f;
constexpr float KITE_WAYPOINT_ARRIVAL_DISTANCE = 2.0f;

float AngleDelta(float a, float b)
{
    float delta = std::fmod(std::fabs(a - b), 2.0f * static_cast<float>(M_PI));
    return delta > static_cast<float>(M_PI) ? 2.0f * static_cast<float>(M_PI) - delta : delta;
}

float SegmentMinDistanceToThreats(
    float ax, float ay, float bx, float by, std::vector<Position> const& threats)
{
    float closest = std::numeric_limits<float>::max();
    float const dx = bx - ax;
    float const dy = by - ay;
    float const lengthSq = dx * dx + dy * dy;
    for (Position const& threat : threats)
    {
        float t = 0.0f;
        if (lengthSq > 0.0f)
        {
            t = ((threat.GetPositionX() - ax) * dx + (threat.GetPositionY() - ay) * dy) / lengthSq;
            t = std::clamp(t, 0.0f, 1.0f);
        }

        closest = std::min(closest,
            std::hypot(threat.GetPositionX() - (ax + t * dx), threat.GetPositionY() - (ay + t * dy)));
    }

    return closest;
}

bool IsInsideLeash(KiteParams const& params, float x, float y)
{
    return params.leashRadius <= 0.0f || params.leashCenter.GetExactDist2d(x, y) <= params.leashRadius;
}

} // namespace

float MinDistanceToThreats(float x, float y, std::vector<Position> const& threats)
{
    float closest = std::numeric_limits<float>::max();
    for (Position const& threat : threats)
        closest = std::min(closest, threat.GetExactDist2d(x, y));

    return closest;
}

bool IsKiteNeeded(Player* bot, std::vector<Position> const& threats, float keepDistance)
{
    return bot && MinDistanceToThreats(bot->GetPositionX(), bot->GetPositionY(), threats) < keepDistance;
}

bool FindOpenKiteStep(
    Player* bot, std::vector<Position> const& threats, KiteParams const& params, Position& step)
{
    if (!bot || threats.empty())
        return false;

    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const currentDistance = MinDistanceToThreats(botX, botY, threats);

    // Half-length steps are tried only when every full-length one is blocked (walls, pillars).
    for (float const lengthFactor : { 1.0f, 0.5f })
    {
        float const length = params.stepDistance * lengthFactor;
        float bestScore = std::numeric_limits<float>::lowest();
        float bestDistance = 0.0f;
        bool found = false;

        for (uint8 i = 0; i < KITE_SAMPLE_ANGLES; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / KITE_SAMPLE_ANGLES;
            float const destX = botX + std::cos(angle) * length;
            float const destY = botY + std::sin(angle) * length;
            if (!IsInsideLeash(params, destX, destY) || (params.isHazard && params.isHazard(destX, destY)))
                continue;

            float stepX;
            float stepY;
            float stepZ;
            if (!EncounterHelpers::CanTakeStepTowards(bot, destX, destY, length, stepX, stepY, stepZ))
                continue;

            float const distance = MinDistanceToThreats(stepX, stepY, threats);
            float score = distance;
            if (params.usePreferredAngle)
                score -= AngleDelta(angle, params.preferredAngle) * KITE_PREFERRED_ANGLE_WEIGHT;

            if (!found || score > bestScore)
            {
                bestScore = score;
                bestDistance = distance;
                step = Position(stepX, stepY, stepZ);
                found = true;
            }
        }

        // Moving without gaining distance only drags the threat around; better to hold position.
        if (found && bestDistance > currentDistance)
            return true;
    }

    return false;
}

bool FindRouteKiteStep(
    Player* bot, std::vector<Position> const& threats, std::vector<Position> const& route,
    KiteParams const& params, Position& step)
{
    if (!bot || threats.empty())
        return false;

    if (route.size() < 2)
        return FindOpenKiteStep(bot, threats, params, step);

    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const currentDistance = MinDistanceToThreats(botX, botY, threats);

    size_t nearest = 0;
    float nearestDistance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < route.size(); ++i)
    {
        float const distance = route[i].GetExactDist2d(botX, botY);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearest = i;
        }
    }

    // Off the route: rejoin it at the nearest waypoint unless that means running at the threat.
    std::vector<size_t> candidates;
    if (nearestDistance > KITE_WAYPOINT_ARRIVAL_DISTANCE)
        candidates.push_back(nearest);
    candidates.push_back(NextSafeRouteIndex(route, nearest, 1, params.isHazard));
    candidates.push_back(NextSafeRouteIndex(route, nearest, -1, params.isHazard));

    float bestDistance = std::numeric_limits<float>::lowest();
    Position const* bestWaypoint = nullptr;
    for (size_t const index : candidates)
    {
        Position const& waypoint = route[index];
        if (waypoint.GetExactDist2d(botX, botY) <= KITE_WAYPOINT_ARRIVAL_DISTANCE)
            continue;

        if (params.isHazard && params.isHazard(waypoint.GetPositionX(), waypoint.GetPositionY()))
            continue;

        float const legDistance = SegmentMinDistanceToThreats(
            botX, botY, waypoint.GetPositionX(), waypoint.GetPositionY(), threats);
        if (legDistance < std::min(currentDistance, params.keepDistance) - KITE_ROUTE_LEG_SLACK)
            continue;

        float const distance =
            MinDistanceToThreats(waypoint.GetPositionX(), waypoint.GetPositionY(), threats);
        if (distance > bestDistance)
        {
            bestDistance = distance;
            bestWaypoint = &waypoint;
        }
    }

    if (bestWaypoint)
    {
        float stepX;
        float stepY;
        float stepZ;
        if (EncounterHelpers::CanTakeStepTowards(bot, bestWaypoint->GetPositionX(),
                bestWaypoint->GetPositionY(), params.stepDistance, stepX, stepY, stepZ) &&
            (!params.isHazard || !params.isHazard(stepX, stepY)))
        {
            step = Position(stepX, stepY, stepZ);
            return true;
        }
    }

    return FindOpenKiteStep(bot, threats, params, step);
}

size_t NextSafeRouteIndex(
    std::vector<Position> const& route, size_t current, int direction, HazardCheck const& isHazard)
{
    size_t const count = route.size();
    if (count < 2)
        return current;

    size_t index = current;
    for (size_t i = 1; i < count; ++i)
    {
        index = direction >= 0 ? (index + 1) % count : (index + count - 1) % count;
        if (!isHazard || !isHazard(route[index].GetPositionX(), route[index].GetPositionY()))
            return index;
    }

    return current;
}

}
