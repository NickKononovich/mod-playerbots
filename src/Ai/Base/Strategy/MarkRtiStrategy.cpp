/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MarkRtiStrategy.h"
#include "Playerbots.h"

void MarkRtiStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("no rti target", { NextAction("mark rti", ACTION_NORMAL) }));
}

void TankMarksStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Above the rotation, below interrupts: one icon change a second at most (the trigger's interval).
    triggers.push_back(
        new TriggerNode("need tank kill marks", { NextAction("tank kill marks", ACTION_HIGH + 9) }));
}

void AutoCcMarksStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Above the class CC casts (30-42) so the icon is up before the CC spell picks its target.
    triggers.push_back(
        new TriggerNode("need auto cc mark", { NextAction("auto cc mark", ACTION_INTERRUPT + 5) }));
    triggers.push_back(
        new TriggerNode("stale auto cc mark", { NextAction("clear auto cc mark", ACTION_INTERRUPT + 4) }));
    // Above movement, so a bot that stands casts before it repositions; the class CC triggers (30-42) are spares.
    // A healer (crowd control only in a group without other) keeps its critical heals (30+) above it.
    float const castRelevance = PlayerbotAI::IsHeal(botAI->GetBot()) ? ACTION_HIGH + 5 : ACTION_MOVE + 5;
    triggers.push_back(
        new TriggerNode("auto cc target free", { NextAction("cast auto cc", castRelevance) }));
    // Next to the mage's Frost Nova for the healer (ACTION_INTERRUPT + 2).
    triggers.push_back(
        new TriggerNode("emergency cc", { NextAction("emergency cc", ACTION_INTERRUPT + 3) }));
}

void PrePullCcStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Above following and the ranged spot (movement band), so the rogue walks to its Sap instead of back to the tank.
    triggers.push_back(
        new TriggerNode("stale pre pull cc mark", { NextAction("clear auto cc mark", ACTION_MOVE + 7) }));
    triggers.push_back(
        new TriggerNode("pre pull cc", { NextAction("pre pull cc", ACTION_MOVE + 6) }));
}
