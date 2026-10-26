/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CHEATACTION_H
#define PLAYERBOTS_CHEATACTION_H

#include "Action.h"

class PlayerbotAI;

class CheatAction : public Action
{
public:
    CheatAction(PlayerbotAI* botAI) : Action(botAI, "cheat") {}

    // Changes nothing: bots have no cheats (see the .cpp); it only tells the master so.
    bool Execute(Event event) override;
};

#endif
