/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BalanceDruidStrategy.h"
#include "FeralDruidStrategy.h"

class BalanceDruidStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    BalanceDruidStrategyActionNodeFactory()
    {
        creators["faerie fire"] = &faerie_fire;
        creators["hibernate"] = &hibernate;
        creators["entangling roots"] = &entangling_roots;
        creators["entangling roots on cc"] = &entangling_roots_on_cc;
        creators["wrath"] = &wrath;
        creators["starfall"] = &starfall;
        creators["insect swarm"] = &insect_swarm;
        creators["moonfire"] = &moonfire;
        creators["starfire"] = &starfire;
        creators["moonkin form"] = &moonkin_form;
        creators["typhoon"] = &typhoon;
        creators["hurricane"] = &hurricane;
        creators["force of nature"] = &force_of_nature;
        creators["cyclone on cc"] = &cyclone_on_cc;
    }

private:
    static ActionNode* faerie_fire([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "faerie fire",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* hibernate([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "hibernate",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ { NextAction("entangling roots") },
            /*C*/ {}
        );
    }

    static ActionNode* entangling_roots([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "entangling roots",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* entangling_roots_on_cc([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "entangling roots on cc",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* wrath([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "wrath",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* starfall([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "starfall",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* insect_swarm([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "insect swarm",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* moonfire([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "moonfire",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* starfire([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "starfire",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    // [death416] the prerequisite of every Balance spell: before Moonkin Form is learned (talent, level 40) it is
    // impossible and its own "caster form" prerequisite never ran - the druid stayed in Aquatic / Travel Form and every
    // cast failed (b456 Salleas L19 under the Zoram Strand: 15 "[stuck258] ... no action that runs", then 39 s in a
    // fight without a cast, dead). It leaves the animal form instead
    static ActionNode* moonkin_form([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "moonkin form",
            /*P*/ { NextAction("caster form") },
            /*A*/ { NextAction("leave animal form") },
            /*C*/ {}
        );
    }

    static ActionNode* typhoon([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "typhoon",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* hurricane([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "hurricane",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* force_of_nature([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "force of nature",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* cyclone_on_cc([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "cyclone on cc",
            /*P*/ { NextAction("moonkin form") },
            /*A*/ {},
            /*C*/ {}
        );
    }
};

BalanceDruidStrategy::BalanceDruidStrategy(PlayerbotAI* botAI) : GenericDruidStrategy(botAI)
{
    actionNodeFactories.Add(new BalanceDruidStrategyActionNodeFactory());
    actionNodeFactories.Add(new ShapeshiftDruidStrategyActionNodeFactory());
}

std::vector<NextAction> BalanceDruidStrategy::getDefaultActions()
{
    return {
        NextAction("moonfire on the move", 5.5f),
        NextAction("starfire", 5.4f),
        NextAction("wrath", 5.3f),
    };
}

void BalanceDruidStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericDruidStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("faerie fire", { NextAction("faerie fire", 29.5f) }));
    // [rot1] Moonkin Form back as soon as nothing the form forbids is being cast (MoonkinFormUpkeepTrigger): above the
    // nukes and DoTs (19.9-20.5), below Faerie Fire / Innervate (29+), which take the form as their prerequisite anyway
    triggers.push_back(new TriggerNode("no moonkin form", { NextAction("moonkin form", ACTION_HIGH + 5.0f) }));
    // [b31_deep] The DoTs go up only when they are missing from the main target: the old "insect swarm" (19.5) and
    // "moonfire" (19.4) refresh nodes had the same condition (DebuffTrigger with no refresh window) and never won.
    // wowsims balance APL: Moonfire missing ranks above the eclipse nukes (20.0); Insect Swarm only outside an
    // eclipse (APL "IS when the Lunar ICD is not ready", Warcraft Tavern "refresh DoTs outside eclipse"), so below.
    triggers.push_back(new TriggerNode("moonfire missing", { NextAction("moonfire", 20.1f) }));
    triggers.push_back(new TriggerNode("insect swarm missing", { NextAction("insect swarm", 19.9f) }));

    // Eclipse procs
    triggers.push_back(new TriggerNode("eclipse (solar)", { NextAction("wrath", 20.0f) }));
    triggers.push_back(new TriggerNode("eclipse (lunar)", { NextAction("starfire", 20.0f) }));

    // Utility/Defensive
    triggers.push_back(new TriggerNode("medium mana", { NextAction("innervate", 29.0f) }));
    // [b31_deep] In a group the moonkin steps out of melee only in danger (low health or 2+ mobs on it); one mob
    // is the tank's to pick up and the cast goes on (1.4 % of the L80 combat ticks fled before).
    triggers.push_back(new TriggerNode("enemy too close for spell in danger", { NextAction("flee", 39.0f) }));

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus lunar surge", { NextAction("starfire", 20.5f) }));
}
