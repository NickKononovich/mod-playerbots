/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROGUETRIGGERS_H
#define PLAYERBOTS_ROGUETRIGGERS_H

#include "AiObjectContext.h"
#include "GenericTriggers.h"
#include "RogueFinishingActions.h"

class PlayerbotAI;

class KickInterruptSpellTrigger : public InterruptSpellTrigger
{
public:
    KickInterruptSpellTrigger(PlayerbotAI* botAI) : InterruptSpellTrigger(botAI, "kick") {}
};

class SliceAndDiceTrigger : public BuffTrigger
{
public:
    SliceAndDiceTrigger(PlayerbotAI* botAI)
        : BuffTrigger(botAI, "slice and dice", 1, false, false, SLICE_AND_DICE_REFRESH_MS) {}
    bool IsActive() override;  // [melee_audit_b30] 2+ combo points, not on a target about to die
};

// [b31_wr] the self buff (63848, 60 s) missing or under 5 s, on a bleeding target (the cast needs one)
class HungerForBloodTrigger : public BuffTrigger
{
public:
    HungerForBloodTrigger(PlayerbotAI* botAI)
        : BuffTrigger(botAI, "hunger for blood", 1, false, false, HUNGER_FOR_BLOOD_REFRESH_MS) {}
    bool IsActive() override;
};

// [b31_wr] Adrenaline Rush and Killing Spree were plain boost triggers and went back to back. wowsims combat APL:
// Killing Spree at energy <= 50 and not under Adrenaline Rush; Adrenaline Rush at energy <= 50, not during Killing
// Spree and only with Killing Spree on cooldown for 15 s more (or not known).
constexpr uint32 ROGUE_BURST_MAX_ENERGY = 50;

class AdrenalineRushTrigger : public BoostTrigger
{
public:
    AdrenalineRushTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "adrenaline rush") {}

    bool IsActive() override;
};

class KillingSpreeTrigger : public BoostTrigger
{
public:
    KillingSpreeTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "killing spree") {}

    bool IsActive() override;
};

// [b31_wr] Combat: Blade Flurry on cooldown on a single target too (3.3: +20 % attack speed), while Slice and Dice is
// up, and never next to a crowd-controlled mob (it would break the CC).
class BladeFlurryReadyTrigger : public Trigger
{
public:
    BladeFlurryReadyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "blade flurry ready") {}

    bool IsActive() override;
};

// [b31_wr] Combat: refresh a running Slice and Dice under 6 s with 4+ points instead of spending them on Eviscerate
class SliceAndDicePreRefreshTrigger : public Trigger
{
public:
    SliceAndDicePreRefreshTrigger(PlayerbotAI* botAI) : Trigger(botAI, "slice and dice pre-refresh") {}

    bool IsActive() override;
};

// [b31_wr] Assassination: Rupture only as the bleed that enables Hunger for Blood (no bleed on the target, the
// buff missing or under HUNGER_FOR_BLOOD_REFRESH_MS); Envenom takes the points otherwise (wowsims mutilate APL).
class AssassinationRuptureTrigger : public Trigger
{
public:
    AssassinationRuptureTrigger(PlayerbotAI* botAI) : Trigger(botAI, "assassination rupture") {}

    bool IsActive() override;
};

// [b31_wr] Cold Blood right before a 5-point finisher: 5 points, energy for the finisher, Slice and Dice running (or
// the next finisher would be it) and no Hunger for Blood enabler Rupture pending (Rupture cannot crit).
class ColdBloodFinisherTrigger : public Trigger
{
public:
    ColdBloodFinisherTrigger(PlayerbotAI* botAI) : Trigger(botAI, "cold blood finisher") {}

    bool IsActive() override;
};

class ShadowDanceTrigger : public BoostTrigger
{
public:
    ShadowDanceTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "shadow dance") {}
    bool IsActive() override;
};

class InShadowDanceTrigger : public HasAuraTrigger
{
public:
    InShadowDanceTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "shadow dance") {}
};

// [b31_wr] Subtlety builder: Hemorrhage when its debuff is missing on the target and combo points are not full
class HemorrhageDebuffTrigger : public DebuffTrigger
{
public:
    HemorrhageDebuffTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "hemorrhage") {}
    bool IsActive() override;
};

// [b31_wr] Subtlety: Premeditation in Stealth or Shadow Dance, off cooldown, with 3 combo points or fewer
class PremeditationTrigger : public Trigger
{
public:
    PremeditationTrigger(PlayerbotAI* botAI) : Trigger(botAI, "premeditation") {}
    bool IsActive() override;
};

class RiposteAvailableTrigger : public SpellCanBeCastTrigger
{
public:
    RiposteAvailableTrigger(PlayerbotAI* botAI) : SpellCanBeCastTrigger(botAI, "riposte") {}
};

class RuptureTrigger : public DebuffTrigger
{
public:
    RuptureTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "rupture", 1, true, RUPTURE_MIN_TARGET_LIFETIME, RUPTURE_REFRESH_MS) {}
    bool IsActive() override;
};

class ExposeArmorTrigger : public DebuffTrigger
{
public:
    ExposeArmorTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "expose armor") {}
    virtual bool IsActive() override;
};

class KickInterruptEnemyHealerSpellTrigger : public InterruptEnemyHealerTrigger
{
public:
    KickInterruptEnemyHealerSpellTrigger(PlayerbotAI* botAI) : InterruptEnemyHealerTrigger(botAI, "kick") {}
};

class InStealthTrigger : public HasAuraTrigger
{
public:
    InStealthTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "stealth") {}
};

class NoStealthTrigger : public HasNoAuraTrigger
{
public:
    NoStealthTrigger(PlayerbotAI* botAI) : HasNoAuraTrigger(botAI, "stealth") {}
};

class UnstealthTrigger : public BuffTrigger
{
public:
    UnstealthTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "stealth", 3) {}

    bool IsActive() override;
};

class StealthTrigger : public Trigger
{
public:
    StealthTrigger(PlayerbotAI* botAI) : Trigger(botAI, "stealth") {}

    bool IsActive() override;
};

class SapTrigger : public HasCcTargetTrigger
{
public:
    SapTrigger(PlayerbotAI* botAI) : HasCcTargetTrigger(botAI, "sap") {}

    bool IsPossible();
};

class SprintTrigger : public BuffTrigger
{
public:
    SprintTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "sprint", 3) {}

    bool IsPossible();
    bool IsActive() override;
};

// [b31_wr] A weapon poison to apply. Out of combat: none on the weapon or under minRemainingMs left (the item field
// is refreshed on save, so it can read high, never low). In combat (minRemainingMs 0): only a missing one, and only on
// a target that lives minTargetLifetime more seconds, since the 3 s cast stops the attacks.
class WeaponPoisonTrigger : public BuffTrigger
{
public:
    WeaponPoisonTrigger(PlayerbotAI* botAI, std::string const name, uint8 slot, uint32 minRemainingMs,
                        float minTargetLifetime)
        : BuffTrigger(botAI, name, 1), slot(slot), minRemainingMs(minRemainingMs), minTargetLifetime(minTargetLifetime)
    {
    }

    bool IsActive() override;

private:
    uint8 const slot;
    uint32 const minRemainingMs;
    float const minTargetLifetime;
};

class TricksOfTheTradeOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    TricksOfTheTradeOnMainTankTrigger(PlayerbotAI* botAI)
        : BuffOnMainTankTrigger(botAI, "tricks of the trade", true) {}
    bool IsActive() override;
    Value<Unit*>* GetTargetValue() override { return context->GetValue<Unit*>("threat redirect target"); }
};

#endif
