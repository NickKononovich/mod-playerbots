/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AfflictionWarlockStrategy.h"
#include "Playerbots.h"

AfflictionWarlockStrategy::AfflictionWarlockStrategy(PlayerbotAI* botAI) : GenericWarlockStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

// ===== Default Actions =====
std::vector<NextAction> AfflictionWarlockStrategy::getDefaultActions()
{
    return {
       NextAction("haunt", 5.5f),
       NextAction("unstable affliction", 5.4f),
       NextAction("corruption", 5.3f),
       NextAction("shadow bolt", 5.2f),
       NextAction("shoot", 5.0f)
    };
}

// ===== Trigger Initialization ===
void AfflictionWarlockStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericWarlockStrategy::InitTriggers(triggers);

    // Main target first: Corruption, Haunt on cooldown, then Unstable Affliction
    triggers.push_back(
        new TriggerNode(
            "haunt",
            {
                NextAction("haunt", 20.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "unstable affliction",
            {
                NextAction("unstable affliction", 19.5f)
            }
        )
    );
    // [rot23] wowsims warlock__affliction: Summon Infernal near the boss' end, after UA, before Curse of Agony (18.5);
    // it was wired for Destruction only
    triggers.push_back(new TriggerNode("inferno boss", {NextAction("inferno", 19.0f)}));
    // [b31_deep] wowsims opener Corruption -> Haunt -> UA -> CoA: Haunt and Shadow Bolt roll Corruption (Everlasting
    // Affliction) only once it is up, so it ranks above Haunt. With EA it rarely needs a refresh after that.
    triggers.push_back(
        new TriggerNode(
            "corruption",
            {
                NextAction("corruption", 20.2f)
            }
        )
    );

    // Multi-dotting other attackers ranks below the main target and its curse
    triggers.push_back(
        new TriggerNode(
            "unstable affliction on attacker",
            {
                NextAction("unstable affliction on attacker", 17.4f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "corruption on attacker",
            {
                NextAction("corruption on attacker", 17.2f)
            }
        )
    );

    // Shadow Trance for free casts; Drain Soul is the filler at or below 25 % (four times its damage there)
    // [rot472] the instant Shadow Bolt ranks below the execute Drain Soul (was 16.0): wowsims
    // warlock__affliction.apl.json has no Shadow Trance row, its Shadow Bolt (instant or not) is the filler below the
    // execute-phase channel
    triggers.push_back(
        new TriggerNode(
            "shadow trance",
            {
                NextAction("shadow bolt", 15.4f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "drain soul execute",
            {
                NextAction("drain soul", 15.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "drain soul clip",
            {
                NextAction("cancel channel", 20.5f)
            }
        )
    );

    // Life Tap glyph buff, and Life Tap as filler
    triggers.push_back(
        new TriggerNode(
            "life tap glyph buff",
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
    triggers.push_back(new TriggerNode("set bonus backlash", { NextAction("shadow bolt", 16.1f) }));
    triggers.push_back(new TriggerNode("set bonus ascendant pact", { NextAction("life tap", 5.65f) }));
}
