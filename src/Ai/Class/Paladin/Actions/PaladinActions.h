/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PALADINACTIONS_H
#define PLAYERBOTS_PALADINACTIONS_H

#include "AiObject.h"
#include "DamageSpikeActions.h"
#include "GenericSpellActions.h"

// seals
BUFF_ACTION(CastSealOfRighteousnessAction, "seal of righteousness");
BUFF_ACTION(CastSealOfJusticeAction, "seal of justice");
BUFF_ACTION(CastSealOfLightAction, "seal of light");
BUFF_ACTION(CastSealOfWisdomAction, "seal of wisdom");
BUFF_ACTION(CastSealOfCommandAction, "seal of command");
BUFF_ACTION(CastSealOfVengeanceAction, "seal of vengeance");
BUFF_ACTION(CastSealOfCorruptionAction, "seal of corruption");

// judgements
SPELL_ACTION(CastJudgementAction, "judgement");

SPELL_ACTION(CastJudgementOfLightAction, "judgement of light");
SPELL_ACTION(CastJudgementOfWisdomAction, "judgement of wisdom");
SPELL_ACTION(CastJudgementOfJusticeAction, "judgement of justice");

// auras
BUFF_ACTION(CastDevotionAuraAction, "devotion aura");
BUFF_ACTION(CastRetributionAuraAction, "retribution aura");
BUFF_ACTION(CastConcentrationAuraAction, "concentration aura");
BUFF_ACTION(CastShadowResistanceAuraAction, "shadow resistance aura");
BUFF_ACTION(CastFrostResistanceAuraAction, "frost resistance aura");
BUFF_ACTION(CastFireResistanceAuraAction, "fire resistance aura");
BUFF_ACTION(CastCrusaderAuraAction, "crusader aura");
// [b31_pal] "sanctity aura" is not in the 3.3.5 Spell.dbc: no creator registers this action any more; the type stays
// because HoRMultipliers.cpp:39 and RampMultipliers.cpp:128 name it in their aura dynamic_casts.
BUFF_ACTION(CastSanctityAuraAction, "sanctity aura");

SPELL_ACTION(CastHolyShockAction, "holy shock");

// consecration
MELEE_ACTION(CastConsecrationAction, "consecration");

// [b31_deep] Retribution's single-target Consecration (wowsims default APL): only while the fight lasts 4 s more
// ([rot281] remainingTime: the longest ExpectedLifetime of the target and the attackers); the AoE trigger keeps the
// plain "consecration".
class CastRetConsecrationAction : public CastConsecrationAction
{
public:
    static constexpr float MIN_TARGET_LIFE_SECS = 4.0f;

    CastRetConsecrationAction(PlayerbotAI* botAI) : CastConsecrationAction(botAI) {}

    std::string const getName() override { return "ret consecration"; }
    bool isUseful() override;
};

// repentance
SNARE_ACTION(CastRepentanceSnareAction, "repentance");
DEBUFF_ACTION(CastRepentanceAction, "repentance");
ENEMY_HEALER_ACTION(CastRepentanceOnHealerAction, "repentance");

// hamme of wrath
SPELL_ACTION(CastHammerOfWrathAction, "hammer of wrath");

// buffs
BUFF_ACTION(CastDivineFavorAction, "divine favor");

// blessings
// fury
BUFF_ACTION(CastRighteousFuryAction, "righteous fury");

class CastDivineStormAction : public CastMeleeSpellAction
{
public:
    CastDivineStormAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "divine storm") {}
};

class CastCrusaderStrikeAction : public CastMeleeSpellAction
{
public:
    CastCrusaderStrikeAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "crusader strike") {}
};

class CastSealSpellAction : public CastBuffSpellAction
{
public:
    CastSealSpellAction(PlayerbotAI* botAI, std::string const name) : CastBuffSpellAction(botAI, name) {}

    bool isUseful() override;
};

class CastHolyLightAction : public CastHealingSpellAction
{
public:
    CastHolyLightAction(PlayerbotAI* botAI) : CastHealingSpellAction(botAI, "holy light") {}
};

