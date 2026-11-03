/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PaladinAiObjectContext.h"
#include "DpsPaladinStrategy.h"
#include "GenericPaladinNonCombatStrategy.h"
#include "HealPaladinStrategy.h"
#include "NamedObjectContext.h"
#include "OffhealRetPaladinStrategy.h"
#include "PaladinActions.h"
#include "PaladinBlessingAction.h"
#include "PaladinBuffStrategies.h"
#include "PaladinPullStrategy.h"
#include "PaladinTriggers.h"
#include "Playerbots.h"
#include "TankAoeThreat.h"
#include "TankPaladinStrategy.h"

class PaladinStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    PaladinStrategyFactoryInternal()
    {
        creators["nc"] = &PaladinStrategyFactoryInternal::nc;
        creators["pull"] = &PaladinStrategyFactoryInternal::pull;
        creators["cure"] = &PaladinStrategyFactoryInternal::cure;
        creators["boost"] = &PaladinStrategyFactoryInternal::boost;
        creators["cc"] = &PaladinStrategyFactoryInternal::cc;
        creators["bthreat"] = &PaladinStrategyFactoryInternal::bthreat;
        creators["healer dps"] = &PaladinStrategyFactoryInternal::healer_dps;
    }

private:
    static Strategy* nc(PlayerbotAI* botAI) { return new GenericPaladinNonCombatStrategy(botAI); }
    static Strategy* pull(PlayerbotAI* botAI) { return new PaladinPullStrategy(botAI); }
    static Strategy* cure(PlayerbotAI* botAI) { return new PaladinCureStrategy(botAI); }
    static Strategy* boost(PlayerbotAI* botAI) { return new PaladinBoostStrategy(botAI); }
    static Strategy* cc(PlayerbotAI* botAI) { return new PaladinCcStrategy(botAI); }
    static Strategy* bthreat(PlayerbotAI* botAI) { return new PaladinBuffThreatStrategy(botAI); }
    static Strategy* healer_dps(PlayerbotAI* botAI) { return new PaladinHealerDpsStrategy(botAI); }
};

class PaladinResistanceStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    PaladinResistanceStrategyFactoryInternal() : NamedObjectContext<Strategy>(false, true)
    {
        creators["rshadow"] = &PaladinResistanceStrategyFactoryInternal::rshadow;
        creators["rfrost"] = &PaladinResistanceStrategyFactoryInternal::rfrost;
        creators["rfire"] = &PaladinResistanceStrategyFactoryInternal::rfire;
        creators["baoe"] = &PaladinResistanceStrategyFactoryInternal::baoe;
        creators["barmor"] = &PaladinResistanceStrategyFactoryInternal::barmor;
        creators["bcast"] = &PaladinResistanceStrategyFactoryInternal::bcast;
        creators["bspeed"] = &PaladinResistanceStrategyFactoryInternal::bspeed;
    }

private:
    static Strategy* rshadow(PlayerbotAI* botAI) { return new PaladinShadowResistanceStrategy(botAI); }
    static Strategy* rfrost(PlayerbotAI* botAI) { return new PaladinFrostResistanceStrategy(botAI); }
    static Strategy* rfire(PlayerbotAI* botAI) { return new PaladinFireResistanceStrategy(botAI); }
    static Strategy* baoe(PlayerbotAI* botAI) { return new PaladinBuffAoeStrategy(botAI); }
    static Strategy* barmor(PlayerbotAI* botAI) { return new PaladinBuffArmorStrategy(botAI); }
    static Strategy* bcast(PlayerbotAI* botAI) { return new PaladinBuffCastStrategy(botAI); }
    static Strategy* bspeed(PlayerbotAI* botAI) { return new PaladinBuffSpeedStrategy(botAI); }
};

class PaladinBuffStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    PaladinBuffStrategyFactoryInternal() : NamedObjectContext<Strategy>(false, true)
    {
        creators["bsanc"] = &PaladinBuffStrategyFactoryInternal::bsanc;
        creators["bwisdom"] = &PaladinBuffStrategyFactoryInternal::bwisdom;
        creators["bmight"] = &PaladinBuffStrategyFactoryInternal::bmight;
        creators["bkings"] = &PaladinBuffStrategyFactoryInternal::bkings;
    }

private:
    static Strategy* bsanc(PlayerbotAI* botAI) { return new PaladinBuffHealthStrategy(botAI); }
    static Strategy* bwisdom(PlayerbotAI* botAI) { return new PaladinBuffManaStrategy(botAI); }
    static Strategy* bmight(PlayerbotAI* botAI) { return new PaladinBuffDpsStrategy(botAI); }
    static Strategy* bkings(PlayerbotAI* botAI) { return new PaladinBuffStatsStrategy(botAI); }
};

class PaladinCombatStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    PaladinCombatStrategyFactoryInternal() : NamedObjectContext<Strategy>(false, true)
    {
        creators["prot"] = &PaladinCombatStrategyFactoryInternal::prot;
        creators["tank"] = &PaladinCombatStrategyFactoryInternal::prot;
        creators["ret"] = &PaladinCombatStrategyFactoryInternal::ret;
        creators["dps"] = &PaladinCombatStrategyFactoryInternal::ret;
        creators["holy"] = &PaladinCombatStrategyFactoryInternal::holy;
        creators["heal"] = &PaladinCombatStrategyFactoryInternal::holy;
        creators["offheal"] = &PaladinCombatStrategyFactoryInternal::offheal;
    }

private:
    static Strategy* prot(PlayerbotAI* botAI) { return new TankPaladinStrategy(botAI); }
    static Strategy* ret(PlayerbotAI* botAI) { return new DpsPaladinStrategy(botAI); }
    static Strategy* holy(PlayerbotAI* botAI) { return new HealPaladinStrategy(botAI); }
    static Strategy* offheal(PlayerbotAI* botAI) { return new OffhealRetPaladinStrategy(botAI); }
};

