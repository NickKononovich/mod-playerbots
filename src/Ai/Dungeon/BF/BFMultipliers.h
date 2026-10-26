/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BFMULTIPLIERS_H
#define PLAYERBOTS_BFMULTIPLIERS_H

#include "Multiplier.h"

// While Burning Nova is on Keli'dan, nobody walks back in to melee or chase him.
class KelidanBurningNovaHoldMultiplier : public Multiplier
{
public:
    KelidanBurningNovaHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "kelidan burning nova hold") {}
    float GetValue(Action* action) override;
};

#endif
