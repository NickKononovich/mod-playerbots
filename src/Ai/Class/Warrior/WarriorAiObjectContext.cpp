/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WarriorAiObjectContext.h"
#include "ArmsWarriorStrategy.h"
#include "FuryWarriorStrategy.h"
#include "GenericWarriorNonCombatStrategy.h"
#include "NamedObjectContext.h"
#include "Playerbots.h"
#include "TankAoeThreat.h"
#include "TankWarriorStrategy.h"
#include "WarriorActions.h"
#include "WarriorPullStrategy.h"
#include "WarriorTriggers.h"

class WarriorStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    WarriorStrategyFactoryInternal()
    {
        creators["nc"] = &WarriorStrategyFactoryInternal::nc;
        creators["pull"] = &WarriorStrategyFactoryInternal::pull;
        creators["aoe"] = &WarriorStrategyFactoryInternal::warrior_aoe;
    }

private:
    static Strategy* nc(PlayerbotAI* botAI) { return new GenericWarriorNonCombatStrategy(botAI); }
    static Strategy* warrior_aoe(PlayerbotAI* botAI) { return new WarriorAoeStrategy(botAI); }
    static Strategy* pull(PlayerbotAI* botAI) { return new WarriorPullStrategy(botAI); }
};

class WarriorCombatStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    WarriorCombatStrategyFactoryInternal() : NamedObjectContext<Strategy>(false, true)
    {
        creators["prot"] = &WarriorCombatStrategyFactoryInternal::prot;
        creators["tank"] = &WarriorCombatStrategyFactoryInternal::prot;
        creators["arms"] = &WarriorCombatStrategyFactoryInternal::arms;
        creators["fury"] = &WarriorCombatStrategyFactoryInternal::fury;
    }

private:
    static Strategy* prot(PlayerbotAI* botAI) { return new TankWarriorStrategy(botAI); }
    static Strategy* arms(PlayerbotAI* botAI) { return new ArmsWarriorStrategy(botAI); }
    static Strategy* fury(PlayerbotAI* botAI) { return new FuryWarriorStrategy(botAI); }
};

