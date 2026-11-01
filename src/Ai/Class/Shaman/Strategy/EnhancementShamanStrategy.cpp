/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EnhancementShamanStrategy.h"
#include "Playerbots.h"

// ===== Action Node Factory =====
class EnhancementShamanStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    EnhancementShamanStrategyActionNodeFactory()
    {
        creators["lava lash"] = &lava_lash;
    }

private:
    static ActionNode* lava_lash([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "lava lash",
            /*P*/ {},
            /*A*/ { NextAction("melee") },
            /*C*/ {}
        );
    }
};

// ===== Single Target Strategy =====
EnhancementShamanStrategy::EnhancementShamanStrategy(PlayerbotAI* botAI) : GenericShamanStrategy(botAI)
{
    actionNodeFactories.Add(new EnhancementShamanStrategyActionNodeFactory());
}

// ===== Default Actions =====
std::vector<NextAction> EnhancementShamanStrategy::getDefaultActions()
{
    return {
       NextAction("stormstrike", 5.5f),
       // [rot6] wowsims enhancement order: Stormstrike, Flame Shock (5.45), Earth Shock, Fire Nova, Lightning Shield
       // (5.22), Lava Lash
       NextAction("earth shock between flame shocks", 5.3f),
       // [b31_deep] Fire Nova on cooldown below Earth Shock, above Lava Lash (wowsims enhancement APLs). Its
       // isUseful() keeps it to a pack, or the single-target case of ai::shaman::AllowsSingleTargetFireAoe.
       NextAction("fire nova", 5.25f),
       NextAction("lava lash", 5.2f),
       NextAction("melee", 5.0f)
    };
}

// ===== Trigger Initialization ===
void EnhancementShamanStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericShamanStrategy::InitTriggers(triggers);

    // Totem Trigger
    triggers.push_back(
        new TriggerNode(
            "call of the elements and enemy within melee",
            {
                NextAction("call of the elements", 60.0f)
            }
        )
    );

    // [b31_sd] In-combat re-imbue of both weapons (moved here from "boost", which only covered the off hand).
    // [b31_deep] ("boost" is a default strategy outside battlegrounds, AiFactory.) Windfury on the main hand;
    // below level 30 (no Windfury Weapon)
    // the Flametongue alternative takes it. Each enchant action is possible only on an unimbued weapon.
    triggers.push_back(new TriggerNode("main hand weapon no imbue",
                                       { NextAction("windfury weapon main hand", 24.6f),
                                         NextAction("flametongue weapon main hand", 24.4f) }));
    triggers.push_back(
        new TriggerNode("off hand weapon no imbue", { NextAction("flametongue weapon off hand", 24.5f) }));

    // Spirit Walk Trigger
    triggers.push_back(
        new TriggerNode(
            "spirit walk ready",
            {
                NextAction("spirit walk", 50.0f)
            }
        )
    );

    // Damage Triggers
    triggers.push_back(
        new TriggerNode(
            "enemy out of melee",
            {
                NextAction("reach melee", 40.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "maelstrom weapon 5",
            {
                NextAction("lightning bolt", 20.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "flame shock",
            {
                NextAction("flame shock", 5.45f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "lightning shield",
            {
                NextAction("lightning shield", 5.22f)
            }
        )
    );

    // [shside_enh_mana] Feral Spirit is a cooldown (bosses, hard pulls: the burst window), not a trash filler.
    triggers.push_back(new TriggerNode("feral spirit boost", { NextAction("feral spirit", 23.5f) }));

    // Health/Mana Triggers
    // [shside_enh_mana] Shamanistic Rage on cooldown in melee whenever mana is not near full: it is the spec's mana
    // tool (scholo-h 20261025-153805-4: 6 casts in ~25 min of fighting, Rtshaab dry at 7.6 %).
    triggers.push_back(new TriggerNode("shamanistic rage for mana", { NextAction("shamanistic rage", 23.0f) }));
    triggers.push_back(
        new TriggerNode(
            "low health",
            {
                NextAction("shamanistic rage", 23.0f)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus titanic strike", { NextAction("stormstrike", 5.6f) }));
    triggers.push_back(new TriggerNode("set bonus maelstrom weapon 4", { NextAction("lightning bolt", 5.15f) }));
}
