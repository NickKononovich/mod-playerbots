/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PALADINTRIGGERS_H
#define PLAYERBOTS_PALADINTRIGGERS_H

#include "CureTriggers.h"
#include "GenericTriggers.h"
#include "SharedDefines.h"
#include "Unit.h"
#include "Value.h"  // [b31_deep] BoolCalculatedValue

class PlayerbotAI;

BUFF_TRIGGER(HolyShieldTrigger, "holy shield");
BUFF_TRIGGER(RighteousFuryTrigger, "righteous fury");

BUFF_TRIGGER(RetributionAuraTrigger, "retribution aura");
// [b31_pal] "sanctity aura" removed: the TBC aura is not in the 3.3.5 Spell.dbc (Sanctified Retribution replaced it)
// and no strategy queued it.

class CrusaderAuraTrigger : public BuffTrigger
{
public:
    CrusaderAuraTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "crusader aura") {}

    bool IsActive() override;
};

class SealTrigger : public BuffTrigger
{
public:
    SealTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "seal") {}

    bool IsActive() override;
};

// judgements
DEBUFF_TRIGGER(JudgementTrigger, "judgement");
DEBUFF_TRIGGER(JudgementOfLightTrigger, "judgement of light");
DEBUFF_TRIGGER(JudgementOfWisdomTrigger, "judgement of wisdom");

DEBUFF_TRIGGER(ConsecrationTrigger, "consecration");

// repentance triggers
INTERRUPT_HEALER_TRIGGER(RepentanceOnHealerTrigger, "repentance on enemy healer");
SNARE_TRIGGER(RepentanceSnareTrigger, "repentance on snare target");
INTERRUPT_TRIGGER(RepentanceInterruptTrigger, "repentance");

class HammerOfJusticeInterruptSpellTrigger : public InterruptSpellTrigger
{
public:
    HammerOfJusticeInterruptSpellTrigger(PlayerbotAI* botAI)
        : InterruptSpellTrigger(botAI, "hammer of justice") {}
};

class HammerOfJusticeSnareTrigger : public SnareTargetTrigger
{
public:
    HammerOfJusticeSnareTrigger(PlayerbotAI* botAI) : SnareTargetTrigger(botAI, "hammer of justice") {}
};

class ArtOfWarTrigger : public HasAuraTrigger
{
public:
    ArtOfWarTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "the art of war") {}
};

class ShadowResistanceAuraTrigger : public BuffTrigger
{
public:
    ShadowResistanceAuraTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "shadow resistance aura") {}
};

class FrostResistanceAuraTrigger : public BuffTrigger
{
public:
    FrostResistanceAuraTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "frost resistance aura") {}
};

class FireResistanceAuraTrigger : public BuffTrigger
{
public:
    FireResistanceAuraTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "fire resistance aura") {}
};

class DevotionAuraTrigger : public BuffTrigger
{
public:
    DevotionAuraTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "devotion aura") {}
};

BUFF_TRIGGER(ConcentrationAuraTrigger, "concentration aura");

class CleanseCureDiseaseTrigger : public NeedCureTrigger
{
public:
    CleanseCureDiseaseTrigger(PlayerbotAI* botAI) : NeedCureTrigger(botAI, "cleanse", DISPEL_DISEASE) {}
};

class CleanseCurePartyMemberDiseaseTrigger : public PartyMemberNeedCureTrigger
{
public:
    CleanseCurePartyMemberDiseaseTrigger(PlayerbotAI* botAI)
        : PartyMemberNeedCureTrigger(botAI, "cleanse", DISPEL_DISEASE) {}
};

class CleanseCurePoisonTrigger : public NeedCureTrigger
{
public:
    CleanseCurePoisonTrigger(PlayerbotAI* botAI) : NeedCureTrigger(botAI, "cleanse", DISPEL_POISON) {}
};

class CleanseCurePartyMemberPoisonTrigger : public PartyMemberNeedCureTrigger
{
public:
    CleanseCurePartyMemberPoisonTrigger(PlayerbotAI* botAI)
        : PartyMemberNeedCureTrigger(botAI, "cleanse", DISPEL_POISON) {}
};

