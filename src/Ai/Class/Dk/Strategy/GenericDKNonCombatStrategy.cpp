/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericDKNonCombatStrategy.h"
#include "AiFactory.h"
#include "Playerbots.h"
#include "RezzedTankHoldMultiplier.h"

GenericDKNonCombatStrategy::GenericDKNonCombatStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

void GenericDKNonCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    NonCombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("often", { NextAction("apply stone", 1.0f) }));
    triggers.push_back(
        new TriggerNode("horn of winter", { NextAction("horn of winter", 21.0f) }));
    // [rot10] a member who came into range after the DK's own Horn
    triggers.push_back(new TriggerNode("horn of winter on party", {NextAction("horn of winter", 20.8f)}));
    // [rot_b31_pdd] a group tank: every Horn of Winter off cooldown, runic power for the next pull's defensives
    triggers.push_back(
        new TriggerNode("horn of winter ready", { NextAction("horn of winter", 20.5f) }));
    // [b31_dk] A presence before the pull, above the buffs: Frost for the Blood tank (the talent tab AiFactory gives
    // the "blood" tank strategy), Blood for Frost / Unholy DPS. Decided once at init, as AiFactory picks the combat
    // strategy; a respec re-inits the engines.
    uint8 const specTab = AiFactory::GetPlayerSpecTab(botAI->GetBot());
    bool const tank = specTab == DEATH_KNIGHT_TAB_BLOOD;
    // [b31_deep] Bone Shield is an Unholy talent: for Blood / Frost the trigger only logged "spell not known" (279)
    if (specTab == DEATH_KNIGHT_TAB_UNHOLY)
        triggers.push_back(new TriggerNode("bone shield", { NextAction("bone shield", 21.0f) }));
    triggers.push_back(new TriggerNode(
        "no presence", { NextAction(tank ? "frost presence" : "blood presence", 22.0f) }));
}

void GenericDKNonCombatStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    NonCombatStrategy::InitMultipliers(multipliers);
    // the non-combat engine runs right after a resurrection, also while the group still fights
    if (PlayerbotAI::IsTank(botAI->GetBot(), true))
        multipliers.push_back(new RezzedTankHoldMultiplier(botAI));
}
