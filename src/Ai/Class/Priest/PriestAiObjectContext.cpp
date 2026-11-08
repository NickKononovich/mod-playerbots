/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PriestAiObjectContext.h"
#include "DiscPriestStrategy.h"
#include "GenericPriestStrategy.h"
#include "HolyPriestStrategy.h"
#include "NamedObjectContext.h"
#include "Playerbots.h"
#include "PriestActions.h"
#include "PriestNonCombatStrategy.h"
#include "PriestTriggers.h"
#include "PullStrategy.h"
#include "ShadowPriestStrategy.h"
#include "DamageSpikeActions.h"

class PriestStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    PriestStrategyFactoryInternal()
    {
        creators["nc"] = &PriestStrategyFactoryInternal::nc;
        creators["pull"] = &PriestStrategyFactoryInternal::pull;
        creators["aoe"] = &PriestStrategyFactoryInternal::shadow_aoe;
        creators["shadow aoe"] = &PriestStrategyFactoryInternal::shadow_aoe;
        creators["dps debuff"] = &PriestStrategyFactoryInternal::shadow_debuff;
        creators["shadow debuff"] = &PriestStrategyFactoryInternal::shadow_debuff;
        creators["cure"] = &PriestStrategyFactoryInternal::cure;
        creators["buff"] = &PriestStrategyFactoryInternal::buff;
        creators["boost"] = &PriestStrategyFactoryInternal::boost;
        creators["rshadow"] = &PriestStrategyFactoryInternal::rshadow;
        creators["cc"] = &PriestStrategyFactoryInternal::cc;
        creators["healer dps"] = &PriestStrategyFactoryInternal::healer_dps;
    }

private:
    static Strategy* cc(PlayerbotAI* botAI) { return new PriestCcStrategy(botAI); }
    static Strategy* rshadow(PlayerbotAI* botAI) { return new PriestShadowResistanceStrategy(botAI); }
    static Strategy* boost(PlayerbotAI* botAI) { return new PriestBoostStrategy(botAI); }
    static Strategy* buff(PlayerbotAI* botAI) { return new PriestBuffStrategy(botAI); }
    static Strategy* nc(PlayerbotAI* botAI) { return new PriestNonCombatStrategy(botAI); }
    static Strategy* shadow_aoe(PlayerbotAI* botAI) { return new ShadowPriestAoeStrategy(botAI); }
    static Strategy* pull(PlayerbotAI* botAI) { return new PullStrategy(botAI, "shoot"); }
    static Strategy* shadow_debuff(PlayerbotAI* botAI) { return new ShadowPriestDebuffStrategy(botAI); }
    static Strategy* cure(PlayerbotAI* botAI) { return new PriestCureStrategy(botAI); }
    static Strategy* healer_dps(PlayerbotAI* botAI) { return new PriestHealerDpsStrategy(botAI); }
};

class PriestCombatStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    PriestCombatStrategyFactoryInternal() : NamedObjectContext<Strategy>(false, true)
    {
        creators["disc"] = &PriestCombatStrategyFactoryInternal::disc;
        creators["heal"] = &PriestCombatStrategyFactoryInternal::disc;
        creators["shadow"] = &PriestCombatStrategyFactoryInternal::shadow;
        creators["dps"] = &PriestCombatStrategyFactoryInternal::shadow;
        creators["holy dps"] = &PriestCombatStrategyFactoryInternal::holy_dps;
        creators["holy"] = &PriestCombatStrategyFactoryInternal::holy_heal;
        creators["holy heal"] = &PriestCombatStrategyFactoryInternal::holy_heal;
    }

private:
    static Strategy* disc(PlayerbotAI* botAI) { return new DiscPriestStrategy(botAI); }
    static Strategy* shadow(PlayerbotAI* botAI) { return new ShadowPriestStrategy(botAI); }
    static Strategy* holy_dps(PlayerbotAI* botAI) { return new HolyDpsPriestStrategy(botAI); }
    static Strategy* holy_heal(PlayerbotAI* botAI) { return new HolyHealPriestStrategy(botAI); }
};

