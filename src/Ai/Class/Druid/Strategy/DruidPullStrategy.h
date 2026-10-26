/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DRUIDPULLSTRATEGY_H
#define PLAYERBOTS_DRUIDPULLSTRATEGY_H

#include "PullStrategy.h"

class DruidPullStrategy : public PullStrategy
{
public:
    DruidPullStrategy(PlayerbotAI* botAI) : PullStrategy(botAI, "faerie fire", "dire bear form") {}

    std::string GetPullActionName() const override;
    std::string GetPreActionName() const override;
    // [b32_tank_aoe] a bear tank out of bear form: (Dire) Bear Form at the pull commit and before the walk to it
    std::string GetPrePullBuffActionName() const override { return GetTankFormActionName(); }
    std::string GetApproachFormActionName() const override { return GetTankFormActionName(); }

private:
    std::string GetTankFormActionName() const;
};

#endif
