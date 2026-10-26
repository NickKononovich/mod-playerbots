/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArcStrategy.h"
#include "ArcMultipliers.h"
#include "DungeonHazard.h"
#include "DungeonTactics.h"

void TbcDungeonArcatrazStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Dalliah the Doomsayer
    triggers.push_back(new TriggerNode("dalliah whirlwind", {
        NextAction("dalliah leave whirlwind", ACTION_EMERGENCY + 6) }));

    // Arcatraz Sentinels (Energy Discharge, Explode at 10 %) and Destroyed Sentinels
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
    // Melee damage dealers leave the Sentinels to the ranged (another target, or wait out of the discharge)
    triggers.push_back(new TriggerNode("dungeon melee hold", {
        NextAction("dungeon melee hold", ACTION_EMERGENCY + 1) }));

    // Gargantuan Abyssal: Meteor is split between everyone it hits
    triggers.push_back(new TriggerNode("abyssal meteor", {
        NextAction("abyssal meteor stack", ACTION_EMERGENCY + 3) }));

    // Warden Mellichar's prison event before Harbinger Skyriss
    triggers.push_back(new TriggerNode("arcatraz warden room", {
        NextAction("arcatraz stay in warden room", ACTION_RAID + 3) }));
    triggers.push_back(new TriggerNode("arcatraz prisoner", {
        NextAction("arcatraz attack prisoner", ACTION_RAID + 2) }));
}

void TbcDungeonArcatrazStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new DalliahWhirlwindHoldMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
    multipliers.push_back(new AbyssalMeteorStackMultiplier(botAI));
}

void TbcDungeonArcatrazStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::DpsTarget || type == TargetValueExclusionType::DebuffTarget)
        DungeonTactics::AppendMeleeHoldExclusions(botAI, exclusions);
}