class PaladinTriggerFactoryInternal : public NamedObjectContext<Trigger>
{
public:
    PaladinTriggerFactoryInternal()
    {
        creators["judgement"] = &PaladinTriggerFactoryInternal::judgement;
        creators["judgement of wisdom"] = &PaladinTriggerFactoryInternal::judgement_of_wisdom;
        creators["judgement of light"] = &PaladinTriggerFactoryInternal::judgement_of_light;
        creators["seal"] = &PaladinTriggerFactoryInternal::seal;
        creators["art of war"] = &PaladinTriggerFactoryInternal::art_of_war;
        creators["crusader aura"] = &PaladinTriggerFactoryInternal::crusader_aura;
        creators["retribution aura"] = &PaladinTriggerFactoryInternal::retribution_aura;
        creators["devotion aura"] = &PaladinTriggerFactoryInternal::devotion_aura;
        creators["concentration aura"] = &PaladinTriggerFactoryInternal::concentration_aura;
        creators["shadow resistance aura"] = &PaladinTriggerFactoryInternal::shadow_resistance_aura;
        creators["frost resistance aura"] = &PaladinTriggerFactoryInternal::frost_resistance_aura;
        creators["fire resistance aura"] = &PaladinTriggerFactoryInternal::fire_resistance_aura;
        creators["hammer of justice snare"] = &PaladinTriggerFactoryInternal::hammer_of_justice_snare;
        creators["hammer of justice interrupt"] = &PaladinTriggerFactoryInternal::hammer_of_justice_interrupt;
        creators["cleanse cure disease"] = &PaladinTriggerFactoryInternal::CleanseCureDisease;
        creators["cleanse party member cure disease"] = &PaladinTriggerFactoryInternal::CleanseCurePartyMemberDisease;
        creators["cleanse cure poison"] = &PaladinTriggerFactoryInternal::CleanseCurePoison;
        creators["cleanse party member cure poison"] = &PaladinTriggerFactoryInternal::CleanseCurePartyMemberPoison;
        creators["cleanse cure magic"] = &PaladinTriggerFactoryInternal::CleanseCureMagic;
        creators["cleanse party member cure magic"] = &PaladinTriggerFactoryInternal::CleanseCurePartyMemberMagic;
        creators["righteous fury"] = &PaladinTriggerFactoryInternal::righteous_fury;
        creators["holy shield"] = &PaladinTriggerFactoryInternal::holy_shield;
        creators["tank aoe threat"] = &PaladinTriggerFactoryInternal::tank_aoe_threat;
        creators["tank aoe pack"] = &PaladinTriggerFactoryInternal::tank_aoe_pack;  // [b32_tank_aoe]
        creators["tank aoe crowd"] = &PaladinTriggerFactoryInternal::tank_aoe_crowd;  // [b32_peel]
        creators["untagged tank mob"] = &PaladinTriggerFactoryInternal::untagged_tank_mob;  // [rot172]
        creators["hammer of justice on enemy healer"] =
            &PaladinTriggerFactoryInternal::hammer_of_justice_on_enemy_target;
        creators["hammer of justice on snare target"] =
            &PaladinTriggerFactoryInternal::hammer_of_justice_on_snare_target;
        creators["not sensing undead"] = &PaladinTriggerFactoryInternal::not_sensing_undead;
        creators["divine favor"] = &PaladinTriggerFactoryInternal::divine_favor;
        creators["divine shield low health"] = &PaladinTriggerFactoryInternal::divine_shield_low_health;
        creators["turn undead"] = &PaladinTriggerFactoryInternal::turn_undead;
        creators["avenger's shield"] = &PaladinTriggerFactoryInternal::avenger_shield;
        creators["consecration"] = &PaladinTriggerFactoryInternal::consecration;
        creators["repentance on enemy healer"] = &PaladinTriggerFactoryInternal::repentance_on_enemy_healer;
        creators["repentance on snare target"] = &PaladinTriggerFactoryInternal::repentance_on_snare_target;
        creators["repentance interrupt"] = &PaladinTriggerFactoryInternal::repentance_interrupt;
        creators["beacon of light on main tank"] = &PaladinTriggerFactoryInternal::beacon_of_light_on_main_tank;
        creators["sacred shield on main tank"] = &PaladinTriggerFactoryInternal::sacred_shield_on_main_tank;
        creators["hand of freedom on party"] = &PaladinTriggerFactoryInternal::hand_of_freedom_on_party;
        creators["hand of salvation on party"] = &PaladinTriggerFactoryInternal::hand_of_salvation_on_party;
        creators["hand of sacrifice on main tank"] = &PaladinTriggerFactoryInternal::hand_of_sacrifice_on_main_tank;
        creators["aura mastery"] = &PaladinTriggerFactoryInternal::aura_mastery;

        creators["avenging wrath"] = &PaladinTriggerFactoryInternal::avenging_wrath;
        creators["ret divine plea"] = &PaladinTriggerFactoryInternal::ret_divine_plea;
        creators["holy wrath target"] = &PaladinTriggerFactoryInternal::holy_wrath_target;
        creators["holy wrath undead pack"] = &PaladinTriggerFactoryInternal::holy_wrath_undead_pack;  // [b34_pal]
        creators["seal state lost"] = &PaladinTriggerFactoryInternal::seal_state_lost;  // [rot_b31_pdd]
        creators["holy divine plea"] = &PaladinTriggerFactoryInternal::holy_divine_plea;  // [rot_b31_pdd]
        // [b31_pal]
        creators["judgements of the pure"] = &PaladinTriggerFactoryInternal::judgements_of_the_pure;
        creators["holy divine illumination"] = &PaladinTriggerFactoryInternal::holy_divine_illumination;
        creators["shield of righteousness ready"] = &PaladinTriggerFactoryInternal::shield_of_righteousness_ready;
        creators["hammer of the righteous ready"] = &PaladinTriggerFactoryInternal::hammer_of_the_righteous_ready;
        creators["judgement of wisdom ready"] = &PaladinTriggerFactoryInternal::judgement_of_wisdom_ready;
        creators["consecration ready"] = &PaladinTriggerFactoryInternal::consecration_ready;
        creators["avenger's shield boss filler"] = &PaladinTriggerFactoryInternal::avengers_shield_boss_filler;
        // [b31_deep]
        creators["paladin tank judges wisdom"] = &PaladinTriggerFactoryInternal::paladin_tank_judges_wisdom;
        creators["tank hammer of wrath"] = &PaladinTriggerFactoryInternal::tank_hammer_of_wrath;  // [lead87]
        creators["tank divine plea"] = &PaladinTriggerFactoryInternal::tank_divine_plea;
        creators["infusion of light"] = &PaladinTriggerFactoryInternal::infusion_of_light;
        creators["holy light boss tank"] = &PaladinTriggerFactoryInternal::holy_light_boss_tank;
        creators["holy avenging wrath"] = &PaladinTriggerFactoryInternal::holy_avenging_wrath;
        creators["blessing needed"] = &PaladinTriggerFactoryInternal::blessing_needed;
    }

private:
    static Trigger* not_sensing_undead(PlayerbotAI* botAI) { return new NotSensingUndeadTrigger(botAI); }
    static Trigger* turn_undead(PlayerbotAI* botAI) { return new TurnUndeadTrigger(botAI); }
    static Trigger* divine_favor(PlayerbotAI* botAI) { return new DivineFavorTrigger(botAI); }
    static Trigger* divine_shield_low_health(PlayerbotAI* botAI) { return new DivineShieldLowHealthTrigger(botAI); }
    static Trigger* holy_shield(PlayerbotAI* botAI) { return new HolyShieldTrigger(botAI); }
    // Consecration covers 8 yd around the paladin
    static Trigger* tank_aoe_threat(PlayerbotAI* botAI) { return new TankAoeThreatTrigger(botAI, 8.0f, 6.0f); }
    static Trigger* tank_aoe_pack(PlayerbotAI* botAI) { return new TankAoePackTrigger(botAI, 8.0f); }  // [b32_tank_aoe]
    static Trigger* tank_aoe_crowd(PlayerbotAI* botAI) { return new TankAoeCrowdTrigger(botAI, 8.0f); }  // [b32_peel]
    static Trigger* untagged_tank_mob(PlayerbotAI* botAI) { return new UntaggedTankMobTrigger(botAI); }  // [rot172]
    static Trigger* righteous_fury(PlayerbotAI* botAI) { return new RighteousFuryTrigger(botAI); }
    static Trigger* judgement(PlayerbotAI* botAI) { return new JudgementTrigger(botAI); }
    static Trigger* judgement_of_wisdom(PlayerbotAI* botAI) { return new JudgementOfWisdomTrigger(botAI); }
    static Trigger* judgement_of_light(PlayerbotAI* botAI) { return new JudgementOfLightTrigger(botAI); }
    static Trigger* seal(PlayerbotAI* botAI) { return new SealTrigger(botAI); }
    static Trigger* art_of_war(PlayerbotAI* botAI) { return new ArtOfWarTrigger(botAI); }
    static Trigger* crusader_aura(PlayerbotAI* botAI) { return new CrusaderAuraTrigger(botAI); }
    static Trigger* retribution_aura(PlayerbotAI* botAI) { return new RetributionAuraTrigger(botAI); }
    static Trigger* devotion_aura(PlayerbotAI* botAI) { return new DevotionAuraTrigger(botAI); }
    static Trigger* concentration_aura(PlayerbotAI* botAI) { return new ConcentrationAuraTrigger(botAI); }
    static Trigger* shadow_resistance_aura(PlayerbotAI* botAI) { return new ShadowResistanceAuraTrigger(botAI); }
    static Trigger* frost_resistance_aura(PlayerbotAI* botAI) { return new FrostResistanceAuraTrigger(botAI); }
    static Trigger* fire_resistance_aura(PlayerbotAI* botAI) { return new FireResistanceAuraTrigger(botAI); }
    static Trigger* hammer_of_justice_snare(PlayerbotAI* botAI) { return new HammerOfJusticeSnareTrigger(botAI); }
    static Trigger* hammer_of_justice_interrupt(PlayerbotAI* botAI)
    {
        return new HammerOfJusticeInterruptSpellTrigger(botAI);
    }
    static Trigger* CleanseCureDisease(PlayerbotAI* botAI) { return new CleanseCureDiseaseTrigger(botAI); }
    static Trigger* CleanseCurePartyMemberDisease(PlayerbotAI* botAI)
    {
        return new CleanseCurePartyMemberDiseaseTrigger(botAI);
    }
    static Trigger* CleanseCurePoison(PlayerbotAI* botAI) { return new CleanseCurePoisonTrigger(botAI); }
    static Trigger* CleanseCurePartyMemberPoison(PlayerbotAI* botAI)
    {
        return new CleanseCurePartyMemberPoisonTrigger(botAI);
    }
    static Trigger* CleanseCureMagic(PlayerbotAI* botAI) { return new CleanseCureMagicTrigger(botAI); }
    static Trigger* CleanseCurePartyMemberMagic(PlayerbotAI* botAI)
    {
        return new CleanseCurePartyMemberMagicTrigger(botAI);
    }
    static Trigger* hammer_of_justice_on_enemy_target(PlayerbotAI* botAI)
    {
        return new HammerOfJusticeEnemyHealerTrigger(botAI);
    }
    static Trigger* hammer_of_justice_on_snare_target(PlayerbotAI* botAI)
    {
        return new HammerOfJusticeSnareTrigger(botAI);
    }
    static Trigger* avenger_shield(PlayerbotAI* botAI) { return new AvengerShieldTrigger(botAI); }
    static Trigger* consecration(PlayerbotAI* botAI) { return new ConsecrationTrigger(botAI); }
    static Trigger* repentance_on_enemy_healer(PlayerbotAI* botAI) { return new RepentanceOnHealerTrigger(botAI); }
    static Trigger* repentance_on_snare_target(PlayerbotAI* botAI) { return new RepentanceSnareTrigger(botAI); }
    static Trigger* repentance_interrupt(PlayerbotAI* botAI) { return new RepentanceInterruptTrigger(botAI); }
    static Trigger* beacon_of_light_on_main_tank(PlayerbotAI* botAI) { return new BeaconOfLightOnMainTankTrigger(botAI); }
    static Trigger* sacred_shield_on_main_tank(PlayerbotAI* botAI) { return new SacredShieldOnMainTankTrigger(botAI); }
    static Trigger* hand_of_freedom_on_party(PlayerbotAI* botAI) { return new HandOfFreedomOnPartyTrigger(botAI); }
    static Trigger* hand_of_salvation_on_party(PlayerbotAI* botAI) { return new HandOfSalvationOnPartyTrigger(botAI); }
    static Trigger* hand_of_sacrifice_on_main_tank(PlayerbotAI* botAI)
    {
        return new HandOfSacrificeOnMainTankTrigger(botAI);
    }
    static Trigger* aura_mastery(PlayerbotAI* botAI) { return new AuraMasteryTrigger(botAI); }

