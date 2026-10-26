/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MECHMULTIPLIERS_H
#define PLAYERBOTS_MECHMULTIPLIERS_H

#include "Multiplier.h"

class SepethreaKiteFlameMultiplier : public Multiplier
{
public:
    SepethreaKiteFlameMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "sepethrea kite flame") {}
    float GetValue(Action* action) override;
};

class SepethreaFocusBossMultiplier : public Multiplier
{
public:
    SepethreaFocusBossMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "sepethrea focus boss") {}
    float GetValue(Action* action) override;
};

// On the lift leg (MechanarLift::ActiveLeg) nothing else moves the bot: no follow, no walk to the harness waypoint
// (no path joins the levels), no chase while it rides.
class MechanarLiftHoldMultiplier : public Multiplier
{
public:
    MechanarLiftHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "mechanar lift hold") {}
    float GetValue(Action* action) override;
};

// Mechano-Lord Capacitus (MechanarCapacitus): no magic spell into Reflective Magic Shield, no melee damage dealer on
// him through Reflective Damage Shield (normal), and the bot of the other charge than the tank keeps its own stack
// beyond the boss instead of chasing back into the tank's pulse (heroic Polarity Shift).
class CapacitusMultiplier : public Multiplier
{
public:
    CapacitusMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "capacitus") {}
    float GetValue(Action* action) override;
};

#endif
