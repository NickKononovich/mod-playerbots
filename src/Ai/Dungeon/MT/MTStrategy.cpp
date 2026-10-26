/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MTStrategy.h"
#include "MTMultipliers.h"
#include "DungeonHazard.h"
#include "MTShared.h"
#include "TbcHazard.h"

void TbcDungeonManaTombsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Pandemonius
    triggers.push_back(new TriggerNode("pandemonius dark shell", {
        NextAction("pandemonius stop attack", ACTION_EMERGENCY + 2) }));

    // Nexus-Prince Shaffar
    triggers.push_back(new TriggerNode("shaffar beacon up", {
        NextAction("shaffar kill beacon", ACTION_RAID + 2) }));

    // Trash: a Mana Leech low on health explodes on death (Arcane Explosion 34933, 10 yd)
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
}

void TbcDungeonManaTombsStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new PandemoniusDarkShellMultiplier(botAI));
    multipliers.push_back(new TbcKeepFocusMultiplier(botAI, "shaffar keep beacon focus", {
        ManaTombs::NPC_ETHEREAL_BEACON, ManaTombs::NPC_ETHEREAL_APPRENTICE }));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
}
