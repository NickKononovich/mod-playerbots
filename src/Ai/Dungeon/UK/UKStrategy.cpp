/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UKStrategy.h"
#include "UKMultipliers.h"
#include "AiObjectContext.h"
#include "DungeonHazard.h"
#include "InstanceScript.h"
#include "Playerbots.h"
#include "UKTriggers.h"

void WotlkDungeonUKStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Prince Keleseth
    triggers.push_back(new TriggerNode("keleseth frost tomb",
             { NextAction("attack frost tomb", ACTION_RAID + 1) }));

    // Skarvald the Constructor & Dalronn the Controller: Skarvald first, so the ghost left over is the melee one
    // (Dalronn's unselectable ghost would keep casting Shadow Bolt); the living Dalronn's casts are interrupted.
    triggers.push_back(new TriggerNode("skarvald priority",
             { NextAction("attack skarvald", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("dalronn interrupt",
             { NextAction("dalronn interrupt", ACTION_INTERRUPT + 5) }));

    // Ingvar the Plunderer

    // No easy way to check LoS here, the pillars do not seem to count as gameobjects.
    // Not implemented for now, unsure if this is needed as a good group can probably burst through the boss
    // and just eat the debuff.
    // triggers.push_back(new TriggerNode("ingvar dreadful roar",
    //          { NextAction("ingvar hide los", ACTION_RAID + 1) }));
    // Phase 2 Shadow Axe: the Throw Dummy spins 5 yd around itself for 10 s (DungeonHazard.cpp, with the hold that
    // keeps a healer from walking back in).
    triggers.push_back(new TriggerNode("dungeon hazard",
            { NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
    // The roars cannot be kicked or outranged (60 yd): the healer keeps everybody topped up instead, and the tank
    // takes Smash / Dark Smash under a defensive cooldown.
    triggers.push_back(new TriggerNode("dungeon top up",
            { NextAction("dungeon top up", ACTION_LIGHT_HEAL + 6) }));
    triggers.push_back(new TriggerNode("dungeon tank cooldown",
            { NextAction("dungeon tank cooldown", ACTION_EMERGENCY + 2) }));
    triggers.push_back(new TriggerNode("ingvar smash tank",
            { NextAction("ingvar dodge smash", ACTION_MOVE + 5) }));
    // Everyone else leaves the cone and his boundary during the 3 s cast (Dark Smash: 8.7-11.2k).
    triggers.push_back(new TriggerNode("ingvar smash danger",
            { NextAction("ingvar avoid smash", ACTION_EMERGENCY + 5) }));
    triggers.push_back(new TriggerNode("ingvar smash tank return",
            { NextAction("ingvar smash return", ACTION_MOVE + 5) }));
    // Buggy... if not behind target, ai can get stuck running towards and away from target.
    // I think for ranged chars, a custom action should be added that doesn't attempt to run into melee.
    // This is a bandaid for now, needs to be improved.
    triggers.push_back(new TriggerNode("not behind ingvar",
            { NextAction("set behind", ACTION_MOVE + 1) }));

}

void WotlkDungeonUKStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new PrinceKelesethMultiplier(botAI));
    multipliers.push_back(new SkarvaldAndDalronnMultiplier(botAI));
    multipliers.push_back(new IngvarThePlundererMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
}

void WotlkDungeonUKStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType /*type*/)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance)
        return;

    // A forge master engaged before the previous one evades at once; chasing it keeps the group in an evade loop.
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (UKIds::IsForgeMasterLocked(unit ? unit->ToCreature() : nullptr, instance))
            exclusions.insert(guid);
    }
}