class PriestTriggerFactoryInternal : public NamedObjectContext<Trigger>
{
public:
    PriestTriggerFactoryInternal()
    {
        creators["devouring plague"] = &PriestTriggerFactoryInternal::devouring_plague;
        creators["shadow word: pain"] = &PriestTriggerFactoryInternal::shadow_word_pain;
        creators["shadow word: pain on attacker"] = &PriestTriggerFactoryInternal::shadow_word_pain_on_attacker;
        creators["dispel magic"] = &PriestTriggerFactoryInternal::dispel_magic;
        creators["dispel magic on party"] = &PriestTriggerFactoryInternal::dispel_magic_party_member;
        creators["cure disease"] = &PriestTriggerFactoryInternal::cure_disease;
        creators["party member cure disease"] = &PriestTriggerFactoryInternal::party_member_cure_disease;
        creators["power word: fortitude"] = &PriestTriggerFactoryInternal::power_word_fortitude;
        creators["power word: fortitude on party"] = &PriestTriggerFactoryInternal::power_word_fortitude_on_party;
        creators["divine spirit"] = &PriestTriggerFactoryInternal::divine_spirit;
        creators["divine spirit on party"] = &PriestTriggerFactoryInternal::divine_spirit_on_party;
        creators["inner fire"] = &PriestTriggerFactoryInternal::inner_fire;
        creators["vampiric touch"] = &PriestTriggerFactoryInternal::vampiric_touch;
        creators["vampiric touch on attacker"] = &PriestTriggerFactoryInternal::vampiric_touch_on_attacker;
        creators["shadowform"] = &PriestTriggerFactoryInternal::shadowform;
        creators["vampiric embrace"] = &PriestTriggerFactoryInternal::vampiric_embrace;
        creators["power infusion"] = &PriestTriggerFactoryInternal::power_infusion;
        creators["hymn of hope needed"] = &PriestTriggerFactoryInternal::hymn_of_hope_needed;
        creators["inner focus"] = &PriestTriggerFactoryInternal::inner_focus;
        creators["inner focus shadow weaving"] = &PriestTriggerFactoryInternal::inner_focus_shadow_weaving;  // [rot18]
        creators["shadow protection"] = &PriestTriggerFactoryInternal::shadow_protection;
        creators["shadow protection on party"] = &PriestTriggerFactoryInternal::shadow_protection_on_party;
        creators["shackle undead"] = &PriestTriggerFactoryInternal::shackle_undead;
        creators["holy fire"] = &PriestTriggerFactoryInternal::holy_fire;
        creators["touch of weakness"] = &PriestTriggerFactoryInternal::touch_of_weakness;
        creators["hex of weakness"] = &PriestTriggerFactoryInternal::hex_of_weakness;
        creators["shadowguard"] = &PriestTriggerFactoryInternal::shadowguard;
        creators["fear ward on main tank"] = &PriestTriggerFactoryInternal::fear_ward_on_main_tank;
        creators["power word: shield on main tank"] = &PriestTriggerFactoryInternal::power_word_shield_on_main_tank;
        creators["prayer of mending on main tank"] = &PriestTriggerFactoryInternal::prayer_of_mending_on_main_tank;
        creators["renew on main tank"] = &PriestTriggerFactoryInternal::renew_on_main_tank;
        creators["feedback"] = &PriestTriggerFactoryInternal::feedback;
        creators["binding heal"] = &PriestTriggerFactoryInternal::binding_heal;
        creators["chastise"] = &PriestTriggerFactoryInternal::chastise;
        creators["silence"] = &PriestTriggerFactoryInternal::silence;
        creators["silence on enemy healer"] = &PriestTriggerFactoryInternal::silence_on_enemy_healer;
        creators["shadowfiend"] = &PriestTriggerFactoryInternal::shadowfiend;
        creators["mind sear channel check"] = &PriestTriggerFactoryInternal::mind_sear_channel_check;
        creators["mind sear heavy aoe"] = &PriestTriggerFactoryInternal::mind_sear_heavy_aoe;  // [b31_deep]
        creators["mind sear pays"] = &PriestTriggerFactoryInternal::mind_sear_pays;  // [rot401]
        creators["mind blast pays"] = &PriestTriggerFactoryInternal::mind_blast_pays;  // [rot603]
        creators["mind flay clip"] = &PriestTriggerFactoryInternal::mind_flay_clip;  // [guide_b31_dead] (+ next line)
        creators["shadowform out of combat"] = &PriestTriggerFactoryInternal::shadowform_out_of_combat;
        creators["battle resurrected member"] = &PriestTriggerFactoryInternal::battle_resurrected_member;
        creators["battle resurrected member low health"] =
            &PriestTriggerFactoryInternal::battle_resurrected_member_low_health;
        creators["tank peel"] = &PriestTriggerFactoryInternal::tank_peel;
        creators["surge of light"] = &PriestTriggerFactoryInternal::surge_of_light;
        // [guide_b31_dead] healer guide gaps (GUIDE_AUDIT_HEALERS 1-2)
        creators["penance on main tank"] = &PriestTriggerFactoryInternal::penance_on_main_tank;
        creators["circle of healing two injured"] = &PriestTriggerFactoryInternal::circle_of_healing_two_injured;
        creators["borrowed time"] = &PriestTriggerFactoryInternal::borrowed_time;
        creators["borrowed time low health"] = &PriestTriggerFactoryInternal::borrowed_time_low_health;
        creators["serendipity greater heal"] = &PriestTriggerFactoryInternal::serendipity_greater_heal;
    }

private:
    static Trigger* vampiric_embrace(PlayerbotAI* botAI) { return new VampiricEmbraceTrigger(botAI); }
    static Trigger* shadowform(PlayerbotAI* botAI) { return new ShadowformTrigger(botAI); }
    static Trigger* vampiric_touch(PlayerbotAI* botAI) { return new VampiricTouchTrigger(botAI); }
    static Trigger* vampiric_touch_on_attacker(PlayerbotAI* botAI) { return new VampiricTouchOnAttackerTrigger(botAI); }
    static Trigger* devouring_plague(PlayerbotAI* botAI) { return new DevouringPlagueTrigger(botAI); }
    static Trigger* shadow_word_pain(PlayerbotAI* botAI) { return new ShadowWordPainTrigger(botAI); }
    static Trigger* shadow_word_pain_on_attacker(PlayerbotAI* botAI)
    {
        return new ShadowWordPainOnAttackerTrigger(botAI);
    }
    static Trigger* dispel_magic(PlayerbotAI* botAI) { return new DispelMagicTrigger(botAI); }
    static Trigger* dispel_magic_party_member(PlayerbotAI* botAI) { return new DispelMagicPartyMemberTrigger(botAI); }
    static Trigger* cure_disease(PlayerbotAI* botAI) { return new CureDiseaseTrigger(botAI); }
    static Trigger* party_member_cure_disease(PlayerbotAI* botAI) { return new PartyMemberCureDiseaseTrigger(botAI); }
    static Trigger* power_word_fortitude(PlayerbotAI* botAI) { return new PowerWordFortitudeTrigger(botAI); }
    static Trigger* power_word_fortitude_on_party(PlayerbotAI* botAI)
    {
        return new PowerWordFortitudeOnPartyTrigger(botAI);
    }
    static Trigger* divine_spirit(PlayerbotAI* botAI) { return new DivineSpiritTrigger(botAI); }
    static Trigger* divine_spirit_on_party(PlayerbotAI* botAI) { return new DivineSpiritOnPartyTrigger(botAI); }
    static Trigger* inner_fire(PlayerbotAI* botAI) { return new InnerFireTrigger(botAI); }
    static Trigger* power_infusion(PlayerbotAI* botAI) { return new PowerInfusionTrigger(botAI); }
    static Trigger* hymn_of_hope_needed(PlayerbotAI* botAI) { return new HymnOfHopeNeededTrigger(botAI); }
    static Trigger* inner_focus(PlayerbotAI* botAI) { return new InnerFocusTrigger(botAI); }
    static Trigger* inner_focus_shadow_weaving(PlayerbotAI* botAI) { return new InnerFocusShadowWeavingTrigger(botAI); }
    static Trigger* shadow_protection_on_party(PlayerbotAI* botAI) { return new ShadowProtectionOnPartyTrigger(botAI); }
    static Trigger* shadow_protection(PlayerbotAI* botAI) { return new ShadowProtectionTrigger(botAI); }
    static Trigger* shackle_undead(PlayerbotAI* botAI) { return new ShackleUndeadTrigger(botAI); }
    static Trigger* feedback(PlayerbotAI* botAI) { return new FeedbackTrigger(botAI); }
    static Trigger* fear_ward_on_main_tank(PlayerbotAI* botAI) { return new FearWardOnMainTankTrigger(botAI); }
    static Trigger* power_word_shield_on_main_tank(PlayerbotAI* botAI)
    {
        return new PowerWordShieldOnMainTankTrigger(botAI);
    }
    static Trigger* prayer_of_mending_on_main_tank(PlayerbotAI* botAI)
    {
        return new PrayerOfMendingOnMainTankTrigger(botAI);
    }
    static Trigger* renew_on_main_tank(PlayerbotAI* botAI) { return new RenewOnMainTankTrigger(botAI); }
    static Trigger* shadowguard(PlayerbotAI* botAI) { return new ShadowguardTrigger(botAI); }
    static Trigger* hex_of_weakness(PlayerbotAI* botAI) { return new HexOfWeaknessTrigger(botAI); }
    static Trigger* touch_of_weakness(PlayerbotAI* botAI) { return new TouchOfWeaknessTrigger(botAI); }
    static Trigger* holy_fire(PlayerbotAI* botAI) { return new HolyFireTrigger(botAI); }
    static Trigger* shadowfiend(PlayerbotAI* botAI) { return new ShadowfiendTrigger(botAI); }
    static Trigger* silence_on_enemy_healer(PlayerbotAI* botAI) { return new SilenceEnemyHealerTrigger(botAI); }
    static Trigger* silence(PlayerbotAI* botAI) { return new SilenceTrigger(botAI); }
    static Trigger* chastise(PlayerbotAI* botAI) { return new ChastiseTrigger(botAI); }
    static Trigger* binding_heal(PlayerbotAI* botAI) { return new BindingHealTrigger(botAI); }
    static Trigger* mind_sear_channel_check(PlayerbotAI* botAI) { return new MindSearChannelCheckTrigger(botAI); }
    // [b31_deep]
    static Trigger* mind_sear_heavy_aoe(PlayerbotAI* botAI) { return new MindSearHeavyAoeTrigger(botAI); }
    static Trigger* mind_sear_pays(PlayerbotAI* botAI) { return new MindSearPaysTrigger(botAI); }
    static Trigger* mind_blast_pays(PlayerbotAI* botAI) { return new MindBlastPaysTrigger(botAI); }
    static Trigger* mind_flay_clip(PlayerbotAI* botAI) { return new MindFlayClipTrigger(botAI); }  // [guide_b31_dead]
    static Trigger* shadowform_out_of_combat(PlayerbotAI* botAI)  // [guide_b31_dead]
    {
        return new ShadowformNonCombatTrigger(botAI);
    }
    static Trigger* battle_resurrected_member(PlayerbotAI* botAI) { return new BattleResurrectedMemberTrigger(botAI); }
    static Trigger* battle_resurrected_member_low_health(PlayerbotAI* botAI)
    {
        return new BattleResurrectedMemberLowHealthTrigger(botAI);
    }
    static Trigger* tank_peel(PlayerbotAI* botAI) { return new TankPeelTrigger(botAI); }
    static Trigger* surge_of_light(PlayerbotAI* botAI) { return new SurgeOfLightTrigger(botAI); }
    // [guide_b31_dead]
    static Trigger* penance_on_main_tank(PlayerbotAI* botAI) { return new PenanceOnMainTankTrigger(botAI); }
    static Trigger* circle_of_healing_two_injured(PlayerbotAI* botAI)
    {
        return new CircleOfHealingTwoInjuredTrigger(botAI);
    }
    static Trigger* borrowed_time(PlayerbotAI* botAI) { return new BorrowedTimeTrigger(botAI, "borrowed time", 90.0f); }
    static Trigger* borrowed_time_low_health(PlayerbotAI* botAI)
    {
        return new BorrowedTimeTrigger(botAI, "borrowed time low health", sPlayerbotAIConfig.LowHealth);
    }
    static Trigger* serendipity_greater_heal(PlayerbotAI* botAI) { return new SerendipityGreaterHealTrigger(botAI); }
};

