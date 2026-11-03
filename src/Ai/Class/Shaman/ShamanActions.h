/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SHAMANACTIONS_H
#define PLAYERBOTS_SHAMANACTIONS_H

#include "GenericSpellActions.h"
#include "HealerSupport.h"
#include "Playerbots.h"
#include "ShamanTriggers.h"
#include "SharedDefines.h"
#include "TotemsShamanStrategy.h"

class PlayerbotAI;

// Buff and Out of Combat Actions

class CastWaterShieldAction : public CastBuffSpellAction
{
public:
    CastWaterShieldAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "water shield") {}
};

class CastLightningShieldAction : public CastBuffSpellAction
{
public:
    CastLightningShieldAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "lightning shield") {}
};

class CastRockbiterWeaponMainHandAction : public CastEnchantItemMainHandAction
{
public:
    CastRockbiterWeaponMainHandAction(PlayerbotAI* botAI) :
        CastEnchantItemMainHandAction(botAI, "rockbiter weapon") {}
};

class CastFlametongueWeaponMainHandAction : public CastEnchantItemMainHandAction
{
public:
    CastFlametongueWeaponMainHandAction(PlayerbotAI* botAI) :
        CastEnchantItemMainHandAction(botAI, "flametongue weapon") {}
};

class CastFlametongueWeaponOffHandAction : public CastEnchantItemOffHandAction
{
public:
    CastFlametongueWeaponOffHandAction(PlayerbotAI* botAI) :
        CastEnchantItemOffHandAction(botAI, "flametongue weapon") {}
};

/* class CastFrostbrandWeaponOffHandAction : public CastEnchantItemOffHandAction
{
public:
    CastFrostbrandWeaponOffHandAction(PlayerbotAI* botAI) :
        CastEnchantItemOffHandAction(botAI, "frostbrand weapon") {}
}; */

class CastEarthlivingWeaponMainHandAction : public CastEnchantItemMainHandAction
{
public:
    CastEarthlivingWeaponMainHandAction(PlayerbotAI* botAI) :
        CastEnchantItemMainHandAction(botAI, "earthliving weapon") {}
};

class CastWindfuryWeaponMainHandAction : public CastEnchantItemMainHandAction
{
public:
    CastWindfuryWeaponMainHandAction(PlayerbotAI* botAI) :
        CastEnchantItemMainHandAction(botAI, "windfury weapon") {}
};

class CastAncestralSpiritAction : public ResurrectPartyMemberAction
{
public:
    CastAncestralSpiritAction(PlayerbotAI* botAI) :
        ResurrectPartyMemberAction(botAI, "ancestral spirit") {}
};

class CastWaterBreathingAction : public CastBuffSpellAction
{
public:
    CastWaterBreathingAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "water breathing") {}
};

class CastWaterWalkingAction : public CastBuffSpellAction
{
public:
    CastWaterWalkingAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "water walking") {}
};

class CastWaterBreathingOnPartyAction : public BuffOnPartyAction
{
public:
    CastWaterBreathingOnPartyAction(PlayerbotAI* botAI) :
        BuffOnPartyAction(botAI, "water breathing") {}
};

class CastWaterWalkingOnPartyAction : public BuffOnPartyAction
{
public:
    CastWaterWalkingOnPartyAction(PlayerbotAI* botAI) :
        BuffOnPartyAction(botAI, "water walking") {}
};

// Boost Actions

class CastHeroismAction : public CastBuffSpellAction
{
public:
    CastHeroismAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "heroism") {}
};

class CastBloodlustAction : public CastBuffSpellAction
{
public:
    CastBloodlustAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "bloodlust") {}
};

class CastElementalMasteryAction : public CastBuffSpellAction
{
public:
    CastElementalMasteryAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "elemental mastery") {}
};

class CastShamanisticRageAction : public CastBuffSpellAction
{
public:
    CastShamanisticRageAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "shamanistic rage") {}
};

class CastFeralSpiritAction : public CastSpellAction
{
public:
    CastFeralSpiritAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "feral spirit") {}
};

class CastSpiritWalkAction : public Action
{
public:
    CastSpiritWalkAction(PlayerbotAI* botAI) : Action(botAI, "spirit walk") {}

    bool Execute(Event event) override;
};

