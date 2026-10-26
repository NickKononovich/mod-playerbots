/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FeralDruidStrategy.h"
#include "Playerbots.h"

bool DruidHasThickHide(Player* bot)  // [b31_sd]
{
    return bot->HasAura(16929) || bot->HasAura(16930) || bot->HasAura(AURA_THICK_HIDE);
}

class FeralDruidStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    FeralDruidStrategyActionNodeFactory()
    {
        creators["survival instincts"] = &survival_instincts;
        creators["thorns"] = &thorns;
        creators["cure poison"] = &cure_poison;
        creators["cure poison on party"] = &cure_poison_on_party;
        creators["abolish poison"] = &abolish_poison;
        creators["abolish poison on party"] = &abolish_poison_on_party;
    }

private:
    static ActionNode* survival_instincts([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("survival instincts",
                              /*P*/ {},
                              /*A*/ { NextAction("barkskin") },
                              /*C*/ {});
    }

    static ActionNode* thorns([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("thorns",
                              /*P*/ { NextAction("caster form") },
                              /*A*/ {},
                              /*C*/ {});
    }

    static ActionNode* cure_poison([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("cure poison",
                              /*P*/ { NextAction("caster form") },
                              /*A*/ {},
                              /*C*/ {});
    }

    static ActionNode* cure_poison_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("cure poison on party",
                              /*P*/ { NextAction("caster form") },
                              /*A*/ {},
                              /*C*/ {});
    }

    static ActionNode* abolish_poison([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("abolish poison",
                              /*P*/ { NextAction("caster form") },
                              /*A*/ {},
                              /*C*/ {});
    }

    static ActionNode* abolish_poison_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("abolish poison on party",
                              /*P*/ { NextAction("caster form") },
                              /*A*/ {},
                              /*C*/ {});
    }

};

FeralDruidStrategy::FeralDruidStrategy(PlayerbotAI* botAI) : GenericDruidStrategy(botAI)
{
    actionNodeFactories.Add(new FeralDruidStrategyActionNodeFactory());
    actionNodeFactories.Add(new ShapeshiftDruidStrategyActionNodeFactory());
}

void FeralDruidStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericDruidStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode(
        "enemy out of melee", { NextAction("reach melee", 21.0f) }));
    triggers.push_back(new TriggerNode(
        "damage spike", { NextAction("survival instincts", 91.0f) }));
    triggers.push_back(new TriggerNode(
        "critical health", { NextAction("survival instincts", 91.0f) }));
    triggers.push_back(new TriggerNode("player has flag",
                                       { NextAction("dash", 92.0f) }));
    triggers.push_back(new TriggerNode("enemy flagcarrier near",
                                       { NextAction("dash", 92.0f) }));
}

void FeralChargeDruidStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Player* bot = botAI->GetBot();

    if (bot->HasSpell(SPELL_CAT_FORM) && !DruidHasThickHide(bot))
        triggers.push_back(new TriggerNode(
            "enemy out of melee", { NextAction("feral charge - cat", 29.0f) }));
    else
        triggers.push_back(new TriggerNode(
            // above the bear's "reach melee" (ACTION_MOVE + 0.5): at 18 it never fired, the bear always walked in
            "enemy out of melee", { NextAction("feral charge - bear", ACTION_MOVE + 1.0f) }));
}
