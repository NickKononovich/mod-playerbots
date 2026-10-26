/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PriestNonCombatStrategy.h"
#include "AiFactory.h"
#include "Playerbots.h"
#include "PriestNonCombatStrategyActionNodeFactory.h"

PriestNonCombatStrategy::PriestNonCombatStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI)
{
    actionNodeFactories.Add(new PriestNonCombatStrategyActionNodeFactory());
}

void PriestNonCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    NonCombatStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode("inner fire",{ NextAction("inner fire", 10.0f) }));
    triggers.push_back(
        new TriggerNode("vampiric embrace", { NextAction("vampiric embrace", 16.0f) }));
    triggers.push_back(new TriggerNode(
        "party member dead",{ NextAction("remove shadowform", ACTION_CRITICAL_HEAL + 11),
                                               NextAction("resurrection", ACTION_CRITICAL_HEAL + 10) }));
    triggers.push_back(new TriggerNode("often",{ NextAction("apply oil", 1.0f) }));
    // [b31_deep] Healing specs only: these heals drop Shadowform, and the non-combat engine also runs while the
    // group already fights and the shadow priest does not yet (renew on party dropped the form on L80 trash).
    if (AiFactory::GetPlayerSpecTab(botAI->GetBot()) != PRIEST_TAB_SHADOW)
    {
        triggers.push_back(
            new TriggerNode("party member critical health",
                            { NextAction("renew on party", ACTION_CRITICAL_HEAL + 3),
                              NextAction("penance on party", ACTION_CRITICAL_HEAL + 2),
                              NextAction("greater heal on party", ACTION_CRITICAL_HEAL + 1) }));

        triggers.push_back(
            new TriggerNode("party member low health",
                            { NextAction("renew on party", ACTION_MEDIUM_HEAL + 3),
                              NextAction("penance on party", ACTION_MEDIUM_HEAL + 2),
                              NextAction("greater heal on party", ACTION_MEDIUM_HEAL + 1) }));

        triggers.push_back(
            new TriggerNode("party member medium health",
                            { NextAction("renew on party", ACTION_LIGHT_HEAL + 9),
                              NextAction("penance on party", ACTION_LIGHT_HEAL + 8) }));

        triggers.push_back(
            new TriggerNode("party member almost full health",
                            { NextAction("renew on party", ACTION_LIGHT_HEAL + 3) }));

        triggers.push_back(
            new TriggerNode("group heal setting", { NextAction("circle of healing on party", 27.0f) }));
    }
    triggers.push_back(new TriggerNode("new pet",
                                      { NextAction("set pet stance", 10.0f) }));

    // [guide_b31_dead] Shadow: keep Shadowform out of combat (after a rez), so the opener DoTs go out in form.
    // Below the rez (40+) and party heals that drop the form, above Inner Fire.
    if (AiFactory::GetPlayerSpecTab(botAI->GetBot()) == PRIEST_TAB_SHADOW)
        triggers.push_back(new TriggerNode("shadowform out of combat", { NextAction("shadowform", 15.0f) }));

    // K6 pre-pull: shield and renew the tank that is about to pull (healing specs)
    if (AiFactory::GetPlayerSpecTab(botAI->GetBot()) != PRIEST_TAB_SHADOW)
    {
        triggers.push_back(new TriggerNode("pull imminent",
                                           { NextAction("power word: shield on main tank", ACTION_HIGH + 3),
                                             NextAction("renew on main tank", ACTION_HIGH + 2) }));
    }
}

void PriestBuffStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    NonCombatStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode("power word: fortitude on party",
                       { NextAction("power word: fortitude on party", 11.0f) }));
    triggers.push_back(new TriggerNode("divine spirit on party",
                                      { NextAction("divine spirit on party", 13.0f) }));
}

void PriestShadowResistanceStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    NonCombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("shadow protection",
                                      { NextAction("shadow protection", 12.0f) }));
    triggers.push_back(
        new TriggerNode("shadow protection on party",
                       { NextAction("shadow protection on party", 11.0f) }));
}
