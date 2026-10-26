/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AKStrategy.h"
#include "AKMultipliers.h"
#include "AKTriggers.h"
#include "DungeonHazard.h"
#include "DungeonTactics.h"

void WotlkDungeonOKStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Elder Nadox
    triggers.push_back(new TriggerNode("nadox guardian",
        { NextAction("attack nadox guardian", ACTION_RAID + 5) }));

    // Prince Taldaram
    // He stays shielded in the air until both Ancient Nerubian Devices are used (out of combat, interactor only).
    triggers.push_back(new TriggerNode("taldaram nerubian device",
        { NextAction("use nerubian device", ACTION_MOVE + 2) }));
    // Flame Spheres spawn on him and pulse Sear Beam (18 yd, every 200 ms) while drifting 25 yd out: everybody,
    // the tank with him included, leaves their circles (DungeonHazard.cpp) until they despawn.
    triggers.push_back(new TriggerNode("dungeon hazard",
        { NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));

    // Trash before Elder Nadox (DungeonTactics.cpp): the Spell Flinger's Shadow Blast (6 s, 80 % of the target's
    // health on heroic) and Shadow Sickle channel (6 s, a 1.9-2.5k hit on a random member every 0.8 s) are always
    // kicked; kill order Spell Flinger, then Web Winder, then Deep Crawler. The kick runs above the kill-order switch:
    // at ACTION_INTERRUPT + 5 it was never evaluated while "dungeon kill first" (62) and "dps assist" (50) traded the
    // target every tick (batch 6, 20261025-000027: no interrupt cast in a 65 s pull with two Flingers).
    triggers.push_back(new TriggerNode("dungeon priority interrupt",
        { NextAction("dungeon priority interrupt", ACTION_RAID + 3) }));
    triggers.push_back(new TriggerNode("dungeon kill first",
        { NextAction("dungeon kill first", ACTION_RAID + 2) }));

    // Jedoga Shadowseeker
    // She descends only once her 15 kneeling initiates are dead; the tank pulls them one at a time, the nearest in
    // sight, when the group is there (AKJedogaRoom owns the pull: JedogaRoomPullMultiplier mutes the planner).
    triggers.push_back(new TriggerNode("jedoga initiates",
        { NextAction("attack jedoga initiate", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("jedoga volunteer",
        { NextAction("attack jedoga volunteer", ACTION_RAID + 5) }));

    // Herald Volazj
    // Trash mobs before him have a big telegraphed shadow crash spell,
    // this can be avoided and is intended to be dodged
    triggers.push_back(new TriggerNode("shadow crash",
        { NextAction("avoid shadow crash", ACTION_MOVE + 5) }));
    // Insanity (boss_herald_volazj.cpp): each bot fights clones of the others alone, in its own phase.
    triggers.push_back(new TriggerNode("volazj insanity",
        { NextAction("attack twisted visage", ACTION_RAID + 4) }));

    // Amanitar (Heroic Only)
    // [b33_ok] Mini (-75 % damage) is removed by Potent Fungus, cast by a dying Healthy Mushroom on players within
    // 4 yd: a bot with Mini walks to the nearest Healthy Mushroom clear of Poisonous ones and kills it there. Above
    // "dungeon kill first" (the Savage Cave Beast add) and below "avoid dungeon hazard" (Poisonous Mushrooms).
    // AoE stays off while the mushrooms stand and no target value picks a mushroom (AmanitarMushroomMultiplier,
    // AppendTargetExclusions).
    triggers.push_back(new TriggerNode("amanitar mini",
        { NextAction("amanitar potent fungus", ACTION_RAID + 4) }));
}

void WotlkDungeonOKStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new ElderNadoxMultiplier(botAI));
    multipliers.push_back(new JedogaShadowseekerMultiplier(botAI));
    multipliers.push_back(new JedogaRoomPullMultiplier(botAI));
    multipliers.push_back(new ForgottenOneMultiplier(botAI));
    multipliers.push_back(new VolazjInsanityMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
    multipliers.push_back(new AmanitarMushroomMultiplier(botAI));  // [b33_ok]
}

// The damage dealers' "dps target" is the kill-first mob while there is one, so "dps assist" keeps the target that
// "dungeon kill first" chose instead of switching it back every tick.
void WotlkDungeonOKStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::DpsTarget)
        DungeonTactics::AppendKillFirstExclusions(botAI, exclusions);

    // [b33_ok] Amanitar's mushrooms are never a target, a debuff target, a pet target or an AoE count: the
    // Healthy ones are killed by "amanitar potent fungus" (its own Attack), the Poisonous ones never.
    AKAmanitar::Snapshot const& amanitar = AKAmanitar::Get(botAI);
    if (!amanitar.active)
        return;

    exclusions.insert(amanitar.healthy.begin(), amanitar.healthy.end());
    exclusions.insert(amanitar.poisonous.begin(), amanitar.poisonous.end());
}
