/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AssassinationRogueStrategy.h"
#include "Playerbots.h"

class AssassinationRogueStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    AssassinationRogueStrategyActionNodeFactory()
    {
        creators["mutilate"] = &mutilate;
        creators["envenom"] = &envenom;
        creators["backstab"] = &backstab;
        creators["hemorrhage"] = &hemorrhage;
    }

private:
    static ActionNode* mutilate([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "mutilate",
            /*P*/ {},
            /*A*/ { NextAction("backstab") },
            /*C*/ {}
        );
    }
    static ActionNode* envenom([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "envenom",
            /*P*/ {},
            /*A*/ { NextAction("eviscerate") },
            /*C*/ {}
        );
    }
    static ActionNode* backstab([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "backstab",
            /*P*/ {},
            /*A*/ { NextAction("hemorrhage") },
            /*C*/ {}
        );
    }
    // Hemorrhage when Backstab is not possible (Subtlety has its own strategy since [b31_wr])
    static ActionNode* hemorrhage([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "hemorrhage",
            /*P*/ {},
            /*A*/ { NextAction("sinister strike") },
            /*C*/ {}
        );
    }
};

AssassinationRogueStrategy::AssassinationRogueStrategy(PlayerbotAI* botAI)
    : GenericRogueStrategy(botAI)
{
    actionNodeFactories.Add(new AssassinationRogueStrategyActionNodeFactory());
}

std::vector<NextAction> AssassinationRogueStrategy::getDefaultActions()
{
    return {
        NextAction("melee", ACTION_DEFAULT)
    };
}

void AssassinationRogueStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
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

    // [rot27] wowsims rogue__mutilate: Mutilate at <= 3 combo points, Vanish for Overkill above the finishers
    triggers.push_back(new TriggerNode("mutilate combo points", {NextAction("mutilate", ACTION_NORMAL + 3)}));
    triggers.push_back(new TriggerNode("vanish for overkill", {NextAction("vanish", ACTION_HIGH + 5.3f)}));

    // [b31_wr] Rupture only as the bleed Hunger for Blood needs ("rupture" at 4 points outranked Envenom: build 30
    // Envenom 0.5 a minute, none in 21 of 33 boss fights, Rupture 1.7).
    triggers.push_back(
        new TriggerNode(
            "assassination rupture",
            {
                NextAction("rupture", ACTION_HIGH + 7),
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "hunger for blood",
            {
                NextAction("hunger for blood", ACTION_HIGH + 6),
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "slice and dice",
            {
                // [b31_wr] above Envenom (HIGH + 5): the tie let Envenom take the points while Slice and Dice was down.
                // [rot340] and above the Hunger for Blood enabler Rupture (HIGH + 7) and Hunger for Blood (HIGH + 6):
                // wowsims rogue__mutilate.apl.json row 2 (Slice and Dice <= 1 s) outranks row 4 (Hunger for Blood).
                // b221 rfc-h 5E Rtrogab opened every boss Mutilate -> Rupture -> Hunger for Blood -> Mutilate -> Slice
                // and Dice, the first 6-8 s without it (sim uptime 98 %, bot 41-84 %)
                NextAction("slice and dice", ACTION_HIGH + 7.5f),
            }
        )
    );

    // [b31_wr] the group's major armor debuff when no warrior takes it (RaidUtilityAssignment); was Combat-only, so an
    // Assassination rogue picked as the owner never applied it
    triggers.push_back(
        new TriggerNode(
            "expose armor",
            {
                NextAction("expose armor", ACTION_HIGH + 5.2f)
            }
        )
    );

    // [b31_wr] Cold Blood at 5 points right before the finisher (was at 4 points, HIGH + 6, also ahead of Rupture)
    triggers.push_back(
        new TriggerNode(
            "cold blood finisher",
            {
                NextAction("cold blood", ACTION_HIGH + 5.1f)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "combo points 4 available",
            {
                NextAction("envenom", ACTION_HIGH + 5)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "target with combo points almost dead",
            {
                NextAction("envenom", ACTION_HIGH + 4)
            }
        )
    );

    // Subtlety talents (no-ops for Assassination, which does not know them)
    triggers.push_back(
        new TriggerNode(
            "shadow dance",
            {
                NextAction("shadow dance", ACTION_HIGH + 3)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "in shadow dance",
            {
                NextAction("ambush", ACTION_HIGH + 6)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus titanic flow", { NextAction("mutilate", ACTION_NORMAL + 4) }));
}
