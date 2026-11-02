/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SHAMANTRIGGERS_H
#define PLAYERBOTS_SHAMANTRIGGERS_H

#include "CureTriggers.h"
#include "GenericTriggers.h"
#include "PlayerbotAI.h"
#include "SharedDefines.h"
#include "TotemsShamanStrategy.h"
#include "Trigger.h"
#include <ctime>
#include <string>

class PlayerbotAI;

// Buff and Out of Combat Triggers

const uint32 SPELL_EARTHBIND_TOTEM_RANK_1 = 2484;
const uint32 SPELL_SEARING_TOTEM_RANK_1 = 3599;
const uint32 SPELL_WRATH_OF_AIR_TOTEM_RANK_1 = 3738;
const uint32 SPELL_HEALING_STREAM_TOTEM_RANK_1 = 5394;
const uint32 SPELL_MANA_SPRING_TOTEM_RANK_1 = 5675;
const uint32 SPELL_STONESKIN_TOTEM_RANK_1 = 8071;
const uint32 SPELL_STRENGTH_OF_EARTH_TOTEM_RANK_1 = 8075;
const uint32 SPELL_TREMOR_TOTEM_RANK_1 = 8143;
const uint32 SPELL_CLEANSING_TOTEM_RANK_1 = 8170;
const uint32 SPELL_GROUNDING_TOTEM_RANK_1 = 8177;
const uint32 SPELL_FROST_RESISTANCE_TOTEM_RANK_1 = 8181;
const uint32 SPELL_FIRE_RESISTANCE_TOTEM_RANK_1 = 8184;
const uint32 SPELL_MAGMA_TOTEM_RANK_1 = 8190;
const uint32 SPELL_FLAMETONGUE_TOTEM_RANK_1 = 8227;
const uint32 SPELL_WINDFURY_TOTEM_RANK_1 = 8512;
const uint32 SPELL_NATURE_RESISTANCE_TOTEM_RANK_1 = 10595;
const uint32 SPELL_TOTEM_OF_WRATH_RANK_1 = 30706;
const uint32 SPELL_TOTEMIC_RECALL = 36936;
const uint32 SPELL_SHAMANISTIC_RAGE = 30823;
const uint32 SPELL_CALL_OF_THE_ELEMENTS = 66842;

class MainHandWeaponNoImbueTrigger : public BuffTrigger
{
public:
    MainHandWeaponNoImbueTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "main hand", 1) {}
    virtual bool IsActive();
};

class OffHandWeaponNoImbueTrigger : public BuffTrigger
{
public:
    OffHandWeaponNoImbueTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "off hand", 1) {}
    virtual bool IsActive();
};

class WaterShieldTrigger : public BuffTrigger
{
public:
    WaterShieldTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "water shield") {}
};

class LightningShieldTrigger : public BuffTrigger
{
public:
    LightningShieldTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "lightning shield") {}
};

class WaterWalkingTrigger : public BuffTrigger
{
public:
    WaterWalkingTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "water walking", 7) {}

    bool IsActive() override;
};