class CastHolyShockOnPartyAction : public HealPartyMemberAction
{
public:
    CastHolyShockOnPartyAction(PlayerbotAI* botAI)
        : HealPartyMemberAction(botAI, "holy shock", 25.0f, HealingManaEfficiency::LOW) {}
};

class CastHolyLightOnPartyAction : public HealPartyMemberAction
{
public:
    CastHolyLightOnPartyAction(PlayerbotAI* botAI)
        : HealPartyMemberAction(botAI, "holy light", 50.0f, HealingManaEfficiency::MEDIUM) {}
};

// [b31_deep] Infusion of Light rank 2 (53576) puts a 12 s heal over time named "Flash of Light" (66922) on a target
// with Sacred Shield. CastHealingSpellAction::isUseful is the aura spell check (the bot's own "flash of light" aura
// on the target must be missing), so the Sacred-Shielded tank took no Flash of Light for 12 s after each one. A
// heal is useful with that aura up: the aura test is skipped.
class CastFlashOfLightAction : public CastHealingSpellAction
{
public:
    CastFlashOfLightAction(PlayerbotAI* botAI) : CastHealingSpellAction(botAI, "flash of light") {}

    bool isUseful() override { return GetTarget() && CastSpellAction::isUseful(); }
};

class CastFlashOfLightOnPartyAction : public HealPartyMemberAction
{
public:
    CastFlashOfLightOnPartyAction(PlayerbotAI* botAI)
        : HealPartyMemberAction(botAI, "flash of light", 15.0f, HealingManaEfficiency::HIGH) {}

    bool isUseful() override { return GetTarget() && CastSpellAction::isUseful(); }  // [b31_deep]
};

class CastLayOnHandsAction : public CastHealingSpellAction
{
public:
    CastLayOnHandsAction(PlayerbotAI* botAI) : CastHealingSpellAction(botAI, "lay on hands") {}
};

class CastLayOnHandsOnPartyAction : public HealPartyMemberAction
{
public:
    CastLayOnHandsOnPartyAction(PlayerbotAI* botAI) : HealPartyMemberAction(botAI, "lay on hands") {}

    bool isUseful() override;
};

class CastDivineProtectionAction : public CastBuffSpellAction
{
public:
    CastDivineProtectionAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "divine protection") {}
};

class CastDivineProtectionOnPartyAction : public HealPartyMemberAction
{
public:
    CastDivineProtectionOnPartyAction(PlayerbotAI* botAI) : HealPartyMemberAction(botAI, "divine protection") {}

    std::string const getName() override { return "divine protection on party"; }
};

class CastDivineShieldAction : public CastBuffSpellAction
{
public:
    CastDivineShieldAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "divine shield") {}

    bool isUseful() override;
};

namespace ai::paladin
{
// [b31_deep] Holy Wrath hits and stuns Undead and Demons only (Spell.dbc TargetCreatureType 36 on 2812...48817):
// the current target or a cached attacker of that type within range of the bot.
bool HasHolyWrathTargetWithin(PlayerbotAI* botAI, Player* bot, float range);

// [b34_pal] Undead and demons among the current target and the cached attackers within `range` of the bot, each once.
uint32 CountHolyWrathTargetsWithin(PlayerbotAI* botAI, Player* bot, float range);

// [b34_pal] A possible target within Holy Wrath's 10 yd that its damage must not touch: a breakable crowd control
// (Shackle Undead, Polymorph, Sap, Repentance...) or a mob not in combat yet (the burst pulls its pack).
bool HolyWrathWouldBreakCcOrPull(PlayerbotAI* botAI, Player* bot);

// [b34_pal] An undead / demon pack around the paladin: 2+ within 8 yd (Holy Wrath reaches 10).
constexpr float HOLY_WRATH_PACK_RANGE = 8.0f;
constexpr uint32 HOLY_WRATH_PACK_MIN = 2;
}

class CastHolyWrathAction : public CastMeleeSpellAction
{
public:
    static constexpr float HOLY_WRATH_RADIUS = 10.0f;

    CastHolyWrathAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "holy wrath") {}

    // [b31_deep] the tank's "medium aoe" queued it on any pack: a GCD and ~20 % base mana for no damage
    bool isUseful() override;
};