// CC, Interrupt, and Dispel Actions

class CastWindShearAction : public CastSpellAction
{
public:
    CastWindShearAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "wind shear") {}
};

class CastWindShearOnEnemyHealerAction : public CastSpellOnEnemyHealerAction
{
public:
    CastWindShearOnEnemyHealerAction(PlayerbotAI* botAI) :
        CastSpellOnEnemyHealerAction(botAI, "wind shear") {}
};

class CastPurgeAction : public CastSpellAction
{
public:
    CastPurgeAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "purge") {}
};

class CastCleanseSpiritAction : public CastCureSpellAction
{
public:
    CastCleanseSpiritAction(PlayerbotAI* botAI) :
        CastCureSpellAction(botAI, "cleanse spirit") {}
};

class CastCleanseSpiritPoisonOnPartyAction : public CurePartyMemberAction
{
public:
    CastCleanseSpiritPoisonOnPartyAction(PlayerbotAI* botAI) :
        CurePartyMemberAction(botAI, "cleanse spirit", DISPEL_POISON) {}

    std::string const getName() override { return "cleanse spirit poison on party"; }
};

class CastCleanseSpiritCurseOnPartyAction : public CurePartyMemberAction
{
public:
    CastCleanseSpiritCurseOnPartyAction(PlayerbotAI* botAI) :
        CurePartyMemberAction(botAI, "cleanse spirit", DISPEL_CURSE) {}

    std::string const getName() override { return "cleanse spirit curse on party"; }
};

class CastCleanseSpiritDiseaseOnPartyAction : public CurePartyMemberAction
{
public:
    CastCleanseSpiritDiseaseOnPartyAction(PlayerbotAI* botAI) :
        CurePartyMemberAction(botAI, "cleanse spirit", DISPEL_DISEASE) {}

    std::string const getName() override { return "cleanse spirit disease on party"; }
};

class CastCureToxinsActionSham : public CastCureSpellAction
{
public:
    CastCureToxinsActionSham(PlayerbotAI* botAI) :
        CastCureSpellAction(botAI, "cure toxins") {}
};

class CastCureToxinsPoisonOnPartyActionSham : public CurePartyMemberAction
{
public:
    CastCureToxinsPoisonOnPartyActionSham(PlayerbotAI* botAI) :
        CurePartyMemberAction(botAI, "cure toxins", DISPEL_POISON) {}

    std::string const getName() override { return "cure toxins poison on party"; }
};

class CastCureToxinsDiseaseOnPartyActionSham : public CurePartyMemberAction
{
public:
    CastCureToxinsDiseaseOnPartyActionSham(PlayerbotAI* botAI) :
        CurePartyMemberAction(botAI, "cure toxins", DISPEL_DISEASE) {}

    std::string const getName() override { return "cure toxins disease on party"; }
};

// Damage and Debuff Actions

// [b31_sd] Fire Nova bursts from the shaman's fire totem, not from the shaman: no melee-range requirement (a ranged
// Elemental never stood in melee, so it never cast it). Cast on self; isUseful() checks the totem and the pack.
class CastFireNovaAction : public CastSpellAction
{
public:
    CastFireNovaAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "fire nova") {}

    std::string const GetTargetName() override { return "self target"; }
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
    bool isUseful() override;
};

class CastStormstrikeAction : public CastMeleeSpellAction
{
public:
    CastStormstrikeAction(PlayerbotAI* botAI) :
        CastMeleeSpellAction(botAI, "stormstrike") {}
};

class CastLavaLashAction : public CastMeleeSpellAction
{
public:
    CastLavaLashAction(PlayerbotAI* botAI) :
        CastMeleeSpellAction(botAI, "lava lash") {}
};

class CastFlameShockAction : public CastDebuffSpellAction
{
public:
    CastFlameShockAction(PlayerbotAI* botAI) :
        CastDebuffSpellAction(botAI, "flame shock", true, FLAME_SHOCK_MIN_LIFETIME_S)
    {
        beforeDuration = FLAME_SHOCK_REFRESH_WINDOW_MS;
    }
    // [b31_sd] The isUseful() override to CastAuraSpellAction is gone: it skipped CastDebuffSpellAction's
    // time-to-live gate, the same dead 6 s argument as FlameShockTrigger.
};

