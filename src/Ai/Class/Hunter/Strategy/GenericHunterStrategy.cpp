/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericHunterStrategy.h"

class GenericHunterStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    GenericHunterStrategyActionNodeFactory()
    {
        creators["rapid fire"] = &rapid_fire;
        creators["mongoose bite"] = &mongoose_bite;
        creators["raptor strike"] = &raptor_strike;
        creators["explosive trap"] = &explosive_trap;
    }

private:
    static ActionNode* rapid_fire([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("rapid fire",
                              /*P*/ {},
                              /*A*/ { NextAction("readiness") },
                              /*C*/ {});
    }
    static ActionNode* mongoose_bite([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("mongoose bite",
                              /*P*/ {},
                              /*A*/ { NextAction("raptor strike") },
                              /*C*/ {});
    }
    static ActionNode* raptor_strike([[maybe_unused]] PlayerbotAI* botAI)
    {
        // no "melee" prerequisite: a white swing stops Auto Shot
        return new ActionNode("raptor strike",
                              /*P*/ {},
                              /*A*/ {},
                              /*C*/ {});
    }
    static ActionNode* explosive_trap([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("explosive trap",
                              /*P*/ {},
                              /*A*/ { NextAction("immolation trap") },
                              /*C*/ {});
    }
};

GenericHunterStrategy::GenericHunterStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericHunterStrategyActionNodeFactory());
}

void GenericHunterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    CombatStrategy::InitTriggers(triggers);

    // Mark/Ammo/Mana Triggers
    triggers.push_back(new TriggerNode("no ammo", { NextAction("equip upgrades packet action", 30.0f) }));
    // [guild2] no ammo, or meleed inside Auto Shot's minimum range: melee auto-attack on (Raptor Strike needs it)
    // [guild14] out of the dead zone to shoot when the mob does not follow (above the melee fallback)
    triggers.push_back(new TriggerNode("hunter step out", {NextAction("hunter step out", 30.3f)}));
    triggers.push_back(new TriggerNode("hunter needs melee",
                                       { NextAction("reach melee", 30.2f), NextAction("melee", 30.1f) }));
    // [guild15] in melee reach with no step out: the bites at once (the white swing starts first, 30.1)
    triggers.push_back(new TriggerNode("hunter melee phase", {NextAction("mongoose bite", 30.05f)}));
    triggers.push_back(new TriggerNode("hunter's mark", { NextAction("hunter's mark", 29.5f) }));
    triggers.push_back(new TriggerNode("rapid fire", { NextAction("rapid fire", 29.0f) }));
    triggers.push_back(new TriggerNode("readiness", { NextAction("readiness", 29.0f) }));  // [rot160]
    triggers.push_back(new TriggerNode("aspect of the viper", { NextAction("aspect of the viper", 28.0f) }));

    // Aggro/Threat/Defensive Triggers
    // [b31_deep] "has aggro" -> "concussive shot" removed: the action aims at "snare target" anyway, so it duplicated
    // the "concussive shot on snare target" node below while "has aggro" was true almost every tick.
    triggers.push_back(
        new TriggerNode("misdirection on main tank", { NextAction("misdirection on main tank", 27.0f) }));
    triggers.push_back(new TriggerNode("low health", { NextAction("deterrence", 35.0f) }));
    triggers.push_back(new TriggerNode("concussive shot on snare target", { NextAction("concussive shot", 20.0f) }));
    triggers.push_back(new TriggerNode("medium threat", { NextAction("feign death", 35.0f) }));
    triggers.push_back(new TriggerNode("high threat", { NextAction("feign death", 35.0f) }));
    // [rot32] a medium-health pet's Mend Pet under Kill Command / Kill Shot / the stings (wowsims never casts it); a
    // low-health pet keeps 21
    triggers.push_back(new TriggerNode("hunters pet medium health", {NextAction("mend pet", 16.0f)}));
    triggers.push_back(new TriggerNode("hunters pet low health", { NextAction("mend pet", 21.0f) }));
    // [b31_deep] the "pet" strategy (Call/Revive Pet) is non-combat only: a pet dismissed in combat stayed away
    triggers.push_back(new TriggerNode("hunters pet dismissed", { NextAction("call pet", 23.0f) }));

    // Dispel Triggers
    triggers.push_back(new TriggerNode("tranquilizing shot enrage", { NextAction("tranquilizing shot", 61.0f) }));
    triggers.push_back(new TriggerNode("tranquilizing shot magic", { NextAction("tranquilizing shot", 61.0f) }));

    // Ranged-based Triggers
    // [lead105] the melee bites only for Beast Mastery / Survival (their strategies add them): never Marksmanship
    // [rot7] Wing Clip below every shot and Auto Shot (wowsims never casts it): only when no shot can go
    triggers.push_back(new TriggerNode(
        "enemy within melee", {NextAction("explosive trap", 37.0f), NextAction("wing clip", ACTION_DEFAULT + 0.05f)}));

    // [rot7] below every shot and Auto Shot: they go only when no shot can (wowsims casts neither)
    triggers.push_back(new TriggerNode(
        "enemy too close for auto shot",
        {NextAction("disengage", ACTION_DEFAULT + 0.09f), NextAction("flee", ACTION_DEFAULT + 0.085f)}));
}