class WaterBreathingTrigger : public BuffTrigger
{
public:
    WaterBreathingTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "water breathing", 10 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class WaterWalkingOnPartyTrigger : public BuffOnPartyTrigger
{
public:
    WaterWalkingOnPartyTrigger(PlayerbotAI* botAI)
        : BuffOnPartyTrigger(botAI, "water walking on party", 4 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class WaterBreathingOnPartyTrigger : public BuffOnPartyTrigger
{
public:
    WaterBreathingOnPartyTrigger(PlayerbotAI* botAI)
        : BuffOnPartyTrigger(botAI, "water breathing on party", 4 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

// Boost Triggers

// Bloodlust/Heroism follow the raid utility coordinator: one shaman per group, boss pulls only in instances.
class HeroismTrigger : public BoostTrigger
{
public:
    HeroismTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "heroism") {}

    bool IsActive() override;
};

class BloodlustTrigger : public BoostTrigger
{
public:
    BloodlustTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "bloodlust") {}

    // [b31_deep] Heroism and Bloodlust: known and off cooldown first (ai::shaman::IsSpellReady). An Alliance shaman
    // ran "bloodlust" every tick into "spell not known" (362 refusals, DEEP_AUDIT_SHAMAN.md).
    bool IsActive() override;
};

// Mana Tide only reaches the shaman's own party: one healer of the party very low, or two getting low.
// [rot_b31_heal] ...and early (shaman below EARLY_MANA_PCT) in a fight that will drain the rest: a boss, or the
// shaman's mana falling fast since the pull ([rot15] the 30 s fight-length path removed: no clock decides). A 5 min
// cooldown used at 15 % came too late (one cast per run in build 30; an-h Hadronox: on cooldown when the shaman was
// at 0-3 %).
class PartyHealerLowManaTrigger : public Trigger
{
public:
    static constexpr float EARLY_MANA_PCT = 65.0f;
    static constexpr float FALLING_MANA_PCT = 30.0f;  // mana spent since the combat started that marks a hard fight
    static constexpr uint32 FIGHT_GAP_MS = 5 * IN_MILLISECONDS;  // no evaluation this long: a new fight

    PartyHealerLowManaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "party healer low mana") {}

    bool IsActive() override;

private:
    uint32 combatStartMs = 0;  // 0 out of combat
    uint32 lastCheckMs = 0;
    float combatStartMana = 0.0f;
};

// Raid utility coordinator: the totem plan of this shaman differs from its totem strategies.
class TotemAssignmentTrigger : public Trigger
{
public:
    TotemAssignmentTrigger(PlayerbotAI* botAI) : Trigger(botAI, "totem assignment", 5) {}

    bool IsActive() override;
};

// [b31_sd] Resto shaman alone on the water slot: Healing Stream while a Blessing of Wisdom is on him (Mana Spring
// does not stack with it), Mana Spring otherwise. Out of combat; see GetWaterTotemChoiceChange.
class WaterTotemChoiceTrigger : public Trigger
{
public:
    WaterTotemChoiceTrigger(PlayerbotAI* botAI) : Trigger(botAI, "water totem choice", 5) {}

    bool IsActive() override;
};

namespace ai::shaman
{
    // Totem strategy to switch to for the element (ai::raidutil::TotemElement), or "" to keep the current one.
    std::string GetTotemAssignmentChange(PlayerbotAI* botAI, uint8 element);
    // [b31_sd] "healing stream" / "mana spring" to switch the resto water strategy to, or "" to keep it.
    std::string GetWaterTotemChoiceChange(PlayerbotAI* botAI);

    // [rot_b31_heal] Out of combat: an own fire / earth totem that acts on enemies (Searing, Magma, Earthbind,
    // Stoneclaw; the Fire Elemental in a dungeon) still stands. Two slot lookups.
    bool HasIdleHostileTotem(Player* bot);
    // Unsummons those totems (what a player's right click does); returns how many.
    uint8 DismissIdleHostileTotems(Player* bot);

    // [b31_deep] The spell is known (cached spell id) and off cooldown: one cooldown lookup. BoostTrigger reads only
    // the buff and the burst gate, so a spell on cooldown (or not known) went to its action every tick.
    bool IsSpellReady(PlayerbotAI* botAI, std::string const& spell);

    // [b31_deep] Enhancement Magma Totem / Fire Nova on a single target (wowsims enhancement APLs: Magma refreshed
    // when it expires, Fire Nova on cooldown, also on one target). From FIRE_AOE_SINGLE_TARGET_LEVEL on (Shamanistic
    // Rage on cooldown covers the mana at 80; DEEP_AUDIT_SHAMAN.md: Magma + Fire Nova 19 % of boss damage before
    // [shside_enh_mana]); below it only on a boss and above FIRE_AOE_SINGLE_TARGET_BOSS_MANA_PCT, so the levelling
    // mana fix of [shside_enh_mana] stays on trash. Defined in ShamanActions.cpp.
    bool AllowsSingleTargetFireAoe(PlayerbotAI* botAI, Unit* target);
    // Magma Totem pays for its cost: the single-target case above, or a pack (3+) around the shaman.
    bool MagmaTotemPays(PlayerbotAI* botAI, Unit* target);
    // [lead149] the target's victim is a tank with threat on it (the damaging totems wait for the tank's hold)
    bool TankHoldsTarget(Unit* target);
}

inline constexpr uint8 FIRE_AOE_SINGLE_TARGET_LEVEL = 70;
inline constexpr float FIRE_AOE_SINGLE_TARGET_BOSS_MANA_PCT = 50.0f;

// [rot_b31_heal] Out of combat a Searing / Magma / Earthbind / Stoneclaw totem left at the last fight opens the next
// pack by itself. It goes alone, so the friendly totems kept for the next pull stay (see TotemicRecallTrigger).
class IdleHostileTotemTrigger : public Trigger
{
public:
    IdleHostileTotemTrigger(PlayerbotAI* botAI) : Trigger(botAI, "idle hostile totem") {}
    bool IsActive() override { return ai::shaman::HasIdleHostileTotem(bot); }
};

// [b31_sd] Elemental Mastery (off the GCD, 3 min): ready, not up, in a burst window (BoostTrigger), and Lava Burst
// ready so the instant charge goes into it (Flame Shock 5.3 > Lava Burst 5.2 > Chain Lightning 5.1 in the rotation).
class ElementalMasteryTrigger : public BoostTrigger
{
public:
    ElementalMasteryTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "elemental mastery") {}

    bool IsActive() override;
};

// [b31_deep] Spirit Walk removes movement impairing effects: only while the shaman is rooted or snared
// (PlayerbotAI::IsMovementImpaired). It went out every 32 s while the wolves lived.
class SpiritWalkTrigger : public Trigger
{
public:
    SpiritWalkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "spirit walk ready") {}

