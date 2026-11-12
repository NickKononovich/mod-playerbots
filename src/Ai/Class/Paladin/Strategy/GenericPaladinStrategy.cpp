/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericPaladinStrategy.h"
#include "AiFactory.h"
#include "GenericPaladinStrategyActionNodeFactory.h"
#include "Playerbots.h"

GenericPaladinStrategy::GenericPaladinStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericPaladinStrategyActionNodeFactory());
}

void GenericPaladinStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    CombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("hammer of justice interrupt",
        { NextAction("hammer of justice", ACTION_INTERRUPT) }));
    triggers.push_back(new TriggerNode("hammer of justice on enemy healer",
        { NextAction("hammer of justice on enemy healer", ACTION_INTERRUPT) }));
    triggers.push_back(new TriggerNode("hammer of justice on snare target",
        { NextAction("hammer of justice on snare target", ACTION_INTERRUPT) }));
    // [kzk1] one paladin keeps Judgement of Wisdom on the boss (the raid's mana), a second one Judgement of Light:
    // ai::paladin::BossJudgementDuty; above the rotations' own judgements (ACTION_HIGH + 5.7 the tank's), below the
    // emergencies and interrupts
    triggers.push_back(new TriggerNode("boss judgement duty", { NextAction("boss judgement duty", ACTION_HIGH + 6) }));
    triggers.push_back(new TriggerNode("critical health", { NextAction("divine shield", ACTION_EMERGENCY) }));
    triggers.push_back(new TriggerNode("party member critical health",
        { NextAction("lay on hands on party", ACTION_EMERGENCY + 2) }));
    triggers.push_back(new TriggerNode("divine shield low health",
        { NextAction("flash of light", ACTION_EMERGENCY + 3), NextAction("holy light", ACTION_EMERGENCY + 2)}));
    triggers.push_back(new TriggerNode("protect party member",
        { NextAction("blessing of protection on party", ACTION_EMERGENCY + 3) }));
    // [b31_pal] no generic "medium mana" Divine Plea: each spec queues its own ("holy divine plea", "ret divine plea",
    // the tank's "high mana", the offheal's "low mana"); the generic one doubled Ret's and Holy's.
    triggers.push_back(new TriggerNode("hand of freedom on party",
        { NextAction("hand of freedom on party", ACTION_HIGH + 4) }));
    triggers.push_back(new TriggerNode("hand of sacrifice on main tank",
        { NextAction("hand of sacrifice on main tank", ACTION_EMERGENCY + 1) }));
    triggers.push_back(new TriggerNode("tank damage spike",
        { NextAction("hand of sacrifice on spiking tank", ACTION_EMERGENCY + 1) }));
    triggers.push_back(new TriggerNode("hand of salvation on party",
        { NextAction("hand of salvation on party", ACTION_HIGH + 5) }));
    triggers.push_back(new TriggerNode("aura mastery", { NextAction("aura mastery", ACTION_HIGH + 6) }));
    // [rot_b31_pdd] a seal without its Judgement aura state goes, the spec's "seal" (ACTION_HIGH) casts it again
    triggers.push_back(new TriggerNode("seal state lost", { NextAction("drop stale seal", ACTION_HIGH + 1) }));
}

void PaladinCureStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // [b31_deep] Dispels are the healer's job: a retribution paladin cleanses below its whole rotation (5.3-5.8),
    // above auto attack. Build 30 L70s boss: Cleanse 2.62/min, 15 % of the spec's casts, at 19.5 above all of it.
    // The urgent cures below keep their relevance for every spec.
    bool const isRet = AiFactory::GetPlayerSpecTab(botAI->GetBot()) == PALADIN_TAB_RETRIBUTION;
    float const selfCure = isRet ? ACTION_DEFAULT + 0.16f : ACTION_LIGHT_HEAL + 9.6f;
    float const partyCure = isRet ? ACTION_DEFAULT + 0.15f : ACTION_LIGHT_HEAL + 9.5f;
    triggers.push_back(new TriggerNode(
        "cleanse cure disease", { NextAction("cleanse disease", selfCure) }));
    triggers.push_back(
        new TriggerNode("cleanse party member cure disease",
                        { NextAction("cleanse disease on party", partyCure) }));
    triggers.push_back(new TriggerNode(
        "cleanse cure poison", { NextAction("cleanse poison", selfCure) }));
    triggers.push_back(
        new TriggerNode("cleanse party member cure poison",
                        { NextAction("cleanse poison on party", partyCure) }));
    triggers.push_back(new TriggerNode(
        "cleanse cure magic", { NextAction("cleanse magic", selfCure) }));
    triggers.push_back(
        new TriggerNode("cleanse party member cure magic",
                        { NextAction("cleanse magic on party", partyCure) }));

    // A debuff that hurts now: after Holy's critical heals (from 34), before its medium ones (25-26); a tank keeps
    // its taunts (from 27) first.
    bool const isTank = AiFactory::GetPlayerSpecTab(botAI->GetBot()) == PALADIN_TAB_PROTECTION;
    float const urgentRelevance = isTank ? ACTION_HIGH + 6.0f : ACTION_MEDIUM_HEAL + 7.5f;
    triggers.push_back(new TriggerNode("party member urgent cure poison",
                                       { NextAction("cleanse poison on party", urgentRelevance) }));
    triggers.push_back(new TriggerNode("party member urgent cure disease",
                                       { NextAction("cleanse disease on party", urgentRelevance - 0.1f) }));
    triggers.push_back(new TriggerNode("party member urgent cure magic",
                                       { NextAction("cleanse magic on party", urgentRelevance + 0.1f) }));
}

void PaladinBoostStrategy::InitTriggers(std::vector<TriggerNode*>& /*triggers*/)
{
    // triggers.push_back(new TriggerNode("divine favor", { NextAction("divine favor",
    // ACTION_HIGH + 1) }));
}

void PaladinCcStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("turn undead", { NextAction("turn undead", ACTION_HIGH + 1) }));
}

void PaladinHealerDpsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("healer should attack",
                        {
                            NextAction("hammer of wrath", ACTION_DEFAULT + 0.6f),
                            NextAction("holy shock", ACTION_DEFAULT + 0.5f),
                            NextAction("shield of righteousness", ACTION_DEFAULT + 0.4f),
                            NextAction("judgement of light", ACTION_DEFAULT + 0.3f),
                            NextAction("consecration", ACTION_DEFAULT + 0.2f),
                            NextAction("exorcism", ACTION_DEFAULT+ 0.1f),
                            // [noattack] The white swings between them: b338 Rollothan L10 holy, solo, judged and
                            // healed only - 2.5-3.0 dps against a formula of 14-15 (white 0 of its 9.6-10.3).
                            // MeleeAction swings for a ranged spec only at a mob in its melee reach.
                            NextAction("melee", ACTION_DEFAULT + 0.05f),
                            }));
    // [lag371] coordinator 2026-11-12 (b450 Rollothan L14 holy, solo in Westfall: 312 "[stuck258] the combat engine
    // out of combat with idle Harvest Watcher L14 (24-32 yd) ... no action that runs" in an hour, 780 xp/h): a holy
    // paladin counts as ranged (its spec's strategy type), so no "reach melee" is among its actions and "melee" never
    // walks - but its damage is a melee one (Judgement 10 yd, Crusader Strike, its swings; no ranged nuke until Holy
    // Shock / Exorcism of an undead): an engaged mob at its pull's distance was never reached. Alone, it walks in
    triggers.push_back(
        new TriggerNode("solo healer should attack", {NextAction("reach melee", ACTION_DEFAULT + 0.04f)}));
}
