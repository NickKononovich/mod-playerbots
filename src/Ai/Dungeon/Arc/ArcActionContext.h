/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ARCACTIONCONTEXT_H
#define PLAYERBOTS_ARCACTIONCONTEXT_H

#include "ArcActions.h"
#include "NamedObjectContext.h"

class TbcDungeonArcatrazActionContext : public NamedObjectContext<Action>
{
public:
    TbcDungeonArcatrazActionContext()
    {
        creators["dalliah leave whirlwind"] = &TbcDungeonArcatrazActionContext::dalliah_leave_whirlwind;
        creators["arcatraz stay in warden room"] = &TbcDungeonArcatrazActionContext::arcatraz_stay_in_warden_room;
        creators["arcatraz attack prisoner"] = &TbcDungeonArcatrazActionContext::arcatraz_attack_prisoner;
        creators["abyssal meteor stack"] = &TbcDungeonArcatrazActionContext::abyssal_meteor_stack;
    }

private:
    static Action* dalliah_leave_whirlwind(PlayerbotAI* botAI) { return new DalliahLeaveWhirlwindAction(botAI); }
    static Action* arcatraz_stay_in_warden_room(PlayerbotAI* botAI) { return new ArcStayInWardenRoomAction(botAI); }
    static Action* arcatraz_attack_prisoner(PlayerbotAI* botAI) { return new ArcAttackPrisonerAction(botAI); }
    static Action* abyssal_meteor_stack(PlayerbotAI* botAI) { return new AbyssalMeteorStackAction(botAI); }
};

#endif
