/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DpsPaladinStrategy.h"
#include "Playerbots.h"
#include "Strategy.h"

class DpsPaladinStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    DpsPaladinStrategyActionNodeFactory()
    {
        creators["retribution aura"] = &retribution_aura;
        creators["blessing of might"] = &blessing_of_might;
        creators["repentance"] = &repentance;
        creators["repentance on enemy healer"] = &repentance_on_enemy_healer;
        creators["repentance on snare target"] = &repentance_on_snare_target;
        // [b31_pal] removed: "sanctity aura" (TBC, not in the 3.3.5 Spell.dbc, no trigger queued it) and
        // "repentance of shield", a key nobody queued whose node was named "repentance" (a second, mismatched
        // "repentance" node with Divine Shield as its alternative)
    }

private:
    static ActionNode* blessing_of_might([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "blessing of might",
            /*P*/ {},
            /*A*/ { NextAction("blessing of kings") },
            /*C*/ {}
        );
    }

    static ActionNode* repentance([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "repentance",
            /*P*/ {},
            /*A*/ { NextAction("hammer of justice") },
            /*C*/ {}
        );
    }

    static ActionNode* repentance_on_enemy_healer([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "repentance on enemy healer",
            /*P*/ {},
            /*A*/ { NextAction("hammer of justice on enemy healer") },
            /*C*/ {}
        );
    }

    static ActionNode* repentance_on_snare_target([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "repentance on snare target",
            /*P*/ {},
            /*A*/ { NextAction("hammer of justice on snare target") },
            /*C*/ {}
        );
    }

    static ActionNode* retribution_aura([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "retribution aura",
            /*P*/ {},
            /*A*/ { NextAction("devotion aura") },
            /*C*/ {}
        );
    }
};

DpsPaladinStrategy::DpsPaladinStrategy(PlayerbotAI* botAI) : GenericPaladinStrategy(botAI)
{
    actionNodeFactories.Add(new DpsPaladinStrategyActionNodeFactory());
}

std::vector<NextAction> DpsPaladinStrategy::getDefaultActions()
{
    // [b31_deep] WotLK 3.3 Retribution priority, the wowsims default APL (first come, first served): Hammer of
    // Wrath > Judgement > Crusader Strike > Divine Storm > Exorcism (Art of War, 5.35) > Consecration (a target
    // that lives 4 s more) > Holy Wrath (undead and demons, 5.1). Hammer of Wrath fails isPossible (TARGET_AURASTATE)
    // above 20 % health and costs no GCD there. Judgement of Light instead of Wisdom while a protection paladin in
    // the group keeps Wisdom up ("paladin tank judges wisdom", 5.75): one of each per target.
    return {
        NextAction("hammer of wrath", ACTION_DEFAULT + 0.8f),
        NextAction("judgement of wisdom", ACTION_DEFAULT + 0.7f),
        NextAction("crusader strike", ACTION_DEFAULT + 0.6f),
        NextAction("divine storm", ACTION_DEFAULT + 0.5f),
        NextAction("ret consecration", ACTION_DEFAULT + 0.3f),
        NextAction("melee", ACTION_DEFAULT)
    };
}

void DpsPaladinStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericPaladinStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode(
            "art of war",
            {
                // [b31_deep] above Consecration (5.3): the instant proc lasts 15 s
                NextAction("exorcism", ACTION_DEFAULT + 0.35f)
            }
        )
    );
    // [b31_deep] a protection paladin in the group keeps Judgement of Wisdom on the target
    triggers.push_back(new TriggerNode("paladin tank judges wisdom",
        { NextAction("judgement of light", ACTION_DEFAULT + 0.75f) }));
    triggers.push_back(
        new TriggerNode(
            "seal",
            {
                NextAction("seal of corruption", ACTION_HIGH)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "holy wrath target",
            {
                NextAction("holy wrath", ACTION_DEFAULT + 0.1f)
            }
        )
    );
    // Mana comes from Judgements of Wisdom and Divine Plea on cooldown, never from swapping the seal to Wisdom:
    // under Seal of Wisdom the Seal of Vengeance stacks and Righteous Vengeance procs are lost.
    triggers.push_back(
        new TriggerNode(
            "ret divine plea",
            {
                NextAction("divine plea", ACTION_HIGH)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "avenging wrath",
            {
                NextAction("avenging wrath", ACTION_HIGH + 2)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "medium aoe",
            {
                NextAction("divine storm", ACTION_HIGH + 4),
                NextAction("consecration", ACTION_HIGH + 3)
            }
        )
    );
    // [b34_pal] AoE FCFS against undead / demons: Divine Storm > Consecration > Holy Wrath (2+ within 8 yd of the
    // paladin), above Avenging Wrath (+2) and the single-target rotation. "holy wrath target" (5.1) stays the
    // single-target filler. Build 34 Ret: Holy Wrath 0.08/min in UK, 0.36 Scholomance, 0.74 HoR.
    triggers.push_back(new TriggerNode("holy wrath undead pack",
        { NextAction("holy wrath undead pack", ACTION_HIGH + 2.9f) }));
    triggers.push_back(
        new TriggerNode(
            "enemy out of melee",
            {
                NextAction("reach melee", ACTION_HIGH + 1)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus divine storm reset",
        { NextAction("divine storm", ACTION_DEFAULT + 0.9f) }));
    triggers.push_back(new TriggerNode("set bonus wrath of the titans",
        { NextAction("hammer of wrath", ACTION_DEFAULT + 0.8f) }));
}
