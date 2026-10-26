/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericWarriorStrategy.h"
#include "Playerbots.h"

class GenericWarriorStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    GenericWarriorStrategyActionNodeFactory() { creators["berserker rage"] = &berserker_rage; }

private:
    static ActionNode* berserker_rage([[maybe_unused]] PlayerbotAI* botAI)
    {
        // [b31_deep] no "berserker stance" prerequisite: 3.3.5 Berserker Rage (18499) has stance mask 0, and the
        // stance swap cost a tank Defensive Stance (and its rage) under a fear
        return new ActionNode(
            "berserker rage",
            /*P*/ {},
            /*A*/ {},
            /*C*/ {}
        );
    }
};

GenericWarriorStrategy::GenericWarriorStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericWarriorStrategyActionNodeFactory());
}

void GenericWarriorStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    CombatStrategy::InitTriggers(triggers);
    triggers.push_back(new TriggerNode(
        "enemy out of melee", { NextAction("reach melee", ACTION_HIGH + 1) }));
    triggers.push_back(new TriggerNode(
        "fear sleep sap", { NextAction("berserker rage", ACTION_EMERGENCY + 1) }));
}

class WarriorAoeStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    WarriorAoeStrategyActionNodeFactory()
    {

    }

private:

};

WarriorAoeStrategy::WarriorAoeStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI)
{
    actionNodeFactories.Add(new WarriorAoeStrategyActionNodeFactory());
}

void WarriorAoeStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // [b31_wr] Stance-aware: each ability only on a trigger its stance passes (Whirlwind berserker, Thunder Clap
    // battle / defensive, Sweeping Strikes battle / berserker), so Arms no longer tries Whirlwind nor Fury Thunder
    // Clap; no stance dance for AoE. Bladestorm needs 3+ mobs here (the Arms "bladestorm" boost trigger covers the
    // rest), Cleave is the rage dump. Shockwave, Demoralizing Shout: any stance.
    triggers.push_back(new TriggerNode(
        "light aoe berserker stance", { NextAction("whirlwind", ACTION_HIGH + 8) }));
    triggers.push_back(new TriggerNode(
        "light aoe sweeping strikes stance", { NextAction("sweeping strikes", ACTION_HIGH + 7) }));
    // [b31_deep] Bladestorm / Shockwave only once known (Fury: "spell not known" 229 / 244 times on L80 build 30)
    triggers.push_back(new TriggerNode(
        "medium aoe bladestorm", { NextAction("bladestorm", ACTION_HIGH + 6) }));
    triggers.push_back(new TriggerNode(
        "light aoe thunder clap stance", { NextAction("thunder clap", ACTION_HIGH + 5) }));
    triggers.push_back(new TriggerNode(
        "light aoe shockwave", { NextAction("shockwave", ACTION_HIGH + 4) }));
    triggers.push_back(new TriggerNode(
        "light aoe", { NextAction("demoralizing shout without life time check", ACTION_HIGH + 1) }));
    triggers.push_back(new TriggerNode(
        "light aoe and medium rage", { NextAction("cleave", ACTION_HIGH + 0.5f) }));
    triggers.push_back(
        new TriggerNode("shockwave on snare target",
                        { NextAction("shockwave on snare target", ACTION_HIGH + 5) }));

}
