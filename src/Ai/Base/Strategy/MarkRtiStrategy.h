/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MARKRTISTRATEGY_H
#define PLAYERBOTS_MARKRTISTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

class MarkRtiStrategy : public Strategy
{
public:
    MarkRtiStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "mark rti"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// Crowd control bots mark their own CC icon on a trash pack mob at the pull and take it back when stale.
class AutoCcMarksStrategy : public Strategy
{
public:
    AutoCcMarksStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "auto cc marks"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// Out of combat: crowd control bots answer the tank's pre-pull plan (PrePullCc in PullStrategy.h), claim and mark a
// mob from the tail of the pack's kill order, and sap it or lay a trap before the pull.
class PrePullCcStrategy : public Strategy
{
public:
    PrePullCcStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "pre pull cc"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// The main tank marks Skull and Cross on the trash it holds so the dps focus one mob, casters first.
class TankMarksStrategy : public Strategy
{
public:
    TankMarksStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "tank marks"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif
