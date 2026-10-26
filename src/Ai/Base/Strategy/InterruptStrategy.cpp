/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InterruptStrategy.h"
#include "Playerbots.h"
#include "ThreatStrategy.h"  // [rot2] LeaderCommandMultiplier

void InterruptStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Above every class interrupt (ACTION_INTERRUPT .. +2): the assigned kick goes first, on any caster.
    triggers.push_back(
        new TriggerNode("assigned interrupt", { NextAction("interrupt assigned cast", ACTION_INTERRUPT + 3) }));

    // [rot2] The dungeon leader's assigned casts (LeaderCommand.h), inert for a bot it does not lead: the external
    // save / buff (above the rotation and the heals, below the survival band's top), the tank's threat take-back (at
    // once, above the interrupts and its own taunts), the crowd control on the assigned mob.
    triggers.push_back(new TriggerNode("leader external", { NextAction("leader external", ACTION_EMERGENCY + 1) }));
    triggers.push_back(new TriggerNode("leader threat", { NextAction("leader threat", ACTION_INTERRUPT + 4) }));
    triggers.push_back(new TriggerNode("leader cc", { NextAction("leader cc", ACTION_INTERRUPT + 2.5f) }));
    // [rot3] the leader's PreSave: the bot's best personal save, above everything but the leader's moves
    triggers.push_back(new TriggerNode("leader pre save", { NextAction("leader pre save", ACTION_EMERGENCY + 3) }));
}

bool InterruptStrategy::IsCoordinating(PlayerbotAI* botAI)
{
    return botAI && botAI->HasStrategy("interrupt", BOT_STATE_COMBAT);
}

void InterruptStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new LeaderCommandMultiplier(botAI));  // [rot2]
}
