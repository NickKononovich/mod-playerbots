/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_INTERRUPTSTRATEGY_H
#define PLAYERBOTS_INTERRUPTSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

// Group interrupt rotation: the bot assigned an enemy cast ("interrupt assignment") interrupts it with the
// spell it was assigned, whichever enemy is casting, not only its current target.
class InterruptStrategy : public Strategy
{
public:
    InterruptStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "interrupt"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;

    // Whether this bot takes part in the group interrupt rotation.
    static bool IsCoordinating(PlayerbotAI* botAI);
};

#endif
