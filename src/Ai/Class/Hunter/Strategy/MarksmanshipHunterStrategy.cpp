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
    return {
        NextAction("kill command", 5.8f),
        // [b31_deep] off the GCD, on cooldown on a target it cannot silence (wowsims mm.apl uses it on cooldown);
        // [rot135] a led hunter on cooldown on every target
        NextAction("silencing shot on cooldown", 5.75f),
        NextAction("kill shot", 5.7f),
        NextAction("serpent sting", 5.6f),
        NextAction("chimera shot", 5.5f),
        NextAction("aimed shot", 5.4f),
        // [b31_deep] Multi-Shot fills the shared Aimed/Multi 10 s cooldown (category 85) while Aimed Shot is not
        // castable (wowsims mm.apl: Aimed > Multi > Steady); with Aimed known the shared cooldown keeps it second
        NextAction("multi-shot", 5.35f),
        // [lead105] wowsims 3.3.5 MM APL: Arcane Shot is not cast (hidden); Steady Shot is the filler
        // Silencing Shot is held for enemy casts (the "silencing shot" trigger and the group interrupt
        // rotation); spent as a filler it was on cooldown whenever a caster started a cast.
        NextAction("steady shot", 5.2f),
        NextAction("auto shot", 5.1f)
    };
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
