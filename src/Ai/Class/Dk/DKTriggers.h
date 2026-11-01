/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DKTRIGGERS_H
#define PLAYERBOTS_DKTRIGGERS_H

#include "GenericTriggers.h"

class Player;  // [b31_deep] HasReadyRunes
class PlayerbotAI;

BUFF_TRIGGER(HornOfWinterTrigger, "horn of winter");
BUFF_TRIGGER(BoneShieldTrigger, "bone shield");
BUFF_TRIGGER(ImprovedIcyTalonsTrigger, "improved icy talons");
// DEBUFF_CHECKISOWNER_TRIGGER(PlagueStrikeDebuffTrigger, "blood plague");
class PlagueStrikeDebuffTrigger : public DebuffTrigger
{
public:
    PlagueStrikeDebuffTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "blood plague", 1, true, .0f) {}
};

class PlagueStrike3sDebuffTrigger : public DebuffTrigger
{
public:
    PlagueStrike3sDebuffTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "blood plague", 1, true, .0f, 3000) {}
};

// DEBUFF_CHECKISOWNER_TRIGGER(IcyTouchDebuffTrigger, "frost fever");
class IcyTouchDebuffTrigger : public DebuffTrigger
{
public:
    IcyTouchDebuffTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "frost fever", 1, true, .0f) {}
};

class IcyTouch3sDebuffTrigger : public DebuffTrigger
{
public:
    IcyTouch3sDebuffTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "frost fever", 1, true, .0f, 3000) {}
};

BUFF_TRIGGER(UnbreakableArmorTrigger, "unbreakable armor");
class PlagueStrikeDebuffOnAttackerTrigger : public DebuffOnMeleeAttackerTrigger
{
public:
    PlagueStrikeDebuffOnAttackerTrigger(PlayerbotAI* botAI)
        : DebuffOnMeleeAttackerTrigger(botAI, "blood plague", true, .0f)
    {
    }
};

class IcyTouchDebuffOnAttackerTrigger : public DebuffOnMeleeAttackerTrigger
{
public:
    IcyTouchDebuffOnAttackerTrigger(PlayerbotAI* botAI) : DebuffOnMeleeAttackerTrigger(botAI, "frost fever", true, .0f)
    {
    }
};

// [b31_dk] The presence auras (by id: three aura lookups, no name scan).
inline constexpr uint32 SPELL_DK_BLOOD_PRESENCE = 48266;
inline constexpr uint32 SPELL_DK_FROST_PRESENCE = 48263;
inline constexpr uint32 SPELL_DK_UNHOLY_PRESENCE = 48265;

// [b31_dk] No presence at all (registered as "no presence"). Presences came only as prerequisites of rotation
// spells, so a DK whose first actions had none (pull, Death Grip, Death Coil, movement) fought without one.
// The strategy picks which presence: Frost for the Blood tank, Blood for Frost / Unholy DPS.
class DKPresenceTrigger : public Trigger
{
public:
    DKPresenceTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no presence") {}

    bool IsActive() override;
};

class BloodTapTrigger : public BuffTrigger
{
public:
    BloodTapTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "blood tap") {}
};

class RaiseDeadTrigger : public BuffTrigger
{
public:
    RaiseDeadTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "raise dead") {}
};

// [b31_deep] Rune Strike (56815, 20 runic power, on the next swing) is usable after a dodge or parry
// (AURA_STATE_DEFENSE). Registered now (the Blood node was dead), and by aura state, runic power and the queued swing:
// PlayerbotAI::CanCastSpell ignores the runic power cost, and a Rune Strike already queued on the next swing would
// be re-cast every tick above the rotation.
inline constexpr uint32 SPELL_DK_RUNE_STRIKE = 56815;
inline constexpr uint32 RUNE_STRIKE_RUNIC_POWER = 200;  // internal units (x10)

class RuneStrikeTrigger : public Trigger
{
public:
    RuneStrikeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "rune strike") {}

    bool IsActive() override;
};

