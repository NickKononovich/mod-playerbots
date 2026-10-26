/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DiscPriestStrategy.h"
#include "HealthTriggers.h"
#include "Playerbots.h"

DiscPriestStrategy::DiscPriestStrategy(PlayerbotAI* botAI) : GenericPriestStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

std::vector<NextAction> DiscPriestStrategy::getDefaultActions()
{
    return {
        NextAction("shoot", ACTION_DEFAULT)
    };
}

void DiscPriestStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    GenericPriestStrategy::InitMultipliers(multipliers);

    multipliers.push_back(new HealerEmergencyMultiplier(botAI));
}

void DiscPriestStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericPriestStrategy::InitTriggers(triggers);

    // [heal_triage b29] EMERGENCY (one member below 35 %): the single-target save comes before the group heals and
    // Divine Hymn.
    triggers.push_back(
        new TriggerNode(
            "party member emergency health",
            {
                NextAction("pain suppression on party", ACTION_CRITICAL_HEAL + 9),
                NextAction("power word: shield on party", ACTION_CRITICAL_HEAL + 8.7f),
                NextAction("penance on party", ACTION_CRITICAL_HEAL + 8.6f),
                NextAction("flash heal on party", ACTION_CRITICAL_HEAL + 8.4f)
            }
        )
    );

    // Healer priority: a dying member (critical band), then the tank picking mobs up, then the rest.
    // A member back from a battle res comes first until safe, then gets his buffs back at once.
    triggers.push_back(
        new TriggerNode(
            "battle resurrected member low health",
            {
                NextAction("power word: shield on battle resurrected", ACTION_CRITICAL_HEAL + 8.5f),
                NextAction("penance on battle resurrected", ACTION_CRITICAL_HEAL + 8),
                NextAction("flash heal on battle resurrected", ACTION_CRITICAL_HEAL + 7.5f)
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
    triggers.push_back(
        new TriggerNode(
            "binding heal",
            {
                NextAction("binding heal", ACTION_MEDIUM_HEAL + 1)
            }
        )
    );
    // The peel: shield the tank before the new mobs' swings, top of the losing-fast band; the instant shield
    // costs one GCD, so a victim still being hit is healed right after.
    triggers.push_back(
        new TriggerNode(
            "tank peel",
            {
                NextAction("power word: shield on peeling tank", ACTION_MEDIUM_HEAL + 8),
                NextAction("flash heal on peeling tank", ACTION_MEDIUM_HEAL + 4.5f)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "group heal setting",
            {
                NextAction("prayer of mending on party", ACTION_MEDIUM_HEAL + 8),
                NextAction("power word: shield on not full", ACTION_MEDIUM_HEAL + 7)
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
                NextAction("prayer of healing on party", ACTION_CRITICAL_HEAL + 7.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "medium group heal setting",
            {
                NextAction("prayer of mending on party", ACTION_CRITICAL_HEAL + 6),
                NextAction("power word: shield on not full", ACTION_CRITICAL_HEAL + 5),
                NextAction("prayer of healing on party", ACTION_CRITICAL_HEAL + 4)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member critical health",
            {
                NextAction("power word: shield on party", ACTION_CRITICAL_HEAL + 5),
                NextAction("penance on party", ACTION_CRITICAL_HEAL + 4),
                NextAction("prayer of mending on party", ACTION_CRITICAL_HEAL + 3),
                NextAction("flash heal on party", ACTION_CRITICAL_HEAL + 2)
            }
        )
    );

    // Above the low band and the tank's shield / Prayer of Mending upkeep, below critical health
    triggers.push_back(
        new TriggerNode(
            "party member losing health fast",
            {
                NextAction("power word: shield on party", ACTION_MEDIUM_HEAL + 7),
                NextAction("penance on party", ACTION_MEDIUM_HEAL + 6),
                NextAction("flash heal on party", ACTION_MEDIUM_HEAL + 5)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "party member low health",
            {
                NextAction("power word: shield on party", ACTION_MEDIUM_HEAL + 4),
                NextAction("prayer of mending on party", ACTION_MEDIUM_HEAL + 3),
                NextAction("penance on party", ACTION_MEDIUM_HEAL + 2),
                NextAction("flash heal on party", ACTION_MEDIUM_HEAL + 0)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member medium health",
            {
                NextAction("power word: shield on party", ACTION_LIGHT_HEAL + 9),
                NextAction("prayer of mending on party", ACTION_LIGHT_HEAL + 7),
                NextAction("penance on party", ACTION_LIGHT_HEAL + 6),
                NextAction("flash heal on party", ACTION_LIGHT_HEAL + 5)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member almost full health",
            {
                NextAction("power word: shield on party", ACTION_LIGHT_HEAL + 3),
                NextAction("prayer of mending on party", ACTION_LIGHT_HEAL + 2),
                NextAction("renew on party", ACTION_LIGHT_HEAL + 1)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "power word: shield on main tank",
            {
                NextAction("power word: shield on main tank", ACTION_LIGHT_HEAL + 9.2f)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "prayer of mending on main tank",
            {
                NextAction("prayer of mending on main tank", ACTION_LIGHT_HEAL + 9.1f)
            }
        )
    );

    // [guide_b31_dead] GUIDE_AUDIT_HEALERS 1: Penance on cooldown on the tank (below 95 %), under the shield / PoM
    // upkeep; the hurt bands keep Penance on the triaged member. Borrowed Time (after a shield, 25 % haste) goes into
    // Flash Heal on the triaged member below 90 %, at medium-heal priority in the low band. [b31_deep] the low band
    // took Greater Heal: a ~2.4 s cast even under Borrowed Time, slower than Penance / Flash Heal for a falling member.
    triggers.push_back(
        new TriggerNode("penance on main tank", { NextAction("penance on main tank", ACTION_LIGHT_HEAL + 9.0f) }));
    triggers.push_back(
        new TriggerNode("borrowed time low health",
                        { NextAction("flash heal on party", ACTION_MEDIUM_HEAL + 4.5f) }));
    triggers.push_back(
        new TriggerNode("borrowed time", { NextAction("flash heal on party", ACTION_LIGHT_HEAL + 8.0f) }));

    triggers.push_back(
        new TriggerNode(
            "party member to heal out of spell range",
            {
                NextAction("reach party member to heal", ACTION_CRITICAL_HEAL + 10)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "critical health", {
                NextAction("pain suppression", ACTION_EMERGENCY + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "damage spike", {
                NextAction("pain suppression", ACTION_EMERGENCY + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "protect party member",
            {
                NextAction("pain suppression on party", ACTION_EMERGENCY)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "tank damage spike",
            {
                NextAction("pain suppression on spiking tank", ACTION_EMERGENCY + 2)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus guardian spirit",
        { NextAction("pain suppression on party", ACTION_MEDIUM_HEAL + 9.6f) }));
    triggers.push_back(new TriggerNode("set bonus ascendant prayer",
        { NextAction("prayer of healing on party", ACTION_MEDIUM_HEAL + 9.5f) }));
    triggers.push_back(new TriggerNode("set bonus surge of light",
        { NextAction("flash heal on party", ACTION_MEDIUM_HEAL + 6.5f) }));
}
