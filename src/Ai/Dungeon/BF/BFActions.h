/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BFACTIONS_H
#define PLAYERBOTS_BFACTIONS_H

#include "TbcHazard.h"

// Keli'dan the Breaker: everyone (the tank too, he is stunned for the cast) runs out of Fire Nova range during
// Burning Nova and comes back once it has gone off.
class KelidanLeaveBurningNovaAction : public TbcHazardMoveAction
{
public:
    KelidanLeaveBurningNovaAction(PlayerbotAI* botAI)
        : TbcHazardMoveAction(botAI, "kelidan leave burning nova") {}
    bool Execute(Event event) override;
};

// Broggok: a ranged bot or healer backs away from a cell orc that is loose, leaving it to the tank.
class BroggokBackOffAction : public TbcHazardMoveAction
{
public:
    BroggokBackOffAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "broggok back off") {}
    bool Execute(Event event) override;
};

#endif
