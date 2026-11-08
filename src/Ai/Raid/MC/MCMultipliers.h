/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MCMULTIPLIERS_H
#define PLAYERBOTS_MCMULTIPLIERS_H

#include "Multiplier.h"
#include "ObjectGuid.h"

class GarrDisableDpsAoeMultiplier : public Multiplier
{
public:
    GarrDisableDpsAoeMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "garr disable dps aoe multiplier") {}
    float GetValue(Action* action) override;
};

class BaronGeddonAbilityMultiplier : public Multiplier
{
public:
    BaronGeddonAbilityMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "baron geddon ability multiplier") {}
    float GetValue(Action* action) override;
};

class GolemaggMultiplier : public Multiplier
{
public:
    GolemaggMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "golemagg multiplier") {}
    float GetValue(Action* action) override;
};

// [mcimps] a melee damage dealer whose target stands where it keeps out of the Flame Imps' Fire Nova
// (McFireNovaKeepsOut) does not walk in to melee it
class FlameImpFireNovaMultiplier : public Multiplier
{
public:
    FlameImpFireNovaMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "flame imp fire nova multiplier") {}
    float GetValue(Action* action) override;

private:
    uint32 _lookMs = 0;
    ObjectGuid _lookTarget;
    bool _keepsOut = false;
};

#endif
