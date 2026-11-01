/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FrostDKStrategy.h"
#include "Playerbots.h"

class FrostDKStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    FrostDKStrategyActionNodeFactory()
    {
        creators["icy touch"] = &icy_touch;
        creators["obliterate"] = &obliterate;
        creators["howling blast"] = &howling_blast;
        creators["frost strike"] = &frost_strike;
        creators["rune strike"] = &rune_strike;
        creators["unbreakable armor"] = &unbreakable_armor;
    }

private:
    static ActionNode* icy_touch([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "icy touch",
            /*P*/ { NextAction("blood presence") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* obliterate([[maybe_unused]] PlayerbotAI* botAI)
    {
        // [b31_deep] Death Strike (the same Frost + Unholy runes) at levels 58-60, before Obliterate is learnt (61)
        return new ActionNode(
            "obliterate",
            /*P*/ { NextAction("blood presence") },
            /*A*/ { NextAction("death strike") },
            /*C*/ {}
        );
    }

    static ActionNode* rune_strike([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "rune strike",
            /*P*/ { NextAction("blood presence") },
            /*A*/ { NextAction("melee") },
            /*C*/ {}
        );
    }

    static ActionNode* frost_strike([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "frost strike",
            /*P*/ { NextAction("blood presence") },
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* howling_blast([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "howling blast",
            /*P*/ { NextAction("blood presence") },
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* unbreakable_armor([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "unbreakable armor",
            /*P*/ { NextAction("blood tap") },
            /*A*/ {},
            /*C*/ {}
        );
    }
};

FrostDKStrategy::FrostDKStrategy(PlayerbotAI* botAI) : GenericDKStrategy(botAI)
{
    actionNodeFactories.Add(new FrostDKStrategyActionNodeFactory());
}

std::vector<NextAction> FrostDKStrategy::getDefaultActions()
{
    return {
        NextAction("obliterate", ACTION_DEFAULT + 0.7f),
        NextAction("frost strike", ACTION_DEFAULT + 0.4f),
        NextAction("horn of winter", ACTION_DEFAULT + 0.1f),
        NextAction("melee", ACTION_DEFAULT)
    };
}

void FrostDKStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    GenericDKStrategy::InitMultipliers(multipliers);
    multipliers.push_back(new DKDiseaseRefreshMultiplier(botAI));  // [b31_dk]
}

void FrostDKStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericDKStrategy::InitTriggers(triggers);

    // [b31_dk] Blood Presence for Frost DPS (the 3.3.5 standard; Frost Presence marks a tank, PlayerbotAI::IsTank).
    // Above movement and peel, below interrupts: an instant, and the rotation's prerequisites expect a presence.
    triggers.push_back(new TriggerNode("no presence", { NextAction("blood presence", ACTION_MOVE + 9) }));

    // [b31_dk] Killing Machine (51124, 100 % crit on the next Icy Touch / Howling Blast / Frost Strike): Frost Strike
    // (runic power). Above the default rotation (Obliterate 5.7), below the disease refreshes (22) and Pestilence (29).
    // [b31_deep] No Howling Blast on Killing Machine: on one target it spends Obliterate's runes for about half the
    // damage (wowsims frost_bl_pesti: Killing Machine on Frost Strike, Howling Blast with Rime only).
    // [rot21] wowsims frost_bl_pesti: Killing Machine Frost Strike right above Obliterate, only while no Frost / Unholy
    // rune has stood ready over a second and Frost Fever has 3 s; Rime Howling Blast (Frost Fever >= 3 s) above both
    triggers.push_back(
        new TriggerNode("killing machine frost strike", {NextAction("frost strike", ACTION_DEFAULT + 0.72f)}));
    triggers.push_back(
        new TriggerNode("freezing fog frost fever", {NextAction("howling blast", ACTION_DEFAULT + 0.75f)}));
    // [rot21] Pestilence at Frost Fever <= 6 s with the left Blood rune (keeps the diseases without Icy Touch / Plague
    // Strike), under Frost Strike at 115 runic power
    triggers.push_back(new TriggerNode("frost pestilence", {NextAction("pestilence", ACTION_DEFAULT + 0.62f)}));
    // [b31_deep] Frost Strike at >= 115 runic power right below Obliterate, above Unbreakable Armor / Rime / Blood
    // Strike (wowsims frost_bl_pesti), so runic power does not cap
    triggers.push_back(
        new TriggerNode("frost strike runic power", { NextAction("frost strike", ACTION_DEFAULT + 0.65f) }));

    triggers.push_back(
        new TriggerNode(
            "unbreakable armor",
            {
                NextAction("unbreakable armor", ACTION_DEFAULT + 0.6f)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "freezing fog",
            {
                NextAction("howling blast", ACTION_DEFAULT + 0.5f)
            }
        )
    );

    // [b31_deep] every ready Blood rune; a Death rune is left for Obliterate. [rot21] wowsims: only while Frost Fever
    // has more than 10 s, so the Blood rune for Pestilence stays
    triggers.push_back(new TriggerNode("frost blood strike", {NextAction("blood strike", ACTION_DEFAULT + 0.45f)}));
    // [b31_deep] Raise Dead as a damage cooldown. [rot39] wowsims frost_bl_pesti summons it inside its opener (the first
    // ~8 GCDs of the boss); at 5.42 (under every spender) the bot raised it late in the 30-60 s rfc-h boss fights: the
    // ghoul's Claw 1.2 % of the damage against the sim's 8.5 % (b179 5G Rtdekad). Now above Obliterate (5.7): once per
    // boss / burn ("raise dead boost"), at its start
    triggers.push_back(new TriggerNode("raise dead boost", {NextAction("raise dead", ACTION_DEFAULT + 0.74f)}));

    // [b35_uh] Empower Rune Weapon on a boss once every rune is down (wowsims frost_bl_pesti: once, in the boss opener
    // after the runes are spent); "no rune" in GenericDKStrategy spent it on trash too. Same priority as before.
    triggers.push_back(new TriggerNode("no rune boss", { NextAction("empower rune weapon", ACTION_HIGH + 1) }));

    triggers.push_back(
        new TriggerNode(
            "army of the dead",
            {
                NextAction("army of the dead", ACTION_HIGH + 6)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "icy touch 3s",
            {
                NextAction("icy touch", ACTION_HIGH + 2)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "plague strike 3s",
            {
                NextAction("plague strike", ACTION_HIGH + 2)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "low health",
            {
                NextAction("icebound fortitude", ACTION_HIGH + 5)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus ascendant power", { NextAction("frost strike", ACTION_HIGH + 1) }));
}

void FrostDKAoeStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode(
            "medium aoe",
            {
                NextAction("howling blast", ACTION_HIGH + 4)
            }
        )
    );
}
