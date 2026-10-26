/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ARCMULTIPLIERS_H
#define PLAYERBOTS_ARCMULTIPLIERS_H

#include "Multiplier.h"

// While Dalliah whirlwinds, nobody walks back in to melee her.
class DalliahWhirlwindHoldMultiplier : public Multiplier
{
public:
    DalliahWhirlwindHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "dalliah whirlwind hold") {}
    float GetValue(Action* action) override;
};

// While a Meteor is cast, only the stack (and hazard escapes) may move a bot that is not on the landing point.
class AbyssalMeteorStackMultiplier : public Multiplier
{
public:
    AbyssalMeteorStackMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "abyssal meteor stack") {}
    float GetValue(Action* action) override;
};

#endif
