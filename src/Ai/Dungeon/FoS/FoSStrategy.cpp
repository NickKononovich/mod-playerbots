/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FoSStrategy.h"
#include "DungeonHazard.h"
#include "DungeonTactics.h"
#include "FoSMultipliers.h"
#include "FoSTriggers.h"

void WotlkDungeonFoSStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("move from bronjahm",
        { NextAction("move from bronjahm", ACTION_RAID + 3) }));
    triggers.push_back(new TriggerNode("switch to soul fragment",
        { NextAction("attack corrupted soul fragment", ACTION_RAID + 2) }));
    triggers.push_back(new TriggerNode("bronjahm position",
        { NextAction("bronjahm group position", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("devourer of souls",
        { NextAction("devourer of souls", ACTION_RAID + 4) }));
    triggers.push_back(new TriggerNode("fos wailing souls",
        { NextAction("fos avoid wailing souls", ACTION_EMERGENCY + 5) }));
    triggers.push_back(new TriggerNode("fos interrupt",
        { NextAction("fos interrupt", ACTION_INTERRUPT + 5) }));
    // Bronjahm's Fear in Soulstorm (no kick) sends its victim out of the storm's eye: Tremor Totem, Fear Ward,
    // Berserker Rage; the racial / class fear breaks of the generic strategies stay as they are.
    triggers.push_back(new TriggerNode("dungeon fear guard",
        { NextAction("dungeon fear guard", ACTION_RAID + 6) }));
    // [boss_puddles] Devourer's Wells of Souls 36536 (4 yd, stay till the end)
    // (DungeonHazard.cpp, BOSS_PUDDLES.md): out of it the short way, and the hold keeps everyone from walking
    // back in while the tank drags the boss out (BossKiteRooms).
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
}

void WotlkDungeonFoSStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new BronjahmMultiplier(botAI));
    multipliers.push_back(new DungeonFearGuardMultiplier(botAI));
    multipliers.push_back(new AttackFragmentMultiplier(botAI));
    multipliers.push_back(new DevourerOfSoulsMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
}

void WotlkDungeonFoSStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType /*type*/)
{
    if (Unit* boss = FoSShared::GetMirroringDevourer(botAI))
        exclusions.insert(boss->GetGUID());
}
