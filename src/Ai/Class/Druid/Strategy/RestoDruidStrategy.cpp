/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RestoDruidStrategy.h"
#include "HealthTriggers.h"
#include "Playerbots.h"

RestoDruidStrategy::RestoDruidStrategy(PlayerbotAI* botAI) : GenericDruidStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

void RestoDruidStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    GenericDruidStrategy::InitMultipliers(multipliers);

    multipliers.push_back(new HealerEmergencyMultiplier(botAI));
}

void RestoDruidStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericDruidStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("no healer dps strategy",
                                       { NextAction("tree form", 5.0f) }));

    triggers.push_back(new TriggerNode(
        "party member to heal out of spell range",
        { NextAction("reach party member to heal", 39.0f) }));

    triggers.push_back(
        new TriggerNode("party member critical health",
                        {
                            NextAction("tree form",              34.1f),
                            NextAction("swiftmend on party",     34.0f),
                            NextAction("wild growth on party",   33.0f),
                            NextAction("nourish on party",       32.0f),
                            NextAction("regrowth on party",      31.0f),
                            NextAction("healing touch filler on party", 30.0f),  // [b31_sd] no Nourish only
                        }));

    triggers.push_back(
        new TriggerNode("party member critical health",
                        { NextAction("nature's swiftness", 58.0f) }));

    // [heal_triage b29] EMERGENCY (one member below 35 %): the single-target saves come before the group heals
    // (Wild Growth 35.1); Swiftmend (instant) also before Tranquility (36.5), Regrowth / Nourish between the two.
    // HoR escape 2026-10-25 16:58:35-39: the mage at 4-28 % got Wild Growth as the first heal.
    triggers.push_back(
        new TriggerNode("party member emergency health",
                        {
                            NextAction("nature's swiftness",  58.0f),
                            NextAction("tree form",           37.6f),
                            NextAction("swiftmend on party",  37.4f),
                            NextAction("regrowth on party",   36.2f),
                            NextAction("nourish on party",    36.1f),
                        }));

    // [heal_triage] GROUP-WIDE DAMAGE (3+ members below 70 %), above the single-target critical band: Wild Growth hits
    // the whole party for one global cooldown, then Swiftmend on the lowest member with a HoT. HoR heroic Falric
    // 2026-10-25: after each Defiling Horror the druid healed one member at a time (or none) while four sat at 17-40 %.
    triggers.push_back(
        new TriggerNode("group damage",
                        {
                            NextAction("tree form",            35.2f),
                            NextAction("wild growth on party", 35.1f),
                            NextAction("swiftmend on party",   35.0f),
                        }));

    triggers.push_back(new TriggerNode(
        "nature's swiftness active",
        { NextAction("healing touch on party", 55.0f) }));

    // [b31_deep] Clearcasting goes to an expensive spell: Lifebloom when the tank's stack needs it, otherwise
    // Regrowth on the member to heal (12530 Clearcasting ticks on L80 bosses found Lifebloom not useful and spent
    // the free cast on whatever came next).
    triggers.push_back(new TriggerNode("clearcasting",
        { NextAction("lifebloom on main tank", 13.0f), NextAction("regrowth on party", 12.9f) }));

    // LOSING HEALTH FAST, above the low band and the main tank HoT upkeep: Rejuvenation so Swiftmend can follow on
    // the next global cooldown, then Nourish (+20 % on a target with a HoT) and Regrowth
    triggers.push_back(
        new TriggerNode("party member losing health fast",
                        {
                            NextAction("swiftmend on party",     22.4f),
                            NextAction("rejuvenation on party",  22.3f),
                            NextAction("nourish on party",       22.2f),
                            NextAction("regrowth on party",      22.1f),
                        }));

    // LOW
    triggers.push_back(
        new TriggerNode("party member low health",
                        {
                            NextAction("tree form",              21.5f),
                            NextAction("swiftmend on party",     21.4f),
                            NextAction("wild growth on party",   21.3f),
                            NextAction("nourish on party",       21.2f),
                            NextAction("regrowth on party",      21.1f),
                            NextAction("healing touch filler on party", 21.0f),
                        }));

    // Rolling HoTs on the main tank, between the low and medium bands
    triggers.push_back(new TriggerNode("lifebloom on main tank",
                                       { NextAction("lifebloom on main tank", 20.8f) }));
    triggers.push_back(new TriggerNode("rejuvenation on main tank",
                                       { NextAction("rejuvenation on main tank", 20.7f) }));

    // [b31_sd] Wild Growth on cooldown (GUIDE_AUDIT_HEALERS.md: 0.21/min on bosses, guide: on CD on a hurt group):
    // in the medium and almost-full bands it sat below the tank's Lifebloom / Rejuvenation upkeep (20.8 / 20.7), at
    // 20.3 and 10.3. Now 20.75, between the two upkeeps; CastWildGrowthOnPartyAction::isUseful() still needs 2 hurt
    // members (3 in a raid) around its target, so on one hurt member the bands go on as before.
    triggers.push_back(new TriggerNode("party member medium health",
                                       { NextAction("wild growth on party", 20.75f) }));
    triggers.push_back(new TriggerNode("party member almost full health",
                                       { NextAction("wild growth on party", 20.75f) }));

    // MEDIUM
    triggers.push_back(
        new TriggerNode("party member medium health",
                        {
                            NextAction("tree form",              20.5f),
                            NextAction("rejuvenation on party",  20.4f),
                            NextAction("nourish on party",       20.2f),
                            NextAction("regrowth on party",      20.1f),
                            NextAction("healing touch filler on party", 20.0f),
                        }));

    // ALMOST FULL
    // [b31_sd] Nourish between Rejuvenation and Regrowth: on a target that already has a HoT (Rejuvenation comes
    // first) it is the cheaper and larger heal (+20 %).
    triggers.push_back(
        new TriggerNode("party member almost full health",
                        {
                            NextAction("rejuvenation on party", 10.2f),
                            NextAction("nourish on party",      10.15f),
                            NextAction("regrowth on party",     10.1f),
                        }));

    triggers.push_back(
        new TriggerNode("medium mana", { NextAction("innervate", 25.0f) }));
    triggers.push_back(
        new TriggerNode("healer low mana", { NextAction("innervate on healer", 24.5f) }));

    // [b31_deep] The healer does not stop healing to flee unless it is in danger itself (own health below the
    // medium band or 2+ mobs on it); flee at 39 sat above the critical band (34.x) for any mob in melee.
    triggers.push_back(new TriggerNode("enemy too close for spell in danger",
                                       { NextAction("flee", 39.0f) }));

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus titanic bloom", { NextAction("swiftmend on party", 22.5f) }));
    triggers.push_back(new TriggerNode("set bonus ascendant bloom", { NextAction("regrowth on party", 22.45f) }));
}

void DruidTranquilityStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // [heal_triage] 3+ members below 40 %: Tranquility is the cooldown for it, above every single-target heal
    // (Nature's Swiftness + Healing Touch at 58 still saves a dying member first, it is off the global cooldown).
    triggers.push_back(new TriggerNode("group critical damage",
                                       { NextAction("tree form", 36.6f), NextAction("tranquility", 36.5f) }));
    triggers.push_back(new TriggerNode("medium group heal setting",
                                       { NextAction("tree form", 30.6f), NextAction("tranquility", 30.5f) }));
}

class DruidBlanketStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    DruidBlanketStrategyActionNodeFactory()
    {
        creators["wild growth blanket"] = &wild_growth_blanket;
        creators["rejuvenation blanket"] = &rejuvenation_blanket;
    }

private:
    static ActionNode* wild_growth_blanket([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("wild growth blanket",
                              /*P*/ { NextAction("tree form") },
                              /*A*/ {},
                              /*C*/ {});
    }

    static ActionNode* rejuvenation_blanket([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("rejuvenation blanket",
                              /*P*/ { NextAction("tree form") },
                              /*A*/ {},
                              /*C*/ {});
    }
};

DruidBlanketStrategy::DruidBlanketStrategy(PlayerbotAI* botAI) : Strategy(botAI)
{
    actionNodeFactories.Add(new DruidBlanketStrategyActionNodeFactory());
}

void DruidBlanketStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("wild growth blanket", { NextAction("wild growth blanket", 8.0f) }));

    triggers.push_back(new TriggerNode("rejuvenation blanket", { NextAction("rejuvenation blanket", 6.0f) }));
}
