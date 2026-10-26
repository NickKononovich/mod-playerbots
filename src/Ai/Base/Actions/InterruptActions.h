/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_INTERRUPTACTIONS_H
#define PLAYERBOTS_INTERRUPTACTIONS_H

#include "Action.h"

class PlayerbotAI;
class SpellInfo;
class Unit;

// Casts the interrupt the group rotation assigned to this bot on the enemy whose cast it was assigned.
class InterruptAssignedCastAction : public Action
{
public:
    InterruptAssignedCastAction(PlayerbotAI* botAI) : Action(botAI, "interrupt assigned cast") {}

    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;

private:
    Unit* GetCaster();
    SpellInfo const* GetAssignedSpell();
};

#endif
