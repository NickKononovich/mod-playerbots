/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericRogueNonCombatStrategy.h"
#include "Playerbots.h"

GenericRogueNonCombatStrategy::GenericRogueNonCombatStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI)
{
}

void GenericRogueNonCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    NonCombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("player has flag",
                                       { NextAction("sprint", ACTION_EMERGENCY + 1) }));
    triggers.push_back(new TriggerNode("enemy flagcarrier near",
                                       { NextAction("sprint", ACTION_EMERGENCY + 2) }));
    // [b31_wr] also re-applied when under 10 min are left, so it does not run out mid-pull
    triggers.push_back(
        new TriggerNode("main hand poison expiring",
                        { NextAction("use poison on main hand", ACTION_HIGH) }));

    triggers.push_back(
        new TriggerNode("off hand poison expiring",
                        { NextAction("use poison on off hand", ACTION_HIGH - 1) }));

    // [b31_wr] was 30, above the poisons, and "unstealth" ran (true) without stealth: 16-25 % of the ticks
    triggers.push_back(new TriggerNode("often", { NextAction("unstealth", ACTION_NORMAL) }));
    // K6 pre-pull: Tricks of the Trade on the tank that is about to pull
    triggers.push_back(
        new TriggerNode("pull imminent", { NextAction("tricks of the trade on main tank", ACTION_HIGH + 2) }));
}
