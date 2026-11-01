/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_WARRIORACTIONS_H
#define PLAYERBOTS_WARRIORACTIONS_H

#include "AiObject.h"
#include "GenericSpellActions.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "ReachTargetActions.h"
#include "TankAoeThreat.h"
#include "WarriorTriggers.h"  // [b31_deep] WARRIOR_SHOUT_REFRESH_MS, WarriorRage

// stances
BUFF_ACTION(CastBattleStanceAction, "battle stance");
BUFF_ACTION(CastDefensiveStanceAction, "defensive stance");
BUFF_ACTION(CastBerserkerStanceAction, "berserker stance");

// shouts
// [b31_deep] refreshed WARRIOR_SHOUT_REFRESH_MS before it runs out, as its trigger
class CastBattleShoutAction : public CastBuffSpellAction
{
public:
    CastBattleShoutAction(PlayerbotAI* botAI)
        : CastBuffSpellAction(botAI, "battle shout", false, WARRIOR_SHOUT_REFRESH_MS) {}
};
MELEE_ACTION_U(CastBattleShoutTauntAction, "battle shout", CastSpellAction::isUseful());  // useful to rebuff
// DEBUFF_ACTION_R(CastDemoralizingShoutAction, "demoralizing shout", 8.0f);                 // low range debuff

// [rot43] The attack-power reduction slot of the target is already held by another source (Demoralizing Roar, Curse of
// Weakness, Vindication, a hunter pet's Demoralizing Screech): Demoralizing Shout would not land.
bool WarriorDemoSlotCovered(PlayerbotAI* botAI, Unit* target);

class CastDemoralizingShoutAction : public CastMeleeDebuffSpellAction
{
public:
    CastDemoralizingShoutAction(PlayerbotAI* botAI) : CastMeleeDebuffSpellAction(botAI, "demoralizing shout") {}
    bool isUseful() override;  // [rot43] not over a covered slot
};

class CastDemoralizingShoutWithoutLifeTimeCheckAction : public CastMeleeDebuffSpellAction
{
public:
    CastDemoralizingShoutWithoutLifeTimeCheckAction(PlayerbotAI* botAI)
        : CastMeleeDebuffSpellAction(botAI, "demoralizing shout", false, 0.0f)
    {
    }

    // In a group the AoE debuff is the tank's job; a dps warrior would only spend GCDs on it.
    bool isUseful() override
    {
        return (!bot->GetGroup() || PlayerbotAI::IsTank(bot)) && CastMeleeDebuffSpellAction::isUseful() &&
               !WarriorDemoSlotCovered(botAI, GetTarget());  // [rot43]
    }
};

// The tank's opener on a pack ("tank aoe threat"): around the tank, not only the current target. The shout's own
// threat is small; it waits for Thunder Clap unless that is on cooldown or the rage covers both.
class CastDemoralizingShoutTankAoeAction : public CastTankAreaThreatAction
{
public:
    CastDemoralizingShoutTankAoeAction(PlayerbotAI* botAI) : CastTankAreaThreatAction(botAI, "demoralizing shout") {}

    bool isUseful() override;
};

MELEE_ACTION(CastChallengingShoutAction, "challenging shout");
DEBUFF_ACTION_R(CastIntimidatingShoutAction, "intimidating shout", 8.0f);
// shouts 2.4.3
class CastCommandingShoutAction : public CastBuffSpellAction  // [b31_deep] refresh window
{
public:
    CastCommandingShoutAction(PlayerbotAI* botAI)
        : CastBuffSpellAction(botAI, "commanding shout", false, WARRIOR_SHOUT_REFRESH_MS) {}
};

// arms
MELEE_ACTION(CastHeroicStrikeAction, "heroic strike");
// A gap closer that also needs the target within its maximum range: out of range it passes CanCastSpell, and its
// stance prerequisite then flipped the stance back and forth on every tick of the walk in.
class CastWarriorGapCloserAction : public CastReachTargetSpellAction
{
public:
    CastWarriorGapCloserAction(PlayerbotAI* botAI, std::string const spell)
        : CastReachTargetSpellAction(botAI, spell, 8.0f)
    {
    }

    bool isUseful() override;
};

// Charge needs Battle Stance (without Warbringer). In Defensive Stance CanCastSpell fails (CheckCast ONLY_SHAPESHIFT),
// and the engine pushes an action's prerequisites only once it is possible: the "battle stance" prerequisite never
// ran and the tank walked in (wave 44, RFC 5B-1 pull 33, "charge" planned for a prot tank). Possible here when the
// stance is all that is missing, as a real warrior dances: Battle Stance, Charge, then "defensive stance" back.
class CastChargeAction : public CastWarriorGapCloserAction
{
public:
    CastChargeAction(PlayerbotAI* botAI) : CastWarriorGapCloserAction(botAI, "charge") {}