class CleanseCureMagicTrigger : public NeedCureTrigger
{
public:
    CleanseCureMagicTrigger(PlayerbotAI* botAI) : NeedCureTrigger(botAI, "cleanse", DISPEL_MAGIC) {}
};

class CleanseCurePartyMemberMagicTrigger : public PartyMemberNeedCureTrigger
{
public:
    CleanseCurePartyMemberMagicTrigger(PlayerbotAI* botAI)
        : PartyMemberNeedCureTrigger(botAI, "cleanse", DISPEL_MAGIC) {}
};

class HammerOfJusticeEnemyHealerTrigger : public InterruptEnemyHealerTrigger
{
public:
    HammerOfJusticeEnemyHealerTrigger(PlayerbotAI* botAI)
        : InterruptEnemyHealerTrigger(botAI, "hammer of justice") {}
};

class DivineFavorTrigger : public BuffTrigger
{
public:
    DivineFavorTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "divine favor") {}
};

class DivineShieldLowHealthTrigger : public Trigger
{
public:
    DivineShieldLowHealthTrigger(PlayerbotAI* botAI) : Trigger(botAI, "divine shield low health") {}

    bool IsActive() override;
};

class NotSensingUndeadTrigger : public BuffTrigger
{
public:
    NotSensingUndeadTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "not sensing undead") {}

    bool IsActive() override;
};

class TurnUndeadTrigger : public HasCcTargetTrigger
{
public:
    // [b34_pal] 3.3.5 has no "Turn Undead" (replaced by Turn Evil 10326 in 3.0): "spell id" of the old name is 0
    TurnUndeadTrigger(PlayerbotAI* botAI) : HasCcTargetTrigger(botAI, "turn evil") {}
};

DEBUFF_TRIGGER(AvengerShieldTrigger, "avenger's shield");

class BeaconOfLightOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    // [b31_deep] renewed 3 s before it ends (the action's BEACON_REFRESH_MS), not only once it is gone
    static constexpr uint32 BEACON_REFRESH_MS = 3 * IN_MILLISECONDS;

    BeaconOfLightOnMainTankTrigger(PlayerbotAI* botAI) : BuffOnMainTankTrigger(botAI, "beacon of light", true)
    {
        beforeDuration = BEACON_REFRESH_MS;
    }
};

class SacredShieldOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    SacredShieldOnMainTankTrigger(PlayerbotAI* botAI)
        : BuffOnMainTankTrigger(botAI, "sacred shield", false) {}
};

// K5: a non-tank close to (or above) the tank's threat on the current target.
class HandOfSalvationOnPartyTrigger : public Trigger
{
public:
    HandOfSalvationOnPartyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hand of salvation on party") {}

    std::string const GetTargetName() override { return "hand of salvation target"; }
    bool IsActive() override;
};

// K5: the main tank is low and the paladin (not a tank itself) can take part of the damage.
class HandOfSacrificeOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    HandOfSacrificeOnMainTankTrigger(PlayerbotAI* botAI) : BuffOnMainTankTrigger(botAI, "hand of sacrifice", false) {}

    bool IsActive() override;
};

// K5: an enemy casts an area magic damage spell, or the raid is taking heavy area damage.
class AuraMasteryTrigger : public Trigger
{
public:
    AuraMasteryTrigger(PlayerbotAI* botAI) : Trigger(botAI, "aura mastery") {}

    bool IsActive() override;
};

class HandOfFreedomOnPartyTrigger : public Trigger
{
public:
    HandOfFreedomOnPartyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hand of freedom on party", 1) {}

    Unit* GetTarget() override;
    std::string const GetTargetName() override { return "party member snared target"; }
    bool IsActive() override;
};

class AvengingWrathTrigger : public BoostTrigger
{
public:
    AvengingWrathTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "avenging wrath") {}
};

class BlessingNeededTrigger : public Trigger
{
public:
    BlessingNeededTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "blessing needed", 4) {}

    bool IsBuffTrigger() override { return true; }
    bool IsActive() override { return true; }
};

// Retribution: Divine Plea on cooldown once mana is below almost full; the seal stays Vengeance/Corruption.
// [b31_pal] Not inside Avenging Wrath (its halved healing is no matter, but the GCD is) unless mana is low; the only
// Divine Plea of the spec now (the generic "medium mana" one is gone).
class RetDivinePleaTrigger : public Trigger
{
public:
    RetDivinePleaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ret divine plea") {}

