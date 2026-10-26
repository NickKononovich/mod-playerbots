/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoSStrategy.h"
#include "CoSMultipliers.h"

void WotlkDungeonCoSStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Event chain for client-less groups: Chromie -> Arcane Disruptor on 5 crates -> Chromie -> Arthas
    triggers.push_back(new TriggerNode("cos chromie start",
        { NextAction("cos get arcane disruptor", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("cos suspicious crate",
        { NextAction("cos dispel crate", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("cos chromie middle",
        { NextAction("cos start arthas intro", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("cos arthas gossip",
        { NextAction("cos arthas gossip", ACTION_RAID + 2) }));
    // Waves and the heroic Infinite Corruptor: walk to them; otherwise stay with Arthas
    triggers.push_back(new TriggerNode("cos event target",
        { NextAction("cos engage event target", ACTION_MOVE + 6) }));
    triggers.push_back(new TriggerNode("cos follow arthas",
        { NextAction("cos follow arthas", ACTION_MOVE + 4) }));

    // Meathook
    // Can tank this in a fixed position to allow healer to LoS the stun, probably not necessary

    // Salramm the Fleshcrafter
    triggers.push_back(new TriggerNode("explode ghoul",
        { NextAction("explode ghoul spread", ACTION_MOVE + 5) }));

    // Chrono-Lord Epoch
    // Not sure if this actually works, I think I've seen him charge melee characters..?
    triggers.push_back(new TriggerNode("epoch ranged",
        { NextAction("epoch stack", ACTION_MOVE + 5) }));

    // Mal'Ganis

    // Infinite Corruptor (Heroic only)
}

void WotlkDungeonCoSStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new EpochMultiplier(botAI));
    multipliers.push_back(new CoSCrateMountMultiplier(botAI));
    multipliers.push_back(new CoSLastCityIdlePullMultiplier(botAI));
    multipliers.push_back(new CoSTimedRunCcMultiplier(botAI));  // [b39_cos]
}
