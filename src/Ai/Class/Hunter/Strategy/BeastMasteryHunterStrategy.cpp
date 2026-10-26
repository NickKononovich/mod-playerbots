/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BeastMasteryHunterStrategy.h"
#include "Playerbots.h"

BeastMasteryHunterStrategy::BeastMasteryHunterStrategy(PlayerbotAI* botAI) : GenericHunterStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

// ===== Default Actions =====
std::vector<NextAction> BeastMasteryHunterStrategy::getDefaultActions()
{
    return {
        NextAction("kill command", 5.7f),
        NextAction("kill shot", 5.6f),
        NextAction("serpent sting", 5.5f),
        NextAction("aimed shot", 5.4f),
        // Beast Mastery has no Chimera Shot: Multi-Shot is a single-target shot in its rotation too.
        // [b31_deep] Multi before Arcane (wowsims bm.apl hides Arcane; Warcraft Tavern BM puts Arcane last)
        NextAction("multi-shot", 5.3f),
        // [b35_bal] Arcane below Steady (wowsims bm.apl never casts it over Steady; no proof for the deviation, and
        // it costs more mana per GCD): Steady takes every standing GCD, Arcane is left for the shot on the move.
        NextAction("steady shot", 5.2f),
        NextAction("arcane shot", 5.15f),
        NextAction("auto shot", 5.1f)
    };
}

// ===== Trigger Initialization ===
void BeastMasteryHunterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericHunterStrategy::InitTriggers(triggers);
    triggers.push_back(
        new TriggerNode(
            "bestial wrath",
            {
                NextAction("bestial wrath", 29.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "intimidation",
            {
                NextAction("intimidation", 40.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "kill command",
            {
                NextAction("kill command", 18.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "target critical health",
            {
                NextAction("kill shot", 18.0f)
            }
        )
    );
    // [b31_deep] "low mana" -> "viper sting" removed: one sting per hunter, so it replaced Serpent Sting; only MM
    // (Chimera Shot on a Viper Sting returns mana) keeps it.
    // [b31_deep] the pet is a large share of BM damage: revive it in combat when it died on a boss with 30+ s left
    triggers.push_back(new TriggerNode("hunters pet dead on boss", { NextAction("revive pet", 23.0f) }));
    triggers.push_back(
        new TriggerNode(
            "no stings",
            {
                NextAction("serpent sting", 17.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "serpent sting on attacker",
            {
                NextAction("serpent sting on attacker", 16.5f)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus ascendant execution", { NextAction("kill shot", 18.4f) }));
    triggers.push_back(new TriggerNode("set bonus lock and load", { NextAction("arcane shot", 17.96f) }));
    triggers.push_back(new TriggerNode("set bonus titan's focus", { NextAction("aimed shot", 17.85f) }));
    triggers.push_back(new TriggerNode("set bonus improved steady shot",
        { NextAction("aimed shot", 17.8f),
          NextAction("arcane shot", 17.7f) }));
}