    static Trigger* avenging_wrath(PlayerbotAI* botAI) { return new AvengingWrathTrigger(botAI); }
    static Trigger* blessing_needed(PlayerbotAI* botAI)
    {
        return new BlessingNeededTrigger(botAI);
    }
    static Trigger* ret_divine_plea(PlayerbotAI* botAI) { return new RetDivinePleaTrigger(botAI); }
    static Trigger* holy_wrath_target(PlayerbotAI* botAI) { return new HolyWrathTargetTrigger(botAI); }
    static Trigger* holy_wrath_undead_pack(PlayerbotAI* botAI)  // [b34_pal]
    {
        return new HolyWrathUndeadPackTrigger(botAI);
    }
    static Trigger* seal_state_lost(PlayerbotAI* botAI) { return new SealStateLostTrigger(botAI); }
    static Trigger* holy_divine_plea(PlayerbotAI* botAI) { return new HolyDivinePleaTrigger(botAI); }
    // [b31_pal]
    static Trigger* judgements_of_the_pure(PlayerbotAI* botAI) { return new JudgementsOfThePureTrigger(botAI); }
    static Trigger* holy_divine_illumination(PlayerbotAI* botAI) { return new HolyDivineIlluminationTrigger(botAI); }
    static Trigger* shield_of_righteousness_ready(PlayerbotAI* botAI)
    {
        return new PaladinSpellReadyTrigger(botAI, "shield of righteousness");
    }
    static Trigger* hammer_of_the_righteous_ready(PlayerbotAI* botAI)
    {
        return new PaladinSpellReadyTrigger(botAI, "hammer of the righteous");
    }
    static Trigger* judgement_of_wisdom_ready(PlayerbotAI* botAI)
    {
        return new PaladinSpellReadyTrigger(botAI, "judgement of wisdom");
    }
    // [rot12] no mana floor (wowsims casts it on cooldown; Divine Plea and Spiritual Attunement carry the mana)
    static Trigger* consecration_ready(PlayerbotAI* botAI)
    {
        return new PaladinSpellReadyTrigger(botAI, "consecration");
    }
    static Trigger* avengers_shield_boss_filler(PlayerbotAI* botAI)
    {
        return new AvengersShieldBossFillerTrigger(botAI);
    }
    // [b31_deep]
    static Trigger* paladin_tank_judges_wisdom(PlayerbotAI* botAI) { return new PaladinTankJudgesWisdomTrigger(botAI); }
    static Trigger* tank_hammer_of_wrath(PlayerbotAI* botAI) { return new TankHammerOfWrathTrigger(botAI); }
    static Trigger* tank_divine_plea(PlayerbotAI* botAI) { return new TankDivinePleaTrigger(botAI); }
    static Trigger* infusion_of_light(PlayerbotAI* botAI) { return new InfusionOfLightTrigger(botAI); }
    static Trigger* holy_light_boss_tank(PlayerbotAI* botAI) { return new HolyLightBossTankTrigger(botAI); }
    static Trigger* holy_avenging_wrath(PlayerbotAI* botAI) { return new HolyAvengingWrathTrigger(botAI); }
};

