/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_WARRIORTRIGGERS_H
#define PLAYERBOTS_WARRIORTRIGGERS_H

#include "GenericTriggers.h"
#include "PlayerbotAI.h"

// [b31_deep] Refresh window of the shouts (wowsims arms / prot APL: Battle / Commanding Shout refreshed 3 s before it
// expires): a plain buff trigger only fired once the shout had dropped. A shout has no ticks, nothing is lost.
constexpr uint32 WARRIOR_SHOUT_REFRESH_MS = 3 * IN_MILLISECONDS;

namespace WarriorRage
{
    // [b31_deep] Rage cost of a spell (x10, as GetPower(POWER_RAGE)); 0 when unknown. CanCastSpell ignores the cost.
    int32 Cost(Player* bot, uint32 spellId);
    bool HasRageFor(Player* bot, uint32 spellId);
}

// [b31_deep] Divine Shield, Ice Block or Hand of Protection (any rank): what Shattering Throw removes.
bool WarriorHasShatterableImmunity(Unit* unit);

class BattleShoutTrigger : public BuffTrigger
{
public:
    BattleShoutTrigger(PlayerbotAI* botAI)
        : BuffTrigger(botAI, "battle shout", 1, false, false, WARRIOR_SHOUT_REFRESH_MS) {}  // [b31_deep]
    bool IsActive() override;
};

BUFF_TRIGGER(BattleStanceTrigger, "battle stance");
BUFF_TRIGGER(DefensiveStanceTrigger, "defensive stance");
BUFF_TRIGGER(BerserkerStanceTrigger, "berserker stance");
BUFF_TRIGGER(ShieldBlockTrigger, "shield block");
// [b31_wr] Commanding Shout is learned at 68: before that the plain buff trigger was always active and its "battle
// shout" alternative was cast on every tick (tank L60-67: Battle Shout 14 a minute). Active only once it is known.
class CommandingShoutTrigger : public BuffTrigger
{
public:
    CommandingShoutTrigger(PlayerbotAI* botAI)
        : BuffTrigger(botAI, "commanding shout", 1, false, false, WARRIOR_SHOUT_REFRESH_MS) {}  // [b31_deep]
    bool IsActive() override;
};
DEBUFF_TRIGGER(DisarmDebuffTrigger, "disarm");
DEBUFF_TRIGGER(SunderArmorDebuffTrigger, "sunder armor");
// [b31_deep] the "mortal strike" debuff trigger is gone: "mortal strike ready" (rage gated) replaces it
// DEBUFF_ENEMY_TRIGGER(RendDebuffOnAttackerTrigger, "rend");

class RendDebuffOnAttackerTrigger : public DebuffOnMeleeAttackerTrigger
{
public:
    RendDebuffOnAttackerTrigger(PlayerbotAI* botAI) : DebuffOnMeleeAttackerTrigger(botAI, "rend") {}
};

CAN_CAST_TRIGGER(RevengeAvailableTrigger, "revenge");
CAN_CAST_TRIGGER(OverpowerAvailableTrigger, "overpower");
BUFF_TRIGGER(RampageAvailableTrigger, "rampage");
BUFF_TRIGGER_A(BloodrageBuffTrigger, "bloodrage");
CAN_CAST_TRIGGER(VictoryRushTrigger, "victory rush");
HAS_AURA_TRIGGER(SwordAndBoardTrigger, "sword and board");
SNARE_TRIGGER(ConcussionBlowTrigger, "concussion blow");
SNARE_TRIGGER(HamstringTrigger, "hamstring");
SNARE_TRIGGER(MockingBlowTrigger, "mocking blow");
SNARE_TRIGGER(ThunderClapSnareTrigger, "thunder clap");

// [b31_deep] Thunder Clap's attack speed slow does not stack with Frost Fever, Judgements of the Just or Infected
// Wounds (one slot): no GCD and rage to keep it up while another class covers it (as DemoralizeRoarTrigger).
class ThunderClapTrigger : public DebuffTrigger
{
public:
    ThunderClapTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "thunder clap") {}

    bool IsActive() override;
};

// [b31_deep] Demoralizing Shout kept up on a target that lives 15 s, unless Demoralizing Roar, Curse of Weakness or
// Vindication (the same slot) is on it. It was only a default action: 1.1 a minute on bosses.
class DemoralizingShoutDebuffTrigger : public DebuffTrigger
{
public:
    DemoralizingShoutDebuffTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "demoralizing shout", 1, false, 15.0f) {}

    bool IsActive() override;
};
SNARE_TRIGGER(TauntSnareTrigger, "taunt");
SNARE_TRIGGER(InterceptSnareTrigger, "intercept");
// [b31_deep] "intercept can cast" / "intercept and far enemy" / "intercept and rage" removed: unused, and the
// second one named an unregistered "enemy is out of melee" trigger (always false).
SNARE_TRIGGER(ShockwaveSnareTrigger, "shockwave");
DEBUFF_TRIGGER(ShockwaveTrigger, "shockwave");

