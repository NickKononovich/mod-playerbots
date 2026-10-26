/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NONCOMBATACTIONS_H
#define PLAYERBOTS_NONCOMBATACTIONS_H

#include "UseItemAction.h"

class PlayerbotAI;

class DrinkAction : public UseItemAction
{
public:
    DrinkAction(PlayerbotAI* botAI) : UseItemAction(botAI, "drink") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
};

class EatAction : public UseItemAction
{
public:
    EatAction(PlayerbotAI* botAI) : UseItemAction(botAI, "food") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
};

// Eating / drinking with nothing left to do (full, or a fight started): stand up, which ends the food / drink.
class StandUpAction : public Action
{
public:
    StandUpAction(PlayerbotAI* botAI) : Action(botAI, "stand up") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
