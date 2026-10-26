/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STRATSTRATEGY_H
#define PLAYERBOTS_STRATSTRATEGY_H

#include "Strategy.h"

// Stratholme (map 329), the Scarlet side streets (RESEARCH_TACTICS.md section 11): wandering Cadavers and
// Berserkers, patrolling Plague Ghouls. The members wait behind the tank at a pack's edge, the tank takes any mob
// that finds a member first, area spells wait for the tank's lead; Ghoul, then Skeletal Guardian die first, the
// Guardian's Blizzard and Arcane Explosion are kicked, nobody stands in the Berserkers' and Ghouls' cleave
// (DungeonTactics.cpp rows for map 329).
class ClassicDungeonStratholmeStrategy : public Strategy
{
public:
    ClassicDungeonStratholmeStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "classic-strat"; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;

    bool HasTargetExclusions() const override { return true; }
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;

private:
    static constexpr uint32 NPC_PLAGUE_GHOUL = 10405;
    static constexpr uint32 NPC_SKELETAL_GUARDIAN = 10390;
};

#endif