class PaladinAiObjectContextInternal : public NamedObjectContext<Action>
{
public:
    PaladinAiObjectContextInternal()
    {
        creators["seal of command"] = &PaladinAiObjectContextInternal::seal_of_command;
        creators["seal of vengeance"] = &PaladinAiObjectContextInternal::seal_of_vengeance;
        creators["seal of corruption"] = &PaladinAiObjectContextInternal::seal_of_corruption;
        creators["divine storm"] = &PaladinAiObjectContextInternal::divine_storm;
        creators["redemption"] = &PaladinAiObjectContextInternal::redemption;
        creators["crusader strike"] = &PaladinAiObjectContextInternal::crusader_strike;
        creators["crusader aura"] = &PaladinAiObjectContextInternal::crusader_aura;
        creators["seal of light"] = &PaladinAiObjectContextInternal::seal_of_light;
        creators["devotion aura"] = &PaladinAiObjectContextInternal::devotion_aura;
        creators["concentration aura"] = &PaladinAiObjectContextInternal::concentration_aura;
        creators["holy wrath"] = &PaladinAiObjectContextInternal::holy_wrath;
        creators["consecration"] = &PaladinAiObjectContextInternal::consecration;
        creators["ret consecration"] = &PaladinAiObjectContextInternal::ret_consecration;  // [b31_deep]
        creators["cleanse disease"] = &PaladinAiObjectContextInternal::cleanse_disease;
        creators["cleanse poison"] = &PaladinAiObjectContextInternal::cleanse_poison;
        creators["cleanse magic"] = &PaladinAiObjectContextInternal::cleanse_magic;
        creators["purify disease"] = &PaladinAiObjectContextInternal::purify_disease;
        creators["purify poison"] = &PaladinAiObjectContextInternal::purify_poison;
        creators["cleanse poison on party"] = &PaladinAiObjectContextInternal::cleanse_poison_on_party;
        creators["cleanse disease on party"] = &PaladinAiObjectContextInternal::cleanse_disease_on_party;
        creators["cleanse magic on party"] = &PaladinAiObjectContextInternal::cleanse_magic_on_party;
        creators["purify poison on party"] = &PaladinAiObjectContextInternal::purify_poison_on_party;
        creators["purify disease on party"] = &PaladinAiObjectContextInternal::purify_disease_on_party;
        creators["seal of wisdom"] = &PaladinAiObjectContextInternal::seal_of_wisdom;
        creators["seal of justice"] = &PaladinAiObjectContextInternal::seal_of_justice;
        creators["seal of righteousness"] = &PaladinAiObjectContextInternal::seal_of_righteousness;
        creators["flash of light"] = &PaladinAiObjectContextInternal::flash_of_light;
        creators["hand of reckoning"] = &PaladinAiObjectContextInternal::hand_of_reckoning;
        creators["avenger's shield"] = &PaladinAiObjectContextInternal::avengers_shield;
        creators["exorcism"] = &PaladinAiObjectContextInternal::exorcism;
        creators["judgement"] = &PaladinAiObjectContextInternal::judgement;
        creators["judgement of light"] = &PaladinAiObjectContextInternal::judgement_of_light;
        creators["judgement of wisdom"] = &PaladinAiObjectContextInternal::judgement_of_wisdom;
        creators["divine shield"] = &PaladinAiObjectContextInternal::divine_shield;
        creators["divine protection"] = &PaladinAiObjectContextInternal::divine_protection;
        creators["divine protection on party"] = &PaladinAiObjectContextInternal::divine_protection_on_party;
        creators["hammer of justice"] = &PaladinAiObjectContextInternal::hammer_of_justice;
        creators["flash of light on party"] = &PaladinAiObjectContextInternal::flash_of_light_on_party;
        creators["holy light"] = &PaladinAiObjectContextInternal::holy_light;
        creators["holy light on party"] = &PaladinAiObjectContextInternal::holy_light_on_party;
        creators["lay on hands"] = &PaladinAiObjectContextInternal::lay_on_hands;
        creators["lay on hands on party"] = &PaladinAiObjectContextInternal::lay_on_hands_on_party;
        creators["judgement of justice"] = &PaladinAiObjectContextInternal::judgement_of_justice;
        creators["hammer of wrath"] = &PaladinAiObjectContextInternal::hammer_of_wrath;
        creators["holy shield"] = &PaladinAiObjectContextInternal::holy_shield;
        // [b31_holyshield_pull]
        creators["holy shield pre pull"] = &PaladinAiObjectContextInternal::holy_shield_pre_pull;
        creators["hammer of the righteous"] = &PaladinAiObjectContextInternal::hammer_of_the_righteous;
        creators["retribution aura"] = &PaladinAiObjectContextInternal::retribution_aura;
        creators["shadow resistance aura"] = &PaladinAiObjectContextInternal::shadow_resistance_aura;
        creators["frost resistance aura"] = &PaladinAiObjectContextInternal::frost_resistance_aura;
        creators["fire resistance aura"] = &PaladinAiObjectContextInternal::fire_resistance_aura;
        creators["righteous fury"] = &PaladinAiObjectContextInternal::righteous_fury;
        creators["hammer of justice on enemy healer"] =
            &PaladinAiObjectContextInternal::hammer_of_justice_on_enemy_healer;
        creators["hammer of justice on snare target"] =
            &PaladinAiObjectContextInternal::hammer_of_justice_on_snare_target;
        creators["divine favor"] = &PaladinAiObjectContextInternal::divine_favor;
        creators["sense undead"] = &PaladinAiObjectContextInternal::sense_undead;
        creators["turn undead"] = &PaladinAiObjectContextInternal::turn_undead;
        creators["blessing of protection on party"] = &PaladinAiObjectContextInternal::blessing_of_protection_on_party;
        creators["righteous defense"] = &PaladinAiObjectContextInternal::righteous_defense;
        creators["righteous defense on attacked group member"] =
            &PaladinAiObjectContextInternal::righteous_defense_on_attacked_group_member;
        creators["righteous defense on swarmed group member"] =
            &PaladinAiObjectContextInternal::righteous_defense_on_swarmed_group_member;
        creators["hand of reckoning on group member attacker"] =
            &PaladinAiObjectContextInternal::hand_of_reckoning_on_group_member_attacker;
        creators["avenger's shield on group member attacker"] =
            &PaladinAiObjectContextInternal::avengers_shield_on_group_member_attacker;
        // [rot172]
        creators["avenger's shield on untagged mob"] =
            &PaladinAiObjectContextInternal::avengers_shield_on_untagged_mob;
        creators["hammer of justice on group member attacker"] =
            &PaladinAiObjectContextInternal::hammer_of_justice_on_group_member_attacker;
        creators["hammer of the righteous on group member attacker"] =
            &PaladinAiObjectContextInternal::hammer_of_the_righteous_on_group_member_attacker;
        creators["shield of righteousness on group member attacker"] =
            &PaladinAiObjectContextInternal::shield_of_righteousness_on_group_member_attacker;
        creators["judgement of wisdom on group member attacker"] =
            &PaladinAiObjectContextInternal::judgement_of_wisdom_on_group_member_attacker;
        creators["consecration on group member attackers"] =
            &PaladinAiObjectContextInternal::consecration_on_group_member_attackers;
        creators["consecration tank aoe"] = &PaladinAiObjectContextInternal::consecration_tank_aoe;
        creators["holy wrath on group member attackers"] =
            &PaladinAiObjectContextInternal::holy_wrath_on_group_member_attackers;
        creators["holy wrath undead pack"] = &PaladinAiObjectContextInternal::holy_wrath_undead_pack;  // [b34_pal]
        creators["repentance"] = &PaladinAiObjectContextInternal::repentance;
        creators["repentance on snare target"] = &PaladinAiObjectContextInternal::repentance_on_snare_target;
        creators["repentance on enemy healer"] = &PaladinAiObjectContextInternal::repentance_on_enemy_healer;
        creators["holy shock"] = &PaladinAiObjectContextInternal::holy_shock;
        creators["holy shock on party"] = &PaladinAiObjectContextInternal::holy_shock_on_party;
        creators["divine plea"] = &PaladinAiObjectContextInternal::divine_plea;
        creators["drop stale seal"] = &PaladinAiObjectContextInternal::drop_stale_seal;  // [rot_b31_pdd]
        creators["shield of righteousness"] = &PaladinAiObjectContextInternal::shield_of_righteousness;
        creators["beacon of light on main tank"] = &PaladinAiObjectContextInternal::beacon_of_light_on_main_tank;
        creators["sacred shield on main tank"] = &PaladinAiObjectContextInternal::sacred_shield_on_main_tank;
        creators["avenging wrath"] = &PaladinAiObjectContextInternal::avenging_wrath;
        creators["divine illumination"] = &PaladinAiObjectContextInternal::divine_illumination;
        creators["divine sacrifice"] = &PaladinAiObjectContextInternal::divine_sacrifice;
        creators["cancel divine sacrifice"] = &PaladinAiObjectContextInternal::cancel_divine_sacrifice;
        creators["hand of freedom on party"] = &PaladinAiObjectContextInternal::hand_of_freedom_on_party;
        creators["hand of salvation on party"] = &PaladinAiObjectContextInternal::hand_of_salvation_on_party;
        creators["hand of sacrifice on main tank"] = &PaladinAiObjectContextInternal::hand_of_sacrifice_on_main_tank;
        creators["hand of sacrifice on spiking tank"] = &PaladinAiObjectContextInternal::hand_of_sacrifice_spike;
        creators["aura mastery"] = &PaladinAiObjectContextInternal::aura_mastery;
        creators["cast blessing"] = &PaladinAiObjectContextInternal::cast_blessing;
    }

private:
    static Action* blessing_of_protection_on_party(PlayerbotAI* botAI)
    {
        return new CastBlessingOfProtectionProtectAction(botAI);
    }
    static Action* sense_undead(PlayerbotAI* botAI) { return new CastSenseUndeadAction(botAI); }
    static Action* turn_undead(PlayerbotAI* botAI) { return new CastTurnUndeadAction(botAI); }
    static Action* divine_favor(PlayerbotAI* botAI) { return new CastDivineFavorAction(botAI); }
    static Action* righteous_fury(PlayerbotAI* botAI) { return new CastRighteousFuryAction(botAI); }
    static Action* seal_of_command(PlayerbotAI* botAI) { return new CastSealOfCommandAction(botAI); }
    static Action* seal_of_vengeance(PlayerbotAI* botAI) { return new CastSealOfVengeanceAction(botAI); }
    static Action* seal_of_corruption(PlayerbotAI* botAI) { return new CastSealOfCorruptionAction(botAI); }
    static Action* divine_storm(PlayerbotAI* botAI) { return new CastDivineStormAction(botAI); }
    static Action* redemption(PlayerbotAI* botAI) { return new CastRedemptionAction(botAI); }
    static Action* crusader_strike(PlayerbotAI* botAI) { return new CastCrusaderStrikeAction(botAI); }
    static Action* crusader_aura(PlayerbotAI* botAI) { return new CastCrusaderAuraAction(botAI); }
    static Action* seal_of_light(PlayerbotAI* botAI) { return new CastSealOfLightAction(botAI); }
    static Action* devotion_aura(PlayerbotAI* botAI) { return new CastDevotionAuraAction(botAI); }
    static Action* concentration_aura(PlayerbotAI* botAI) { return new CastConcentrationAuraAction(botAI); }
    static Action* holy_wrath(PlayerbotAI* botAI) { return new CastHolyWrathAction(botAI); }
    static Action* consecration(PlayerbotAI* botAI) { return new CastConsecrationAction(botAI); }
    static Action* ret_consecration(PlayerbotAI* botAI) { return new CastRetConsecrationAction(botAI); }
    static Action* cleanse_poison(PlayerbotAI* botAI) { return new CastCleansePoisonAction(botAI); }
    static Action* cleanse_disease(PlayerbotAI* botAI) { return new CastCleanseDiseaseAction(botAI); }
    static Action* cleanse_magic(PlayerbotAI* botAI) { return new CastCleanseMagicAction(botAI); }
    static Action* purify_poison(PlayerbotAI* botAI) { return new CastPurifyPoisonAction(botAI); }
    static Action* purify_disease(PlayerbotAI* botAI) { return new CastPurifyDiseaseAction(botAI); }
    static Action* cleanse_poison_on_party(PlayerbotAI* botAI) { return new CastCleansePoisonOnPartyAction(botAI); }
    static Action* cleanse_disease_on_party(PlayerbotAI* botAI) { return new CastCleanseDiseaseOnPartyAction(botAI); }
    static Action* cleanse_magic_on_party(PlayerbotAI* botAI) { return new CastCleanseMagicOnPartyAction(botAI); }
    static Action* purify_poison_on_party(PlayerbotAI* botAI) { return new CastPurifyPoisonOnPartyAction(botAI); }
    static Action* purify_disease_on_party(PlayerbotAI* botAI) { return new CastPurifyDiseaseOnPartyAction(botAI); }
    static Action* seal_of_wisdom(PlayerbotAI* botAI) { return new CastSealOfWisdomAction(botAI); }
    static Action* seal_of_justice(PlayerbotAI* botAI) { return new CastSealOfJusticeAction(botAI); }
    static Action* seal_of_righteousness(PlayerbotAI* botAI) { return new CastSealOfRighteousnessAction(botAI); }
    static Action* flash_of_light(PlayerbotAI* botAI) { return new CastFlashOfLightAction(botAI); }
    static Action* hand_of_reckoning(PlayerbotAI* botAI) { return new CastHandOfReckoningAction(botAI); }
    static Action* avengers_shield(PlayerbotAI* botAI) { return new CastAvengersShieldAction(botAI); }
    static Action* exorcism(PlayerbotAI* botAI) { return new CastExorcismAction(botAI); }
    static Action* judgement(PlayerbotAI* botAI) { return new CastJudgementAction(botAI); }
    static Action* judgement_of_light(PlayerbotAI* botAI) { return new CastJudgementOfLightAction(botAI); }
    static Action* judgement_of_wisdom(PlayerbotAI* botAI) { return new CastJudgementOfWisdomAction(botAI); }
    static Action* divine_shield(PlayerbotAI* botAI) { return new CastDivineShieldAction(botAI); }
    static Action* divine_protection(PlayerbotAI* botAI) { return new CastDivineProtectionAction(botAI); }
    static Action* divine_protection_on_party(PlayerbotAI* botAI)
    {
        return new CastDivineProtectionOnPartyAction(botAI);
    }
    static Action* hammer_of_justice(PlayerbotAI* botAI) { return new CastHammerOfJusticeAction(botAI); }
    static Action* flash_of_light_on_party(PlayerbotAI* botAI) { return new CastFlashOfLightOnPartyAction(botAI); }
    static Action* holy_light(PlayerbotAI* botAI) { return new CastHolyLightAction(botAI); }
    static Action* holy_light_on_party(PlayerbotAI* botAI) { return new CastHolyLightOnPartyAction(botAI); }
    static Action* lay_on_hands(PlayerbotAI* botAI) { return new CastLayOnHandsAction(botAI); }
    static Action* lay_on_hands_on_party(PlayerbotAI* botAI) { return new CastLayOnHandsOnPartyAction(botAI); }
    static Action* judgement_of_justice(PlayerbotAI* botAI) { return new CastJudgementOfJusticeAction(botAI); }
    static Action* hammer_of_wrath(PlayerbotAI* botAI) { return new CastHammerOfWrathAction(botAI); }
    static Action* holy_shield(PlayerbotAI* botAI) { return new CastHolyShieldAction(botAI); }
    static Action* holy_shield_pre_pull(PlayerbotAI* botAI) { return new CastHolyShieldPrePullAction(botAI); }
    static Action* hammer_of_the_righteous(PlayerbotAI* botAI) { return new CastHammerOfTheRighteousAction(botAI); }
    static Action* retribution_aura(PlayerbotAI* botAI) { return new CastRetributionAuraAction(botAI); }
    static Action* shadow_resistance_aura(PlayerbotAI* botAI) { return new CastShadowResistanceAuraAction(botAI); }
    static Action* frost_resistance_aura(PlayerbotAI* botAI) { return new CastFrostResistanceAuraAction(botAI); }
    static Action* fire_resistance_aura(PlayerbotAI* botAI) { return new CastFireResistanceAuraAction(botAI); }
    static Action* hammer_of_justice_on_enemy_healer(PlayerbotAI* botAI)
    {
        return new CastHammerOfJusticeOnEnemyHealerAction(botAI);
    }
    static Action* hammer_of_justice_on_snare_target(PlayerbotAI* botAI)
    {
        return new CastHammerOfJusticeSnareAction(botAI);
    }
    static Action* righteous_defense(PlayerbotAI* botAI) { return new CastRighteousDefenseAction(botAI); }
    static Action* righteous_defense_on_attacked_group_member(PlayerbotAI* botAI)
    {
        return new CastSpellOnAttackedGroupMemberAction(botAI, "righteous defense");
    }
    static Action* righteous_defense_on_swarmed_group_member(PlayerbotAI* botAI)
    {
        return new CastSpellOnAttackedGroupMemberAction(botAI, "righteous defense", 2);
    }
    static Action* hand_of_reckoning_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "hand of reckoning");
    }
    static Action* avengers_shield_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "avenger's shield");
    }
    static Action* avengers_shield_on_untagged_mob(PlayerbotAI* botAI)  // [rot172]
    {
        return new CastSpellOnUntaggedTankMobAction(botAI, "avenger's shield");
    }
    static Action* hammer_of_justice_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "hammer of justice", true);
    }
    static Action* hammer_of_the_righteous_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "hammer of the righteous");
    }
    static Action* shield_of_righteousness_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "shield of righteousness");
    }
    static Action* judgement_of_wisdom_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "judgement of wisdom");
    }
    static Action* consecration_on_group_member_attackers(PlayerbotAI* botAI)
    {
        return new CastAreaSpellOnGroupMemberAttackersAction(botAI, "consecration", 8.0f);
    }
    static Action* consecration_tank_aoe(PlayerbotAI* botAI)
    {
        return new CastTankAreaThreatAction(botAI, "consecration");
    }
    static Action* holy_wrath_on_group_member_attackers(PlayerbotAI* botAI)
    {
        return new CastHolyWrathOnGroupMemberAttackersAction(botAI);  // [b31_deep] undead / demons only
    }
    static Action* holy_wrath_undead_pack(PlayerbotAI* botAI)  // [b34_pal]
    {
        return new CastHolyWrathUndeadPackAction(botAI);
    }
    static Action* repentance(PlayerbotAI* botAI) { return new CastRepentanceAction(botAI); }
    static Action* repentance_on_snare_target(PlayerbotAI* botAI) { return new CastRepentanceSnareAction(botAI); }
    static Action* repentance_on_enemy_healer(PlayerbotAI* botAI) { return new CastRepentanceOnHealerAction(botAI); }
    static Action* holy_shock(PlayerbotAI* botAI) { return new CastHolyShockAction(botAI); }
    static Action* holy_shock_on_party(PlayerbotAI* botAI) { return new CastHolyShockOnPartyAction(botAI); }
    static Action* divine_plea(PlayerbotAI* botAI) { return new CastDivinePleaAction(botAI); }
    static Action* hand_of_salvation_on_party(PlayerbotAI* botAI)
    {
        return new CastHandOfSalvationOnPartyAction(botAI);
    }
    static Action* hand_of_sacrifice_on_main_tank(PlayerbotAI* botAI)
    {
        return new CastHandOfSacrificeOnMainTankAction(botAI);
    }
    static Action* aura_mastery(PlayerbotAI* botAI) { return new CastAuraMasteryAction(botAI); }
    static Action* hand_of_sacrifice_spike(PlayerbotAI* botAI)
    {
        return new CastHandOfSacrificeOnSpikingTankAction(botAI);
    }
    static Action* shield_of_righteousness(PlayerbotAI* botAI) { return new ShieldOfRighteousnessAction(botAI); }
    static Action* drop_stale_seal(PlayerbotAI* botAI) { return new DropStaleSealAction(botAI); }
    static Action* beacon_of_light_on_main_tank(PlayerbotAI* botAI) { return new CastBeaconOfLightOnMainTankAction(botAI); }
    static Action* sacred_shield_on_main_tank(PlayerbotAI* botAI) { return new CastSacredShieldOnMainTankAction(botAI); }
    static Action* avenging_wrath(PlayerbotAI* botAI) { return new CastAvengingWrathAction(botAI); }
    static Action* divine_illumination(PlayerbotAI* botAI) { return new CastDivineIlluminationAction(botAI); }
    static Action* divine_sacrifice(PlayerbotAI* botAI) { return new CastDivineSacrificeAction(botAI); }
    static Action* cancel_divine_sacrifice(PlayerbotAI* botAI) { return new CastCancelDivineSacrificeAction(botAI); }
    static Action* hand_of_freedom_on_party(PlayerbotAI* botAI) { return new CastHandOfFreedomOnPartyAction(botAI); }
    static Action* cast_blessing(PlayerbotAI* botAI) { return new CastBlessingAction(botAI); }
};

