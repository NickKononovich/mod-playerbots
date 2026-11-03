/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FireMageStrategy.h"
#include "Playerbots.h"
#include "Strategy.h"

FireMageStrategy::FireMageStrategy(PlayerbotAI* botAI) : GenericMageStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

// ===== Default Actions =====
std::vector<NextAction> FireMageStrategy::getDefaultActions()
{
    return {
        NextAction("fireball", 5.3f),
        NextAction("frostbolt", 5.2f),   // fire immune target
        NextAction("fire blast", 5.1f),  // cast during movement
        NextAction("shoot", 5.0f)
    };
}

// ===== Trigger Initialization =====
void FireMageStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericMageStrategy::InitTriggers(triggers);

    // Debuff Triggers
    triggers.push_back(
        new TriggerNode(
            "improved scorch",
            {
                NextAction("scorch", 19.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "living bomb",
            {
                NextAction("living bomb", 18.5f)
            }
        )
    );

    // [rot251] wowsims fire APL, the fight's end: below Living Bomb (18.5, it needs 12 s of target life), above
    // Fireball
    triggers.push_back(new TriggerNode("fire blast fight end", { NextAction("fire blast", 18.4f) }));
    triggers.push_back(new TriggerNode("scorch fight end", { NextAction("scorch", 18.3f) }));

    // Proc Trigger
    triggers.push_back(
        new TriggerNode(
            "hot streak",
            {
                NextAction("pyroblast", 25.0f)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "enemy too close for spell and solo or being attacked",
            {
                NextAction("dragon's breath", ACTION_INTERRUPT + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "enemy is close and solo or being attacked",
            {
                NextAction("blast wave", ACTION_INTERRUPT)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus fire blast", { NextAction("fire blast", 18.2f) }));
    triggers.push_back(new TriggerNode("set bonus arcane surge", { NextAction("fireball", 18.0f) }));
}

// Combat strategy to run to melee for Dragon's Breath and Blast Wave
// Disabled by default for the Fire/Frostfire spec
// To enable, type "co +firestarter"
// To disable, type "co -firestarter"
FirestarterStrategy::FirestarterStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI) {}

void FirestarterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode(
            "blast wave off cd and medium aoe",
            {
                NextAction("reach melee", 25.5f)
            }
        )
    );
}
