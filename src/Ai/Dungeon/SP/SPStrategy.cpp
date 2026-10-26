/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SPStrategy.h"
#include "SPMultipliers.h"
#include "SPShared.h"
#include "TbcHazard.h"

void TbcDungeonSlavePensStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Mennu the Betrayer
    triggers.push_back(new TriggerNode("mennu nova totem", {
        NextAction("mennu leave nova totem", ACTION_EMERGENCY + 6) }));

    triggers.push_back(new TriggerNode("mennu totems up", {
        NextAction("mennu kill totems", ACTION_RAID + 1) }));
}

void TbcDungeonSlavePensStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new MennuNovaTotemHoldMultiplier(botAI));
    multipliers.push_back(new TbcKeepFocusMultiplier(botAI, "mennu keep totem focus", {
        SlavePens::NPC_CORRUPTED_NOVA_TOTEM, SlavePens::NPC_TAINTED_EARTHGRAB_TOTEM,
        SlavePens::NPC_MENNU_HEALING_WARD }));
}
