/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MeleeCombatStrategy.h"
#include "Playerbots.h"

void MeleeCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    CombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode(
        "enemy out of melee", { NextAction("reach melee", ACTION_HIGH + 1) }));
}

void SetBehindCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    CombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("not behind target",
                                       { NextAction("set behind", ACTION_MOVE + 7) }));
    // A mob meleeing the dps away from the tank: the dps brings it to the tank, where the peel and the heals reach,
    // before stepping behind anything (Stockade 2026-10-24: a rogue died alone 23 yd from the tank).
    triggers.push_back(new TriggerNode("chased away from tank", { NextAction("move to tank", ACTION_MOVE + 8) }));
}
