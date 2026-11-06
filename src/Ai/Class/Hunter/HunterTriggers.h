/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HUNTERTRIGGERS_H
#define PLAYERBOTS_HUNTERTRIGGERS_H

#include "AiObjectContext.h"
#include "CureTriggers.h"
#include "GenericTriggers.h"
#include "PlayerbotAI.h"
#include "Trigger.h"
#include <set>

class PlayerbotAI;

// Buff and Out of Combat Triggers

class HunterAspectOfTheDragonhawkTrigger : public BuffTrigger
{
public:
    HunterAspectOfTheDragonhawkTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "aspect of the dragonhawk") {}
    bool IsActive() override;
};

class HunterAspectOfTheWildTrigger : public BuffTrigger
{
public:
    HunterAspectOfTheWildTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "aspect of the wild") {}
};

class HunterAspectOfTheViperTrigger : public BuffTrigger
{
public:
    HunterAspectOfTheViperTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "aspect of the viper") {}
    bool IsActive() override;
};

class HunterAspectOfThePackTrigger : public BuffTrigger
{
public:
    HunterAspectOfThePackTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "aspect of the pack") {}
    bool IsActive() override;
};

class TrueshotAuraTrigger : public BuffTrigger
{
public:
    TrueshotAuraTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "trueshot aura") {}
};

class NoTrackTrigger : public BuffTrigger
{
public:
    NoTrackTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "no track") {}
    bool IsActive() override;
};

class HunterLowAmmoTrigger : public AmmoCountTrigger
{
public:
    HunterLowAmmoTrigger(PlayerbotAI* botAI) : AmmoCountTrigger(botAI, "ammo", 1, 30) {}

    bool IsActive() override;
};

class HunterNoAmmoTrigger : public AmmoCountTrigger
{
public:
    HunterNoAmmoTrigger(PlayerbotAI* botAI) : AmmoCountTrigger(botAI, "ammo", 1, 10) {}
};

class HunterHasAmmoTrigger : public AmmoCountTrigger
{
public:
    HunterHasAmmoTrigger(PlayerbotAI* botAI) : AmmoCountTrigger(botAI, "ammo", 1, 10) {}

    bool IsActive() override;
};

// Cooldown Triggers

// [b35_bal] Rapid Fire (5 min) is paced like Bloodlust/Heroism (ShouldCastLust): in an instance only on an engaged
// boss (or a player / ordered burst / lust window), never on trash. The plain BoostTrigger fired on any trash pack
// with "balance" <= 50, so RF was on cooldown in 47 of 98 level-80 BM boss attempts.
class RapidFireTrigger : public BoostTrigger
{
public:
    RapidFireTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "rapid fire") {}

    bool IsActive() override;
};

// [rot160] b214 rfc-h 5H / 5A: Readiness was never cast in a run (the MM hunters, sim 0.69 casts/min: a second Rapid
// Fire per boss) - it was only the "rapid fire" node's alternative, and that node runs only while Rapid Fire is ready.
// wowsims hunter/talents.go registerReadinessCD: cast while Rapid Fire is on cooldown (ExtraCastCondition), not when
// Rapid Fire comes back by itself within 10 s, and while Rapid Fire's aura is off or under 10 s. The leader's burn
// verdict gates the cast like every burn cooldown (Leader::BURN_COOLDOWNS)
class ReadinessTrigger : public Trigger
{
public:
    ReadinessTrigger(PlayerbotAI* botAI) : Trigger(botAI, "readiness", 1) {}

    bool IsActive() override;
};

class BestialWrathTrigger : public BoostTrigger
{
public:
    BestialWrathTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "bestial wrath") {}
    std::string const GetTargetName() override { return "pet target"; }
};

class IntimidationTrigger : public BuffTrigger
{
public:
    IntimidationTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "intimidation") {}
    // [guide_b31_dead] was a self-aura check that never matched, so it fired at 40 above every shot. Now a peel /
    // interrupt: off cooldown, and the pet's target is hitting the hunter or a healer, or is casting.
    bool IsActive() override;
};

class KillCommandTrigger : public BuffTrigger
{
public:
    KillCommandTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "kill command") {}
    bool IsActive() override;
};

// [b31_deep] The proc buff (2 charges: no cooldown, no cost on Arcane/Explosive Shot). The talent's passive
// (56342-56344) is also named "Lock and Load", so the old by-name check was always true for every SV with the talent.
constexpr uint32 SPELL_LOCK_AND_LOAD_PROC = 56453;

class LockAndLoadTrigger : public BuffTrigger
{
public:
    LockAndLoadTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "lock and load") {}

    bool IsActive() override { return bot->HasAura(SPELL_LOCK_AND_LOAD_PROC); }  // [b31_deep]
};

// CC Triggers

class FreezingTrapTrigger : public HasCcTargetTrigger
{
public:
    FreezingTrapTrigger(PlayerbotAI* botAI) : HasCcTargetTrigger(botAI, "freezing trap") {}
};

