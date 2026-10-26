/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericMageStrategy.h"
#include "AiFactory.h"
#include "Playerbots.h"
#include "RangedCombatStrategy.h"

namespace
{
constexpr uint32 SPELL_FROSTFIRE_BOLT = 44614;
constexpr uint32 SPELL_ICE_SHARDS = 15047;
}

class GenericMageStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    // [b31_deep] no "shoot" alternative on frostbolt / scorch / fireball: an impossible cast (GCD, line of sight)
    // pushed the wand at the spell's relevance + 0.003 and started auto-repeat instead of waiting for the GCD; the
    // wand stays in each spec's default actions
    GenericMageStrategyActionNodeFactory()
    {
        creators["frostfire bolt"] = &frostfire_bolt;
        creators["remove curse"] = &remove_curse;
        creators["remove curse on party"] = &remove_curse_on_party;
    }

private:
    static ActionNode* frostfire_bolt([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("frostfire bolt",
                              /*P*/ {},
                              /*A*/ { NextAction("fireball") },
                              /*C*/ {});
    }

    static ActionNode* remove_curse([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("remove curse",
                              /*P*/ {},
                              /*A*/ { NextAction("remove lesser curse") },
                              /*C*/ {});
    }

    static ActionNode* remove_curse_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("remove curse on party",
                              /*P*/ {},
                              /*A*/ { NextAction("remove lesser curse on party") },
                              /*C*/ {});
    }
};

GenericMageStrategy::GenericMageStrategy(PlayerbotAI* botAI) : RangedCombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericMageStrategyActionNodeFactory());
}

void GenericMageStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    RangedCombatStrategy::InitTriggers(triggers);

    // Threat Triggers
    triggers.push_back(new TriggerNode("medium threat", { NextAction("mirror image", 60.0f) }));
    // [b31_deep] no Invisibility on "medium threat": a 3 s fade that stops the damage or is broken by the next cast
    // (a lost GCD); it stays on "high threat in raid" below
    triggers.push_back(new TriggerNode("high threat", { NextAction("mirror image", 60.0f) }));
    triggers.push_back(new TriggerNode("high threat in raid", { NextAction("invisibility", 30.0f) }));

    // Defensive Triggers
    triggers.push_back(new TriggerNode("critical health", { NextAction("ice block", 90.0f) }));
    triggers.push_back(new TriggerNode("low health", { NextAction("mana shield", 85.0f) }));
    triggers.push_back(new TriggerNode("fire ward", { NextAction("fire ward", 90.0f) }));
    triggers.push_back(new TriggerNode("frost ward", { NextAction("frost ward", 90.0f) }));
    // [b31_deep] in a group only in self-defence (a mob on the mage), not on mobs the tank holds
    triggers.push_back(new TriggerNode("enemy is close and no firestarter strategy and solo or being attacked",
                                       { NextAction("frost nova", 50.0f) }));
    // above interrupts: a melee mob on the healer kills it in a few swings
    triggers.push_back(new TriggerNode("healer attacker in frost nova range",
                                       { NextAction("frost nova on healer attacker", ACTION_INTERRUPT + 2.0f) }));
    triggers.push_back(
        new TriggerNode("enemy too close for spell and no firestarter strategy and solo or being attacked",
                        { NextAction("blink back", 35.0f) }));

    // Mana Threshold Triggers
    // [b31_deep] the gem actually in the bags, best first (each use action checks the bag and the shared gem
    // cooldown), instead of the one gem the highest known Conjure spell makes at strategy init
    triggers.push_back(new TriggerNode("high mana", {
                                                   NextAction("use mana sapphire", ACTION_EMERGENCY - 0.1f),
                                                   NextAction("use mana emerald", ACTION_EMERGENCY - 0.2f),
                                                   NextAction("use mana ruby", ACTION_EMERGENCY - 0.3f),
                                                   NextAction("use mana citrine", ACTION_EMERGENCY - 0.4f),
                                                   NextAction("use mana jade", ACTION_EMERGENCY - 0.5f),
                                                   NextAction("use mana agate", ACTION_EMERGENCY - 0.6f) }));

    // [b31_deep] wowsims arcane / frost APL: Evocation at <= 25 % mana (was "low mana", 15 %)
    triggers.push_back(new TriggerNode("mana at or below 25", { NextAction("evocation", ACTION_EMERGENCY - 1.0f) }));

    // Counterspell / Spellsteal Triggers
    triggers.push_back(new TriggerNode("spellsteal", { NextAction("spellsteal", 40.0f) }));
    triggers.push_back(new TriggerNode("counterspell", { NextAction("counterspell", ACTION_INTERRUPT + 1) }));
    triggers.push_back(new TriggerNode("counterspell on enemy healer", { NextAction("counterspell on enemy healer", 40.0f) }));
}

void MageCureStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("remove curse", { NextAction("remove curse", 41.0f) }));
    triggers.push_back(new TriggerNode("remove curse on party", { NextAction("remove curse on party", 40.0f) }));
    triggers.push_back(
        new TriggerNode("party member urgent cure curse", { NextAction("remove curse on party", ACTION_DISPEL) }));
}

void MageBoostStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Player* bot = botAI->GetBot();
    int tab = AiFactory::GetPlayerSpecTab(bot);

    if (tab == MAGE_TAB_ARCANE)
    {
        triggers.push_back(new TriggerNode("arcane power", { NextAction("arcane power", 29.0f) }));
        triggers.push_back(new TriggerNode("presence of mind", { NextAction("presence of mind", 28.8f) }));
        triggers.push_back(new TriggerNode("icy veins", { NextAction("icy veins", 28.5f) }));
        triggers.push_back(new TriggerNode("mirror image", { NextAction("mirror image", 28.0f) }));
    }
    else if (tab == MAGE_TAB_FIRE)
    {
        if (bot->HasSpell(SPELL_FROSTFIRE_BOLT) && bot->HasAura(SPELL_ICE_SHARDS))
        { // Frostfire
            triggers.push_back(new TriggerNode("combustion", { NextAction("combustion", 18.0f) }));
            triggers.push_back(new TriggerNode("icy veins", { NextAction("icy veins", 17.5f) }));
            triggers.push_back(new TriggerNode("mirror image", { NextAction("mirror image", 17.0f) }));
        }
        else
        { // Fire
            triggers.push_back(new TriggerNode("combustion", { NextAction("combustion", 18.0f) }));
            triggers.push_back(new TriggerNode("mirror image", { NextAction("mirror image", 17.5f) }));
        }
    }
    else if (tab == MAGE_TAB_FROST)  // Frost
    {
        triggers.push_back(new TriggerNode("cold snap", { NextAction("cold snap", 28.0f) }));
        triggers.push_back(new TriggerNode("icy veins", { NextAction("icy veins", 27.5f) }));
        triggers.push_back(new TriggerNode("mirror image", { NextAction("mirror image", 26.0f) }));
    }
}

void MageCcStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("polymorph", { NextAction("polymorph", 30.0f) }));
}

void MageAoeStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("blizzard channel check", { NextAction("cancel channel", 26.0f) }));

    Player* bot = botAI->GetBot();
    int tab = AiFactory::GetPlayerSpecTab(bot);

    if (tab == MAGE_TAB_ARCANE)
    {
        triggers.push_back(new TriggerNode("flamestrike active and medium aoe", { NextAction("blizzard", 24.0f) }));
        triggers.push_back(new TriggerNode("medium aoe", {
                                                     NextAction("flamestrike", 23.0f),
                                                     NextAction("blizzard", 22.0f) }));
    }
    else if (tab == MAGE_TAB_FIRE)
    {
        triggers.push_back(
            new TriggerNode("medium aoe", {
                                      NextAction("dragon's breath", 39.0f),
                                      NextAction("blast wave", 38.0f),
                                      NextAction("flamestrike", 23.0f),
                                      NextAction("blizzard", 22.0f) }));

        triggers.push_back(new TriggerNode("flamestrike active and medium aoe", { NextAction("blizzard", 24.0f) }));
        triggers.push_back(new TriggerNode("firestarter", { NextAction("flamestrike", 40.0f) }));
        // [guide_b31_dead] below Living Bomb on the main target (18.5, FireMageStrategy) and Improved Scorch (19)
        triggers.push_back(new TriggerNode("living bomb on attackers", { NextAction("living bomb on attackers", 18.0f) }));
    }
    else if (tab == MAGE_TAB_FROST)
    {
        triggers.push_back(new TriggerNode("flamestrike active and medium aoe", { NextAction("blizzard", 24.0f) }));
        triggers.push_back(new TriggerNode("medium aoe", {
                                                     NextAction("flamestrike", 23.0f),
                                                     NextAction("blizzard", 22.0f) }));
        triggers.push_back(new TriggerNode("light aoe", { NextAction("cone of cold", 21.0f) }));
    }
}
