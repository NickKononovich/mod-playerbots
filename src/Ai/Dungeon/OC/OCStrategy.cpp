/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "OCStrategy.h"
#include "DungeonHazard.h"
#include "DungeonTactics.h"
#include "OCMultipliers.h"

void WotlkDungeonOccStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Entrance: everybody clicks the Nexus Portal to Drakos' island himself
    triggers.push_back(new TriggerNode("oc nexus portal",
        { NextAction("oc use nexus portal", ACTION_MOVE + 9) }));

    // Drakos, Varos and Urom never aggro on sight
    triggers.push_back(new TriggerNode("oc pull boss",
        { NextAction("oc pull boss", ACTION_RAID + 1) }));

    // Drakos the Interrogator: everybody keeps out of the standing Unstable Spheres until they detonate
    // (DungeonHazard.cpp; wave 11: Sphere Detonation killed the healer).
    // Ring-Lord packs on Varos' rings (on foot): out of the Sorceress' Blizzard and Flamestrike patch, and out of the
    // Conjurer's Charged Skin unless a healthy melee (DungeonHazard.cpp) too.
    triggers.push_back(new TriggerNode("dungeon hazard",
        { NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
    // Ring-Lord Sorceress: Flamestrike and Blizzard are kicked whenever someone can (DungeonTactics.cpp).
    triggers.push_back(new TriggerNode("dungeon priority interrupt",
        { NextAction("dungeon priority interrupt", ACTION_INTERRUPT + 5) }));
    // Kill order of a Ring-Lord pack on foot: Sorceress, then Conjurer, then the Centrifuge Construct.
    triggers.push_back(new TriggerNode("dungeon kill first",
        { NextAction("dungeon kill first", ACTION_RAID + 2) }));

    // DRAKES
    triggers.push_back(new TriggerNode("drake mount",
        { NextAction("mount drake", ACTION_RAID + 5) }));
    triggers.push_back(new TriggerNode("drake dismount",
        { NextAction("dismount drake", ACTION_RAID + 5) }));
    triggers.push_back(new TriggerNode("group flying",
        { NextAction("occ fly drake", ACTION_NORMAL + 1) }));
    triggers.push_back(new TriggerNode("drake combat",
        { NextAction("occ drake attack", ACTION_NORMAL + 5) }));
    // Landed first: back to the own landing spot until the whole group is down (OccLandingHoldMultiplier)
    triggers.push_back(new TriggerNode("oc landing hold",
        { NextAction("oc hold landing spot", ACTION_MOVE + 3) }));

    // Varos Cloudstrider: the four Energize Cores directions are fixed (boss_varos.cpp ZapAngle)
    triggers.push_back(new TriggerNode("varos cloudstrider",
        { NextAction("avoid energize cores", ACTION_RAID + 4) }));

    // Mage-Lord Urom
    triggers.push_back(new TriggerNode("arcane explosion",
        { NextAction("avoid arcane explosion", ACTION_MOVE + 5) }));
    triggers.push_back(new TriggerNode("time bomb",
        { NextAction("time bomb spread", ACTION_MOVE + 4) }));

    // Ley-Guardian Eregos
    // [b36_occ] Planar Anomalies (heroic, Planar Shift at 60 % / 20 %): every drake flies clear of them before their
    // Planar Blast (20 yd, 15.5 s after spawning); above "occ drake attack" and "occ fly drake", which hold the drake still.
    triggers.push_back(new TriggerNode("oc planar anomaly",
        { NextAction("avoid planar anomaly", ACTION_EMERGENCY + 5) }));
}

void WotlkDungeonOccStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new MountingDrakeMultiplier(botAI));
    multipliers.push_back(new OccPortalMountMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
    multipliers.push_back(new OccFlyingMultiplier(botAI));
    multipliers.push_back(new OccRingGatherMultiplier(botAI));
    multipliers.push_back(new OccHealerGatherMultiplier(botAI));
    multipliers.push_back(new OccLandingHoldMultiplier(botAI));
    multipliers.push_back(new UromMultiplier(botAI));
    multipliers.push_back(new EregosMultiplier(botAI));
}

// The damage dealers' "dps target" is the kill-first mob while there is one, so "dps assist" keeps the target that
// "dungeon kill first" chose instead of switching it back every tick.
void WotlkDungeonOccStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::DpsTarget)
        DungeonTactics::AppendKillFirstExclusions(botAI, exclusions);
}