// [b31_deep] Only once Death Wish is known: the Arms 80 template has no Death Wish and logged "spell not known" 125
// times from this boost trigger.
class DeathWishTrigger : public BoostTrigger
{
public:
    DeathWishTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "death wish") {}

    bool IsActive() override;
};
BOOST_TRIGGER(RecklessnessTrigger, "recklessness");
BUFF_TRIGGER(BerserkerRageBuffTrigger, "berserker rage");

// [b31_wr] "<spell> ready": known, off cooldown, and a current target. Bloodthirst and Whirlwind were buff triggers
// (a "bloodthirst" self aura that lasts 8 s / a "whirlwind" aura that never exists), so Bloodthirst fell to the
// default list half the time and Whirlwind's trigger was always on. Rage, range and stance stay with the action.
// [b31_deep] checkRage: also its rage cost on hand (Mortal Strike, 30): CanCastSpell ignores the cost, so a strike
// short of rage failed in Execute and the next action (Slam, 15) spent what was there.
class WarriorSpellReadyTrigger : public SpellTrigger
{
public:
    WarriorSpellReadyTrigger(PlayerbotAI* botAI, std::string const spell, bool checkRage = false)
        : SpellTrigger(botAI, spell), triggerName(spell + " ready"), checkRage(checkRage) {}

    std::string const getName() override { return triggerName; }
    bool IsActive() override;

private:
    std::string const triggerName;
    bool const checkRage;
};

// [b31_deep] A rage threshold under its own name (RageAvailable is always "rage available").
class WarriorRageAvailableTrigger : public RageAvailable
{
public:
    WarriorRageAvailableTrigger(PlayerbotAI* botAI, std::string const name, int32 amount)
        : RageAvailable(botAI, amount), triggerName(name) {}

    std::string const getName() override { return triggerName; }

private:
    std::string const triggerName;
};

// [rot19] wowsims fury: Heroic Strike at 12+ rage, as long as it leaves the rage for a Bloodthirst / Whirlwind about
// to come off cooldown (WarriorLeavesRageForStrike)
class FuryHeroicStrikeRageTrigger : public WarriorRageAvailableTrigger
{
public:
    static constexpr int32 FURY_HEROIC_STRIKE_RAGE = 12;

    FuryHeroicStrikeRageTrigger(PlayerbotAI* botAI)
        : WarriorRageAvailableTrigger(botAI, "fury heroic strike rage", FURY_HEROIC_STRIKE_RAGE)
    {
    }

    bool IsActive() override;
};

// [b31_deep] Shattering Throw as a boss cooldown (wowsims arms / fury APL MCD, Warcraft Tavern Arms): engaged boss
// after 6 s, not while the burst waits for Bloodlust, target lives 15 s more, no Shattering Throw on it yet, Battle
// Stance (no dance), its rage on hand. It was only cast on Divine Shield / Ice Block / Hand of Protection: 0 in PvE.
class ShatteringThrowBossTrigger : public Trigger
{
public:
    ShatteringThrowBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "shattering throw boss") {}

    bool IsActive() override;
};

// [b31_wr] An AoE trigger for an ability the current stance can cast (Spell.dbc stance masks: Whirlwind berserker,
// Thunder Clap battle | defensive, Sweeping Strikes battle | berserker). CanCastSpell lets SPELL_FAILED_NOT_SHAPESHIFT
// through, so Arms tried Whirlwind and Fury Thunder Clap on every AoE tick. No stance dance for AoE.
class WarriorStanceAoeTrigger : public AoeTrigger
{
public:
    static constexpr uint32 BATTLE = 1u << (FORM_BATTLESTANCE - 1);
    static constexpr uint32 DEFENSIVE = 1u << (FORM_DEFENSIVESTANCE - 1);
    static constexpr uint32 BERSERKER = 1u << (FORM_BERSERKERSTANCE - 1);
    static constexpr uint32 ANY = BATTLE | DEFENSIVE | BERSERKER;

    // [b31_deep] spell: the ability must also be known (Fury 80 has no Sweeping Strikes / Bladestorm / Shockwave:
    // "spell not known" 347 / 229 / 244 times on L80 build 30 from the plain AoE triggers).
    WarriorStanceAoeTrigger(PlayerbotAI* botAI, std::string const name, uint32 stanceMask, std::string const spell,
                            int32 amount = 2)
        : AoeTrigger(botAI, amount, 8.0f), triggerName(name), stanceMask(stanceMask), spellName(spell) {}

    std::string const getName() override { return triggerName; }
    bool IsActive() override;

private:
    std::string const triggerName;
    uint32 const stanceMask;
    std::string const spellName;
};

