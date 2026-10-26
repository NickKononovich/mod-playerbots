/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TbcHazard.h"
#include "AttackAction.h"
#include "ChooseTargetActions.h"
#include "Group.h"
#include "HunterActions.h"
#include "MageActions.h"
#include "Map.h"
#include "Playerbots.h"
#include "ReachTargetActions.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace TbcHazard
{
namespace
{
// Straight out first, then fanning out to either side; the back half is a last resort.
constexpr std::array<float, 11> EXIT_ANGLE_OFFSETS = {
    0.0f,
    static_cast<float>(M_PI) / 8.0f, -static_cast<float>(M_PI) / 8.0f,
    static_cast<float>(M_PI) / 4.0f, -static_cast<float>(M_PI) / 4.0f,
    static_cast<float>(M_PI) * 3.0f / 8.0f, -static_cast<float>(M_PI) * 3.0f / 8.0f,
    static_cast<float>(M_PI) / 2.0f, -static_cast<float>(M_PI) / 2.0f,
    static_cast<float>(M_PI) * 3.0f / 4.0f, -static_cast<float>(M_PI) * 3.0f / 4.0f,
};

// How far short of the requested ring a collision-clamped spot may land and still count.
constexpr float RING_TOLERANCE = 1.0f;

bool FindPointOnRing(Player* bot, Position const& center, float radius, float& x, float& y, float& z)
{
    float const dist = bot->GetExactDist2d(center);
    float const baseAngle = dist > 0.5f ? center.GetAngle(bot) : bot->GetOrientation() + static_cast<float>(M_PI);

    for (float offset : EXIT_ANGLE_OFFSETS)
    {
        float const angle = baseAngle + offset;
        float destX = center.GetPositionX() + std::cos(angle) * radius;
        float destY = center.GetPositionY() + std::sin(angle) * radius;
        float destZ = bot->GetPositionZ();

        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), destX, destY, destZ))
            continue;

        // A spot pushed back towards the hazard by a wall is no exit.
        if (std::fabs(center.GetExactDist2d(destX, destY) - radius) > RING_TOLERANCE)
            continue;

        x = destX;
        y = destY;
        z = destZ;
        return true;
    }

    return false;
}
}  // namespace

bool FindExitPoint(Player* bot, Position const& center, float radius, float& x, float& y, float& z)
{
    if (bot->GetExactDist2d(center) >= radius)
        return false;

    return FindPointOnRing(bot, center, radius, x, y, z);
}

bool FindRingPoint(Player* bot, Position const& center, float radius, float& x, float& y, float& z)
{
    return FindPointOnRing(bot, center, radius, x, y, z);
}

bool AnyGroupMemberWithin(Player* bot, Position const& center, float radius)
{
    Group* group = bot->GetGroup();
    if (!group)
        return bot->GetExactDist2d(center) < radius;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || member->GetMapId() != bot->GetMapId())
            continue;

        if (member->GetExactDist2d(center) < radius)
            return true;
    }

    return false;
}

bool IsRepositionAction(Action* action)
{
    // Attack actions derive from MovementAction but only pick and engage a target; the reach actions that would
    // then carry a melee bot to it are caught below.
    if (dynamic_cast<TbcHazardMoveAction*>(action) || dynamic_cast<AttackAction*>(action))
        return false;

    return dynamic_cast<MovementAction*>(action) || dynamic_cast<CastReachTargetSpellAction*>(action) ||
           dynamic_cast<CastBlinkBackAction*>(action) || dynamic_cast<CastDisengageAction*>(action);
}

bool IsInside(Player* bot, Position const& center, float dangerRadius)
{
    constexpr float safety = 1.0f;
    return bot->GetExactDist2d(center) < dangerRadius + safety;
}

bool ShouldHoldOutside(PlayerbotAI* botAI, Position const& center, float dangerRadius)
{
    Player* bot = botAI->GetBot();
    constexpr float safety = 1.0f;
    if (bot->GetExactDist2d(center) >= dangerRadius + safety + HOLD_BUFFER)
        return false;

    // Moving to a target that stands well clear of the hazard leads away from it (melee following a boss the
    // tank drags out, a healer walking to a member outside); anything else would walk back in.
    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (target && target->GetExactDist2d(center) > dangerRadius + safety + HOLD_BUFFER)
        return false;

    return true;
}

Unit* FindAttackerByEntry(PlayerbotAI* botAI, std::initializer_list<uint32> entries)
{
    GuidVector const& attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
    {
        if (std::find(entries.begin(), entries.end(), guid.GetEntry()) == entries.end())
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive())
            return unit;
    }

    return nullptr;
}

Unit* FindBoss(PlayerbotAI* botAI, std::string const& lowerCaseName)
{
    return botAI->GetAiObjectContext()->GetValue<Unit*>("find target", lowerCaseName)->Get();
}
}  // namespace TbcHazard

float TbcKeepFocusMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<DpsAssistAction*>(action) && !dynamic_cast<DpsAoeAction*>(action))
        return 1.0f;

    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return 1.0f;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive())
        return 1.0f;

    return std::find(_entries.begin(), _entries.end(), target->GetEntry()) != _entries.end() ? 0.0f : 1.0f;
}

bool TbcHazardMoveAction::LeaveCircle(Position const& center, float radius, bool stopCasting)
{
    float const exitRadius = radius + TbcHazard::EXIT_MARGIN;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!TbcHazard::FindExitPoint(bot, center, exitRadius, x, y, z))
        return false;

    if (stopCasting)
        bot->CastStop();

    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED, true);
}

bool TbcHazardMoveAction::MoveToRing(Position const& center, float radius, MovementPriority priority)
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!TbcHazard::FindRingPoint(bot, center, radius, x, y, z))
        return false;

    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, priority, true);
}
