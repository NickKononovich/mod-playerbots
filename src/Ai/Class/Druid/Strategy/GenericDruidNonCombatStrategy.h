/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GENERICDRUIDNONCOMBATSTRATEGY_H
#define PLAYERBOTS_GENERICDRUIDNONCOMBATSTRATEGY_H

#include "NonCombatStrategy.h"

class PlayerbotAI;

class GenericDruidNonCombatStrategy : public NonCombatStrategy
{
public:
    GenericDruidNonCombatStrategy(PlayerbotAI* botAI);

    std::string const getName() override { return "nc"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

// [rot1] A Balance druid in a group with a living healer heals a party member only below BALANCE_HEAL_PCT (50 %) and
// keeps Thorns to itself: each heal or buff on another member costs the moonkin a GCD, the form (13 % base mana to
// shift back) and its Moonkin Aura (+5 % spell crit to the party). strat-h: Rejuvenation on the Fury warrior at 84-91 %
// ("almost full health", 85 %) took the druid out of form three times, once to the end of the run.
class BalanceGroupSupportMultiplier : public Multiplier
{
public:
    static constexpr uint32 LOG_MS = 10 * IN_MILLISECONDS;

    BalanceGroupSupportMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "balance group support") {}

    float GetValue(Action* action) override;

private:
    uint32 _loggedMs = 0;
};

class GenericDruidBuffStrategy : public NonCombatStrategy
{
public:
    GenericDruidBuffStrategy(PlayerbotAI* botAI);

    std::string const getName() override { return "buff"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif
