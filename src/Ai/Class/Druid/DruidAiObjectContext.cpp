/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DruidAiObjectContext.h"
#include "BalanceDruidStrategy.h"
#include "BearDruidStrategy.h"
#include "CatDruidStrategy.h"
#include "DruidActions.h"
#include "DruidBearActions.h"
#include "DruidCatActions.h"
#include "DruidPullStrategy.h"
#include "DruidShapeshiftActions.h"
#include "DruidTriggers.h"
#include "GenericDruidNonCombatStrategy.h"
#include "GenericDruidStrategy.h"
#include "Playerbots.h"
#include "RestoDruidStrategy.h"
#include "TankAoeThreat.h"

class DruidStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    DruidStrategyFactoryInternal()
    {
        creators["nc"] = &DruidStrategyFactoryInternal::nc;
        creators["pull"] = &DruidStrategyFactoryInternal::pull;
        creators["aoe"] = &DruidStrategyFactoryInternal::aoe;
        creators["cure"] = &DruidStrategyFactoryInternal::cure;
        creators["buff"] = &DruidStrategyFactoryInternal::buff;
        creators["boost"] = &DruidStrategyFactoryInternal::boost;
        creators["cc"] = &DruidStrategyFactoryInternal::cc;
        creators["healer dps"] = &DruidStrategyFactoryInternal::healer_dps;
        creators["offheal"] = &DruidStrategyFactoryInternal::offheal;
        creators["blanketing"] = &DruidStrategyFactoryInternal::blanketing;
        creators["tranquility"] = &DruidStrategyFactoryInternal::tranquility;
        creators["feral charge"] = &DruidStrategyFactoryInternal::feral_charge;
    }

private:
    static Strategy* nc(PlayerbotAI* botAI) { return new GenericDruidNonCombatStrategy(botAI); }
    static Strategy* pull(PlayerbotAI* botAI) { return new DruidPullStrategy(botAI); }
    static Strategy* aoe(PlayerbotAI* botAI) { return new DruidAoeStrategy(botAI); }
    static Strategy* cure(PlayerbotAI* botAI) { return new DruidCureStrategy(botAI); }
    static Strategy* buff(PlayerbotAI* botAI) { return new GenericDruidBuffStrategy(botAI); }
    static Strategy* boost(PlayerbotAI* botAI) { return new DruidBoostStrategy(botAI); }
    static Strategy* cc(PlayerbotAI* botAI) { return new DruidCcStrategy(botAI); }
    static Strategy* healer_dps(PlayerbotAI* botAI) { return new DruidHealerDpsStrategy(botAI); }
    static Strategy* offheal(PlayerbotAI* botAI) { return new CatOffhealStrategy(botAI); }
    static Strategy* blanketing(PlayerbotAI* botAI) { return new DruidBlanketStrategy(botAI); }
    static Strategy* tranquility(PlayerbotAI* botAI) { return new DruidTranquilityStrategy(botAI); }
    static Strategy* feral_charge(PlayerbotAI* botAI) { return new FeralChargeDruidStrategy(botAI); }
};

class DruidDruidStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    DruidDruidStrategyFactoryInternal() : NamedObjectContext<Strategy>(false, true)
    {
        creators["bear"] = &DruidDruidStrategyFactoryInternal::bear;
        creators["tank"] = &DruidDruidStrategyFactoryInternal::bear;
        creators["cat"] = &DruidDruidStrategyFactoryInternal::cat;
        creators["dps"] = &DruidDruidStrategyFactoryInternal::cat;
        creators["balance"] = &DruidDruidStrategyFactoryInternal::balance;
        creators["resto"] = &DruidDruidStrategyFactoryInternal::resto;
        creators["heal"] = &DruidDruidStrategyFactoryInternal::resto;
    }

private:
    static Strategy* bear(PlayerbotAI* botAI) { return new BearDruidStrategy(botAI); }
    static Strategy* cat(PlayerbotAI* botAI) { return new CatDruidStrategy(botAI); }
    static Strategy* balance(PlayerbotAI* botAI) { return new BalanceDruidStrategy(botAI); }
    static Strategy* resto(PlayerbotAI* botAI) { return new RestoDruidStrategy(botAI); }
};

