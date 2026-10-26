/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SHStrategy.h"
#include "SHMultipliers.h"
#include "DungeonHazard.h"

void TbcDungeonShatteredHallsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Warchief Kargath Bladefist: everybody out of Blade Dance (15 yd of where he stands, again at each charge); the
    // healer and the ranged keep a ring round him between the dances. His guards from the portal and the assassins
    // are the tank's to pick up (tank assist); the damage dealers stay on what the dps target names.
    triggers.push_back(new TriggerNode("kargath blade dance", {
        NextAction("kargath leave blade dance", ACTION_EMERGENCY + 6) }));
    triggers.push_back(new TriggerNode("kargath ranged ring", {
        NextAction("kargath keep ring", ACTION_MOVE + 6) }));
    // [boss_puddles] Nethekurse's Lesser Shadow Fissures 17471 (3 yd, 26 s)
    // (DungeonHazard.cpp, BOSS_PUDDLES.md): out of it the short way, and the hold keeps everyone from walking
    // back in while the tank drags the boss out (BossKiteRooms).
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
}

void TbcDungeonShatteredHallsStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new KargathBladeDanceHoldMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
}
