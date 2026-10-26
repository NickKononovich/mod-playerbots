/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UseFoodStrategy.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

void UseFoodStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);
    triggers.push_back(new TriggerNode("low health", { NextAction("food", 3.0f) }));
    triggers.push_back(new TriggerNode("low mana", { NextAction("drink", 3.0f) }));
    // Resting between pulls with nothing to fight: above an idle "dps assist" / "move to ranged position" out of
    // combat (they kept preempting the 3.0 drink at 8 % mana), below the reaction and pull sequence.
    triggers.push_back(new TriggerNode("rest mana", { NextAction("drink", ACTION_DISPEL + 5.0f) }));
    triggers.push_back(new TriggerNode("rest health", { NextAction("food", ACTION_DISPEL + 4.0f) }));
    // Full again (a food / drink aura runs up to 30 s): stand up instead of sitting out the rest of it.
    triggers.push_back(new TriggerNode("rested", { NextAction("stand up", ACTION_DISPEL + 6.0f) }));
    triggers.push_back(new TriggerNode("need food restock", { NextAction("restock food", 2.0f) }));
}