// ===== AoE Strategy, 2/3+ enemies =====
AoEHunterStrategy::AoEHunterStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI) {}

void AoEHunterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("volley channel check", { NextAction("cancel channel", 23.0f) }));
    // [b33_aoe_class] 3+ stacked: Multi-Shot on cooldown above the single-target shots (it hits the current target,
    // so a "dungeon kill first" target still takes it); Volley only on 4+ ("high aoe") and, like Multi-Shot, never
    // beside a crowd-controlled mob (the actions' isUseful). Was: Volley on 3+ at 22, Multi-Shot only at 17.4.
    triggers.push_back(new TriggerNode("medium aoe", { NextAction("multi-shot", 22.5f) }));
    // [b44_hos] Volley is the 4+ filler in place of Steady Shot (5.2), below every other shot of the three specs'
    // lists (5.3 and up) and their triggers. At 22 it outranked Kill Command, Kill Shot, Chimera, Aimed, Serpent
    // Sting and Explosive Shot: the Tribunal MM hunter cast 12 Steady Shots in 16 attempts, 6 s Volley channels
    // instead (DPS_GAP_TRIBUNAL_COS.md 2.5). The math (Spell.dbc 58433 tick 353 + 0.0837 RAP, 6 ticks in the 6 s
    // channel; RAP 5000): ~771 per target per second, N = 4 -> ~3.1k/s, ~2.3k/s after the Auto Shots the channel
    // delays; Steady Shot 49052 weapon + 252 + 0.1 RAP (~1.85k) per 1.5 s cast ~1.2-1.4k/s, Kill Shot ~4.2k and
    // Chimera Shot ~3.5-4k per GCD ~2.5-3k/s. wowsims' hunter aoe.apl: Explosive Trap, Serpent multi-dot, then Volley.
    triggers.push_back(new TriggerNode("high aoe", { NextAction("volley", 5.25f) }));
    // [b31_deep] 21 -> 17.4: on two targets Multi-Shot went before Kill Command, Kill Shot and Explosive Shot
    triggers.push_back(new TriggerNode("light aoe", { NextAction("multi-shot", 17.4f) }));
}

void HunterCcStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("scare beast", { NextAction("scare beast on cc", 23.0f) }));
    // [guide_b31_dead] "freezing trap on cc" had no action creator; "freezing trap" already targets "cc target"
    triggers.push_back(new TriggerNode("freezing trap", { NextAction("freezing trap", 23.0f) }));
}

void HunterTrapWeaveStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("immolation trap no cd", { NextAction("reach melee", 23.0f) }));
}
