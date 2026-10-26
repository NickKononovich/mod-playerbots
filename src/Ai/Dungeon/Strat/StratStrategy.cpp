/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "StratStrategy.h"

#include "DungeonTactics.h"
#include "Playerbots.h"
#include "StratMultipliers.h"
#include "TbcHazard.h"

void ClassicDungeonStratholmeStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Out of combat: a mob on a member while the tank stands idle is the tank's at once (Righteous Defense / taunt)
    triggers.push_back(new TriggerNode("strat member swarmed", {
        NextAction("strat defend on attacked group member", ACTION_EMERGENCY + 1) }));
    // Out of combat: nobody waits at the pack's edge in front of the tank (a wanderer takes the first one it meets)
    triggers.push_back(new TriggerNode("strat aggro edge", {
        NextAction("strat step behind tank", ACTION_MOVE + 6) }));
    // The Skeletal Guardian's Blizzard and Arcane Explosion: whoever can kick does
    triggers.push_back(new TriggerNode("dungeon priority interrupt", {
        NextAction("dungeon priority interrupt", ACTION_INTERRUPT + 5) }));
    // Plague Ghoul (elite, enrages at 30 %), then Skeletal Guardian (caster) first
    triggers.push_back(new TriggerNode("dungeon kill first", {
        NextAction("dungeon kill first", ACTION_RAID + 2) }));
    // Skeletal Berserker and Plague Ghoul cleave: behind them, out of the front 180 degrees
    triggers.push_back(new TriggerNode("dungeon frontal cleave", {
        NextAction("dungeon avoid frontal cleave", ACTION_EMERGENCY + 3) }));
}

void ClassicDungeonStratholmeStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new StratHoldBehindTankMultiplier(botAI));
    multipliers.push_back(new StratAoeOpenerHoldMultiplier(botAI));
    multipliers.push_back(new DungeonFrontalCleaveMultiplier(botAI));
    multipliers.push_back(new TbcKeepFocusMultiplier(botAI, "strat kill order focus", {NPC_PLAGUE_GHOUL,
                                                                                      NPC_SKELETAL_GUARDIAN}));
}

// The damage dealers' "dps target" is the kill-first mob while there is one, so "dps assist" keeps the target that
// "dungeon kill first" chose instead of switching it back every tick.
void ClassicDungeonStratholmeStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::DpsTarget)
        DungeonTactics::AppendKillFirstExclusions(botAI, exclusions);
}