    bool IsActive() override;

private:
    time_t lastSpiritWalkTime = 0;
};

class FireElementalTotemTrigger : public BoostTrigger
{
public:
    FireElementalTotemTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "fire elemental totem") {}

    // [b31_deep] Known and off cooldown first: the 10 min cooldown went to the action every tick (804 "on cooldown"
    // refusals on L80 Elemental, 1202 on Enhancement, DEEP_AUDIT_SHAMAN.md).
    bool IsActive() override { return ai::shaman::IsSpellReady(botAI, spell) && BoostTrigger::IsActive(); }
};

// [shside_enh_mana] Feral Spirit (3 min, 12 % base mana) is a cooldown for bosses and hard pulls, like the Fire
// Elemental: it lines up with the burst window instead of going out on every trash mob.
class FeralSpiritBoostTrigger : public BoostTrigger
{
public:
    FeralSpiritBoostTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "feral spirit") {}
    std::string const getName() override { return "feral spirit boost"; }
    // [b31_deep] Known and off cooldown first (30 938 "on cooldown" refusals on L80 Enhancement).
    bool IsActive() override { return ai::shaman::IsSpellReady(botAI, spell) && BoostTrigger::IsActive(); }
};

// [shside_enh_mana] Shamanistic Rage returns mana on every melee hit for 15 s (1 min cooldown): an Enhancement
// shaman uses it on cooldown whenever mana is not near full and he stands in melee, not only below 40 %.
class ShamanisticRageForManaTrigger : public Trigger
{
public:
    ShamanisticRageForManaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "shamanistic rage for mana") {}

    bool IsActive() override;
};

    // CC, Interrupt, and Dispel Triggers

class WindShearInterruptSpellTrigger : public InterruptSpellTrigger
{
public:
    WindShearInterruptSpellTrigger(PlayerbotAI* botAI) : InterruptSpellTrigger(botAI, "wind shear") {}
};

class WindShearInterruptEnemyHealerSpellTrigger : public InterruptEnemyHealerTrigger
{
public:
    WindShearInterruptEnemyHealerSpellTrigger(PlayerbotAI* botAI) : InterruptEnemyHealerTrigger(botAI, "wind shear") {}
};

class PurgeTrigger : public TargetAuraDispelTrigger
{
public:
    PurgeTrigger(PlayerbotAI* botAI) : TargetAuraDispelTrigger(botAI, "purge", DISPEL_MAGIC) {}

    // [rot_b31_heal] Only a dangerous magic buff (damage / haste / shield / heal / immunity / reflect, see the .cpp)
    // and only above a mana floor. Was: any dispellable buff of the target at any mana; the healer audit (build 30)
    // counted 0.7 Purge a minute on the resto shamans, a GCD and 9 % base mana each.
    static constexpr float HEALER_MANA_FLOOR = 50.0f;
    static constexpr float MANA_FLOOR = 30.0f;

