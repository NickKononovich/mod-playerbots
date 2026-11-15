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
    std::vector<NextAction> actions;
    // [rot640] `spell` is the name the action casts; nullptr means the entry is never gated. The gate is
    // GenericHunterStrategy::KnowsSpell, shared with Marksmanship and Survival
    auto const add = [&](char const* action, float relevance, char const* spell)
    {
        if (!spell || KnowsSpell(spell))
            actions.push_back(NextAction(action, relevance));
    };

    add("kill command", 5.7f, "kill command");
    add("kill shot", 5.6f, "kill shot");
    add("serpent sting", 5.5f, "serpent sting");
    add("aimed shot", 5.4f, "aimed shot");
    // Beast Mastery has no Chimera Shot: Multi-Shot is a single-target shot in its rotation too.
    // [b31_deep] Multi before Arcane (wowsims bm.apl hides Arcane; Warcraft Tavern BM puts Arcane last)
    add("multi-shot", 5.3f, "multi-shot");
    // [b35_bal] Arcane below Steady (wowsims bm.apl never casts it over Steady; no proof for the deviation, and
    // it costs more mana per GCD): Steady takes every standing GCD, Arcane is left for the shot on the move.
    add("steady shot", 5.2f, "steady shot");
    add("arcane shot", 5.15f, "arcane shot");
    // [rot640] Auto Shot is the hunter's white damage and every hunter has it from level 1: never gated, so no
    // gate of mine can ever cost a hunter its shots
    add("auto shot", 5.1f, nullptr);

    return actions;
}

// ===== Trigger Initialization ===
void BeastMasteryHunterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericHunterStrategy::InitTriggers(triggers);
    // [lead105] [rot7] below every shot and Auto Shot (wowsims never casts it): only when no shot can go
    triggers.push_back(new TriggerNode("enemy within melee", {NextAction("mongoose bite", ACTION_DEFAULT + 0.08f)}));
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