class PriestAiObjectContextInternal : public NamedObjectContext<Action>
{
public:
    PriestAiObjectContextInternal()
    {
        creators["power infusion"] = &PriestAiObjectContextInternal::power_infusion;
        creators["inner focus"] = &PriestAiObjectContextInternal::inner_focus;
        creators["shadow word: pain"] = &PriestAiObjectContextInternal::shadow_word_pain;
        creators["shadow word: pain on attacker"] = &PriestAiObjectContextInternal::shadow_word_pain_on_attacker;
        creators["devouring plague"] = &PriestAiObjectContextInternal::devouring_plague;
        creators["mind flay"] = &PriestAiObjectContextInternal::mind_flay;
        creators["holy fire"] = &PriestAiObjectContextInternal::holy_fire;
        creators["smite"] = &PriestAiObjectContextInternal::smite;
        creators["mind blast"] = &PriestAiObjectContextInternal::mind_blast;
        creators["shadowform"] = &PriestAiObjectContextInternal::shadowform;
        creators["remove shadowform"] = &PriestAiObjectContextInternal::remove_shadowform;
        creators["holy nova"] = &PriestAiObjectContextInternal::holy_nova;
        creators["power word: fortitude"] = &PriestAiObjectContextInternal::power_word_fortitude;
        creators["power word: fortitude on party"] = &PriestAiObjectContextInternal::power_word_fortitude_on_party;
        creators["divine spirit"] = &PriestAiObjectContextInternal::divine_spirit;
        creators["divine spirit on party"] = &PriestAiObjectContextInternal::divine_spirit_on_party;
        creators["power word: shield"] = &PriestAiObjectContextInternal::power_word_shield;
        creators["power word: shield on party"] = &PriestAiObjectContextInternal::power_word_shield_on_party;
        creators["power word: shield on almost full health below"] =
            &PriestAiObjectContextInternal::power_word_shield_on_almost_full_health_below;
        creators["power word: shield on not full"] = &PriestAiObjectContextInternal::power_word_shield_on_not_full;
        creators["renew"] = &PriestAiObjectContextInternal::renew;
        creators["renew on party"] = &PriestAiObjectContextInternal::renew_on_party;
        creators["greater heal"] = &PriestAiObjectContextInternal::greater_heal;
        creators["greater heal on party"] = &PriestAiObjectContextInternal::greater_heal_on_party;
        creators["heal"] = &PriestAiObjectContextInternal::heal;
        creators["heal on party"] = &PriestAiObjectContextInternal::heal_on_party;
        creators["lesser heal"] = &PriestAiObjectContextInternal::lesser_heal;
        creators["lesser heal on party"] = &PriestAiObjectContextInternal::lesser_heal_on_party;
        creators["flash heal"] = &PriestAiObjectContextInternal::flash_heal;
        creators["flash heal on party"] = &PriestAiObjectContextInternal::flash_heal_on_party;
        creators["dispel magic"] = &PriestAiObjectContextInternal::dispel_magic;
        creators["dispel magic on party"] = &PriestAiObjectContextInternal::dispel_magic_on_party;
        creators["dispel magic on target"] = &PriestAiObjectContextInternal::dispel_magic_on_target;
        creators["cure disease"] = &PriestAiObjectContextInternal::cure_disease;
        creators["cure disease on party"] = &PriestAiObjectContextInternal::cure_disease_on_party;
        creators["abolish disease"] = &PriestAiObjectContextInternal::abolish_disease;
        creators["abolish disease on party"] = &PriestAiObjectContextInternal::abolish_disease_on_party;
        creators["fade"] = &PriestAiObjectContextInternal::fade;
        creators["inner fire"] = &PriestAiObjectContextInternal::inner_fire;
        creators["resurrection"] = &PriestAiObjectContextInternal::resurrection;
        creators["circle of healing on party"] = &PriestAiObjectContextInternal::circle_of_healing;
        creators["psychic scream"] = &PriestAiObjectContextInternal::psychic_scream;
        creators["vampiric touch"] = &PriestAiObjectContextInternal::vampiric_touch;
        creators["vampiric touch on attacker"] = &PriestAiObjectContextInternal::vampiric_touch_on_attacker;
        creators["vampiric embrace"] = &PriestAiObjectContextInternal::vampiric_embrace;
        creators["dispersion"] = &PriestAiObjectContextInternal::dispersion;
        creators["shadow protection"] = &PriestAiObjectContextInternal::shadow_protection;
        creators["shadow protection on party"] = &PriestAiObjectContextInternal::shadow_protection_on_party;
        creators["shackle undead"] = &PriestAiObjectContextInternal::shackle_undead;
        creators["power infusion on party"] = &PriestAiObjectContextInternal::power_infusion_on_party;
        creators["silence"] = &PriestAiObjectContextInternal::silence;
        creators["silence on enemy healer"] = &PriestAiObjectContextInternal::silence_on_enemy_healer;
        creators["mana burn"] = &PriestAiObjectContextInternal::mana_burn;
        creators["levitate"] = &PriestAiObjectContextInternal::levitate;
        creators["prayer of healing on party"] = &PriestAiObjectContextInternal::prayer_of_healing;
        creators["lightwell"] = &PriestAiObjectContextInternal::lightwell;
        creators["mind soothe"] = &PriestAiObjectContextInternal::mind_soothe;
        creators["touch of weakness"] = &PriestAiObjectContextInternal::touch_of_weakness;
        creators["hex of weakness"] = &PriestAiObjectContextInternal::hex_of_weakness;
        creators["shadowguard"] = &PriestAiObjectContextInternal::shadowguard;
        creators["desperate prayer"] = &PriestAiObjectContextInternal::desperate_prayer;
        creators["fear ward on main tank"] = &PriestAiObjectContextInternal::fear_ward_on_main_tank;
        creators["power word: shield on main tank"] = &PriestAiObjectContextInternal::power_word_shield_on_main_tank;
        creators["prayer of mending on main tank"] = &PriestAiObjectContextInternal::prayer_of_mending_on_main_tank;
        creators["renew on main tank"] = &PriestAiObjectContextInternal::renew_on_main_tank;
        creators["starshards"] = &PriestAiObjectContextInternal::starshards;
        creators["elune's grace"] = &PriestAiObjectContextInternal::elunes_grace;
        creators["feedback"] = &PriestAiObjectContextInternal::feedback;
        creators["symbol of hope"] = &PriestAiObjectContextInternal::symbol_of_hope;
        creators["consume magic"] = &PriestAiObjectContextInternal::consume_magic;
        creators["chastise"] = &PriestAiObjectContextInternal::chastise;
        creators["shadow word: death"] = &PriestAiObjectContextInternal::shadow_word_death;
        creators["shadowfiend"] = &PriestAiObjectContextInternal::shadowfiend;
        creators["mass dispel"] = &PriestAiObjectContextInternal::mass_dispel;
        creators["pain suppression"] = &PriestAiObjectContextInternal::pain_suppression;
        creators["pain suppression on party"] = &PriestAiObjectContextInternal::pain_suppression_on_party;
        creators["pain suppression on spiking tank"] = &PriestAiObjectContextInternal::pain_suppression_spike;
        creators["prayer of mending on party"] = &PriestAiObjectContextInternal::prayer_of_mending;
        creators["binding heal"] = &PriestAiObjectContextInternal::binding_heal;
        creators["penance on party"] = &PriestAiObjectContextInternal::penance_on_party;
        // [guide_b31_dead] main-tank upkeep heals for the healer guide gaps
        creators["penance on main tank"] = &PriestAiObjectContextInternal::penance_on_main_tank;
        creators["greater heal on main tank"] = &PriestAiObjectContextInternal::greater_heal_on_main_tank;
        creators["hymn of hope"] = &PriestAiObjectContextInternal::hymn_of_hope;
        creators["divine hymn"] = &PriestAiObjectContextInternal::divine_hymn;
        creators["mind sear"] = &PriestAiObjectContextInternal::mind_sear;
        creators["guardian spirit on party"] = &PriestAiObjectContextInternal::guardian_spirit_on_party;
        creators["guardian spirit on spiking tank"] = &PriestAiObjectContextInternal::guardian_spirit_spike;
        creators["power word: shield on battle resurrected"] = &PriestAiObjectContextInternal::shield_on_rezzed;
        creators["penance on battle resurrected"] = &PriestAiObjectContextInternal::penance_on_rezzed;
        creators["flash heal on battle resurrected"] = &PriestAiObjectContextInternal::flash_heal_on_rezzed;
        creators["renew on battle resurrected"] = &PriestAiObjectContextInternal::renew_on_rezzed;
        creators["power word: fortitude on battle resurrected"] = &PriestAiObjectContextInternal::fortitude_on_rezzed;
        creators["divine spirit on battle resurrected"] = &PriestAiObjectContextInternal::divine_spirit_on_rezzed;
        creators["shadow protection on battle resurrected"] =
            &PriestAiObjectContextInternal::shadow_protection_on_rezzed;
        creators["power word: shield on peeling tank"] = &PriestAiObjectContextInternal::shield_on_peeling_tank;
        creators["renew on peeling tank"] = &PriestAiObjectContextInternal::renew_on_peeling_tank;
        creators["flash heal on peeling tank"] = &PriestAiObjectContextInternal::flash_heal_on_peeling_tank;
    }

private:
    static Action* shadow_protection_on_party(PlayerbotAI* botAI)
    {
        return new CastShadowProtectionOnPartyAction(botAI);
    }
    static Action* shadow_protection(PlayerbotAI* botAI) { return new CastShadowProtectionAction(botAI); }
    static Action* power_infusion(PlayerbotAI* botAI) { return new CastPowerInfusionAction(botAI); }
    static Action* inner_focus(PlayerbotAI* botAI) { return new CastInnerFocusAction(botAI); }
    static Action* dispersion(PlayerbotAI* botAI) { return new CastDispersionAction(botAI); }
    static Action* vampiric_embrace(PlayerbotAI* botAI) { return new CastVampiricEmbraceAction(botAI); }
    static Action* vampiric_touch(PlayerbotAI* botAI) { return new CastVampiricTouchAction(botAI); }
    static Action* vampiric_touch_on_attacker(PlayerbotAI* botAI)
    {
        return new CastVampiricTouchOnAttackerAction(botAI);
    }
    static Action* psychic_scream(PlayerbotAI* botAI) { return new CastPsychicScreamAction(botAI); }
    static Action* circle_of_healing(PlayerbotAI* botAI) { return new CastCircleOfHealingAction(botAI); }
    static Action* resurrection(PlayerbotAI* botAI) { return new CastResurrectionAction(botAI); }
    static Action* shadow_word_pain(PlayerbotAI* botAI) { return new CastShadowWordPainAction(botAI); }
    static Action* shadow_word_pain_on_attacker(PlayerbotAI* botAI)
    {
        return new CastShadowWordPainOnAttackerAction(botAI);
    }
    static Action* devouring_plague(PlayerbotAI* botAI) { return new CastDevouringPlagueAction(botAI); }
    static Action* mind_flay(PlayerbotAI* botAI) { return new CastMindFlayAction(botAI); }
    static Action* holy_fire(PlayerbotAI* botAI) { return new CastHolyFireAction(botAI); }
    static Action* smite(PlayerbotAI* botAI) { return new CastSmiteAction(botAI); }
    static Action* mind_blast(PlayerbotAI* botAI) { return new CastMindBlastAction(botAI); }
    static Action* shadowform(PlayerbotAI* botAI) { return new CastShadowformAction(botAI); }
    static Action* remove_shadowform(PlayerbotAI* botAI) { return new CastRemoveShadowformAction(botAI); }
    static Action* holy_nova(PlayerbotAI* botAI) { return new CastHolyNovaAction(botAI); }
    static Action* power_word_fortitude(PlayerbotAI* botAI) { return new CastPowerWordFortitudeAction(botAI); }
    static Action* power_word_fortitude_on_party(PlayerbotAI* botAI)
    {
        return new CastPowerWordFortitudeOnPartyAction(botAI);
    }
    static Action* divine_spirit(PlayerbotAI* botAI) { return new CastDivineSpiritAction(botAI); }
    static Action* divine_spirit_on_party(PlayerbotAI* botAI) { return new CastDivineSpiritOnPartyAction(botAI); }
    static Action* power_word_shield(PlayerbotAI* botAI) { return new CastPowerWordShieldAction(botAI); }
    static Action* power_word_shield_on_party(PlayerbotAI* botAI)
    {
        return new CastPowerWordShieldOnPartyAction(botAI);
    }
    static Action* power_word_shield_on_almost_full_health_below(PlayerbotAI* botAI)
    {
        return new CastPowerWordShieldOnAlmostFullHealthBelowAction(botAI);
    }
    static Action* power_word_shield_on_not_full(PlayerbotAI* botAI) { return new CastPowerWordShieldOnNotFullAction(botAI); }
    static Action* renew(PlayerbotAI* botAI) { return new CastRenewAction(botAI); }
    static Action* renew_on_party(PlayerbotAI* botAI) { return new CastRenewOnPartyAction(botAI); }
    static Action* greater_heal(PlayerbotAI* botAI) { return new CastGreaterHealAction(botAI); }
    static Action* greater_heal_on_party(PlayerbotAI* botAI) { return new CastGreaterHealOnPartyAction(botAI); }
    static Action* heal(PlayerbotAI* botAI) { return new CastHealAction(botAI); }
    static Action* heal_on_party(PlayerbotAI* botAI) { return new CastHealOnPartyAction(botAI); }
    static Action* lesser_heal(PlayerbotAI* botAI) { return new CastLesserHealAction(botAI); }
    static Action* lesser_heal_on_party(PlayerbotAI* botAI) { return new CastLesserHealOnPartyAction(botAI); }
    static Action* flash_heal(PlayerbotAI* botAI) { return new CastFlashHealAction(botAI); }
    static Action* flash_heal_on_party(PlayerbotAI* botAI) { return new CastFlashHealOnPartyAction(botAI); }
    static Action* dispel_magic(PlayerbotAI* botAI) { return new CastDispelMagicAction(botAI); }
    static Action* dispel_magic_on_party(PlayerbotAI* botAI) { return new CastDispelMagicOnPartyAction(botAI); }
    static Action* dispel_magic_on_target(PlayerbotAI* botAI) { return new CastDispelMagicOnTargetAction(botAI); }
    static Action* cure_disease(PlayerbotAI* botAI) { return new CastCureDiseaseAction(botAI); }
    static Action* cure_disease_on_party(PlayerbotAI* botAI) { return new CastCureDiseaseOnPartyAction(botAI); }
    static Action* abolish_disease(PlayerbotAI* botAI) { return new CastAbolishDiseaseAction(botAI); }
    static Action* abolish_disease_on_party(PlayerbotAI* botAI) { return new CastAbolishDiseaseOnPartyAction(botAI); }
    static Action* fade(PlayerbotAI* botAI) { return new CastFadeAction(botAI); }
    static Action* inner_fire(PlayerbotAI* botAI) { return new CastInnerFireAction(botAI); }
    static Action* shackle_undead(PlayerbotAI* botAI) { return new CastShackleUndeadAction(botAI); }
    static Action* feedback(PlayerbotAI* botAI) { return new CastFeedbackAction(botAI); }
    static Action* elunes_grace(PlayerbotAI* botAI) { return new CastElunesGraceAction(botAI); }
    static Action* starshards(PlayerbotAI* botAI) { return new CastStarshardsAction(botAI); }
    static Action* fear_ward_on_main_tank(PlayerbotAI* botAI) { return new CastFearWardOnMainTankAction(botAI); }
    static Action* power_word_shield_on_main_tank(PlayerbotAI* botAI)
    {
        return new CastPowerWordShieldOnMainTankAction(botAI);
    }
    static Action* prayer_of_mending_on_main_tank(PlayerbotAI* botAI)
    {
        return new CastPrayerOfMendingOnMainTankAction(botAI);
    }
    static Action* renew_on_main_tank(PlayerbotAI* botAI) { return new CastRenewOnMainTankAction(botAI); }
    static Action* desperate_prayer(PlayerbotAI* botAI) { return new CastDesperatePrayerAction(botAI); }
    static Action* shadowguard(PlayerbotAI* botAI) { return new CastShadowguardAction(botAI); }
    static Action* hex_of_weakness(PlayerbotAI* botAI) { return new CastHexOfWeaknessAction(botAI); }
    static Action* touch_of_weakness(PlayerbotAI* botAI) { return new CastTouchOfWeaknessAction(botAI); }
    static Action* mind_soothe(PlayerbotAI* botAI) { return new CastMindSootheAction(botAI); }
    static Action* lightwell(PlayerbotAI* botAI) { return new CastLightwellAction(botAI); }
    static Action* prayer_of_healing(PlayerbotAI* botAI) { return new CastPrayerOfHealingAction(botAI); }
    static Action* levitate(PlayerbotAI* botAI) { return new CastLevitateAction(botAI); }
    static Action* mana_burn(PlayerbotAI* botAI) { return new CastManaBurnAction(botAI); }
    static Action* silence_on_enemy_healer(PlayerbotAI* botAI) { return new CastSilenceOnEnemyHealerAction(botAI); }
    static Action* silence(PlayerbotAI* botAI) { return new CastSilenceAction(botAI); }
    static Action* power_infusion_on_party(PlayerbotAI* botAI) { return new CastPowerInfusionOnPartyAction(botAI); }
    static Action* binding_heal(PlayerbotAI* botAI) { return new CastBindingHealAction(botAI); }
    static Action* prayer_of_mending(PlayerbotAI* botAI) { return new CastPrayerOfMendingAction(botAI); }
    static Action* pain_suppression_on_party(PlayerbotAI* botAI) { return new CastPainSuppressionProtectAction(botAI); }
    static Action* pain_suppression_spike(PlayerbotAI* botAI)
    {
        return new CastSpikeProtectSpellAction(botAI, "pain suppression");
    }
    static Action* guardian_spirit_spike(PlayerbotAI* botAI)
    {
        return new CastSpikeProtectSpellAction(botAI, "guardian spirit");
    }
    static Action* shield_on_rezzed(PlayerbotAI* botAI)
    {
        return new CastPowerWordShieldOnValueTargetAction(botAI, "battle resurrected member", "battle resurrected");
    }
    // [guide_b31_dead]
    static Action* penance_on_main_tank(PlayerbotAI* botAI)
    {
        return new CastHealOnValueTargetAction(botAI, "penance", "main tank", "main tank", 25.0f,
                                               HealingManaEfficiency::HIGH, true, 95.0f);
    }
    static Action* greater_heal_on_main_tank(PlayerbotAI* botAI)
    {
        return new CastHealOnValueTargetAction(botAI, "greater heal", "main tank", "main tank", 50.0f,
                                               HealingManaEfficiency::MEDIUM, true, 70.0f);
    }
    static Action* penance_on_rezzed(PlayerbotAI* botAI)
    {
        return new CastHealOnValueTargetAction(botAI, "penance", "battle resurrected member", "battle resurrected",
                                               25.0f, HealingManaEfficiency::HIGH);
    }
    static Action* flash_heal_on_rezzed(PlayerbotAI* botAI)
    {
        return new CastHealOnValueTargetAction(botAI, "flash heal", "battle resurrected member", "battle resurrected",
                                               15.0f, HealingManaEfficiency::LOW);
    }
    static Action* renew_on_rezzed(PlayerbotAI* botAI)
    {
        return new CastHealOnValueTargetAction(botAI, "renew", "battle resurrected member", "battle resurrected",
                                               15.0f, HealingManaEfficiency::VERY_HIGH);
    }
    static Action* fortitude_on_rezzed(PlayerbotAI* botAI)
    {
        return new CastBuffOnValueTargetAction(botAI, "power word: fortitude", "battle resurrected member",
                                               "battle resurrected");
    }
    static Action* divine_spirit_on_rezzed(PlayerbotAI* botAI)
    {
        return new CastBuffOnValueTargetAction(botAI, "divine spirit", "battle resurrected member",
                                               "battle resurrected");
    }
    static Action* shadow_protection_on_rezzed(PlayerbotAI* botAI)
    {
        return new CastBuffOnValueTargetAction(botAI, "shadow protection", "battle resurrected member",
                                               "battle resurrected");
    }
    static Action* shield_on_peeling_tank(PlayerbotAI* botAI)
    {
        return new CastPowerWordShieldOnValueTargetAction(botAI, "peeling tank", "peeling tank");
    }
    static Action* renew_on_peeling_tank(PlayerbotAI* botAI)
    {
        return new CastHealOnValueTargetAction(botAI, "renew", "peeling tank", "peeling tank", 15.0f,
                                               HealingManaEfficiency::VERY_HIGH);
    }
    static Action* flash_heal_on_peeling_tank(PlayerbotAI* botAI)
    {
        return new CastHealOnValueTargetAction(botAI, "flash heal", "peeling tank", "peeling tank", 15.0f,
                                               HealingManaEfficiency::LOW, true, ai::healer::PEEL_TANK_TOP_UP_PCT);
    }
    static Action* pain_suppression(PlayerbotAI* botAI) { return new CastPainSuppressionAction(botAI); }
    static Action* mass_dispel(PlayerbotAI* botAI) { return new CastMassDispelAction(botAI); }
    static Action* shadowfiend(PlayerbotAI* botAI) { return new CastShadowfiendAction(botAI); }
    static Action* shadow_word_death(PlayerbotAI* botAI) { return new CastShadowWordDeathAction(botAI); }
    static Action* chastise(PlayerbotAI* botAI) { return new CastChastiseAction(botAI); }
    static Action* consume_magic(PlayerbotAI* botAI) { return new CastConsumeMagicAction(botAI); }
    static Action* symbol_of_hope(PlayerbotAI* botAI) { return new CastSymbolOfHopeAction(botAI); }
    static Action* penance_on_party(PlayerbotAI* botAI) { return new CastPenanceOnPartyAction(botAI); }
    static Action* hymn_of_hope(PlayerbotAI* botAI) { return new CastHymnOfHopeAction(botAI); }
    static Action* divine_hymn(PlayerbotAI* botAI) { return new CastDivineHymnAction(botAI); }
    static Action* mind_sear(PlayerbotAI* botAI) { return new CastMindSearAction(botAI); }
    static Action* guardian_spirit_on_party(PlayerbotAI* botAI) { return new CastGuardianSpiritOnPartyAction(botAI); }
};

