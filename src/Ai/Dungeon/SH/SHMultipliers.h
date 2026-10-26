/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SHMULTIPLIERS_H
#define PLAYERBOTS_SHMULTIPLIERS_H

#include "Multiplier.h"

// Blade Dance: no movement back into 15 yd of Kargath while he dances (the melee's reach, a follow, a ranged
// position); the escape itself is exempt (TbcHazardMoveAction).
class KargathBladeDanceHoldMultiplier : public Multiplier
{
public:
    KargathBladeDanceHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "kargath blade dance hold") {}
    float GetValue(Action* action) override;
};

#endif