    bool isPossible() override;

private:
    uint32 _stanceDanceMs = 0;  // when the last stance dance for Charge began (a failed one is not retried at once)
};
DEBUFF_CHECKISOWNER_ACTION(CastRendAction, "rend");
// DEBUFF_ENEMY_ACTION(CastRendOnAttackerAction, "rend");

class CastRendOnAttackerAction : public CastDebuffSpellOnMeleeAttackerAction
{
public:
    CastRendOnAttackerAction(PlayerbotAI* botAI) : CastDebuffSpellOnMeleeAttackerAction(botAI, "rend") {}
};

MELEE_ACTION(CastThunderClapAction, "thunder clap");
SNARE_ACTION(CastThunderClapSnareAction, "thunder clap");
SNARE_ACTION(CastHamstringAction, "hamstring");
MELEE_ACTION(CastOverpowerAction, "overpower");
MELEE_ACTION(CastMockingBlowAction, "mocking blow");
// BUFF_ACTION(CastRetaliationAction, "retaliation");
// arms 3.3.5
SPELL_ACTION(CastHeroicThrowAction, "heroic throw");
SNARE_ACTION(CastHeroicThrowSnareAction, "heroic throw");
// DEBUFF_ACTION(CastShatteringThrowAction, "shattering throw");

// arms talents
MELEE_ACTION(CastMortalStrikeAction, "mortal strike");
BUFF_ACTION(CastSweepingStrikesAction, "sweeping strikes");
// arms talents 3.3.5
MELEE_ACTION(CastBladestormAction, "bladestorm");

// fury
MELEE_ACTION(CastCleaveAction, "cleave");
MELEE_ACTION(CastExecuteAction, "execute");
class CastInterceptAction : public CastWarriorGapCloserAction
{
public:
    CastInterceptAction(PlayerbotAI* botAI) : CastWarriorGapCloserAction(botAI, "intercept") {}
};
ENEMY_HEALER_ACTION(CastInterceptOnEnemyHealerAction, "intercept");
SNARE_ACTION(CastInterceptOnSnareTargetAction, "intercept");
MELEE_ACTION(CastSlamAction, "slam");

// [b31_deep] Arms filler Slam: not with the rage Mortal Strike needs. Only with 45 rage (Mortal Strike 30 plus this
// one 15) or while Mortal Strike is more than 1.5 s from ready. It used to share a node with Mortal Strike on 20
// rage: Mortal Strike short of rage failed and Slam spent the rest (build 30: Mortal Strike 5.1 a minute of ~10).
class CastSlamFillerAction : public CastSlamAction
{
public:
    CastSlamFillerAction(PlayerbotAI* botAI) : CastSlamAction(botAI) {}

    std::string const getName() override { return "slam filler"; }
    bool isUseful() override;
};

class CastBerserkerRageAction : public CastSpellAction
{
public:
    CastBerserkerRageAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "berserker rage") {}

    std::string const GetTargetName() override { return "self target"; }
    bool isPossible() override;
    bool isUseful() override;
    bool Execute(Event event) override;  // [fear_counter] logs the fear / sleep break
};

// [b31_deep] Enraged Regeneration needs an enrage effect (Spell.dbc 55694 CasterAuraState 17, AURA_STATE_ENRAGE) and
// no strategy made one: ER was 0 for Fury and Prot in every log. Berserker Rage (any stance in 3.3.5) enrages first,
// Bloodrage when Berserker Rage is not there; both only while Enraged Regeneration is ready.
namespace WarriorEnrage
{
    bool EnragedRegenerationWaitsForEnrage(PlayerbotAI* botAI);
}

class CastBerserkerRageForEnrageAction : public CastBerserkerRageAction
{
public:
    CastBerserkerRageForEnrageAction(PlayerbotAI* botAI) : CastBerserkerRageAction(botAI) {}

    std::string const getName() override { return "berserker rage for enrage"; }
    bool isUseful() override;
};
MELEE_ACTION(CastWhirlwindAction, "whirlwind");
MELEE_ACTION(CastPummelAction, "pummel");
ENEMY_HEALER_ACTION(CastPummelOnEnemyHealerAction, "pummel");
// fury 2.4.3
MELEE_ACTION(CastVictoryRushAction, "victory rush");
// fury 3.3.5
BUFF_ACTION(CastEnragedRegenerationAction, "enraged regeneration");
BUFF_ACTION(CastHeroicFuryAction, "heroic fury");