// [b31_deep] Ready runes for a rune-paid spell, Death runes standing in for any type (MAX_RUNES = 6 slots).
// PlayerbotAI::CanCastSpell does not see rune costs, so a rune-paid priority gates itself with this.
bool HasReadyRunes(Player* bot, uint8 blood, uint8 frost, uint8 unholy);


// [b31_deep] Frost Strike before the runic power caps (wowsims frost_bl_pesti: Frost Strike at >= 115 right after
// Obliterate, above Pestilence / Blood Strike / Rime Howling Blast; 130 is the cap with Runic Power Mastery 2/2).
inline constexpr uint32 FROST_STRIKE_CAP_RUNIC_POWER = 1150;  // internal units (x10)

class FrostStrikeRunicPowerTrigger : public Trigger
{
public:
    FrostStrikeRunicPowerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "frost strike runic power") {}

    bool IsActive() override;
};

// [b31_deep] Ghoul Frenzy (63560: 1 Unholy rune, 10 s cooldown, 30 s on the ghoul) missing from our ghoul: wowsims
// uh_2h_ss casts it above Scourge Strike (Blood Tap first for the Death rune). Build 30 logged 0 casts: it sat at 5.9
// in the Death and Decay block under every rune spender. Ready when the rune is there or Blood Tap can make one.
inline constexpr uint32 SPELL_DK_GHOUL_FRENZY = 63560;
inline constexpr uint32 SPELL_DK_BLOOD_TAP = 45529;

class GhoulFrenzyTrigger : public Trigger
{
public:
    GhoulFrenzyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ghoul frenzy") {}

    bool IsActive() override;
};

// [b31_deep] Raise Dead as Frost's damage cooldown (wowsims frost_bl_pesti: in the opener and on cooldown) in a fight
// Summon Gargoyle would be used in (IsGargoyleFight: boss, player, burst window, long elite). Only with the Glyph of
// Raise Dead (60200, the Frost template's minor glyph): without a corpse the spell otherwise takes Corpse Dust, which
// PlayerbotAI::CanCastSpell does not check. The temporary ghoul also backs the "critical health" Death Pact.
inline constexpr uint32 SPELL_DK_GLYPH_OF_RAISE_DEAD = 60200;

class RaiseDeadBoostTrigger : public Trigger
{
public:
    RaiseDeadBoostTrigger(PlayerbotAI* botAI) : Trigger(botAI, "raise dead boost") {}

    bool IsActive() override;
};

// [b31_deep] Unholy Presence up while Summon Gargoyle is neither waiting for its fight nor out: back to Blood
// Presence (wowsims uh_2h_ss: Blood Presence when in Unholy Presence, Gargoyle not ready and not active). Unholy
// Presence is the prerequisite of Summon Gargoyle (UnholyDKStrategy).
inline constexpr uint32 SPELL_DK_SUMMON_GARGOYLE = 49206;
inline constexpr uint32 GARGOYLE_COOLDOWN_MS = 180000;
inline constexpr uint32 GARGOYLE_DURATION_MS = 30000;

class UnholyPresenceAfterGargoyleTrigger : public Trigger
{
public:
    UnholyPresenceAfterGargoyleTrigger(PlayerbotAI* botAI) : Trigger(botAI, "unholy presence after gargoyle") {}

    bool IsActive() override;
};

class DeathCoilTrigger : public SpellCanBeCastTrigger
{
public:
    DeathCoilTrigger(PlayerbotAI* botAI) : SpellCanBeCastTrigger(botAI, "death coil") {}
};

class PestilenceGlyphTrigger : public SpellTrigger
{
public:
    PestilenceGlyphTrigger(PlayerbotAI* botAI) : SpellTrigger(botAI, "pestilence") {}
    virtual bool IsActive() override;
};

class BloodStrikeTrigger : public DebuffTrigger
{
public:
    BloodStrikeTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "blood strike", 1, true) {}
};

class HowlingBlastTrigger : public DebuffTrigger
{
public:
    HowlingBlastTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "howling blast", 1, true) {}
};