// [b31_deep] "holy wrath on group member attackers": as the base action (a tank, attackers within 10 yd), with an
// undead or demon among them.
class CastHolyWrathOnGroupMemberAttackersAction : public CastAreaSpellOnGroupMemberAttackersAction
{
public:
    CastHolyWrathOnGroupMemberAttackersAction(PlayerbotAI* botAI)
        : CastAreaSpellOnGroupMemberAttackersAction(botAI, "holy wrath", CastHolyWrathAction::HOLY_WRATH_RADIUS)
    {
    }

    bool isUseful() override;
};

// [b34_pal] "holy wrath undead pack": Holy Wrath around the paladin itself, whatever and wherever the current target
// is (CastHolyWrathAction is a melee spell action: with the tank's target on the healer 23 yd away it was useless
// while 20+ undead stood on the tank, strat-h-5E-1 pull 2: 0 casts in 34 s). 2+ undead / demons within 8 yd, no
// breakable crowd control and no idle mob within its 10 yd.
class CastHolyWrathUndeadPackAction : public CastSpellAction
{
public:
    CastHolyWrathUndeadPackAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "holy wrath") {}

    std::string const GetTargetName() override { return "self target"; }
    std::string const getName() override { return "holy wrath undead pack"; }
    bool isUseful() override;
};

class CastHammerOfJusticeAction : public CastMeleeSpellAction
{
public:
    CastHammerOfJusticeAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "hammer of justice") {}
};

class CastHammerOfTheRighteousAction : public CastMeleeSpellAction
{
public:
    CastHammerOfTheRighteousAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "hammer of the righteous") {}
};

class CastPurifyPoisonAction : public CastCureSpellAction
{
public:
    CastPurifyPoisonAction(PlayerbotAI* botAI) : CastCureSpellAction(botAI, "purify") {}
};

class CastPurifyDiseaseAction : public CastCureSpellAction
{
public:
    CastPurifyDiseaseAction(PlayerbotAI* botAI) : CastCureSpellAction(botAI, "purify") {}
};

class CastPurifyPoisonOnPartyAction : public CurePartyMemberAction
{
public:
    CastPurifyPoisonOnPartyAction(PlayerbotAI* botAI) : CurePartyMemberAction(botAI, "purify", DISPEL_POISON) {}

    std::string const getName() override { return "purify poison on party"; }
};

class CastPurifyDiseaseOnPartyAction : public CurePartyMemberAction
{
public:
    CastPurifyDiseaseOnPartyAction(PlayerbotAI* botAI) : CurePartyMemberAction(botAI, "purify", DISPEL_DISEASE) {}

    std::string const getName() override { return "purify disease on party"; }
};

class CastHandOfReckoningAction : public CastSpellAction
{
public:
    CastHandOfReckoningAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "hand of reckoning") {}
};

class CastRighteousDefenseAction : public CastSpellAction
{
public:
    CastRighteousDefenseAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "righteous defense") {}
    virtual Unit* GetTarget() override;
};

class CastCleansePoisonAction : public CastCureSpellAction
{
public:
    CastCleansePoisonAction(PlayerbotAI* botAI) : CastCureSpellAction(botAI, "cleanse") {}
};

class CastCleanseDiseaseAction : public CastCureSpellAction
{
public:
    CastCleanseDiseaseAction(PlayerbotAI* botAI) : CastCureSpellAction(botAI, "cleanse") {}
};

class CastCleanseMagicAction : public CastCureSpellAction
{
public:
    CastCleanseMagicAction(PlayerbotAI* botAI) : CastCureSpellAction(botAI, "cleanse") {}
};

class CastCleansePoisonOnPartyAction : public CurePartyMemberAction
{
public:
    CastCleansePoisonOnPartyAction(PlayerbotAI* botAI) : CurePartyMemberAction(botAI, "cleanse", DISPEL_POISON) {}

    std::string const getName() override { return "cleanse poison on party"; }
};

class CastCleanseDiseaseOnPartyAction : public CurePartyMemberAction
{
public:
    CastCleanseDiseaseOnPartyAction(PlayerbotAI* botAI) : CurePartyMemberAction(botAI, "cleanse", DISPEL_DISEASE) {}

    std::string const getName() override { return "cleanse disease on party"; }
};

