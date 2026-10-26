/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RANGEDCOMBATSTRATEGY_H
#define PLAYERBOTS_RANGEDCOMBATSTRATEGY_H

#include "CombatStrategy.h"

class PlayerbotAI;

class RangedCombatStrategy : public CombatStrategy
{
public:
    RangedCombatStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI) {}
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    uint32 GetType() const override { return STRATEGY_TYPE_COMBAT | STRATEGY_TYPE_RANGED; }
    std::string const getName() override { return "ranged"; }
};

// While the party's tank fights, a healer or ranged bot stands like a player would: behind the tank, out of the
// pack's melee and cleaves, and (healer) inside heal range and sight of the tank. Added to both engines: a healer
// still out of combat walks into range of a tank that has engaged instead of following it into the pack.
class RangedPositionStrategy : public Strategy
{
public:
    RangedPositionStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "ranged position"; }
};

// Out of combat a follower walks up to its leader; with the tank already fighting that walks a healer or ranged
// bot into the pack. Following waits while the fight lasts; the ranged position triggers keep the bot in range.
class RangedPositionFollowMultiplier : public Multiplier
{
public:
    RangedPositionFollowMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "ranged position follow") {}

    float GetValue(Action* action) override;

private:
    static constexpr uint32 FOLLOW_RELEASE_LOG_MS = 10000;
    uint32 _loggedMs = 0;
};

#endif
