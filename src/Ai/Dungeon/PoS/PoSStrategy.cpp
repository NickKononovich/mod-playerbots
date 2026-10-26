/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PoSStrategy.h"
#include "DungeonHazard.h"
#include "PoSMultipliers.h"
#include "PoSTriggers.h"  // [b33_pos]
#include "AiObjectContext.h"
#include "Playerbots.h"

void WotlkDungeonPoSStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("ick and krick",
        { NextAction("ick and krick", ACTION_RAID + 5) }));

    triggers.push_back(new TriggerNode("tyrannus",
        { NextAction("tyrannus", ACTION_RAID + 5) }));

    triggers.push_back(new TriggerNode("pos area trigger",
        { NextAction("pos send area trigger", ACTION_RAID + 9) }));

    triggers.push_back(new TriggerNode("pos permafrost",
        { NextAction("pos hide from permafrost", ACTION_EMERGENCY + 3) }));

    triggers.push_back(new TriggerNode("pos throw saronite",
        { NextAction("pos dodge saronite", ACTION_EMERGENCY + 4) }));

    triggers.push_back(new TriggerNode("pos icicle",
        { NextAction("pos dodge icicle", ACTION_EMERGENCY + 6) }));

    triggers.push_back(new TriggerNode("pos overlords brand",
        { NextAction("pos brand hold fire", ACTION_RAID + 8) }));

    triggers.push_back(new TriggerNode("pos mark of rimefang",
        { NextAction("pos avoid hoarfrost", ACTION_EMERGENCY + 4) }));

    // [b33_pos] Tyrannus is fought away from the freed slaves' line, where the Fallen Warriors keep walking in
    triggers.push_back(new TriggerNode("pos tyrannus position",
        { NextAction("pos tyrannus position", ACTION_RAID + 6) }));

    // Garfrost: the healer shields and tops up the Deep Freeze target before it lands (DungeonTactics.cpp).
    triggers.push_back(new TriggerNode("dungeon top up",
        { NextAction("dungeon top up", ACTION_MEDIUM_HEAL + 6) }));

    // Trash: Plagueborn Horror's Blight Bomb
    triggers.push_back(new TriggerNode("dungeon hazard",
        { NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
}

void WotlkDungeonPoSStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new IckAndKrickMultiplier(botAI));
    multipliers.push_back(new GarfrostMultiplier(botAI));
    multipliers.push_back(new PoSOverlordsBrandMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
}

// [b33_pos] Nobody picks, debuffs or counts for an AoE a Fallen Warrior that is on its way to the freed slaves or
// fights them (both wipes: 30-71 member target samples on them, AoE pulled them onto the casters).
void WotlkDungeonPoSStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType /*type*/)
{
    Player* bot = botAI->GetBot();
    if (!PoSShared::GetTyrannus(bot))
        return;

    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (PoSShared::IsSlaveFightWarrior(unit))
            exclusions.insert(guid);
    }
}
