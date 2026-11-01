/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AggressiveStrategy.h"
#include "Playerbots.h"

void AggressiveStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode(
            "no target",
            {
                NextAction("aggressive target", 4.0f)
            }
        )
    );
    // [lead95] the leader's issued pull order is taken first (above the out-of-combat buffs and the ready reply;
    // under the drink, which the leader's readiness has ended before it issued)
    triggers.push_back(
        new TriggerNode(
            "leader pull issued",
            {
                NextAction("aggressive target", 50.0f)
            }
        )
    );
}
