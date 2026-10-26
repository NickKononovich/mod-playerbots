/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STRATACTIONCONTEXT_H
#define PLAYERBOTS_STRATACTIONCONTEXT_H

#include "NamedObjectContext.h"
#include "StratActions.h"

class ClassicDungeonStratholmeActionContext : public NamedObjectContext<Action>
{
public:
    ClassicDungeonStratholmeActionContext()
    {
        creators["strat step behind tank"] = &ClassicDungeonStratholmeActionContext::strat_step_behind_tank;
        creators["strat defend on attacked group member"] = &ClassicDungeonStratholmeActionContext::strat_defend_member;
    }

private:
    static Action* strat_step_behind_tank(PlayerbotAI* botAI) { return new StratStepBehindTankAction(botAI); }
    static Action* strat_defend_member(PlayerbotAI* botAI) { return new StratDefendMemberAction(botAI); }
};

#endif