// [b31_deep] Elemental Flame Shock multidot (FlameShockOnAttackerTrigger). Shocks share one cooldown, so a second
// Flame Shock goes out only while the current target's own Flame Shock outlasts that cooldown plus a second (the
// rule of "earth shock between flame shocks"): the main target's Flame Shock and its Lava Burst crits come first.
class CastFlameShockOnAttackerAction : public CastDebuffSpellOnAttackerAction
{
public:
    CastFlameShockOnAttackerAction(PlayerbotAI* botAI) :
        CastDebuffSpellOnAttackerAction(botAI, "flame shock", true, FLAME_SHOCK_MULTIDOT_MIN_LIFETIME_S) {}

    bool isUseful() override;
};

class CastEarthShockAction : public CastSpellAction
{
public:
    CastEarthShockAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "earth shock") {}
};

// Enhancement Earth Shock on cooldown (wowsims); Flame Shock, ranked above it, takes the shared cooldown when due.
class CastEarthShockBetweenFlameShocksAction : public CastSpellAction
{
public:
    CastEarthShockBetweenFlameShocksAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "earth shock") {}

    std::string const getName() override { return "earth shock between flame shocks"; }
    bool isUseful() override;
};

class CastEarthShockOnTheMoveAction : public CastSpellAction
{
public:
    CastEarthShockOnTheMoveAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "earth shock") {}

    bool isUseful() override;
};

class CastFrostShockAction : public CastSnareSpellAction
{
public:
    CastFrostShockAction(PlayerbotAI* botAI) :
        CastSnareSpellAction(botAI, "frost shock") {}
};

class CastChainLightningAction : public CastSpellAction
{
public:
    CastChainLightningAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "chain lightning") {}

    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
};

class CastLightningBoltAction : public CastSpellAction
{
public:
    CastLightningBoltAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "lightning bolt") {}
};

class CastThunderstormAction : public CastSpellAction
{
public:
    CastThunderstormAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "thunderstorm") {}
};

class CastLavaBurstAction : public CastSpellAction
{
public:
    CastLavaBurstAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "lava burst") {}
    bool isUseful() override;
};

// Healing Actions

class CastLesserHealingWaveAction : public CastHealingSpellAction
{
public:
    CastLesserHealingWaveAction(PlayerbotAI* botAI) :
        CastHealingSpellAction(botAI, "lesser healing wave") {}
};

class CastLesserHealingWaveOnPartyAction : public HealPartyMemberAction
{
public:
    CastLesserHealingWaveOnPartyAction(PlayerbotAI* botAI) :
        HealPartyMemberAction(botAI, "lesser healing wave", 25.0f, HealingManaEfficiency::LOW) {}
};

class CastHealingWaveAction : public CastHealingSpellAction
{
public:
    CastHealingWaveAction(PlayerbotAI* botAI) :
        CastHealingSpellAction(botAI, "healing wave") {}
};

class CastHealingWaveOnPartyAction : public HealPartyMemberAction
{
public:
    CastHealingWaveOnPartyAction(PlayerbotAI* botAI) :
        HealPartyMemberAction(botAI, "healing wave", 50.0f, HealingManaEfficiency::MEDIUM) {}
};

class CastChainHealAction : public HealPartyMemberAction
{
public:
    CastChainHealAction(PlayerbotAI* botAI) :
        HealPartyMemberAction(botAI, "chain heal", 15.0f, HealingManaEfficiency::HIGH) {}

    bool isUseful() override;
};

class CastRiptideAction : public CastHealingSpellAction
{
public:
    CastRiptideAction(PlayerbotAI* botAI) :
        CastHealingSpellAction(botAI, "riptide") {}
};

class CastRiptideOnPartyAction : public HealPartyMemberAction
{
public:
    CastRiptideOnPartyAction(PlayerbotAI* botAI) :
        HealPartyMemberAction(botAI, "riptide", 15.0f, HealingManaEfficiency::VERY_HIGH) {}
};

class CastEarthShieldAction : public CastBuffSpellAction
{
public:
    CastEarthShieldAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "earth shield") {}
};

class CastEarthShieldOnPartyAction : public BuffOnPartyAction
{
public:
    CastEarthShieldOnPartyAction(PlayerbotAI* botAI) :
        BuffOnPartyAction(botAI, "earth shield") {}
};

