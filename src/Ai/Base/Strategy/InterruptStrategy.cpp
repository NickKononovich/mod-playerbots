/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InterruptStrategy.h"
#include "Playerbots.h"

void InterruptStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Above every class interrupt (ACTION_INTERRUPT .. +2): the assigned kick goes first, on any caster.
    triggers.push_back(
        new TriggerNode("assigned interrupt", { NextAction("interrupt assigned cast", ACTION_INTERRUPT + 3) }));
}

bool InterruptStrategy::IsCoordinating(PlayerbotAI* botAI)
{
    return botAI && botAI->HasStrategy("interrupt", BOT_STATE_COMBAT);
}
