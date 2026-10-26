/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SHACTIONCONTEXT_H
#define PLAYERBOTS_SHACTIONCONTEXT_H

#include "NamedObjectContext.h"
#include "SHActions.h"

class TbcDungeonShatteredHallsActionContext : public NamedObjectContext<Action>
{
public:
    TbcDungeonShatteredHallsActionContext()
    {
        creators["kargath leave blade dance"] = &TbcDungeonShatteredHallsActionContext::kargath_leave_blade_dance;
        creators["kargath keep ring"] = &TbcDungeonShatteredHallsActionContext::kargath_keep_ring;
    }

private:
    static Action* kargath_leave_blade_dance(PlayerbotAI* botAI) { return new KargathLeaveBladeDanceAction(botAI); }
    static Action* kargath_keep_ring(PlayerbotAI* botAI) { return new KargathKeepRingAction(botAI); }
};

#endif