// [b31_wr] Tank before Commanding Shout (L68): Battle Shout through BattleShoutTrigger (which also yields to a
// stronger Blessing of Might) instead of Commanding Shout's alternative.
class BattleShoutNoCommandingShoutTrigger : public BattleShoutTrigger
{
public:
    BattleShoutNoCommandingShoutTrigger(PlayerbotAI* botAI) : BattleShoutTrigger(botAI) {}

    std::string const getName() override { return "battle shout no commanding shout"; }
    bool IsActive() override;
};
INTERRUPT_HEALER_TRIGGER(ShieldBashInterruptEnemyHealerSpellTrigger, "shield bash");
INTERRUPT_TRIGGER(ShieldBashInterruptSpellTrigger, "shield bash");
INTERRUPT_HEALER_TRIGGER(PummelInterruptEnemyHealerSpellTrigger, "pummel");
INTERRUPT_TRIGGER(PummelInterruptSpellTrigger, "pummel");

// [melee_audit_b30] Arms fights in Battle Stance, where Pummel cannot be cast: build 30 Arms 80 pummelled once in
// 30 min (Fury 2.4 a minute). This one fires only when the dance pays: Pummel off cooldown and its 10 rage there,
// so the strategy swaps to Berserker Stance for the kick and "battle stance" swaps back.
class PummelReadyInterruptSpellTrigger : public PummelInterruptSpellTrigger
{
public:
    PummelReadyInterruptSpellTrigger(PlayerbotAI* botAI) : PummelInterruptSpellTrigger(botAI) {}

    bool IsActive() override
    {
        constexpr uint32 PUMMEL_RAGE = 10 * 10;  // rage is stored x10
        // Pummel ranks 6554 / 6552 (header-only: AiObjectContext is incomplete here)
        uint32 const spellId = bot->HasSpell(6554) ? 6554 : (bot->HasSpell(6552) ? 6552 : 0);
        if (!spellId || bot->HasSpellCooldown(spellId) || bot->GetPower(POWER_RAGE) < PUMMEL_RAGE)
            return false;

        return PummelInterruptSpellTrigger::IsActive();
    }
};
INTERRUPT_HEALER_TRIGGER(InterceptInterruptEnemyHealerSpellTrigger, "intercept");
INTERRUPT_TRIGGER(InterceptInterruptSpellTrigger, "intercept");
DEFLECT_TRIGGER(SpellReflectionTrigger, "spell reflection");
HAS_AURA_TRIGGER(SuddenDeathTrigger, "sudden death");
HAS_AURA_TRIGGER(SlamInstantTrigger, "slam!");
HAS_AURA_TRIGGER(TasteForBloodTrigger, "taste for blood");

class RendDebuffTrigger : public DebuffTrigger
{
public:
    RendDebuffTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "rend", 1, true) {}
};

namespace WarriorVigilance
{
    // Group member that should carry this warrior's Vigilance: in a fight the non-tank with the most threat on
    // the warrior's attackers, else the best geared dps. The current holder keeps it unless another member is
    // clearly ahead, so it does not hop between two close dps. nullptr when nobody qualifies.
    Player* FindTarget(PlayerbotAI* botAI);

    // Member carrying this warrior's own Vigilance, or nullptr.
    Player* CurrentHolder(Player* bot);
}

class VigilanceTrigger : public BuffTrigger
{
public:
    VigilanceTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "vigilance", 5 * IN_MILLISECONDS) {}

    bool IsActive() override;

private:
    time_t _nextReview = 0;
};

class ShatteringThrowTrigger : public Trigger
{
public:
    ShatteringThrowTrigger(PlayerbotAI* botAI) : Trigger(botAI, "shattering throw trigger") {}

    bool IsActive() override;
};

BOOST_TRIGGER(BladestormBoostTrigger, "bladestorm");

// Recklessness needs Berserker Stance; only fire when it is castable so a non-fury spec does not stance-dance for
// nothing while it is on cooldown.
class RecklessnessReadyTrigger : public BoostTrigger
{
public:
    RecklessnessReadyTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "recklessness") {}

    bool IsActive() override;
};

// The pull shot leaves the tank without rage while the pack runs in, and Thunder Clap needs 20 when it lands.
// Bloodrage is off the global cooldown.
class BloodrageLowRageTrigger : public Trigger
{
public:
    static constexpr uint8 LOW_RAGE = 10;

    BloodrageLowRageTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bloodrage low rage") {}

    bool IsActive() override;
};

class CriticalHealthSoloTrigger : public LowHealthTrigger
{
public:
    CriticalHealthSoloTrigger(PlayerbotAI* botAI)
        : LowHealthTrigger(botAI, "critical health solo", sPlayerbotAIConfig.CriticalHealth, 0)
    {
    }

    bool IsActive() override;
};

// class SlamTrigger : public HasAuraTrigger
// {
// public:
//     SlamTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "slam!") {}
// };

#endif