class WarriorTriggerFactoryInternal : public NamedObjectContext<Trigger>
{
public:
    WarriorTriggerFactoryInternal()
    {
        creators["hamstring"] = &WarriorTriggerFactoryInternal::hamstring;
        creators["victory rush"] = &WarriorTriggerFactoryInternal::victory_rush;
        creators["death wish"] = &WarriorTriggerFactoryInternal::death_wish;
        creators["recklessness"] = &WarriorTriggerFactoryInternal::recklessness;
        creators["battle shout"] = &WarriorTriggerFactoryInternal::battle_shout;
        creators["rend"] = &WarriorTriggerFactoryInternal::rend;
        creators["rend on attacker"] = &WarriorTriggerFactoryInternal::rend_on_attacker;
        creators["bloodrage"] = &WarriorTriggerFactoryInternal::bloodrage;
        creators["bloodrage low rage"] = &WarriorTriggerFactoryInternal::bloodrage_low_rage;
        creators["tank aoe threat"] = &WarriorTriggerFactoryInternal::tank_aoe_threat;
        creators["tank aoe pack"] = &WarriorTriggerFactoryInternal::tank_aoe_pack;  // [b32_tank_aoe]
        creators["tank aoe crowd"] = &WarriorTriggerFactoryInternal::tank_aoe_crowd;  // [b32_peel]
        creators["untagged tank mob"] = &WarriorTriggerFactoryInternal::untagged_tank_mob;  // [rot172]
        creators["shield bash"] = &WarriorTriggerFactoryInternal::shield_bash;
        creators["tank bloodrage"] = &WarriorTriggerFactoryInternal::tank_bloodrage;                    // [rot20]
        creators["prot heroic strike rage"] = &WarriorTriggerFactoryInternal::prot_heroic_strike_rage;  // [rot20]
        creators["concussion blow"] = &WarriorTriggerFactoryInternal::concussion_blow;
        creators["sword and board"] = &WarriorTriggerFactoryInternal::SwordAndBoard;
        creators["shield bash on enemy healer"] = &WarriorTriggerFactoryInternal::shield_bash_on_enemy_healer;
        creators["battle stance"] = &WarriorTriggerFactoryInternal::battle_stance;
        creators["defensive stance"] = &WarriorTriggerFactoryInternal::defensive_stance;
        creators["berserker stance"] = &WarriorTriggerFactoryInternal::berserker_stance;
        creators["shield block"] = &WarriorTriggerFactoryInternal::shield_block;
        creators["sunder armor"] = &WarriorTriggerFactoryInternal::sunder_armor;
        creators["revenge"] = &WarriorTriggerFactoryInternal::revenge;
        creators["overpower"] = &WarriorTriggerFactoryInternal::overpower;
        creators["mocking blow"] = &WarriorTriggerFactoryInternal::mocking_blow;
        creators["rampage"] = &WarriorTriggerFactoryInternal::rampage;
        // [b31_deep] "mortal strike" (debuff) replaced by "mortal strike ready" (rage gated)
        creators["mortal strike ready"] = &WarriorTriggerFactoryInternal::mortal_strike_ready;
        creators["heroic throw ready"] = &WarriorTriggerFactoryInternal::heroic_throw_ready;
        creators["arms heroic strike rage"] = &WarriorTriggerFactoryInternal::arms_heroic_strike_rage;
        creators["arms slam rage"] = &WarriorTriggerFactoryInternal::arms_slam_rage;  // [rot290]
        creators["fury heroic strike rage"] = &WarriorTriggerFactoryInternal::fury_heroic_strike_rage;  // [rot19]
        creators["shattering throw boss"] = &WarriorTriggerFactoryInternal::shattering_throw_boss;
        creators["victory rush and medium health"] = &WarriorTriggerFactoryInternal::victory_rush_and_medium_health;
        creators["demoralizing shout"] = &WarriorTriggerFactoryInternal::demoralizing_shout;
        creators["medium aoe bladestorm"] = &WarriorTriggerFactoryInternal::medium_aoe_bladestorm;
        creators["light aoe shockwave"] = &WarriorTriggerFactoryInternal::light_aoe_shockwave;
        creators["thunder clap on snare target"] = &WarriorTriggerFactoryInternal::thunder_clap_on_snare_target;
        creators["thunder clap"] = &WarriorTriggerFactoryInternal::thunder_clap;
        // [b31_wr] cooldown-ready triggers (were buff triggers named "bloodthirst" / "whirlwind")
        creators["bloodthirst ready"] = &WarriorTriggerFactoryInternal::bloodthirst_ready;
        creators["whirlwind ready"] = &WarriorTriggerFactoryInternal::whirlwind_ready;
        creators["shield slam ready"] = &WarriorTriggerFactoryInternal::shield_slam_ready;
        // [b31_wr] stance-aware AoE and the rage-gated Cleave
        creators["light aoe berserker stance"] = &WarriorTriggerFactoryInternal::light_aoe_berserker_stance;
        creators["light aoe thunder clap stance"] = &WarriorTriggerFactoryInternal::light_aoe_thunder_clap_stance;
        creators["light aoe sweeping strikes stance"] =
            &WarriorTriggerFactoryInternal::light_aoe_sweeping_strikes_stance;
        creators["light aoe and medium rage"] = &WarriorTriggerFactoryInternal::light_aoe_and_medium_rage;
        creators["battle shout no commanding shout"] =
            &WarriorTriggerFactoryInternal::battle_shout_no_commanding_shout;
        creators["berserker rage"] = &WarriorTriggerFactoryInternal::berserker_rage;
        creators["pummel on enemy healer"] = &WarriorTriggerFactoryInternal::pummel_on_enemy_healer;
        creators["pummel"] = &WarriorTriggerFactoryInternal::pummel;
        creators["pummel ready"] = &WarriorTriggerFactoryInternal::pummel_ready;  // [melee_audit_b30]
        creators["intercept on enemy healer"] = &WarriorTriggerFactoryInternal::intercept_on_enemy_healer;
        creators["intercept"] = &WarriorTriggerFactoryInternal::intercept;
        creators["taunt on snare target"] = &WarriorTriggerFactoryInternal::taunt_on_snare_target;
        creators["commanding shout"] = &WarriorTriggerFactoryInternal::commanding_shout;
        creators["intercept on snare target"] = &WarriorTriggerFactoryInternal::intercept_on_snare_target;
        creators["spell reflection"] = &WarriorTriggerFactoryInternal::spell_reflection;
        creators["sudden death"] = &WarriorTriggerFactoryInternal::sudden_death;
        creators["instant slam"] = &WarriorTriggerFactoryInternal::instant_slam;
        creators["shockwave"] = &WarriorTriggerFactoryInternal::shockwave;
        creators["shockwave on snare target"] = &WarriorTriggerFactoryInternal::shockwave_on_snare_target;
        creators["taste for blood"] = &WarriorTriggerFactoryInternal::taste_for_blood;
        creators["thunder clap and rage"] = &WarriorTriggerFactoryInternal::thunderclap_and_rage;
        // [b31_deep] "intercept can cast" / "intercept and far enemy" / "intercept and rage" removed (unused, dead)
        // creators["slam"] = &WarriorTriggerFactoryInternal::slam;
        creators["vigilance"] = &WarriorTriggerFactoryInternal::vigilance;
        creators["shattering throw trigger"] = &WarriorTriggerFactoryInternal::shattering_throw_trigger;
        creators["bladestorm"] = &WarriorTriggerFactoryInternal::bladestorm;
        creators["recklessness ready"] = &WarriorTriggerFactoryInternal::recklessness_ready;
        creators["execute phase and high rage"] = &WarriorTriggerFactoryInternal::execute_phase_and_high_rage;
        creators["critical health solo"] = &WarriorTriggerFactoryInternal::critical_health_solo;
    }

private:
    static Trigger* bladestorm(PlayerbotAI* botAI) { return new BladestormBoostTrigger(botAI); }
    static Trigger* recklessness_ready(PlayerbotAI* botAI) { return new RecklessnessReadyTrigger(botAI); }
    static Trigger* execute_phase_and_high_rage(PlayerbotAI* botAI)
    {
        return new TwoTriggers(botAI, "target critical health", "high rage available");
    }
    static Trigger* critical_health_solo(PlayerbotAI* botAI) { return new CriticalHealthSoloTrigger(botAI); }
    static Trigger* shield_block(PlayerbotAI* botAI) { return new ShieldBlockTrigger(botAI); }
    static Trigger* defensive_stance(PlayerbotAI* botAI) { return new DefensiveStanceTrigger(botAI); }
    static Trigger* berserker_stance(PlayerbotAI* botAI) { return new BerserkerStanceTrigger(botAI); }
    static Trigger* battle_stance(PlayerbotAI* botAI) { return new BattleStanceTrigger(botAI); }
    static Trigger* hamstring(PlayerbotAI* botAI) { return new HamstringTrigger(botAI); }
    static Trigger* victory_rush(PlayerbotAI* botAI) { return new VictoryRushTrigger(botAI); }
    static Trigger* death_wish(PlayerbotAI* botAI) { return new DeathWishTrigger(botAI); }
    static Trigger* recklessness(PlayerbotAI* botAI) { return new RecklessnessTrigger(botAI); }
    static Trigger* battle_shout(PlayerbotAI* botAI) { return new BattleShoutTrigger(botAI); }
    static Trigger* rend(PlayerbotAI* botAI) { return new RendDebuffTrigger(botAI); }
    static Trigger* rend_on_attacker(PlayerbotAI* botAI) { return new RendDebuffOnAttackerTrigger(botAI); }
    static Trigger* bloodrage(PlayerbotAI* botAI) { return new BloodrageBuffTrigger(botAI); }
    static Trigger* bloodrage_low_rage(PlayerbotAI* botAI) { return new BloodrageLowRageTrigger(botAI); }
    // Thunder Clap reaches 8 yd around the warrior
    static Trigger* tank_aoe_threat(PlayerbotAI* botAI) { return new TankAoeThreatTrigger(botAI, 8.0f); }
    static Trigger* tank_aoe_pack(PlayerbotAI* botAI) { return new TankAoePackTrigger(botAI, 8.0f); }  // [b32_tank_aoe]
    static Trigger* tank_aoe_crowd(PlayerbotAI* botAI) { return new TankAoeCrowdTrigger(botAI, 8.0f); }  // [b32_peel]
    static Trigger* untagged_tank_mob(PlayerbotAI* botAI) { return new UntaggedTankMobTrigger(botAI); }  // [rot172]
    static Trigger* shield_bash(PlayerbotAI* botAI) { return new ShieldBashInterruptSpellTrigger(botAI); }
    static Trigger* tank_bloodrage(PlayerbotAI* botAI) { return new BloodrageTankTrigger(botAI); }
    // wowsims protection_warrior__default: Heroic Strike at 30+ rage. [rot36] keeping the rage of a Shield Slam due
    // within a GCD (WarriorLeavesRageForStrike)
    static Trigger* prot_heroic_strike_rage(PlayerbotAI* botAI)
    {
        return new FuryHeroicStrikeRageTrigger(botAI, "prot heroic strike rage", 30);
    }
    static Trigger* concussion_blow(PlayerbotAI* botAI) { return new ConcussionBlowTrigger(botAI); }
    static Trigger* SwordAndBoard(PlayerbotAI* botAI) { return new SwordAndBoardTrigger(botAI); }
    static Trigger* shield_bash_on_enemy_healer(PlayerbotAI* botAI)
    {
        return new ShieldBashInterruptEnemyHealerSpellTrigger(botAI);
    }
    static Trigger* thunderclap_and_rage(PlayerbotAI* botAI)
    {
        return new TwoTriggers(botAI, "thunder clap", "light rage available");
    }
    static Trigger* intercept_on_snare_target(PlayerbotAI* botAI) { return new InterceptSnareTrigger(botAI); }
    static Trigger* spell_reflection(PlayerbotAI* botAI) { return new SpellReflectionTrigger(botAI); }
    static Trigger* taste_for_blood(PlayerbotAI* botAI) { return new TasteForBloodTrigger(botAI); }
    static Trigger* shockwave_on_snare_target(PlayerbotAI* botAI) { return new ShockwaveSnareTrigger(botAI); }
    static Trigger* shockwave(PlayerbotAI* botAI) { return new ShockwaveTrigger(botAI); }
    static Trigger* instant_slam(PlayerbotAI* botAI) { return new SlamInstantTrigger(botAI); }
    static Trigger* sudden_death(PlayerbotAI* botAI) { return new SuddenDeathTrigger(botAI); }
    static Trigger* commanding_shout(PlayerbotAI* botAI) { return new CommandingShoutTrigger(botAI); }
    static Trigger* taunt_on_snare_target(PlayerbotAI* botAI) { return new TauntSnareTrigger(botAI); }
    static Trigger* intercept(PlayerbotAI* botAI) { return new InterceptInterruptSpellTrigger(botAI); }
    static Trigger* intercept_on_enemy_healer(PlayerbotAI* botAI)
    {
        return new InterceptInterruptEnemyHealerSpellTrigger(botAI);
    }
    static Trigger* pummel(PlayerbotAI* botAI) { return new PummelInterruptSpellTrigger(botAI); }
    static Trigger* pummel_ready(PlayerbotAI* botAI) { return new PummelReadyInterruptSpellTrigger(botAI); }
    static Trigger* pummel_on_enemy_healer(PlayerbotAI* botAI)
    {
        return new PummelInterruptEnemyHealerSpellTrigger(botAI);
    }
    static Trigger* berserker_rage(PlayerbotAI* botAI) { return new BerserkerRageBuffTrigger(botAI); }
    // [b31_wr]
    static Trigger* bloodthirst_ready(PlayerbotAI* botAI) { return new WarriorSpellReadyTrigger(botAI, "bloodthirst"); }
    static Trigger* whirlwind_ready(PlayerbotAI* botAI) { return new WarriorSpellReadyTrigger(botAI, "whirlwind"); }
    static Trigger* shield_slam_ready(PlayerbotAI* botAI) { return new WarriorSpellReadyTrigger(botAI, "shield slam"); }
    static Trigger* light_aoe_berserker_stance(PlayerbotAI* botAI)
    {
        return new WarriorStanceAoeTrigger(botAI, "light aoe berserker stance", WarriorStanceAoeTrigger::BERSERKER,
                                           "whirlwind");  // [b31_deep] spell known
    }
    static Trigger* light_aoe_thunder_clap_stance(PlayerbotAI* botAI)
    {
        return new WarriorStanceAoeTrigger(botAI, "light aoe thunder clap stance",
                                           WarriorStanceAoeTrigger::BATTLE | WarriorStanceAoeTrigger::DEFENSIVE,
                                           "thunder clap");  // [b31_deep]
    }
    static Trigger* light_aoe_sweeping_strikes_stance(PlayerbotAI* botAI)
    {
        return new WarriorStanceAoeTrigger(botAI, "light aoe sweeping strikes stance",
                                           WarriorStanceAoeTrigger::BATTLE | WarriorStanceAoeTrigger::BERSERKER,
                                           "sweeping strikes");  // [b31_deep]
    }
    static Trigger* light_aoe_and_medium_rage(PlayerbotAI* botAI)
    {
        return new TwoTriggers(botAI, "light aoe", "medium rage available");
    }
    static Trigger* battle_shout_no_commanding_shout(PlayerbotAI* botAI)
    {
        return new BattleShoutNoCommandingShoutTrigger(botAI);
    }
    static Trigger* thunder_clap_on_snare_target(PlayerbotAI* botAI) { return new ThunderClapSnareTrigger(botAI); }
    static Trigger* thunder_clap(PlayerbotAI* botAI) { return new ThunderClapTrigger(botAI); }
    // [b31_deep]
    static Trigger* mortal_strike_ready(PlayerbotAI* botAI)
    {
        return new WarriorSpellReadyTrigger(botAI, "mortal strike", true);
    }
    static Trigger* heroic_throw_ready(PlayerbotAI* botAI)
    {
        return new WarriorSpellReadyTrigger(botAI, "heroic throw");
    }
    // [rot29] wowsims arms: Heroic Strike at 40 rage, keeping the rage of a Mortal Strike due within a GCD (was a flat
    // 45). [rot63] and only at the swing (ArmsHeroicStrikeTrigger)
    static Trigger* arms_heroic_strike_rage(PlayerbotAI* botAI) { return new ArmsHeroicStrikeTrigger(botAI); }
    static Trigger* arms_slam_rage(PlayerbotAI* botAI) { return new ArmsSlamRageTrigger(botAI); }  // [rot290]
    static Trigger* fury_heroic_strike_rage(PlayerbotAI* botAI) { return new FuryHeroicStrikeRageTrigger(botAI); }
    static Trigger* shattering_throw_boss(PlayerbotAI* botAI) { return new ShatteringThrowBossTrigger(botAI); }
    static Trigger* victory_rush_and_medium_health(PlayerbotAI* botAI)
    {
        return new TwoTriggers(botAI, "victory rush", "medium health");
    }
    static Trigger* demoralizing_shout(PlayerbotAI* botAI) { return new DemoralizingShoutDebuffTrigger(botAI); }
    static Trigger* medium_aoe_bladestorm(PlayerbotAI* botAI)
    {
        return new WarriorStanceAoeTrigger(botAI, "medium aoe bladestorm", WarriorStanceAoeTrigger::ANY, "bladestorm",
                                           3);
    }
    static Trigger* light_aoe_shockwave(PlayerbotAI* botAI)
    {
        return new WarriorStanceAoeTrigger(botAI, "light aoe shockwave", WarriorStanceAoeTrigger::ANY, "shockwave");
    }
    static Trigger* rampage(PlayerbotAI* botAI) { return new RampageAvailableTrigger(botAI); }
    static Trigger* mocking_blow(PlayerbotAI* botAI) { return new MockingBlowTrigger(botAI); }
    static Trigger* overpower(PlayerbotAI* botAI) { return new OverpowerAvailableTrigger(botAI); }
    static Trigger* revenge(PlayerbotAI* botAI) { return new RevengeAvailableTrigger(botAI); }
    static Trigger* sunder_armor(PlayerbotAI* botAI) { return new SunderArmorDebuffTrigger(botAI); }
    // static Trigger* slam(PlayerbotAI* botAI) { return new SlamTrigger(botAI); }
    static Trigger* vigilance(PlayerbotAI* botAI) { return new VigilanceTrigger(botAI); }
    static Trigger* shattering_throw_trigger(PlayerbotAI* botAI) { return new ShatteringThrowTrigger(botAI); }
};

