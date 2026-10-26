/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_UBMULTIPLIERS_H
#define PLAYERBOTS_UBMULTIPLIERS_H

#include "Multiplier.h"

class HungarfenFoulSporesMultiplier : public Multiplier
{
public:
    HungarfenFoulSporesMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hungarfen foul spores") {}
    float GetValue(Action* action) override;
};

class HungarfenMushroomIgnoreMultiplier : public Multiplier
{
public:
    HungarfenMushroomIgnoreMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hungarfen mushroom ignore") {}
    float GetValue(Action* action) override;
};

class UnderbatFacingMultiplier : public Multiplier
{
public:
    UnderbatFacingMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "underbat facing") {}
    float GetValue(Action* action) override;
};

// Ghaz'an's lake: a bot on dry land does not reposition towards a target that swims (Frenzies, Ghaz'an on patrol):
// melee hit what reaches the shore, ranged shoot from it, and Ghaz'an walks out to the tank.
class UBGhazanDryLandMultiplier : public Multiplier
{
public:
    UBGhazanDryLandMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "ub ghazan dry land") {}
    float GetValue(Action* action) override;
};

#endif
