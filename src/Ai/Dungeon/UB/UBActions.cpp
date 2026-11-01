/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UBActions.h"
#include "BotPath.h"  // [lead109]
#include "Group.h"
#include "GridTerrainData.h"
#include "Map.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "Timer.h"
#include "UBShared.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

using namespace UnderbogHungarfen;

bool UBRetreatFromFoulSporesAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "hungarfen");
    if (!boss)
        return false;

    float const safeDistance = MaxEffectRadius(SPELL_FOUL_SPORES, FOUL_SPORES_RADIUS_FALLBACK) + FOUL_SPORES_MARGIN;
    float const currentDistance = bot->GetDistance2d(boss);
    if (currentDistance >= safeDistance)
        return false;

    float const moveDist = safeDistance - currentDistance + 1.0f;
    float const awayAngle = boss->GetAngle(bot);

    auto const& mushrooms = AI_VALUE_REF(GuidVector, "ub mushrooms");

    for (float delta : { 0.0f, float(M_PI / 8), float(-M_PI / 8), float(M_PI / 4), float(-M_PI / 4),
                         float(3 * M_PI / 8), float(-3 * M_PI / 8), float(M_PI / 2), float(-M_PI / 2) })
    {
        float const angle = awayAngle + delta;
        float dx = bot->GetPositionX() + cos(angle) * moveDist;
        float dy = bot->GetPositionY() + sin(angle) * moveDist;
        float dz = bot->GetPositionZ();
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), dx, dy, dz))
            continue;

        if (RetreatPathUnsafe(bot, mushrooms, dx, dy))
            continue;

        if (MoveTo(bot->GetMapId(), dx, dy, dz, false, false, true, true,
                   MovementPriority::MOVEMENT_COMBAT))
            return true;
    }

    return MoveAway(boss, moveDist);
}

bool UBVacateSporeCloudAction::Execute(Event /*event*/)
{
    float const dangerRange = MushroomDangerRange(bot);
    auto const& mushrooms = AI_VALUE_REF(GuidVector, "ub mushrooms");
    Creature* mushroom = GetNearestDangerousMushroom(bot, mushrooms, dangerRange);
    if (!mushroom)
        return false;

    if (PlayerbotAI::IsTank(bot))
    {
        Unit* boss = AI_VALUE2(Unit*, "find target", "hungarfen");
        if (boss && boss->GetVictim() == bot)
        {
            float const currentDistance = bot->GetDistance2d(mushroom);
            return MoveAway(mushroom, dangerRange - currentDistance + 2.0f);
        }
    }

    return FleePosition(mushroom->GetPosition(), dangerRange);
}

bool UBClearUnderbatBackAction::Execute(Event /*event*/)
{
    if (PlayerbotAI::IsTank(bot))
        return false;

    auto const& attackers = AI_VALUE_REF(GuidVector, "attackers");
    Creature* bat = GetNearestUnderbatInLashRange(bot, attackers);
    if (!bat)
        return false;

    bool const throttled = _lastReposition && GetMSTimeDiffToNow(_lastReposition) < UNDERBAT_REPOSITION_COOLDOWN;

    bool const melee = PlayerbotAI::IsMelee(bot);
    if (melee)
    {
        Unit* rally = UnderbatRallyUnit(bot, attackers);
        bool const parked = rally && bot->GetExactDist(rally) <= UNDERBAT_RALLY_TOLERANCE;

        if (rally && !parked)
        {
            if (throttled)
                return false;

            float const rx = rally->GetPositionX();
            float const ry = rally->GetPositionY();
            float const rz = rally->GetPositionZ();
            if (SpotClearOfUnderbats(bot, attackers, rx, ry, rz) &&
                MoveTo(bot->GetMapId(), rx, ry, rz, false, false, true, true,
                       MovementPriority::MOVEMENT_COMBAT))
            {
                _lastReposition = getMSTime();
                return true;
            }
        }

        if (!GetLashingUnderbat(bot, attackers))
            return false;
    }
    else if (bot->IsNonMeleeSpellCast(true) && !GetLashingUnderbat(bot, attackers))
        return false;

    if (throttled)
        return false;

    float const clearDistance = UNDERBAT_LASH_RANGE + UNDERBAT_LASH_MARGIN;
    float const gap = bot->GetDistance2d(bat);
    if (gap >= clearDistance)
        return false;

    if (!MoveAway(bat, clearDistance - gap + 1.0f))
        return false;

    _lastReposition = getMSTime();
    return true;
}

bool UBLeaveLakeAction::PathOk(float x, float y, float z, float maxLength)
{
    BotPath const path = FindBotPath(bot, x, y, z);  // [lead109]
    return path.valid && !(path.type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) && path.length <= maxLength;
}

bool UBLeaveLakeAction::Execute(Event /*event*/)
{
    using namespace UnderbogGhazan;
    if (!LakeFight(bot) || !IsWet(bot))
        return false;

    constexpr std::array<float, 5> steps = {6.0f, 10.0f, 15.0f, 20.0f, 26.0f};
    constexpr uint32 directions = 12;
    constexpr uint32 maxPathChecks = 8;
    constexpr float heightProbeUp = 10.0f;
    constexpr float heightSearch = 60.0f;
    constexpr float nearPathMax = 45.0f;
    constexpr float memberRange = 80.0f;
    constexpr float farPathMax = 120.0f;

    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const botZ = bot->GetPositionZ();

    // Shore spots around the bot, nearest first. From the lake bottom the probe only finds the bottom (wet), so the
    // anchors below take over there.
    std::vector<std::pair<float, std::array<float, 3>>> candidates;
    for (float step : steps)
    {
        for (uint32 i = 0; i < directions; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / directions;
            float const x = botX + std::cos(angle) * step;
            float const y = botY + std::sin(angle) * step;
            float const z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, botZ + heightProbeUp, true,
                                                     heightSearch);
            if (z <= INVALID_HEIGHT + 1.0f || !IsDrySpot(bot, x, y, z))
                continue;

            candidates.push_back({step, {x, y, z}});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](auto const& left, auto const& right) { return left.first < right.first; });

    uint32 pathChecks = 0;
    for (auto const& candidate : candidates)
    {
        if (++pathChecks > maxPathChecks)
            break;

        auto const& spot = candidate.second;
        if (PathOk(spot[0], spot[1], spot[2], nearPathMax))
            return MoveTo(bot->GetMapId(), spot[0], spot[1], spot[2], false, false, false, false,
                          MovementPriority::MOVEMENT_FORCED, true);
    }

    // A member standing on dry land, then the walkway and the platform.
    std::vector<std::array<float, 3>> anchors;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId() ||
                bot->GetExactDist(member) > memberRange || IsWet(member))
                continue;

            anchors.push_back({member->GetPositionX(), member->GetPositionY(), member->GetPositionZ()});
        }
    }
    anchors.push_back({WALKWAY_X, WALKWAY_Y, WALKWAY_Z});
    anchors.push_back({PLATFORM_X, PLATFORM_Y, PLATFORM_Z});

    for (auto const& anchor : anchors)
    {
        if (!IsDrySpot(bot, anchor[0], anchor[1], anchor[2]) || !PathOk(anchor[0], anchor[1], anchor[2], farPathMax))
            continue;

        return MoveTo(bot->GetMapId(), anchor[0], anchor[1], anchor[2], false, false, false, false,
                      MovementPriority::MOVEMENT_FORCED, true);
    }
    return false;
}