    bool IsActive() override;
};

// The current target is an undead or a demon: Holy Wrath is worth its mana and global cooldown only on those.
class HolyWrathTargetTrigger : public Trigger
{
public:
    HolyWrathTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "holy wrath target") {}

    bool IsActive() override;
};

// [b34_pal] 2+ undead / demons (current target and cached attackers) within 8 yd of the paladin, whatever the current
// target is: Holy Wrath's burst and 3 s stun on the pack (the 969 AoE rotation, Ret's undead AoE slot).
class HolyWrathUndeadPackTrigger : public Trigger
{
public:
    HolyWrathUndeadPackTrigger(PlayerbotAI* botAI) : Trigger(botAI, "holy wrath undead pack") {}

    bool IsActive() override;
};

// [rot_b31_pdd] A seal aura is on the paladin, but the caster aura state it grants (AURA_STATE_JUDGEMENT, which
// every Judgement needs: Spell.dbc CasterAuraState 5 on 20271 / 53407 / 53408) is gone. Player::InitStatsForLevel
// zeroes UNIT_FIELD_AURASTATE while the auras stay (TestHarnessPrepare calls it after login, where a seal saved by
// the previous run is already back): no Judgement is possible (CheckCast CASTER_AURASTATE) until the 30 min seal
// runs out, and "seal" never fires because the seal is there. Build 30: Ret Judgement 0 in 8 of 9 boss attempts.
class SealStateLostTrigger : public Trigger
{
public:
    SealStateLostTrigger(PlayerbotAI* botAI) : Trigger(botAI, "seal state lost", 2) {}

    bool IsActive() override;
};

// [rot_b31_pdd] Holy: Divine Plea on cooldown in combat once mana is below HOLY_DIVINE_PLEA_MANA_PCT (on a boss
// from HOLY_DIVINE_PLEA_BOSS_MANA_PCT, early in the fight, as WotLK holy paladins open with it). The action itself
// holds it while a member needs a big heal or the group takes area damage (CastDivinePleaAction::isUseful).
// Build 30: 2 casts in 60 min at level 80, "medium mana" (40 %) was the only trigger.
class HolyDivinePleaTrigger : public Trigger
{
public:
    static constexpr uint8 HOLY_DIVINE_PLEA_MANA_PCT = 70;
    static constexpr uint8 HOLY_DIVINE_PLEA_BOSS_MANA_PCT = 90;

    HolyDivinePleaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "holy divine plea") {}

    bool IsActive() override;
};

// [b31_pal] Holy: Judgements of the Pure (talent 53671 / 53673 / 54151 / 54154 / 54155) gives 15 % haste for 60 s
// after a Judgement (buff 53655 / 53656 / 53657 / 54152 / 54153). The holy default action was "judgement of light"
// on every free GCD (L60 4.3/min, the most frequent cast; L72 1.9/min): one Judgement a minute keeps the buff.
// Without the talent no Judgement is queued here. Ten aura lookups on the bot, no scans.
class JudgementsOfThePureTrigger : public Trigger
{
public:
    JudgementsOfThePureTrigger(PlayerbotAI* botAI) : Trigger(botAI, "judgements of the pure", 2) {}

    bool IsActive() override;
};

// [b31_pal] Holy: Divine Illumination (halved mana cost, 3 min) on cooldown while healing, not only at 40 % mana
// ("medium mana": 0.09/min at L80 build 30, ceiling ~0.33). In combat, mana below HOLY_DI_MANA_PCT and a member
// below almost-full health (the cached "party member to heal"), or mana below HOLY_DI_ANYWAY_MANA_PCT.
class HolyDivineIlluminationTrigger : public Trigger
{
public:
    static constexpr uint8 HOLY_DI_MANA_PCT = 85;
    static constexpr uint8 HOLY_DI_ANYWAY_MANA_PCT = 50;

    HolyDivineIlluminationTrigger(PlayerbotAI* botAI) : Trigger(botAI, "holy divine illumination") {}

    bool IsActive() override;
};