    bool IsActive() override;
};

class CleanseSpiritPoisonTrigger : public NeedCureTrigger
{
public:
    CleanseSpiritPoisonTrigger(PlayerbotAI* botAI) : NeedCureTrigger(botAI, "cleanse spirit", DISPEL_POISON) {}
};

class PartyMemberCleanseSpiritPoisonTrigger : public PartyMemberNeedCureTrigger
{
public:
    PartyMemberCleanseSpiritPoisonTrigger(PlayerbotAI* botAI)
        : PartyMemberNeedCureTrigger(botAI, "cleanse spirit", DISPEL_POISON) {}
};

class CleanseSpiritCurseTrigger : public NeedCureTrigger
{
public:
    CleanseSpiritCurseTrigger(PlayerbotAI* botAI) : NeedCureTrigger(botAI, "cleanse spirit", DISPEL_CURSE) {}
};

class PartyMemberCleanseSpiritCurseTrigger : public PartyMemberNeedCureTrigger
{
public:
    PartyMemberCleanseSpiritCurseTrigger(PlayerbotAI* botAI)
        : PartyMemberNeedCureTrigger(botAI, "cleanse spirit", DISPEL_CURSE) {}
};

class CleanseSpiritDiseaseTrigger : public NeedCureTrigger
{
public:
    CleanseSpiritDiseaseTrigger(PlayerbotAI* botAI) : NeedCureTrigger(botAI, "cleanse spirit", DISPEL_DISEASE) {}
};

class PartyMemberCleanseSpiritDiseaseTrigger : public PartyMemberNeedCureTrigger
{
public:
    PartyMemberCleanseSpiritDiseaseTrigger(PlayerbotAI* botAI)
        : PartyMemberNeedCureTrigger(botAI, "cleanse spirit", DISPEL_DISEASE) {}
};

// Damage and Debuff Triggers

class ShockTrigger : public DebuffTrigger
{
public:
    ShockTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "earth shock", 1, true) {}

    bool IsActive() override;
};

class FrostShockSnareTrigger : public SnareTargetTrigger
{
public:
    FrostShockSnareTrigger(PlayerbotAI* botAI) : SnareTargetTrigger(botAI, "frost shock") {}
};

class MaelstromWeaponTrigger : public HasAuraStackTrigger
{
public:
    MaelstromWeaponTrigger(PlayerbotAI* botAI, int stack = 5) : HasAuraStackTrigger(botAI, "maelstrom weapon", stack) {}
};

// [rot105] wowsims enhancement APLs (default_wf / default_ft), second line: Lightning Bolt at 3+ Maelstrom Weapon
// stacks when its cast time + 300 ms is shorter than the time to the next auto attack (APLValueAutoTimeToNext: the
// sooner of the main- and off-hand swing). b211 rfc-h 5A: Lightning Bolt 2.8 casts/min vs the sim's 9.1, the bot cast it
// only at 5 stacks.
inline constexpr uint32 MAELSTROM_WEAVE_STACKS = 3;
inline constexpr uint32 MAELSTROM_WEAVE_MARGIN_MS = 300;

class MaelstromWeaponWeaveTrigger : public MaelstromWeaponTrigger
{
public:
    MaelstromWeaponWeaveTrigger(PlayerbotAI* botAI) : MaelstromWeaponTrigger(botAI, MAELSTROM_WEAVE_STACKS) {}

    bool IsActive() override;
};

inline constexpr uint32 FLAME_SHOCK_REFRESH_WINDOW_MS = 1 * IN_MILLISECONDS;
// [b31_sd] Flame Shock (and the Lava Burst crit it sets up) only on a target expected to live this long.
inline constexpr float FLAME_SHOCK_MIN_LIFETIME_S = 6.0f;
inline constexpr float CHAIN_LIGHTNING_JUMP_RANGE = 10.0f;
inline constexpr float THUNDERSTORM_RADIUS = 10.0f;

class FlameShockTrigger : public DebuffTrigger
{
public:
    FlameShockTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "flame shock", 1, true, FLAME_SHOCK_MIN_LIFETIME_S, FLAME_SHOCK_REFRESH_WINDOW_MS) {}
    // [b31_sd] No IsActive() override any more: it went straight to BuffTrigger::IsActive() and skipped
    // DebuffTrigger's time-to-live gate, so the 6 s argument was dead and Flame Shock went out on dying mobs.
};