class CastEarthShieldOnMainTankAction : public BuffOnMainTankAction
{
public:
    CastEarthShieldOnMainTankAction(PlayerbotAI* botAI) :
        BuffOnMainTankAction(botAI, "earth shield", false) {}

    // [b31_sd] Useful also on the shaman's own Earth Shield down to EARTH_SHIELD_RECAST_CHARGES (the trigger's rule).
    bool isUseful() override;
};

class CastRiptideOnMainTankAction : public BuffOnMainTankAction
{
public:
    CastRiptideOnMainTankAction(PlayerbotAI* botAI) :
        BuffOnMainTankAction(botAI, "riptide", true) {}
};

// Earth Shield goes back on a battle-resurrected member only when he is a tank.
class CastEarthShieldOnBattleResurrectedTankAction : public CastBuffOnValueTargetAction
{
public:
    CastEarthShieldOnBattleResurrectedTankAction(PlayerbotAI* botAI)
        : CastBuffOnValueTargetAction(botAI, "earth shield", "battle resurrected member", "battle resurrected")
    {
    }

    bool isUseful() override;
};

class CastShamanNaturesSwiftnessAction : public CastBuffSpellAction
{
public:
    CastShamanNaturesSwiftnessAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "nature's swiftness") {}
};

class CastTidalForceAction : public CastBuffSpellAction
{
public:
    CastTidalForceAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "tidal force") {}
};

// Totem Spells

class CastTotemAction : public CastBuffSpellAction
{
public:
    CastTotemAction(
        PlayerbotAI* botAI, std::string const spell,
        std::string const buffName = "") : CastBuffSpellAction(botAI, spell)
    {
        buff = (buffName == "") ? spell : buffName;
        auraTotem = buffName != "";
    }

    bool isUseful() override;

protected:
    std::string buff;
    bool auraTotem = false;  // its aura reaches the group around it (strength of earth, wrath of air, ...)
};

class CastCallOfTheElementsAction : public CastSpellAction
{
public:
    CastCallOfTheElementsAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "call of the elements") {}

    bool isUseful() override;
    // [rot300] A self cast that needs no target: on "current target" a healer with none (the resto shaman heals
    // the group, no enemy picked) never cast it, and the single drops wait for it while 2+ slots are empty
    // (DefersToCallOfTheElements). b218 rfc-h 5D: Rtshaad 0 totems in 4 boss fights except Mana Tide, while the
    // enh / ele shamans cast it "-> Earthborer" (their current target).
    std::string const GetTargetName() override { return "self target"; }
};

class CastTotemicRecallAction : public CastSpellAction
{
public:
    CastTotemicRecallAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "totemic recall") {}

    // [drift_fe_recall] A self cast, out of combat: on "current target" (none after a fight) isUseful was always
    // false ("no target (current target is empty)", scholo-h 20261025-150737-4), so no totem was ever recalled.
    std::string const GetTargetName() override { return "self target"; }
};

// [rot_b31_heal] Out of combat: unsummons the own Searing / Magma / Earthbind / Stoneclaw totem (and a dungeon Fire
// Elemental) so it does not open the next pack, and keeps the friendly totems for the next pull. No GCD, no mana.
class DismissHostileTotemsAction : public Action
{
public:
    DismissHostileTotemsAction(PlayerbotAI* botAI) : Action(botAI, "dismiss hostile totems") {}

    bool isUseful() override { return ai::shaman::HasIdleHostileTotem(bot); }
    bool Execute(Event /*event*/) override { return ai::shaman::DismissIdleHostileTotems(bot) > 0; }
};

class CastStrengthOfEarthTotemAction : public CastTotemAction
{
public:
    CastStrengthOfEarthTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "strength of earth totem", "strength of earth") {}
};

class CastStoneskinTotemAction : public CastTotemAction
{
public:
    CastStoneskinTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "stoneskin totem", "stoneskin") {}
};

class CastTremorTotemAction : public CastTotemAction
{
public:
    CastTremorTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "tremor totem", "") {}
};

class CastEarthbindTotemAction : public CastTotemAction
{
public:
    CastEarthbindTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "earthbind totem", "") {}
};