class WarriorAiObjectContextInternal : public NamedObjectContext<Action>
{
public:
    WarriorAiObjectContextInternal()
    {
        creators["devastate"] = &WarriorAiObjectContextInternal::devastate;
        creators["overpower"] = &WarriorAiObjectContextInternal::overpower;
        creators["charge"] = &WarriorAiObjectContextInternal::charge;
        creators["bloodthirst"] = &WarriorAiObjectContextInternal::bloodthirst;
        creators["rend"] = &WarriorAiObjectContextInternal::rend;
        creators["rend on attacker"] = &WarriorAiObjectContextInternal::rend_on_attacker;
        creators["mocking blow"] = &WarriorAiObjectContextInternal::mocking_blow;
        creators["death wish"] = &WarriorAiObjectContextInternal::death_wish;
        creators["recklessness"] = &WarriorAiObjectContextInternal::recklessness;
        creators["berserker rage"] = &WarriorAiObjectContextInternal::berserker_rage;
        creators["victory rush"] = &WarriorAiObjectContextInternal::victory_rush;
        creators["execute"] = &WarriorAiObjectContextInternal::execute;
        creators["defensive stance"] = &WarriorAiObjectContextInternal::defensive_stance;
        creators["hamstring"] = &WarriorAiObjectContextInternal::hamstring;
        creators["shield bash"] = &WarriorAiObjectContextInternal::shield_bash;
        creators["shield block"] = &WarriorAiObjectContextInternal::shield_block;
        creators["bloodrage"] = &WarriorAiObjectContextInternal::bloodrage;
        creators["battle stance"] = &WarriorAiObjectContextInternal::battle_stance;
        creators["heroic strike"] = &WarriorAiObjectContextInternal::heroic_strike;
        creators["intimidating shout"] = &WarriorAiObjectContextInternal::intimidating_shout;
        creators["demoralizing shout"] = &WarriorAiObjectContextInternal::demoralizing_shout;
        creators["demoralizing shout without life time check"] =
            &WarriorAiObjectContextInternal::demoralizing_shout_without_life_time_check;
        creators["challenging shout"] = &WarriorAiObjectContextInternal::challenging_shout;
        creators["shield wall"] = &WarriorAiObjectContextInternal::shield_wall;
        creators["battle shout"] = &WarriorAiObjectContextInternal::battle_shout;
        creators["battle shout taunt"] = &WarriorAiObjectContextInternal::battle_shout_taunt;
        creators["thunder clap"] = &WarriorAiObjectContextInternal::thunder_clap;
        creators["thunder clap on group member attackers"] =
            &WarriorAiObjectContextInternal::thunder_clap_on_group_member_attackers;
        creators["thunder clap tank aoe"] = &WarriorAiObjectContextInternal::thunder_clap_tank_aoe;
        creators["demoralizing shout tank aoe"] = &WarriorAiObjectContextInternal::demoralizing_shout_tank_aoe;
        creators["taunt"] = &WarriorAiObjectContextInternal::taunt;
        creators["taunt on group member attacker"] = &WarriorAiObjectContextInternal::taunt_on_group_member_attacker;
        creators["mocking blow on group member attacker"] =
            &WarriorAiObjectContextInternal::mocking_blow_on_group_member_attacker;
        creators["heroic throw on group member attacker"] =
            &WarriorAiObjectContextInternal::heroic_throw_on_group_member_attacker;
        // [rot172]
        creators["heroic throw on untagged mob"] =
            &WarriorAiObjectContextInternal::heroic_throw_on_untagged_mob;
        creators["shield slam on group member attacker"] =
            &WarriorAiObjectContextInternal::shield_slam_on_group_member_attacker;
        creators["revenge"] = &WarriorAiObjectContextInternal::revenge;
        creators["slam"] = &WarriorAiObjectContextInternal::slam;
        creators["shield slam"] = &WarriorAiObjectContextInternal::shield_slam;
        creators["disarm"] = &WarriorAiObjectContextInternal::disarm;
        creators["sunder armor"] = &WarriorAiObjectContextInternal::sunder_armor;
        creators["last stand"] = &WarriorAiObjectContextInternal::last_stand;
        creators["shockwave on snare target"] = &WarriorAiObjectContextInternal::shockwave_on_snare_target;
        creators["shockwave"] = &WarriorAiObjectContextInternal::shockwave;
        creators["cleave"] = &WarriorAiObjectContextInternal::cleave;
        creators["concussion blow"] = &WarriorAiObjectContextInternal::concussion_blow;
        creators["shield bash on enemy healer"] = &WarriorAiObjectContextInternal::shield_bash_on_enemy_healer;
        creators["berserker stance"] = &WarriorAiObjectContextInternal::berserker_stance;
        creators["commanding shout"] = &WarriorAiObjectContextInternal::commanding_shout;
        creators["retaliation"] = &WarriorAiObjectContextInternal::retaliation;
        creators["mortal strike"] = &WarriorAiObjectContextInternal::mortal_strike;
        creators["sweeping strikes"] = &WarriorAiObjectContextInternal::sweeping_strikes;
        creators["intercept"] = &WarriorAiObjectContextInternal::intercept;
        creators["whirlwind"] = &WarriorAiObjectContextInternal::whirlwind;
        creators["pummel"] = &WarriorAiObjectContextInternal::pummel;
        creators["pummel on enemy healer"] = &WarriorAiObjectContextInternal::pummel_on_enemy_healer;
        creators["recklessness"] = &WarriorAiObjectContextInternal::recklessness;
        creators["piercing howl"] = &WarriorAiObjectContextInternal::piercing_howl;
        creators["rampage"] = &WarriorAiObjectContextInternal::rampage;
        creators["intervene"] = &WarriorAiObjectContextInternal::intervene;
        creators["intervene on attacked group member"] =
            &WarriorAiObjectContextInternal::intervene_on_attacked_group_member;
        creators["spell reflection"] = &WarriorAiObjectContextInternal::spell_reflection;
        creators["thunder clap on snare target"] = &WarriorAiObjectContextInternal::thunder_clap_on_snare_target;
        creators["taunt on snare target"] = &WarriorAiObjectContextInternal::taunt_on_snare_target;
        creators["intercept on enemy healer"] = &WarriorAiObjectContextInternal::intercept_on_enemy_healer;
        creators["intercept on snare target"] = &WarriorAiObjectContextInternal::intercept_on_snare_target;
        creators["bladestorm"] = &WarriorAiObjectContextInternal::bladestorm;
        creators["heroic throw"] = &WarriorAiObjectContextInternal::heroic_throw;
        creators["heroic throw on snare target"] = &WarriorAiObjectContextInternal::heroic_throw_on_snare_target;
        creators["shattering throw"] = &WarriorAiObjectContextInternal::shattering_throw;
        creators["vigilance"] = &WarriorAiObjectContextInternal::vigilance;
        creators["enraged regeneration"] = &WarriorAiObjectContextInternal::enraged_regeneration;
        // [b31_deep]
        creators["slam filler"] = &WarriorAiObjectContextInternal::slam_filler;
        creators["berserker rage for enrage"] = &WarriorAiObjectContextInternal::berserker_rage_for_enrage;
        creators["bloodrage for enrage"] = &WarriorAiObjectContextInternal::bloodrage_for_enrage;
        creators["shattering throw boss"] = &WarriorAiObjectContextInternal::shattering_throw_boss;
    }

private:
    static Action* devastate(PlayerbotAI* botAI) { return new CastDevastateAction(botAI); }
    static Action* last_stand(PlayerbotAI* botAI) { return new CastLastStandAction(botAI); }
    static Action* shockwave(PlayerbotAI* botAI) { return new CastShockwaveAction(botAI); }
    static Action* shockwave_on_snare_target(PlayerbotAI* botAI) { return new CastShockwaveSnareAction(botAI); }
    static Action* cleave(PlayerbotAI* botAI) { return new CastCleaveAction(botAI); }
    static Action* concussion_blow(PlayerbotAI* botAI) { return new CastConcussionBlowAction(botAI); }
    static Action* taunt(PlayerbotAI* botAI) { return new CastTauntAction(botAI); }
    static Action* taunt_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "taunt");
    }
    static Action* mocking_blow_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "mocking blow");
    }
    static Action* heroic_throw_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "heroic throw");
    }
    static Action* heroic_throw_on_untagged_mob(PlayerbotAI* botAI)  // [rot172]
    {
        return new CastSpellOnUntaggedTankMobAction(botAI, "heroic throw");
    }
    static Action* shield_slam_on_group_member_attacker(PlayerbotAI* botAI)
    {
        return new CastSpellOnGroupMemberAttackerAction(botAI, "shield slam");
    }
    static Action* revenge(PlayerbotAI* botAI) { return new CastRevengeAction(botAI); }
    static Action* slam(PlayerbotAI* botAI) { return new CastSlamAction(botAI); }
    static Action* shield_slam(PlayerbotAI* botAI) { return new CastShieldSlamAction(botAI); }
    static Action* disarm(PlayerbotAI* botAI) { return new CastDisarmAction(botAI); }
    static Action* sunder_armor(PlayerbotAI* botAI) { return new CastSunderArmorAction(botAI); }
    static Action* overpower(PlayerbotAI* botAI) { return new CastOverpowerAction(botAI); }
    static Action* charge(PlayerbotAI* botAI) { return new CastChargeAction(botAI); }
    static Action* bloodthirst(PlayerbotAI* botAI) { return new CastBloodthirstAction(botAI); }
    static Action* rend(PlayerbotAI* botAI) { return new CastRendAction(botAI); }
    static Action* rend_on_attacker(PlayerbotAI* botAI) { return new CastRendOnAttackerAction(botAI); }
    static Action* mocking_blow(PlayerbotAI* botAI) { return new CastMockingBlowAction(botAI); }
    static Action* death_wish(PlayerbotAI* botAI) { return new CastDeathWishAction(botAI); }
    static Action* recklessness(PlayerbotAI* botAI) { return new CastRecklessnessAction(botAI); }
    static Action* berserker_rage(PlayerbotAI* botAI) { return new CastBerserkerRageAction(botAI); }
    static Action* victory_rush(PlayerbotAI* botAI) { return new CastVictoryRushAction(botAI); }
    static Action* execute(PlayerbotAI* botAI) { return new CastExecuteAction(botAI); }
    static Action* defensive_stance(PlayerbotAI* botAI) { return new CastDefensiveStanceAction(botAI); }
    static Action* hamstring(PlayerbotAI* botAI) { return new CastHamstringAction(botAI); }
    static Action* shield_bash(PlayerbotAI* botAI) { return new CastShieldBashAction(botAI); }
    static Action* shield_block(PlayerbotAI* botAI) { return new CastShieldBlockAction(botAI); }
    static Action* bloodrage(PlayerbotAI* botAI) { return new CastBloodrageAction(botAI); }
    static Action* battle_stance(PlayerbotAI* botAI) { return new CastBattleStanceAction(botAI); }
    static Action* heroic_strike(PlayerbotAI* botAI) { return new CastHeroicStrikeAction(botAI); }
    static Action* intimidating_shout(PlayerbotAI* botAI) { return new CastIntimidatingShoutAction(botAI); }
    static Action* demoralizing_shout(PlayerbotAI* botAI) { return new CastDemoralizingShoutAction(botAI); }
    static Action* demoralizing_shout_without_life_time_check(PlayerbotAI* botAI)
    {
        return new CastDemoralizingShoutWithoutLifeTimeCheckAction(botAI);
    }
    static Action* challenging_shout(PlayerbotAI* botAI) { return new CastChallengingShoutAction(botAI); }
    static Action* shield_wall(PlayerbotAI* botAI) { return new CastShieldWallAction(botAI); }
    static Action* battle_shout(PlayerbotAI* botAI) { return new CastBattleShoutAction(botAI); }
    static Action* battle_shout_taunt(PlayerbotAI* botAI) { return new CastBattleShoutTauntAction(botAI); }
    static Action* thunder_clap(PlayerbotAI* botAI) { return new CastThunderClapAction(botAI); }
    static Action* thunder_clap_on_group_member_attackers(PlayerbotAI* botAI)
    {
        return new CastAreaSpellOnGroupMemberAttackersAction(botAI, "thunder clap", 8.0f);
    }
    static Action* thunder_clap_tank_aoe(PlayerbotAI* botAI)
    {
        return new CastTankAreaThreatAction(botAI, "thunder clap");
    }
    static Action* demoralizing_shout_tank_aoe(PlayerbotAI* botAI)
    {
        return new CastDemoralizingShoutTankAoeAction(botAI);
    }
    static Action* shield_bash_on_enemy_healer(PlayerbotAI* botAI)
    {
        return new CastShieldBashOnEnemyHealerAction(botAI);
    }
    static Action* intercept_on_snare_target(PlayerbotAI* botAI) { return new CastInterceptOnSnareTargetAction(botAI); }
    static Action* intercept_on_enemy_healer(PlayerbotAI* botAI) { return new CastInterceptOnEnemyHealerAction(botAI); }
    static Action* taunt_on_snare_target(PlayerbotAI* botAI) { return new CastTauntOnSnareTargetAction(botAI); }
    static Action* thunder_clap_on_snare_target(PlayerbotAI* botAI) { return new CastThunderClapSnareAction(botAI); }
    static Action* berserker_stance(PlayerbotAI* botAI) { return new CastBerserkerStanceAction(botAI); }
    static Action* commanding_shout(PlayerbotAI* botAI) { return new CastCommandingShoutAction(botAI); }
    static Action* retaliation(PlayerbotAI* botAI) { return new CastRetaliationAction(botAI); }
    static Action* mortal_strike(PlayerbotAI* botAI) { return new CastMortalStrikeAction(botAI); }
    static Action* sweeping_strikes(PlayerbotAI* botAI) { return new CastSweepingStrikesAction(botAI); }
    static Action* intercept(PlayerbotAI* botAI) { return new CastInterceptAction(botAI); }
    static Action* whirlwind(PlayerbotAI* botAI) { return new CastWhirlwindAction(botAI); }
    static Action* pummel(PlayerbotAI* botAI) { return new CastPummelAction(botAI); }
    static Action* pummel_on_enemy_healer(PlayerbotAI* botAI) { return new CastPummelOnEnemyHealerAction(botAI); }
    static Action* piercing_howl(PlayerbotAI* botAI) { return new CastPiercingHowlAction(botAI); }
    static Action* rampage(PlayerbotAI* botAI) { return new CastRampageAction(botAI); }
    static Action* intervene(PlayerbotAI* botAI) { return new CastInterveneAction(botAI); }
    static Action* intervene_on_attacked_group_member(PlayerbotAI* botAI)
    {
        return new CastInterveneOnAttackedGroupMemberAction(botAI);
    }
    static Action* spell_reflection(PlayerbotAI* botAI) { return new CastSpellReflectionAction(botAI); }
    static Action* shattering_throw(PlayerbotAI* botAI) { return new CastShatteringThrowAction(botAI); }
    static Action* heroic_throw_on_snare_target(PlayerbotAI* botAI) { return new CastHeroicThrowSnareAction(botAI); }
    static Action* heroic_throw(PlayerbotAI* botAI) { return new CastHeroicThrowAction(botAI); }
    static Action* bladestorm(PlayerbotAI* botAI) { return new CastBladestormAction(botAI); }
    static Action* vigilance(PlayerbotAI* botAI) { return new CastVigilanceAction(botAI); }
    static Action* enraged_regeneration(PlayerbotAI* botAI) { return new CastEnragedRegenerationAction(botAI); }
    // [b31_deep]
    static Action* slam_filler(PlayerbotAI* botAI) { return new CastSlamFillerAction(botAI); }
    static Action* berserker_rage_for_enrage(PlayerbotAI* botAI) { return new CastBerserkerRageForEnrageAction(botAI); }
    static Action* bloodrage_for_enrage(PlayerbotAI* botAI) { return new CastBloodrageForEnrageAction(botAI); }
    static Action* shattering_throw_boss(PlayerbotAI* botAI) { return new CastShatteringThrowBossAction(botAI); }
};