class MindFreezeInterruptSpellTrigger : public InterruptSpellTrigger
{
public:
    MindFreezeInterruptSpellTrigger(PlayerbotAI* botAI) : InterruptSpellTrigger(botAI, "mind freeze") {}
};

class StrangulateInterruptSpellTrigger : public InterruptSpellTrigger
{
public:
    StrangulateInterruptSpellTrigger(PlayerbotAI* botAI) : InterruptSpellTrigger(botAI, "strangulate") {}
};

// [b31_dk] The Killing Machine proc buff (51124), by id: the talent's passive ranks (51123-51130) carry the same
// name, so a name lookup ("killing machine") can match the talent instead of the proc.
inline constexpr uint32 SPELL_DK_KILLING_MACHINE_PROC = 51124;

// [rot21] wowsims deathknight__frost_bl_pesti conditions on the bot's own Frost Fever (FF) on the current target:
//  - "killing machine frost strike": Killing Machine with neither a Frost nor an Unholy rune ready longer than
//    FROST_KM_RUNE_GRACE_MS (rune grace, a game mechanic: Player::GetGracePeriod) and FF >= FROST_FF_KEEP_MS
//  - "freezing fog frost fever": Rime (Freezing Fog) with FF >= FROST_FF_KEEP_MS: Howling Blast above Obliterate
//  - "frost blood strike": a ready Blood rune and FF > FROST_BLOOD_STRIKE_FF_MS (the runes for Pestilence stay)
//  - "frost pestilence": Glyph of Disease, FF up and <= FROST_PESTILENCE_FF_MS, the left Blood rune ready, Unbreakable
//    Armor not ready within FROST_PESTILENCE_FF_MS
inline constexpr uint32 FROST_KM_RUNE_GRACE_MS = 1000;
inline constexpr int32 FROST_FF_KEEP_MS = 3000;
inline constexpr int32 FROST_BLOOD_STRIKE_FF_MS = 10000;
inline constexpr int32 FROST_PESTILENCE_FF_MS = 6000;

class KillingMachineFrostStrikeTrigger : public Trigger
{
public:
    KillingMachineFrostStrikeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "killing machine frost strike") {}
    bool IsActive() override;
};

class FreezingFogFrostFeverTrigger : public Trigger
{
public:
    FreezingFogFrostFeverTrigger(PlayerbotAI* botAI) : Trigger(botAI, "freezing fog frost fever") {}
    bool IsActive() override;
};

class FrostBloodStrikeTrigger : public Trigger
{
public:
    FrostBloodStrikeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "frost blood strike") {}
    bool IsActive() override;
};

class FrostPestilenceTrigger : public Trigger
{
public:
    FrostPestilenceTrigger(PlayerbotAI* botAI) : Trigger(botAI, "frost pestilence") {}
    bool IsActive() override;
};

// [b31_deep] "killing machine no rime" (Howling Blast on Killing Machine) removed: on a single target Howling Blast
// spends the Frost + Unholy runes Obliterate needs for about half its damage, and Killing Machine does not act on
// Obliterate; wowsims frost_bl_pesti spends Killing Machine on Frost Strike only (Howling Blast only with Rime).

class MindFreezeOnEnemyHealerTrigger : public InterruptEnemyHealerTrigger
{
public:
    MindFreezeOnEnemyHealerTrigger(PlayerbotAI* botAI) : InterruptEnemyHealerTrigger(botAI, "mind freeze") {}
};

class ChainsOfIceSnareTrigger : public SnareTargetTrigger
{
public:
    ChainsOfIceSnareTrigger(PlayerbotAI* botAI) : SnareTargetTrigger(botAI, "chains of ice") {}
};

class StrangulateOnEnemyHealerTrigger : public InterruptEnemyHealerTrigger
{
public:
    StrangulateOnEnemyHealerTrigger(PlayerbotAI* botAI) : InterruptEnemyHealerTrigger(botAI, "strangulate") {}
};

class HighBloodRuneTrigger : public Trigger
{
public:
    HighBloodRuneTrigger(PlayerbotAI* botAI) : Trigger(botAI, "high blood rune") {}
    bool IsActive() override;
};