class PaladinValueContextInternal : public NamedObjectContext<UntypedValue>
{
public:
    PaladinValueContextInternal()
    {
        creators["paladin tank in group"] = &PaladinValueContextInternal::paladin_tank_in_group;  // [b31_deep]
    }

private:
    static UntypedValue* paladin_tank_in_group(PlayerbotAI* botAI) { return new PaladinTankInGroupValue(botAI); }
};

SharedNamedObjectContextList<Strategy> PaladinAiObjectContext::sharedStrategyContexts;
SharedNamedObjectContextList<Action> PaladinAiObjectContext::sharedActionContexts;
SharedNamedObjectContextList<Trigger> PaladinAiObjectContext::sharedTriggerContexts;
SharedNamedObjectContextList<UntypedValue> PaladinAiObjectContext::sharedValueContexts;

PaladinAiObjectContext::PaladinAiObjectContext(PlayerbotAI* botAI)
    : AiObjectContext(botAI, sharedStrategyContexts, sharedActionContexts, sharedTriggerContexts, sharedValueContexts)
{
}

void PaladinAiObjectContext::BuildSharedContexts()
{
    BuildSharedStrategyContexts(sharedStrategyContexts);
    BuildSharedActionContexts(sharedActionContexts);
    BuildSharedTriggerContexts(sharedTriggerContexts);
    BuildSharedValueContexts(sharedValueContexts);
}