class CastCleanseMagicOnPartyAction : public CurePartyMemberAction
{
public:
    CastCleanseMagicOnPartyAction(PlayerbotAI* botAI) : CurePartyMemberAction(botAI, "cleanse", DISPEL_MAGIC) {}

    std::string const getName() override { return "cleanse magic on party"; }
};

BEGIN_SPELL_ACTION(CastAvengersShieldAction, "avenger's shield")
END_SPELL_ACTION()

BEGIN_SPELL_ACTION(CastExorcismAction, "exorcism")
END_SPELL_ACTION()

class CastHolyShieldAction : public CastBuffSpellAction
{
public:
    CastHolyShieldAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "holy shield") {}

    // [holy_shield_in_combat] only in a fight: out of combat it drained a tank holding an idle target to 0 % mana
    // (ac-n build 27, Rlpalba 89 % -> 0 %), so it could never become ready to pull.
    bool isUseful() override { return bot->IsInCombat() && CastBuffSpellAction::isUseful(); }
};

// [rot280] Holy Shield on its cooldown in a fight, its aura up or not (wowsims protection_paladin__default:
// castSpell 48952 on spellIsReady, after Consecration, before Judgement). The recast renews the 10 s aura and its 8
// charges; the plain "holy shield" keeps the cast-when-missing upkeep for the area-threat trigger.
class CastHolyShieldOnCooldownAction : public CastHolyShieldAction
{
public:
    CastHolyShieldOnCooldownAction(PlayerbotAI* botAI) : CastHolyShieldAction(botAI) {}

    std::string const getName() override { return "holy shield on cooldown"; }
    bool isUseful() override { return bot->IsInCombat() && CastSpellAction::isUseful(); }
};

// [b31_holyshield_pull] Holy Shield as the first step of the tank's own pull (wowsims casts it pre-pull). Never
// queued by a trigger: PullAction runs it at the pull commit only (PaladinPullStrategy::GetPrePullBuffActionName),
// once per pull, so a tank waiting or holding an idle target does not cast it ([holy_shield_in_combat] above).
class CastHolyShieldPrePullAction : public CastBuffSpellAction
{
public:
    CastHolyShieldPrePullAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "holy shield") {}

    static constexpr float MIN_MANA_PCT = 30.0f;

    bool isUseful() override;
    bool Execute(Event event) override;
};

class CastRedemptionAction : public ResurrectPartyMemberAction
{
public:
    CastRedemptionAction(PlayerbotAI* botAI) : ResurrectPartyMemberAction(botAI, "redemption") {}
};

class CastHammerOfJusticeOnEnemyHealerAction : public CastSpellOnEnemyHealerAction
{
public:
    CastHammerOfJusticeOnEnemyHealerAction(PlayerbotAI* botAI)
        : CastSpellOnEnemyHealerAction(botAI, "hammer of justice") {}
};

class CastHammerOfJusticeSnareAction : public CastSnareSpellAction
{
public:
    CastHammerOfJusticeSnareAction(PlayerbotAI* botAI) : CastSnareSpellAction(botAI, "hammer of justice") {}
};

class CastSenseUndeadAction : public CastBuffSpellAction
{
public:
    CastSenseUndeadAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "sense undead") {}
};

class CastTurnUndeadAction : public CastBuffSpellAction
{
public:
    // [b34_pal] Turn Evil (10326) replaced Turn Undead in 3.0; the strategy keys stay "turn undead"
    CastTurnUndeadAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "turn evil") {}

    Value<Unit*>* GetTargetValue() override;
};

class CastHandOfFreedomOnPartyAction : public CastBuffSpellAction, public PartyMemberActionNameSupport
{
public:
    CastHandOfFreedomOnPartyAction(PlayerbotAI* botAI)
        : CastBuffSpellAction(botAI, "hand of freedom"), PartyMemberActionNameSupport("hand of freedom") {}

    Unit* GetTarget() override;
    Value<Unit*>* GetTargetValue() override;
    std::string const GetTargetName() override { return "party member snared target"; }
    std::string const getName() override { return PartyMemberActionNameSupport::getName(); }
    bool isUseful() override;
};

PROTECT_ACTION(CastBlessingOfProtectionProtectAction, "blessing of protection");

