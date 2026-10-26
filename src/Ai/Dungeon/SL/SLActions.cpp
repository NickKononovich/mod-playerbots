/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SLActions.h"
#include "CharmInfo.h"
#include "Group.h"
#include "PetDefines.h"
#include "Playerbots.h"
#include "SLShared.h"

using namespace ShadowLabyrinth;

bool MurmurLeaveSonicBoomAction::Execute(Event /*event*/)
{
    Unit* murmur = TbcHazard::FindBoss(botAI, MURMUR_NAME);
    if (!IsCastingSonicBoom(murmur))
        return false;

    return LeaveCircle(murmur->GetPosition(), SonicBoomDangerRadius(bot), true);
}

bool MurmurAvoidThunderingStormAction::Execute(Event /*event*/)
{
    Unit* murmur = TbcHazard::FindBoss(botAI, MURMUR_NAME);
    if (!murmur)
        return false;

    return MoveToRing(murmur->GetPosition(), MURMUR_RANGED_RING, MovementPriority::MOVEMENT_COMBAT);
}

bool MurmursTouchSpreadAction::Execute(Event /*event*/)
{
    return MoveFromGroup(SHOCKWAVE_SPREAD);
}

bool BlackheartPetsAction::Execute(Event /*event*/)
{
    bool const fighting = TbcHazard::FindAttackerByEntry(botAI, {NPC_BLACKHEART}) != nullptr;
    bool acted = false;
    for (Unit* controlled : bot->m_Controlled)
    {
        Creature* pet = controlled ? controlled->ToCreature() : nullptr;
        CharmInfo* charmInfo = pet ? pet->GetCharmInfo() : nullptr;
        if (!pet || !pet->IsAlive() || pet->IsTotem() || !charmInfo)
            continue;

        if (fighting)
        {
            if (pet->HasReactState(REACT_PASSIVE))
                continue;

            _parked[pet->GetGUID()] = pet->GetReactState();
            pet->SetReactState(REACT_PASSIVE);
            pet->AttackStop();
            charmInfo->SetIsCommandAttack(false);
            charmInfo->SetCommandState(COMMAND_FOLLOW);
            charmInfo->SetIsCommandFollow(true);
            pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
            acted = true;
            continue;
        }

        auto const parked = _parked.find(pet->GetGUID());
        if (parked == _parked.end())
            continue;

        pet->SetReactState(parked->second);
        _parked.erase(parked);
        acted = true;
    }

    if (!fighting)
        _parked.clear();

    return acted;
}

bool BlackheartSpreadAction::Execute(Event /*event*/)
{
    Player* nearest = nullptr;
    Group* group = bot->GetGroup();
    for (GroupReference* ref = group ? group->GetFirstMember() : nullptr; ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId())
            continue;

        if (!nearest || bot->GetExactDist2d(member) < bot->GetExactDist2d(nearest))
            nearest = member;
    }

    float const spread = BlackheartSpread(bot);
    if (!nearest || bot->GetExactDist2d(nearest) >= spread)
        return false;

    return LeaveCircle(nearest->GetPosition(), spread - TbcHazard::EXIT_MARGIN + 1.0f, false);
}
