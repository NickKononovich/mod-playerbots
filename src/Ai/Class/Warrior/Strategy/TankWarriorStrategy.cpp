/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TankWarriorStrategy.h"

#include "Player.h"
#include "PlayerbotAI.h"
#include "RezzedTankHoldMultiplier.h"

namespace
{
    // Charge, Intercept and Intervene in combat and in any stance
    constexpr uint32 SPELL_WARBRINGER = 57499;
}

class TankWarriorStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    TankWarriorStrategyActionNodeFactory()
    {
        creators["charge"] = &charge;
        creators["sunder armor"] = &sunder_armor;
        creators["devastate"] = &devastate;
        creators["last stand"] = &last_stand;
        creators["heroic throw on snare target"] = &heroic_throw_on_snare_target;
        creators["heroic throw taunt"] = &heroic_throw_taunt;
        creators["taunt"] = &taunt;
        creators["taunt spell"] = &taunt;
        creators["mocking blow"] = &mocking_blow;
    }

private:
    static ActionNode* heroic_throw_taunt(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "heroic throw",
            /*P*/ {},
            /*A*/ { NextAction("shield slam") },
            /*C*/ {}
        );
    }

    static ActionNode* heroic_throw_on_snare_target(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "heroic throw on snare target",
            /*P*/ {},
            /*A*/ { NextAction("taunt on snare target") },
            /*C*/ {}
        );
    }

    static ActionNode* last_stand(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "last stand",
            /*P*/ {},
            /*A*/ {},
            /*C*/ {}
        );
    }

    static ActionNode* devastate(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "devastate",
            /*P*/ {},
            /*A*/ { NextAction("sunder armor") },
            /*C*/ {}
        );
    }

    static ActionNode* sunder_armor(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "sunder armor",
            /*P*/ {},
            /*A*/ { NextAction("melee") },
            /*C*/ {}
        );
    }

    // Without Warbringer the opener needs Battle Stance ("defensive stance" takes the tank back after it) and
    // only works out of combat; with it Intercept follows when Charge is on cooldown. No "reach melee"
    // alternative: Heroic Throw below it takes a target Charge cannot reach.
    static ActionNode* charge(PlayerbotAI* botAI)
    {
        Player* bot = botAI ? botAI->GetBot() : nullptr;
        bool const warbringer = bot && bot->HasAura(SPELL_WARBRINGER);
        std::vector<NextAction> prerequisites;
        if (bot && !warbringer && !bot->IsInCombat())
            prerequisites.push_back(NextAction("battle stance"));

        std::vector<NextAction> alternatives;
        if (warbringer)
            alternatives.push_back(NextAction("intercept"));

        return new ActionNode("charge", prerequisites, alternatives, {});
    }

    static ActionNode* taunt(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "taunt",
            /*P*/ {},
            // Both listed: a useless (out of melee) Mocking Blow does not push its own alternatives.
            /*A*/ { NextAction("mocking blow"), NextAction("heroic throw taunt") },
            /*C*/ {}
        );
    }

    // 3.3.5 Mocking Blow is usable in Defensive Stance (Spell.dbc stance mask battle | defensive).
    static ActionNode* mocking_blow(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode(
            "mocking blow",
            /*P*/ {},
            /*A*/ { NextAction("heroic throw taunt") },
            /*C*/ {}
        );
    }
};

TankWarriorStrategy::TankWarriorStrategy(PlayerbotAI* botAI) : GenericWarriorStrategy(botAI)
{
    actionNodeFactories.Add(new TankWarriorStrategyActionNodeFactory());
}

std::vector<NextAction> TankWarriorStrategy::getDefaultActions()
{
    return {
        NextAction("devastate", ACTION_DEFAULT + 0.3f),
        NextAction("revenge", ACTION_DEFAULT + 0.2f),
        NextAction("demoralizing shout", ACTION_DEFAULT + 0.1f),
        NextAction("melee", ACTION_DEFAULT)
    };
}

void TankWarriorStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    GenericWarriorStrategy::InitMultipliers(multipliers);
    multipliers.push_back(new RezzedTankHoldMultiplier(botAI));
}

void TankWarriorStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericWarriorStrategy::InitTriggers(triggers);
    // [lead96] a mob coming at the group with no tank threat: the ranged opener on it (TankOpenIncomingAction)
    // [lead106] above the tank's own target / facing / marks actions (tank assist 40, set facing 37): taking a
    // mob back from another member is the most urgent thing a tank does
    triggers.push_back(
        new TriggerNode("tank incoming mob", { NextAction("tank open incoming", ACTION_INTERRUPT + 1.0f) }));

    triggers.push_back(
        new TriggerNode(
            "vigilance",
            {
                NextAction("vigilance", ACTION_HIGH + 7)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "enemy out of melee",
            {
                // Charge first (rage, a stun, no walk); Heroic Throw when Charge is not possible
                NextAction("charge", ACTION_MOVE + 11),
                NextAction("heroic throw", ACTION_MOVE + 10),
                // above the rotation and "tank face" (ACTION_MOVE), below the peel steps and area threat
                NextAction("reach melee", ACTION_MOVE + 0.5f)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "thunder clap and rage",
            {
                // [b31_deep] wowsims prot: Thunder Clap upkeep after Shield Slam / Revenge, before Devastate. It was
                // ACTION_MOVE + 11, level with Taunt and Shield Block; now between Revenge (+4) and Shockwave (+3.6).
                NextAction("thunder clap", ACTION_HIGH + 3.8f)
            }
        )
    );
    // [b31_deep] wowsims prot / Warcraft Tavern: Demoralizing Shout kept up (it was only a default action)
    triggers.push_back(
        new TriggerNode(
            "demoralizing shout",
            {
                NextAction("demoralizing shout", ACTION_HIGH + 2.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "defensive stance",
            {
                NextAction("defensive stance", ACTION_HIGH + 9)
            }
        )
    );
    // [b31_wr] Commanding Shout once learned (68), Battle Shout before: the "battle shout" alternative of Commanding
    // Shout fired on every tick while Commanding Shout was unknown (L60-67: Battle Shout 14 a minute).
    // [b36_tankaoe] ACTION_HIGH + 8 -> + 2.2: the missing shout went out above Thunder Clap ("tank aoe pack" 25.5,
    // upkeep 23.8), Shield Slam (25), Revenge (24) and Shockwave on the first global cooldowns of a pull. Now below
    // every threat builder, above the Devastate sunder upkeep (22) and Heroic Strike (20).
    triggers.push_back(
        new TriggerNode(
            "commanding shout",
            {
                NextAction("commanding shout", ACTION_HIGH + 2.2f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "battle shout no commanding shout",
            {
                NextAction("battle shout", ACTION_HIGH + 2.2f)
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
            "sunder armor",
            {
                NextAction("devastate", ACTION_HIGH + 2)
            }
        )
    );
    // [b31_wr] 3.3.5 prot single target: Shield Slam > Revenge > Shockwave > Concussion Blow > Devastate, each on
    // cooldown (Shield Slam was gated on 40 rage: 7.2 a minute against 10+). Heroic Strike / Cleave keep the rage gate.
    triggers.push_back(
        new TriggerNode(
            "shield slam ready",
            {
                NextAction("shield slam", ACTION_HIGH + 5)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "shockwave ready",
            {
                NextAction("shockwave", ACTION_HIGH + 3.6f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "concussion blow ready",
            {
                NextAction("concussion blow", ACTION_HIGH + 3.4f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "medium rage available",
            {
                NextAction("devastate", ACTION_HIGH + 1),
                // [b31_deep] Heroic Strike from 40 rage (wowsims prot: 30), was 60; off the global cooldown
                NextAction("heroic strike", ACTION_HIGH)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "shield block",
            {
                NextAction("shield block", ACTION_INTERRUPT + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "revenge",
            {
                NextAction("revenge", ACTION_HIGH + 4)  // [b31_wr] on every proc, below Shield Slam
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "disarm",
            {
                NextAction("disarm", ACTION_HIGH + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "lose aggro",
            {
                NextAction("taunt", ACTION_INTERRUPT + 1)
            }
        )
    );
    // Peel: take back a mob hitting the healer or a damage dealer, also when it is not the current target.
    triggers.push_back(
        new TriggerNode(
            "mob attacking group member",
            {
                NextAction("taunt on group member attacker", ACTION_INTERRUPT + 0.5f),
                // Taunt on cooldown and the ally out of reach: Intervene takes the next hit and puts the tank there
                NextAction("intervene on attacked group member", ACTION_INTERRUPT + 0.45f),
                // Taunt on cooldown: Mocking Blow forces the mob for 6 s, Heroic Throw is high threat at range
                NextAction("mocking blow on group member attacker", ACTION_INTERRUPT + 0.4f),
                NextAction("heroic throw on group member attacker", ACTION_INTERRUPT + 0.3f),
                // a loose mob in melee (any, not only the pick): Shield Slam's threat by hand
                NextAction("shield slam on group member attacker", ACTION_INTERRUPT + 0.25f)
            }
        )
    );
    // A pack around the tank with a mob held by proximity aggro alone (the first heal takes it) or on a member: area
    // threat as the pull lands and on cooldown while one is loose. Below the peel steps, above "tank assist".
    triggers.push_back(
        new TriggerNode(
            "tank aoe threat",
            {
                NextAction("thunder clap tank aoe", ACTION_INTERRUPT + 0.2f),
                NextAction("demoralizing shout tank aoe", ACTION_INTERRUPT + 0.15f)
            }
        )
    );
    // [b32_tank_aoe] "tank aoe pack" (TankAoeThreat.h): 3+ mobs in reach and one without a safe threat lead -> the area
    // spell above the single-target builders until the lead is safe; then the single-target order below resumes.
    // Above Shield Slam (ACTION_HIGH + 5) and Revenge (+4); below the stance, shouts and the loose-mob steps.
    triggers.push_back(new TriggerNode("tank aoe pack", { NextAction("thunder clap tank aoe", ACTION_HIGH + 5.5f) }));
    // [b32_peel] "tank aoe crowd" (TankAoeThreat.h): more than 3 mobs in reach, one not held -> Thunder Clap above
    // every peel step (+0.25..+0.5) and "taunt reaction" (TankPeel::CrowdFirst); off while a mob is on the healer.
    triggers.push_back(
        new TriggerNode("tank aoe crowd", { NextAction("thunder clap tank aoe", ACTION_INTERRUPT + 0.6f) }));
    // Off the global cooldown: rage for Thunder Clap while the pulled pack runs in.
    triggers.push_back(
        new TriggerNode(
            "bloodrage low rage",
            {
                NextAction("bloodrage", ACTION_INTERRUPT + 0.47f)
            }
        )
    );
    // One mob within Thunder Clap's reach is held by it while Taunt cools down (two or more: below).
    triggers.push_back(
        new TriggerNode(
            "mob attacking group member nearby",
            {
                NextAction("thunder clap on group member attackers", ACTION_MOVE + 2.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "group member attacker near",
            {
                NextAction("reach group member attacker", ACTION_MOVE + 0.9f)
            }
        )
    );
    // Thunder Clap first; Challenging Shout when it is not ready. Above "tank face" (ACTION_MOVE).
    triggers.push_back(
        new TriggerNode(
            "mobs attacking group members nearby",
            {
                NextAction("thunder clap on group member attackers", ACTION_MOVE + 2.5f),
                NextAction("challenging shout", ACTION_MOVE + 2)
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
            "taunt on snare target",
            {
                NextAction("heroic throw on snare target", ACTION_INTERRUPT)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "damage spike",
            {
                NextAction("shield wall", ACTION_EMERGENCY + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "critical health",
            {
                NextAction("last stand", ACTION_EMERGENCY + 3),
                // [b31_deep] the enrage Enraged Regeneration needs (a tank has none: ER 0.00-0.01 a minute)
                NextAction("berserker rage for enrage", ACTION_EMERGENCY + 2.5f),
                NextAction("bloodrage for enrage", ACTION_EMERGENCY + 2.3f),
                NextAction("enraged regeneration", ACTION_EMERGENCY + 2)
            }
        )
    );
    // [b31_deep] Last Stand from low health (45 %), before Shield Wall's spike / pressure triggers: on 25 % only it
    // was cast 0 times on L80 bosses
    triggers.push_back(
        new TriggerNode(
            "low health",
            {
                NextAction("last stand", ACTION_EMERGENCY + 3)
            }
        )
    );
    // Falling health under a hard pull: Shield Wall early, not only on a 30% spike.
    triggers.push_back(
        new TriggerNode(
            "tank pressure",
            {
                NextAction("shield wall", ACTION_EMERGENCY + 1)
            }
        )
    );
    // [b36_tankaoe] Challenging Shout left "high aoe" (any 4 mobs in 8 yd, 23), as the bear's Challenging Roar did in
    // [b31_deep]: on a fresh pull it went out before any threat existed and the mobs left after its 6 s. It stays the
    // emergency for loose mobs on group members ("mobs attacking group members nearby", below the peel).
    triggers.push_back(
        new TriggerNode(
            "concussion blow",
            {
                NextAction("concussion blow", ACTION_INTERRUPT)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "shield bash",
            {
                NextAction("shield bash", ACTION_INTERRUPT)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "shield bash on enemy healer",
            {
                NextAction("shield bash on enemy healer", ACTION_INTERRUPT)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "spell reflection",
            {
                NextAction("spell reflection", ACTION_INTERRUPT + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "victory rush",
            {
                // [b31_deep] filler (was ACTION_INTERRUPT, above the whole rotation); urgent below medium health
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
    triggers.push_back(
        new TriggerNode(
            "sword and board",
            {
                NextAction("shield slam", ACTION_INTERRUPT)
            }
        )
    );
    // [b31_wr] No Rend for a tank (3.3.5 prot guides): it took 1.7 GCDs a minute on bosses, 3-4 on L15-45.
    triggers.push_back(
        new TriggerNode(
            "protect party member",
            {
                NextAction("intervene", ACTION_EMERGENCY)
            }
        )
    );
    // [b31_deep] the "high rage available" Heroic Strike moved to "medium rage available" above
    // [b31_wr] Thunder Clap only to keep its debuff up ("thunder clap and rage") or on 2+ mobs (aoe strategy, "tank
    // aoe threat"); the extra single-target one on 40 rage is gone.

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus shield wall tank", { NextAction("shield wall", ACTION_HIGH + 8) }));
    triggers.push_back(new TriggerNode("set bonus last stand tank", { NextAction("last stand", ACTION_HIGH + 8) }));
    triggers.push_back(new TriggerNode("set bonus revenge", { NextAction("revenge", ACTION_HIGH + 2.5f) }));
}
