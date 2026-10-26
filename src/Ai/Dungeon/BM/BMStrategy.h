/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BMSTRATEGY_H
#define PLAYERBOTS_BMSTRATEGY_H

#include "Strategy.h"

#include <string>
#include <vector>

// Caverns of Time: The Black Morass (BMShared.h has what the AzerothCore event does).
class TbcDungeonBlackMorassStrategy : public Strategy
{
public:
    TbcDungeonBlackMorassStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "tbc-bm"; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    // Without it the engine never asks AppendTargetExclusions (bm-n 2026-10-25: dps assist kept a far Rift Keeper).
    bool HasTargetExclusions() const override { return true; }
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;
};

#endif
