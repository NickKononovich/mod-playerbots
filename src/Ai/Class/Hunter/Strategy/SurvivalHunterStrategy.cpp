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
    std::vector<NextAction> actions;
    // [rot640] `spell` is the name the action casts; nullptr means the entry is never gated. The gate is
    // GenericHunterStrategy::KnowsSpell, shared with Marksmanship and Beast Mastery
    auto const add = [&](char const* action, float relevance, char const* spell)
    {
        if (!spell || KnowsSpell(spell))
            actions.push_back(NextAction(action, relevance));
    };

    add("kill command", 5.9f, "kill command");
    add("kill shot", 5.8f, "kill shot");
    add("explosive shot", 5.7f, "explosive shot");
    add("black arrow", 5.6f, "black arrow");
    add("serpent sting", 5.5f, "serpent sting");
    // [b31_deep] Aimed and Multi-Shot share one 10 s cooldown (category 85); the SV template takes Aimed Shot only
    // at 80, so below it the slot sat empty on single target (wowsims sv.apl: Aimed > Multi > Steady)
    add("aimed shot", 5.4f, "aimed shot");
    add("multi-shot", 5.35f, "multi-shot");
    // Useless once Explosive Shot is known ([b31_deep], CastArcaneShotAction::isUseful): pre-talent levels only
    add("arcane shot", 5.3f, "arcane shot");
    add("steady shot", 5.2f, "steady shot");
    // [rot640] Auto Shot is the hunter's white damage and every hunter has it from level 1: never gated, so no
    // gate of mine can ever cost a hunter its shots
    add("auto shot", 5.1f, nullptr);

    return actions;
}

// ===== Trigger Initialization ===
void SurvivalHunterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericHunterStrategy::InitTriggers(triggers);
    // [lead105] [rot7] below every shot and Auto Shot (wowsims never casts it): only when no shot can go
    triggers.push_back(new TriggerNode("enemy within melee", {NextAction("mongoose bite", ACTION_DEFAULT + 0.08f)}));

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
                NextAction("black arrow", 15.5f)  // [rot32] wowsims hunter__sv: Serpent Sting before Black Arrow
            }
        )
    );
    // [b31_deep] "low mana" -> "viper sting" removed: one sting per hunter, so it replaced Serpent Sting; only MM
    // (Chimera Shot on a Viper Sting returns mana) keeps it.
    triggers.push_back(
        new TriggerNode(
            "no stings",
            {
                NextAction("serpent sting", 16.5f)
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
