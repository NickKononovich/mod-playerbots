/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SLStrategy.h"
#include "SLMultipliers.h"

void TbcDungeonShadowLabyrinthStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Ambassador Hellmaw (reuses the Sethekk Halls tremor totem action, registered in SethActionContext.h)
    triggers.push_back(new TriggerNode("hellmaw fear", {
        NextAction("sethekk prophet set tremor totem", ACTION_RAID) }));

    // Murmur
    triggers.push_back(new TriggerNode("murmur sonic boom", {
        NextAction("murmur leave sonic boom", ACTION_EMERGENCY + 7) }));

    triggers.push_back(new TriggerNode("murmurs touch", {
        NextAction("murmurs touch spread", ACTION_EMERGENCY + 3) }));

    triggers.push_back(new TriggerNode("murmur thundering storm range", {
        NextAction("murmur avoid thundering storm", ACTION_MOVE + 7) }));

    // Blackheart the Inciter: pets passive for the whole fight, ranged apart, everybody topped up after Incite Chaos
    triggers.push_back(new TriggerNode("blackheart pets", {
        NextAction("blackheart pets", ACTION_RAID + 3) }));
    triggers.push_back(new TriggerNode("blackheart spread", {
        NextAction("blackheart spread", ACTION_MOVE + 4) }));
    triggers.push_back(new TriggerNode("dungeon top up", {
        NextAction("dungeon top up", ACTION_LIGHT_HEAL + 6) }));
}

void TbcDungeonShadowLabyrinthStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new MurmurSonicBoomHoldMultiplier(botAI));
    multipliers.push_back(new HellmawTremorTotemMultiplier(botAI));
}
