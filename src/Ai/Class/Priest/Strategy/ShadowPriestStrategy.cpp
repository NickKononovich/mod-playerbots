/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ShadowPriestStrategy.h"
#include "Playerbots.h"
#include "ShadowPriestStrategyActionNodeFactory.h"

ShadowPriestStrategy::ShadowPriestStrategy(PlayerbotAI* botAI) : GenericPriestStrategy(botAI)
{
    actionNodeFactories.Add(new ShadowPriestStrategyActionNodeFactory());
}

std::vector<NextAction> ShadowPriestStrategy::getDefaultActions()
{
    // [rot18] wowsims shadow_priest__default: Mind Blast is hidden (never cast); Inner Focus right before a Mind Flay
    // at 5 Shadow Weaving stacks
    return {
        NextAction("mind flay", ACTION_DEFAULT + 0.2f),
        NextAction("shadow word: death", ACTION_DEFAULT + 0.1f), // cast during movement
        NextAction("shoot", ACTION_DEFAULT)
    };
}

void ShadowPriestStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericPriestStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode(
            "shadowform",
            {
                // [guide_b31_dead] top priority whenever missing: above Dispersion (+5), Mind Sear (+4) and the DoTs
                NextAction("shadowform", ACTION_HIGH + 6)
            }
        )
    );
    // [guide_b31_dead] clip Mind Flay right after a tick when a DoT is due ([rot18] not for Mind Blast, which wowsims
    // never casts). Movement already breaks the channel, so Shadow Word: Death needs no clip.
    triggers.push_back(new TriggerNode("mind flay clip", { NextAction("cancel channel", ACTION_HIGH + 5.5f) }));
    triggers.push_back(
        new TriggerNode("inner focus shadow weaving", {NextAction("inner focus", ACTION_DEFAULT + 0.25f)}));
    triggers.push_back(
        new TriggerNode(
            "low mana",
            {
                NextAction("dispersion", ACTION_HIGH + 5)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "critical health",
            {
                NextAction("dispersion", ACTION_HIGH + 5)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "silence",
            {
                NextAction("silence", ACTION_INTERRUPT + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "silence on enemy healer",
            {
                NextAction("silence on enemy healer", ACTION_INTERRUPT)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus titanic insight", { NextAction("mind blast", ACTION_HIGH + 0.5f) }));
    triggers.push_back(new TriggerNode("set bonus ascendant void",
        { NextAction("shadow word: death", ACTION_HIGH + 0.4f) }));
}

void ShadowPriestAoeStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode(
            "shadow word: pain on attacker",
            {
                NextAction("shadow word: pain on attacker", ACTION_NORMAL + 5)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "vampiric touch on attacker",
            {
                NextAction("vampiric touch on attacker", ACTION_NORMAL + 4)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "mind sear channel check",
            {
                NextAction("cancel channel", ACTION_HIGH + 5)
            }
        )
    );
    // [b31_deep] wowsims aoe APLs: at 2-5 targets the current target's DoTs (VT +3, DP +2, SW:P +1) come first and
    // Mind Sear after them (above Mind Blast / Mind Flay, below the set-bonus procs +0.4 / +0.5); it goes over
    // the DoTs only with 5+ other enemies within its 10 yd around the target (it never hits the target itself).
    // [rot401] on the mobs where it beats Mind Flay (MindSearPaysTrigger), not on any 3 in 8 yd
    triggers.push_back(new TriggerNode("mind sear pays", { NextAction("mind sear", ACTION_HIGH + 0.3f) }));
    triggers.push_back(new TriggerNode("mind sear heavy aoe", { NextAction("mind sear", ACTION_HIGH + 4) }));
}

void ShadowPriestDebuffStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode(
            "vampiric touch",
            {
                NextAction("vampiric touch", ACTION_HIGH + 3)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "devouring plague",
            {
                NextAction("devouring plague", ACTION_HIGH + 2)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "shadow word: pain",
            {
                NextAction("shadow word: pain", ACTION_HIGH + 1)
            }
        )
    );
}
