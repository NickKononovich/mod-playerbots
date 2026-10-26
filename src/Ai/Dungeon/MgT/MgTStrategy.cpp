/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MgTStrategy.h"
#include "DungeonTactics.h"
#include "MgTMultipliers.h"
#include "MgTTriggers.h"
#include "Playerbots.h"

void TbcDungeonMagistersTerraceStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("mgt out of room", {
        NextAction("mgt return to room", ACTION_EMERGENCY + 10) }));

    // Selin's Wretched are fought in the corridor while she is idle (her room trash joined her at 97 % in wave 9)
    triggers.push_back(new TriggerNode("dungeon boss room trash", {
        NextAction("dungeon boss room pull out", ACTION_EMERGENCY + 2) }));

    // Her room is cleared pack by pack before she is pulled (MagistersTerrace::SelinRoom, MgTShared.h): the group
    // waits at the corridor rally, the tank pulls one pack with its ranged pull from a spot out of the other packs'
    // reach, the pack is fought at the rally, and nobody stands in an idle pack's reach (waves 24/26: a member at the
    // door pulled the pair next to her, she came 2 s later with all 14 Wretched).
    triggers.push_back(new TriggerNode("mgt selin room pull", {
        NextAction("mgt selin room pull", 4.5f) }));

    // Selin inert with Wretched of hers alive: the room is cleared first, the pull outranks the combat defaults.
    triggers.push_back(new TriggerNode("mgt selin room pull inert", {
        NextAction("mgt selin room pull", ACTION_MOVE + 3) }));

    // [selin pet] no pet in her room or on her while the room is cleared (a hunter's Crab, mgt-h build 25).
    triggers.push_back(new TriggerNode("mgt selin pet out", {
        NextAction("mgt selin pet out", ACTION_EMERGENCY + 3) }));

    triggers.push_back(new TriggerNode("mgt selin room gather", {
        NextAction("mgt selin room to rally", ACTION_MOVE + 4) }));

    triggers.push_back(new TriggerNode("mgt selin room drift", {
        NextAction("mgt selin room to rally", ACTION_MOVE + 9) }));

    triggers.push_back(new TriggerNode("mgt selin room rally", {
        NextAction("mgt selin room to rally", ACTION_MOVE + 6) }));

    // The room entry order (MgTShared.h, SelinRoom::LEASH_EDGE_X): nobody past the door while a Wretched of hers
    // lives, and into the doorway when she fights somebody inside with a member shut out (mgt-h 2026-10-25: the
    // room door closed on the tank alone in 17 of 17 runs).
    triggers.push_back(new TriggerNode("mgt selin room leash", {
        NextAction("mgt selin room leash", ACTION_EMERGENCY + 3) }));

    // Fel Infusion (MgTShared.h): the tank's cooldown under a drinking Wretched, the shaman's haste on a whole pack.
    triggers.push_back(new TriggerNode("mgt wretched infusion", {
        NextAction("mgt wretched infusion", ACTION_EMERGENCY + 1) }));

    triggers.push_back(new TriggerNode("mgt crystal active", {
        NextAction("mgt kill crystal", ACTION_EMERGENCY + 8) }));

    triggers.push_back(new TriggerNode("mgt in dampening field", {
        NextAction("mgt leave dampening field", ACTION_EMERGENCY + 6) }));

    triggers.push_back(new TriggerNode("mgt priority interrupt", {
        NextAction("mgt priority interrupt", ACTION_INTERRUPT + 5) }));

    triggers.push_back(new TriggerNode("mgt arcane nova range", {
        NextAction("mgt clear arcane nova", ACTION_EMERGENCY + 4) }));

    triggers.push_back(new TriggerNode("mgt mage guard at range", {
        NextAction("mgt close on mage guard", ACTION_MOVE + 5) }));

    triggers.push_back(new TriggerNode("mgt enraged wretched", {
        NextAction("mgt taunt enraged wretched", ACTION_HIGH + 2) }));

    triggers.push_back(new TriggerNode("mgt focus target", {
        NextAction("mgt focus target", ACTION_RAID + 2) }));

    triggers.push_back(new TriggerNode("mgt delrissa interrupt", {
        NextAction("mgt delrissa interrupt", ACTION_INTERRUPT + 5) }));

    triggers.push_back(new TriggerNode("mgt delrissa focus target", {
        NextAction("mgt delrissa focus target", ACTION_RAID + 2) }));

    triggers.push_back(new TriggerNode("mgt delrissa healer spot", {
        NextAction("mgt delrissa healer spot", ACTION_MOVE + 7) }));

    triggers.push_back(new TriggerNode("mgt delrissa tremor totem", {
        NextAction("mgt delrissa tremor totem", ACTION_HIGH + 5) }));

    // Two of the retinue in crowd control (the generic auto cc marks leave boss fights alone)
    // [delrissa_retinue_left] a lackey outlived Delrissa: the encounter (and Kael's door) waits for it
    triggers.push_back(new TriggerNode("mgt delrissa retinue left", {
        NextAction("mgt delrissa retinue left", ACTION_MOVE + 2) }));

    triggers.push_back(new TriggerNode("mgt delrissa cc mark", {
        NextAction("mgt delrissa cc mark", ACTION_INTERRUPT + 6) }));

    triggers.push_back(new TriggerNode("mgt flame strike", {
        NextAction("mgt leave flame strike", ACTION_EMERGENCY + 7) }));

    triggers.push_back(new TriggerNode("mgt phoenix burn", {
        NextAction("mgt leave phoenix burn", ACTION_EMERGENCY + 6) }));

    triggers.push_back(new TriggerNode("mgt kael focus target", {
        NextAction("mgt kael focus target", ACTION_RAID + 2) }));

    triggers.push_back(new TriggerNode("mgt kael interrupt", {
        NextAction("mgt kael interrupt", ACTION_INTERRUPT + 6) }));

    triggers.push_back(new TriggerNode("mgt gravity lapse", {
        NextAction("mgt take lapse spot", ACTION_MOVE + 8) }));
}

void TbcDungeonMagistersTerraceStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new DungeonBossRoomHoldMultiplier(botAI));
    multipliers.push_back(new MgTSelinRoomMultiplier(botAI));
    multipliers.push_back(new MgTCrystalFocusMultiplier(botAI));
    multipliers.push_back(new MgTFocusOrderMultiplier(botAI));
    multipliers.push_back(new MgTFocusBurstMultiplier(botAI));
    multipliers.push_back(new MgTRoomLeashMultiplier(botAI));
    multipliers.push_back(new MgTDampeningFieldMultiplier(botAI));
    multipliers.push_back(new MgTDelrissaTremorTotemMultiplier(botAI));
    multipliers.push_back(new MgTDelrissaHealerHoldMultiplier(botAI));
    multipliers.push_back(new MgTFlameStrikeMultiplier(botAI));
    multipliers.push_back(new MgTPhoenixBurnMultiplier(botAI));
    multipliers.push_back(new MgTKaelUnattackableMultiplier(botAI));
    multipliers.push_back(new MgTGravityLapseMultiplier(botAI));
}

void TbcDungeonMagistersTerraceStrategy::AppendTargetExclusions(GuidSet& exclusions,
                                                               TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::Aoe)
        return;

    AiObjectContext* context = botAI->GetAiObjectContext();

    // [selin pet] no pet fights Selin or a Wretched at her door while her room is cleared (MgTShared.h)
    if (type == TargetValueExclusionType::PetTarget)
        MagistersTerrace::SelinRoom::AppendPetExclusions(botAI, exclusions);

    for (ObjectGuid const guid : context->GetValue<GuidVector>("mgt delrissa pets")->Get())
        exclusions.insert(guid);

    for (ObjectGuid const guid : context->GetValue<GuidVector>("mgt melee phoenix exclusions")->Get())
        exclusions.insert(guid);

    if (type != TargetValueExclusionType::DebuffTarget)
        return;

    for (ObjectGuid const guid : context->GetValue<GuidVector>("mgt focus exclusions")->Get())
        exclusions.insert(guid);
}