class DruidTriggerFactoryInternal : public NamedObjectContext<Trigger>
{
public:
    DruidTriggerFactoryInternal()
    {
        creators["clearcasting"] = &DruidTriggerFactoryInternal::clearcasting;
        creators["thorns"] = &DruidTriggerFactoryInternal::thorns;
        creators["thorns on party"] = &DruidTriggerFactoryInternal::thorns_on_party;
        creators["thorns on main tank"] = &DruidTriggerFactoryInternal::thorns_on_main_tank;
        creators["bash"] = &DruidTriggerFactoryInternal::bash;
        creators["faerie fire (feral)"] = &DruidTriggerFactoryInternal::faerie_fire_feral;
        creators["faerie fire"] = &DruidTriggerFactoryInternal::faerie_fire;
        creators["insect swarm on attacker"] = &DruidTriggerFactoryInternal::insect_swarm_on_attacker;
        creators["moonfire on attacker"] = &DruidTriggerFactoryInternal::moonfire_on_attacker;
        creators["insect swarm missing"] = &DruidTriggerFactoryInternal::insect_swarm_missing;  // [b31_sd]
        creators["moonfire missing"] = &DruidTriggerFactoryInternal::moonfire_missing;          // [b31_sd]
        creators["nature's grasp"] = &DruidTriggerFactoryInternal::natures_grasp;
        creators["berserk"] = &DruidTriggerFactoryInternal::berserk;
        creators["berserk active"] = &DruidTriggerFactoryInternal::berserk_active;
        creators["savage roar"] = &DruidTriggerFactoryInternal::savage_roar;
        creators["savage roar clip"] = &DruidTriggerFactoryInternal::savage_roar_clip;  // [b31_sd]
        creators["tiger's fury"] = &DruidTriggerFactoryInternal::tigers_fury;            // [b31_sd]
        creators["rake"] = &DruidTriggerFactoryInternal::rake;
        creators["mark of the wild"] = &DruidTriggerFactoryInternal::mark_of_the_wild;
        creators["mark of the wild on party"] = &DruidTriggerFactoryInternal::mark_of_the_wild_on_party;
        creators["cure poison"] = &DruidTriggerFactoryInternal::cure_poison;
        creators["party member cure poison"] = &DruidTriggerFactoryInternal::party_member_cure_poison;
        creators["entangling roots"] = &DruidTriggerFactoryInternal::entangling_roots;
        creators["entangling roots kite"] = &DruidTriggerFactoryInternal::entangling_roots_kite;
        creators["hibernate"] = &DruidTriggerFactoryInternal::hibernate;
        creators["bear form"] = &DruidTriggerFactoryInternal::bear_form;
        creators["bear tank form"] = &DruidTriggerFactoryInternal::bear_tank_form;  // [b32_tank_aoe]
        creators["no moonkin form"] = &DruidTriggerFactoryInternal::no_moonkin_form;  // [rot1]
        creators["cat form"] = &DruidTriggerFactoryInternal::cat_form;
        creators["tree form"] = &DruidTriggerFactoryInternal::tree_form;
        creators["eclipse (solar)"] = &DruidTriggerFactoryInternal::eclipse_solar;
        creators["eclipse (lunar)"] = &DruidTriggerFactoryInternal::eclipse_lunar;
        creators["bash on enemy healer"] = &DruidTriggerFactoryInternal::bash_on_enemy_healer;
        creators["nature's swiftness"] = &DruidTriggerFactoryInternal::natures_swiftness;
        creators["nature's swiftness active"] = &DruidTriggerFactoryInternal::natures_swiftness_active;
        creators["party member remove curse"] = &DruidTriggerFactoryInternal::party_member_remove_curse;
        creators["mangle (bear)"] = &DruidTriggerFactoryInternal::mangle_bear_trigger;
        creators["lacerate"] = &DruidTriggerFactoryInternal::lacerate_trigger;
        creators["swipe (bear) rage dump"] = &DruidTriggerFactoryInternal::swipe_bear_rage_dump;  // [b31_deep]
        creators["demoralizing roar"] = &DruidTriggerFactoryInternal::demoralize_roar;
        creators["tank aoe threat"] = &DruidTriggerFactoryInternal::tank_aoe_threat;
        creators["tank aoe pack"] = &DruidTriggerFactoryInternal::tank_aoe_pack;  // [b32_tank_aoe]
        creators["tank aoe crowd"] = &DruidTriggerFactoryInternal::tank_aoe_crowd;  // [b32_peel]
        creators["bear battle res"] = &DruidTriggerFactoryInternal::bear_battle_res;                  // [b32_brez]
        creators["bear battle res reshift"] = &DruidTriggerFactoryInternal::bear_battle_res_reshift;  // [b32_brez]
        creators["mangle (cat)"] = &DruidTriggerFactoryInternal::mangle_cat;
        creators["ferocious bite time"] = &DruidTriggerFactoryInternal::ferocious_bite_time;
        creators["ferocious bite execute"] = &DruidTriggerFactoryInternal::ferocious_bite_execute;
        creators["hurricane channel check"] = &DruidTriggerFactoryInternal::hurricane_channel_check;
        creators["no healer dps strategy"] = &DruidTriggerFactoryInternal::no_healer_dps_strategy;
        // [b31_deep]
        creators["enemy too close for spell in danger"] = &DruidTriggerFactoryInternal::enemy_too_close_in_danger;
        creators["starfall"] = &DruidTriggerFactoryInternal::starfall;
        creators["force of nature"] = &DruidTriggerFactoryInternal::force_of_nature;
        creators["cyclone"] = &DruidTriggerFactoryInternal::cyclone;
        creators["predator's swiftness"] = &DruidTriggerFactoryInternal::predators_swiftness;
        creators["predator's swiftness and cyclone"] = &DruidTriggerFactoryInternal::predators_swiftness_and_cyclone;
        creators["predator's swiftness and hibernate"] = &DruidTriggerFactoryInternal::predators_swiftness_and_hibernate;
        creators["predator's swiftness and entangling roots"] = &DruidTriggerFactoryInternal::predators_swiftness_and_entangling_roots;
        creators["predator's swiftness and combat party member dead"] = &DruidTriggerFactoryInternal::predators_swiftness_and_combat_party_member_dead;
        creators["clearcasting and medium aoe"] = &DruidTriggerFactoryInternal::clearcasting_and_medium_aoe;
        creators["prowl"] = &DruidTriggerFactoryInternal::prowl_trigger;
        creators["rebirth target low health"] = &DruidTriggerFactoryInternal::rebirth_target_low_health;
        creators["rebirth target unbuffed"] = &DruidTriggerFactoryInternal::rebirth_target_unbuffed;
        creators["rejuvenation blanket"] = &DruidTriggerFactoryInternal::rejuvenation_blanket;
        creators["wild growth blanket"] = &DruidTriggerFactoryInternal::wild_growth_blanket;
        creators["healer should attack and not blanketing"] =
            &DruidTriggerFactoryInternal::healer_should_attack_and_not_blanketing;
        creators["aquatic form"] = &DruidTriggerFactoryInternal::aquatic_form;
        creators["lifebloom on main tank"] = &DruidTriggerFactoryInternal::lifebloom_on_main_tank;
        creators["rejuvenation on main tank"] = &DruidTriggerFactoryInternal::rejuvenation_on_main_tank;
    }

private:
    static Trigger* natures_swiftness(PlayerbotAI* botAI) { return new NaturesSwiftnessTrigger(botAI); }
    static Trigger* natures_swiftness_active(PlayerbotAI* botAI) { return new NaturesSwiftnessActiveTrigger(botAI); }
    static Trigger* clearcasting(PlayerbotAI* botAI) { return new ClearcastingTrigger(botAI); }
    static Trigger* eclipse_solar(PlayerbotAI* botAI) { return new EclipseSolarTrigger(botAI); }
    static Trigger* eclipse_lunar(PlayerbotAI* botAI) { return new EclipseLunarTrigger(botAI); }
    static Trigger* thorns(PlayerbotAI* botAI) { return new ThornsTrigger(botAI); }
    static Trigger* thorns_on_party(PlayerbotAI* botAI) { return new ThornsOnPartyTrigger(botAI); }
    static Trigger* thorns_on_main_tank(PlayerbotAI* botAI) { return new ThornsOnMainTankTrigger(botAI); }
    static Trigger* bash(PlayerbotAI* botAI) { return new BashInterruptSpellTrigger(botAI); }
    static Trigger* faerie_fire_feral(PlayerbotAI* botAI) { return new FaerieFireFeralTrigger(botAI); }
    static Trigger* insect_swarm_on_attacker(PlayerbotAI* botAI) { return new InsectSwarmOnAttackerTrigger(botAI); }
    static Trigger* moonfire_on_attacker(PlayerbotAI* botAI) { return new MoonfireOnAttackerTrigger(botAI); }
    static Trigger* insect_swarm_missing(PlayerbotAI* botAI)  // [b31_sd] [rot30]
    {
        return new InsectSwarmMissingTrigger(botAI);
    }
    static Trigger* moonfire_missing(PlayerbotAI* botAI) { return new DruidDotMissingTrigger(botAI, "moonfire"); }
    static Trigger* faerie_fire(PlayerbotAI* botAI) { return new FaerieFireTrigger(botAI); }
    static Trigger* natures_grasp(PlayerbotAI* botAI) { return new NaturesGraspTrigger(botAI); }
    static Trigger* berserk(PlayerbotAI* botAI) { return new BerserkTrigger(botAI); }
    static Trigger* berserk_active(PlayerbotAI* botAI) { return new BerserkActiveTrigger(botAI); }
    static Trigger* savage_roar(PlayerbotAI* botAI) { return new SavageRoarTrigger(botAI); }
    static Trigger* savage_roar_clip(PlayerbotAI* botAI) { return new SavageRoarClipTrigger(botAI); }  // [b31_sd]
    static Trigger* tigers_fury(PlayerbotAI* botAI) { return new TigersFuryTrigger(botAI); }            // [b31_sd]
    static Trigger* rake(PlayerbotAI* botAI) { return new RakeTrigger(botAI); }
    static Trigger* mark_of_the_wild(PlayerbotAI* botAI) { return new MarkOfTheWildTrigger(botAI); }
    static Trigger* mark_of_the_wild_on_party(PlayerbotAI* botAI) { return new MarkOfTheWildOnPartyTrigger(botAI); }
    static Trigger* cure_poison(PlayerbotAI* botAI) { return new CurePoisonTrigger(botAI); }
    static Trigger* party_member_cure_poison(PlayerbotAI* botAI) { return new PartyMemberCurePoisonTrigger(botAI); }
    static Trigger* entangling_roots(PlayerbotAI* botAI) { return new EntanglingRootsTrigger(botAI); }
    static Trigger* entangling_roots_kite(PlayerbotAI* botAI) { return new EntanglingRootsKiteTrigger(botAI); }
    static Trigger* hibernate(PlayerbotAI* botAI) { return new HibernateTrigger(botAI); }
    static Trigger* bear_form(PlayerbotAI* botAI) { return new BearFormTrigger(botAI); }
    static Trigger* bear_tank_form(PlayerbotAI* botAI) { return new BearTankFormTrigger(botAI); }  // [b32_tank_aoe]
    static Trigger* no_moonkin_form(PlayerbotAI* botAI) { return new MoonkinFormUpkeepTrigger(botAI); }  // [rot1]
    static Trigger* cat_form(PlayerbotAI* botAI) { return new CatFormTrigger(botAI); }
    static Trigger* tree_form(PlayerbotAI* botAI) { return new TreeFormTrigger(botAI); }
    static Trigger* bash_on_enemy_healer(PlayerbotAI* botAI) { return new BashInterruptEnemyHealerSpellTrigger(botAI); }
    static Trigger* party_member_remove_curse(PlayerbotAI* botAI) { return new DruidPartyMemberRemoveCurseTrigger(botAI); }
    static Trigger* mangle_bear_trigger(PlayerbotAI* botAI) { return new MangleBearTrigger(botAI); }
    static Trigger* lacerate_trigger(PlayerbotAI* botAI) { return new LacerateTrigger(botAI); }
    static Trigger* swipe_bear_rage_dump(PlayerbotAI* botAI) { return new SwipeBearRageDumpTrigger(botAI); }
    static Trigger* demoralize_roar(PlayerbotAI* botAI) { return new DemoralizeRoarTrigger(botAI); }
    // Swipe (Bear) reaches 8 yd
    static Trigger* tank_aoe_threat(PlayerbotAI* botAI) { return new TankAoeThreatTrigger(botAI, 8.0f); }
    static Trigger* tank_aoe_pack(PlayerbotAI* botAI) { return new TankAoePackTrigger(botAI, 8.0f); }  // [b32_tank_aoe]
    static Trigger* tank_aoe_crowd(PlayerbotAI* botAI) { return new TankAoeCrowdTrigger(botAI, 8.0f); }  // [b32_peel]
    static Trigger* bear_battle_res(PlayerbotAI* botAI) { return new BearBattleResTrigger(botAI); }  // [b32_brez]
    static Trigger* bear_battle_res_reshift(PlayerbotAI* botAI)  // [b32_brez]
    {
        return new BearBattleResReshiftTrigger(botAI);
    }
    static Trigger* mangle_cat(PlayerbotAI* botAI) { return new MangleCatTrigger(botAI); }
    static Trigger* ferocious_bite_time(PlayerbotAI* botAI) { return new FerociousBiteTimeTrigger(botAI); }
    static Trigger* ferocious_bite_execute(PlayerbotAI* botAI) { return new FerociousBiteExecuteTrigger(botAI); }
    static Trigger* hurricane_channel_check(PlayerbotAI* botAI) { return new HurricaneChannelCheckTrigger(botAI); }
    static Trigger* no_healer_dps_strategy(PlayerbotAI* botAI) { return new NoHealerDpsStrategyTrigger(botAI); }
    static Trigger* enemy_too_close_in_danger(PlayerbotAI* botAI)  // [b31_deep]
    {
        return new DruidEnemyTooCloseInDangerTrigger(botAI);
    }
    static Trigger* starfall(PlayerbotAI* botAI) { return new StarfallTrigger(botAI); }
    static Trigger* force_of_nature(PlayerbotAI* botAI) { return new ForceOfNatureTrigger(botAI); }
    static Trigger* cyclone(PlayerbotAI* botAI) { return new CycloneTrigger(botAI); }
    static Trigger* predators_swiftness(PlayerbotAI* botAI) { return new PredatorsSwiftnessTrigger(botAI); }
    static Trigger* predators_swiftness_and_cyclone(PlayerbotAI* botAI) { return new TwoTriggers(botAI, "predator's swiftness", "cyclone"); }
    static Trigger* predators_swiftness_and_hibernate(PlayerbotAI* botAI) { return new TwoTriggers(botAI, "predator's swiftness", "hibernate"); }
    static Trigger* predators_swiftness_and_entangling_roots(PlayerbotAI* botAI) { return new TwoTriggers(botAI, "predator's swiftness", "entangling roots"); }
    static Trigger* predators_swiftness_and_combat_party_member_dead(PlayerbotAI* botAI) { return new TwoTriggers(botAI, "predator's swiftness", "combat party member dead"); }
    static Trigger* clearcasting_and_medium_aoe(PlayerbotAI* botAI) { return new TwoTriggers(botAI, "clearcasting", "medium aoe"); }
    static Trigger* prowl_trigger(PlayerbotAI* botAI) { return new ProwlTrigger(botAI); }
    static Trigger* rebirth_target_low_health(PlayerbotAI* botAI) { return new RebirthTargetLowHealthTrigger(botAI); }
    static Trigger* rebirth_target_unbuffed(PlayerbotAI* botAI) { return new RebirthTargetNoMarkTrigger(botAI); }
    static Trigger* rejuvenation_blanket(PlayerbotAI* botAI)
    {
        return new BlanketHotTrigger(botAI, "rejuvenation blanket", "rejuvenation");
    }
    static Trigger* wild_growth_blanket(PlayerbotAI* botAI)
    {
        return new BlanketHotTrigger(botAI, "wild growth blanket", "wild growth");
    }
    static Trigger* healer_should_attack_and_not_blanketing(PlayerbotAI* botAI)
    {
        return new HealerShouldAttackAndNotBlanketingTrigger(botAI);
    }
    static Trigger* aquatic_form(PlayerbotAI* botAI) { return new AquaticFormTrigger(botAI); }
    static Trigger* lifebloom_on_main_tank(PlayerbotAI* botAI) { return new LifebloomOnMainTankTrigger(botAI); }
    static Trigger* rejuvenation_on_main_tank(PlayerbotAI* botAI) { return new RejuvenationOnMainTankTrigger(botAI); }
};

