/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SVStrategy.h"
#include "DungeonHazard.h"
#include "DungeonTactics.h"
#include "SVMultipliers.h"
#include "SVShared.h"
#include "TbcHazard.h"

void TbcDungeonSteamvaultStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Warlord Kalithresh
    triggers.push_back(new TriggerNode("kalithresh spell reflection", {
        NextAction("kalithresh stop casting", ACTION_EMERGENCY + 2) }));

    triggers.push_back(new TriggerNode("kalithresh distiller", {
        NextAction("kalithresh kill distiller", ACTION_RAID + 2) }));

    // Coilfang naga packs (DungeonTactics.cpp, DungeonHazard.cpp): the Oracle's Heal and the Siren's Fear are kicked
    // whenever someone can, the Oracle dies first, then the Siren; nobody stands in the Sorceress' Blizzard.
    triggers.push_back(new TriggerNode("dungeon priority interrupt", {
        NextAction("dungeon priority interrupt", ACTION_INTERRUPT + 5) }));
    triggers.push_back(new TriggerNode("dungeon kill first", {
        NextAction("dungeon kill first", ACTION_RAID + 2) }));
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));

    // Bog Overlord (SVShared.h): the heroic one kills a non-tank in seconds; the tank takes it back at once. Its
    // Disease Cloud is a ranged / healer hazard (DungeonHazard.cpp), its Enrage the hunter's Tranquilizing Shot.
    triggers.push_back(new TriggerNode("sv bog overlord loose", {
        NextAction("sv taunt bog overlord", ACTION_EMERGENCY + 1) }));
}

void TbcDungeonSteamvaultStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new KalithreshSpellReflectionMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
    multipliers.push_back(new TbcKeepFocusMultiplier(botAI, "kalithresh keep distiller focus", {
        Steamvault::NPC_NAGA_DISTILLER }));
}

// The damage dealers' "dps target" is the kill-first mob while there is one, so "dps assist" keeps the target that
// "dungeon kill first" chose instead of switching it back every tick.
void TbcDungeonSteamvaultStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::DpsTarget)
        DungeonTactics::AppendKillFirstExclusions(botAI, exclusions);
}
