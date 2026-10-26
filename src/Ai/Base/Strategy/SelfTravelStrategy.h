/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFTRAVELSTRATEGY_H
#define PLAYERBOTS_SELFTRAVELSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

// Into the instance through its portal on the bot's own feet or wings (MoveToInstancePortalAction): alive outside
// the dungeon its group is in, or bound for another wing ("instance portal", non-combat), and as a ghost whose
// corpse lies inside ("ghost portal", dead state - above the dead strategy's "find corpse", which cannot walk to
// a corpse on another map). Opt-in per bot: "nc +instance portal", "dead +ghost portal".
class InstancePortalStrategy : public Strategy
{
public:
    InstancePortalStrategy(PlayerbotAI* botAI, std::string const name, float relevance)
        : Strategy(botAI), _name(name), _relevance(relevance)
    {
    }

    std::string const getName() override { return _name; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;

private:
    std::string const _name;
    float const _relevance;
};

// Up for air before the breath runs out, in a fight too (LowBreathTrigger, SurfaceForAirAction). Opt-in per bot:
// "co +breathe", "nc +breathe".
class BreatheStrategy : public Strategy
{
public:
    BreatheStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "breathe"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif
