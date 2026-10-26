/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ARCACTIONS_H
#define PLAYERBOTS_ARCACTIONS_H

#include "AttackAction.h"
#include "TbcHazard.h"

// Dalliah the Doomsayer: everyone, the tank included, steps out of Whirlwind for its 6 s.
class DalliahLeaveWhirlwindAction : public TbcHazardMoveAction
{
public:
    DalliahLeaveWhirlwindAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "dalliah leave whirlwind") {}
    bool Execute(Event event) override;
};

// Prison event: back to the warden before the group leaves his 100 yd check or drags a prisoner into an evade.
class ArcStayInWardenRoomAction : public MovementAction
{
public:
    ArcStayInWardenRoomAction(PlayerbotAI* botAI) : MovementAction(botAI, "arcatraz stay in warden room") {}
    bool Execute(Event event) override;
};

// Prison event: the tank and every damage dealer turn on the released prisoner.
class ArcAttackPrisonerAction : public AttackAction
{
public:
    ArcAttackPrisonerAction(PlayerbotAI* botAI) : AttackAction(botAI, "arcatraz attack prisoner") {}
    bool Execute(Event event) override;
};

// Gargantuan Abyssal Meteor: run onto the landing point so the split damage is shared by the whole group.
class AbyssalMeteorStackAction : public MovementAction
{
public:
    AbyssalMeteorStackAction(PlayerbotAI* botAI) : MovementAction(botAI, "abyssal meteor stack") {}
    bool Execute(Event event) override;
};

#endif
