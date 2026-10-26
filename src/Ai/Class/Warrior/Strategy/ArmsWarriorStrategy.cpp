/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArmsWarriorStrategy.h"

class ArmsWarriorStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    ArmsWarriorStrategyActionNodeFactory()
    {
        creators["charge"] = &charge;
        creators["death wish"] = &death_wish;
        creators["piercing howl"] = &piercing_howl;
        creators["heroic strike"] = &heroic_strike;
    }

private:
    static ActionNode* charge(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "charge",
            /*P*/ {},
            /*A*/ { NextAction("reach melee") },
            /*C*/ {}
        );
    }

    static ActionNode* death_wish(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "death wish",
            /*P*/ {},
            /*A*/ { NextAction("bloodrage") },
            /*C*/ {}
        );
    }

    static ActionNode* piercing_howl(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "piercing howl",
            /*P*/ {},
            // [b31_wr] straight to Hamstring: Mocking Blow is a taunt and has no place in a dps snare chain
            /*A*/ { NextAction("hamstring") },
            /*C*/ {}
        );
    }

    static ActionNode* heroic_strike(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "heroic strike",
            /*P*/ {},
            /*A*/ { NextAction("melee") },
            /*C*/ {}
        );
    }
};

ArmsWarriorStrategy::ArmsWarriorStrategy(PlayerbotAI* botAI) : GenericWarriorStrategy(botAI)
{
    actionNodeFactories.Add(new ArmsWarriorStrategyActionNodeFactory());
}

std::vector<NextAction> ArmsWarriorStrategy::getDefaultActions()
{
    return {
        NextAction("mortal strike", ACTION_DEFAULT + 0.1f),
        NextAction("sunder armor", ACTION_DEFAULT + 0.05f),
        NextAction("melee", ACTION_DEFAULT)
    };
}
void ArmsWarriorStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericWarriorStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode(
            "enemy out of melee",
            {
                NextAction("charge", ACTION_MOVE + 10)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "battle stance",
            {
                NextAction("battle stance", ACTION_HIGH + 10)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "battle shout",
            {
                NextAction("battle shout", ACTION_HIGH + 9)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "rend",
            {
                NextAction("rend", ACTION_HIGH + 8)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "rend on attacker",
            {
                NextAction("rend on attacker", ACTION_HIGH + 8)
            }
        )
    );

    // [b31_deep] Mortal Strike on cooldown once its 30 rage is there (was the "mortal strike" debuff trigger plus a
    // 20 rage node shared with Slam): 5.1 a minute against a ceiling of ~10.
    triggers.push_back(
        new TriggerNode(
            "mortal strike ready",
            {
                NextAction("mortal strike", ACTION_HIGH + 5)
            }
        )
    );
    // [b31_deep] wowsims arms: Shattering Throw as a boss cooldown; Arms is in Battle Stance already
    triggers.push_back(
        new TriggerNode(
            "shattering throw boss",
            {
                NextAction("shattering throw boss", ACTION_HIGH + 9)
            }
        )
    );
    // [b31_deep] Warcraft Tavern Arms: Heroic Throw whenever possible; no rage, below every other GCD
    triggers.push_back(
        new TriggerNode(
            "heroic throw ready",
            {
                NextAction("heroic throw", ACTION_HIGH + 0.4f)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "target critical health",
            {
                NextAction("execute", ACTION_HIGH + 3)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "sudden death",
            {
                NextAction("execute", ACTION_HIGH + 3)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "hamstring",
            {
                NextAction("piercing howl", ACTION_HIGH)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "overpower",
            {
                NextAction("overpower", ACTION_HIGH + 7)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "taste for blood",
            {
                NextAction("overpower", ACTION_HIGH + 7)
            }
        )
    );

    // [b31_deep] Victory Rush is free but weaker than Mortal Strike / Overpower: a filler, above the rotation only
    // below medium health (it was ACTION_INTERRUPT: 2-2.9 a minute on trash, above Mortal Strike).
    triggers.push_back(
        new TriggerNode(
            "victory rush",
            {
                NextAction("victory rush", ACTION_HIGH + 0.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "victory rush and medium health",
            {
                NextAction("victory rush", ACTION_INTERRUPT)
            }
        )
    );

    // [b31_deep] wowsims arms: Heroic Strike from 40 rage (was 60: 2.8 a minute); 45 keeps Mortal Strike's 30
    triggers.push_back(
        new TriggerNode(
            "arms heroic strike rage",
            {
                NextAction("heroic strike", ACTION_HIGH)
            }
        )
    );

    // [b31_deep] Slam filler only with rage to spare for Mortal Strike (CastSlamFillerAction)
    triggers.push_back(
        new TriggerNode(
            "light rage available",
            {
                NextAction("slam filler", ACTION_HIGH + 1)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "execute phase and high rage",
            {
                NextAction("execute", ACTION_HIGH + 6)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "bladestorm",
            {
                NextAction("bladestorm", ACTION_HIGH + 4)
            }
        )
    );

    // Stance-dance: above "battle stance" so the swap back waits until Recklessness is cast.
    triggers.push_back(
        new TriggerNode(
            "recklessness ready",
            {
                NextAction("berserker stance", ACTION_HIGH + 12),
                NextAction("recklessness", ACTION_HIGH + 11)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "bloodrage",
            {
                NextAction("bloodrage", ACTION_HIGH + 2)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "death wish",
            {
                NextAction("death wish", ACTION_HIGH + 2)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "critical health solo",
            {
                NextAction("intimidating shout", ACTION_EMERGENCY)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "medium health",
            {
                // [b31_deep] the enrage Enraged Regeneration needs first (it was only castable on Wrecking Crew)
                NextAction("berserker rage for enrage", ACTION_EMERGENCY + 0.5f),
                NextAction("bloodrage for enrage", ACTION_EMERGENCY + 0.3f),
                NextAction("enraged regeneration", ACTION_EMERGENCY)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "almost full health",
            {
                NextAction("retaliation", ACTION_EMERGENCY + 1)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "shattering throw trigger",
            {
                NextAction("shattering throw", ACTION_INTERRUPT + 1)
            }
        )
    );

    // [melee_audit_b30] Interrupts: Pummel needs Berserker Stance; the stance goes up first only when Pummel is ready
    // ("pummel ready"), and "battle stance" (HIGH + 10) swaps back once it is spent.
    triggers.push_back(
        new TriggerNode(
            "pummel ready",
            {
                NextAction("berserker stance", ACTION_INTERRUPT + 0.5f),
                NextAction("pummel", ACTION_INTERRUPT)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus slam!", { NextAction("slam", ACTION_HIGH + 2) }));
}
