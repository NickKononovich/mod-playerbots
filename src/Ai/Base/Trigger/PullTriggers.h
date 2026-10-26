/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

/*
 * Ported from the CMaNGOS playerbots project (https://github.com/cmangos/playerbots), GPL v2,
 * with modifications for AzerothCore.
 * Original authors:
 *   David Parra Ausina (davidonete/Flekz) <davidparraausina@gmail.com> - original author
 *   Sebastiaan Keek (mostlikely4r) <sebastiaan.keek@gmail.com>
 *   Cyberium <cyberium@users.noreply.github.com>
 */

#ifndef PLAYERBOTS_PULLTRIGGERS_H
#define PLAYERBOTS_PULLTRIGGERS_H

#include "Trigger.h"

class PullStartTrigger : public Trigger
{
public:
    PullStartTrigger(PlayerbotAI* botAI, std::string const name = "pull start") : Trigger(botAI, name) {}

    bool IsActive() override;
};

class PullEndTrigger : public Trigger
{
public:
    PullEndTrigger(PlayerbotAI* botAI, std::string const name = "pull end") : Trigger(botAI, name) {}

    bool IsActive() override;
};

class ReturnToPullPositionTrigger : public Trigger
{
public:
    ReturnToPullPositionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "return to pull position") {}

    bool IsActive() override;
};

// [chain_pull_in_combat] A dungeon tank finishing the last mob of its fight (one attacker left, not a boss, below
// CHAIN_PULL_HEALTH_PCT or about to die): the next small pack close by can be pulled now ("chain pull").
class ChainPullTrigger : public Trigger
{
public:
    ChainPullTrigger(PlayerbotAI* botAI) : Trigger(botAI, "chain pull") {}

    bool IsActive() override;
};

// A group member is away from the group spot of its tank's line-of-sight pull (LosPull in PullStrategy.h).
class LosPullGatherTrigger : public Trigger
{
public:
    LosPullGatherTrigger(PlayerbotAI* botAI) : Trigger(botAI, "los pull gather") {}

    bool IsActive() override;
};

#endif