class DruidAiObjectContextInternal : public NamedObjectContext<Action>
{
public:
    DruidAiObjectContextInternal()
    {
        creators["feral charge - bear"] = &DruidAiObjectContextInternal::feral_charge_bear;
        creators["feral charge - cat"] = &DruidAiObjectContextInternal::feral_charge_cat;
        creators["swipe (bear)"] = &DruidAiObjectContextInternal::swipe_bear;
        creators["swipe (bear) on group member attackers"] =
            &DruidAiObjectContextInternal::swipe_bear_on_group_member_attackers;
        creators["swipe (bear) tank aoe"] = &DruidAiObjectContextInternal::swipe_bear_tank_aoe;
        creators["faerie fire (feral)"] = &DruidAiObjectContextInternal::faerie_fire_feral;
        creators["faerie fire"] = &DruidAiObjectContextInternal::faerie_fire;
        creators["bear form"] = &DruidAiObjectContextInternal::bear_form;
        creators["dire bear form"] = &DruidAiObjectContextInternal::dire_bear_form;
        creators["moonkin form"] = &DruidAiObjectContextInternal::moonkin_form;
        creators["cat form"] = &DruidAiObjectContextInternal::cat_form;
        creators["tree form"] = &DruidAiObjectContextInternal::tree_form;
        creators["travel form"] = &DruidAiObjectContextInternal::travel_form;
        creators["aquatic form"] = &DruidAiObjectContextInternal::aquatic_form;
        creators["caster form"] = &DruidAiObjectContextInternal::caster_form;
        creators["leave animal form"] = &DruidAiObjectContextInternal::leave_animal_form;  // [rot1]
        creators["cancel tree form"] = &DruidAiObjectContextInternal::cancel_tree_form;
        creators["cancel travel form"] = &DruidAiObjectContextInternal::cancel_travel_form;
        creators["cancel bear form"] = &DruidAiObjectContextInternal::cancel_bear_form;
        creators["cancel dire bear form"] = &DruidAiObjectContextInternal::cancel_dire_bear_form;
        creators["cancel cat form"] = &DruidAiObjectContextInternal::cancel_cat_form;
        creators["cancel moonkin form"] = &DruidAiObjectContextInternal::cancel_moonkin_form;
        creators["cancel aquatic form"] = &DruidAiObjectContextInternal::cancel_aquatic_form;
        creators["mangle (bear)"] = &DruidAiObjectContextInternal::mangle_bear;
        creators["maul"] = &DruidAiObjectContextInternal::maul;
        creators["bash"] = &DruidAiObjectContextInternal::bash;
        creators["growl"] = &DruidAiObjectContextInternal::growl;
        creators["growl on group member attacker"] = &DruidAiObjectContextInternal::growl_on_group_member_attacker;
        creators["rejuvenation on rebirth target"] = &DruidAiObjectContextInternal::rejuvenation_on_rebirth_target;
        creators["swiftmend on rebirth target"] = &DruidAiObjectContextInternal::swiftmend_on_rebirth_target;
        creators["regrowth on rebirth target"] = &DruidAiObjectContextInternal::regrowth_on_rebirth_target;
        creators["nourish on rebirth target"] = &DruidAiObjectContextInternal::nourish_on_rebirth_target;
        creators["healing touch on rebirth target"] = &DruidAiObjectContextInternal::healing_touch_on_rebirth_target;
        creators["mark of the wild on rebirth target"] =
            &DruidAiObjectContextInternal::mark_of_the_wild_on_rebirth_target;
        creators["thorns on rebirth target"] = &DruidAiObjectContextInternal::thorns_on_rebirth_target;
        creators["feral charge - bear on group member attacker"] =
            &DruidAiObjectContextInternal::feral_charge_bear_on_group_member_attacker;
        creators["faerie fire (feral) on group member attacker"] =
            &DruidAiObjectContextInternal::faerie_fire_feral_on_group_member_attacker;
        creators["mangle (bear) on group member attacker"] =
            &DruidAiObjectContextInternal::mangle_bear_on_group_member_attacker;
        creators["challenging roar"] = &DruidAiObjectContextInternal::challenging_roar;
        creators["demoralizing roar"] = &DruidAiObjectContextInternal::demoralizing_roar;
        creators["hibernate"] = &DruidAiObjectContextInternal::hibernate;
        creators["entangling roots"] = &DruidAiObjectContextInternal::entangling_roots;
        creators["entangling roots on cc"] = &DruidAiObjectContextInternal::entangling_roots_on_cc;
        creators["hibernate on cc"] = &DruidAiObjectContextInternal::hibernate_on_cc;
        creators["cyclone on cc"] = &DruidAiObjectContextInternal::cyclone_on_cc;
        creators["wrath"] = &DruidAiObjectContextInternal::wrath;
        creators["starfall"] = &DruidAiObjectContextInternal::starfall;
        creators["insect swarm"] = &DruidAiObjectContextInternal::insect_swarm;
        creators["moonfire"] = &DruidAiObjectContextInternal::moonfire;
        creators["moonfire on the move"] = &DruidAiObjectContextInternal::moonfire_on_the_move;
        creators["starfire"] = &DruidAiObjectContextInternal::starfire;
        creators["nature's grasp"] = &DruidAiObjectContextInternal::natures_grasp;
        creators["claw"] = &DruidAiObjectContextInternal::claw;
        creators["mangle (cat)"] = &DruidAiObjectContextInternal::mangle_cat;
        creators["swipe (cat)"] = &DruidAiObjectContextInternal::swipe_cat;
        creators["rake"] = &DruidAiObjectContextInternal::rake;
        creators["ferocious bite"] = &DruidAiObjectContextInternal::ferocious_bite;
        creators["rip"] = &DruidAiObjectContextInternal::rip;
        creators["maim"] = &DruidAiObjectContextInternal::maim;
        creators["cower"] = &DruidAiObjectContextInternal::cower;
        creators["survival instincts"] = &DruidAiObjectContextInternal::survival_instincts;
        creators["frenzied regeneration"] = &DruidAiObjectContextInternal::frenzied_regeneration;
        creators["thorns"] = &DruidAiObjectContextInternal::thorns;
        creators["thorns on party"] = &DruidAiObjectContextInternal::thorns_on_party;
        creators["thorns on main tank"] = &DruidAiObjectContextInternal::thorns_on_main_tank;
        creators["lifebloom on main tank"] = &DruidAiObjectContextInternal::lifebloom_on_main_tank;
        creators["rejuvenation on main tank"] = &DruidAiObjectContextInternal::rejuvenation_on_main_tank;
        creators["cure poison"] = &DruidAiObjectContextInternal::cure_poison;
        creators["cure poison on party"] = &DruidAiObjectContextInternal::cure_poison_on_party;
        creators["abolish poison"] = &DruidAiObjectContextInternal::abolish_poison;
        creators["abolish poison on party"] = &DruidAiObjectContextInternal::abolish_poison_on_party;
        creators["berserk"] = &DruidAiObjectContextInternal::berserk;
        creators["tiger's fury"] = &DruidAiObjectContextInternal::tigers_fury;
        creators["savage roar"] = &DruidAiObjectContextInternal::savage_roar;
        creators["savage roar clip"] = &DruidAiObjectContextInternal::savage_roar_clip;  // [b31_sd]
        creators["mark of the wild"] = &DruidAiObjectContextInternal::mark_of_the_wild;
        creators["mark of the wild on party"] = &DruidAiObjectContextInternal::mark_of_the_wild_on_party;
        creators["regrowth"] = &DruidAiObjectContextInternal::regrowth;
        creators["rejuvenation"] = &DruidAiObjectContextInternal::rejuvenation;
        creators["healing touch"] = &DruidAiObjectContextInternal::healing_touch;
        creators["regrowth on party"] = &DruidAiObjectContextInternal::regrowth_on_party;
        creators["rejuvenation on party"] = &DruidAiObjectContextInternal::rejuvenation_on_party;
        creators["rejuvenation on not full"] = &DruidAiObjectContextInternal::rejuvenation_on_not_full;
        creators["healing touch on party"] = &DruidAiObjectContextInternal::healing_touch_on_party;
        // [b31_sd]
        creators["healing touch filler on party"] = &DruidAiObjectContextInternal::healing_touch_filler_on_party;
        creators["rebirth"] = &DruidAiObjectContextInternal::rebirth;
        creators["bear battle res"] = &DruidAiObjectContextInternal::bear_battle_res;  // [b32_brez]
        creators["revive"] = &DruidAiObjectContextInternal::revive;
        creators["barkskin"] = &DruidAiObjectContextInternal::barkskin;
        creators["lacerate"] = &DruidAiObjectContextInternal::lacerate;
        creators["hurricane"] = &DruidAiObjectContextInternal::hurricane;
        creators["innervate"] = &DruidAiObjectContextInternal::innervate;
        creators["innervate on healer"] = &DruidAiObjectContextInternal::innervate_on_healer;
        creators["tranquility"] = &DruidAiObjectContextInternal::tranquility;
        creators["bash on enemy healer"] = &DruidAiObjectContextInternal::bash_on_enemy_healer;
        creators["nature's swiftness"] = &DruidAiObjectContextInternal::natures_swiftness;
        creators["prowl"] = &DruidAiObjectContextInternal::prowl;
        creators["dash"] = &DruidAiObjectContextInternal::dash;
        creators["shred"] = &DruidAiObjectContextInternal::shred;
        creators["ravage"] = &DruidAiObjectContextInternal::ravage;
        creators["pounce"] = &DruidAiObjectContextInternal::pounce;
        creators["wild growth on party"] = &DruidAiObjectContextInternal::wild_growth_on_party;
        creators["swiftmend on party"] = &DruidAiObjectContextInternal::swiftmend_on_party;
        creators["nourish on party"] = &DruidAiObjectContextInternal::nourish_on_party;
        creators["remove curse on party"] = &DruidAiObjectContextInternal::remove_curse_on_party;
        creators["insect swarm on attacker"] = &DruidAiObjectContextInternal::insect_swarm_on_attacker;
        creators["moonfire on attacker"] = &DruidAiObjectContextInternal::moonfire_on_attacker;
        creators["enrage"] = &DruidAiObjectContextInternal::enrage;
        creators["force of nature"] = &DruidAiObjectContextInternal::force_of_nature;
        creators["typhoon"] = &DruidAiObjectContextInternal::typhoon;
        creators["rejuvenation blanket"] = &DruidAiObjectContextInternal::rejuvenation_blanket;
        creators["wild growth blanket"] = &DruidAiObjectContextInternal::wild_growth_blanket;
    }

private:
    static Action* natures_swiftness(PlayerbotAI* botAI) { return new CastNaturesSwiftnessAction(botAI); }
    static Action* tranquility(PlayerbotAI* botAI) { return new CastTranquilityAction(botAI); }
    static Action* feral_charge_bear(PlayerbotAI* botAI) { return new CastFeralChargeBearAction(botAI); }
    static Action* feral_charge_cat(PlayerbotAI* botAI) { return new CastFeralChargeCatAction(botAI); }
    static Action* swipe_bear(PlayerbotAI* botAI) { return new CastSwipeBearAction(botAI); }
    static Action* swipe_bear_on_group_member_attackers(PlayerbotAI* botAI)
    {
        return new CastAreaSpellOnGroupMemberAttackersAction(botAI, "swipe (bear)", 8.0f);
    }
    // own name: the strategy's "swipe (bear)" node falls back to "melee", which would hold the tick at this relevance
    static Action* swipe_bear_tank_aoe(PlayerbotAI* botAI)
    {
        return new CastTankAreaThreatAction(botAI, "swipe (bear)");
    }
    static Action* faerie_fire_feral(PlayerbotAI* botAI) { return new CastFaerieFireFeralAction(botAI); }
    static Action* faerie_fire(PlayerbotAI* botAI) { return new CastFaerieFireAction(botAI); }
    static Action* bear_form(PlayerbotAI* botAI) { return new CastBearFormAction(botAI); }
    static Action* dire_bear_form(PlayerbotAI* botAI) { return new CastDireBearFormAction(botAI); }
    static Action* cat_form(PlayerbotAI* botAI) { return new CastCatFormAction(botAI); }
    static Action* tree_form(PlayerbotAI* botAI) { return new CastTreeFormAction(botAI); }
    static Action* travel_form(PlayerbotAI* botAI) { return new CastTravelFormAction(botAI); }
    static Action* aquatic_form(PlayerbotAI* botAI) { return new CastAquaticFormAction(botAI); }
    static Action* caster_form(PlayerbotAI* botAI) { return new CastCasterFormAction(botAI); }
    static Action* leave_animal_form(PlayerbotAI* botAI) { return new CastLeaveAnimalFormAction(botAI); }  // [rot1]
    static Action* cancel_tree_form(PlayerbotAI* botAI) { return new CastCancelTreeFormAction(botAI); }
    static Action* cancel_travel_form(PlayerbotAI* botAI) { return new CastCancelTravelFormAction(botAI); }
    static Action* cancel_bear_form(PlayerbotAI* botAI) { return new CastCancelBearFormAction(botAI); }
    static Action* cancel_dire_bear_form(PlayerbotAI* botAI) { return new CastCancelDireBearFormAction(botAI); }
    static Action* cancel_cat_form(PlayerbotAI* botAI) { return new CastCancelCatFormAction(botAI); }
    static Action* cancel_moonkin_form(PlayerbotAI* botAI) { return new CastCancelMoonkinFormAction(botAI); }
    static Action* cancel_aquatic_form(PlayerbotAI* botAI) { return new CastCancelAquaticFormAction(botAI); }
    static Action* mangle_bear(PlayerbotAI* botAI) { return new CastMangleBearAction(botAI); }
    static Action* maul(PlayerbotAI* botAI) { return new CastMaulAction(botAI); }
    static Action* bash(PlayerbotAI* botAI) { return new CastBashAction(botAI); }
    static Action* growl(PlayerbotAI* botAI) { return new CastGrowlAction(botAI); }
    static Action* faerie_fire_feral_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "faerie fire (feral)");
    }
    static Action* mangle_bear_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "mangle (bear)");
    }
    static Action* growl_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "growl");
    }
    static Action* rejuvenation_on_rebirth_target(PlayerbotAI* botAI)
    {
        return new CastSpellOnRebirthTargetAction(botAI, "rejuvenation", true);
    }
    static Action* swiftmend_on_rebirth_target(PlayerbotAI* botAI)
    {
        return new CastSpellOnRebirthTargetAction(botAI, "swiftmend");
    }
    static Action* regrowth_on_rebirth_target(PlayerbotAI* botAI)
    {
        return new CastSpellOnRebirthTargetAction(botAI, "regrowth", true);
    }
    static Action* nourish_on_rebirth_target(PlayerbotAI* botAI)
    {
        return new CastSpellOnRebirthTargetAction(botAI, "nourish");
    }
    static Action* healing_touch_on_rebirth_target(PlayerbotAI* botAI)
    {
        return new CastSpellOnRebirthTargetAction(botAI, "healing touch");
    }
    static Action* mark_of_the_wild_on_rebirth_target(PlayerbotAI* botAI)
    {
        return new CastBuffOnRebirthTargetAction(botAI, "mark of the wild", "gift of the wild", false);
    }
    static Action* thorns_on_rebirth_target(PlayerbotAI* botAI)
    {
        return new CastBuffOnRebirthTargetAction(botAI, "thorns", "", true);
    }
    static Action* feral_charge_bear_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "feral charge - bear");
    }
    static Action* challenging_roar(PlayerbotAI* botAI) { return new CastChallengingRoarAction(botAI); }
    static Action* demoralizing_roar(PlayerbotAI* botAI) { return new CastDemoralizingRoarAction(botAI); }
    static Action* moonkin_form(PlayerbotAI* botAI) { return new CastMoonkinFormAction(botAI); }
    static Action* hibernate(PlayerbotAI* botAI) { return new CastHibernateAction(botAI); }
    static Action* entangling_roots(PlayerbotAI* botAI) { return new CastEntanglingRootsAction(botAI); }
    static Action* hibernate_on_cc(PlayerbotAI* botAI) { return new CastHibernateCcAction(botAI); }
    static Action* entangling_roots_on_cc(PlayerbotAI* botAI) { return new CastEntanglingRootsCcAction(botAI); }
    static Action* cyclone_on_cc(PlayerbotAI* botAI) { return new CastCycloneCcAction(botAI); }
    static Action* wrath(PlayerbotAI* botAI) { return new CastWrathAction(botAI); }
    static Action* starfall(PlayerbotAI* botAI) { return new CastStarfallAction(botAI); }
    static Action* insect_swarm(PlayerbotAI* botAI) { return new CastInsectSwarmAction(botAI); }
    static Action* moonfire(PlayerbotAI* botAI) { return new CastMoonfireAction(botAI); }
    static Action* moonfire_on_the_move(PlayerbotAI* botAI) { return new CastMoonfireOnTheMoveAction(botAI); }
    static Action* starfire(PlayerbotAI* botAI) { return new CastStarfireAction(botAI); }
    static Action* natures_grasp(PlayerbotAI* botAI) { return new CastNaturesGraspAction(botAI); }
    static Action* claw(PlayerbotAI* botAI) { return new CastClawAction(botAI); }
    static Action* mangle_cat(PlayerbotAI* botAI) { return new CastMangleCatAction(botAI); }
    static Action* swipe_cat(PlayerbotAI* botAI) { return new CastSwipeCatAction(botAI); }
    static Action* rake(PlayerbotAI* botAI) { return new CastRakeAction(botAI); }
    static Action* ferocious_bite(PlayerbotAI* botAI) { return new CastFerociousBiteAction(botAI); }
    static Action* rip(PlayerbotAI* botAI) { return new CastRipAction(botAI); }
    static Action* maim(PlayerbotAI* botAI) { return new CastMaimAction(botAI); }
    static Action* cower(PlayerbotAI* botAI) { return new CastCowerAction(botAI); }
    static Action* survival_instincts(PlayerbotAI* botAI) { return new CastSurvivalInstinctsAction(botAI); }
    static Action* frenzied_regeneration(PlayerbotAI* botAI) { return new CastFrenziedRegenerationAction(botAI); }
    static Action* thorns(PlayerbotAI* botAI) { return new CastThornsAction(botAI); }
    static Action* thorns_on_party(PlayerbotAI* botAI) { return new CastThornsOnPartyAction(botAI); }
    static Action* thorns_on_main_tank(PlayerbotAI* botAI) { return new CastThornsOnMainTankAction(botAI); }
    static Action* lifebloom_on_main_tank(PlayerbotAI* botAI) { return new CastLifebloomOnMainTankAction(botAI); }
    static Action* rejuvenation_on_main_tank(PlayerbotAI* botAI) { return new CastRejuvenationOnMainTankAction(botAI); }
    static Action* cure_poison(PlayerbotAI* botAI) { return new CastCurePoisonAction(botAI); }
    static Action* cure_poison_on_party(PlayerbotAI* botAI) { return new CastCurePoisonOnPartyAction(botAI); }
    static Action* abolish_poison(PlayerbotAI* botAI) { return new CastAbolishPoisonAction(botAI); }
    static Action* abolish_poison_on_party(PlayerbotAI* botAI) { return new CastAbolishPoisonOnPartyAction(botAI); }
    static Action* berserk(PlayerbotAI* botAI) { return new CastBerserkAction(botAI); }
    static Action* tigers_fury(PlayerbotAI* botAI) { return new CastTigersFuryAction(botAI); }
    static Action* savage_roar(PlayerbotAI* botAI) { return new CastSavageRoarAction(botAI); }
    static Action* savage_roar_clip(PlayerbotAI* botAI) { return new CastSavageRoarClipAction(botAI); }  // [b31_sd]
    static Action* mark_of_the_wild(PlayerbotAI* botAI) { return new CastMarkOfTheWildAction(botAI); }
    static Action* mark_of_the_wild_on_party(PlayerbotAI* botAI) { return new CastMarkOfTheWildOnPartyAction(botAI); }
    static Action* regrowth(PlayerbotAI* botAI) { return new CastRegrowthAction(botAI); }
    static Action* rejuvenation(PlayerbotAI* botAI) { return new CastRejuvenationAction(botAI); }
    static Action* healing_touch(PlayerbotAI* botAI) { return new CastHealingTouchAction(botAI); }
    static Action* regrowth_on_party(PlayerbotAI* botAI) { return new CastRegrowthOnPartyAction(botAI); }
    static Action* rejuvenation_on_party(PlayerbotAI* botAI) { return new CastRejuvenationOnPartyAction(botAI); }
    static Action* rejuvenation_on_not_full(PlayerbotAI* botAI) { return new CastRejuvenationOnNotFullAction(botAI); }
    static Action* healing_touch_on_party(PlayerbotAI* botAI) { return new CastHealingTouchOnPartyAction(botAI); }
    static Action* healing_touch_filler_on_party(PlayerbotAI* botAI)  // [b31_sd]
    {
        return new CastHealingTouchFillerOnPartyAction(botAI);
    }
    static Action* rebirth(PlayerbotAI* botAI) { return new CastRebirthAction(botAI); }
    static Action* bear_battle_res(PlayerbotAI* botAI) { return new CastBearBattleResAction(botAI); }  // [b32_brez]
    static Action* revive(PlayerbotAI* botAI) { return new CastReviveAction(botAI); }
    static Action* barkskin(PlayerbotAI* botAI) { return new CastBarkskinAction(botAI); }
    static Action* lacerate(PlayerbotAI* botAI) { return new CastLacerateAction(botAI); }
    static Action* hurricane(PlayerbotAI* botAI) { return new CastHurricaneAction(botAI); }
    static Action* innervate(PlayerbotAI* botAI) { return new CastInnervateAction(botAI); }
    static Action* innervate_on_healer(PlayerbotAI* botAI) { return new CastInnervateOnHealerAction(botAI); }
    static Action* bash_on_enemy_healer(PlayerbotAI* botAI) { return new CastBashOnEnemyHealerAction(botAI); }
    static Action* ravage(PlayerbotAI* botAI) { return new CastRavageAction(botAI); }
    static Action* pounce(PlayerbotAI* botAI) { return new CastPounceAction(botAI); }
    static Action* prowl(PlayerbotAI* botAI) { return new CastProwlAction(botAI); }
    static Action* dash(PlayerbotAI* botAI) { return new CastDashAction(botAI); }
    static Action* shred(PlayerbotAI* botAI) { return new CastShredAction(botAI); }
    static Action* wild_growth_on_party(PlayerbotAI* botAI) { return new CastWildGrowthOnPartyAction(botAI); }
    static Action* swiftmend_on_party(PlayerbotAI* botAI) { return new CastPartySwiftmendAction(botAI); }
    static Action* nourish_on_party(PlayerbotAI* botAI) { return new CastPartyNourishAction(botAI); }
    static Action* remove_curse_on_party(PlayerbotAI* botAI) { return new CastDruidRemoveCurseOnPartyAction(botAI); }
    static Action* insect_swarm_on_attacker(PlayerbotAI* botAI) { return new CastInsectSwarmOnAttackerAction(botAI); }
    static Action* moonfire_on_attacker(PlayerbotAI* botAI) { return new CastMoonfireOnAttackerAction(botAI); }
    static Action* enrage(PlayerbotAI* botAI) { return new CastEnrageAction(botAI); }
    static Action* force_of_nature(PlayerbotAI* botAI) { return new CastForceOfNatureAction(botAI); }
    static Action* typhoon(PlayerbotAI* botAI) { return new CastTyphoonAction(botAI); }
    static Action* rejuvenation_blanket(PlayerbotAI* botAI) { return new CastRejuvenationBlanketAction(botAI); }
    static Action* wild_growth_blanket(PlayerbotAI* botAI) { return new CastWildGrowthBlanketAction(botAI); }
};

