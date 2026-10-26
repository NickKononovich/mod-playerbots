/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BFStrategy.h"
#include "BFMultipliers.h"
#include "BFTechnician.h"
#include "DungeonHazard.h"
#include "TbcHazard.h"

void TbcDungeonBloodFurnaceStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Keli'dan the Breaker
    triggers.push_back(new TriggerNode("kelidan burning nova", {
        NextAction("kelidan leave burning nova", ACTION_EMERGENCY + 7) }));

    // Shadowmoon Technician trash: Proximity Bombs (DungeonHazard.cpp). In combat everybody keeps out of the 10 yd
    // blast, not only the 2.5 yd trigger: whoever steps on a bomb blows up everyone around it (wave 8: three deaths).
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 5) }));
    // ... and the Technicians die first (Dynamite, bombs and Silence all come from them).
    triggers.push_back(new TriggerNode("shadowmoon technician", {
        NextAction("attack shadowmoon technician", ACTION_RAID + 3) }));

    // Broggok
    triggers.push_back(new TriggerNode("broggok loose add", {
        NextAction("broggok back off", ACTION_MOVE + 6) }));
}

void TbcDungeonBloodFurnaceStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new KelidanBurningNovaHoldMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
    multipliers.push_back(new TbcKeepFocusMultiplier(botAI, "shadowmoon technician focus",
                                                     { BloodFurnaceTechnician::NPC_SHADOWMOON_TECHNICIAN }));
}