class CastStoneclawTotemAction : public CastTotemAction
{
public:
    CastStoneclawTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "stoneclaw totem", "") {}
    bool isUseful() override;
};

class CastSearingTotemAction : public CastTotemAction
{
public:
    CastSearingTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "searing totem", "") {}

    bool isUseful() override;
};

class CastMagmaTotemAction : public CastTotemAction
{
public:
    CastMagmaTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "magma totem", "") {}

    std::string const GetTargetName() override { return "self target"; }
    bool isUseful() override;
};

class CastFlametongueTotemAction : public CastTotemAction
{
public:
    CastFlametongueTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "flametongue totem", "flametongue totem") {}
};

class CastTotemOfWrathAction : public CastTotemAction
{
public:
    CastTotemOfWrathAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "totem of wrath", "totem of wrath") {}
};

class CastFrostResistanceTotemAction : public CastTotemAction
{
public:
    CastFrostResistanceTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "frost resistance totem", "frost resistance") {}
};

class CastFireElementalTotemAction : public CastTotemAction
{
public:
    CastFireElementalTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "fire elemental totem", "") {}

    virtual std::string const GetTargetName() override { return "self target"; }
    virtual bool isUseful() override { return CastTotemAction::isUseful(); }
};

class CastFireElementalTotemMeleeAction : public CastTotemAction
{
public:
    CastFireElementalTotemMeleeAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "fire elemental totem", "") {}

    virtual std::string const GetTargetName() override { return "self target"; }
    virtual bool isUseful() override
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (!target || !bot->IsWithinMeleeRange(target))
            return false;
        return CastTotemAction::isUseful();
    }
};

class CastHealingStreamTotemAction : public CastTotemAction
{
public:
    CastHealingStreamTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "healing stream totem", "") {}
};

class CastManaSpringTotemAction : public CastTotemAction
{
public:
    CastManaSpringTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "mana spring totem", "mana spring") {}
};

class CastCleansingTotemAction : public CastTotemAction
{
public:
    CastCleansingTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "cleansing totem", "") {}
    virtual bool isUseful();
};

class CastManaTideTotemAction : public CastTotemAction
{
public:
    CastManaTideTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "mana tide totem", "") {}

    std::string const GetTargetName() override { return "self target"; }
};

class CastFireResistanceTotemAction : public CastTotemAction
{
public:
    CastFireResistanceTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "fire resistance totem", "fire resistance") {}
};

class CastWrathOfAirTotemAction : public CastTotemAction
{
public:
    CastWrathOfAirTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "wrath of air totem", "wrath of air totem") {}
};

class CastWindfuryTotemAction : public CastTotemAction
{
public:
    CastWindfuryTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "windfury totem", "windfury totem") {}
};

// The last fallback of the air totem chain (Wrath of Air -> Windfury -> Grounding) before Windfury is learned.
class CastGroundingTotemAction : public CastTotemAction
{
public:
    CastGroundingTotemAction(PlayerbotAI* botAI) : CastTotemAction(botAI, "grounding totem") {}
};

class CastNatureResistanceTotemAction : public CastTotemAction
{
public:
    CastNatureResistanceTotemAction(PlayerbotAI* botAI) :
        CastTotemAction(botAI, "nature resistance totem", "nature resistance") {}
};

// Set Strategy Assigned Totems

class SetTotemAction : public Action
{
public:
    // Template constructor: infers N (size of the id array) at compile time
    template <size_t N>
    SetTotemAction(PlayerbotAI* botAI, std::string const& totemName, const uint32 (&ids)[N], int actionButtonId) :
        Action(botAI, "set " + totemName), totemSpellIds(ids), totemSpellIdsCount(N), actionButtonId(actionButtonId) {}

    bool Execute(Event event) override;
    uint32 const* totemSpellIds;
    size_t totemSpellIdsCount;
    int actionButtonId;
};

class SetStrengthOfEarthTotemAction : public SetTotemAction
{
public:
    SetStrengthOfEarthTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "strength of earth totem", STRENGTH_OF_EARTH_TOTEM, TOTEM_BAR_SLOT_EARTH) {}
};

class SetStoneskinTotemAction : public SetTotemAction
{
public:
    SetStoneskinTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "stoneskin totem", STONESKIN_TOTEM, TOTEM_BAR_SLOT_EARTH) {}
};