SharedNamedObjectContextList<Strategy> PriestAiObjectContext::sharedStrategyContexts;
SharedNamedObjectContextList<Action> PriestAiObjectContext::sharedActionContexts;
SharedNamedObjectContextList<Trigger> PriestAiObjectContext::sharedTriggerContexts;
SharedNamedObjectContextList<UntypedValue> PriestAiObjectContext::sharedValueContexts;

PriestAiObjectContext::PriestAiObjectContext(PlayerbotAI* botAI)
    : AiObjectContext(botAI, sharedStrategyContexts, sharedActionContexts, sharedTriggerContexts, sharedValueContexts)
{
}

void PriestAiObjectContext::BuildSharedContexts()
{
    BuildSharedStrategyContexts(sharedStrategyContexts);
    BuildSharedActionContexts(sharedActionContexts);
    BuildSharedTriggerContexts(sharedTriggerContexts);
    BuildSharedValueContexts(sharedValueContexts);
}

void PriestAiObjectContext::BuildSharedStrategyContexts(SharedNamedObjectContextList<Strategy>& strategyContexts)
{
    AiObjectContext::BuildSharedStrategyContexts(strategyContexts);
    strategyContexts.Add(new PriestStrategyFactoryInternal());
    strategyContexts.Add(new PriestCombatStrategyFactoryInternal());
}

