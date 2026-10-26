/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SLMULTIPLIERS_H
#define PLAYERBOTS_SLMULTIPLIERS_H

#include "Multiplier.h"

// While Murmur casts Sonic Boom, nobody walks back into its reach.
class MurmurSonicBoomHoldMultiplier : public Multiplier
{
public:
    MurmurSonicBoomHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "murmur sonic boom hold") {}
    float GetValue(Action* action) override;
};

// A shaman in the Hellmaw fight keeps Tremor Totem instead of swapping in another earth totem.
class HellmawTremorTotemMultiplier : public Multiplier
{
public:
    HellmawTremorTotemMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hellmaw tremor totem") {}
    float GetValue(Action* action) override;
};

#endif
