/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericMageNonCombatStrategy.h"
#include "AiFactory.h"
#include "Playerbots.h"

class GenericMageNonCombatStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    GenericMageNonCombatStrategyActionNodeFactory()
    {
        creators["ice armor"] = &ice_armor;
    }

private:
    static ActionNode* ice_armor([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("ice armor",
                              /*P*/ {},
                              /*A*/ { NextAction("frost armor") },
                              /*C*/ {});
    }
};

GenericMageNonCombatStrategy::GenericMageNonCombatStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericMageNonCombatStrategyActionNodeFactory());
}

void GenericMageNonCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    NonCombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("arcane intellect", { NextAction("arcane intellect", 21.0f) }));
    triggers.push_back(new TriggerNode("no focus magic", { NextAction("focus magic on party", 19.0f) }));
    triggers.push_back(new TriggerNode("often", { NextAction("apply oil", 1.0f) }));
    triggers.push_back(new TriggerNode("no mana gem", { NextAction("conjure mana gem", 20.0f) }));

    // Own conjured food and water (the bots drink only category-59 water, so water stays Conjure Water);
    // Conjure Refreshment is preferred for food once learned, Conjure Food otherwise.
    triggers.push_back(new TriggerNode("no drink", { NextAction("conjure water", 19.8f) }));
    triggers.push_back(new TriggerNode("no food", { NextAction("conjure refreshment", 19.7f),
                                                    NextAction("conjure food", 19.6f) }));
    // Hand conjured food / water to group members who have none
    triggers.push_back(new TriggerNode("give water", { NextAction("give water", 19.5f) }));
    triggers.push_back(new TriggerNode("give food", { NextAction("give food", 19.4f) }));
    // Refreshment Table for the raid before a boss (ready check or boss near), when none is up
    triggers.push_back(new TriggerNode("ritual of refreshment", { NextAction("ritual of refreshment", 20.5f) }));
}

void MageBuffManaStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("molten armor", { NextAction("molten armor", 19.5f) }));
    triggers.push_back(new TriggerNode("mage armor", { NextAction("mage armor", 19.0f) }));
}

void MageBuffDpsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("molten armor", { NextAction("molten armor", 19.0f) }));
}

void MageBuffStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("arcane intellect on party", { NextAction("arcane intellect on party", 20.0f) }));
}
