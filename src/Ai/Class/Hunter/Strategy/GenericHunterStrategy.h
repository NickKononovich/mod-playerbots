/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GENERICHUNTERSTRATEGY_H
#define PLAYERBOTS_GENERICHUNTERSTRATEGY_H

#include "CombatStrategy.h"
#include "Strategy.h"

class PlayerbotAI;

class GenericHunterStrategy : public CombatStrategy
{
public:
    GenericHunterStrategy(PlayerbotAI* botAI);

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "hunter"; }
    uint32 GetType() const override { return CombatStrategy::GetType() | STRATEGY_TYPE_RANGED | STRATEGY_TYPE_DPS; }

protected:
    // [rot640] A list entry for a spell the bot has not learned is not pushed at all. The engine would create its
    // node, resolve its target and then refuse it on the very same cached "spell id" this reads (SpellIdValue, 20 s
    // per name, the bot's own spell map and active spec), so the choice cannot change - only the work disappears.
    // b483 (hunters of level 19-26, who know none of the three): kill command 114, silencing shot 105 and kill shot
    // 93 refusals out of the window's 351 for Marksmanship. None of these actions has an ActionNode of its own, so
    // no prerequisite, alternative or continuer was ever pushed in their stead either.
    // [rot640] The gate sat in Marksmanship's file alone, so Survival and Beast Mastery kept pushing every entry:
    // an 84 min guild window (wc-n, Beast Mastery L24) holds `steady shot`, `kill shot` and `aimed shot` 215 times
    // each, and the log says why on each one - "spell not known (no spell id)", Steady Shot being learned at level
    // 50. Lives here so one gate serves all three specs.
    bool KnowsSpell(char const* spell) const;
};

class AoEHunterStrategy : public CombatStrategy
{
public:
    AoEHunterStrategy(PlayerbotAI* botAI);

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "aoe"; }
};

class HunterCcStrategy : public Strategy
{
public:
    HunterCcStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "cc"; }
};

class HunterTrapWeaveStrategy : public Strategy
{
public:
    HunterTrapWeaveStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "trap weave"; }
};

#endif