// [b31_deep] Elemental Flame Shock multidot (wowsims elemental APLs: Flame Shock on up to 3 targets): 2+ attackers
// and an attacker without this shaman's Flame Shock expected to live FLAME_SHOCK_MULTIDOT_MIN_LIFETIME_S. No count
// of dotted mobs is needed: the 6 s shock cooldown against the 18 s DoT allows at most 18 / 6 = 3 Flame Shocks at
// once. The target is the cached "attacker without aura" value (as the druid's "moonfire on attacker").
inline constexpr float FLAME_SHOCK_MULTIDOT_MIN_LIFETIME_S = 12.0f;
inline constexpr uint8 FLAME_SHOCK_MULTIDOT_MIN_ATTACKERS = 2;

class FlameShockOnAttackerTrigger : public DebuffOnAttackerTrigger
{
public:
    FlameShockOnAttackerTrigger(PlayerbotAI* botAI)
        : DebuffOnAttackerTrigger(botAI, "flame shock", true, FLAME_SHOCK_MULTIDOT_MIN_LIFETIME_S) {}

    bool IsActive() override;  // AI_VALUE is not available in this header
};

class EarthShockExecuteTrigger : public Trigger
{
public:
    EarthShockExecuteTrigger(PlayerbotAI* botAI) : Trigger(botAI, "earth shock execute") {}

    bool IsActive() override;
};

class MaelstromWeapon5AndMediumAoeTrigger : public TwoTriggers
{
public:
    MaelstromWeapon5AndMediumAoeTrigger(PlayerbotAI* botAI) : TwoTriggers(botAI, "maelstrom weapon 5", "medium aoe") {}
};

class MaelstromWeapon4AndMediumAoeTrigger : public TwoTriggers
{
public:
    MaelstromWeapon4AndMediumAoeTrigger(PlayerbotAI* botAI) : TwoTriggers(botAI, "maelstrom weapon 4", "medium aoe") {}
};

class ChainLightningNoCdTrigger : public SpellNoCooldownTrigger
{
public:
    ChainLightningNoCdTrigger(PlayerbotAI* botAI) : SpellNoCooldownTrigger(botAI, "chain lightning") {}
};

// Single-target filler; Flame Shock and Lava Burst outrank it, so it only fills their cooldowns.
class ChainLightningFillerTrigger : public ChainLightningNoCdTrigger
{
public:
    ChainLightningFillerTrigger(PlayerbotAI* botAI) : ChainLightningNoCdTrigger(botAI) {}

    std::string const getName() override { return "chain lightning filler"; }
    bool IsActive() override;
};

class ChainLightningAoeTrigger : public ChainLightningNoCdTrigger
{
public:
    ChainLightningAoeTrigger(PlayerbotAI* botAI) : ChainLightningNoCdTrigger(botAI) {}

    std::string const getName() override { return "chain lightning aoe"; }
    bool IsActive() override;
};

class ThunderstormKnockbackTrigger : public Trigger
{
public:
    ThunderstormKnockbackTrigger(PlayerbotAI* botAI) : Trigger(botAI, "thunderstorm knockback") {}

    bool IsActive() override;
};

// Healing Triggers

class EarthShieldOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    EarthShieldOnMainTankTrigger(PlayerbotAI* botAI) : BuffOnMainTankTrigger(botAI, "earth shield", false) {}

    // [b31_sd] Also when this shaman's Earth Shield on the tank is down to EARTH_SHIELD_RECAST_CHARGES: before, only a
    // missing aura fired, leaving a gap between the last charge and the recast (GUIDE_AUDIT_HEALERS.md, Resto Shaman).
    bool IsActive() override;
};

// [b31_sd] Earth Shield is recast on the tank at this many charges left (6 base, 8 with Improved Earth Shield).
// [b31_deep] 1, was 2: a recast at 2 of 8 charges threw away 2 / 8 = 25 % of every Earth Shield, at 1 it is 12.5 %
// (DEEP_AUDIT_SHAMAN.md, Resto).
inline constexpr uint8 EARTH_SHIELD_RECAST_CHARGES = 1;
// [b31_sd] Riptide on the tank on cooldown once he takes any damage, not only below the almost-full band (85 %).
inline constexpr float RIPTIDE_TANK_HEALTH_PCT = 95.0f;

class RiptideOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    RiptideOnMainTankTrigger(PlayerbotAI* botAI) : BuffOnMainTankTrigger(botAI, "riptide", true) {}

    std::string const getName() override { return "riptide on main tank"; }
    bool IsActive() override;
};

class ShamanNaturesSwiftnessActiveTrigger : public Trigger
{
public:
    ShamanNaturesSwiftnessActiveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nature's swiftness active") {}

    bool IsActive() override { return botAI->HasAura("nature's swiftness", bot); }
};

// [b31_sd] Tidal Waves (53390, from Riptide / Chain Heal: Healing Wave cast 30 % faster) is up and the member to heal
// is hurt enough for a full Healing Wave: below TIDAL_WAVES_HEALING_WAVE_PCT, so a top-off in the almost-full band
// (85 %) stays a Lesser Healing Wave. GUIDE_AUDIT_HEALERS.md: HW 0.58 vs LHW 3.3/min on bosses. One aura lookup and
// the cached "party member to heal".
inline constexpr uint32 SPELL_TIDAL_WAVES_BUFF = 53390;
inline constexpr float TIDAL_WAVES_HEALING_WAVE_PCT = 80.0f;

class TidalWavesHealingWaveTrigger : public Trigger
{
public:
    TidalWavesHealingWaveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tidal waves healing wave") {}

    bool IsActive() override;  // AI_VALUE is not available in this header
};

class ChainHealTargetsTrigger : public AoeHealTrigger
{
public:
    ChainHealTargetsTrigger(PlayerbotAI* botAI) : AoeHealTrigger(botAI, "chain heal targets", "almost full", 3) {}
};

// Totem Triggers

class TotemTrigger : public Trigger
{
public:
    TotemTrigger(PlayerbotAI* botAI, std::string const spell, uint32 attackerCount = 0)
        : Trigger(botAI, spell), attackerCount(attackerCount)
    {
    }

    bool IsActive() override;

protected:
    uint32 attackerCount;
};

class CallOfTheElementsTrigger : public Trigger
{
public:
    CallOfTheElementsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "call of the elements") {}
    bool IsActive() override;
};

class TotemicRecallTrigger : public Trigger
{
public:
    TotemicRecallTrigger(PlayerbotAI* botAI) : Trigger(botAI, "totemic recall") {}
    bool IsActive() override;
};

class NoEarthTotemTrigger : public Trigger
{
public:
    NoEarthTotemTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no earth totem") {}
    bool IsActive() override;
};

class NoFireTotemTrigger : public Trigger
{
public:
    NoFireTotemTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no fire totem") {}
    bool IsActive() override;
};

class NoWaterTotemTrigger : public Trigger
{
public:
    NoWaterTotemTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no water totem") {}
    bool IsActive() override;
};

class NoAirTotemTrigger : public Trigger
{
public:
    NoAirTotemTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no air totem") {}
    bool IsActive() override;
};

class CallOfTheElementsAndEnemyWithinMeleeTrigger : public TwoTriggers
{
public:
    CallOfTheElementsAndEnemyWithinMeleeTrigger(PlayerbotAI* botAI) : TwoTriggers(botAI, "call of the elements", "enemy within melee") {}
};

// Set Strategy Assigned Totems

class SetTotemTrigger : public Trigger
{
public:
    // Template constructor: infers N (size of the id array) at compile time
    template <size_t N>
    SetTotemTrigger(PlayerbotAI* botAI, std::string const& spellName, const uint32 (&ids)[N], int actionButtonId)
        : Trigger(botAI, "set " + spellName)
        , totemSpellIds(ids)
        , totemSpellIdsCount(N)
        , actionButtonId(actionButtonId)
    {}
    bool IsActive() override;

private:
    uint32 const* totemSpellIds;
    size_t totemSpellIdsCount;
    int actionButtonId;
};

class SetStrengthOfEarthTotemTrigger : public SetTotemTrigger
{
public:
    SetStrengthOfEarthTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "strength of earth totem", STRENGTH_OF_EARTH_TOTEM, TOTEM_BAR_SLOT_EARTH) {}
};