class ConcussiveShotOnSnareTargetTrigger : public SnareTargetTrigger
{
public:
    ConcussiveShotOnSnareTargetTrigger(PlayerbotAI* botAI) : SnareTargetTrigger(botAI, "concussive shot") {}
};

class ScareBeastTrigger : public HasCcTargetTrigger
{
public:
    ScareBeastTrigger(PlayerbotAI* botAI) : HasCcTargetTrigger(botAI, "scare beast") {}
};

class SilencingShotTrigger : public InterruptSpellTrigger
{
public:
    SilencingShotTrigger(PlayerbotAI* botAI) : InterruptSpellTrigger(botAI, "silencing shot") {}
};

// DoT/Debuff Triggers

// Any hunter's Mark counts (one per target); trash dying within a few seconds is not worth the GCD
constexpr float HUNTERS_MARK_MIN_TARGET_LIFETIME = 10.0f;

class HuntersMarkTrigger : public DebuffTrigger
{
public:
    HuntersMarkTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "hunter's mark", 1, false, HUNTERS_MARK_MIN_TARGET_LIFETIME) {}
};

class ExplosiveShotTrigger : public DebuffTrigger
{
public:
    ExplosiveShotTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "explosive shot", 1, true) {}
    bool IsActive() override { return BuffTrigger::IsActive(); }
};

class BlackArrowTrigger : public DebuffTrigger
{
public:
    BlackArrowTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "black arrow", 1, true) {}
    bool IsActive() override;
};

class HunterNoStingsActiveTrigger : public DebuffTrigger
{
public:
    // [rot550] no fixed lifetime: SerpentStingPays weighs the ticks that land
    HunterNoStingsActiveTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "no stings", 1, false, 0.0f) {}
    bool IsActive() override;

private:
    uint32 _payoffLoggedMs = 0;  // [rot550] SerpentStingPays' log clock
};

class SerpentStingOnAttackerTrigger : public DebuffOnAttackerTrigger
{
public:
    SerpentStingOnAttackerTrigger(PlayerbotAI* botAI) : DebuffOnAttackerTrigger(botAI, "serpent sting", true, 0.0f) {}
    bool IsActive() override;

protected:
    // [rot550] the ticks that land before the attacker dies against Steady Shot (DebuffTrigger::DotPaysOff)
    std::string const DotFiller() override { return "steady shot"; }
};

// Damage/Combat Triggers

class AutoShotTrigger : public Trigger
{
public:
    AutoShotTrigger(PlayerbotAI* botAI) : Trigger(botAI, "auto shot") {}
};

class MisdirectionOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    MisdirectionOnMainTankTrigger(PlayerbotAI* botAI) : BuffOnMainTankTrigger(botAI, "misdirection", true) {}
    bool IsActive() override;
    Value<Unit*>* GetTargetValue() override { return context->GetValue<Unit*>("threat redirect target"); }
};

class TargetRemoveEnrageTrigger : public TargetAuraDispelTrigger
{
public:
    TargetRemoveEnrageTrigger(PlayerbotAI* botAI) : TargetAuraDispelTrigger(botAI, "tranquilizing shot", DISPEL_ENRAGE) {}
};

class TargetRemoveMagicTrigger : public TargetAuraDispelTrigger
{
public:
    TargetRemoveMagicTrigger(PlayerbotAI* botAI) : TargetAuraDispelTrigger(botAI, "tranquilizing shot", DISPEL_MAGIC) {}
};

class ImmolationTrapNoCdTrigger : public SpellNoCooldownTrigger
{
public:
    ImmolationTrapNoCdTrigger(PlayerbotAI* botAI) : SpellNoCooldownTrigger(botAI, "immolation trap") {}
};

BEGIN_TRIGGER(HuntersPetDeadTrigger, Trigger)
END_TRIGGER()

// [b31_deep] In combat: the pet died fighting a boss with enough fight left for the Revive Pet cast to pay off.
// The "pet dead" value queries the database when no pet is summoned, so this reads the pet and the pet stable.
class HuntersPetDeadOnBossTrigger : public Trigger
{
public:
    HuntersPetDeadOnBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hunters pet dead on boss") {}
    bool IsActive() override;
};

// [b31_deep] In combat: the living current pet is not summoned (dismissed), so Call Pet brings it back.
class HuntersPetDismissedTrigger : public Trigger
{
public:
    HuntersPetDismissedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hunters pet dismissed") {}
    bool IsActive() override;
};

BEGIN_TRIGGER(HuntersPetLowHealthTrigger, Trigger)
END_TRIGGER()

BEGIN_TRIGGER(HuntersPetMediumHealthTrigger, Trigger)
END_TRIGGER()

BEGIN_TRIGGER(HunterPetNotHappy, Trigger)
END_TRIGGER()

class VolleyChannelCheckTrigger : public Trigger
{
public:
    VolleyChannelCheckTrigger(PlayerbotAI* botAI, uint32 minEnemies = 2)
        : Trigger(botAI, "volley channel check"), minEnemies(minEnemies)
    {
    }

    bool IsActive() override;

protected:
    uint32 minEnemies;
    static const std::set<uint32> VOLLEY_SPELL_IDS;
};

#endif
