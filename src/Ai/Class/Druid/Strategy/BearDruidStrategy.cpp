/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BearDruidStrategy.h"
#include "InstanceScript.h"
#include "Playerbots.h"
#include "RezzedTankHoldMultiplier.h"

class BearDruidStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    BearDruidStrategyActionNodeFactory()
    {
        creators["feral charge - bear"] = &feral_charge_bear;
        creators["dire bear form"] = &dire_bear_form;
        creators["maul"] = &maul;
        creators["bash"] = &bash;
        creators["swipe (bear)"] = &swipe_bear;
        creators["lacerate"] = &lacerate;
        creators["taunt spell"] = &growl; // Empty ActionNode needed to register as taunt spell
    }

private:
    static ActionNode* feral_charge_bear([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "feral charge - bear",
            /*P*/ {},
            /*A*/ { NextAction("reach melee") },
            /*C*/ {}
        );
    }

    static ActionNode* dire_bear_form([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "dire bear form",
            /*P*/ { NextAction("caster form") },
            /*A*/ { NextAction("bear form") },
            /*C*/ {}
        );
    }

    static ActionNode* maul([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "maul",
            /*P*/ {},
            /*A*/ { NextAction("melee") },
            /*C*/ {}
        );
    }

    static ActionNode* bash([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "bash",
            /*P*/ {},
            /*A*/ { NextAction("melee") },
            /*C*/ {}
        );
    }

    static ActionNode* swipe_bear([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "swipe (bear)",
            /*P*/ {},
            /*A*/ { NextAction("melee") },
            /*C*/ {}
        );
    }

    static ActionNode* lacerate([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "lacerate",
            /*P*/ {},
            /*A*/ { NextAction("maul") },
            /*C*/ {}
        );
    }

    static ActionNode* growl([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "growl",
            /*P*/ {},
            /*A*/ {},
            /*C*/ {}
        );
    }
};

BearDruidStrategy::BearDruidStrategy(PlayerbotAI* botAI) : FeralDruidStrategy(botAI)
{
    actionNodeFactories.Add(new BearDruidStrategyActionNodeFactory());
}

std::vector<NextAction> BearDruidStrategy::getDefaultActions()
{
    return {
        NextAction("maul",   5.2f),
        NextAction("enrage", 5.1f),
        NextAction("melee",  5.0f)
    };
}

float BearCooldownPacingMultiplier::GetValue(Action* action)
{
    // [rot_b31_pdd] the action name first: every other action leaves at once
    if (!action)
        return 1.0f;

    std::string const& name = action->getName();
    // [b33_vh] Barkskin (1 min) is paced too, more loosely (below)
    bool const barkskin = name == "barkskin";
    if (name != "survival instincts" && name != "frenzied regeneration" && !barkskin)
        return 1.0f;

    Map* map = bot->GetMap();
    if (!bot->IsInCombat() || !map || !map->IsDungeon())
        return 1.0f;

    auto const isBoss = [](Creature const* creature)
    {
        return creature && (creature->IsDungeonBoss() || creature->isWorldBoss() ||
                            creature->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS);
    };

    Unit* target = AI_VALUE(Unit*, "current target");
    if (isBoss(target ? target->ToCreature() : nullptr))
        return 1.0f;

    // a boss fight on its adds (the boss is not the bear's target). [b33_vh] Only with a boss among the attackers:
    // event instances keep IsEncounterInProgress() true for the whole event (Violet Hold: _encounterStatus from the
    // seal to Cyanigosa), so every portal wave counted as a boss fight. vh-h 20261026-001930 #13 (659 s): Survival
    // Instincts and Frenzied Regeneration went on a trash wave, both still cooling down on wave #17 (815 s) where the
    // bear fell 100 -> 0 % in 7 s to 5 mobs with DEF={}.
    if (InstanceScript* instance = bot->GetInstanceScript())
        if (instance->IsEncounterInProgress())
            for (ObjectGuid const guid : AI_VALUE(GuidVector, "attackers"))
            {
                Unit* attacker = botAI->GetUnit(guid);
                if (attacker && attacker->IsAlive() && isBoss(attacker->ToCreature()))
                    return 1.0f;
            }

    float const health = bot->GetHealthPct();
    float const recentLoss = AI_VALUE(float, "recent health loss");
    // [b33_vh] Barkskin on trash: below "medium health" or while health really falls, not from "almost full health"
    // at 70-100 % with the bar standing still (the same run: cast at 647 s at 100 %, 706 s at 71 % healed back up and
    // in pull #16, then on cooldown at the start of #17).
    if (barkskin)
        return health < sPlayerbotAIConfig.MediumHealth || recentLoss >= sPlayerbotAIConfig.TankCooldownLossPct
                   ? 1.0f
                   : 0.0f;

    if (health < BEAR_TRASH_LAST_STAND_PCT)
        return 1.0f;

    if (health < BEAR_TRASH_COOLDOWN_PCT && recentLoss >= sPlayerbotAIConfig.TankCooldownLossPct)
        return 1.0f;

    return 0.0f;
}

void BearDruidStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    FeralDruidStrategy::InitMultipliers(multipliers);
    multipliers.push_back(new RezzedTankHoldMultiplier(botAI));
    multipliers.push_back(new BearCooldownPacingMultiplier(botAI));  // [rot_b31_pdd]
}

void BearDruidStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    FeralDruidStrategy::InitTriggers(triggers);
    // [lead96] a mob coming at the group with no tank threat: the ranged opener on it (TankOpenIncomingAction)
    // [lead106] above the tank's own target / facing / marks actions (tank assist 40, set facing 37): taking a
    // mob back from another member is the most urgent thing a tank does
    triggers.push_back(
        new TriggerNode("tank incoming mob", { NextAction("tank open incoming", ACTION_INTERRUPT + 1.0f) }));

    triggers.push_back(
        new TriggerNode(
            "bear form",
            { NextAction("dire bear form", 28.0f) }
        )
    );
    // [b32_brez] Last-resort battle res (BearBattleRes, DruidTriggers.h): the healer (else a tank / real player) dead,
    // no other druid able to do it, the bear healthy with its pack held. Below Survival Instincts in "tank pressure"
    // (ACTION_EMERGENCY + 1), above Barkskin there and every peel / threat step (the action leaves form itself).
    triggers.push_back(new TriggerNode("bear battle res", { NextAction("bear battle res", ACTION_EMERGENCY + 0.5f) }));
    // [b32_brez] Out of form after that attempt: Dire Bear Form above everything (the plain "bear form" node at 28
    // loses to the peel and movement steps, which would run in caster form).
    triggers.push_back(new TriggerNode("bear battle res reshift",
        {
            NextAction("dire bear form", ACTION_EMERGENCY + 5),
            NextAction("bear form",      ACTION_EMERGENCY + 4.9f)
        }));
    triggers.push_back(
        new TriggerNode(
            "medium health",
            {
                // [b31_sd] Enrage left this node: at 65 % its armor loss lands on a falling tank. It stays the low
                // default (5.1), gated by CastEnrageAction::isUseful() (low rage, healthy bear).
                NextAction("frenzied regeneration", 27.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "damage spike",
            {
                NextAction("frenzied regeneration", 27.0f)
            }
        )
    );
    triggers.push_back(new TriggerNode(
        "mangle (bear)", { NextAction("mangle (bear)", 17.5f) }
    ));
    triggers.push_back(new TriggerNode(
        "faerie fire (feral)", { NextAction("faerie fire (feral)", 17.0f) }
    ));
    triggers.push_back(new TriggerNode(
        "lacerate", { NextAction("lacerate", 16.0f) }
    ));
    // [rot31] wowsims feral tank: the Demoralizing Roar refresh above Mangle
    triggers.push_back(new TriggerNode(
        "demoralizing roar", { NextAction("demoralizing roar", 17.6f) }
    ));
    // [b31_deep] Challenging Roar left "high aoe" (any 4 mobs in 8 yd, 26.5): on a fresh pull it went out before
    // any threat existed and the mobs left after its 6 s. It stays an emergency for loose mobs on group members
    // ("mobs attacking group members nearby", below).
    triggers.push_back(new TriggerNode("lose aggro",
        {
            NextAction("growl",               26.0f),
            NextAction("faerie fire (feral)", 25.5f)
        }
    ));
    // Peel: take back a mob hitting the healer or a damage dealer, also when it is not the current target.
    // Above "set facing" (ACTION_MOVE + 7) and "tank face" (ACTION_MOVE), as the warrior's taunt; Swipe first,
    // Challenging Roar when it is not possible.
    triggers.push_back(new TriggerNode("mob attacking group member",
        {
            NextAction("growl on group member attacker", ACTION_INTERRUPT + 0.5f),
            // Growl on cooldown: Feral Charge puts the bear on the mob (8-25 yd), Swipe and Maul take it then.
            // [b32_peel] only onto a mob next to the member it hits (TankPeel::MayCloseGap), not a ranged caster
            NextAction("feral charge - bear on group member attacker", ACTION_INTERRUPT + 0.4f),
            // a loose mob in melee (any, not only the pick): Mangle's threat by hand
            NextAction("mangle (bear) on group member attacker", ACTION_INTERRUPT + 0.35f),
            // Growl on cooldown: Faerie Fire (Feral) is threat at range
            NextAction("faerie fire (feral) on group member attacker", ACTION_INTERRUPT + 0.3f)
        }));
    // A pack around the bear with a mob held by proximity aggro alone (the first heal takes it) or on a member: Swipe
    // as the pull lands and while one is loose. Below the peel steps, above "tank assist".
    triggers.push_back(new TriggerNode("tank aoe threat",
        {
            NextAction("swipe (bear) tank aoe", ACTION_INTERRUPT + 0.2f),
            NextAction("demoralizing roar", ACTION_INTERRUPT + 0.15f)
        }));
    // [b32_tank_aoe] "tank aoe pack" (TankAoeThreat.h): 3+ mobs in reach and one without a safe threat lead -> the area
    // spell above the single-target builders until the lead is safe; then the single-target order below resumes.
    // Above Mangle (17.5; Berserk 25 stays above), Faerie Fire (17), Lacerate (16); below the peel, "tank aoe threat".
    triggers.push_back(new TriggerNode("tank aoe pack", { NextAction("swipe (bear) tank aoe", 17.8f) }));
    // [b32_peel] "tank aoe crowd" (TankAoeThreat.h): more than 3 mobs in reach, one not held -> Swipe above every
    // peel step (+0.3..+0.5) and "taunt reaction" (TankPeel::CrowdFirst); off while a mob is on the healer.
    triggers.push_back(
        new TriggerNode("tank aoe crowd", { NextAction("swipe (bear) tank aoe", ACTION_INTERRUPT + 0.6f) }));
    // One mob within Swipe's reach is held by it while Growl cools down (two or more: below).
    triggers.push_back(new TriggerNode("mob attacking group member nearby",
        { NextAction("swipe (bear) on group member attackers", ACTION_MOVE + 2.5f) }));
    triggers.push_back(new TriggerNode("group member attacker near",
        { NextAction("reach group member attacker", ACTION_MOVE + 0.9f) }));
    // Falling health under a hard pull: the bear's cooldowns early, not only on a 30% spike.
    triggers.push_back(new TriggerNode("tank pressure",
        {
            NextAction("survival instincts", ACTION_EMERGENCY + 1),
            NextAction("barkskin", ACTION_EMERGENCY)
        }));
    triggers.push_back(new TriggerNode("mobs attacking group members nearby",
        {
            NextAction("swipe (bear) on group member attackers", ACTION_MOVE + 2.5f),
            NextAction("challenging roar", ACTION_MOVE + 2.0f)
        }
    ));
    // Two or more adds out of reach of the area threat: step towards them (what the tank holds follows) and take
    // them with the AoE of "mobs attacking group members nearby"; reach melee brings the tank back afterwards.
    triggers.push_back(new TriggerNode("group member attackers far",
        { NextAction("move to group member attackers", ACTION_MOVE + 1) }));
    // Above the rotation and "tank face" (ACTION_MOVE), below the peel steps and area threat (the feral 21 lost
    // to every ranged and self spell, and the tank stood out of melee).
    triggers.push_back(new TriggerNode("enemy out of melee", { NextAction("reach melee", ACTION_MOVE + 0.5f) }));
    triggers.push_back(new TriggerNode("berserk active", { NextAction("mangle (bear)", 25.0f) }));
    // [b31_sd] Swipe ran at 24 from "light aoe" (2 mobs), above Mangle (17.5) and Faerie Fire (17): on bosses FF got
    // 5.5/min of its ~10, and on trash Swipe outnumbered Mangle (GUIDE_AUDIT_TANKS.md, bear). Mangle stays first on
    // its cooldown: on 3+ mobs Swipe comes right after it (above FF and Lacerate), on 2 it is the filler below
    // Demoralizing Roar (15.5). The loose-mob threat steps above ("tank aoe threat", peel) keep their own Swipe.
    // [b36_tankaoe] 17.3 -> 17.6, above Mangle (17.5): on 3+ mobs (now counted round the bear, AoeTrigger) Swipe's
    // threat on every mob outweighs Mangle's on one (3.3.5 bear AoE: Swipe, then Mangle; Maul stays off the global
    // cooldown). Build 35 L80, 5+ kill pulls: Swipe 12.2 and Mangle 9.8 a minute. Two mobs keep Mangle first.
    triggers.push_back(
        new TriggerNode(
            "medium aoe",
            {
                NextAction("demoralizing roar", 24.5f),
                NextAction("swipe (bear)",      17.6f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "light aoe",
            { NextAction("swipe (bear)", 15.4f) }
        )
    );
    // [b31_deep] wowsims feral tank APL: Swipe on a single target at 40+ rage, once Lacerate holds 5 stacks (the
    // Lacerate node above still refreshes it first). The lowest rotation step, below Demoralizing Roar.
    triggers.push_back(new TriggerNode("swipe (bear) rage dump", { NextAction("swipe (bear)", 15.3f) }));
    triggers.push_back(
        new TriggerNode(
            "bash",
            { NextAction("bash", 42.0f) }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "bash on enemy healer",
            { NextAction("bash on enemy healer", 41.0f) }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus survival instincts tank",
        { NextAction("survival instincts", 25.8f) }));
    triggers.push_back(new TriggerNode("set bonus ascendant frenzy maul", { NextAction("maul", 18.0f) }));
    triggers.push_back(new TriggerNode("set bonus clearcasting maul", { NextAction("maul", 17.9f) }));
}
