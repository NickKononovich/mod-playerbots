/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SVACTIONCONTEXT_H
#define PLAYERBOTS_SVACTIONCONTEXT_H

#include "NamedObjectContext.h"
#include "SVActions.h"

class TbcDungeonSteamvaultActionContext : public NamedObjectContext<Action>
{
public:
    TbcDungeonSteamvaultActionContext()
    {
        creators["kalithresh kill distiller"] = &TbcDungeonSteamvaultActionContext::kalithresh_kill_distiller;
        creators["kalithresh stop casting"] = &TbcDungeonSteamvaultActionContext::kalithresh_stop_casting;
        creators["sv taunt bog overlord"] = &TbcDungeonSteamvaultActionContext::sv_taunt_bog_overlord;
    }

private:
    static Action* kalithresh_kill_distiller(PlayerbotAI* botAI) { return new KalithreshKillDistillerAction(botAI); }
    static Action* kalithresh_stop_casting(PlayerbotAI* botAI) { return new KalithreshStopCastingAction(botAI); }
    static Action* sv_taunt_bog_overlord(PlayerbotAI* botAI) { return new SvTauntBogOverlordAction(botAI); }
};

#endif