void PaladinAiObjectContext::BuildSharedStrategyContexts(SharedNamedObjectContextList<Strategy>& strategyContexts)
{
    AiObjectContext::BuildSharedStrategyContexts(strategyContexts);
    strategyContexts.Add(new PaladinStrategyFactoryInternal());
    strategyContexts.Add(new PaladinCombatStrategyFactoryInternal());
    strategyContexts.Add(new PaladinBuffStrategyFactoryInternal());
    strategyContexts.Add(new PaladinResistanceStrategyFactoryInternal());
}

void PaladinAiObjectContext::BuildSharedActionContexts(SharedNamedObjectContextList<Action>& actionContexts)
{
    AiObjectContext::BuildSharedActionContexts(actionContexts);
    actionContexts.Add(new PaladinAiObjectContextInternal());
}

void PaladinAiObjectContext::BuildSharedTriggerContexts(SharedNamedObjectContextList<Trigger>& triggerContexts)
{
    AiObjectContext::BuildSharedTriggerContexts(triggerContexts);
    triggerContexts.Add(new PaladinTriggerFactoryInternal());
}

void PaladinAiObjectContext::BuildSharedValueContexts(SharedNamedObjectContextList<UntypedValue>& valueContexts)
{
    AiObjectContext::BuildSharedValueContexts(valueContexts);
    valueContexts.Add(new PaladinValueContextInternal());
}