class SetTremorTotemAction : public SetTotemAction
{
public:
    SetTremorTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "tremor totem", TREMOR_TOTEM, TOTEM_BAR_SLOT_EARTH) {}
};

class SetEarthbindTotemAction : public SetTotemAction
{
public:
    SetEarthbindTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "earthbind totem", EARTHBIND_TOTEM, TOTEM_BAR_SLOT_EARTH) {}
};

class SetSearingTotemAction : public SetTotemAction
{
public:
    SetSearingTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "searing totem", SEARING_TOTEM, TOTEM_BAR_SLOT_FIRE) {}
};

class SetMagmaTotemAction : public SetTotemAction
{
public:
    SetMagmaTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "magma totem", MAGMA_TOTEM, TOTEM_BAR_SLOT_FIRE) {}
};

class SetFlametongueTotemAction : public SetTotemAction
{
public:
    SetFlametongueTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "flametongue totem", FLAMETONGUE_TOTEM, TOTEM_BAR_SLOT_FIRE) {}
};

class SetTotemOfWrathAction : public SetTotemAction
{
public:
    SetTotemOfWrathAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "totem of wrath", TOTEM_OF_WRATH, TOTEM_BAR_SLOT_FIRE) {}
};

class SetFrostResistanceTotemAction : public SetTotemAction
{
public:
    SetFrostResistanceTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "frost resistance totem", FROST_RESISTANCE_TOTEM, TOTEM_BAR_SLOT_FIRE) {}
};

class SetHealingStreamTotemAction : public SetTotemAction
{
public:
    SetHealingStreamTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "healing stream totem", HEALING_STREAM_TOTEM, TOTEM_BAR_SLOT_WATER) {}
};

class SetManaSpringTotemAction : public SetTotemAction
{
public:
    SetManaSpringTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "mana spring totem", MANA_SPRING_TOTEM, TOTEM_BAR_SLOT_WATER) {}
};

class SetCleansingTotemAction : public SetTotemAction
{
public:
    SetCleansingTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "cleansing totem", CLEANSING_TOTEM, TOTEM_BAR_SLOT_WATER) {}
};

class SetFireResistanceTotemAction : public SetTotemAction
{
public:
    SetFireResistanceTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "fire resistance totem", FIRE_RESISTANCE_TOTEM, TOTEM_BAR_SLOT_WATER) {}
};

class SetWrathOfAirTotemAction : public SetTotemAction
{
public:
    SetWrathOfAirTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "wrath of air totem", WRATH_OF_AIR_TOTEM, TOTEM_BAR_SLOT_AIR) {}
};

class SetWindfuryTotemAction : public SetTotemAction
{
public:
    SetWindfuryTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "windfury totem", WINDFURY_TOTEM, TOTEM_BAR_SLOT_AIR) {}
};

class SetNatureResistanceTotemAction : public SetTotemAction
{
public:
    SetNatureResistanceTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "nature resistance totem", NATURE_RESISTANCE_TOTEM, TOTEM_BAR_SLOT_AIR) {}
};

class SetGroundingTotemAction : public SetTotemAction
{
public:
    SetGroundingTotemAction(PlayerbotAI* botAI) :
        SetTotemAction(botAI, "grounding totem", GROUNDING_TOTEM, TOTEM_BAR_SLOT_AIR) {}
};

// Raid utility coordinator: swaps buff totem strategies so two shamans of a party do not drop the same totem.
class ApplyTotemAssignmentAction : public Action
{
public:
    ApplyTotemAssignmentAction(PlayerbotAI* botAI) : Action(botAI, "apply totem assignment") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// [b31_sd] Swaps the resto water totem strategy (WaterTotemChoiceTrigger); the water strategies are siblings, so
// adding one replaces the other.
class ApplyWaterTotemChoiceAction : public Action
{
public:
    ApplyWaterTotemChoiceAction(PlayerbotAI* botAI) : Action(botAI, "apply water totem choice") {}

    bool Execute(Event /*event*/) override
    {
        std::string const strategy = ai::shaman::GetWaterTotemChoiceChange(botAI);
        if (strategy.empty())
            return false;

        botAI->ChangeStrategy("+" + strategy, BOT_STATE_COMBAT);
        return true;
    }
};

#endif
