/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HealPaladinStrategy.h"
#include "GenericSpellActions.h"
#include "HealthTriggers.h"
#include "Playerbots.h"
#include "Strategy.h"

namespace
{
    // Crowd control keeps a mob off the healer or the group; it is not damage.
    bool IsCrowdControl(std::string const& spell)
    {
        return spell == "hammer of justice" || spell == "repentance" || spell == "turn undead" ||
               spell == "turn evil";
    }
}

// Stockade 2026-10-24, Bazil Thredd: the tank went 87 -> 25 % in 13 s while the holy paladin turned to a mob
// ("set facing", 37) and cast Judgement of Light on it, which pulled the mob onto the healer.
float HolyHealsFirstMultiplier::GetValue(Action* action)
{
    // Alone the paladin has to fight back between its own heals.
    if (!action || !bot->IsInCombat() || !bot->GetGroup())
        return 1.0f;

    bool const facing = action->getName() == "set facing";
    if (!facing)
    {
        CastSpellAction* spellAction = dynamic_cast<CastSpellAction*>(action);
        if (!spellAction || dynamic_cast<CastHealingSpellAction*>(action) || IsCrowdControl(spellAction->getSpell()))
            return 1.0f;

        Unit* target = action->GetTarget();
        if (!target || !bot->IsHostileTo(target))
            return 1.0f;
    }

    Unit* healTarget = AI_VALUE(Unit*, "party member to heal");
    if (!healTarget || !healTarget->IsAlive() || healTarget->GetHealthPct() >= HEALS_FIRST_HEALTH_PCT)
        return 1.0f;

    return 0.0f;
}

HealPaladinStrategy::HealPaladinStrategy(PlayerbotAI* botAI) : GenericPaladinStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

void HealPaladinStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    GenericPaladinStrategy::InitMultipliers(multipliers);

    multipliers.push_back(new HolyHealsFirstMultiplier(botAI));
    // [heal_triage b29] cures on other members wait while one is below 35 %
    multipliers.push_back(new HealerEmergencyMultiplier(botAI));
}

// [b31_pal] No default action: "judgement of light" here took every free GCD (L60 4.3/min, the most frequent cast;
// L72 an-n 1.9/min) and walked the healer to its target. Judgement of Light now keeps Judgements of the Pure up
// ("judgements of the pure" below); a holy paladin with nothing to heal waits, and "healer dps" attacks.
std::vector<NextAction> HealPaladinStrategy::getDefaultActions()
{
    return {};
}

void HealPaladinStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericPaladinStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode(
            "seal",
            {
                NextAction("seal of wisdom", ACTION_HIGH),
            }
        )
    );
    // [b31_pal] Divine Illumination while healing below 85 % mana (or below 50 % anyway), not only at 40 %
    triggers.push_back(
        new TriggerNode(
            "holy divine illumination",
            {
                NextAction("divine illumination", ACTION_HIGH + 2)
            }
        )
    );
    // [b31_pal] One Judgement a minute for the 15 % haste of Judgements of the Pure: above the almost-full top-up
    // (LIGHT_HEAL + 3), below the medium heals (LIGHT_HEAL + 8 and up); HolyHealsFirstMultiplier still holds it
    // while a member is low.
    triggers.push_back(
        new TriggerNode(
            "judgements of the pure",
            {
                NextAction("judgement of light", ACTION_LIGHT_HEAL + 3.5f)
            }
        )
    );
    // [rot_b31_pdd] Above the medium / almost-full heals (up to LIGHT_HEAL + 9), below the low-health ones
    // (MEDIUM_HEAL + 5 and up); CastDivinePleaAction::isUseful holds it while a member is below MediumHealth.
    triggers.push_back(
        new TriggerNode(
            "holy divine plea",
            {
                NextAction("divine plea", ACTION_MEDIUM_HEAL + 4)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "party member to heal out of spell range",
            {
                NextAction("reach party member to heal", ACTION_EMERGENCY + 3)
            }
        )
    );
    // Half the damage while the healer is focused (Divine Shield stays for critical health).
    triggers.push_back(
        new TriggerNode(
            "damage spike",
            {
                NextAction("divine protection", ACTION_EMERGENCY)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "medium group heal setting",
            {
                NextAction("divine sacrifice", ACTION_CRITICAL_HEAL + 5),
                NextAction("avenging wrath", ACTION_HIGH + 4),
            }
        )
    );
    // [heal_triage b29] EMERGENCY (one member below 35 %): the instant Holy Shock, then Flash of Light; Lay on Hands
    // stays on "party member critical health" (ACTION_EMERGENCY + 2).
    triggers.push_back(
        new TriggerNode(
            "party member emergency health",
            {
                NextAction("holy shock on party", ACTION_CRITICAL_HEAL + 9),
                NextAction("flash of light on party", ACTION_CRITICAL_HEAL + 8.5f)
            }
        )
    );
    // [b31_deep] Avenging Wrath (+20 % healing, off the GCD: Spell.dbc StartRecoveryTime 0) when a member drops below
    // 35 % on a boss; trash keeps it for the boss. The group variant is "medium group heal setting" above.
    triggers.push_back(new TriggerNode("holy avenging wrath",
        { NextAction("avenging wrath", ACTION_CRITICAL_HEAL + 9.5f) }));
    triggers.push_back(
        new TriggerNode(
            "party member critical health",
            {
                NextAction("holy shock on party", ACTION_CRITICAL_HEAL + 6),
                // [b31_deep] no Divine Sacrifice here: a GCD while a heal is needed, and 30 % of the group's damage
                // on the healer; it stays on "medium group heal setting"
                NextAction("divine favor", ACTION_CRITICAL_HEAL + 4.5f),
                NextAction("holy light on party", ACTION_CRITICAL_HEAL + 4)
            }
        )
    );
    // Above the low band, below critical health
    triggers.push_back(
        new TriggerNode(
            "party member losing health fast",
            {
                NextAction("holy shock on party", ACTION_MEDIUM_HEAL + 8),
                NextAction("flash of light on party", ACTION_MEDIUM_HEAL + 7)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "party member low health",
            {
                NextAction("holy shock on party", ACTION_MEDIUM_HEAL + 6),
                NextAction("holy light on party", ACTION_MEDIUM_HEAL + 5)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "party member medium health",
            {
                NextAction("holy light on party", ACTION_LIGHT_HEAL + 9),
                NextAction("flash of light on party", ACTION_LIGHT_HEAL + 8)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
        "party member almost full health",
        {
            NextAction("flash of light on party", ACTION_LIGHT_HEAL + 3)
        }
    )
);
    // [b31_deep] Holy Light is the main heal (it keeps Light's Grace, 31834: the next one 0.5 s faster): on a boss the
    // tank's top-up is Holy Light, not Flash of Light (build 30 L80 boss: HL 0.75/min, FoL 3.72/min). Above the
    // almost-full FoL, below Judgements of the Pure (LIGHT_HEAL + 3.5).
    triggers.push_back(new TriggerNode("holy light boss tank",
        { NextAction("holy light on party", ACTION_LIGHT_HEAL + 3.2f) }));
    // [b31_deep] Infusion of Light (53672 / 54149, after a Holy Shock crit): the next Flash of Light is instant
    // (rank 2). Above the medium heals (LIGHT_HEAL + 9), below Divine Plea and the low-health heals.
    triggers.push_back(new TriggerNode("infusion of light",
        { NextAction("flash of light on party", ACTION_LIGHT_HEAL + 9.5f) }));

    triggers.push_back(
        new TriggerNode(
        "beacon of light on main tank",
        {
            NextAction("beacon of light on main tank", ACTION_CRITICAL_HEAL + 7)
        }
    )
);

    triggers.push_back(
        new TriggerNode(
        "sacred shield on main tank",
        {
            NextAction("sacred shield on main tank", ACTION_CRITICAL_HEAL + 6)
        }
    )
);

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus divine favor",
        { NextAction("holy light on party", ACTION_CRITICAL_HEAL + 5) }));
    triggers.push_back(new TriggerNode("set bonus titan's light",
        { NextAction("holy light on party", ACTION_MEDIUM_HEAL + 6.5f) }));
    triggers.push_back(new TriggerNode("set bonus shocking light",
        { NextAction("holy light on party", ACTION_MEDIUM_HEAL + 5.5f) }));
    triggers.push_back(new TriggerNode("set bonus judgement divine favor",
        { NextAction("judgement of light", ACTION_LIGHT_HEAL + 3.5f) }));
}
