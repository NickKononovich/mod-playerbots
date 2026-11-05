/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FuryWarriorStrategy.h"

class FuryWarriorStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    FuryWarriorStrategyActionNodeFactory()
    {
        creators["charge"] = &charge;
        creators["intercept"] = &intercept;
        creators["piercing howl"] = &piercing_howl;
        creators["pummel"] = &pummel;
    }

private:
    static ActionNode* charge(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "charge",
            /*P*/ {},
            /*A*/ { NextAction("intercept" )},
            /*C*/ {}
        );
    }

    static ActionNode* intercept(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "intercept",
            /*P*/ {},
            /*A*/ { NextAction("reach melee" )},
            /*C*/ {}
        );
    }

    static ActionNode* piercing_howl(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "piercing howl",
            /*P*/ {},
            /*A*/ { NextAction("hamstring" )},
            /*C*/ {}
        );
    }

    static ActionNode* pummel(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "pummel",
            /*P*/ {},
            /*A*/ { NextAction("intercept" )},
            /*C*/ {}
        );
    }
};

FuryWarriorStrategy::FuryWarriorStrategy(PlayerbotAI* botAI) : GenericWarriorStrategy(botAI)
{
    actionNodeFactories.Add(new FuryWarriorStrategyActionNodeFactory());
}

std::vector<NextAction> FuryWarriorStrategy::getDefaultActions()
{
    return {
        NextAction("bloodthirst", ACTION_DEFAULT + 0.5f),
        NextAction("whirlwind", ACTION_DEFAULT + 0.4f),
        // [melee_audit_b30] Execute (below 20 %) above Sunder Armor: build 30 Fury 80 cast 5 Executes in 22.9 min
        // against 94 Sunders, the filler took every execute-phase GCD.
        NextAction("execute", ACTION_DEFAULT + 0.35f),
        NextAction("sunder armor", ACTION_DEFAULT + 0.2f),
        NextAction("melee", ACTION_DEFAULT)
    };
}

void FuryWarriorStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericWarriorStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode(
            "enemy out of melee",
            {
                NextAction("charge", ACTION_MOVE + 9)
            }
        )
    );
    // [rot450] "fury berserker stance": not while the Rend weave waits in Battle Stance for its Rend
    triggers.push_back(
        new TriggerNode(
            "fury berserker stance", {
                NextAction("berserker stance", ACTION_HIGH + 9)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "battle shout",
            {
                NextAction("battle shout", ACTION_HIGH + 8)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "pummel on enemy healer",
            {
                NextAction("pummel on enemy healer", ACTION_INTERRUPT)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "pummel",
            {
                NextAction("pummel", ACTION_INTERRUPT)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "victory rush",
            {
                // [b31_deep] filler (was ACTION_INTERRUPT, above Bloodthirst); above the rotation below medium health
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
    // [b31_wr] 3.3.5 Fury: Bloodthirst > Whirlwind > Slam! (Bloodsurge) > Execute (below 20 % or Sudden Death) on
    // cooldown; Heroic Strike / Cleave dump rage off the global cooldown.
    triggers.push_back(
        new TriggerNode(
            "bloodthirst ready",
            {
                NextAction("bloodthirst", ACTION_HIGH + 7)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "whirlwind ready",
            {
                NextAction("whirlwind", ACTION_HIGH + 6)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "instant slam",
            {
                NextAction("slam", ACTION_HIGH + 5)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "target critical health",
            {
                NextAction("execute", ACTION_HIGH + 4)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "sudden death",
            {
                NextAction("execute", ACTION_HIGH + 4)
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
    // [rot19] wowsims fury: Heroic Strike from 12 rage (was 40), keeping the rage of the next Bloodthirst / Whirlwind
    triggers.push_back(
        new TriggerNode("fury heroic strike rage", {NextAction("heroic strike", ACTION_DEFAULT + 0.1f)}));
    // [rot520] wowsims fury: Cleave, not Heroic Strike, at the same 12 rage while another attacker stands by the target
    triggers.push_back(new TriggerNode("fury cleave rage", {NextAction("cleave", ACTION_DEFAULT + 0.1f)}));
    // [rot440] wowsims fury Rend weave: above "berserker stance" (HIGH + 9) so the swap back waits for the Rend; the
    // trigger's own conditions keep it under Bloodthirst, Whirlwind, Slam! and the execute phase as the APL does
    triggers.push_back(new TriggerNode("fury rend weave",
                                       { NextAction("battle stance", ACTION_HIGH + 10.6f),
                                         NextAction("rend", ACTION_HIGH + 10.5f) }));
    // [rot19] wowsims fury: Heroic Throw as the filler (no rage) under every strike
    triggers.push_back(new TriggerNode("heroic throw ready", {NextAction("heroic throw", ACTION_DEFAULT + 0.25f)}));

    triggers.push_back(
        new TriggerNode(
            "death wish",
            {
                NextAction("death wish", ACTION_HIGH)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "recklessness",
            {
                NextAction("recklessness", ACTION_HIGH)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "critical health",
            {
                // [b31_deep] Fury 80 has no Enrage talent: Enraged Regeneration was 0 in every log without this
                NextAction("berserker rage for enrage", ACTION_EMERGENCY + 0.5f),
                NextAction("bloodrage for enrage", ACTION_EMERGENCY + 0.3f),
                NextAction("enraged regeneration", ACTION_EMERGENCY)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus sudden death", { NextAction("execute", ACTION_HIGH + 3) }));
    triggers.push_back(new TriggerNode("set bonus taste for blood",
        { NextAction("battle stance", ACTION_HIGH + 11),
          NextAction("overpower", ACTION_HIGH + 10) }));
    triggers.push_back(new TriggerNode("set bonus ascendant whirl", { NextAction("whirlwind", ACTION_HIGH + 6.5f) }));
}
