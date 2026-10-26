/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MTACTIONCONTEXT_H
#define PLAYERBOTS_MTACTIONCONTEXT_H

#include "MTActions.h"
#include "NamedObjectContext.h"

class TbcDungeonManaTombsActionContext : public NamedObjectContext<Action>
{
public:
    TbcDungeonManaTombsActionContext()
    {
        creators["shaffar kill beacon"] = &TbcDungeonManaTombsActionContext::shaffar_kill_beacon;
        creators["pandemonius stop attack"] = &TbcDungeonManaTombsActionContext::pandemonius_stop_attack;
    }

private:
    static Action* shaffar_kill_beacon(PlayerbotAI* botAI) { return new ShaffarKillBeaconAction(botAI); }
    static Action* pandemonius_stop_attack(PlayerbotAI* botAI) { return new PandemoniusStopAttackAction(botAI); }
};

#endif
