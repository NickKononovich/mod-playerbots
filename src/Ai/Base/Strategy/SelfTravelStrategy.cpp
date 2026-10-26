/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SelfTravelStrategy.h"

#include "Playerbots.h"

void InstancePortalStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("instance portal", { NextAction("move to instance portal", _relevance) }));
}

void BreatheStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("low breath", { NextAction("surface for air", ACTION_EMERGENCY + 5.0f) }));
}
