/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DemonologyWarlockStrategy.h"
#include "Playerbots.h"

DemonologyWarlockStrategy::DemonologyWarlockStrategy(PlayerbotAI* botAI) : GenericWarlockStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

// ===== Default Actions =====
std::vector<NextAction> DemonologyWarlockStrategy::getDefaultActions()
{
    // [b31_deep] No default Immolate: it bypassed the 12 s lifetime of the "demo immolate" trigger.
    return {
       NextAction("corruption", 5.5f),
       NextAction("shadow bolt", 5.3f),
       NextAction("incinerate", 5.2f),
       NextAction("shoot", 5.0f) };
}

// ===== Trigger Initialization ===
void DemonologyWarlockStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericWarlockStrategy::InitTriggers(triggers);

    // High priority cooldowns
    triggers.push_back(
        new TriggerNode(
            "metamorphosis",
            {
                NextAction("metamorphosis", 28.5f)
            }
        )
    );
    // [rot481] Rawr 2.3.23 (Demonology rotation: Immolation Aura) and the wowsims demo APL (castSpell 50589 right
    // after the cooldowns): Immolation Aura in Metamorphosis on the target, a single one too, where its area reaches
    triggers.push_back(new TriggerNode("immolation aura ready", { NextAction("immolation aura", 28.2f) }));
    triggers.push_back(
        new TriggerNode(
            "demonic empowerment",
            {
                NextAction("demonic empowerment", 28.0f)
            }
        )
    );

    // [b31_deep] wowsims demo APL: CoD (fight > 60 s, else CoA), then the main target's Corruption (maxDots 1) and
    // Immolate (fight >= 12 s) above the Decimation / Molten Core procs. The multi-dot on other attackers ranked
    // above all of them (C12: Corruption 5.8/min on L80 bosses, 39 Corruption + 20 Immolate in 209 s on the
    // Maulgar council) and now ranks below the procs, in packs only.
    triggers.push_back(
        new TriggerNode(
            "damage curse of doom",
            {
                NextAction("curse of doom", 19.6f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "corruption",
            {
                NextAction("corruption", 19.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "demo immolate",
            {
                NextAction("immolate", 19.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "demo corruption on attacker",
            {
                NextAction("corruption on attacker", 16.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "demo immolate on attacker",
            {
                NextAction("immolate on attacker", 15.8f)
            }
        )
    );

    // Procs
    triggers.push_back(
        new TriggerNode(
            "decimation",
            {
                NextAction("soul fire", 17.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "molten core",
            {
                NextAction("incinerate", 16.5f)
            }
        )
    );

    // Life Tap glyph buff, and Life Tap as filler ([b31_deep] refreshed < 3 s before it drops, wowsims demo)
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
            "meta melee flee check",
            {
                NextAction("flee", 39.0f)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus backlash", { NextAction("shadow bolt", 16.8f) }));
    triggers.push_back(new TriggerNode("set bonus ascendant pact", { NextAction("life tap", 5.65f) }));
}

// Combat strategy to run to melee for Immolation Aura
// Enabled by default for the Demonology spec
// To enable, type "co +meta melee"
// To disable, type "co -meta melee"
MetaMeleeAoeStrategy::MetaMeleeAoeStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI) {}

void MetaMeleeAoeStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // [rot481] in Metamorphosis the warlock walks to the target first, so Immolation Aura's area reaches it (the
    // wowsims demo APL casts it on the target the whole form; the sim stands in range)
    triggers.push_back(new TriggerNode("metamorphosis active", { NextAction("reach melee", 25.5f) }));
    triggers.push_back(
        new TriggerNode(
            "immolation aura active",
            {
                NextAction("reach melee", 25.5f),
                NextAction("demon charge", 25.0f)
            }
        )
    );
}