SharedNamedObjectContextList<Strategy> DruidAiObjectContext::sharedStrategyContexts;
SharedNamedObjectContextList<Action> DruidAiObjectContext::sharedActionContexts;
SharedNamedObjectContextList<Trigger> DruidAiObjectContext::sharedTriggerContexts;
SharedNamedObjectContextList<UntypedValue> DruidAiObjectContext::sharedValueContexts;

DruidAiObjectContext::DruidAiObjectContext(PlayerbotAI* botAI)
    : AiObjectContext(botAI, sharedStrategyContexts, sharedActionContexts, sharedTriggerContexts, sharedValueContexts)
{
}

void DruidAiObjectContext::BuildSharedContexts()
{
    BuildSharedStrategyContexts(sharedStrategyContexts);
    BuildSharedActionContexts(sharedActionContexts);
    BuildSharedTriggerContexts(sharedTriggerContexts);
    BuildSharedValueContexts(sharedValueContexts);
}

void DruidAiObjectContext::BuildSharedStrategyContexts(SharedNamedObjectContextList<Strategy>& strategyContexts)
{
    AiObjectContext::BuildSharedStrategyContexts(strategyContexts);
    strategyContexts.Add(new DruidStrategyFactoryInternal());
    strategyContexts.Add(new DruidDruidStrategyFactoryInternal());
}

