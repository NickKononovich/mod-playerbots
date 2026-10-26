/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDLEADERSTRATEGY_H
#define PLAYERBOTS_RAIDLEADERSTRATEGY_H

#include "Multiplier.h"
#include "Strategy.h"

class PlayerbotAI;

// Every bot carries it: the coordinator's triggers fire only on the coordinator, the multiplier on everyone.
class RaidLeaderStrategy : public Strategy
{
public:
    RaidLeaderStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "raid leader"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

// Out of combat in raid instances: own flask, buff food, pre-pull potion, Soulwell.
class RaidConsumablesStrategy : public Strategy
{
public:
    RaidConsumablesStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "raid consumables"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// Holds attacks before the ready check passes, for a few seconds after the boss pull, and after a wipe call.
class RaidLeaderHoldMultiplier : public Multiplier
{
public:
    RaidLeaderHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "raid leader hold") {}

    float GetValue(Action* action) override;
    // [lead15] the executor's vetoes are the leader's: no no-damage release of them
    bool KeepsVeto(Action* action) override;
};

#endif
