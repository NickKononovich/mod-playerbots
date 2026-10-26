/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HoLStrategy.h"
#include "DungeonHazard.h"
#include "DungeonTactics.h"
#include "HoLMultipliers.h"

namespace
{
constexpr uint32 NPC_BLISTERING_STEAMRAGER = 28583;
}

void WotlkDungeonHoLStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // General Bjarngrim
    triggers.push_back(new TriggerNode("stormforged lieutenant",
        { NextAction("bjarngrim target", ACTION_RAID + 5) }));
    triggers.push_back(new TriggerNode("whirlwind",
        { NextAction("avoid whirlwind", ACTION_RAID + 4) }));

    // Volkhan
    triggers.push_back(new TriggerNode("volkhan",
        { NextAction("volkhan target", ACTION_RAID + 5) }));
    triggers.push_back(new TriggerNode("volkhan brittle golem",
        { NextAction("avoid brittle golem", ACTION_RAID + 6) }));

    // Ionar
    triggers.push_back(new TriggerNode("ionar tank aggro",
        { NextAction("ionar tank position", ACTION_MOVE + 4) }));
    triggers.push_back(new TriggerNode("static overload",
        { NextAction("static overload spread", ACTION_MOVE + 3) }));
    // TODO: Targeted player can dodge the ball, but a single player soaking it isn't too bad to heal
    triggers.push_back(new TriggerNode("ball lightning",
        { NextAction("ball lightning spread", ACTION_MOVE + 2) }));

    // Ionar's Sparks and the spot he reforms at, Slag's death Blast Wave (DungeonHazard.cpp); Blistering Steamragers
    // on a member the tank does not hold die first, nobody stands in their Steam Blast cone (DungeonTactics.cpp).
    triggers.push_back(new TriggerNode("dungeon hazard",
        { NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
    triggers.push_back(new TriggerNode("dungeon kill first",
        { NextAction("dungeon kill first", ACTION_RAID + 2) }));
    triggers.push_back(new TriggerNode("dungeon frontal cleave",
        { NextAction("dungeon avoid frontal cleave", ACTION_EMERGENCY + 3) }));

    // Loken
    triggers.push_back(new TriggerNode("lightning nova",
        { NextAction("avoid lightning nova", ACTION_MOVE + 5) }));
    triggers.push_back(new TriggerNode("loken ranged",
        { NextAction("loken stack", ACTION_RAID + 1) }));
}

void WotlkDungeonHoLStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new BjarngrimMultiplier(botAI));
    multipliers.push_back(new VolkhanMultiplier(botAI));
    multipliers.push_back(new IonarMultiplier(botAI));
    multipliers.push_back(new LokenMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
    multipliers.push_back(new DungeonFrontalCleaveMultiplier(botAI));
    multipliers.push_back(new TbcKeepFocusMultiplier(botAI, "hol steamrager focus", {NPC_BLISTERING_STEAMRAGER}));
}

// The damage dealers' "dps target" is the kill-first mob while there is one, so "dps assist" keeps the target that
// "dungeon kill first" chose instead of switching it back every tick.
void WotlkDungeonHoLStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::DpsTarget)
        DungeonTactics::AppendKillFirstExclusions(botAI, exclusions);
}
