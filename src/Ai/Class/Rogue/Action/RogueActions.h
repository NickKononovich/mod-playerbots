/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROGUEACTIONS_H
#define PLAYERBOTS_ROGUEACTIONS_H

#include "AiObjectContext.h"
#include "GenericSpellActions.h"
#include "RogueFinishingActions.h"
#include "UseItemAction.h"

class PlayerbotAI;

class CastEvasionAction : public CastBuffSpellAction
{
public:
    CastEvasionAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "evasion") {}
};

class CastCloakOfShadowsAction : public CastBuffSpellAction
{
public:
    CastCloakOfShadowsAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "cloak of shadows") {}
};

// [b31_wr] Hunger for Blood (51662) is cast on the ENEMY: Spell.dbc ImplicitTargetA TARGET_UNIT_TARGET_ENEMY and
// TargetAuraState AURA_STATE_BLEEDING; its script effect then puts the self buff (63848, 60 s) on the rogue. So the
// target stays "current target" (cast on self it fails CheckCast); usefulness reads the self buff, refreshed below
// HUNGER_FOR_BLOOD_REFRESH_MS (RogueFinishingActions.h) like the trigger.
// [rot60] a spell on the enemy, not a buff action: the led member's trigger gate takes a damage dealer's buff action
// aimed at anything but itself for a buff on another member (RaidLeader::GateVetoes, OwnTargetName), so b201 rfc-h
// 5E Rtrogab cast Hunger for Blood 0 times in 41 pulls with the target bleeding (wowsims mutilate: kept up, 1.8/min)
class CastHungerForBloodAction : public CastSpellAction
{
public:
    CastHungerForBloodAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "hunger for blood") {}

    bool isUseful() override;
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

// [b31_wr] Subtlety: 2 combo points on the target, off the global cooldown; needs Stealth or Shadow Dance
class CastPremeditationAction : public CastSpellAction
{
public:
    CastPremeditationAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "premeditation") {}
};

class CastSprintAction : public CastBuffSpellAction
{
public:
    CastSprintAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "sprint") {}

    std::string const GetTargetName() override { return "self target"; }
};

class CastStealthAction : public CastBuffSpellAction
{
public:
    CastStealthAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "stealth") {}

    bool isUseful() override;
    bool isPossible() override;
    std::string const GetTargetName() override { return "self target"; }
};

class UnstealthAction : public Action
{
public:
    UnstealthAction(PlayerbotAI* botAI) : Action(botAI, "unstealth") {}

    bool Execute(Event event) override;
    bool isPossible() override;  // [b31_wr] only in stealth
};

class CheckStealthAction : public Action
{
public:
    CheckStealthAction(PlayerbotAI* botAI) : Action(botAI, "check stealth") {}

    bool Execute(Event event) override;
    bool isPossible() override { return true; }
};

class CastKickAction : public CastSpellAction
{
public:
    CastKickAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "kick") {}
};

class CastFeintAction : public CastBuffSpellAction
{
public:
    CastFeintAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "feint") {}
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastColdBloodAction : public CastBuffSpellAction
{
public:
    CastColdBloodAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "cold blood") {}
};

class CastDismantleAction : public CastSpellAction
{
public:
    CastDismantleAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "dismantle") {}
};

class CastDistractAction : public CastSpellAction
{
public:
    CastDistractAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "distract") {}
};

class CastVanishAction : public CastBuffSpellAction
{
public:
    CastVanishAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "vanish") {}

    bool isUseful() override;
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastBlindAction : public CastDebuffSpellAction
{
public:
    CastBlindAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "blind") {}
};

class CastBladeFlurryAction : public CastBuffSpellAction
{
public:
    CastBladeFlurryAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "blade flurry") {}
    // [b33_aoe_class] Combat talent: Assassination/Subtlety took "light aoe" here (gd-h-5F-51: 84 x "spell not known")
    bool isUseful() override { return botAI->HasSpell("blade flurry") && CastBuffSpellAction::isUseful(); }
};

class CastAdrenalineRushAction : public CastBuffSpellAction
{
public:
    CastAdrenalineRushAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "adrenaline rush") {}
};

// Subtlety: Ambush and Garrote without stealth for 6 s; used with energy pooled for the Ambushes
class CastShadowDanceAction : public CastBuffSpellAction
{
public:
    CastShadowDanceAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "shadow dance") {}
};

class CastKillingSpreeAction : public CastMeleeSpellAction
{
public:
    CastKillingSpreeAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "killing spree") {}
};

class CastKickOnEnemyHealerAction : public CastSpellOnEnemyHealerAction
{
public:
    CastKickOnEnemyHealerAction(PlayerbotAI* botAI) : CastSpellOnEnemyHealerAction(botAI, "kick") {}
};

// [b31_wr] Envenom eats the rogue's own Deadly Poison doses; without one on the target it is "impossible" and the
// node's alternative (Eviscerate) goes instead.
class CastEnvenomAction : public CastMeleeSpellAction
{
public:
    CastEnvenomAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "envenom") {}

    bool isUseful() override;
    bool isPossible() override;

private:
    // [rot49] debug trace of the finisher choice at 4+ combo points on a dungeon boss (logged when it changes)
    void TraceFinisher();
    std::string lastTrace;
};

class CastTricksOfTheTradeOnMainTankAction : public BuffOnMainTankAction
{
public:
    CastTricksOfTheTradeOnMainTankAction(PlayerbotAI* botAI) : BuffOnMainTankAction(botAI, "tricks of the trade", true) {}

    // Raid utility coordinator: the tank holding the current pull, else the main tank.
    std::string const GetTargetName() override { return "threat redirect target"; }
    Value<Unit*>* GetTargetValue() override { return context->GetValue<Unit*>("threat redirect target"); }
    bool isUseful() override;
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

// [b31_wr] One poison action per hand; RoguePoisons decides the kind (spec, Wound next to another rogue's Deadly)
// and falls back to what is in the bags (replaces the hard-wired Instant MH / Deadly OH / Instant OH actions).
class UseRoguePoisonAction : public UseItemAction
{
public:
    UseRoguePoisonAction(PlayerbotAI* botAI, std::string const name, uint8 slot)
        : UseItemAction(botAI, name), slot(slot)
    {
    }

    bool Execute(Event event) override;
    bool isPossible() override { return true; }

private:
    uint8 const slot;
};

class FanOfKnivesAction : public CastMeleeSpellAction
{
public:
    FanOfKnivesAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "fan of knives") {}

    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
};

#endif
