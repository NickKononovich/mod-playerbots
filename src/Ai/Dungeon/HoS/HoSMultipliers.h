/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HOSMULTIPLIERS_H
#define PLAYERBOTS_HOSMULTIPLIERS_H

#include "Multiplier.h"

class KrystallusMultiplier : public Multiplier
{
    public:
        KrystallusMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "krystallus") {}

    public:
        float GetValue(Action* action) override;
};

class SjonnirMultiplier : public Multiplier
{
    public:
        SjonnirMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "sjonnir the ironshaper") {}

    public:
        float GetValue(Action* action) override;
};

// Tribunal of Ages: no wipe escape (Feign Death, Vanish, ...) inside a Searing Gaze or Dark Matter; out first.
class TribunalMultiplier : public Multiplier
{
    public:
        TribunalMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tribunal of ages") {}

    public:
        float GetValue(Action* action) override;
};

// Tribunal of Ages: the tank's movement leash around its hold spot next to Brann.
class TribunalHoldMultiplier : public Multiplier
{
    public:
        TribunalHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tribunal hold") {}

    public:
        float GetValue(Action* action) override;
};

// Tribunal of Ages: no walk to a unit standing in a Searing Gaze or Dark Matter (the tank's add, the healer's tank).
class TribunalHazardKeepOutMultiplier : public Multiplier
{
    public:
        TribunalHazardKeepOutMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tribunal hazard keep out") {}

    public:
        float GetValue(Action* action) override;
};

// [b33_hos] Tribunal of Ages: no walk out to an add far from the hold spot (tank: beyond its leash, damage dealers:
// beyond TRIBUNAL_DPS_REACH, the healer: to no enemy at all), and no tank gap closer (Charge, Intercept, Intervene)
// that lands beyond the leash.
class TribunalNoChaseMultiplier : public Multiplier
{
    public:
        TribunalNoChaseMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tribunal no chase") {}

    public:
        float GetValue(Action* action) override;
};

#endif