// [b31_pal] Protection 969: a 6 s / 9 s ability on its cooldown ("<spell> ready"), for a current target that is
// alive; minManaPct > 0 also keeps a mana floor (Consecration). The spell id is the cached "spell id" value, 0 for a
// spell the bot does not know (Shield of Righteousness before 75, an untalented Hammer of the Righteous): the trigger
// stays off and the next one in the band takes the GCD. Build 30 L80: SoR + HotR 11.7/min of ~20, Judgement 2.9/min
// of ~6 (defaults at 5.x lost every tick to a trigger; Judgement only on a missing debuff).
class PaladinSpellReadyTrigger : public SpellTrigger
{
public:
    PaladinSpellReadyTrigger(PlayerbotAI* botAI, std::string const spell, uint8 minManaPct = 0)
        : SpellTrigger(botAI, spell), triggerName(spell + " ready"), minManaPct(minManaPct)
    {
    }

    std::string const getName() override { return triggerName; }
    bool IsActive() override;

protected:
    std::string const triggerName;
    uint8 const minManaPct;
};

// [b31_pal] Protection: Avenger's Shield on cooldown as the 969 filler, on a boss only: it bounces to two more
// enemies nearby, and on trash that may be a crowd-controlled or idle mob (trash keeps its AoE / peel triggers).
class AvengersShieldBossFillerTrigger : public PaladinSpellReadyTrigger
{
public:
    AvengersShieldBossFillerTrigger(PlayerbotAI* botAI) : PaladinSpellReadyTrigger(botAI, "avenger's shield") {}

    std::string const getName() override { return "avenger's shield boss filler"; }
    bool IsActive() override;
};

// [b31_deep] Another paladin in the group tanks (alive, on the bot's map): it keeps Judgement of Wisdom on the target,
// and one Judgement of each kind stays per target, so a retribution paladin judges Light. A group scan, cached
// PALADIN_TANK_CACHE_MS.
class PaladinTankInGroupValue : public BoolCalculatedValue
{
public:
    static constexpr uint32 PALADIN_TANK_CACHE_MS = 10 * IN_MILLISECONDS;

    PaladinTankInGroupValue(PlayerbotAI* botAI)
        : BoolCalculatedValue(botAI, "paladin tank in group", PALADIN_TANK_CACHE_MS)
    {
    }

protected:
    bool Calculate() override;
};

// [b31_deep] Retribution: the cached "paladin tank in group" and a live current target.
class PaladinTankJudgesWisdomTrigger : public Trigger
{
public:
    PaladinTankJudgesWisdomTrigger(PlayerbotAI* botAI) : Trigger(botAI, "paladin tank judges wisdom") {}

    bool IsActive() override;
};

// [b31_deep] Protection: Seal of Wisdom only below TANK_SEAL_OF_WISDOM_MANA_PCT mana.
class TankSealOfWisdomTrigger : public Trigger
{
public:
    static constexpr uint8 TANK_SEAL_OF_WISDOM_MANA_PCT = 10;

    TankSealOfWisdomTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tank seal of wisdom") {}

    bool IsActive() override;
};

// [b31_deep] Holy: the Infusion of Light proc (53672 rank 1, 54149 rank 2; two aura lookups) and a member to heal
// below almost-full health (the cached "party member to heal").
class InfusionOfLightTrigger : public Trigger
{
public:
    InfusionOfLightTrigger(PlayerbotAI* botAI) : Trigger(botAI, "infusion of light") {}

    bool IsActive() override;
};

// [b31_deep] Holy: in a boss fight (the cached "boss target"), the member to heal is a tank below almost-full health.
class HolyLightBossTankTrigger : public Trigger
{
public:
    HolyLightBossTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "holy light boss tank") {}

    bool IsActive() override;
};

// [b31_deep] Holy: Avenging Wrath ready in a boss fight while the member to heal is below the heal triage emergency
// (HealTriage::EMERGENCY_HEALTH_PCT).
class HolyAvengingWrathTrigger : public Trigger
{
public:
    HolyAvengingWrathTrigger(PlayerbotAI* botAI) : Trigger(botAI, "holy avenging wrath") {}

    bool IsActive() override;
};

#endif
