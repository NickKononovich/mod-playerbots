/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DestructionWarlockStrategy.h"
#include "Playerbots.h"

DestructionWarlockStrategy::DestructionWarlockStrategy(PlayerbotAI* botAI) : GenericWarlockStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

// ===== Default Actions =====
std::vector<NextAction> DestructionWarlockStrategy::getDefaultActions()
{
    return {
       NextAction("immolate", 5.9f),
       NextAction("conflagrate", 5.8f),
       NextAction("chaos bolt", 5.7f),
       NextAction("incinerate", 5.6f),
       NextAction("shadow bolt", 5.2f),
       NextAction("shoot", 5.0f)
    };
}

// ===== Trigger Initialization ===
void DestructionWarlockStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericWarlockStrategy::InitTriggers(triggers);

    // Main DoT triggers for high uptime + high priority cooldowns
    triggers.push_back(
        new TriggerNode(
            "immolate",
            {
                NextAction("immolate", 20.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "conflagrate",
            {
                NextAction("conflagrate", 20.1f)  // [rot24] wowsims warlock__destro: Conflagrate before Immolate
            }
        )
    );
    // [b31_deep] wowsims destro APL: Conflagrate, then Curse of Doom while the fight lasts > 60 s (only in the
    // coordinator's damage-curse slot; the Curse of the Elements owner and a solo Destruction warlock keep CoE).
    triggers.push_back(
        new TriggerNode(
            "damage curse of doom",
            {
                NextAction("curse of doom", 19.4f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "chaos bolt",
            {
                NextAction("chaos bolt", 19.0f)
            }
        )
    );

    // [rot410] no Corruption at all: the wowsims destro APL (warlock__destro) never casts it - b229 rfc-h the
    // destruction warlock still cast it below Incinerate (the leveling filler triggers, an instant while it moved or
    // Incinerate waited); Shadow Bolt stays the filler before Incinerate is learned

    // Shadowburn as execute if target is low HP. [b35_lock] Below Incinerate (5.6): standing still, Incinerate deals
    // more per second (bot logs, Thalnos: Shadowburn 3157 per 1.33 s GCD = 2374 dps, Incinerate 5441 per ~1.77 s =
    // 3074 dps; at 18.0 it cost ~0.4 %). It still wins while moving (no cast-time spell is possible then) and when
    // Incinerate would not land before the target dies (CastTimeMultiplier cuts Incinerate to x0.1).
    triggers.push_back(
        new TriggerNode(
            "target critical health",
            {
                NextAction("shadowburn", 5.55f)
            }
        )
    );

    // [b35_lock] Inferno on a boss whose fight ends within 61 s (wowsims destro "remaining < 61 s"; sim +6-10 % on
    // the short 5-man boss fights). After Conflagrate / Curse of Doom, before Chaos Bolt.
    triggers.push_back(
        new TriggerNode(
            "inferno boss",
            {
                NextAction("inferno", 19.3f)
            }
        )
    );

    // Life Tap glyph buff, and Life Tap as filler ([b31_deep] refreshed < 3 s before it drops, wowsims destro)
    triggers.push_back(
        new TriggerNode(
            "life tap glyph buff early",
            {
                NextAction("life tap", 29.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "life tap",
            {
                NextAction("life tap", 5.1f)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "enemy too close for spell",
            {
                NextAction("flee", 39.0f)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus backlash", { NextAction("incinerate", 18.8f) }));
    triggers.push_back(new TriggerNode("set bonus ascendant pact", { NextAction("life tap", 5.65f) }));
}
