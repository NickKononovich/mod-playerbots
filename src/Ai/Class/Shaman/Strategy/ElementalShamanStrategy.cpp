/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ElementalShamanStrategy.h"
#include "Playerbots.h"

ElementalShamanStrategy::ElementalShamanStrategy(PlayerbotAI* botAI) : GenericShamanStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

// ===== Default Actions =====
std::vector<NextAction> ElementalShamanStrategy::getDefaultActions()
{
    // Lava Burst, Chain Lightning and Lightning Bolt have cast times and wait for the shaman to stand; on the move
    // the instants keep going: Flame Shock through its trigger, Earth Shock while Flame Shock outlasts the shock
    // cooldown.
    return {
        NextAction("earth shock on the move", 5.4f),
        NextAction("lava burst", 5.2f),
        NextAction("lightning bolt", 5.0f)
    };
}

// ===== Trigger Initialization ===
void ElementalShamanStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericShamanStrategy::InitTriggers(triggers);

    // Totem Triggers
    triggers.push_back(
        new TriggerNode(
            "call of the elements",
            {
                NextAction("call of the elements", 60.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "low health",
            {
                NextAction("stoneclaw totem", 40.0f)
            }
        )
    );

    // [b31_sd] Flametongue Weapon's spell power (it also feeds the Fire Elemental): an imbue that expired mid-fight is
    // restored (moved here from "boost").
    // [b31_deep] "boost" IS a default strategy outside battlegrounds (AiFactory); the move is still right: the
    // imbue belongs to the spec, not to the burst window.
    triggers.push_back(
        new TriggerNode("main hand weapon no imbue", { NextAction("flametongue weapon main hand", 23.5f) }));

    // Cooldown Trigger
    triggers.push_back(
        new TriggerNode(
            "elemental mastery",
            {
                NextAction("elemental mastery", 29.0f)
            }
        )
    );

    // Damage Triggers
    triggers.push_back(
        new TriggerNode(
            "earth shock execute",
            {
                NextAction("earth shock", 5.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "flame shock",
            {
                NextAction("flame shock", 5.3f)
            }
        )
    );
    // [b31_deep] Flame Shock multidot on a second / third mob (wowsims elemental APLs): below Lava Burst (5.2), above
    // Lightning Bolt (5.0); the action keeps the current target's own Flame Shock first.
    triggers.push_back(new TriggerNode("flame shock on attacker", { NextAction("flame shock on attacker", 5.15f) }));
    // [b44_cast] No single-target Chain Lightning filler: wowsims elemental casts Chain Lightning only when
    // numberTargets > 1 ("chain lightning aoe", ShamanAoeStrategy). Sim A/B on the bots' own counted boss fights
    // (3 requests, 3000 iterations, same seed): Chain Lightning on cooldown on one target 4801 DPS vs the APL 5062
    // (-5.2 %); the bot cast it 5.5/min there, 8 % of its boss time (ROTATION_AUDIT_NIGHT.md, b44_cast).

    // Mana Triggers
    triggers.push_back(
        new TriggerNode(
            "water shield",
            {
                NextAction("water shield", 19.5f)
            }
        )
    );

    // Range Triggers
    triggers.push_back(
        new TriggerNode(
            "thunderstorm knockback",
            {
                NextAction("thunderstorm", 19.0f)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus lava surge", { NextAction("lava burst", 5.28f) }));
    triggers.push_back(new TriggerNode("set bonus ascendant storm", { NextAction("chain lightning", 5.27f) }));
}
