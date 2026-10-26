/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SubtletyRogueStrategy.h"
#include "Playerbots.h"

// [b31_wr]
class SubtletyRogueStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    SubtletyRogueStrategyActionNodeFactory()
    {
        creators["backstab"] = &backstab;
        creators["hemorrhage"] = &hemorrhage;
        creators["rupture"] = &rupture;
    }

private:
    // Backstab needs the rogue behind the target and a dagger: Hemorrhage otherwise
    static ActionNode* backstab([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "backstab",
            /*P*/ {},
            /*A*/ { NextAction("hemorrhage") },
            /*C*/ {}
        );
    }
    // below the Hemorrhage talent
    static ActionNode* hemorrhage([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "hemorrhage",
            /*P*/ {},
            /*A*/ { NextAction("sinister strike") },
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

SubtletyRogueStrategy::SubtletyRogueStrategy(PlayerbotAI* botAI) : GenericRogueStrategy(botAI)
{
    actionNodeFactories.Add(new SubtletyRogueStrategyActionNodeFactory());
}

std::vector<NextAction> SubtletyRogueStrategy::getDefaultActions()
{
    return {
        NextAction("melee", ACTION_DEFAULT)
    };
}

void SubtletyRogueStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericRogueStrategy::InitTriggers(triggers);

    // Openers from Stealth (Ambush first for Subtlety); Premeditation is off the global cooldown
    triggers.push_back(
        new TriggerNode(
            "premeditation",
            {
                NextAction("premeditation", ACTION_HIGH + 8)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "high energy available",
            {
                NextAction("ambush", ACTION_HIGH + 7),
                NextAction("garrote", ACTION_HIGH + 6)
            }
        )
    );

    // Shadow Dance with energy pooled (ShadowDanceTrigger), then Ambush for its 6 s
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
                NextAction("ambush", ACTION_HIGH + 6.5f)
            }
        )
    );

    // Finishers: Slice and Dice upkeep above everything, then Rupture / Eviscerate at 5 points
    triggers.push_back(
        new TriggerNode(
            "slice and dice",
            {
                NextAction("slice and dice", ACTION_HIGH + 5.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "combo points 5 available",
            {
                NextAction("rupture", ACTION_HIGH + 5),
                NextAction("eviscerate", ACTION_HIGH + 4)
            }
        )
    );
    // the group's major armor debuff when this rogue owns it and no warrior is there (RaidUtilityAssignment)
    triggers.push_back(
        new TriggerNode(
            "expose armor",
            {
                NextAction("expose armor", ACTION_HIGH + 5.2f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "target with combo points almost dead",
            {
                NextAction("eviscerate", ACTION_HIGH + 4.5f)
            }
        )
    );

    // Builders: Hemorrhage while its debuff is missing, else Backstab from behind (Hemorrhage otherwise)
    triggers.push_back(
        new TriggerNode(
            "hemorrhage",
            {
                NextAction("hemorrhage", ACTION_NORMAL + 4)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "combo points not full",
            {
                NextAction("backstab", ACTION_NORMAL + 3)
            }
        )
    );
}