class HighFrostRuneTrigger : public Trigger
{
public:
    HighFrostRuneTrigger(PlayerbotAI* botAI) : Trigger(botAI, "high frost rune") {}
    bool IsActive() override;
};

class HighUnholyRuneTrigger : public Trigger
{
public:
    HighUnholyRuneTrigger(PlayerbotAI* botAI) : Trigger(botAI, "high unholy rune") {}
    bool IsActive() override;
};

class NoRuneTrigger : public Trigger
{
public:
    NoRuneTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no rune") {}
    bool IsActive() override;
};

class FreezingFogTrigger : public HasAuraTrigger
{
public:
    FreezingFogTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "freezing fog") {}
};

class DesolationTrigger : public BuffTrigger
{
public:
    DesolationTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "desolation", 1, false, true, 10000) {}
    bool IsActive() override;
};

// [b31_deep] "death and decay cooldown" removed: Unholy's rune spenders hung on it, so with Death and Decay off
// cooldown but not castable (threat hold, range, a missing rune) the whole block was off. They are default actions now.

// [b31_deep] Army of the Dead (42650: 1 Blood + 1 Frost + 1 Unholy rune, 10 min, a 4 s channel) on a boss only, as
// the pull starts (wowsims: prepull for Frost, under the Gargoyle for Unholy); the BoostTrigger cast it on trash too.
class ArmyOfTheDeadTrigger : public Trigger
{
public:
    ArmyOfTheDeadTrigger(PlayerbotAI* botAI) : Trigger(botAI, "army of the dead") {}

    bool IsActive() override;

protected:
    // [b35_uh] the Unholy variants (below) under their own names
    ArmyOfTheDeadTrigger(PlayerbotAI* botAI, std::string const name) : Trigger(botAI, name) {}
};

class HysteriaNoCooldownTrigger : public SpellNoCooldownTrigger
{
public:
    HysteriaNoCooldownTrigger(PlayerbotAI* botAI) : SpellNoCooldownTrigger(botAI, "hysteria") {}
};

// [rot_b31_pdd] Summon Gargoyle (49206: 60 runic power, 3 min, 30 s): on a boss, a player, under Bloodlust or on
// an elite the group needs at least GARGOYLE_MIN_LIFETIME_S for, with the 60 runic power in hand. The generic
// BoostTrigger waited for "balance" <= 50 (a fight the group is losing) and cast it 3 times in 28.7 min at level 80.
inline constexpr uint32 GARGOYLE_RUNIC_POWER = 600;     // internal units (x10)
inline constexpr uint32 DEATH_COIL_RUNIC_POWER = 400;
inline constexpr float GARGOYLE_MIN_LIFETIME_S = 25.0f;

// A fight Summon Gargoyle is saved for (no spell or cooldown check).
bool IsGargoyleFight(PlayerbotAI* botAI);
// Summon Gargoyle known, off cooldown and the fight is one for it: Death Coil pools runic power.
bool IsGargoyleWaiting(PlayerbotAI* botAI);

class SummonGargoyleTrigger : public Trigger
{
public:
    SummonGargoyleTrigger(PlayerbotAI* botAI) : Trigger(botAI, "summon gargoyle") {}

    bool IsActive() override;
};

// [b35_uh] Summon Gargoyle on trash inside a dungeon or raid: the elite branch of IsGargoyleFight (an elite living
// GARGOYLE_MIN_LIFETIME_S) spent the 3 min cooldown on trash, so the Gargoyle was down on the boss that came next
// (DEEP_AUDIT_DK_UNHOLY_NIGHT.md B8: 6 of 72 boss kills without it, Moorabi's Gargoyle 138 s before the boss pull
// ended). In an instance a non-boss fight now needs the whole pack (every live attacker's health over the group's
// estimated dps) to last GARGOYLE_INSTANCE_PACK_LIFETIME_S: half the cooldown spent fighting that pack alone, with the
// walk / rest / next pulls on top, leaves the Gargoyle back for the next boss in the usual case, and the pack itself
// takes the full 30 s of the Gargoyle. Open world keeps the elite rule (no boss to save it for).
inline constexpr float GARGOYLE_INSTANCE_PACK_LIFETIME_S = 90.0f;