void PriestAiObjectContext::BuildSharedActionContexts(SharedNamedObjectContextList<Action>& actionContexts)
{
    AiObjectContext::BuildSharedActionContexts(actionContexts);
    actionContexts.Add(new PriestAiObjectContextInternal());
}

void PriestAiObjectContext::BuildSharedTriggerContexts(SharedNamedObjectContextList<Trigger>& triggerContexts)
{
    AiObjectContext::BuildSharedTriggerContexts(triggerContexts);
    triggerContexts.Add(new PriestTriggerFactoryInternal());
}

class PriestValueContextInternal : public NamedObjectContext<UntypedValue>
{
public:
    PriestValueContextInternal()
    {
        creators["battle resurrected member"] = &PriestValueContextInternal::battle_resurrected_member;
        creators["peeling tank"] = &PriestValueContextInternal::peeling_tank;
    }

private:
    static UntypedValue* battle_resurrected_member(PlayerbotAI* botAI)
    {
        return new BattleResurrectedMemberValue(botAI);
    }
    static UntypedValue* peeling_tank(PlayerbotAI* botAI) { return new PeelingTankValue(botAI); }
};

void PriestAiObjectContext::BuildSharedValueContexts(SharedNamedObjectContextList<UntypedValue>& valueContexts)
{
    AiObjectContext::BuildSharedValueContexts(valueContexts);
    valueContexts.Add(new PriestValueContextInternal());
}
