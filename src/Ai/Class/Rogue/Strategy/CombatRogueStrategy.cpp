/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatRogueStrategy.h"
#include "Playerbots.h"

class CombatRogueStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    CombatRogueStrategyActionNodeFactory()
    {
        creators["sinister strike"] = &sinister_strike;
        creators["rupture"] = &rupture;
    }

private:
    static ActionNode* sinister_strike([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "sinister strike",
            /*P*/ {},
            /*A*/ { NextAction("melee") },
            /*C*/ {}
        );
    }
    static ActionNode* rupture([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "rupture",
            /*P*/ {},
            /*A*/ { NextAction("eviscerate") },
            /*C*/ {}
        );
    }
};

CombatRogueStrategy::CombatRogueStrategy(PlayerbotAI* botAI) : GenericRogueStrategy(botAI)
{
    actionNodeFactories.Add(new CombatRogueStrategyActionNodeFactory());
}

std::vector<NextAction> CombatRogueStrategy::getDefaultActions()
{
    return {
        NextAction("melee", ACTION_DEFAULT)
    };
}

void CombatRogueStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericRogueStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode(
            "high energy available",
            {
                NextAction("garrote", ACTION_HIGH + 7),
                NextAction("ambush", ACTION_HIGH + 6)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "combo points not full",
            {
                NextAction("sinister strike", ACTION_NORMAL + 3)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "slice and dice",
            {
                NextAction("slice and dice", ACTION_HIGH + 5)
            }
        )
    );

    // [b31_wr] a running Slice and Dice under 6 s takes 4-5 points before Rupture / Eviscerate (wowsims combat APL)
    triggers.push_back(
        new TriggerNode(
            "slice and dice pre-refresh",
            {
                NextAction("slice and dice", ACTION_HIGH + 5)
            }
        )
    );

    // [b31_wr] Blade Flurry on cooldown on one target too, under Slice and Dice (was "light aoe" only: 0.44 a minute)
    triggers.push_back(
        new TriggerNode(
            "blade flurry ready",
            {
                NextAction("blade flurry", ACTION_HIGH + 2.5f)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "riposte",
            {
                NextAction("riposte", ACTION_HIGH + 4)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "combo points 5 available",
            {
                NextAction("rupture", ACTION_HIGH + 1),
                NextAction("eviscerate", ACTION_HIGH)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "target with combo points almost dead",
            {
                NextAction("eviscerate", ACTION_HIGH + 2)
            }
        )
    );
    // [rot28] wowsims rogue__combat: a 4-point Eviscerate while Slice and Dice (>= 4 s) and Rupture (>= 10 s) hold
    triggers.push_back(new TriggerNode("combat eviscerate", {NextAction("eviscerate", ACTION_HIGH)}));

    triggers.push_back(
        new TriggerNode(
            "expose armor",
            {
                NextAction("expose armor", ACTION_HIGH + 3)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus titanic flow", { NextAction("sinister strike", ACTION_NORMAL + 4) }));
}
