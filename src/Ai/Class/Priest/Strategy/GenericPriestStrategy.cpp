/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericPriestStrategy.h"
#include "GenericPriestStrategyActionNodeFactory.h"
#include "Playerbots.h"

GenericPriestStrategy::GenericPriestStrategy(PlayerbotAI* botAI) : RangedCombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericPriestStrategyActionNodeFactory());
}

void GenericPriestStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    CombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("medium threat", { NextAction("fade", 55.0f) }));
    triggers.push_back(new TriggerNode("high threat", { NextAction("fade", 55.0f) }));
    triggers.push_back(new TriggerNode("critical health", { NextAction("desperate prayer",
        ACTION_HIGH + 5) }));
    triggers.push_back(new TriggerNode(
        "critical health", { NextAction("power word: shield", ACTION_NORMAL) }));
    triggers.push_back(new TriggerNode("fear ward on main tank",
        { NextAction("fear ward on main tank", ACTION_HIGH + 3) }));
    triggers.push_back(
        new TriggerNode("low health", { NextAction("power word: shield", ACTION_HIGH) }));
    // [b31_deep] Inner Focus is now a prereq of Divine Hymn / Prayer of Healing / Serendipity Greater Heal
    // (GenericPriestStrategyActionNodeFactory.h); here it went into whatever spell came next.
    triggers.push_back(new TriggerNode("medium mana", { NextAction("shadowfiend", ACTION_HIGH + 2) }));
    triggers.push_back(
        new TriggerNode("hymn of hope needed", { NextAction("hymn of hope", ACTION_HIGH) }));
    triggers.push_back(new TriggerNode("enemy too close for spell",
                                       { NextAction("flee", ACTION_MOVE + 9) }));
    triggers.push_back(new TriggerNode("often", { NextAction("apply oil", 1.0f) }));
    triggers.push_back(new TriggerNode("being attacked",
        { NextAction("power word: shield", ACTION_HIGH + 1) }));
    triggers.push_back(new TriggerNode("new pet", { NextAction("set pet stance", 60.0f) }));
}

PriestCureStrategy::PriestCureStrategy(PlayerbotAI* botAI) : Strategy(botAI)
{
    actionNodeFactories.Add(new CurePriestStrategyActionNodeFactory());
}

void PriestCureStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("dispel magic", { NextAction("dispel magic", ACTION_LIGHT_HEAL + 9.6f) }));
    triggers.push_back(new TriggerNode("dispel magic on party",
                                       { NextAction("dispel magic on party", ACTION_LIGHT_HEAL + 9.5f) }));
    triggers.push_back(
        new TriggerNode("cure disease", { NextAction("abolish disease", ACTION_LIGHT_HEAL + 9.4f) }));
    triggers.push_back(new TriggerNode(
        "party member cure disease", { NextAction("abolish disease on party", ACTION_LIGHT_HEAL + 9.3f) }));

    // A debuff that hurts now: after critical heals (from 31), before medium and low ones.
    triggers.push_back(new TriggerNode("party member urgent cure magic",
                                       { NextAction("dispel magic on party", ACTION_CRITICAL_HEAL - 0.4f) }));
    triggers.push_back(new TriggerNode("party member urgent cure disease",
                                       { NextAction("abolish disease on party", ACTION_CRITICAL_HEAL - 0.5f) }));
}

void PriestBoostStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("power infusion", { NextAction("power infusion", 41.0f) }));
    // [guide_b31_dead] "boost" was never registered as a priest trigger, so this node was dead.
    triggers.push_back(new TriggerNode("shadowfiend", { NextAction("shadowfiend", 20.0f) }));
}

void PriestCcStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("shackle undead", { NextAction("shackle undead", 31.0f) }));
}

void PriestHealerDpsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("healer should attack",
                        {
                            NextAction("shadow word: pain", ACTION_DEFAULT + 0.5f),
                            NextAction("holy fire", ACTION_DEFAULT + 0.4f),
                            NextAction("smite", ACTION_DEFAULT + 0.3f),
                            NextAction("mind blast", ACTION_DEFAULT + 0.2f),
                            NextAction("shoot", ACTION_DEFAULT) }));

    triggers.push_back(
        new TriggerNode("medium aoe and healer should attack",
                        {
                            NextAction("mind sear", ACTION_DEFAULT + 0.5f) }));
}
