/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArcActions.h"
#include "ArcShared.h"
#include "Playerbots.h"

using namespace Arcatraz;

bool DalliahLeaveWhirlwindAction::Execute(Event /*event*/)
{
    Unit* dalliah = TbcHazard::FindBoss(botAI, DALLIAH_NAME);
    if (!dalliah || !dalliah->HasAura(SPELL_DALLIAH_WHIRLWIND))
        return false;

    return LeaveCircle(dalliah->GetPosition(), WhirlwindDangerRadius(bot, dalliah), false);
}

bool ArcStayInWardenRoomAction::Execute(Event /*event*/)
{
    if (!IsWardenEventActive(bot))
        return false;

    return MoveNear(bot->GetMapId(), WARDEN_ROOM_X, WARDEN_ROOM_Y, WARDEN_ROOM_Z, WARDEN_ROOM_RETURN,
                    MovementPriority::MOVEMENT_COMBAT);
}

bool ArcAttackPrisonerAction::Execute(Event /*event*/)
{
    Unit* prisoner = FindReleasedPrisoner(botAI);
    if (!prisoner)
        return false;

    return Attack(prisoner);
}

bool AbyssalMeteorStackAction::Execute(Event /*event*/)
{
    Position point;
    if (!FindMeteorPoint(botAI, point) || bot->GetExactDist2d(point) <= METEOR_STACK_DISTANCE)
        return false;

    return MoveTo(bot->GetMapId(), point.GetPositionX(), point.GetPositionY(), point.GetPositionZ(), false, false,
                  false, false, MovementPriority::MOVEMENT_FORCED, true);
}
