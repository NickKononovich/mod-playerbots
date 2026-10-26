/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericWarriorNonCombatStrategy.h"
#include "Playerbots.h"
#include "RezzedTankHoldMultiplier.h"

class GenericWarriorNonCombatStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    GenericWarriorNonCombatStrategyActionNodeFactory() { creators["berserker rage"] = &berserker_rage; }

private:
    static ActionNode* berserker_rage([[maybe_unused]] PlayerbotAI* botAI)
    {
        // [b31_deep] no "berserker stance" prerequisite: 3.3.5 Berserker Rage has stance mask 0
        return new ActionNode(
            "berserker rage",
            /*P*/ {},
            /*A*/ {},
            /*C*/ {}
        );
    }
};

GenericWarriorNonCombatStrategy::GenericWarriorNonCombatStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericWarriorNonCombatStrategyActionNodeFactory());
}

void GenericWarriorNonCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    NonCombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("often", { NextAction("apply stone", 1.0f) }));
    triggers.push_back(new TriggerNode("vigilance", { NextAction("vigilance", 10.0f) }));
    triggers.push_back(new TriggerNode(
        "fear sleep sap", { NextAction("berserker rage", ACTION_EMERGENCY + 1) }));
}

void GenericWarriorNonCombatStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    NonCombatStrategy::InitMultipliers(multipliers);
    // the non-combat engine runs right after a resurrection, also while the group still fights
    if (PlayerbotAI::IsTank(botAI->GetBot(), true))
        multipliers.push_back(new RezzedTankHoldMultiplier(botAI));
}
