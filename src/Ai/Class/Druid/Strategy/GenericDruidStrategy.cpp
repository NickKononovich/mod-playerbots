/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericDruidStrategy.h"
#include "AiFactory.h"
#include "FeralDruidStrategy.h"
#include "Playerbots.h"

class GenericDruidStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    GenericDruidStrategyActionNodeFactory()
    {
        creators["entangling roots on cc"] = &entangling_roots_on_cc;
        creators["cyclone on cc"] = &cyclone_on_cc;
        creators["hibernate on cc"] = &hibernate_on_cc;
    }

private:
    static ActionNode* entangling_roots_on_cc([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("entangling roots on cc",
                              /*P*/ { NextAction("caster form") },
                              /*A*/ {},
                              /*C*/ {});
    }

    static ActionNode* cyclone_on_cc([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("cyclone on cc",
                              /*P*/ { NextAction("caster form") },
                              /*A*/ {},
                              /*C*/ {});
    }

    static ActionNode* hibernate_on_cc([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("hibernate on cc",
                              /*P*/ { NextAction("caster form") },
                              /*A*/ {},
                              /*C*/ {});
    }
};

GenericDruidStrategy::GenericDruidStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericDruidStrategyActionNodeFactory());
}

void GenericDruidStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    CombatStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode("almost full health", { NextAction("barkskin", 40.0f) }));
    triggers.push_back(
        new TriggerNode("medium health", { NextAction("barkskin", 40.0f) }));
    triggers.push_back(
        new TriggerNode("damage spike", { NextAction("barkskin", 41.0f) }));

    Player* bot = botAI->GetBot();
    int tab = AiFactory::GetPlayerSpecTab(bot);

    if (tab == DRUID_TAB_FERAL)
    {
        if (!DruidHasThickHide(bot) /*thick hide — bear spec [b31_sd] any rank*/)
        {
            triggers.push_back(new TriggerNode("predator's swiftness and combat party member dead",
                                               { NextAction("rebirth", 29.0f) }));
            triggers.push_back(new TriggerNode("combat party member dead",
                                               { NextAction("rebirth", 28.5f) }));
        }
    }
    else
    {
        triggers.push_back(new TriggerNode("combat party member dead",
                                           { NextAction("rebirth", 29.0f) }));
        // The ally back from Rebirth first: healed to a safe level above every other heal (a restoration druid;
        // a moonkin cannot heal in form), then Thorns on a tank and its Mark of the Wild.
        if (tab == DRUID_TAB_RESTORATION)
            triggers.push_back(new TriggerNode("rebirth target low health",
                { NextAction("tree form", ACTION_CRITICAL_HEAL + 5.5f),
                  NextAction("rejuvenation on rebirth target", ACTION_CRITICAL_HEAL + 5.4f),
                  NextAction("swiftmend on rebirth target", ACTION_CRITICAL_HEAL + 5.3f),
                  NextAction("regrowth on rebirth target", ACTION_CRITICAL_HEAL + 5.2f),
                  NextAction("nourish on rebirth target", ACTION_CRITICAL_HEAL + 5.1f),
                  NextAction("healing touch on rebirth target", ACTION_CRITICAL_HEAL + 5) }));
        triggers.push_back(new TriggerNode("rebirth target unbuffed",
            { NextAction("thorns on rebirth target", ACTION_CRITICAL_HEAL + 4.5f),
              NextAction("mark of the wild on rebirth target", ACTION_CRITICAL_HEAL + 4.4f) }));
    }

    // [b31_deep] Nature's Grasp is on the GCD (1.5 s) and roots what hits the druid. A bear tank is always "being
    // attacked": the root held the mobs it gathers and cost a GCD per pull (0.9-1.8/min), so it gets none. A
    // restoration druid casts it below its critical heal band (30.0-34.1) and the urgent cures (29.5).
    bool const bearTank = tab == DRUID_TAB_FERAL && DruidHasThickHide(bot);
    if (!bearTank)
        triggers.push_back(new TriggerNode("being attacked",
            { NextAction("nature's grasp", tab == DRUID_TAB_RESTORATION ? 28.9f : 39.0f) }));
}

void DruidCureStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // A resto druid dispels after its critical and low heals (low band starts at 21).
    bool const isResto = AiFactory::GetPlayerSpecTab(botAI->GetBot()) == DRUID_TAB_RESTORATION;

    triggers.push_back(
        new TriggerNode("party member cure poison",
                        { NextAction("abolish poison on party", isResto ? 20.85f : 51.0f) }));

    triggers.push_back(
        new TriggerNode("party member remove curse",
                        { NextAction("remove curse on party", isResto ? 20.9f : 57.0f) }));

    // A debuff that hurts now goes before low-health heals, still after the critical ones (from 30).
    triggers.push_back(
        new TriggerNode("party member urgent cure poison",
                        { NextAction("abolish poison on party", isResto ? 29.5f : 57.5f) }));
    triggers.push_back(
        new TriggerNode("party member urgent cure curse",
                        { NextAction("remove curse on party", isResto ? 29.6f : 57.6f) }));
}

void DruidBoostStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Player* bot = botAI->GetBot();
    int tab = AiFactory::GetPlayerSpecTab(bot);

    if (tab == DRUID_TAB_BALANCE)
    {
        triggers.push_back(new TriggerNode("force of nature", { NextAction("force of nature", 29.0f) }));
        triggers.push_back(new TriggerNode("new pet", { NextAction("set pet stance", 60.0f) }));
    }

    if (tab == DRUID_TAB_FERAL)
    {
        triggers.push_back(new TriggerNode("berserk", { NextAction("berserk", 27.5f) }));
    }
}

void DruidCcStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Player* bot = botAI->GetBot();
    int tab = AiFactory::GetPlayerSpecTab(bot);

    if (tab == DRUID_TAB_BALANCE || tab == DRUID_TAB_RESTORATION)
    {
        triggers.push_back(new TriggerNode(
            "cyclone", { NextAction("cyclone on cc", 42.0f) }));
        triggers.push_back(new TriggerNode(
            "hibernate", { NextAction("hibernate on cc", 41.0f) }));
        triggers.push_back(new TriggerNode(
            "entangling roots", { NextAction("entangling roots on cc", 40.0f) }));
    }
    if (tab == DRUID_TAB_FERAL)
    {
        if (bot->HasSpell(SPELL_CAT_FORM) && !DruidHasThickHide(bot))
        {
            triggers.push_back(new TriggerNode(
                "predator's swiftness and cyclone", { NextAction("cyclone on cc", 42.0f) }));
            triggers.push_back(new TriggerNode(
                "predator's swiftness and hibernate", { NextAction("hibernate on cc", 41.0f) }));
            triggers.push_back(new TriggerNode(
                "predator's swiftness and entangling roots", { NextAction("entangling roots on cc", 40.0f) }));
        }
        else
        {
            triggers.push_back(new TriggerNode(
                "cyclone", { NextAction("cyclone on cc", 42.0f) }));
            triggers.push_back(new TriggerNode(
                "hibernate", { NextAction("hibernate on cc", 41.0f) }));
            triggers.push_back(new TriggerNode(
                "entangling roots", { NextAction("entangling roots on cc", 40.0f) }));
        }
    }
}

void DruidHealerDpsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("healer should attack and not blanketing",
                        {
                            NextAction("cancel tree form", 5.4f),
                            NextAction("moonfire",         5.3f),
                            NextAction("wrath",            5.2f),
                            NextAction("starfire",         5.1f),
                        }));
}

void DruidAoeStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Player* bot = botAI->GetBot();
    int tab = AiFactory::GetPlayerSpecTab(bot);

    if (tab == DRUID_TAB_BALANCE)
    {
        triggers.push_back(new TriggerNode("hurricane channel check", { NextAction("cancel channel", 22.0f) }));
        triggers.push_back(new TriggerNode("starfall", { NextAction("starfall", 28.5f) }));
        triggers.push_back(new TriggerNode("medium aoe", { NextAction("hurricane", 23.0f) }));
        triggers.push_back(new TriggerNode("enemy within melee", { NextAction("typhoon", 40.0f) }));
        // [b31_sd] Multi-DoT was dead at 5.2 / 5.1, below the Starfire / Wrath default nukes (5.4 / 5.3). The triggers
        // now need 2+ attackers and a long-lived mob; the DoTs rank below the main-target DoTs (19.4-20.2) and the
        // eclipse nukes (20.0), above the filler nukes.
        triggers.push_back(new TriggerNode("insect swarm on attacker",
                                           { NextAction("insect swarm on attacker", ACTION_NORMAL + 1.0f) }));
        triggers.push_back(new TriggerNode("moonfire on attacker",
                                           { NextAction("moonfire on attacker", ACTION_NORMAL + 0.9f) }));
    }

    if (tab == DRUID_TAB_RESTORATION)
    {
        triggers.push_back(new TriggerNode("hurricane channel check", { NextAction("cancel channel", 22.0f) }));
        triggers.push_back(new TriggerNode("medium aoe", { NextAction("hurricane", 23.0f) }));
        triggers.push_back(new TriggerNode("insect swarm on attacker", { NextAction("insect swarm on attacker", 5.2f) }));
        triggers.push_back(new TriggerNode("moonfire on attacker", { NextAction("moonfire on attacker", 5.1f) }));
    }

    if (tab == DRUID_TAB_FERAL && bot->HasSpell(SPELL_CAT_FORM) && !DruidHasThickHide(bot))
    {
        triggers.push_back(new TriggerNode("clearcasting and medium aoe", { NextAction("swipe (cat)", 25.5f) }));
        triggers.push_back(new TriggerNode("medium aoe", { NextAction("swipe (cat)", 25.0f) }));
    }
}