// fury talents
BUFF_ACTION(CastDeathWishAction, "death wish");
BUFF_ACTION(CastRecklessnessAction, "recklessness");
MELEE_ACTION(CastBloodthirstAction, "bloodthirst");
DEBUFF_ACTION_R(CastPiercingHowlAction, "piercing howl", 8.0f);
// fury talents 2.4.3
BUFF_ACTION(CastRampageAction, "rampage");

// protection
MELEE_ACTION_U(CastTauntAction, "taunt", GetTarget() && GetTarget()->GetTarget() != bot->GetGUID());
SNARE_ACTION(CastTauntOnSnareTargetAction, "taunt");
BUFF_ACTION(CastBloodrageAction, "bloodrage");

// [b31_deep] Bloodrage as the enrage for Enraged Regeneration when Berserker Rage is unknown or on cooldown; no
// health gate (the "bloodrage" trigger skips it below 65 % health, where Enraged Regeneration is wanted).
class CastBloodrageForEnrageAction : public CastBloodrageAction
{
public:
    CastBloodrageForEnrageAction(PlayerbotAI* botAI) : CastBloodrageAction(botAI) {}

    std::string const getName() override { return "bloodrage for enrage"; }
    bool isUseful() override;
};
MELEE_ACTION(CastShieldBashAction, "shield bash");
ENEMY_HEALER_ACTION(CastShieldBashOnEnemyHealerAction, "shield bash");
MELEE_ACTION(CastRevengeAction, "revenge");
BUFF_ACTION(CastShieldBlockAction, "shield block");
DEBUFF_ACTION_U(CastDisarmAction, "disarm",
                GetTarget() && GetTarget()->IsPlayer() ? !PlayerbotAI::IsRanged((Player*)GetTarget())
                                                       : CastDebuffSpellAction::isUseful());
DEBUFF_ENEMY_ACTION(CastDisarmOnAttackerAction, "disarm");
BUFF_ACTION(CastShieldWallAction, "shield wall");
// protection 2.4.3
PROTECT_ACTION(CastInterveneAction, "intervene");

// Peel: Intervene to the ally a mob is hitting. It takes one hit and puts the tank next to the mob, but does not
// hold it, so the mob is not marked as peeled: the taunts and Mocking Blow still go to it.
class CastInterveneOnAttackedGroupMemberAction : public CastSpellAction
{
public:
    CastInterveneOnAttackedGroupMemberAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "intervene") {}

    std::string const getName() override { return "intervene on attacked group member"; }
    Unit* GetTarget() override;
    bool isUseful() override;
};
BUFF_ACTION(CastSpellReflectionAction, "spell reflection");

// protection talents
BUFF_ACTION(CastLastStandAction, "last stand");
MELEE_ACTION(CastShieldSlamAction, "shield slam");
MELEE_ACTION(CastConcussionBlowAction, "concussion blow");
// protection talents 2.4.3
MELEE_ACTION(CastDevastateAction, "devastate");
// protection talents 3.3.5
// [b36_tankaoe] a plain spell, not a debuff: the debuff checks (Shockwave's stun already on the target, the target's
// 8 s lifetime) refused the cone on the pack, Build 35 L80 3+ mobs: "shockwave ... isUseful false" 254 times against
// 39 on cooldown, 0.87 a minute of its 3. Its threat and damage hit every mob in the cone.
class CastShockwaveAction : public CastSpellAction
{
public:
    CastShockwaveAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "shockwave") { range = 8.0f; }
};
SNARE_ACTION(CastShockwaveSnareAction, "shockwave");

class CastSunderArmorAction : public CastDebuffSpellAction
{
public:
    CastSunderArmorAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "sunder armor")
    {
        range = ATTACK_DISTANCE;
    }

    bool isUseful() override;
};

class CastVigilanceAction : public CastBuffSpellAction
{
public:
    CastVigilanceAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "vigilance") {}

    Unit* GetTarget() override;
};

class CastRetaliationAction : public CastBuffSpellAction
{
public:
    CastRetaliationAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "retaliation") {}

    bool isUseful() override;
};

class CastShatteringThrowAction : public CastSpellAction
{
public:
    CastShatteringThrowAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "shattering throw") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
    Unit* GetTarget() override;
};

// [b31_deep] Shattering Throw on the current target as a boss cooldown ("shattering throw boss" trigger); the action
// above only ever takes an immune target.
class CastShatteringThrowBossAction : public CastSpellAction
{
public:
    CastShatteringThrowBossAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "shattering throw") {}

    std::string const getName() override { return "shattering throw boss"; }
    bool isUseful() override;
};

// [rot19] Rage (x10) left after `spell` for Mortal Strike / Bloodthirst / Whirlwind coming off cooldown within a GCD
bool WarriorLeavesRageForStrike(PlayerbotAI* botAI, Player* bot, char const* spell);

#endif