void DruidAiObjectContext::BuildSharedActionContexts(SharedNamedObjectContextList<Action>& actionContexts)
{
    AiObjectContext::BuildSharedActionContexts(actionContexts);
    actionContexts.Add(new DruidAiObjectContextInternal());
}

void DruidAiObjectContext::BuildSharedTriggerContexts(SharedNamedObjectContextList<Trigger>& triggerContexts)
{
    AiObjectContext::BuildSharedTriggerContexts(triggerContexts);
    triggerContexts.Add(new DruidTriggerFactoryInternal());
}

class DruidValueContextInternal : public NamedObjectContext<UntypedValue>
{
public:
    DruidValueContextInternal()
    {
        creators["eclipse lunar proc time"] = &DruidValueContextInternal::eclipse_lunar_proc_time;
        creators["blanket hot target"] = &DruidValueContextInternal::blanket_hot_target;
        creators["rebirth target"] = &DruidValueContextInternal::rebirth_target;
        creators["rebirth time"] = &DruidValueContextInternal::rebirth_time;
        creators["bear battle res target"] = &DruidValueContextInternal::bear_battle_res_target;  // [b32_brez]
    }

private:
    static UntypedValue* eclipse_lunar_proc_time(PlayerbotAI* botAI) { return new EclipseLunarProcTimeValue(botAI); }
    static UntypedValue* blanket_hot_target(PlayerbotAI* botAI) { return new BlanketHotTargetValue(botAI); }
    static UntypedValue* rebirth_target(PlayerbotAI* botAI)
    {
        return new ManualSetValue<ObjectGuid>(botAI, ObjectGuid::Empty, "rebirth target");
    }
    static UntypedValue* bear_battle_res_target(PlayerbotAI* botAI)  // [b32_brez]
    {
        return new BearBattleResTargetValue(botAI);
    }
    static UntypedValue* rebirth_time(PlayerbotAI* botAI)
    {
        return new ManualSetValue<uint32>(botAI, 0, "rebirth time");
    }
};

void DruidAiObjectContext::BuildSharedValueContexts(SharedNamedObjectContextList<UntypedValue>& valueContexts)
{
    AiObjectContext::BuildSharedValueContexts(valueContexts);
    valueContexts.Add(new DruidValueContextInternal());
}
