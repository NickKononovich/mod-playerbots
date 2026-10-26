/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPMULTIPLIERS_H
#define PLAYERBOTS_SPMULTIPLIERS_H

#include "Multiplier.h"

// Once out of a nova totem's reach, do not walk back in after the boss or a formation spot.
class MennuNovaTotemHoldMultiplier : public Multiplier
{
public:
    MennuNovaTotemHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "mennu nova totem hold") {}
    float GetValue(Action* action) override;
};

#endif
