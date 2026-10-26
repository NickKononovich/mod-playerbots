/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_POSMULTIPLIERS_H
#define PLAYERBOTS_POSMULTIPLIERS_H

#include "Multiplier.h"

class IckAndKrickMultiplier : public Multiplier
{
    public:
    IckAndKrickMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "ick and krick") {}

    public:
        float GetValue(Action* action) override;
};

// A non-tank hiding from Permafrost (stacks high, or already behind a Saronite Rock) does not walk back out.
class GarfrostMultiplier : public Multiplier
{
public:
    GarfrostMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "garfrost") { }

    float GetValue(Action* action) override;

private:
    bool IsHiding();

    uint32 hideCheckAt = 0;
    bool hiding = false;
};

// Overlord's Brand: the bearer neither heals (copied x5.5 onto Tyrannus) nor, unless tanking, deals damage.
class PoSOverlordsBrandMultiplier : public Multiplier
{
public:
    PoSOverlordsBrandMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "pos overlords brand") {}

    float GetValue(Action* action) override;
};

#endif
