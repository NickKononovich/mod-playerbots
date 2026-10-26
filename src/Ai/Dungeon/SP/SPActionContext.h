/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPACTIONCONTEXT_H
#define PLAYERBOTS_SPACTIONCONTEXT_H

#include "NamedObjectContext.h"
#include "SPActions.h"

class TbcDungeonSlavePensActionContext : public NamedObjectContext<Action>
{
public:
    TbcDungeonSlavePensActionContext()
    {
        creators["mennu leave nova totem"] = &TbcDungeonSlavePensActionContext::mennu_leave_nova_totem;
        creators["mennu kill totems"] = &TbcDungeonSlavePensActionContext::mennu_kill_totems;
    }

private:
    static Action* mennu_leave_nova_totem(PlayerbotAI* botAI) { return new MennuLeaveNovaTotemAction(botAI); }
    static Action* mennu_kill_totems(PlayerbotAI* botAI) { return new MennuKillTotemsAction(botAI); }
};

#endif