class SetStoneskinTotemTrigger : public SetTotemTrigger
{
public:
    SetStoneskinTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "stoneskin totem", STONESKIN_TOTEM, TOTEM_BAR_SLOT_EARTH) {}
};

class SetTremorTotemTrigger : public SetTotemTrigger
{
public:
    SetTremorTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "tremor totem", TREMOR_TOTEM, TOTEM_BAR_SLOT_EARTH) {}
};

class SetEarthbindTotemTrigger : public SetTotemTrigger
{
public:
    SetEarthbindTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "earthbind totem", EARTHBIND_TOTEM, TOTEM_BAR_SLOT_EARTH) {}
};

class SetSearingTotemTrigger : public SetTotemTrigger
{
public:
    SetSearingTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "searing totem", SEARING_TOTEM, TOTEM_BAR_SLOT_FIRE) {}
};

class SetMagmaTotemTrigger : public SetTotemTrigger
{
public:
    SetMagmaTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "magma totem", MAGMA_TOTEM, TOTEM_BAR_SLOT_FIRE) {}
};

class SetFlametongueTotemTrigger : public SetTotemTrigger
{
public:
    SetFlametongueTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "flametongue totem", FLAMETONGUE_TOTEM, TOTEM_BAR_SLOT_FIRE) {}
};

class SetTotemOfWrathTrigger : public SetTotemTrigger
{
public:
    SetTotemOfWrathTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "totem of wrath", TOTEM_OF_WRATH, TOTEM_BAR_SLOT_FIRE) {}
};

class SetFrostResistanceTotemTrigger : public SetTotemTrigger
{
public:
    SetFrostResistanceTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "frost resistance totem", FROST_RESISTANCE_TOTEM, TOTEM_BAR_SLOT_FIRE) {}
};

class SetHealingStreamTotemTrigger : public SetTotemTrigger
{
public:
    SetHealingStreamTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "healing stream totem", HEALING_STREAM_TOTEM, TOTEM_BAR_SLOT_WATER) {}
};

class SetManaSpringTotemTrigger : public SetTotemTrigger
{
public:
    SetManaSpringTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "mana spring totem", MANA_SPRING_TOTEM, TOTEM_BAR_SLOT_WATER) {}
};

class SetCleansingTotemTrigger : public SetTotemTrigger
{
public:
    SetCleansingTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "cleansing totem", CLEANSING_TOTEM, TOTEM_BAR_SLOT_WATER) {}
};

class SetFireResistanceTotemTrigger : public SetTotemTrigger
{
public:
    SetFireResistanceTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "fire resistance totem", FIRE_RESISTANCE_TOTEM, TOTEM_BAR_SLOT_WATER) {}
};

class SetWrathOfAirTotemTrigger : public SetTotemTrigger
{
public:
    SetWrathOfAirTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "wrath of air totem", WRATH_OF_AIR_TOTEM, TOTEM_BAR_SLOT_AIR) {}
};

class SetWindfuryTotemTrigger : public SetTotemTrigger
{
public:
    SetWindfuryTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "windfury totem", WINDFURY_TOTEM, TOTEM_BAR_SLOT_AIR) {}
};

class SetNatureResistanceTotemTrigger : public SetTotemTrigger
{
public:
    SetNatureResistanceTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "nature resistance totem", NATURE_RESISTANCE_TOTEM, TOTEM_BAR_SLOT_AIR) {}
};

class SetGroundingTotemTrigger : public SetTotemTrigger
{
public:
    SetGroundingTotemTrigger(PlayerbotAI* botAI)
        : SetTotemTrigger(botAI, "grounding totem", GROUNDING_TOTEM, TOTEM_BAR_SLOT_AIR) {}
};

// Reincarnation waits until the mobs that wiped the group have reset and walked off the corpse; a pack that stays
// on it keeps the Ankh unused (AutoReleaseSpiritAction then releases after SELF_RESURRECT_WAIT_MS).
class ShamanReincarnationTrigger : public Trigger
{
public:
    ShamanReincarnationTrigger(PlayerbotAI* botAI) : Trigger(botAI, "can self resurrect", 2) {}

    bool IsActive() override;

private:
    bool HostileNearCorpse(bool& onlyIdle);
};

#endif
