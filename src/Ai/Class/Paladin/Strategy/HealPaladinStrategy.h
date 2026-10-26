/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HEALPALADINSTRATEGY_H
#define PLAYERBOTS_HEALPALADINSTRATEGY_H

#include "GenericPaladinStrategy.h"

class PlayerbotAI;

// While a group member is below HEALS_FIRST_HEALTH_PCT the healer casts nothing at a mob (Judgement, Exorcism,
// Holy Shock as damage, ...) and does not turn to its target: only the heals and its crowd control stay.
class HolyHealsFirstMultiplier : public Multiplier
{
public:
    static constexpr float HEALS_FIRST_HEALTH_PCT = 70.0f;

    HolyHealsFirstMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "heals first") {}

    float GetValue(Action* action) override;
};

class HealPaladinStrategy : public GenericPaladinStrategy
{
public:
    HealPaladinStrategy(PlayerbotAI* botAI);

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "holy"; }
    std::vector<NextAction> getDefaultActions() override;
    uint32 GetType() const override { return STRATEGY_TYPE_HEAL | STRATEGY_TYPE_RANGED; }
};

#endif
