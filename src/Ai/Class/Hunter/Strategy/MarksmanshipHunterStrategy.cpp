/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MarksmanshipHunterStrategy.h"
#include "Playerbots.h"

MarksmanshipHunterStrategy::MarksmanshipHunterStrategy(PlayerbotAI* botAI) : GenericHunterStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

// ===== Default Actions =====
std::vector<NextAction> MarksmanshipHunterStrategy::getDefaultActions()
{
    std::vector<NextAction> actions;
    // [rot640] `spell` is the name the action casts; nullptr means the entry is never gated. The gate itself is
    // GenericHunterStrategy::KnowsSpell, shared with Survival and Beast Mastery
    auto const add = [&](char const* action, float relevance, char const* spell)
    {
        if (!spell || KnowsSpell(spell))
            actions.push_back(NextAction(action, relevance));
    };

    add("kill command", 5.8f, "kill command");
    // [b31_deep] off the GCD, on cooldown on a target it cannot silence (wowsims mm.apl uses it on cooldown);
    // [rot135] a led hunter on cooldown on every target
    add("silencing shot on cooldown", 5.75f, "silencing shot");
    add("kill shot", 5.7f, "kill shot");
    add("serpent sting", 5.6f, "serpent sting");
    add("chimera shot", 5.5f, "chimera shot");
    add("aimed shot", 5.4f, "aimed shot");
    // [b31_deep] Multi-Shot fills the shared Aimed/Multi 10 s cooldown (category 85) while Aimed Shot is not
    // castable (wowsims mm.apl: Aimed > Multi > Steady); with Aimed known the shared cooldown keeps it second
    add("multi-shot", 5.35f, "multi-shot");
    // [lead105] wowsims 3.3.5 MM APL: Arcane Shot is not cast (hidden) once Steady Shot is the filler
    // Silencing Shot is held for enemy casts (the "silencing shot" trigger and the group interrupt
    // rotation); spent as a filler it was on cooldown whenever a caster started a cast.
    add("steady shot", 5.2f, "steady shot");
    // [dps230] below Steady Shot (learned at L50) Arcane Shot is the only shot between the cooldowns: L14-20
    // marksmen cast none (b426-b429: 0 a fight against the formula's 8.9 dps of it, 0.78 of the ideal); Steady
    // Shot above it keeps it out of the L50+ rotation (wowsims mm.apl) but for the shot on the move
    add("arcane shot", 5.15f, "arcane shot");
    // [rot640] Auto Shot is the hunter's white damage and every hunter has it from level 1: never gated, so no
    // gate of mine can ever cost a hunter its shots
    add("auto shot", 5.1f, nullptr);

    return actions;
}

// ===== Trigger Initialization ===
void MarksmanshipHunterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericHunterStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode(
            "silencing shot",
            {
                NextAction("silencing shot", 40.0f)
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
    // [rot95] no "low mana" -> Viper Sting: wowsims mm.apl never casts it (its mana is Aspect of the Viper below 10 %).
    // One sting per hunter: b211 rfc-h 5H Rthunad Jergosh cast it twice, it replaced Serpent Sting (4988 vs the sim's
    // 10718 damage) and Chimera Shot's serpent burst (4266 vs 14188) - 67 % of the sim in that fight
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
    triggers.push_back(new TriggerNode("set bonus titan's focus",
        { NextAction("chimera shot", 17.95f),
          NextAction("aimed shot", 17.85f) }));
    triggers.push_back(new TriggerNode("set bonus improved steady shot",
        { NextAction("chimera shot", 17.9f),
          NextAction("aimed shot", 17.8f) }));
}