// [b35_uh] Empower Rune Weapon (47568, 5 min): every ready rune and 25 runic power at once.
inline constexpr uint32 SPELL_DK_EMPOWER_RUNE_WEAPON = 47568;
// [b35_uh] At most this many ready runes (any type) when Empower Rune Weapon fires under the Gargoyle.
inline constexpr uint8 ERW_MAX_READY_RUNES = 1;
// [b35_uh] wowsims unholy_dw_ss: Scourge Strike / Blood Strike / Blood Boil only while Death and Decay is more than
// this far from ready.
inline constexpr uint32 DK_DND_POOL_MS = 6000;
// [b35_uh] Death and Decay must still tick this long on the target for the pooling to pay: its 10 s (~6545 damage in
// the sim run of the audit) against the Scourge Strike + Blood Strike pair it displaces (~3890): 6545 * T / 10 >= 3890
// gives T >= 5.9 s.
inline constexpr float DK_DND_MIN_TICK_S = 6.0f;

// [b35_uh] Ready runes of any type (MAX_RUNES = 6 slots).
uint8 CountReadyRunes(Player* bot);
// [b35_uh] In combat with a live current target that is a player or a dungeon / world boss.
bool IsDKBossFight(PlayerbotAI* botAI);
// [rot2] [rot3] The dungeon leader's cooldown rule for a led bot: true, `burn` = a fresh Burn (no
// Stop; a stale command: false). False: not led, the bot's own fight test stands.
bool LeaderBurnRule(Player* bot, bool& burn);
// [b35_uh] Summon Gargoyle cast less than GARGOYLE_DURATION_MS ago (its cooldown started then).
bool IsGargoyleOut(Player* bot);
// [b35_uh] No Gargoyle this fight: not known, or not out and its cooldown outlasts the current target.
bool IsGargoyleUnavailable(PlayerbotAI* botAI);

// [b35_uh] Unholy's Empower Rune Weapon (wowsims unholy_dw_ss: "ERW if the Gargoyle is active", below the rune
// spenders, so it comes once they are out of runes). On a boss (or a player) only, with the Gargoyle out and at most
// ERW_MAX_READY_RUNES ready; without a Gargoyle this fight (IsGargoyleUnavailable) on the boss alone. Its 5 min
// cooldown makes that once per dungeon boss fight. "no rune" (any fight) burnt it on trash: ~100 of 124 casts, 24 of 66
// boss kills with it (DEEP_AUDIT_DK_UNHOLY_NIGHT.md B3).
class EmpowerRuneWeaponUnderGargoyleTrigger : public Trigger
{
public:
    EmpowerRuneWeaponUnderGargoyleTrigger(PlayerbotAI* botAI) : Trigger(botAI, "empower rune weapon under gargoyle") {}

    bool IsActive() override;
};

// [b35_uh] Frost's Empower Rune Weapon: every rune on cooldown, on a boss (or a player) only. wowsims frost_bl_pesti
// casts it once, in the boss opener sequence after the runes are spent.
class NoRuneBossTrigger : public Trigger
{
public:
    NoRuneBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no rune boss") {}

    bool IsActive() override;
};

// [b35_uh] Unholy's Army of the Dead (wowsims unholy_dw_ss has no prepull Army; in combat "AotD if the Gargoyle is
// active", below Death and Decay). Boss only (ArmyOfTheDeadTrigger), the runes for it, and the Gargoyle out, or no
// Gargoyle this fight. The plain "army of the dead" trigger (Frost: a prepull Army in wowsims) cast it as the boss pull
// started, a 4 s channel and 3 runes before the diseases (DEEP_AUDIT_DK_UNHOLY_NIGHT.md B4).
class ArmyOfTheDeadUnderGargoyleTrigger : public ArmyOfTheDeadTrigger
{
public:
    ArmyOfTheDeadUnderGargoyleTrigger(PlayerbotAI* botAI)
        : ArmyOfTheDeadTrigger(botAI, "army of the dead under gargoyle")
    {
    }