SharedNamedObjectContextList<Strategy> WarriorAiObjectContext::sharedStrategyContexts;
SharedNamedObjectContextList<Action> WarriorAiObjectContext::sharedActionContexts;
SharedNamedObjectContextList<Trigger> WarriorAiObjectContext::sharedTriggerContexts;
SharedNamedObjectContextList<UntypedValue> WarriorAiObjectContext::sharedValueContexts;

WarriorAiObjectContext::WarriorAiObjectContext(PlayerbotAI* botAI)
    : AiObjectContext(botAI, sharedStrategyContexts, sharedActionContexts, sharedTriggerContexts, sharedValueContexts)
{
}

void WarriorAiObjectContext::BuildSharedContexts()
{
    BuildSharedStrategyContexts(sharedStrategyContexts);
    BuildSharedActionContexts(sharedActionContexts);
    BuildSharedTriggerContexts(sharedTriggerContexts);
    BuildSharedValueContexts(sharedValueContexts);
}

void WarriorAiObjectContext::BuildSharedStrategyContexts(SharedNamedObjectContextList<Strategy>& strategyContexts)
{
    AiObjectContext::BuildSharedStrategyContexts(strategyContexts);
    strategyContexts.Add(new WarriorStrategyFactoryInternal());
    strategyContexts.Add(new WarriorCombatStrategyFactoryInternal());
}

void WarriorAiObjectContext::BuildSharedActionContexts(SharedNamedObjectContextList<Action>& actionContexts)
{
    AiObjectContext::BuildSharedActionContexts(actionContexts);
    actionContexts.Add(new WarriorAiObjectContextInternal());
}

void WarriorAiObjectContext::BuildSharedTriggerContexts(SharedNamedObjectContextList<Trigger>& triggerContexts)
{
    AiObjectContext::BuildSharedTriggerContexts(triggerContexts);
    triggerContexts.Add(new WarriorTriggerFactoryInternal());
}

void WarriorAiObjectContext::BuildSharedValueContexts(SharedNamedObjectContextList<UntypedValue>& valueContexts)
{
    AiObjectContext::BuildSharedValueContexts(valueContexts);
}
