/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HolyPriestStrategy.h"
#include "HealthTriggers.h"
#include "Playerbots.h"

class HolyDpsPriestStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    HolyDpsPriestStrategyActionNodeFactory() { creators["smite"] = &smite; }

private:
    static ActionNode* smite([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "smite",
            /*P*/ {},
            /*A*/ { NextAction("shoot") },
            /*C*/ {}
        );
    }
};

HolyDpsPriestStrategy::HolyDpsPriestStrategy(PlayerbotAI* botAI) : GenericPriestStrategy(botAI)
{
    actionNodeFactories.Add(new HolyDpsPriestStrategyActionNodeFactory());
}

std::vector<NextAction> HolyDpsPriestStrategy::getDefaultActions()
{
    return {
        NextAction("smite", ACTION_DEFAULT + 0.2f),
        NextAction("mana burn", ACTION_DEFAULT + 0.1f),
        NextAction("starshards", ACTION_DEFAULT)
    };
}

void HolyDpsPriestStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericPriestStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode(
            "holy fire",
            {
                NextAction("holy fire", ACTION_NORMAL + 9)
            }
        )
    );
    // [rot603] Mind Blast where it pays against Smite, below Holy Fire
    triggers.push_back(new TriggerNode("mind blast pays", { NextAction("mind blast", ACTION_NORMAL + 8) }));
    triggers.push_back(
        new TriggerNode(
            "shadowfiend",
            {
                NextAction("shadowfiend", ACTION_HIGH)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "medium mana",
            {
                NextAction("shadowfiend", ACTION_HIGH)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "low mana",
            {
                NextAction("mana burn", ACTION_HIGH)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "party member low health",
            {
                NextAction("power word: shield on party", ACTION_MEDIUM_HEAL + 3),
                NextAction("flash heal on party", ACTION_MEDIUM_HEAL + 2)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "party member medium health",
            {
                NextAction("renew on party", ACTION_LIGHT_HEAL + 2)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "party member to heal out of spell range",
            {
                NextAction("reach party member to heal", ACTION_CRITICAL_HEAL + 10)
            }
        )
    );
}

HolyHealPriestStrategy::HolyHealPriestStrategy(PlayerbotAI* botAI) : GenericPriestStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

std::vector<NextAction> HolyHealPriestStrategy::getDefaultActions()
{
    return { NextAction("shoot", ACTION_DEFAULT) };
}

void HolyHealPriestStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    GenericPriestStrategy::InitMultipliers(multipliers);

    multipliers.push_back(new HealerEmergencyMultiplier(botAI));
}

void HolyHealPriestStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericPriestStrategy::InitTriggers(triggers);

    // [heal_triage b29] EMERGENCY (one member below 35 %): the single-target save comes before the group heals and
    // Divine Hymn.
    triggers.push_back(
        new TriggerNode(
            "party member emergency health",
            {
                NextAction("guardian spirit on party", ACTION_CRITICAL_HEAL + 9),
                NextAction("power word: shield on party", ACTION_CRITICAL_HEAL + 8.7f),
                NextAction("flash heal on party", ACTION_CRITICAL_HEAL + 8.5f)
            }
        )
    );

    // Healer priority: a dying member (critical band), then the tank picking mobs up, then the rest.
    // A member back from a battle res comes first until safe, then gets his buffs back at once.
    triggers.push_back(
        new TriggerNode(
            "battle resurrected member low health",
            {
                NextAction("renew on battle resurrected", ACTION_CRITICAL_HEAL + 8.5f),
                NextAction("flash heal on battle resurrected", ACTION_CRITICAL_HEAL + 8)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "battle resurrected member",
            {
                NextAction("power word: fortitude on battle resurrected", ACTION_MEDIUM_HEAL + 9.5f),
                NextAction("divine spirit on battle resurrected", ACTION_MEDIUM_HEAL + 9.4f),
                NextAction("shadow protection on battle resurrected", ACTION_MEDIUM_HEAL + 9.3f)
            }
        )
    );
    // Free instant Flash Heal (Surge of Light) on whoever needs it; Binding Heal when the priest is hurt too.
    triggers.push_back(
        new TriggerNode(
            "surge of light",
            {
                NextAction("flash heal on party", ACTION_MEDIUM_HEAL + 6.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "binding heal",
            {
                NextAction("binding heal", ACTION_MEDIUM_HEAL + 5)
            }
        )
    );
    // The peel: Renew on the tank before the new mobs' swings, top of the losing-fast band; Flash Heal only
    // once he is actually down, below a victim who is losing health fast.
    triggers.push_back(
        new TriggerNode(
            "tank peel",
            {
                NextAction("renew on peeling tank", ACTION_MEDIUM_HEAL + 8),
                NextAction("flash heal on peeling tank", ACTION_MEDIUM_HEAL + 4.5f)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "group heal setting",
            {
                NextAction("prayer of mending on party", ACTION_MEDIUM_HEAL + 9),
                NextAction("circle of healing on party", ACTION_MEDIUM_HEAL + 8)
            }
        )
    );
    // [heal_triage] Divine Hymn is the cooldown for 3+ members below 40 % (it went at 65 % before, so it was gone
    // when the group really dropped); then the party heal.
    triggers.push_back(
        new TriggerNode(
            "group critical damage",
            {
                NextAction("divine hymn", ACTION_CRITICAL_HEAL + 8),
                NextAction("circle of healing on party", ACTION_CRITICAL_HEAL + 7.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "medium group heal setting",
            {
                NextAction("prayer of mending on party", ACTION_CRITICAL_HEAL + 6),
                NextAction("circle of healing on party", ACTION_CRITICAL_HEAL + 5),
                NextAction("prayer of healing on party", ACTION_CRITICAL_HEAL + 4)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "tank damage spike",
            {
                NextAction("guardian spirit on spiking tank", ACTION_EMERGENCY + 2)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "party member critical health",
            {
                NextAction("guardian spirit on party", ACTION_CRITICAL_HEAL + 6),
                NextAction("power word: shield on party", ACTION_CRITICAL_HEAL + 5),
                NextAction("prayer of mending on party", ACTION_CRITICAL_HEAL + 3),
                NextAction("greater heal on party", ACTION_MEDIUM_HEAL + 2),
                NextAction("flash heal on party", ACTION_CRITICAL_HEAL + 1),
            }
        )
    );
    // Above the low band and the tank's Renew upkeep, below critical health
    triggers.push_back(
        new TriggerNode(
            "party member losing health fast",
            {
                NextAction("power word: shield on party", ACTION_MEDIUM_HEAL + 7),
                NextAction("flash heal on party", ACTION_MEDIUM_HEAL + 6)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "party member low health",
            {
                NextAction("circle of healing on party", ACTION_MEDIUM_HEAL + 4),
                NextAction("prayer of mending on party", ACTION_MEDIUM_HEAL + 3),
                NextAction("greater heal on party", ACTION_MEDIUM_HEAL + 2),
                NextAction("flash heal on party", ACTION_MEDIUM_HEAL + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "party member medium health",
            {
                // [b31_deep] Flash Heal (builds Serendipity) above Renew: 66-78 % of raw healing was Renew
                NextAction("prayer of mending on party", ACTION_LIGHT_HEAL + 7),
                NextAction("flash heal on party", ACTION_LIGHT_HEAL + 6),
                NextAction("renew on party", ACTION_LIGHT_HEAL + 5),
                NextAction("circle of healing on party", ACTION_LIGHT_HEAL + 4)
            }
        )
    );
    // [healer_audit_b30] Prayer of Mending on the tank on its cooldown, above the Renew upkeep, as disc does:
    // build 30 holy at 80 cast it 0.9 times a minute (disc 4.8) and Renew 5.1 times, so the strongest holy heal per
    // mana went unused. Learned at 68; below that the action is not possible and the node falls through.
    triggers.push_back(
        new TriggerNode(
            "prayer of mending on main tank",
            {
                NextAction("prayer of mending on main tank", ACTION_LIGHT_HEAL + 9.2f)
            }
        )
    );
    // [healer_audit_b30] GROUP DAMAGE (3+ members below 70 %): Circle of Healing (instant, 6 s cooldown) then Prayer
    // of Healing, above the low band; build 30 holy at 80 cast Circle of Healing 0.3 times a minute.
    triggers.push_back(
        new TriggerNode(
            "group damage",
            {
                NextAction("circle of healing on party", ACTION_MEDIUM_HEAL + 9.5f),
                NextAction("prayer of healing on party", ACTION_MEDIUM_HEAL + 4.8f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "renew on main tank",
            {
                NextAction("renew on main tank", ACTION_LIGHT_HEAL + 9.1f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "party member almost full health",
            {
                // [guide_b31_dead] Prayer of Mending above Renew (GUIDE_AUDIT_HEALERS 2: the bot was a "Renew bot")
                NextAction("prayer of mending on party", ACTION_LIGHT_HEAL + 2),
                NextAction("renew on party", ACTION_LIGHT_HEAL + 1),
            }
        )
    );

    // [guide_b31_dead] GUIDE_AUDIT_HEALERS 2: Circle of Healing on cooldown at 2+ members <= 90 %, just under the
    // tank's Prayer of Mending (9.2) and above his Renew (9.1); Greater Heal on the tank under 3 Serendipity stacks.
    triggers.push_back(new TriggerNode("circle of healing two injured",
                                       { NextAction("circle of healing on party", ACTION_LIGHT_HEAL + 9.15f) }));
    triggers.push_back(new TriggerNode("serendipity greater heal",
                                       { NextAction("greater heal on main tank", ACTION_MEDIUM_HEAL + 5) }));
    triggers.push_back(
        new TriggerNode(
            "party member to heal out of spell range",
            {
                NextAction("reach party member to heal", ACTION_CRITICAL_HEAL + 10)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus guardian spirit",
        { NextAction("guardian spirit on party", ACTION_MEDIUM_HEAL + 9.6f) }));
    triggers.push_back(new TriggerNode("set bonus ascendant prayer",
        { NextAction("prayer of healing on party", ACTION_MEDIUM_HEAL + 9.5f) }));
}
