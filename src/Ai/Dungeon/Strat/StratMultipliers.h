/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STRATMULTIPLIERS_H
#define PLAYERBOTS_STRATMULTIPLIERS_H

#include "Multiplier.h"

// Out of combat, "follow" does not carry a member from a safe spot behind the tank onto a formation spot an idle
// street mob would aggro on first (StratShared.h); a member far from the tank always catches up.
class StratHoldBehindTankMultiplier : public Multiplier
{
public:
    StratHoldBehindTankMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "strat hold behind tank") {}
    float GetValue(Action* action) override;
};

// A damage dealer or healer holds its area spells and the Magma / Fire Nova totems until the tank has held every mob
// of the fight for STRAT_AOE_HOLD_MS: a pull of five elites burns through 2-2.9k health in 3-4 s once it turns.
class StratAoeOpenerHoldMultiplier : public Multiplier
{
public:
    StratAoeOpenerHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "strat aoe opener hold") {}
    float GetValue(Action* action) override;

private:
    uint32 _heldSinceMs = 0;
    uint32 _checkedMs = 0;
    bool _hold = false;
};

#endif
