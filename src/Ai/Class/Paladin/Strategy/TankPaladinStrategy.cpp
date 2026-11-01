/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TankPaladinStrategy.h"
#include "Playerbots.h"
#include "RezzedTankHoldMultiplier.h"

class TankPaladinStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    TankPaladinStrategyActionNodeFactory()
    {
        creators["seal of corruption"] = &seal_of_corruption;
        creators["seal of vengeance"] = &seal_of_vengeance;
        creators["seal of command"] = &seal_of_command;
        creators["hand of reckoning"] = &hand_of_reckoning;
        creators["taunt spell"] = &hand_of_reckoning;
    }

private:
    static ActionNode* seal_of_command([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "seal of command",
            /*P*/ {},
            /*A*/ { NextAction("seal of corruption") },
            /*C*/ {}
        );
    }
    static ActionNode* seal_of_corruption([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "seal of corruption",
            /*P*/ {},
            /*A*/ { NextAction("seal of vengeance") },
            /*C*/ {}
        );
    }

    static ActionNode* seal_of_vengeance([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "seal of vengeance",
            /*P*/ {},
            /*A*/ { NextAction("seal of righteousness") },
            /*C*/ {}
        );
    }

    static ActionNode* hand_of_reckoning([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "hand of reckoning",
            /*P*/ {},
            /*A*/ { NextAction("righteous defense") },
            /*C*/ {}
        );
    }
};

TankPaladinStrategy::TankPaladinStrategy(PlayerbotAI* botAI) : GenericPaladinStrategy(botAI)
{
    actionNodeFactories.Add(new TankPaladinStrategyActionNodeFactory());
}

std::vector<NextAction> TankPaladinStrategy::getDefaultActions()
{
    // [lead67] user: «проверь, как эффективно танк набирает агро» - b100: Avenger's Shield 23 times in 45 pulls,
    // Shield of Righteousness / Hammer of the Righteous / Consecration / Holy Shield ~2/3 of their cooldowns. The 969
    // threat order on its own cooldowns: Avenger's Shield whenever ready (3 targets, the pull's first threat), Holy
    // Shield kept up, Consecration under the pack, then Shield of Righteousness, Hammer of the Righteous, Judgement;
    // the fillers (Hand of Reckoning, Hammer of Wrath, Exorcism) only from their own triggers in free GCDs
    // wowsims-wotlk default APL "969" (C:/WoW/DEEP_AUDIT_PALADIN.md): Avenger's Shield as the opener and on cooldown,
    // Shield of Righteousness and Hammer of the Righteous (6 s) alternating, then Hammer of Wrath > Consecration >
    // Holy Shield > Judgement
    return {
        NextAction("avenger's shield", ACTION_DEFAULT + 0.9f),
        NextAction("shield of righteousness", ACTION_DEFAULT + 0.8f),
        NextAction("hammer of the righteous", ACTION_DEFAULT + 0.75f),
        NextAction("consecration", ACTION_DEFAULT + 0.65f),
        NextAction("holy shield", ACTION_DEFAULT + 0.6f),
        NextAction("judgement of wisdom", ACTION_DEFAULT + 0.5f),
        NextAction("melee", ACTION_DEFAULT)
    };
}

void TankPaladinStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    GenericPaladinStrategy::InitMultipliers(multipliers);
    multipliers.push_back(new RezzedTankHoldMultiplier(botAI));
}

void TankPaladinStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericPaladinStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode(
            "seal",
            {
                NextAction("seal of corruption", ACTION_HIGH)
            }
        )
    );
    // [b31_deep] Seal of Vengeance / Corruption the whole fight (Holy Vengeance is ~13 % of the damage and its threat);
    // mana comes from Divine Plea, Judgement of Wisdom and Sanctuary. Seal of Wisdom only below 10 % mana ("low
    // mana", 15 %, swapped the seal 0.1/min); SealTrigger puts Vengeance back above 70 %.
    triggers.push_back(
        new TriggerNode(
            "tank seal of wisdom",
            {
                NextAction("seal of wisdom", ACTION_HIGH + 9)
            }
        )
    );
    // A paladin tank lives on mana: taunts, Consecration and Holy Shield all cost it. Divine Plea goes on cooldown
    // once mana dips (its halved healing does not matter to a tank that leaves healing to the healer), and the
    // target keeps Judgement of Wisdom, whose procs feed the whole melee group.
    // [lead87] Divine Plea whenever its return fits (TankDivinePleaTrigger), not only below "high mana"
    triggers.push_back(
        new TriggerNode(
            "tank divine plea",
            {
                NextAction("divine plea", ACTION_HIGH + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "judgement of wisdom",
            {
                NextAction("judgement of wisdom", ACTION_HIGH + 3)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "light aoe",
            {
                NextAction("avenger's shield", ACTION_HIGH + 5)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "medium aoe",
            {
                NextAction("consecration", ACTION_HIGH + 7),
                NextAction("avenger's shield", ACTION_HIGH + 6),
                // 10 yd around the tank; it hits and stuns Undead and Demons only (Spell.dbc TargetCreatureType 36):
                // [b31_deep] CastHolyWrathAction::isUseful wants one of those within 10 yd
                NextAction("holy wrath", ACTION_HIGH + 5.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "lose aggro",
            {
                NextAction("hand of reckoning", ACTION_HIGH + 7)
            }
        )
    );
    // Peel: take back a mob hitting the healer or a damage dealer, also when it is not the current target.
    // Above "set facing" (ACTION_MOVE + 7) and "tank face" (ACTION_MOVE), as the warrior's taunt: at ACTION_HIGH + 9
    // the tank spent its ticks turning and stepping while the mob stayed on the healer.
    triggers.push_back(
        new TriggerNode(
            "mob attacking group member",
            {
                // two or three fresh mobs on one friend: Righteous Defense takes them all, Hand of Reckoning one
                NextAction("righteous defense on swarmed group member", ACTION_INTERRUPT + 0.55f),
                NextAction("hand of reckoning on group member attacker", ACTION_INTERRUPT + 0.5f),
                NextAction("righteous defense on attacked group member", ACTION_INTERRUPT + 0.4f),
                NextAction("avenger's shield on group member attacker", ACTION_INTERRUPT + 0.3f),
                // taunts on cooldown or GCD (wave 8 sample: 87% of peel ticks): a 6 s stun keeps it off the healer
                NextAction("hammer of justice on group member attacker", ACTION_INTERRUPT + 0.25f),
                // then threat by hand on the loose mob in reach (these take the best one within their reach)
                NextAction("hammer of the righteous on group member attacker", ACTION_INTERRUPT + 0.2f),
                NextAction("shield of righteousness on group member attacker", ACTION_INTERRUPT + 0.15f),
                NextAction("judgement of wisdom on group member attacker", ACTION_INTERRUPT + 0.1f)
            }
        )
    );
    // A pack around the tank with a mob held by proximity aggro alone (the first heal takes it) or on a member: area
    // threat as the pull lands and while one is loose. Righteous Fury first, it nearly doubles all of it. Below the
    // peel steps, above "tank assist".
    triggers.push_back(
        new TriggerNode(
            "tank aoe threat",
            {
                NextAction("righteous fury", ACTION_INTERRUPT + 0.22f),
                NextAction("consecration tank aoe", ACTION_INTERRUPT + 0.19f),
                NextAction("avenger's shield", ACTION_INTERRUPT + 0.18f),
                NextAction("holy shield", ACTION_INTERRUPT + 0.17f)
            }
        )
    );
    // [b34_pal] 969 AoE: Holy Wrath on its 30 s cooldown on an undead / demon pack (2+ within 8 yd), around the tank
    // whatever its target. Its 3 s stun holds the pack too. Above the area threat of "tank aoe threat" (+0.17..0.19)
    // and the threat-by-hand peel steps (+0.1..0.15), below Hammer of the Righteous peel (+0.2) and every taunt.
    // strat-h-5E-1 pull 2: 25-31 attackers, the tank's target on the healer, Holy Wrath 0 in 34 s ("medium aoe"
    // holy wrath at ACTION_HIGH + 5.5 is a melee action on the current target and sat under the peel all fight).
    triggers.push_back(new TriggerNode("holy wrath undead pack",
        { NextAction("holy wrath undead pack", ACTION_INTERRUPT + 0.185f) }));
    // [b32_tank_aoe] "tank aoe pack" (TankAoeThreat.h): 3+ mobs in reach and one without a safe threat lead -> the area
    // spell above the single-target builders until the lead is safe; then the single-target order below resumes.
    // Above Holy Shield (ACTION_HIGH + 6.5) and the 969 band (+5.2..5.9), level with "medium aoe" Consecration (+7).
    triggers.push_back(new TriggerNode("tank aoe pack",
        {
            NextAction("consecration tank aoe", ACTION_HIGH + 7.1f),
            NextAction("avenger's shield", ACTION_HIGH + 6.6f)
        }));
    // [b32_peel] "tank aoe crowd" (TankAoeThreat.h): more than 3 mobs in reach, one not held -> the area threat above
    // every peel step of "mob attacking group member" (+0.1..+0.55) and "taunt reaction" (TankPeel::CrowdFirst);
    // below the multi-mob Righteous Defense of "mobs attacking group members" (+0.6). Off while a mob is on the healer.
    triggers.push_back(new TriggerNode("tank aoe crowd",
        {
            NextAction("consecration tank aoe", ACTION_INTERRUPT + 0.58f),
            NextAction("avenger's shield", ACTION_INTERRUPT + 0.57f)
        }));
    // One mob within Consecration's reach is held by it while the taunts cool down (two or more: below).
    triggers.push_back(
        new TriggerNode(
            "mob attacking group member nearby",
            {
                NextAction("consecration on group member attackers", ACTION_MOVE + 2)
            }
        )
    );
    // The mob the peel would take stands within a few steps: walk to it (above "reach melee" back to the target).
    triggers.push_back(
        new TriggerNode(
            "group member attacker near",
            {
                NextAction("reach group member attacker", ACTION_MOVE + 0.9f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "mobs attacking group members",
            {
                NextAction("righteous defense on attacked group member", ACTION_INTERRUPT + 0.6f),
                NextAction("avenger's shield on group member attacker", ACTION_INTERRUPT + 0.2f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "mobs attacking group members nearby",
            {
                // around the tank, not the current target: that one may stand away from the adds
                NextAction("consecration on group member attackers", ACTION_MOVE + 2),
                // Consecration down: Holy Wrath's burst, which also stuns Undead and Demons (Stratholme wave 25: the
                // healer died to Cadavers and Berserkers 2 yd from the tank)
                // [b31_deep] only with an undead or demon attacker in reach (Holy Wrath hits no other type)
                NextAction("holy wrath on group member attackers", ACTION_MOVE + 1.9f)
            }
        )
    );
    // Two or more adds out of reach of the area threat: step towards them (what the tank holds follows) and take
    // them with the AoE of "mobs attacking group members nearby"; reach melee brings the tank back afterwards.
    triggers.push_back(
        new TriggerNode(
            "group member attackers far",
            {
                NextAction("move to group member attackers", ACTION_MOVE + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "holy shield",
            {
                NextAction("holy shield", ACTION_HIGH + 6.5f)
            }
        )
    );
    // [b31_pal] 969: the 6 s Shield of Righteousness / Hammer of the Righteous and the 9 s Judgement / Consecration on
    // their cooldowns, Avenger's Shield as the filler on a boss, all in one band under Holy Shield's upkeep (above)
    // and the taunts (ACTION_HIGH + 7), above Avenging Wrath, Divine Plea and the Judgement debuff upkeep. The peel,
    // area threat, movement and emergencies stay above the band. At 60-74 the unknown Shield of Righteousness (and an
    // untalented Hammer) keep their trigger off; the defaults at 5.x stay as the floor.
    triggers.push_back(new TriggerNode("shield of righteousness ready",
        { NextAction("shield of righteousness", ACTION_HIGH + 5.9f) }));
    triggers.push_back(new TriggerNode("hammer of the righteous ready",
        { NextAction("hammer of the righteous", ACTION_HIGH + 5.8f) }));
    // [lead96] a mob coming at the group with no tank threat: the ranged opener on it first (above the 969 band,
    // under the taunts)
    triggers.push_back(new TriggerNode("tank incoming mob",
        { NextAction("tank open incoming", ACTION_HIGH + 6.5f) }));
    triggers.push_back(new TriggerNode("judgement of wisdom ready",
        { NextAction("judgement of wisdom", ACTION_HIGH + 5.7f) }));
    triggers.push_back(new TriggerNode("consecration ready",
        { NextAction("consecration", ACTION_HIGH + 5.5f) }));
    triggers.push_back(new TriggerNode("avenger's shield boss filler",
        { NextAction("avenger's shield", ACTION_HIGH + 5.2f) }));
    triggers.push_back(
        new TriggerNode(
            "damage spike",
            {
                NextAction("divine protection", ACTION_EMERGENCY)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "critical health",
            {
                // before Divine Protection and the generic Divine Shield: both share its Forbearance
                NextAction("lay on hands", ACTION_EMERGENCY + 2),
                NextAction("divine protection", ACTION_EMERGENCY)
            }
        )
    );
    // Falling health under a hard pull: Divine Protection early, as a tank does, not at 25%.
    triggers.push_back(
        new TriggerNode(
            "tank pressure",
            {
                NextAction("divine protection", ACTION_EMERGENCY + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "avenging wrath",
            {
                NextAction("avenging wrath", ACTION_HIGH + 2)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "tank hammer of wrath",  // [lead87] only while its cost leaves the tank at its own line
            {
                NextAction("hammer of wrath", ACTION_CRITICAL_HEAL)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "righteous fury",
            {
                NextAction("righteous fury", ACTION_HIGH + 8)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "medium group heal setting",
            {
                NextAction("divine sacrifice", ACTION_HIGH + 5)
            }
        )
    );
    // [b31_pal] "enough mana" (above HighMana) -> Consecration at ACTION_HIGH + 4 replaced by "consecration ready"
    triggers.push_back(
        new TriggerNode(
            "not facing target",
            {
                NextAction("set facing", ACTION_NORMAL + 7)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "enemy out of melee",
            {
                // above the ranged rotation, Hammer of Wrath and "tank face" (ACTION_MOVE): a tank out of melee
                // walks to its target; the peel steps and area threat (ACTION_MOVE + 1..2) stay above it
                NextAction("reach melee", ACTION_MOVE + 0.5f)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus righteous aegis",
        { NextAction("shield of righteousness", ACTION_HIGH + 4.5f) }));
    triggers.push_back(new TriggerNode("set bonus avenger's shield",
        { NextAction("avenger's shield", ACTION_HIGH + 4) }));
    triggers.push_back(new TriggerNode("set bonus titanic judgement",
        { NextAction("judgement of wisdom", ACTION_HIGH + 3.5f) }));
}
