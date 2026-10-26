/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FEARCOUNTERSTRATEGY_H
#define PLAYERBOTS_FEARCOUNTERSTRATEGY_H

#include "Multiplier.h"
#include "Strategy.h"

class PlayerbotAI;

// [fear_counter] Generic fear / charm / sleep counters, per class (no dungeon list): shaman Tremor Totem
// (reactive and pre-dropped), priest Fear Ward on the tank, warrior Berserker Rage before the fear lands, and the
// PvP trinket. The racial breaks (Will of the Forsaken, Every Man for Himself) and the reactive Berserker Rage stay
// in "racials" and the warrior strategies. Added to the combat engine of every bot and the priest's non-combat
// engine (pre-pull Fear Ward).
class FearCounterStrategy : public Strategy
{
public:
    FearCounterStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "fear counter"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

// While Tremor Totem is wanted, the shaman's other earth totems (and Call of the Elements, which drops the bar's
// earth totem) would take the slot back.
class FearCounterTotemMultiplier : public Multiplier
{
public:
    FearCounterTotemMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "fear counter totem") {}

    float GetValue(Action* action) override;
};

#endif
