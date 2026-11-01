/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SurvivalHunterStrategy.h"
#include "Playerbots.h"

// ===== Action Node Factory =====
class SurvivalHunterStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    SurvivalHunterStrategyActionNodeFactory()
    {
        creators["explosive shot rank 4"] = &explosive_shot_rank_4;
        creators["explosive shot rank 3"] = &explosive_shot_rank_3;
        creators["explosive shot rank 2"] = &explosive_shot_rank_2;
    }

private:
    static ActionNode* explosive_shot_rank_4([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("explosive shot rank 4",
                              /*P*/ {},
                              /*A*/ { NextAction("explosive shot rank 3") },
                              /*C*/ {});
    }
    static ActionNode* explosive_shot_rank_3([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("explosive shot rank 3",
                              /*P*/ {},
                              /*A*/ { NextAction("explosive shot rank 2") },
                              /*C*/ {});
    }
    static ActionNode* explosive_shot_rank_2([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("explosive shot rank 2",
                              /*P*/ {},
                              /*A*/ { NextAction("explosive shot rank 1") },
                              /*C*/ {});
    }
};

SurvivalHunterStrategy::SurvivalHunterStrategy(PlayerbotAI* botAI) : GenericHunterStrategy(botAI)
{
    actionNodeFactories.Add(new SurvivalHunterStrategyActionNodeFactory());
}

// ===== Default Actions =====
std::vector<NextAction> SurvivalHunterStrategy::getDefaultActions()
{
    return {
        NextAction("kill command", 5.9f),
        NextAction("kill shot", 5.8f),
        NextAction("explosive shot", 5.7f),
        NextAction("black arrow", 5.6f),
        NextAction("serpent sting", 5.5f),
        NextAction("aimed shot", 5.4f),
        // [b31_deep] Aimed and Multi-Shot share one 10 s cooldown (category 85); the SV template takes Aimed Shot only
        // at 80, so below it the slot sat empty on single target (wowsims sv.apl: Aimed > Multi > Steady)
        NextAction("multi-shot", 5.35f),
        // Useless once Explosive Shot is known ([b31_deep], CastArcaneShotAction::isUseful): pre-talent levels only
        NextAction("arcane shot", 5.3f),
        NextAction("steady shot", 5.2f),
        NextAction("auto shot", 5.1f)
    };
}

// ===== Trigger Initialization ===
void SurvivalHunterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericHunterStrategy::InitTriggers(triggers);
    triggers.push_back(new TriggerNode("enemy within melee", { NextAction("mongoose bite", 22.0f) }));  // [lead105]

    triggers.push_back(
        new TriggerNode(
            "lock and load",
            {
                // [b31_deep] double tap: highest rank, then the next rank while our highest-rank DoT ticks
                NextAction("explosive shot lock and load", 28.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "kill command",
            {
                NextAction("kill command", 18.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "target critical health",
            {
                NextAction("kill shot", 18.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "explosive shot",
            {
                NextAction("explosive shot", 17.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "black arrow",
            {
                NextAction("black arrow", 16.5f)
            }
        )
    );
    // [b31_deep] "low mana" -> "viper sting" removed: one sting per hunter, so it replaced Serpent Sting; only MM
    // (Chimera Shot on a Viper Sting returns mana) keeps it.
    triggers.push_back(
        new TriggerNode(
            "no stings",
            {
                NextAction("serpent sting", 15.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "serpent sting on attacker",
            {
                NextAction("serpent sting on attacker", 15.0f)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus ascendant execution", { NextAction("kill shot", 18.4f) }));
    triggers.push_back(new TriggerNode("set bonus titan's focus", { NextAction("explosive shot rank 4", 17.95f) }));
    triggers.push_back(new TriggerNode("set bonus improved steady shot",
        { NextAction("aimed shot", 17.8f),
          NextAction("arcane shot", 17.7f) }));
}