// [rot_b31_pdd] "seal state lost": the seal without its aura state is dropped, and the spec's "seal" trigger casts
// it again on the next tick (a fresh application sets AURA_STATE_JUDGEMENT; a recast of the same seal only
// refreshes the old aura and would not).
class DropStaleSealAction : public Action
{
public:
    DropStaleSealAction(PlayerbotAI* botAI) : Action(botAI, "drop stale seal") {}

    bool Execute(Event event) override;
};

class CastDivinePleaAction : public CastBuffSpellAction
{
public:
    CastDivinePleaAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "divine plea") {}

    bool isUseful() override;
};

class ShieldOfRighteousnessAction : public CastMeleeSpellAction
{
public:
    ShieldOfRighteousnessAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "shield of righteousness") {}
};

// [b31_deep] Beacon of Light and Sacred Shield are friendly buffs: ActionThreatType::None, so the threat multiplier
// (it measured the healer's threat on its current mob) no longer drops their upkeep (build 30 L80: Sacred Shield 47
// of 135 refusals, Beacon 20 of 58, "multiplier threat"). Beacon is renewed BEACON_REFRESH_MS before it ends.
class CastBeaconOfLightOnMainTankAction : public BuffOnMainTankAction
{
public:
    static constexpr uint32 BEACON_REFRESH_MS = 3 * IN_MILLISECONDS;

    CastBeaconOfLightOnMainTankAction(PlayerbotAI* botAI) : BuffOnMainTankAction(botAI, "beacon of light", true)
    {
        beforeDuration = BEACON_REFRESH_MS;
    }

    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastHandOfSalvationOnPartyAction : public CastSpellAction
{
public:
    CastHandOfSalvationOnPartyAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "hand of salvation") {}

    std::string const GetTargetName() override { return "hand of salvation target"; }
    std::string const getName() override { return "hand of salvation on party"; }
};

class CastHandOfSacrificeOnMainTankAction : public BuffOnMainTankAction
{
public:
    CastHandOfSacrificeOnMainTankAction(PlayerbotAI* botAI) : BuffOnMainTankAction(botAI, "hand of sacrifice", false) {}
};

// Hand of Sacrifice on the tank picked by "tank damage spike"; a tank paladin or a hurt paladin keeps out.
class CastHandOfSacrificeOnSpikingTankAction : public CastSpikeProtectSpellAction
{
public:
    CastHandOfSacrificeOnSpikingTankAction(PlayerbotAI* botAI) : CastSpikeProtectSpellAction(botAI, "hand of sacrifice")
    {
    }

    std::string const getName() override { return "hand of sacrifice on spiking tank"; }
    bool isUseful() override;
};

class CastAuraMasteryAction : public CastBuffSpellAction
{
public:
    CastAuraMasteryAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "aura mastery") {}
};

class CastSacredShieldOnMainTankAction : public BuffOnMainTankAction
{
public:
    CastSacredShieldOnMainTankAction(PlayerbotAI* botAI) : BuffOnMainTankAction(botAI, "sacred shield", false) {}

    ActionThreatType getThreatType() override { return ActionThreatType::None; }  // [b31_deep]
};

class CastAvengingWrathAction : public CastBuffSpellAction
{
public:
    CastAvengingWrathAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "avenging wrath") {}

    // [b31_pal] not under Forbearance (25771) or the Avenging Wrath Marker (61987) that Divine Shield, Divine
    // Protection, Hand of Protection and Lay on Hands on self put up with it (spell_pal_immunities,
    // spell_pal_lay_on_hands; Spell.dbc ExcludeCasterAuraSpell 61987 on 31884): two aura lookups instead of a
    // Spell::CheckCast every tick of the boost trigger.
    bool isUseful() override;
};

class CastDivineIlluminationAction : public CastBuffSpellAction
{
public:
    CastDivineIlluminationAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "divine illumination") {}
};

class CastDivineSacrificeAction : public CastBuffSpellAction
{
public:
    CastDivineSacrificeAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "divine sacrifice") {}
    bool isUseful() override;
};

class CastCancelDivineSacrificeAction : public Action
{
public:
    CastCancelDivineSacrificeAction(PlayerbotAI* botAI) : Action(botAI, "cancel divine sacrifice") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