    bool IsActive() override;
};

// [b35_uh] The wowsims unholy_dw_ss Army above Scourge Strike and Death and Decay: Gargoyle active, Empower Rune Weapon
// spent and exactly one non-Death Frost rune ready; below the disease refreshes (both of ours on the target with more
// than DK_DISEASE_REFRESH_MS left).
class ArmyOfTheDeadLastFrostRuneTrigger : public ArmyOfTheDeadTrigger
{
public:
    ArmyOfTheDeadLastFrostRuneTrigger(PlayerbotAI* botAI)
        : ArmyOfTheDeadTrigger(botAI, "army of the dead last frost rune")
    {
    }

    bool IsActive() override;
};

// [rot_b31_pdd] Out of combat Horn of Winter whenever it is off cooldown (10 runic power each, 20 s), not only when
// its buff is missing: a tank about to pull starts with runic power for Anti-Magic Shell / Icebound Fortitude (20
// each). Runic power decays out of combat (3 per 2 s tick), so it pays only right before the pull: the tank keeps it
// to the moment its group is ready for the next pull (in a group, the tank only).
class HornOfWinterReadyTrigger : public Trigger
{
public:
    HornOfWinterReadyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "horn of winter ready") {}

    bool IsActive() override;
};

// [rot10] Out of combat a group member within Horn of Winter's radius has neither Horn of Winter nor Strength of
// Earth (the DK's own buff can still run while a member who came later lacks it). A scan of the group (5 members).
class HornOfWinterOnPartyTrigger : public Trigger
{
public:
    HornOfWinterOnPartyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "horn of winter on party") {}

    bool IsActive() override;
};

// The current target is casting a magic (non-physical) spell at the bot.
class AntiMagicShellTrigger : public SpellTrigger
{
public:
    AntiMagicShellTrigger(PlayerbotAI* botAI) : SpellTrigger(botAI, "anti-magic shell") {}

    bool IsActive() override;
};

// The tank's target holds at range: a caster casting, a mob running away, or one standing to shoot. A mob still
// walking in is left to come; Death Grip on cooldown does not fire (its "icy touch" alternative is not wanted here).
class DeathGripFarTargetTrigger : public Trigger
{
public:
    static constexpr float MIN_GRIP_DISTANCE = 10.0f;

    DeathGripFarTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "death grip far target") {}

    bool IsActive() override;
};

// Blood tank cooldown thresholds, staggered so they are not all spent on the same hit.
// [b31_dk] Raised from 50 / 40 / 35: build 30 L80 Blood DKs used Rune Tap, Vampiric Blood and Icebound Fortitude
// 0.08/min each (GUIDE_AUDIT_TANKS.md section 3). Rune Tap (a 10 % self-heal on a 30-60 s cooldown) is a frequent tool,
// Vampiric Blood a medium one, Icebound Fortitude the last line. CastVampiricBloodAction / CastIceboundFortitudeAction
// do not stack the two while health is at or above DK_ICEBOUND_FORTITUDE_HEALTH_PCT.
constexpr float DK_RUNE_TAP_HEALTH_PCT = 70.0f;
constexpr float DK_VAMPIRIC_BLOOD_HEALTH_PCT = 50.0f;
constexpr float DK_ICEBOUND_FORTITUDE_HEALTH_PCT = 35.0f;

// [b31_dk] Aura ids for the overlap guards and the Death Pact gate.
inline constexpr uint32 SPELL_DK_ICEBOUND_FORTITUDE = 48792;
inline constexpr uint32 SPELL_DK_VAMPIRIC_BLOOD = 55233;
inline constexpr uint32 SPELL_DK_MASTER_OF_GHOULS = 52143;  // the Unholy talent: the ghoul is a permanent pet
inline constexpr uint32 SPELL_DK_GLYPH_OF_DISEASE = 63334;
// [b31_dk] With the permanent ghoul Death Pact only below this health and with no living healer in the group.
constexpr float DK_DEATH_PACT_PERMANENT_GHOUL_HEALTH_PCT = 15.0f;
// [b31_dk] The disease refresh window of the "icy touch 3s" / "plague strike 3s" / "pestilence glyph" triggers.
constexpr int32 DK_DISEASE_REFRESH_MS = 3000;

