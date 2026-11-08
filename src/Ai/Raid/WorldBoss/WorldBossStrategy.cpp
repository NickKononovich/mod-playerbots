/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldBossStrategy.h"

void WorldBossStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // survival: a carried bomb / mark / infection leaves the group; nobody stands in Manastorm or a Dream Fog
    triggers.push_back(
        new TriggerNode("world boss carrier", {NextAction("world boss carrier move", ACTION_EMERGENCY + 3)}));
    triggers.push_back(
        new TriggerNode("world boss move out", {NextAction("world boss move out of area", ACTION_EMERGENCY + 2)}));

    // tanks: swap by the boss's stacking debuff, take the boss back after a threat reset, pick up loose adds
    triggers.push_back(new TriggerNode("world boss tank swap", {NextAction("world boss taunt", ACTION_RAID + 5)}));
    triggers.push_back(
        new TriggerNode("world boss stacked tank", {NextAction("world boss step off", ACTION_RAID + 6)}));
    triggers.push_back(new TriggerNode("world boss threat reset", {NextAction("world boss taunt", ACTION_RAID + 4)}));
    triggers.push_back(new TriggerNode("world boss loose add", {NextAction("world boss taunt add", ACTION_RAID + 2)}));

    // healers: the raid cooldown when a raid-damage phase starts and the raid is hurt
    triggers.push_back(
        new TriggerNode("world boss raid cooldown", {NextAction("world boss raid cooldown", ACTION_RAID + 3)}));

    // damage dealers: the healer adds (Heal on the boss) and Lethon's Spirit Shades first; kick the Heal
    triggers.push_back(
        new TriggerNode("world boss priority add", {NextAction("world boss attack priority add", ACTION_RAID + 1)}));
    triggers.push_back(
        new TriggerNode("world boss add heal cast", {NextAction("world boss interrupt add", ACTION_INTERRUPT + 5)}));

    // Chill, Twisted Reflection, Mark of Kazzak, Volatile Infection
    triggers.push_back(new TriggerNode("world boss dispel", {NextAction("world boss dispel", ACTION_DISPEL + 5)}));

    // positions: the dragons' and Azuregos' sides; Kruul's front cone for melee; chain spells: ranged spread
    triggers.push_back(new TriggerNode("world boss position", {NextAction("world boss position", ACTION_MOVE + 5)}));
    triggers.push_back(new TriggerNode("world boss spread", {NextAction("world boss spread out", ACTION_MOVE + 6)}));
}

void WorldBossStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new WorldBossStackedTankMultiplier(botAI));
    multipliers.push_back(new WorldBossCommittedAddMultiplier(botAI));
}

void WorldBossStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    WorldBoss::AppendTargetExclusions(botAI, exclusions, type);
}
