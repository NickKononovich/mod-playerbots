/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericShamanStrategy.h"
#include "AiFactory.h"
#include "Playerbots.h"
#include "Strategy.h"

class GenericShamanStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    GenericShamanStrategyActionNodeFactory()
    {
        creators["totem of wrath"] = &totem_of_wrath;
        creators["flametongue totem"] = &flametongue_totem;
        creators["magma totem"] = &magma_totem;
        creators["strength of earth totem"] = &strength_of_earth_totem;
        creators["cleansing totem"] = &cleansing_totem;
        creators["wrath of air totem"] = &wrath_of_air_totem;
        creators["windfury totem"] = &windfury_totem;
        creators["cleanse spirit"] = &cleanse_spirit;
        creators["cleanse spirit poison on party"] = &cleanse_spirit_poison_on_party;
        creators["cleanse spirit disease on party"] = &cleanse_spirit_disease_on_party;
    }

private:
    // Passthrough totems are set up so lower level shamans will still cast totems.
    // Totem of Wrath -> Flametongue Totem -> Searing Totem
    // Magma Totem -> Searing Totem
    // Strength of Earth Totem -> Stoneskin Totem
    // Cleansing Totem -> Mana Spring Totem
    // Wrath of Air Totem -> Windfury Totem -> Grounding Totem

    static ActionNode* totem_of_wrath([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("totem of wrath",
                              /*P*/ {},
                              /*A*/ { NextAction("flametongue totem") },
                              /*C*/ {});
    }
    static ActionNode* flametongue_totem([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("flametongue totem",
                              /*P*/ {},
                              /*A*/ { NextAction("searing totem") },
                              /*C*/ {});
    }
    static ActionNode* magma_totem([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("magma totem",
                              /*P*/ {},
                              /*A*/ { NextAction("searing totem") },
                              /*C*/ {});
    }
    static ActionNode* strength_of_earth_totem([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("strength of earth totem",
                              /*P*/ {},
                              /*A*/ { NextAction("stoneskin totem") },
                              /*C*/ {});
    }
    static ActionNode* cleansing_totem([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("cleansing totem",
                              /*P*/ {},
                              /*A*/ { NextAction("mana spring totem") },
                              /*C*/ {});
    }
    static ActionNode* wrath_of_air_totem([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("wrath of air totem",
                              /*P*/ {},
                              /*A*/ { NextAction("windfury totem") },
                              /*C*/ {});
    }
    static ActionNode* windfury_totem([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("windfury totem",
                              /*P*/ {},
                              /*A*/ { NextAction("grounding totem") },
                              /*C*/ {});
    }
    static ActionNode* cleanse_spirit([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("cleanse spirit",
                              /*P*/ {},
                              /*A*/ { NextAction("cure toxins") },
                              /*C*/ {});
    }
    static ActionNode* cleanse_spirit_poison_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("cleanse spirit poison on party",
                              /*P*/ {},
                              /*A*/ { NextAction("cure toxins poison on party") },
                              /*C*/ {});
    }
    static ActionNode* cleanse_spirit_disease_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("cleanse spirit disease on party",
                              /*P*/ {},
                              /*A*/ { NextAction("cure toxins disease on party") },
                              /*C*/ {});
    }
};

GenericShamanStrategy::GenericShamanStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericShamanStrategyActionNodeFactory());
}

void GenericShamanStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    CombatStrategy::InitTriggers(triggers);

    // Interrupts sit in the interrupt band, above movement (ACTION_MOVE), like every other class kick.
    triggers.push_back(new TriggerNode("wind shear", { NextAction("wind shear", ACTION_INTERRUPT + 1), }));
    triggers.push_back(new TriggerNode("wind shear on enemy healer",
                                       { NextAction("wind shear on enemy healer", ACTION_INTERRUPT + 2), }));
    triggers.push_back(new TriggerNode("purge", { NextAction("purge", ACTION_DISPEL), }));
    triggers.push_back(new TriggerNode("new pet", { NextAction("set pet stance", 65.0f), }));
}

void ShamanCureStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // A healer's dispels wait behind its critical and low heals.
    bool const isResto = AiFactory::GetPlayerSpecTab(botAI->GetBot()) == SHAMAN_TAB_RESTORATION;
    float const selfRelevance = isResto ? SHAMAN_RESTO_DISPEL_RELEVANCE + 0.1f : 24.0f;
    float const partyRelevance = isResto ? SHAMAN_RESTO_DISPEL_RELEVANCE : 23.0f;

    triggers.push_back(new TriggerNode("cleanse spirit poison", { NextAction("cleanse spirit", selfRelevance) }));
    triggers.push_back(new TriggerNode("party member cleanse spirit poison",
                                       { NextAction("cleanse spirit poison on party", partyRelevance) }));
    triggers.push_back(new TriggerNode("cleanse spirit disease", { NextAction("cleanse spirit", selfRelevance) }));
    triggers.push_back(new TriggerNode("party member cleanse spirit disease",
                                       { NextAction("cleanse spirit disease on party", partyRelevance) }));
    triggers.push_back(new TriggerNode("cleanse spirit curse", { NextAction("cleanse spirit", selfRelevance) }));
    triggers.push_back(new TriggerNode("party member cleanse spirit curse",
                                       { NextAction("cleanse spirit curse on party", partyRelevance) }));

    // A debuff that hurts now: a resto shaman takes it after critical heals (from 23), before low-health heals.
    float const urgentRelevance = isResto ? SHAMAN_RESTO_URGENT_DISPEL_RELEVANCE : ACTION_DISPEL;
    triggers.push_back(new TriggerNode("party member urgent cure poison",
                                       { NextAction("cleanse spirit poison on party", urgentRelevance) }));
    triggers.push_back(new TriggerNode("party member urgent cure disease",
                                       { NextAction("cleanse spirit disease on party", urgentRelevance) }));
    triggers.push_back(new TriggerNode("party member urgent cure curse",
                                       { NextAction("cleanse spirit curse on party", urgentRelevance) }));
}

void ShamanBoostStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("heroism", { NextAction("heroism", 30.0f), }));
    triggers.push_back(new TriggerNode("bloodlust", { NextAction("bloodlust", 30.0f), }));

    Player* bot = botAI->GetBot();
    int tab = AiFactory::GetPlayerSpecTab(bot);

    // [b31_sd] The in-combat re-imbues (Elemental main-hand Flametongue, Enhancement main-hand Windfury and off-hand
    // Flametongue) moved to the spec strategies. The imbue still outranks the Fire Elemental there (23.5 / 24.5 vs
    // 23 / 24). [b31_deep] The old reason given here was wrong: "boost" is a default combat strategy outside
    // battlegrounds (AiFactory::AddDefaultCombatStrategies); the imbue belongs to the spec all the same.
    if (tab == SHAMAN_TAB_ELEMENTAL)
        triggers.push_back(new TriggerNode("fire elemental totem", { NextAction("fire elemental totem", 23.0f), }));
    else if (tab == SHAMAN_TAB_ENHANCEMENT)
        triggers.push_back(new TriggerNode("fire elemental totem", { NextAction("fire elemental totem melee", 24.0f), }));
}

void ShamanAoeStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{

    Player* bot = botAI->GetBot();
    int tab = AiFactory::GetPlayerSpecTab(bot);

    if (tab == SHAMAN_TAB_ELEMENTAL)
    {
        triggers.push_back(new TriggerNode("medium aoe",{ NextAction("fire nova", 23.0f), }));
        // [b31_sd] Above Lava Burst (5.2), below Flame Shock (5.3). At 5.1 it was the filler's duplicate.
        // [b31_deep] 2+ mobs within the jump range of the target (ChainLightningAoeTrigger, wowsims numberTargets > 1).
        triggers.push_back(new TriggerNode("chain lightning aoe", { NextAction("chain lightning", 5.25f), }));
    }
    else if (tab == SHAMAN_TAB_ENHANCEMENT)
    {
        triggers.push_back(new TriggerNode("medium aoe",{ NextAction("fire nova", 23.0f), }));
        triggers.push_back(new TriggerNode("maelstrom weapon 5 and medium aoe", { NextAction("chain lightning", 22.0f), }));
        triggers.push_back(new TriggerNode("maelstrom weapon 4 and medium aoe", { NextAction("chain lightning", 21.0f), }));
        // [shside_enh_mana] No Fire Nova on "enemy within melee": 22 % base mana on one target was the largest
        // single drain of the rotation. Fire Nova is the 3+ mob ("medium aoe") tool only.
    }
    // Resto AoE handled by "Healer DPS" Strategy
}