class RuneTapHealthTrigger : public LowHealthTrigger
{
public:
    RuneTapHealthTrigger(PlayerbotAI* botAI) : LowHealthTrigger(botAI, "rune tap health", DK_RUNE_TAP_HEALTH_PCT) {}
};

class VampiricBloodHealthTrigger : public LowHealthTrigger
{
public:
    VampiricBloodHealthTrigger(PlayerbotAI* botAI)
        : LowHealthTrigger(botAI, "vampiric blood health", DK_VAMPIRIC_BLOOD_HEALTH_PCT)
    {
    }
};

class IceboundFortitudeHealthTrigger : public LowHealthTrigger
{
public:
    IceboundFortitudeHealthTrigger(PlayerbotAI* botAI)
        : LowHealthTrigger(botAI, "icebound fortitude health", DK_ICEBOUND_FORTITUDE_HEALTH_PCT)
    {
    }
};

// [rot1] Blood's long cooldowns as the wowsims blood_aggro APL uses them (tools/simpct_data/vendor/apl/
// tank_deathknight__blood_aggro.apl.json): Hysteria and Dancing Rune Weapon on cooldown above Death Strike / Heart
// Strike, Raise Dead below them, Empower Rune Weapon below Raise Dead (no rune left), Army of the Dead pre-pull.
// The sim scores one boss fight; the bots ran Hysteria (3 min), Raise Dead (3 min) and Empower Rune Weapon (5 min) on
// any trash pack that qualified and met the boss with them on cooldown (sim: 1.28/min each, bots 0.30 / 0 / 0.39,
// abilities_blood_dk.md). A dungeon's bosses are 5+ min apart in the usual run: saving these for a boss fight (or the
// leader's burn window) loses no use, and the boss fight is where the kill time is scored.
bool IsDKCooldownFight(PlayerbotAI* botAI);

// [rot1] Dancing Rune Weapon (49028, 60 runic power, 1.5 min) ready with the runic power for it and a target in
// melee. PlayerbotAI::CanCastSpell ignores the runic power cost.
inline constexpr uint32 SPELL_DK_DANCING_RUNE_WEAPON = 49028;
inline constexpr uint32 DANCING_RUNE_WEAPON_RUNIC_POWER = 600;  // internal units (x10)

class DancingRuneWeaponReadyTrigger : public Trigger
{
public:
    DancingRuneWeaponReadyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dancing rune weapon ready") {}

    bool IsActive() override;
};

// [rot1] Hysteria off cooldown in a cooldown fight (IsDKCooldownFight).
class HysteriaCooldownFightTrigger : public HysteriaNoCooldownTrigger
{
public:
    HysteriaCooldownFightTrigger(PlayerbotAI* botAI) : HysteriaNoCooldownTrigger(botAI) {}

    std::string const getName() override { return "hysteria boss"; }
    bool IsActive() override;
};

// [rot1] The glyphed Raise Dead (RaiseDeadBoostTrigger) in a cooldown fight only.
class RaiseDeadBossTrigger : public RaiseDeadBoostTrigger
{
public:
    RaiseDeadBossTrigger(PlayerbotAI* botAI) : RaiseDeadBoostTrigger(botAI) {}

    std::string const getName() override { return "raise dead boss"; }
    bool IsActive() override;
};

// [rot1] Empower Rune Weapon outside a boss fight only as the tank's last defence: every rune down and the knight
// below low health (the runes go to Death Strike / Rune Tap).
class NoRuneLowHealthTrigger : public NoRuneTrigger
{
public:
    NoRuneLowHealthTrigger(PlayerbotAI* botAI) : NoRuneTrigger(botAI) {}

    std::string const getName() override { return "no rune low health"; }
    bool IsActive() override;
};

#endif
