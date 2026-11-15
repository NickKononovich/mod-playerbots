/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DpsEstimate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "AiFactory.h"
#include "Creature.h"
#include "DBCStores.h"
#include "EstimatedLifetimeValue.h"  // [dps44] SND_MIN_COMBO_POINTS / SND_MIN_LIFETIME_S, the caster's own gate
#include "Group.h"
#include "HealthTriggers.h"  // [dun230] RestState::DrinkRegenPerSec
#include "Item.h"
#include "LeaderOrders.h"  // [fight21] Leader::Runs::OfBot: which run this bot is in, read from a map thread
#include "Log.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "PetAI.h"  // [rot606] SpecialPets
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellPayoff.h"
#include "Timer.h"
#include "StringFormat.h"
#include "Unit.h"
#include "World.h"

namespace
{
using DpsEstimate::BuffContext;
using DpsEstimate::Result;

// Spell::TriggerGlobalCooldown: a global cooldown stays within 1-1.5 s; category 133 is hasted by cast speed
constexpr int32 GCD_MIN_MS = 1000;
constexpr int32 GCD_MAX_MS = 1500;
constexpr uint32 GCD_HASTED_CATEGORY = 133;
// Unit::RollMeleeOutcomeAgainst: 0.04 % per point of weapon skill over the victim's maximum skill
constexpr float SKILL_BONUS_PCT = 0.04f;
// Unit::RollMeleeOutcomeAgainst: glancing 6 % + 1.2 % per defense point over the attacker's skill, at most 40 %
constexpr float GLANCE_BASE_PCT = 6.0f;
constexpr float GLANCE_PER_SKILL_PCT = 1.2f;
constexpr float GLANCE_MAX_PCT = 40.0f;
// Unit::CalculateMeleeDamage: a white crit doubles the hit
constexpr float WHITE_CRIT_MULT = 2.0f;
// Unit::MeleeSpellMissChance: the miss chance is capped at 60 %
constexpr float MISS_MAX_PCT = 60.0f;
// Player::UpdateAttackPowerAndDamage / Unit::CalculateMinMaxDamage: weapon damage gains AP / 14 per second
constexpr float AP_PER_DPS = 14.0f;
// Player::Regenerate (POWER_ENERGY): 10 energy per second before the flat modifier
constexpr float ENERGY_BASE_REGEN = 10.0f;
// Creature::Regenerate (POWER_FOCUS): a hunter pet's 5 focus per second
constexpr float PET_FOCUS_REGEN = 5.0f;
// Unit::RewardRage: core power units are tenths of a rage point
constexpr float RAGE_UNITS = 10.0f;
// the 3.3.5 combo point maximum (Unit::AddComboPoints)
constexpr uint8 MAX_COMBO = 5;
constexpr uint8 RUNE_TYPES = 3;  // blood, frost, unholy (SpellRuneCostEntry::RuneCost)
constexpr uint8 RUNE_SLOTS = 6;
constexpr size_t CACHE_PER_BOT = 8;

// core combo point scripts (SpellEffects.cpp Spell::EffectSchoolDMG; spell_rogue.cpp:601 Rupture)
constexpr float EVISCERATE_AP_PER_CP = 0.07f;
constexpr float FEROCIOUS_BITE_AP_PER_CP = 0.07f;
constexpr float ENVENOM_AP_PER_CP = 0.09f;
constexpr std::array<float, MAX_COMBO + 1> RUPTURE_AP_PER_TICK = {0.0f, 0.015f, 0.024f, 0.03f, 0.03428571f, 0.0375f};
// spell_paladin.cpp spell_pal_seal_of_righteousness: (0.022 AP + 0.044 holy SP) x the main hand's speed
constexpr float SEAL_RIGHTEOUSNESS_AP = 0.022f;
constexpr float SEAL_RIGHTEOUSNESS_SP = 0.044f;
constexpr uint32 SPELL_SEAL_OF_RIGHTEOUSNESS_DAMAGE = 25742;
constexpr uint32 SPELL_JUDGEMENT_OF_RIGHTEOUSNESS = 20187;
// SpellEffects.cpp Steady Shot: + 0.1 x ranged AP on the normalized weapon and ammo damage
constexpr float STEADY_SHOT_RAP = 0.1f;
// Spell::Delayed: a landed hit pushes a cast back 500 ms (less its pushback reduction), at most twice a cast
constexpr float PUSHBACK_MS = 500.0f;
constexpr float PUSHBACK_MAX = 2.0f;

enum class Kind : uint8
{
    Dot,           // kept up: once per its duration
    Cooldown,      // on cooldown
    Filler,        // the rest of the time (non-rune classes: the best one only)
    Swing,         // on the next swing (Heroic Strike, Maul): replaces a main hand swing
    Builder,       // combo point builder
    Finisher,      // combo point finisher at five points
    ComboDot,      // combo point finisher kept up (Rupture, Rip)
    Dump,          // runic power dump
    Judgement,     // the seal's judgement on cooldown
    PaidCooldown,  // [rot603] on cooldown where one cast beats the filler in its execute time (DpsEstimate::CastPays)
    Opener,        // [rot605] once at a solo fight's start (its fresh mob): nothing in the steady state
    HasteFinisher  // [dps237] a combo point finisher kept up for its melee haste (Slice and Dice)
};

enum class Script : uint8
{
    None,
    ApPctOfEffect,  // Bloodthirst: the effect value is a % of AP (spell_warrior.cpp spell_warr_bloodthirst)
    Eviscerate,
    Envenom,
    FerociousBite,
    Rupture,
    SteadyShot,
    Conflagrate,  // Immolate's full damage x (effect 1 + effect 2) % (SpellEffects.cpp)
    BestFiller    // [rot604] the stronger of the marked fillers it knows by DpsEstimate::FillerDps, as the bot picks
};

struct Ability
{
    char const* name;
    Kind kind;
    Script script = Script::None;
};

struct SpecRotation
{
    uint8 cls;
    uint8 tab;    // 255: any talent tree
    bool caster;  // casts instead of swinging (white damage is only its fallback)
    std::vector<Ability> abilities;
};

// priorities after the wowsims APLs (ui/<spec>/apls/*.apl.json) without procs, trinkets and execute phases.
// [dpscal] Only what the bot's own strategy casts (src/Ai/Class/<Class>/Strategy): the fillers in its priority order,
// the first one it knows is the one it casts (b337: frost mages measured 0.63 of a formula that took Fireball over
// the Frostbolt the bot always casts, holy priests 0.67-0.71 with a Mind Blast the bot never casts)
std::vector<SpecRotation> const& Rotations()
{
    static std::vector<SpecRotation> const rotations = {
        // warrior/apls arms: Rend, Mortal Strike, Slam, Heroic Strike as the rage dump
        {CLASS_WARRIOR,
         0,
         false,
         {{"Rend", Kind::Dot},
          {"Mortal Strike", Kind::Cooldown},
          {"Slam", Kind::Filler},
          {"Heroic Strike", Kind::Swing}}},
        // warrior/apls fury: Bloodthirst, Whirlwind, Heroic Strike
        {CLASS_WARRIOR,
         1,
         false,
         {{"Bloodthirst", Kind::Cooldown, Script::ApPctOfEffect},
          {"Whirlwind", Kind::Cooldown},
          {"Rend", Kind::Dot},
          {"Heroic Strike", Kind::Swing}}},
        // protection_warrior/apls/default: Shield Slam, Revenge, Thunder Clap, Devastate, Heroic Strike; [rot609]
        // Thunder Clap only to keep its debuff up (the APL's auraShouldRefresh, the bot's "thunder clap and rage"; b341
        // L10-19: 0.2 casts a fight against a formula that cast it on its 6 s cooldown, 0.81 of it)
        {CLASS_WARRIOR,
         2,
         false,
         {{"Shield Slam", Kind::Cooldown},
          {"Revenge", Kind::Cooldown},
          {"Thunder Clap", Kind::Dot},
          {"Rend", Kind::Dot},
          {"Devastate", Kind::Filler},
          {"Sunder Armor", Kind::Filler},
          {"Heroic Strike", Kind::Swing}}},
        // healing paladins swing with a seal and judge
        {CLASS_PALADIN,
         0,
         false,
         {{"Judgement of Light", Kind::Judgement},
          {"Judgement of Wisdom", Kind::Judgement},
          {"Holy Shock", Kind::Cooldown},
          {"Exorcism", Kind::Cooldown}}},
        // protection_paladin/apls/default
        {CLASS_PALADIN,
         1,
         false,
         {{"Hammer of the Righteous", Kind::Cooldown},
          {"Shield of Righteousness", Kind::Cooldown},
          {"Avenger's Shield", Kind::Cooldown},
          {"Consecration", Kind::Cooldown},
          {"Judgement of Wisdom", Kind::Judgement},
          {"Judgement of Light", Kind::Judgement},
          {"Exorcism", Kind::Cooldown}}},
        // retribution_paladin/apls/default
        {CLASS_PALADIN,
         2,
         false,
         {{"Judgement of Wisdom", Kind::Judgement},
          {"Judgement of Light", Kind::Judgement},
          {"Crusader Strike", Kind::Cooldown},
          {"Divine Storm", Kind::Cooldown},
          {"Consecration", Kind::Cooldown},
          {"Exorcism", Kind::Cooldown}}},
        // hunter/apls bm / mm / sv
        {CLASS_HUNTER,
         255,
         false,
         {{"Serpent Sting", Kind::Dot},
          {"Chimera Shot", Kind::Cooldown},
          {"Explosive Shot", Kind::Cooldown},
          {"Aimed Shot", Kind::Cooldown},
          {"Arcane Shot", Kind::Cooldown},
          {"Multi-Shot", Kind::Cooldown},
          {"Steady Shot", Kind::Filler, Script::SteadyShot}}},
        // rogue/apls mutilate; [dps237] Slice and Dice kept up in all three (wowsims rogue APLs, Rawr.Rogue)
        {CLASS_ROGUE,
         0,
         false,
         {{"Slice and Dice", Kind::HasteFinisher},
          {"Mutilate", Kind::Builder},
          {"Sinister Strike", Kind::Builder},
          {"Rupture", Kind::ComboDot, Script::Rupture},
          {"Envenom", Kind::Finisher, Script::Envenom},
          {"Eviscerate", Kind::Finisher, Script::Eviscerate}}},
        // rogue/apls combat
        {CLASS_ROGUE,
         1,
         false,
         {{"Slice and Dice", Kind::HasteFinisher},
          {"Sinister Strike", Kind::Builder},
          {"Rupture", Kind::ComboDot, Script::Rupture},
          {"Eviscerate", Kind::Finisher, Script::Eviscerate}}},
        {CLASS_ROGUE,
         2,
         false,
         {{"Slice and Dice", Kind::HasteFinisher},
          {"Hemorrhage", Kind::Builder},
          {"Sinister Strike", Kind::Builder},
          {"Rupture", Kind::ComboDot, Script::Rupture},
          {"Eviscerate", Kind::Finisher, Script::Eviscerate}}},
        // smite_priest/apls/default (Holy Fire, Shadow Word: Pain, Smite): the solo "holy dps" strategy
        // (HolyDpsPriestStrategy: Smite, the "holy fire" trigger, "shadow debuff"); [rot603] Mind Blast where it pays
        // ("mind blast pays": b338 L10-19 ideal / formula 1.11 with it)
        {CLASS_PRIEST,
         0,
         true,
         {{"Shadow Word: Pain", Kind::Dot},
          {"Holy Fire", Kind::Cooldown},
          {"Mind Blast", Kind::PaidCooldown},
          {"Smite", Kind::Filler}}},
        {CLASS_PRIEST,
         1,
         true,
         {{"Shadow Word: Pain", Kind::Dot},
          {"Holy Fire", Kind::Cooldown},
          {"Mind Blast", Kind::PaidCooldown},
          {"Smite", Kind::Filler}}},
        // shadow_priest/apls/default
        {CLASS_PRIEST,
         2,
         true,
         {{"Vampiric Touch", Kind::Dot},
          {"Devouring Plague", Kind::Dot},
          {"Shadow Word: Pain", Kind::Dot},
          {"Mind Blast", Kind::Cooldown},
          {"Mind Flay", Kind::Filler},
          {"Smite", Kind::Filler}}},
        // deathknight/apls blood_dps
        {CLASS_DEATH_KNIGHT,
         0,
         false,
         {{"Icy Touch", Kind::Dot},
          {"Plague Strike", Kind::Dot},
          {"Heart Strike", Kind::Filler},
          {"Death Strike", Kind::Filler},
          {"Blood Strike", Kind::Filler},
          {"Death Coil", Kind::Dump}}},
        // deathknight/apls frost
        {CLASS_DEATH_KNIGHT,
         1,
         false,
         {{"Icy Touch", Kind::Dot},
          {"Plague Strike", Kind::Dot},
          {"Howling Blast", Kind::Cooldown},
          {"Obliterate", Kind::Filler},
          {"Death Strike", Kind::Filler},
          {"Blood Strike", Kind::Filler},
          {"Frost Strike", Kind::Dump},
          {"Death Coil", Kind::Dump}}},
        // deathknight/apls unholy
        {CLASS_DEATH_KNIGHT,
         2,
         false,
         {{"Icy Touch", Kind::Dot},
          {"Plague Strike", Kind::Dot},
          {"Scourge Strike", Kind::Filler},
          {"Death Strike", Kind::Filler},
          {"Blood Strike", Kind::Filler},
          {"Death Coil", Kind::Dump}}},
        // elemental_shaman/apls/default
        {CLASS_SHAMAN,
         0,
         true,
         {{"Flame Shock", Kind::Dot},
          {"Lava Burst", Kind::Cooldown},
          {"Chain Lightning", Kind::Cooldown},
          {"Lightning Bolt", Kind::Filler}}},
        // enhancement_shaman/apls default_wf
        {CLASS_SHAMAN,
         1,
         false,
         {{"Stormstrike", Kind::Cooldown},
          {"Flame Shock", Kind::Dot},
          {"Earth Shock", Kind::Cooldown},
          {"Lava Lash", Kind::Cooldown}}},
        {CLASS_SHAMAN, 2, true, {{"Flame Shock", Kind::Dot}, {"Lightning Bolt", Kind::Filler}}},
        // mage/apls arcane / fire / frost; the bot's defaults (Arcane / Fire / FrostMageStrategy): Arcane Blast, Arcane
        // Missiles, Frostbolt; Fireball, Frostbolt; Frostbolt, Fireball. Arcane Barrage only on the move (below the
        // filler). [rot604] Arcane and Frost cast Fireball over Frostbolt where it deals more ("fireball over
        // frostbolt": Fireball rank 2 at L6-7 against Frostbolt rank 1; b338 L1-9 frost ideal / formula 1.06).
        // [rot603] Fire Blast where it pays for Fire and Frost ("fire blast pays", user 2026-11-08). [dps231] Before
        // Arcane Blast, Arcane Missiles only where it beats that filler ("fireball / frostbolt over arcane missiles")
        {CLASS_MAGE,
         0,
         true,
         {{"Arcane Blast", Kind::Filler},
          {"Arcane Missiles", Kind::Filler, Script::BestFiller},
          {"Fireball", Kind::Filler, Script::BestFiller},
          {"Frostbolt", Kind::Filler, Script::BestFiller}}},
        {CLASS_MAGE,
         1,
         true,
         {{"Living Bomb", Kind::Dot},
          {"Fire Blast", Kind::PaidCooldown},
          {"Fireball", Kind::Filler},
          {"Frostbolt", Kind::Filler}}},
        {CLASS_MAGE,
         2,
         true,
         {{"Fire Blast", Kind::PaidCooldown},
          {"Frostbolt", Kind::Filler, Script::BestFiller},
          {"Fireball", Kind::Filler, Script::BestFiller}}},
        // warlock/apls affliction / demonology / destruction; AfflictionWarlockStrategy (+ "curse of agony"): Haunt,
        // Unstable Affliction, Corruption, Curse of Agony, [rot602] Immolate before Unstable Affliction ("affliction
        // immolate"; ComputeAt drops it once Unstable Affliction is known), Shadow Bolt
        {CLASS_WARLOCK,
         0,
         true,
         {{"Haunt", Kind::Cooldown},
          {"Unstable Affliction", Kind::Dot},
          {"Corruption", Kind::Dot},
          {"Curse of Agony", Kind::Dot},
          {"Immolate", Kind::Dot},
          {"Shadow Bolt", Kind::Filler}}},
        // DemonologyWarlockStrategy: Corruption, Immolate, Shadow Bolt before Incinerate (Molten Core only); [dps235]
        // Curse of Agony too: AiFactory gives Demonology the "curse of agony" strategy (b426-b429: 0.9-1.0 casts a
        // fight against a formula that had none)
        {CLASS_WARLOCK,
         1,
         true,
         {{"Corruption", Kind::Dot},
          {"Immolate", Kind::Dot},
          {"Curse of Agony", Kind::Dot},
          {"Shadow Bolt", Kind::Filler},
          {"Incinerate", Kind::Filler}}},
        // DestructionWarlockStrategy [rot410]: no Corruption (the wowsims destro APL never casts it); [rot601] Immolate
        // under the payoff rule against Shadow Bolt before Incinerate (the formula's DotRateOver rule)
        {CLASS_WARLOCK,
         2,
         true,
         {{"Immolate", Kind::Dot},
          {"Conflagrate", Kind::Cooldown, Script::Conflagrate},
          {"Chaos Bolt", Kind::Cooldown},
          {"Incinerate", Kind::Filler},
          {"Shadow Bolt", Kind::Filler}}},
        // balance_druid/apls (no Eclipse modelled)
        {CLASS_DRUID,
         0,
         true,
         {{"Insect Swarm", Kind::Dot}, {"Moonfire", Kind::Dot}, {"Starfire", Kind::Filler}, {"Wrath", Kind::Filler}}},
        // feral_druid/apls/default (cat), feral_tank_druid/apls/default (bear): picked by the form below
        {CLASS_DRUID,
         1,
         false,
         {{"Rake", Kind::Dot},
          {"Mangle (Cat)", Kind::Builder},
          {"Claw", Kind::Builder},
          {"Rip", Kind::ComboDot},
          {"Ferocious Bite", Kind::Finisher, Script::FerociousBite}}},
        {CLASS_DRUID, 2, true, {{"Moonfire", Kind::Dot}, {"Wrath", Kind::Filler}}},
    };
    return rotations;
}

// feral_tank_druid/apls/default
SpecRotation const& BearRotation()
{
    static SpecRotation const bear = {CLASS_DRUID,
                                      1,
                                      false,
                                      {{"Mangle (Bear)", Kind::Cooldown},
                                       {"Lacerate", Kind::Dot},
                                       {"Swipe (Bear)", Kind::Filler},
                                       {"Maul", Kind::Swing}}};
    return bear;
}

// [rot605] A hunter alone without a pet (below L10, or its pet dead): the mob it shoots runs to it and stays in melee,
// inside Auto Shot's minimum range (GenericHunterStrategy "hunter needs melee" / "hunter melee phase"). Serpent Sting
// and one Arcane Shot open the fight from range, then its weapon and Raptor Strike on the next swing at its cooldown.
// b338 L1-9 beast mastery: Raptor Strike 1.7, Serpent Sting 0.8, Arcane Shot 0.6 casts a 16 s fight, melee swings
// after the opener, 0.78 of a formula that kept Auto Shot and Arcane Shot up the whole fight.
SpecRotation const& HunterMeleeRotation()
{
    static SpecRotation const melee = {
        CLASS_HUNTER,
        255,
        false,
        {{"Serpent Sting", Kind::Dot}, {"Arcane Shot", Kind::Opener}, {"Raptor Strike", Kind::Swing}}};
    return melee;
}

// the solo hunter's case above: no living pet, no group of its own
bool PetlessSoloHunter(Player* bot)
{
    if (bot->getClass() != CLASS_HUNTER)
        return false;
    Pet* pet = bot->GetPet();
    Group* group = bot->GetGroup();
    return (!pet || !pet->IsAlive()) && (!group || group->GetMembersCount() <= 1);
}

// [death446] ... and a hunter whose target is on it inside its melee reach (a pet that holds nothing, a mob that came
// past it) or that has no usable ranged weapon: Auto Shot goes off only past its minimum range, the hunter melees
// ("hunter needs melee"; "hunter step out" finds no spot from a mob that follows it). The formula read Auto Shot
// there: death441's projection reads the formula alone, b337 Eraelor L7 dealt 2.4 dps in melee against 10.2 at range
bool HunterInMelee(Player* bot, Unit* target)
{
    if (bot->getClass() != CLASS_HUNTER)
        return false;
    if (PetlessSoloHunter(bot) || !bot->GetWeaponForAttack(RANGED_ATTACK, true))
        return true;
    return target && target != bot && target->IsAlive() && target->GetVictim() == bot &&
           bot->IsWithinMeleeRange(target);
}

// ---- buffs -------------------------------------------------------------------------------------------------

enum class Category : uint8
{
    ApFlat,
    ApPct,
    StatPct,
    StatFlat,
    Intellect,
    StrAgi,
    MeleeHaste,
    SpellHaste,
    SpellPower,
    MeleeCrit,
    SpellCrit,
    DamagePct,
    ManaRegen,
    SelfInner,
    SelfArmor,
    SelfAspect,
    SelfForm,
    ArmorMajor,
    ArmorMinor,
    SpellTaken,
    PhysTaken,
    CritTaken,
    SpellHitTaken,
    SpellCritTaken,
    Count
};

struct BuffDef
{
    uint32 spell;  // the provider's spell (first rank)
    uint32 aura;   // 0: the provider's rank itself; else this aura (talent area auras, debuffs)
    uint8 cls;
    Category category;
    bool self;      // only on the caster
    bool debuff;    // on the target
    bool raidOnly;  // a talent the raid brings, not read from a group member's spells
};

// spell ids checked against the 3.3.5a Spell.dbc; values are always read from the spell data
std::vector<BuffDef> const& BuffDefs()
{
    static std::vector<BuffDef> const defs = {
        {6673, 0, CLASS_WARRIOR, Category::ApFlat, false, false, false},           // Battle Shout
        {19740, 0, CLASS_PALADIN, Category::ApFlat, false, false, false},          // Blessing of Might
        {20217, 0, CLASS_PALADIN, Category::StatPct, false, false, false},         // Blessing of Kings
        {19742, 0, CLASS_PALADIN, Category::ManaRegen, false, false, false},       // Blessing of Wisdom
        {1126, 0, CLASS_DRUID, Category::StatFlat, false, false, false},           // Mark of the Wild
        {1459, 0, CLASS_MAGE, Category::Intellect, false, false, false},           // Arcane Intellect
        {19506, 0, CLASS_HUNTER, Category::ApPct, false, false, false},            // Trueshot Aura
        {30802, 0, CLASS_SHAMAN, Category::ApPct, false, false, true},             // Unleashed Rage
        {53137, 0, CLASS_DEATH_KNIGHT, Category::ApPct, false, false, true},       // Abomination's Might
        {17007, 24932, CLASS_DRUID, Category::MeleeCrit, false, false, false},     // Leader of the Pack
        {24858, 24907, CLASS_DRUID, Category::SpellCrit, false, false, false},     // Moonkin Form: Moonkin Aura
        {8075, 0, CLASS_SHAMAN, Category::StrAgi, false, false, false},            // Strength of Earth Totem
        {8512, 0, CLASS_SHAMAN, Category::MeleeHaste, false, false, false},        // Windfury Totem
        {3738, 0, CLASS_SHAMAN, Category::SpellHaste, false, false, false},        // Wrath of Air Totem
        {8227, 0, CLASS_SHAMAN, Category::SpellPower, false, false, false},        // Flametongue Totem
        {30706, 0, CLASS_SHAMAN, Category::SpellPower, false, false, false},       // Totem of Wrath
        {5675, 0, CLASS_SHAMAN, Category::ManaRegen, false, false, false},         // Mana Spring Totem
        {75447, 75447, CLASS_HUNTER, Category::DamagePct, false, false, true},     // Ferocious Inspiration
        {588, 0, CLASS_PRIEST, Category::SelfInner, true, false, false},           // Inner Fire
        {15473, 0, CLASS_PRIEST, Category::SelfForm, true, false, false},          // Shadowform
        {30482, 0, CLASS_MAGE, Category::SelfArmor, true, false, false},           // Molten Armor
        {28176, 0, CLASS_WARLOCK, Category::SelfArmor, true, false, false},        // Fel Armor
        {13165, 0, CLASS_HUNTER, Category::SelfAspect, true, false, false},        // Aspect of the Hawk
        {7386, 58567, CLASS_WARRIOR, Category::ArmorMajor, false, true, false},    // Sunder Armor (5 stacks)
        {770, 0, CLASS_DRUID, Category::ArmorMinor, false, true, false},           // Faerie Fire
        {1490, 0, CLASS_WARLOCK, Category::SpellTaken, false, true, false},        // Curse of the Elements
        {30070, 30070, CLASS_WARRIOR, Category::PhysTaken, false, true, true},     // Blood Frenzy
        {21183, 21183, CLASS_PALADIN, Category::CritTaken, false, true, true},     // Heart of the Crusader
        {33196, 33196, CLASS_PRIEST, Category::SpellHitTaken, false, true, true},  // Misery
        {22959, 22959, CLASS_MAGE, Category::SpellCritTaken, false, true, true},   // Improved Scorch
    };
    return defs;
}

struct Bonus
{
    float apFlat = 0.0f, rapFlat = 0.0f, apPct = 0.0f, rapPct = 0.0f;
    float stat[MAX_STATS] = {};
    float statPct = 0.0f;
    float sp = 0.0f, spFromSpiritPct = 0.0f;
    float meleeCrit = 0.0f, spellCrit = 0.0f;
    float meleeHaste = 0.0f, rangedHaste = 0.0f, spellHaste = 0.0f;
    float physDone = 0.0f, magicDone = 0.0f;
    float manaPerSec = 0.0f;
    float armorCut = 0.0f, physTaken = 0.0f, magicTaken = 0.0f;
    float critTaken = 0.0f, spellHitTaken = 0.0f, spellCritTaken = 0.0f;
};

uint32 RankAtLevel(uint32 first, uint8 level)
{
    uint32 best = 0;
    for (uint32 id = first; id; id = sSpellMgr->GetNextSpellInChain(id))
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
        if (!info || info->SpellLevel > level)
            break;
        best = id;
    }
    return best;
}

uint32 KnownRank(Player* player, uint32 first)
{
    uint32 best = 0;
    for (uint32 id = first; id; id = sSpellMgr->GetNextSpellInChain(id))
        if (player->HasSpell(id))
            best = id;
    return best;
}

bool WearsChain(Unit* unit, uint32 first, uint32 aura)
{
    if (aura)
        return unit->HasAura(aura);
    for (uint32 id = first; id; id = sSpellMgr->GetNextSpellInChain(id))
        if (unit->HasAura(id))
            return true;
    return false;
}

// a totem's aura: the summoned creature's first spell (creature_template.spell1)
SpellInfo const* AuraOf(SpellInfo const* info)
{
    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.Effect == SPELL_EFFECT_SUMMON && effect.MiscValue > 0)
            if (CreatureTemplate const* totem = sObjectMgr->GetCreatureTemplate(uint32(effect.MiscValue)))
                if (totem->spells[0])
                    return sSpellMgr->GetSpellInfo(totem->spells[0]);
    return info;
}

// one buff's numbers into `out` (amounts as the provider casts them)
void ReadBuff(SpellInfo const* info, Unit* caster, Bonus& out)
{
    uint8 const stacks = std::max<uint8>(1, uint8(info->StackAmount));
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.IsAura())
            continue;
        float const amount = float(effect.CalcValue(caster));
        int32 const misc = effect.MiscValue;
        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_MOD_ATTACK_POWER:
                out.apFlat += amount;
                break;
            case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
                out.rapFlat += amount;
                break;
            case SPELL_AURA_MOD_ATTACK_POWER_PCT:
                out.apPct += amount;
                break;
            case SPELL_AURA_MOD_RANGED_ATTACK_POWER_PCT:
                out.rapPct += amount;
                break;
            case SPELL_AURA_MOD_STAT:
                for (uint8 s = STAT_STRENGTH; s < MAX_STATS; ++s)
                    if (misc < 0 || misc == s)
                        out.stat[s] += amount;
                break;
            case SPELL_AURA_MOD_PERCENT_STAT:
            case SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE:
                if (misc < 0)
                    out.statPct += amount;
                break;
            case SPELL_AURA_MOD_DAMAGE_DONE:
                if (misc & SPELL_SCHOOL_MASK_MAGIC)
                    out.sp += amount;
                break;
            case SPELL_AURA_MOD_SPELL_DAMAGE_OF_STAT_PERCENT:
                if (misc == STAT_SPIRIT)
                    out.spFromSpiritPct += amount;
                break;
            case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:
                if (misc & SPELL_SCHOOL_MASK_NORMAL)
                    out.physDone += amount;
                if (misc & SPELL_SCHOOL_MASK_MAGIC)
                    out.magicDone += amount;
                break;
            case SPELL_AURA_MOD_WEAPON_CRIT_PERCENT:
                out.meleeCrit += amount;
                break;
            case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:
                out.spellCrit += amount;
                break;
            case SPELL_AURA_MOD_CRIT_PCT:
                out.meleeCrit += amount;
                out.spellCrit += amount;
                break;
            case SPELL_AURA_MOD_MELEE_HASTE:
                out.meleeHaste += amount;
                break;
            case SPELL_AURA_MOD_MELEE_RANGED_HASTE:
                out.meleeHaste += amount;
                out.rangedHaste += amount;
                break;
            case SPELL_AURA_MOD_RANGED_HASTE:
                out.rangedHaste += amount;
                break;
            case SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK:
            case SPELL_AURA_HASTE_SPELLS:
                out.spellHaste += amount;
                break;
            case SPELL_AURA_MOD_POWER_REGEN:
                // Player::UpdateManaRegen: mana per 5 seconds
                if (misc == POWER_MANA)
                    out.manaPerSec += amount / 5.0f;
                break;
            case SPELL_AURA_MOD_RESISTANCE_PCT:
                if ((misc & SPELL_SCHOOL_MASK_NORMAL) && amount < 0.0f)
                    out.armorCut += -amount * stacks;
                break;
            case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
                if (misc & SPELL_SCHOOL_MASK_NORMAL)
                    out.physTaken += amount;
                if (misc & SPELL_SCHOOL_MASK_MAGIC)
                    out.magicTaken += amount;
                break;
            case SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE:
                out.critTaken += amount;
                break;
            case SPELL_AURA_MOD_ATTACKER_SPELL_CRIT_CHANCE:
                out.spellCritTaken += amount;
                break;
            case SPELL_AURA_MOD_ATTACKER_SPELL_HIT_CHANCE:
                out.spellHitTaken += amount;
                break;
            default:
                break;
        }
    }
}

float CategoryValue(Bonus const& b)
{
    float sum = b.apFlat + b.rapFlat + b.apPct + b.rapPct + b.statPct + b.sp + b.spFromSpiritPct + b.meleeCrit +
                b.spellCrit + b.meleeHaste + b.rangedHaste + b.spellHaste + b.physDone + b.magicDone + b.manaPerSec +
                b.armorCut + b.physTaken + b.magicTaken + b.critTaken + b.spellHitTaken + b.spellCritTaken;
    for (float s : b.stat)
        sum += s;
    return sum;
}

void Add(Bonus& to, Bonus const& b)
{
    to.apFlat += b.apFlat;
    to.rapFlat += b.rapFlat;
    to.apPct += b.apPct;
    to.rapPct += b.rapPct;
    for (uint8 s = 0; s < MAX_STATS; ++s)
        to.stat[s] += b.stat[s];
    to.statPct += b.statPct;
    to.sp += b.sp;
    to.spFromSpiritPct += b.spFromSpiritPct;
    to.meleeCrit += b.meleeCrit;
    to.spellCrit += b.spellCrit;
    to.meleeHaste += b.meleeHaste;
    to.rangedHaste += b.rangedHaste;
    to.spellHaste += b.spellHaste;
    to.physDone += b.physDone;
    to.magicDone += b.magicDone;
    to.manaPerSec += b.manaPerSec;
    to.armorCut += b.armorCut;
    to.physTaken += b.physTaken;
    to.magicTaken += b.magicTaken;
    to.critTaken += b.critTaken;
    to.spellHitTaken += b.spellHitTaken;
    to.spellCritTaken += b.spellCritTaken;
}

// the buffs the context can supply that it (or the target) does not wear now: the best one per category
// [dps20] `categories`: how many of them there were. A category the bot already wears is skipped below, so this
// counts the buffs the formula and the ideal assumed and the bot did not have - the measurement stores it, because
// a denominator raised by a buff nobody cast is a finding about the party and not a defect of the instrument.
Bonus PlannedBuffs(Player* bot, Unit* target, BuffContext ctx, uint8* categories = nullptr)
{
    std::vector<Player*> providers{bot};
    if (ctx == BuffContext::Group)
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    // [deaths9] a member present: alive within the group reward distance (one far away buffs no one)
                    if (member != bot && member->IsInWorld() && member->IsAlive() &&
                        member->IsAtGroupRewardDistance(bot) &&
                        (!group->isRaidGroup() || member->GetSubGroup() == bot->GetSubGroup()))
                        providers.push_back(member);

    std::array<float, size_t(Category::Count)> bestValue{};
    std::array<Bonus, size_t(Category::Count)> best{};
    std::array<bool, size_t(Category::Count)> worn{};
    for (BuffDef const& def : BuffDefs())
    {
        Unit* on = def.debuff ? target : static_cast<Unit*>(bot);
        if (on && WearsChain(on, def.spell, def.aura))
            worn[size_t(def.category)] = true;
    }

    for (BuffDef const& def : BuffDefs())
    {
        size_t const cat = size_t(def.category);
        if (worn[cat] || (def.debuff && !target))
            continue;
        SpellInfo const* info = nullptr;
        Unit* caster = bot;
        if (ctx == BuffContext::Raid && !def.self)
        {
            uint32 const rank = def.aura ? def.aura : RankAtLevel(def.spell, bot->GetLevel());
            info = rank ? sSpellMgr->GetSpellInfo(rank) : nullptr;
        }
        else if (!def.raidOnly)
            for (Player* provider : providers)
            {
                if (provider->getClass() != def.cls || (def.self && provider != bot))
                    continue;
                uint32 const rank = KnownRank(provider, def.spell);
                if (!rank)
                    continue;
                info = sSpellMgr->GetSpellInfo(def.aura ? def.aura : rank);
                caster = provider;
                break;
            }
        if (!info)
            continue;
        Bonus one;
        ReadBuff(AuraOf(info), caster, one);
        float const value = CategoryValue(one);
        if (value > bestValue[cat])
        {
            bestValue[cat] = value;
            best[cat] = one;
        }
    }
    Bonus total;
    for (Bonus const& one : best)
        Add(total, one);
    if (categories)
    {
        uint8 count = 0;
        for (size_t cat = 0; cat < bestValue.size(); ++cat)
            if (bestValue[cat] > 0.0f)
                ++count;
        *categories = count;
    }
    return total;
}

// ---- the model ----------------------------------------------------------------------------------------------

struct Swing
{
    float damage = 0.0f;  // expected per swing
    float landed = 0.0f;  // hits + crits + glancing
    float crit = 0.0f;
    float perSec = 0.0f;  // swings per second
    float rage = 0.0f;    // core rage units per swing
};

struct Model
{
    Player* bot = nullptr;
    Unit* target = nullptr;
    bool front = true;  // it stands before the target: parry counts (solo the mob faces it)
    Bonus bonus;
    float apGain = 0.0f, rapGain = 0.0f, spGain = 0.0f;
    float meleeCritGain = 0.0f, spellCritGain = 0.0f;
    float armorFactor = 1.0f;  // the planned armor debuffs' change of armor-reduced damage
    uint8 cp = MAX_COMBO;
    uint32 immolate = 0;  // its Immolate (Conflagrate reads it)
    // no target: a mob of its level stands in (creature_classlevelstats armor, the core's creature avoidance); the
    // bot is passed as the victim only where the victim's own numbers are not read
    bool standIn = false;
    float standInArmor = 0.0f;
    float pushHitsPerSec = 0.0f;  // [rot609] the target's landed melee hits on the bot per second (cast pushback)
};

// Unit::GetUnitMissChance: a creature's 5 %; Unit::MeleeSpellMissChance: + 19 % for a dual wielder's white swing
constexpr float CREATURE_MISS_PCT = 5.0f;
constexpr float DUAL_WIELD_MISS_PCT = 19.0f;
// Unit::GetUnitDodgeChance: a creature's 5 %
constexpr float CREATURE_DODGE_PCT = 5.0f;

float AttackerMissPct(Model const& m, Unit* attacker, WeaponAttackType att, uint32 spellId)
{
    if (m.standIn)
    {
        float miss = CREATURE_MISS_PCT;
        if (!spellId && att != RANGED_ATTACK && attacker->HasOffhandWeaponForAttack())
            miss += DUAL_WIELD_MISS_PCT;
        miss -= att == RANGED_ATTACK ? attacker->m_modRangedHitChance : attacker->m_modMeleeHitChance;
        return std::clamp(miss, 0.0f, MISS_MAX_PCT);
    }
    Unit* target = m.target;
    int32 const skillDiff =
        int32(attacker->GetWeaponSkillValue(att, target)) - int32(target->GetMaxSkillValueForLevel(attacker));
    return std::clamp(attacker->MeleeSpellMissChance(target, att, skillDiff, spellId), 0.0f, MISS_MAX_PCT);
}

// Unit::RollMeleeOutcomeAgainst: dodge and parry less expertise and the skill bonus (percent)
void Avoidance(Model const& m, Unit* attacker, WeaponAttackType att, float& dodge, float& parry)
{
    dodge = parry = 0.0f;
    if (att == RANGED_ATTACK)
        return;  // Unit::MeleeSpellHitResult: ranged attacks only miss
    float const expertisePct =
        attacker->IsPlayer() ? attacker->ToPlayer()->GetExpertiseDodgeOrParryReduction(att) : 0.0f;
    if (m.standIn)
    {
        dodge = std::max(0.0f, CREATURE_DODGE_PCT - expertisePct);
        return;
    }
    Unit* target = m.target;
    bool const front = m.front;
    float const skillBonus = SKILL_BONUS_PCT * float(int32(attacker->GetWeaponSkillValue(att, target)) -
                                                     int32(target->GetMaxSkillValueForLevel(attacker)));
    float const expertise = expertisePct;
    Creature* creature = target->ToCreature();
    bool const controlled = target->HasUnitState(UNIT_STATE_CONTROLLED);
    if (!controlled && (!creature || !creature->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_DODGE)))
        dodge = std::max(0.0f, target->GetUnitDodgeChance() - expertise - skillBonus);
    if (front && !controlled && (!creature || !creature->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_PARRY)))
        parry = std::max(0.0f, target->GetUnitParryChance() - expertise - skillBonus);
}

float ArmorReduced(Model const& m, Unit* attacker, float damage, SpellInfo const* info, WeaponAttackType att)
{
    if (damage <= 0.0f)
        return 0.0f;
    float reduced = 0.0f;
    if (m.standIn)
    {
        // Unit::CalcArmorReducedDamage against the stand-in's armor (no armor penetration counted)
        float level = float(attacker->GetLevel());
        if (level > 59.0f)
            level += 4.5f * (level - 59.0f);
        float share = 0.1f * m.standInArmor / (8.5f * level + 40.0f);
        share = std::clamp(share / (1.0f + share), 0.0f, 0.75f);
        reduced = damage * (1.0f - share);
    }
    else
        reduced = float(Unit::CalcArmorReducedDamage(attacker, m.target, uint32(damage), info, 0, att));
    if (attacker != m.bot || m.armorFactor >= 1.0f || reduced >= damage)
        return reduced;
    // armor x (1 - cut): the mitigated share a / (a + K) with a / K = m / (1 - m)
    float const mitigated = 1.0f - reduced / damage;
    float const x = mitigated / std::max(1.0f - mitigated, 0.01f) * m.armorFactor;
    return damage * (1.0f - x / (1.0f + x));
}

// a white swing of `attacker` (the bot or its pet) with its damage before the table
Swing WhiteSwing(Model const& m, Unit* attacker, WeaponAttackType att, float raw, float hasteGain)
{
    Swing out;
    uint32 const timeMs = attacker->GetAttackTime(att);
    if (!timeMs || raw <= 0.0f)
        return out;
    out.perSec = float(IN_MILLISECONDS) / float(timeMs) * (1.0f + hasteGain / 100.0f);

    float done = float(attacker->MeleeDamageBonusDone(m.target, uint32(raw), att, nullptr));
    if (!m.standIn)
        done = float(m.target->MeleeDamageBonusTaken(attacker, uint32(done), att, nullptr));
    if (attacker == m.bot)
        done *= (1.0f + m.bonus.physDone / 100.0f) * (1.0f + m.bonus.physTaken / 100.0f);
    done = ArmorReduced(m, attacker, done, nullptr, att);

    float const miss = AttackerMissPct(m, attacker, att, 0);
    float dodge = 0.0f, parry = 0.0f;
    Avoidance(m, attacker, att, dodge, parry);
    float glance = 0.0f, glanceMult = 1.0f;
    int32 const targetLevel = m.standIn ? int32(m.bot->GetLevel()) : int32(m.target->getLevelForTarget(attacker));
    if (att != RANGED_ATTACK && (attacker->IsPlayer() || attacker->IsPet()) && (m.standIn || !m.target->IsPlayer()) &&
        int32(attacker->GetLevel()) <= targetLevel)
    {
        // a creature's defense is 5 x its level (Unit::GetDefenseSkillValue)
        int32 const defense = m.standIn ? targetLevel * 5 : int32(m.target->GetDefenseSkillValue(attacker));
        int32 const skill =
            std::min<int32>(attacker->GetWeaponSkillValue(att, m.target), attacker->GetMaxSkillValueForLevel(m.target));
        glance = std::clamp(GLANCE_BASE_PCT + float(defense - skill) * GLANCE_PER_SKILL_PCT, 0.0f, GLANCE_MAX_PCT);
        // 1-10 % less against the same level and +1, up to 20-30 % against +3
        int32 const diff = std::clamp<int32>(targetLevel - int32(attacker->GetLevel()), 1, 3);
        float const hi = float(diff * 10);
        float const lo = std::max(1.0f, hi - 10.0f);
        glanceMult = 1.0f - (lo + hi) / 2.0f / 100.0f;
    }
    float crit = std::max(0.0f, attacker->GetUnitCriticalChance(att, m.target));
    if (attacker == m.bot)
        crit += m.meleeCritGain + m.bonus.critTaken;
    // [death445] Unit::RollMeleeOutcomeAgainst: a creature blocks from the front (Unit::GetUnitBlockChance 5 % plus
    // its block auras, less the skill bonus; not with CREATURE_FLAG_EXTRA_NO_BLOCK, controlled or casting), the hit
    // less its block value (Creature::GetShieldBlockValue: level / 2 + strength / 20) and never a crit
    float block = 0.0f;
    float blockValue = 0.0f;
    if (att != RANGED_ATTACK && !m.standIn && m.front && !m.target->HasUnitState(UNIT_STATE_CONTROLLED))
        if (Creature* creature = m.target->ToCreature();
            creature && !creature->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_BLOCK))
        {
            float const skillBonus = SKILL_BONUS_PCT * float(int32(attacker->GetWeaponSkillValue(att, m.target)) -
                                                             int32(m.target->GetMaxSkillValueForLevel(attacker)));
            block = std::max(0.0f, m.target->GetUnitBlockChance() - skillBonus);
            blockValue = float(m.target->GetShieldBlockValue());
        }
    float const before = miss + dodge + parry + block + glance;
    crit = std::clamp(crit, 0.0f, std::max(0.0f, 100.0f - before));
    float const hit = std::max(0.0f, 100.0f - before - crit);

    float critMult = WHITE_CRIT_MULT;
    if (att != RANGED_ATTACK)
        critMult *=
            1.0f +
            ((attacker->GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_CRIT_DAMAGE_BONUS, SPELL_SCHOOL_MASK_NORMAL) -
              1.0f) *
                 100.0f +
             float(attacker->GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_CRIT_PERCENT_VERSUS,
                                                            m.target->GetCreatureTypeMask()))) /
                100.0f;
    out.damage = (done * (hit + glance * glanceMult + crit * critMult) + std::max(0.0f, done - blockValue) * block) /
                 100.0f;
    out.landed = (hit + glance + crit + block) / 100.0f;
    out.crit = crit / 100.0f;

    // Unit::RewardRage on the landed hit: (damage / conversion x 7.5 + speed factor) / 2, the factor doubled on a
    // crit, at most twice the damage part
    if (attacker == m.bot && m.bot->HasActivePowerType(POWER_RAGE) && att != RANGED_ATTACK)
    {
        float const level = float(m.bot->GetLevel());
        float conversion = 0.0091107836f * level * level + 3.225598133f * level + 4.2652911f;
        if (level > 70.0f)
            conversion += 13.27f * (level - 70.0f);
        float const factor = float(m.bot->GetRageWeaponSpeedHitFactor(att));
        float const part = done / conversion * 7.5f;
        float const normal = std::min((part + factor) / 2.0f, part * 2.0f);
        float const critical = std::min((part * WHITE_CRIT_MULT + factor * 2.0f) / 2.0f, part * 4.0f);
        float rage = ((hit + glance + block) * normal + crit * critical) / 100.0f;
        AddPct(rage, float(m.bot->GetTotalAuraModifier(SPELL_AURA_MOD_RAGE_FROM_DAMAGE_DEALT)));
        out.rage = rage * sWorld->getRate(RATE_POWER_RAGE_INCOME) * RAGE_UNITS;
    }
    return out;
}

float GlobalCooldownMs(Player* modOwner, Unit* caster, SpellInfo const* info, float spellHaste)
{
    int32 gcd = int32(info->StartRecoveryTime);
    if (gcd < GCD_MIN_MS || gcd > GCD_MAX_MS)
        return float(std::max(gcd, 0));
    if (modOwner)
        modOwner->ApplySpellMod(info->Id, SPELLMOD_GLOBAL_COOLDOWN, gcd);
    float ms = float(gcd);
    if (info->StartRecoveryCategory == GCD_HASTED_CATEGORY && info->StartRecoveryTime == uint32(GCD_MAX_MS) &&
        info->DmgClass != SPELL_DAMAGE_CLASS_MELEE && info->DmgClass != SPELL_DAMAGE_CLASS_RANGED &&
        !info->HasAttribute(SPELL_ATTR0_USES_RANGED_SLOT) && !info->HasAttribute(SPELL_ATTR0_IS_ABILITY))
        ms = ms * caster->GetFloatValue(UNIT_MOD_CAST_SPEED) / (1.0f + spellHaste / 100.0f);
    return std::clamp(ms, float(GCD_MIN_MS), float(GCD_MAX_MS));
}

struct Cast
{
    uint32 id = 0;
    Kind kind = Kind::Filler;
    float damage = 0.0f;  // expected per cast, all parts (direct + every tick)
    float threat = 0.0f;
    float executeSec = 0.0f;  // cast / channel / global cooldown
    float periodSec = 0.0f;   // Dot / ComboDot: its duration; Cooldown: its cooldown
    float cost = 0.0f;        // in its power type's core units
    Powers power = POWER_MANA;
    float runes[RUNE_TYPES] = {};
    float runicGain = 0.0f;
    float comboGain = 0.0f;
    uint32 category = 0;
    bool mainHand = false;  // a main hand weapon strike (a seal procs on it)
    float landed = 1.0f;
    // [rot610] its parts after the land chance: the direct hits, the periodic damage of its full duration
    float direct = 0.0f;
    float periodic = 0.0f;
    float tickMs = 0.0f;      // between its ticks (0: no periodic part)
    float durationMs = 0.0f;  // its periodic part's duration
    float castMs = 0.0f;      // till it lands (its cast time with pushback; 0 for an instant)
    // [death457] its cast time before pushback, the delay one landed hit adds (0: never pushed back) and its global
    // cooldown: what PushStretch prices again at another rate of hits
    float baseCastMs = 0.0f;
    float pushDelayMs = 0.0f;
    float gcdMs = 0.0f;
    float firstSec = 0.0f;    // [dps232] a fresh fight: the earliest it can go (a reactive strike's first avoid)
};

// [rot609] Spell::Delayed: the landed hits within a cast of `castMs` at `hitsPerSec`, the cast stretched by their
// delays (at most PUSHBACK_MAX a cast)
float PushedHits(float hitsPerSec, float castMs, float delayMs)
{
    if (hitsPerSec <= 0.0f || castMs <= 0.0f)
        return 0.0f;
    float hits = std::min(PUSHBACK_MAX, hitsPerSec * castMs / float(IN_MILLISECONDS));
    return std::min(PUSHBACK_MAX, hitsPerSec * (castMs + hits * delayMs) / float(IN_MILLISECONDS));
}

float ThreatOf(Player* bot, SpellInfo const* info, float damage, float hits)
{
    float threat = damage;
    if (SpellThreatEntry const* entry = sSpellMgr->GetSpellThreatEntry(info->Id))
        threat = damage * entry->pctMod +
                 hits * (float(entry->flatMod) + entry->apPctMod * bot->GetTotalAttackPowerValue(BASE_ATTACK));
    return threat * bot->GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_THREAT, info->GetSchoolMask());
}

// spell power's share of a spell: SpellDamageBonusDone(0) is (coefficient x power) x the done mods
float PowerGain(Model const& m, Unit* caster, SpellInfo const* info, DamageEffectType type, uint8 effIndex)
{
    if (caster != m.bot)
        return 0.0f;
    float gain = 0.0f;
    if (m.spGain > 0.0f && (info->GetSchoolMask() & SPELL_SCHOOL_MASK_MAGIC))
    {
        float const sp = float(m.bot->SpellBaseDamageBonusDone(info->GetSchoolMask()));
        if (sp > 0.0f)
            gain += m.spGain * float(m.bot->SpellDamageBonusDone(m.target, info, 0, type, effIndex)) / sp;
        else if (SpellBonusEntry const* bonus = sSpellMgr->GetSpellBonusData(info->Id))
            gain += m.spGain * std::max(0.0f, type == DOT ? bonus->dot_damage : bonus->direct_damage);
    }
    if (SpellBonusEntry const* bonus = sSpellMgr->GetSpellBonusData(info->Id))
    {
        float const ap = info->DmgClass == SPELL_DAMAGE_CLASS_RANGED ? m.rapGain : m.apGain;
        gain += ap * std::max(0.0f, type == DOT ? bonus->ap_dot_bonus : bonus->ap_bonus);
    }
    return gain;
}

// expected value of a hit after crit
float WithCrit(Model const& m, Unit* caster, SpellInfo const* info, float damage, bool periodic, WeaponAttackType att)
{
    if (damage <= 0.0f || info->HasAttribute(SPELL_ATTR2_CANT_CRIT))
        return damage;
    if (periodic && !caster->HasAuraTypeWithAffectMask(SPELL_AURA_ABILITY_PERIODIC_CRIT, info))
        return damage;
    bool const magic = info->DmgClass == SPELL_DAMAGE_CLASS_MAGIC;
    float chance = caster->SpellDoneCritChance(m.target, info, info->GetSchoolMask(), att, periodic);
    if (caster == m.bot)
        chance += (magic ? m.spellCritGain + m.bonus.spellCritTaken : m.meleeCritGain) + m.bonus.critTaken;
    chance = std::clamp(chance, 0.0f, 100.0f);
    float const crit = float(Unit::SpellCriticalDamageBonus(caster, info, uint32(damage), m.target));
    return damage + chance / 100.0f * std::max(0.0f, crit - damage);
}

// chance a spell of `info` lands (miss, dodge, parry, average resist), 0-1
float LandChance(Model const& m, Unit* caster, SpellInfo const* info, WeaponAttackType att)
{
    if (info->HasAttribute(SPELL_ATTR3_ALWAYS_HIT) || info->HasAttribute(SPELL_ATTR7_NO_ATTACK_MISS))
        return 1.0f;
    if (info->DmgClass == SPELL_DAMAGE_CLASS_MELEE || info->DmgClass == SPELL_DAMAGE_CLASS_RANGED)
    {
        float const miss = AttackerMissPct(m, caster, att, info->Id);
        float dodge = 0.0f, parry = 0.0f;
        Avoidance(m, caster, att, dodge, parry);
        return std::clamp(1.0f - (miss + dodge + parry) / 100.0f, 0.0f, 1.0f);
    }
    // WorldObject::MagicSpellHitResult
    SpellSchoolMask const mask = info->GetSchoolMask();
    int32 const levelDiff =
        m.standIn ? 0 : int32(m.target->getLevelForTarget(caster)) - int32(caster->getLevelForTarget(m.target));
    float const multiplier = sWorld->getRate(m.target->IsPlayer() ? RATE_MISS_CHANCE_MULTIPLIER_TARGET_PLAYER
                                                                  : RATE_MISS_CHANCE_MULTIPLIER_TARGET_CREATURE);
    int32 hitPct = levelDiff < 3 ? 96 - levelDiff : 94 - int32(float(levelDiff - 2) * multiplier);
    if (Player* modOwner = caster->GetSpellModOwner())
        modOwner->ApplySpellMod(info->Id, SPELLMOD_RESIST_MISS_CHANCE, hitPct);
    hitPct += caster->GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_INCREASES_SPELL_PCT_TO_HIT, mask);
    if (!m.standIn)
        hitPct += m.target->GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_ATTACKER_SPELL_HIT_CHANCE, mask);
    float hit = float(hitPct) + caster->m_modSpellHitChance;
    if (caster == m.bot)
        hit += m.bonus.spellHitTaken;
    hit = std::clamp(hit, 1.0f, 100.0f) / 100.0f;
    if (!(mask & SPELL_SCHOOL_MASK_NORMAL) && !m.standIn)
        hit *= 1.0f - Unit::GetEffectiveResistChance(caster, mask, m.target, info);
    return hit;
}

float TakenAndArmor(Model const& m, Unit* caster, SpellInfo const* info, float damage, DamageEffectType type,
                    uint8 effIndex, WeaponAttackType att)
{
    damage = std::max(0.0f, damage);
    if (!m.standIn)
        damage = float(m.target->SpellDamageBonusTaken(caster, info, uint32(damage), type));
    if (caster == m.bot)
    {
        bool const magic = info->GetSchoolMask() & SPELL_SCHOOL_MASK_MAGIC;
        damage *= (1.0f + (magic ? m.bonus.magicDone : m.bonus.physDone) / 100.0f) *
                  (1.0f + (magic ? m.bonus.magicTaken : m.bonus.physTaken) / 100.0f);
    }
    if (Unit::IsDamageReducedByArmor(info->GetSchoolMask(), info, effIndex))
        damage = ArmorReduced(m, caster, damage, info, att);
    return damage;
}

float ChannelMs(Player* modOwner, Unit* caster, SpellInfo const* info, float spellHaste)
{
    int32 duration = info->GetDuration();
    if (duration <= 0)
        return 0.0f;
    if (modOwner)
        modOwner->ApplySpellMod(info->Id, SPELLMOD_DURATION, duration);
    float ms = float(duration);
    if (caster->HasAuraTypeWithAffectMask(SPELL_AURA_PERIODIC_HASTE, info) ||
        info->HasAttribute(SPELL_ATTR5_SPELL_HASTE_AFFECTS_PERIODIC))
        ms = ms * caster->GetFloatValue(UNIT_MOD_CAST_SPEED) / (1.0f + spellHaste / 100.0f);
    return ms;
}

float PeriodicTotal(Model const& m, Unit* caster, SpellInfo const* info, float& durationMs, float apTick,
                    float* tickMs = nullptr);

// one cast of `id` by `caster` against the target: its damage and its costs
bool ReadCast(Model const& m, Unit* caster, uint32 id, Ability const& ability, Cast& out, uint8 depth = 0)
{
    out = Cast();
    SpellInfo const* info = id ? sSpellMgr->GetSpellInfo(id) : nullptr;
    if (!info)
        return false;
    Player* modOwner = caster->GetSpellModOwner();
    Player* player = caster->ToPlayer();
    float const spellHaste = caster == m.bot ? m.bonus.spellHaste : 0.0f;
    out.id = id;
    out.kind = ability.kind;
    // Spell::Spell: the off hand for a spell that requires it (Mutilate's and Frost Strike's off-hand strikes)
    WeaponAttackType const att = (info->DmgClass == SPELL_DAMAGE_CLASS_RANGED || info->IsRangedWeaponSpell())
                                     ? RANGED_ATTACK
                                 : info->HasAttribute(SPELL_ATTR3_REQUIRES_OFF_HAND_WEAPON) ? OFF_ATTACK
                                                                                            : BASE_ATTACK;
    uint8 const cp = info->NeedsComboPoints() ? m.cp : 0;
    uint8 const nowCp = caster->GetComboPoints(m.target);
    float const ap = caster->GetTotalAttackPowerValue(att) +
                     (caster == m.bot ? (att == RANGED_ATTACK ? m.rapGain : m.apGain) : 0.0f);

    // weapon strikes (Spell::EffectWeaponDmg)
    float weaponPct = 100.0f;
    float fixed = 0.0f;
    bool weapon = false;
    bool normalized = false;
    for (SpellEffectInfo const& effect : info->GetEffects())
        switch (effect.Effect)
        {
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                weapon = true;
                fixed += float(effect.CalcValue(caster));
                break;
            case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                weapon = normalized = true;
                fixed += float(effect.CalcValue(caster));
                break;
            case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                weapon = true;
                weaponPct = weaponPct * float(effect.CalcValue(caster)) / 100.0f;
                break;
            case SPELL_EFFECT_ADD_COMBO_POINTS:
                out.comboGain += float(std::max(1, effect.CalcValue(caster)));
                break;
            default:
                break;
        }
    float direct = 0.0f;
    float hits = 0.0f;
    bool const physical = info->GetSchoolMask() & SPELL_SCHOOL_MASK_NORMAL;
    if (weapon && player)
    {
        float lo = 0.0f, hi = 0.0f;
        player->CalculateMinMaxDamage(att, normalized, physical, lo, hi, 0);
        float base = (lo + hi) / 2.0f;
        if (caster == m.bot)
            base +=
                (att == RANGED_ATTACK ? m.rapGain : m.apGain) / AP_PER_DPS * caster->GetAPMultiplier(att, normalized);
        UnitMods const mod = att == RANGED_ATTACK ? UNIT_MOD_DAMAGE_RANGED
                             : att == OFF_ATTACK  ? UNIT_MOD_DAMAGE_OFFHAND
                                                  : UNIT_MOD_DAMAGE_MAINHAND;
        if (physical)
            fixed *= caster->GetPctModifierValue(mod, TOTAL_PCT);
        float damage = (base + fixed) * weaponPct / 100.0f;
        damage = float(caster->MeleeDamageBonusDone(m.target, uint32(damage), att, info, info->GetSchoolMask()));
        if (!m.standIn)
            damage = float(m.target->MeleeDamageBonusTaken(caster, uint32(damage), att, info, info->GetSchoolMask()));
        if (caster == m.bot)
            damage *= (1.0f + m.bonus.physDone / 100.0f) * (1.0f + m.bonus.physTaken / 100.0f);
        if (Unit::IsDamageReducedByArmor(info->GetSchoolMask(), info))
            damage = ArmorReduced(m, caster, damage, info, att);
        direct += WithCrit(m, caster, info, damage, false, att);
        hits += 1.0f;
        out.mainHand = att == BASE_ATTACK;
    }

    float periodic = 0.0f;
    float durationMs = 0.0f;
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (effect.Effect != SPELL_EFFECT_SCHOOL_DAMAGE)
            continue;
        float base =
            float(std::max(effect.CalcValue(caster), 0)) + effect.PointsPerComboPoint * float(int32(cp) - int32(nowCp));
        switch (ability.script)
        {
            case Script::ApPctOfEffect:
                base = base * ap / 100.0f;
                break;
            case Script::Eviscerate:
                base += ap * float(cp) * EVISCERATE_AP_PER_CP;
                break;
            case Script::Envenom:
                base += ap * float(cp) * ENVENOM_AP_PER_CP;
                break;
            case Script::FerociousBite:
                base += ap * float(cp) * FEROCIOUS_BITE_AP_PER_CP;
                break;
            case Script::SteadyShot:
                if (player)
                {
                    float lo = 0.0f, hi = 0.0f;
                    player->CalculateMinMaxDamage(RANGED_ATTACK, true, true, lo, hi, 0);
                    base += (lo + hi) / 2.0f + ap * STEADY_SHOT_RAP;
                }
                break;
            case Script::Conflagrate:
                if (SpellInfo const* immolate = m.immolate ? sSpellMgr->GetSpellInfo(m.immolate) : nullptr)
                {
                    float ms = 0.0f;
                    base =
                        PeriodicTotal(m, caster, immolate, ms, 0.0f) *
                        float(info->Effects[EFFECT_1].CalcValue(caster) + info->Effects[EFFECT_2].CalcValue(caster)) /
                        100.0f;
                }
                break;
            default:
                break;
        }
        float damage =
            float(caster->SpellDamageBonusDone(m.target, info, uint32(base), SPELL_DIRECT_DAMAGE, effect.EffectIndex)) +
            PowerGain(m, caster, info, SPELL_DIRECT_DAMAGE, effect.EffectIndex);
        damage = TakenAndArmor(m, caster, info, damage, SPELL_DIRECT_DAMAGE, effect.EffectIndex, att);
        direct += WithCrit(m, caster, info, damage, false, att);
        hits += 1.0f;
    }

    float const apTick = ability.script == Script::Rupture ? ap * RUPTURE_AP_PER_TICK[cp] : 0.0f;
    float tickMs = 0.0f;
    periodic = PeriodicTotal(m, caster, info, durationMs, apTick, &tickMs);
    // Icy Touch / Plague Strike: the disease is a triggered spell
    if (periodic <= 0.0f)
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_TRIGGER_SPELL && effect.TriggerSpell)
                if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                    periodic += PeriodicTotal(m, caster, triggered, durationMs, 0.0f, &tickMs);

    float const land = LandChance(m, caster, info, att);
    out.landed = land;
    out.damage = (direct + periodic) * land;
    out.direct = direct * land;
    out.periodic = periodic * land;
    out.tickMs = tickMs;
    out.durationMs = durationMs;
    if (player)
        out.threat = ThreatOf(player, info, out.damage, hits * land);
    // Mutilate, Stormstrike: the strikes are triggered spells (one level deep)
    if (direct <= 0.0f && periodic <= 0.0f && !depth)
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            Cast strike;
            if (effect.Effect != SPELL_EFFECT_TRIGGER_SPELL || !effect.TriggerSpell ||
                !ReadCast(m, caster, effect.TriggerSpell, ability, strike, depth + 1))
                continue;
            out.damage += strike.damage;
            out.threat += strike.threat;
            out.mainHand = out.mainHand || strike.mainHand;
        }
    if (out.damage <= 0.0f)
        return false;

    // timing
    bool const channel = info->IsChanneled();
    float castMs = channel ? 0.0f : float(info->CalcCastTime(caster)) / (1.0f + spellHaste / 100.0f);
    // [rot609] Unit::DealDamage -> Spell::Delayed: every landed hit on the caster while it casts a push-back spell
    // delays it (b341 solo casters hit in melee: Wrath every 2.5 s for a 1.7 s cast, frost mages and demonology
    // warlocks at 0.5-0.6 of the formula when hit, at 1.0 when not)
    if (castMs > 0.0f && caster == m.bot && player && m.pushHitsPerSec > 0.0f &&
        (info->InterruptFlags & SPELL_INTERRUPT_FLAG_PUSH_BACK) && !info->HasAttribute(SPELL_ATTR6_NO_PUSHBACK))
    {
        int32 reduce = 100;
        player->ApplySpellMod(info->Id, SPELLMOD_NOT_LOSE_CASTING_TIME, reduce);
        reduce += player->GetTotalAuraModifier(SPELL_AURA_REDUCE_PUSHBACK) - 100;
        if (reduce < 100)
        {
            float const delayMs = PUSHBACK_MS * float(100 - reduce) / 100.0f;
            out.baseCastMs = castMs;
            out.pushDelayMs = delayMs;
            castMs += PushedHits(m.pushHitsPerSec, castMs, delayMs) * delayMs;
        }
    }
    out.castMs = castMs;
    float const channelMs = channel ? ChannelMs(modOwner, caster, info, spellHaste) : 0.0f;
    float const gcdMs = GlobalCooldownMs(modOwner, caster, info, spellHaste);
    out.gcdMs = gcdMs;
    out.executeSec = std::max({castMs, channelMs, gcdMs}) / float(IN_MILLISECONDS);
    // [rot609] a filler cast back to back refreshes its own DoT (Fireball): one execute time of its ticks per cast
    if (ability.kind == Kind::Filler && periodic > 0.0f && durationMs > out.executeSec * float(IN_MILLISECONDS))
    {
        float const lost = periodic * land * (1.0f - out.executeSec * float(IN_MILLISECONDS) / durationMs);
        out.threat *= (out.damage - lost) / out.damage;
        out.damage -= lost;
    }
    int32 cooldown = int32(std::max(info->RecoveryTime, info->CategoryRecoveryTime));
    if (modOwner)
        modOwner->ApplySpellMod(info->Id, SPELLMOD_COOLDOWN, cooldown);
    out.category = info->CategoryRecoveryTime ? info->GetCategory() : 0;
    if (ability.kind == Kind::Dot || ability.kind == Kind::ComboDot)
    {
        // [rot609] a debuff kept up (Thunder Clap: wowsims protection_warrior auraShouldRefresh) once per its aura's
        // duration, not on its cooldown
        float auraMs = 0.0f;
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.IsAura())
            {
                int32 ms = info->GetMaxDuration();
                if (modOwner && ms > 0)
                    modOwner->ApplySpellMod(info->Id, SPELLMOD_DURATION, ms);
                auraMs = std::max(auraMs, float(ms));
                break;
            }
        out.periodSec = std::max({durationMs, auraMs, float(cooldown)}) / float(IN_MILLISECONDS);
    }
    else
        out.periodSec = float(std::max(cooldown, 0)) / float(IN_MILLISECONDS);

    // costs
    out.power = Powers(info->PowerType);
    out.cost = float(std::max(0, info->CalcPowerCost(caster, info->GetSchoolMask())));
    if (info->RuneCostID)
        if (SpellRuneCostEntry const* runes = sSpellRuneCostStore.LookupEntry(info->RuneCostID))
        {
            for (uint8 r = 0; r < RUNE_TYPES; ++r)
                out.runes[r] = float(runes->RuneCost[r]);
            out.runicGain = float(runes->runePowerGain);
        }
    return true;
}

// a spell's periodic damage over its full duration (its own ticks, or a channel's triggered hits)
float PeriodicTotal(Model const& m, Unit* caster, SpellInfo const* info, float& durationMs, float apTick,
                    float* tickMs)
{
    Player* modOwner = caster->GetSpellModOwner();
    float const spellHaste = caster == m.bot ? m.bonus.spellHaste : 0.0f;
    bool const channel = info->IsChanneled();
    WeaponAttackType const att = info->DmgClass == SPELL_DAMAGE_CLASS_RANGED ? RANGED_ATTACK : BASE_ATTACK;
    float total = 0.0f;
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.Amplitude || (!effect.IsAura() && effect.Effect != SPELL_EFFECT_PERSISTENT_AREA_AURA))
            continue;
        float tick = 0.0f;
        if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE || effect.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH)
        {
            float const base = float(std::max(effect.CalcValue(caster), 0)) + apTick;
            tick = float(caster->SpellDamageBonusDone(m.target, info, uint32(base), DOT, effect.EffectIndex)) +
                   PowerGain(m, caster, info, DOT, effect.EffectIndex);
            tick = WithCrit(m, caster, info, TakenAndArmor(m, caster, info, tick, DOT, effect.EffectIndex, att), true,
                            att);
        }
        else if ((effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL ||
                  effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE) &&
                 effect.TriggerSpell)
        {
            SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell);
            if (!triggered)
                continue;
            for (SpellEffectInfo const& hit : triggered->GetEffects())
            {
                if (hit.Effect != SPELL_EFFECT_SCHOOL_DAMAGE)
                    continue;
                float const base = float(std::max(effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE
                                                      ? effect.CalcValue(caster)
                                                      : hit.CalcValue(caster),
                                                  0));
                float damage = float(caster->SpellDamageBonusDone(m.target, triggered, uint32(base),
                                                                  SPELL_DIRECT_DAMAGE, hit.EffectIndex)) +
                               PowerGain(m, caster, triggered, SPELL_DIRECT_DAMAGE, hit.EffectIndex);
                damage = TakenAndArmor(m, caster, triggered, damage, SPELL_DIRECT_DAMAGE, hit.EffectIndex, att);
                tick += WithCrit(m, caster, triggered, damage, false, att);
            }
        }
        if (tick <= 0.0f)
            continue;
        float duration = channel ? ChannelMs(modOwner, caster, info, spellHaste) : float(info->GetMaxDuration());
        if (!channel && duration > 0.0f && modOwner)
        {
            int32 ms = int32(duration);
            modOwner->ApplySpellMod(info->Id, SPELLMOD_DURATION, ms);
            duration = float(ms);
        }
        float amplitude = float(effect.Amplitude);
        if (channel && info->GetDuration() > 0)
            amplitude = amplitude * duration / float(info->GetDuration());
        if (duration <= 0.0f || amplitude <= 0.0f)
            continue;
        durationMs = std::max(durationMs, duration);
        if (tickMs)
            *tickMs = *tickMs > 0.0f ? std::min(*tickMs, amplitude) : amplitude;
        total += tick * std::floor(duration / amplitude);
    }
    return total;
}

// its spells by name, the highest rank it knows (one pass over its spell map)
std::unordered_map<std::string, uint32> KnownByName(Player* bot, SpecRotation const& rotation)
{
    std::unordered_map<std::string, uint32> known;
    for (Ability const& ability : rotation.abilities)
        known.emplace(ability.name, 0);
    known.emplace("Seal of Righteousness", 0);
    known.emplace("Life Tap", 0);
    known.emplace("Shoot", 0);
    for (auto const& [id, spell] : bot->GetSpellMap())
    {
        if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active || !spell->IsInSpec(bot->GetActiveSpec()))
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
        if (!info || info->IsPassive())
            continue;
        // [deaths9] a spell whose equipped-item requirement it cannot meet now is no part of its rotation (Spell::
        // CheckItems: SPELL_FAILED_EQUIPPED_ITEM_CLASS; a broken weapon is not usable, Player::GetUseableItemByPos).
        // b345 Ginde L5 (rogue, both weapons at 0 durability): Sinister Strike / Eviscerate need a melee weapon
        // (Spell.dbc EquippedItemClass 2), 0 casts in 21 fights, the formula read 7.0 dps, it dealt 1-3, died twice
        if (!bot->HasItemFitToSpellRequirements(info))
            continue;
        auto const found = known.find(info->SpellName[LOCALE_enUS]);
        if (found == known.end())
            continue;
        SpellInfo const* had = found->second ? sSpellMgr->GetSpellInfo(found->second) : nullptr;
        if (!had || sSpellMgr->GetSpellRank(id) > sSpellMgr->GetSpellRank(found->second) ||
            (sSpellMgr->GetSpellRank(id) == sSpellMgr->GetSpellRank(found->second) &&
             info->SpellLevel > had->SpellLevel))
            found->second = id;
    }
    return known;
}

// [dpscal] The ideal: the wowsims / Rawr priority for the spec (ui/<spec>/apls, Rawr 2.3.23) where the bot's own
// strategy casts less (the specs Rotations() trims to the bot's casts); the strongest filler it knows is cast (Compute)
// and a DoT where it pays by the core's numbers. Logged as ideal= beside the formula: a gap above 10 % names a rotation
// the bot should fix. No decision reads it.
std::vector<SpecRotation> const& IdealRotations()
{
    static std::vector<SpecRotation> const rotations = {
        {CLASS_PRIEST,
         0,
         true,
         {{"Shadow Word: Pain", Kind::Dot},
          {"Holy Fire", Kind::Cooldown},
          {"Mind Blast", Kind::Cooldown},
          {"Smite", Kind::Filler}}},
        {CLASS_PRIEST,
         1,
         true,
         {{"Shadow Word: Pain", Kind::Dot},
          {"Holy Fire", Kind::Cooldown},
          {"Mind Blast", Kind::Cooldown},
          {"Smite", Kind::Filler}}},
        {CLASS_MAGE,
         0,
         true,
         {{"Arcane Barrage", Kind::Cooldown},
          {"Arcane Blast", Kind::Filler},
          {"Arcane Missiles", Kind::Filler},
          {"Fireball", Kind::Filler},
          {"Frostbolt", Kind::Filler}}},
        {CLASS_MAGE,
         1,
         true,
         {{"Living Bomb", Kind::Dot},
          {"Fire Blast", Kind::PaidCooldown},
          {"Fireball", Kind::Filler},
          {"Frostbolt", Kind::Filler}}},
        {CLASS_MAGE,
         2,
         true,
         {{"Fire Blast", Kind::PaidCooldown}, {"Frostbolt", Kind::Filler}, {"Fireball", Kind::Filler}}},
        {CLASS_WARLOCK,
         0,
         true,
         {{"Haunt", Kind::Cooldown},
          {"Unstable Affliction", Kind::Dot},
          {"Corruption", Kind::Dot},
          {"Curse of Agony", Kind::Dot},
          {"Immolate", Kind::Dot},
          {"Shadow Bolt", Kind::Filler}}},
        {CLASS_WARLOCK,
         1,
         true,
         {{"Corruption", Kind::Dot},
          {"Immolate", Kind::Dot},
          {"Curse of Agony", Kind::Dot},
          {"Incinerate", Kind::Filler},
          {"Shadow Bolt", Kind::Filler}}},
        {CLASS_WARLOCK,
         2,
         true,
         {{"Immolate", Kind::Dot},
          {"Conflagrate", Kind::Cooldown, Script::Conflagrate},
          {"Chaos Bolt", Kind::Cooldown},
          {"Corruption", Kind::Dot},
          {"Incinerate", Kind::Filler},
          {"Shadow Bolt", Kind::Filler}}},
    };
    return rotations;
}

SpecRotation const* RotationOf(Player* bot, bool ideal = false, Unit* target = nullptr)
{
    uint8 const cls = bot->getClass();
    uint8 const tab = AiFactory::GetPlayerSpecTab(bot);
    if (HunterInMelee(bot, target))
        return &HunterMeleeRotation();  // [rot605] the ideal too: no pet takes the mob off it
    if (ideal)
        for (SpecRotation const& rotation : IdealRotations())
            if (rotation.cls == cls && (rotation.tab == 255 || rotation.tab == tab))
                return &rotation;
    if (cls == CLASS_DRUID && tab == 1)
    {
        ShapeshiftForm const form = bot->GetShapeshiftForm();
        if (form == FORM_BEAR || form == FORM_DIREBEAR || (form != FORM_CAT && PlayerbotAI::IsTank(bot)))
            return &BearRotation();
    }
    for (SpecRotation const& rotation : Rotations())
        if (rotation.cls == cls && (rotation.tab == 255 || rotation.tab == tab))
            return &rotation;
    return nullptr;
}

struct Profile
{
    float burst = 0.0f;
    float white = 0.0f;
    float pet = 0.0f;
    float tps = 0.0f;
    float manaSpend = 0.0f;  // per second
    float manaRegen = 0.0f;  // per second, in combat
    bool lifeTap = false;
    uint8 assumedBuffs = 0;  // [dps20] buff categories the denominator assumed and the bot did not wear
    float cycleSec = 0.0f;
    uint8 spells = 0;
    float lifeSec = 0.0f;                          // [dpscal] the fight it was computed for (0: the steady state)
    uint32 fillerId = 0;                           // [rot610] the filler it casts
    float altPerRage = 0.0f;    // [rot610] the best damage per core rage unit of its rotation's other rage spells
    float altPerEnergy = 0.0f;  // [rot610] ... energy spells (a builder-finisher cycle per its energy)
    std::vector<std::pair<uint32, float>> parts;  // [dpscal] damage per second by rotation spell ([dpsparts] log)
    float replacedWhite = 0.0f;  // [rot611] the swings a next-swing strike replaces (in `white`, logged in its part)
    // [dps60] the same swings in hits a second, taken out of `whiteHitsPerSec` below. The swing timer produced them;
    // a next-swing strike spent them, so they land as that strike and not as a white hit. [meleeup] adds them back
    // to price the timer's own output, which is what the able time allows - and the gap between the strikes the
    // rotation planned and the ones the bot cast is then the rotation's, read off one line instead of guessed.
    float replacedHitsPerSec = 0.0f;
    float whiteHitsPerSec = 0.0f;  // [dps238] its landed melee swings a second ([dpsparts] log only)
    // [dps50] the part of the fight `whiteHitsPerSec` has it in melee reach (1 for every rotation but the petless
    // hunter's, who shoots until the mob arrives): the [meleeup] allowance prices only the able time, which already
    // holds no out-of-reach second, so it reads the rate without this share or it discounts the arrival twice
    float meleeShare = 1.0f;
    // [lead1309] the threat's rage terms (ThreatWithin): its white threat per second, the rage its swings bring and
    // its rotation spends (core units per second), each swing's rage
    float whiteTps = 0.0f;
    float rageSpend = 0.0f;
    float ragePerSwing = 0.0f;
    float offRagePerSwing = 0.0f;
    // [death457] the pushed-back casts of its rotation (casts per second, base cast, delay a hit, global cooldown), the
    // hits a second it was computed at and its time-bound damage (the weapon's and the pet's are not its casts')
    struct PushCast
    {
        float rate = 0.0f;
        float baseCastMs = 0.0f;
        float delayMs = 0.0f;
        float gcdMs = 0.0f;
    };
    std::vector<PushCast> pushCasts;
    float pushHitsPerSec = 0.0f;
    float rotationWhite = 0.0f;
};

// [rot606] PetAI::_canMeleeAttack: an imp or a water elemental with the mana for its bolt chases to the bolt's range
// and never swings (b338: the formula gave the L1-12 warlocks' imps a melee swing too)
bool BoltCaster(Pet* pet)
{
    switch (pet->GetEntry())
    {
        case ENTRY_IMP:
        case ENTRY_WATER_ELEMENTAL:
        case ENTRY_WATER_ELEMENTAL_PERM:
            break;
        default:
            return false;
    }
    for (uint8 i = 0; i < pet->GetPetAutoSpellSize(); ++i)
    {
        uint32 const id = pet->GetPetAutoSpellOnPos(i);
        switch (id)
        {
            case IMP_FIREBOLT_RANK_1:
            case IMP_FIREBOLT_RANK_2:
            case IMP_FIREBOLT_RANK_3:
            case IMP_FIREBOLT_RANK_4:
            case IMP_FIREBOLT_RANK_5:
            case IMP_FIREBOLT_RANK_6:
            case IMP_FIREBOLT_RANK_7:
            case IMP_FIREBOLT_RANK_8:
            case IMP_FIREBOLT_RANK_9:
            case WATER_ELEMENTAL_WATERBOLT_1:
            case WATER_ELEMENTAL_WATERBOLT_2:
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(id))
                    if (int32(pet->GetPower(POWER_MANA)) >= info->CalcPowerCost(pet, info->GetSchoolMask()))
                        return true;
                break;
            default:
                break;
        }
    }
    return false;
}

// [rot607] A solo bot's fresh mob: its melee pet runs to it first, from beside its owner at its run speed (the mob
// standing; it comes on once hit, so this is the longest run). A bolt caster casts from where it stands: its first bolt
// is under way with the owner's first cast, which the [dps] window's lead covers.
float PetReachSec(Pet* pet, Unit* target)
{
    if (!pet || !target || BoltCaster(pet))
        return 0.0f;
    float const speed = pet->GetSpeed(MOVE_RUN);
    if (speed <= 0.0f)
        return 0.0f;
    return std::max(0.0f, pet->GetExactDist(target) - pet->GetMeleeRange(target)) / speed;
}

// the pet's white damage and its autocast spells on cooldown (focus-limited for a hunter's)
float PetDps(Model const& m, Pet* pet)
{
    if (!pet || !pet->IsAlive())
        return 0.0f;
    float const raw = (pet->GetFloatValue(UNIT_FIELD_MINDAMAGE) + pet->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f;
    Model pm = m;
    pm.front = false;  // a pet attacks from where it stands; the mob faces its owner or the pet itself
    Swing const swing = WhiteSwing(pm, pet, BASE_ATTACK, raw, 0.0f);
    float dps = BoltCaster(pet) ? 0.0f : swing.damage * swing.perSec;
    float time = 1.0f;
    float focus = pet->getPowerType() == POWER_FOCUS ? PET_FOCUS_REGEN : std::numeric_limits<float>::max();
    for (uint8 i = 0; i < pet->GetPetAutoSpellSize(); ++i)
    {
        Cast cast;
        Ability const ability{"", Kind::Cooldown};
        if (!ReadCast(pm, pet, pet->GetPetAutoSpellOnPos(i), ability, cast))
            continue;
        float rate = cast.periodSec > 0.0f ? 1.0f / cast.periodSec : 1.0f / std::max(cast.executeSec, 0.1f);
        if (cast.executeSec > 0.0f)
            rate = std::min(rate, time / cast.executeSec);
        if (cast.power == POWER_FOCUS && cast.cost > 0.0f)
            rate = std::min(rate, focus / cast.cost);
        time -= rate * cast.executeSec;
        if (cast.power == POWER_FOCUS)
            focus -= rate * cast.cost;
        dps += rate * cast.damage;
    }
    return dps;
}

// [dpscal] A DoT over a fight of `lifeSec` (a solo bot's fresh mob: its health over this formula, Compute below),
// cast as the bot's DoT triggers cast it (DebuffTrigger::DotPaysOff, SpellPayoff::DotPays): every application that
// runs its full duration, and the last one when the ticks that land before the death beat the filler in its execute
// time. `cast.damage` becomes the mean per application; returns the applications per second (0: none pays). b337:
// warlocks measured 0.44-0.66 of a formula that kept Immolate / Corruption up on mobs dying in 8-13 s, where the bot
// holds them ("[rot400] Idunk holds corruption on Wendigo: lives 8.5 s").
float DotRateOver(Player* bot, Unit* target, Cast& cast, uint32 fillerId, float lifeSec)
{
    SpellPayoff::Cast dot;
    if (cast.periodSec <= 0.0f || lifeSec <= 0.0f || !SpellPayoff::Read(bot, target, cast.id, dot) ||
        dot.Total() <= 0.0f)
        return 0.0f;
    float const whole = std::floor(lifeSec / cast.periodSec);
    float const restMs = (lifeSec - whole * cast.periodSec) * float(IN_MILLISECONDS);
    float applications = whole;
    float damage = whole * cast.damage;
    // the last application: its direct hit once it lands, the ticks before the death
    float const afterLandMs = restMs - dot.castMs;
    if (afterLandMs >= 0.0f)
    {
        float ticks = float(dot.ticks);
        if (dot.amplitudeMs)
            ticks = std::min(ticks, std::floor(afterLandMs / float(dot.amplitudeMs)));
        float const landed = dot.direct + dot.tick * ticks;
        float const filler =
            fillerId ? SpellPayoff::FillerDps(bot, target, fillerId) * dot.executeMs / float(IN_MILLISECONDS) : 0.0f;
        if (landed > 0.0f && landed >= filler)
        {
            applications += 1.0f;
            damage += cast.damage * landed / dot.Total();
        }
    }
    if (applications <= 0.0f)
        return 0.0f;
    cast.damage = damage / applications;
    return applications / lifeSec;
}

// [rot609] The target's melee hits that land on the bot per second, for the pushback of its casts (ReadCast): when the
// target attacks it (solo, a fresh mob comes at the one that pulls it; a pet or a tank holding it: none). Its attack
// speed and its miss / dodge / parry against the bot (Unit::RollMeleeOutcomeAgainst); over a fight of `lifeSec` from
// the time it has run into melee at its run speed.
// [rot611] the time a mob that comes at the bot takes to reach its melee range, at its run speed (0: there)
float MeleeArrivalSec(Unit* mob, Unit* bot)
{
    float const speed = mob->GetSpeed(MOVE_RUN);
    return speed > 0.0f ? std::max(0.0f, mob->GetExactDist(bot) - mob->GetMeleeRange(bot)) / speed : 0.0f;
}

// [death457] GenericDruidStrategy: a druid that is no bear tank casts Nature's Grasp (a global cooldown, its mana)
// whenever it is attacked and the spell is ready - a cast of its fight the rotation pays in time. Its highest rank
// when it is due now (known, off its cooldown, not worn; in caster or moonkin form, where it is cast)
constexpr uint32 SPELL_NATURES_GRASP_RANK_1 = 16689;
SpellInfo const* NaturesGraspDue(Player* bot)
{
    if (bot->getClass() != CLASS_DRUID || PlayerbotAI::IsTank(bot) ||
        (bot->GetShapeshiftForm() != FORM_NONE && bot->GetShapeshiftForm() != FORM_MOONKIN))
        return nullptr;
    SpellInfo const* best = nullptr;
    for (SpellInfo const* rank = sSpellMgr->GetSpellInfo(SPELL_NATURES_GRASP_RANK_1); rank;
         rank = rank->GetNextRankSpell())
        if (bot->HasSpell(rank->Id))
            best = rank;
    if (!best || bot->HasSpellCooldown(best->Id) || bot->HasAura(best->Id))
        return nullptr;
    return best;
}

float PushbackHitsPerSec(Player* bot, Unit* target, BuffContext ctx, float lifeSec)
{
    if (!target || target == bot || !target->CanHaveThreatList())
        return 0.0f;
    Unit* victim = target->GetVictim();
    if (victim ? victim != bot : ctx != BuffContext::Solo)
        return 0.0f;
    uint32 const attackMs = target->GetAttackTime(BASE_ATTACK);
    if (!attackMs)
        return 0.0f;
    int32 const skillDiff =
        int32(target->GetWeaponSkillValue(BASE_ATTACK, bot)) - int32(bot->Unit::GetMaxSkillValueForLevel(target));
    float const miss = std::clamp(target->MeleeSpellMissChance(bot, BASE_ATTACK, skillDiff, 0), 0.0f, MISS_MAX_PCT);
    float const avoid = std::clamp(miss + bot->GetUnitDodgeChance() + bot->GetUnitParryChance(), 0.0f, 100.0f);
    float rate = (1.0f - avoid / 100.0f) * float(IN_MILLISECONDS) / float(attackMs);
    if (lifeSec > 0.0f)
        rate *= std::max(0.0f, lifeSec - MeleeArrivalSec(target, bot)) / lifeSec;
    return rate;
}

// [dps232] Revenge (CasterAuraState AURA_STATE_DEFENSE) goes only within REACTIVE_TIMER_START (5 s) of a dodge,
// parry or block of the bot's (Unit::ProcDamageAndSpellFor; a rogue's dodge and a hunter's parry set other states):
// the avoids per second of the target's swings on it (Unit::RollMeleeOutcomeAgainst: its chances less 0.04 % per
// weapon skill point of the attacker over the bot's level), the target attacking it (a fresh solo mob or a tank's
// target too). No target: a mob of its level on its base attack time. 0: it never goes. The formula had it on its
// cooldown: b426-b429 L16-20 protection 1.2-1.9 Revenges a 18-26 s fight against a formula that cast one every 5 s
// (Revenge 19-31 dps of a 42-57 formula, the spec at 0.65 of it); wowsims procs it on the avoids, Rawr's 0.73 Revenge
// a 6 s cycle is a raid boss on a level 80 tank's ~50 % avoidance
float DefenseProcsPerSec(Player* bot, Unit* target, BuffContext ctx)
{
    uint32 attackMs = BASE_ATTACK_TIME;
    float shift = 0.0f;
    if (target && target != bot)
    {
        if (!target->CanHaveThreatList())
            return 0.0f;
        Unit* victim = target->GetVictim();
        if (victim ? victim != bot : ctx != BuffContext::Solo && !PlayerbotAI::IsTank(bot))
            return 0.0f;
        attackMs = target->GetAttackTime(BASE_ATTACK);
        if (!attackMs)
            return 0.0f;
        int32 const skillDiff =
            int32(target->GetWeaponSkillValue(BASE_ATTACK, bot)) - int32(bot->Unit::GetMaxSkillValueForLevel(target));
        shift = float(skillDiff) * SKILL_BONUS_PCT;
    }
    float pct = std::max(0.0f, bot->GetUnitBlockChance() - shift);
    if (bot->getClass() != CLASS_ROGUE)
        pct += std::max(0.0f, bot->GetUnitDodgeChance() - shift);
    if (bot->getClass() != CLASS_HUNTER)
        pct += std::max(0.0f, bot->GetUnitParryChance() - shift);
    return std::clamp(pct, 0.0f, 100.0f) / 100.0f * float(IN_MILLISECONDS) / float(attackMs);
}

// `lifeSec` > 0: the fight's length (a solo bot on a fresh mob); 0: the steady state
Profile ComputeAt(Player* bot, Unit* target, BuffContext ctx, float lifeSec, bool ideal)
{
    Profile p;
    p.lifeSec = lifeSec;
    Model m;
    m.bot = bot;
    m.front = ctx == BuffContext::Solo || PlayerbotAI::IsTank(bot);
    m.bonus = PlannedBuffs(bot, target, ctx, &p.assumedBuffs);  // [dps20] ... and how many of them it did not wear
    if (!target)
    {
        m.standIn = true;
        target = bot;
        if (CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(bot->GetLevel(), CLASS_WARRIOR))
            m.standInArmor = stats->BaseArmor;
    }
    m.target = target;
    if (!m.standIn)
        m.pushHitsPerSec = PushbackHitsPerSec(bot, target, ctx, lifeSec);

    // the planned buffs' stats into attack power, spell power and crit (Player::UpdateAttackPowerAndDamage)
    uint8 const cls = bot->getClass();
    ShapeshiftForm const form = bot->GetShapeshiftForm();
    float stat[MAX_STATS];
    for (uint8 s = STAT_STRENGTH; s < MAX_STATS; ++s)
        stat[s] = m.bonus.stat[s] + bot->GetStat(Stats(s)) * m.bonus.statPct / 100.0f;
    float apPerStr = 1.0f, apPerAgi = 0.0f;
    if (cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_DEATH_KNIGHT ||
        (cls == CLASS_DRUID && form != FORM_NONE))
        apPerStr = 2.0f;
    if (cls == CLASS_HUNTER || cls == CLASS_SHAMAN || cls == CLASS_ROGUE || (cls == CLASS_DRUID && form == FORM_CAT))
        apPerAgi = 1.0f;
    float const apMult = 1.0f + bot->GetFloatValue(UNIT_FIELD_ATTACK_POWER_MULTIPLIER);
    float const rapMult = 1.0f + bot->GetFloatValue(UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER);
    m.apGain = (stat[STAT_STRENGTH] * apPerStr + stat[STAT_AGILITY] * apPerAgi + m.bonus.apFlat) * apMult +
               bot->GetTotalAttackPowerValue(BASE_ATTACK) * m.bonus.apPct / 100.0f;
    float const rapPerAgi = cls == CLASS_HUNTER || cls == CLASS_ROGUE || cls == CLASS_WARRIOR ? 1.0f : 0.0f;
    m.rapGain = (stat[STAT_AGILITY] * rapPerAgi + m.bonus.rapFlat) * rapMult +
                bot->GetTotalAttackPowerValue(RANGED_ATTACK) * m.bonus.rapPct / 100.0f;
    float const agi = std::max(1.0f, bot->GetStat(STAT_AGILITY));
    float const intellect = std::max(1.0f, bot->GetStat(STAT_INTELLECT));
    m.meleeCritGain = m.bonus.meleeCrit + stat[STAT_AGILITY] * bot->GetMeleeCritFromAgility() / agi;
    m.spellCritGain = m.bonus.spellCrit + stat[STAT_INTELLECT] * bot->GetSpellCritFromIntellect() / intellect;
    m.spGain = m.bonus.sp + (bot->GetStat(STAT_SPIRIT) + stat[STAT_SPIRIT]) * m.bonus.spFromSpiritPct / 100.0f;
    m.armorFactor = std::max(0.0f, 1.0f - m.bonus.armorCut / 100.0f);

    SpecRotation const* rotation = RotationOf(bot, ideal, target);
    std::unordered_map<std::string, uint32> known;
    if (rotation)
        known = KnownByName(bot, *rotation);
    bool const caster = rotation && rotation->caster;
    // [rot602] Immolate and Unstable Affliction do not stack (Rawr 2.3.23 CalculationOptionsWarlock): the warlock with
    // Unstable Affliction keeps that one
    if (rotation && cls == CLASS_WARLOCK && known["Unstable Affliction"])
        known["Immolate"] = 0;
    if (rotation && cls == CLASS_WARLOCK)
        m.immolate = known["Immolate"];

    // white damage
    Swing mh, oh, ranged;
    {
        float lo = 0.0f, hi = 0.0f;
        bot->CalculateMinMaxDamage(BASE_ATTACK, false, true, lo, hi, 0);
        float const raw = (lo + hi) / 2.0f + m.apGain / AP_PER_DPS * bot->GetAPMultiplier(BASE_ATTACK, false);
        mh = WhiteSwing(m, bot, BASE_ATTACK, raw, m.bonus.meleeHaste);
        if (bot->HasOffhandWeaponForAttack())
        {
            bot->CalculateMinMaxDamage(OFF_ATTACK, false, true, lo, hi, 0);
            oh = WhiteSwing(m, bot, OFF_ATTACK,
                            (lo + hi) / 2.0f + m.apGain / AP_PER_DPS * bot->GetAPMultiplier(OFF_ATTACK, false) / 2.0f,
                            m.bonus.meleeHaste);
        }
        if (bot->GetWeaponForAttack(RANGED_ATTACK, true))
        {
            bot->CalculateMinMaxDamage(RANGED_ATTACK, false, true, lo, hi, 0);
            ranged = WhiteSwing(m, bot, RANGED_ATTACK,
                                (lo + hi) / 2.0f + m.rapGain / AP_PER_DPS * bot->GetAPMultiplier(RANGED_ATTACK, false),
                                m.bonus.rangedHaste);
            // a wand's school damage takes no armor: its hit again without the armor
            if (Item* wand = bot->GetWeaponForAttack(RANGED_ATTACK, true))
                if (wand->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_WAND)
                {
                    float const done =
                        float(bot->MeleeDamageBonusDone(target, uint32((lo + hi) / 2.0f), RANGED_ATTACK));
                    ranged.damage = done * (ranged.landed + ranged.crit);  // a crit doubles it
                }
        }
    }
    float const meleeWhite = mh.damage * mh.perSec + oh.damage * oh.perSec;
    float const rangedWhite = ranged.damage * ranged.perSec;
    bool const hunter = cls == CLASS_HUNTER;
    bool const meleeHunter = rotation == &HunterMeleeRotation();  // [rot605]
    float rotationWhite = caster ? 0.0f : (hunter && !meleeHunter ? rangedWhite : meleeWhite);
    // [rot611] the petless hunter's fresh mob runs at it: Auto Shot until it arrives, its weapon from then (b341
    // Dekorhan L8: 4.3-5.8 s of each 16-24 s fight at range, the formula had its melee swings the whole fight)
    float meleeShare = 1.0f;
    if (meleeHunter && lifeSec > 0.0f && !m.standIn)
    {
        meleeShare = std::max(0.0f, lifeSec - MeleeArrivalSec(target, bot)) / lifeSec;
        rotationWhite = rangedWhite * (1.0f - meleeShare) + meleeWhite * meleeShare;
    }
    float whiteTps =
        rotationWhite * bot->GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_THREAT, SPELL_SCHOOL_MASK_NORMAL);
    p.white = caster ? std::max(meleeWhite, rangedWhite) : rotationWhite;
    if (!caster && (!hunter || meleeHunter))
        p.whiteHitsPerSec = (mh.perSec * mh.landed + oh.perSec * oh.landed) * meleeShare;  // [dps238]
    p.meleeShare = meleeShare;  // [dps50]

    // budgets per second (core power units)
    float time = 1.0f;
    float rage = caster || hunter ? 0.0f : mh.rage * mh.perSec + oh.rage * oh.perSec;
    float const rageIncome = rage;  // [lead1309]
    float energy =
        (ENERGY_BASE_REGEN + bot->GetFloatValue(uint16(UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER) + uint16(POWER_ENERGY))) *
        sWorld->getRate(RATE_POWER_ENERGY);
    // [rot609] a fresh solo mob: it comes to the fight with full energy (Player::Regenerate fills it out of combat),
    // spent on top of the regeneration over the fight (b341 L10-19 assassination: 188 energy cast a ~10 s fight
    // against the formula's 100, 1.13 of it)
    // [deaths12] ... with the energy it has now: a pull right after a fight starts with what that fight left
    // (Player::Regenerate fills it only out of combat, 10 a second): b350 assassination 10-19 fights of 10 s and more
    // dealt 0.76 of the formula's specials (one source of it), Sycina L11's pulls among them. The ideal keeps the
    // full bar
    if (lifeSec > 0.0f && bot->getPowerType() == POWER_ENERGY)
        energy += float(ideal ? bot->GetMaxPower(POWER_ENERGY) : bot->GetPower(POWER_ENERGY)) / lifeSec;
    float runes[RUNE_TYPES] = {};
    if (cls == CLASS_DEATH_KNIGHT)
        for (uint8 i = 0; i < RUNE_SLOTS; ++i)
        {
            // Spell::CheckRuneCost: RuneCost[0] blood, [1] frost, [2] unholy
            uint32 const cd = bot->GetRuneBaseCooldown(i, true);
            RuneType const type = bot->GetBaseRune(i);
            uint8 const index = type == RUNE_BLOOD ? 0 : type == RUNE_FROST ? 1 : 2;
            if (cd)
                runes[index] += float(IN_MILLISECONDS) / float(cd);
        }
    float runic = 0.0f;
    std::unordered_map<uint32, float> categoryUsed;  // shared cooldowns (shocks): uses per second

    float damage = rotationWhite;
    float threat = whiteTps;
    float mana = 0.0f;
    float cycle = 0.0f;
    float const swingsLeft = mh.perSec * meleeShare;
    float strikes = 0.0f;  // landed main hand weapon strikes per second

    auto const affordable = [&](Cast const& cast, float rate) -> float
    {
        if (cast.executeSec > 0.0f && cast.kind != Kind::Swing)
            rate = std::min(rate, time / cast.executeSec);
        if (cast.cost > 0.0f)
        {
            if (cast.power == POWER_RAGE)
                rate = std::min(rate, rage / cast.cost);
            else if (cast.power == POWER_ENERGY)
                rate = std::min(rate, energy / cast.cost);
            else if (cast.power == POWER_RUNIC_POWER)
                rate = std::min(rate, runic / cast.cost);
        }
        for (uint8 r = 0; r < RUNE_TYPES; ++r)
            if (cast.runes[r] > 0.0f)
                rate = std::min(rate, runes[r] / cast.runes[r]);
        if (cast.category)
        {
            float const used = categoryUsed[cast.category];
            if (cast.periodSec > 0.0f)
                rate = std::min(rate, std::max(0.0f, 1.0f / cast.periodSec - used));
        }
        return std::max(0.0f, rate);
    };
    auto const spend = [&](Cast const& cast, float rate)
    {
        if (cast.cost > 0.0f && cast.kind != Kind::Dot && cast.kind != Kind::ComboDot)
        {
            if (cast.power == POWER_RAGE)
                p.altPerRage = std::max(p.altPerRage, cast.damage / cast.cost);
            else if (cast.power == POWER_ENERGY)
                p.altPerEnergy = std::max(p.altPerEnergy, cast.damage / cast.cost);
        }
        if (rate <= 0.0f)
            return;
        if (cast.kind != Kind::Swing)
            time -= rate * cast.executeSec;
        if (cast.power == POWER_RAGE)
            rage -= rate * cast.cost;
        else if (cast.power == POWER_ENERGY)
            energy -= rate * cast.cost;
        else if (cast.power == POWER_RUNIC_POWER)
            runic -= rate * cast.cost;
        else if (cast.power == POWER_MANA)
            mana += rate * cast.cost;
        for (uint8 r = 0; r < RUNE_TYPES; ++r)
            runes[r] -= rate * cast.runes[r];
        runic += rate * cast.runicGain;
        if (cast.category)
            categoryUsed[cast.category] += rate;
        damage += rate * cast.damage;
        threat += rate * cast.threat;
        if (cast.mainHand)
            strikes += rate * cast.landed;
        cycle += cast.executeSec;
        if (cast.pushDelayMs > 0.0f)  // [death457]
            p.pushCasts.push_back({rate, cast.baseCastMs, cast.pushDelayMs, cast.gcdMs});
        ++p.spells;
        if (cast.id)
            p.parts.emplace_back(cast.id, rate * cast.damage);
    };

    // [death457] Nature's Grasp when the mob attacks it: once this fight (a fresh solo mob), else on its cooldown.
    // b467 Sylaenas L17 balance: one in 4 of 5 fights, 1.5 s of a 16 s fight the formula gave to Wrath
    if (rotation && !ideal && m.pushHitsPerSec > 0.0f)
        if (SpellInfo const* grasp = NaturesGraspDue(bot))
        {
            Cast cast;
            cast.kind = Kind::Opener;
            cast.power = POWER_MANA;
            cast.cost = float(std::max(0, grasp->CalcPowerCost(bot, grasp->GetSchoolMask())));
            cast.executeSec = GlobalCooldownMs(bot, bot, grasp, m.bonus.spellHaste) / float(IN_MILLISECONDS);
            int32 cooldownMs = int32(grasp->RecoveryTime);
            bot->ApplySpellMod(grasp->Id, SPELLMOD_COOLDOWN, cooldownMs);
            float const period =
                lifeSec > 0.0f ? lifeSec : float(std::max(cooldownMs, 1)) / float(IN_MILLISECONDS);
            if (cast.executeSec > 0.0f && period >= cast.executeSec)
                spend(cast, affordable(cast, 1.0f / period));
        }
    if (rotation)
    {
        std::vector<Cast> fillers;
        Cast builder;
        bool haveBuilder = false;
        std::vector<Cast> finishers;
        Cast swingCast;
        bool haveSwing = false;
        uint32 seal = 0;
        if (cls == CLASS_PALADIN)
            seal = known["Seal of Righteousness"];
        // [dpscal] the filler it casts: the first one it knows in the bot's priority (the list's order); [rot604] the
        // stronger of the BestFiller ones it knows when it gets to them (DpsEstimate::FillerDps, the bot's rule)
        // (the ideal: the strongest one it knows)
        uint32 fillerId = 0;
        float fillerBest = 0.0f;
        for (Ability const& ability : rotation->abilities)
            if (ability.kind == Kind::Filler && known[ability.name])
            {
                uint32 const id = known[ability.name];
                if (!ideal && ability.script != Script::BestFiller)
                {
                    if (!fillerId)
                        fillerId = id;
                    break;
                }
                float const dps = m.standIn ? 0.0f : DpsEstimate::FillerDps(bot, target, id);
                if (!fillerId || dps > fillerBest)
                {
                    fillerId = id;
                    fillerBest = dps;
                }
            }
        p.fillerId = fillerId;
        bool judged = false;
        SpellInfo const* hasteFinisher = nullptr;  // [dps237]
        for (Ability const& ability : rotation->abilities)
        {
            uint32 const id = known[ability.name];
            if (!id)
                continue;
            if (ability.kind == Kind::HasteFinisher)
            {
                hasteFinisher = sSpellMgr->GetSpellInfo(id);
                continue;
            }
            Cast cast;
            uint32 readId = id;
            if (ability.kind == Kind::Judgement)
            {
                // [rot609] Judgement of Light / Wisdom / Justice share one cooldown: one judgement (b341 L10-19
                // protection: Judgement of Wisdom 2.0 casts a fight against a formula that judged twice, 0.76 of it)
                if (!seal || judged)
                    continue;
                readId = SPELL_JUDGEMENT_OF_RIGHTEOUSNESS;  // the judgement of the seal it keeps
            }
            if (!ReadCast(m, bot, readId, ability, cast))
                continue;
            // [dps232] a reactive strike: its cooldown and then the wait for an avoid when none came in the last
            // window (avoids at `procs` a second, memoryless); a fresh mob's: its run into melee and the first avoid
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(readId);
                info && info->CasterAuraState == AURA_STATE_DEFENSE && cast.kind == Kind::Cooldown)
            {
                float const procs = DefenseProcsPerSec(bot, m.standIn ? nullptr : target, ctx);
                if (procs <= 0.0f)
                    continue;
                float const windowSec = float(REACTIVE_TIMER_START) / float(IN_MILLISECONDS);
                cast.periodSec += std::exp(-procs * windowSec) / procs;
                if (lifeSec > 0.0f && !m.standIn)
                    cast.firstSec = MeleeArrivalSec(target, bot) + 1.0f / procs;
            }
            if (ability.kind == Kind::Judgement)
            {
                judged = true;
                // the judgement's own cooldown and cost
                if (SpellInfo const* judge = sSpellMgr->GetSpellInfo(id))
                {
                    int32 cd = int32(judge->RecoveryTime);
                    bot->ApplySpellMod(judge->Id, SPELLMOD_COOLDOWN, cd);
                    cast.periodSec = float(cd) / float(IN_MILLISECONDS);
                    cast.cost = float(std::max(0, judge->CalcPowerCost(bot, judge->GetSchoolMask())));
                    cast.executeSec = GlobalCooldownMs(bot, bot, judge, m.bonus.spellHaste) / float(IN_MILLISECONDS);
                    // [dps80] and its own name: the damage is the seal's judgement, the cast is this ability, and
                    // the plan is read against the bot's cast log. Named by the priced spell, the [dpsparts] plan
                    // said "Judgement of Righteousness" in 96-98 % of paladin fights while the log said "Judgement
                    // of Wisdom" / "of Light" - a ledger audit reads that as the model pricing a judgement the bot
                    // never casts, and it is nothing of the kind: the cost and the cooldown above already come
                    // from the ability the bot casts. The one spell id that is both is this one.
                    cast.id = judge->Id;
                }
                cast.kind = Kind::Cooldown;
            }
            // [rot603] a cooldown nuke where it pays against the filler, as the bot casts it
            if (cast.kind == Kind::PaidCooldown)
            {
                if (!m.standIn && !DpsEstimate::CastPays(bot, target, id, fillerId))
                    continue;
                cast.kind = Kind::Cooldown;
            }
            // [dpscal] a mana spell over a fight of lifeSec: a DoT as the bot's payoff rule casts it, a cooldown at the
            // fight's start and then on its cooldown (the casts that land before the death)
            bool const finite = lifeSec > 0.0f && cast.power == POWER_MANA;
            switch (cast.kind)
            {
                case Kind::Opener:  // [rot605]
                    if (lifeSec > 0.0f && lifeSec >= cast.executeSec)
                        spend(cast, affordable(cast, 1.0f / lifeSec));
                    break;
                case Kind::Dot:
                    if (finite)
                    {
                        float const rate = DotRateOver(bot, target, cast, fillerId, lifeSec);
                        if (rate > 0.0f)
                            spend(cast, affordable(cast, rate));
                        break;
                    }
                    [[fallthrough]];
                case Kind::Cooldown:
                    if (finite && cast.kind == Kind::Cooldown && cast.periodSec > 0.0f)
                    {
                        float const landSec = lifeSec - cast.firstSec - cast.executeSec;
                        if (landSec >= 0.0f)
                            spend(cast, affordable(cast, (std::floor(landSec / cast.periodSec) + 1.0f) / lifeSec));
                        break;
                    }
                    spend(cast, affordable(cast, cast.periodSec > 0.0f ? 1.0f / cast.periodSec
                                                                       : std::numeric_limits<float>::max()));
                    break;
                case Kind::Filler:
                    fillers.push_back(cast);
                    break;
                case Kind::Builder:
                    if (!haveBuilder)
                    {
                        builder = cast;
                        haveBuilder = true;
                    }
                    break;
                case Kind::ComboDot:
                    // [dps43] a bleed the target will not live to take is not damage. A ComboDot was the one kind
                    // that reached the combo cycle whole: priced once per its duration with every tick of that
                    // duration counted, however long the mob had left. Its own caster refuses it on the same
                    // ground - RupturePays (RogueActions.cpp, [rot561]) prices Rupture over
                    // min(lifetime, duration) and holds it when Eviscerate beats it - so the formula promised a
                    // finisher the bot was right not to cast: b479 4/combat measured 73 % of its ideal with
                    // Rupture 27 % of that plan and 0 casts in 21 fights (0 of 23 over both rogue specs).
                    // DotRateOver is the judge the Dot kind already uses, and it is the same rule: the last
                    // application keeps the ticks that land before the death and is dropped where the filler it
                    // replaced pays more. No second rule and no threshold of its own - the rate it returns
                    // carries the clamp, and a ComboDot that pays nothing is not priced at all.
                    if (lifeSec > 0.0f)
                    {
                        float const rate = DotRateOver(bot, target, cast, fillerId, lifeSec);
                        if (rate <= 0.0f)
                            break;
                        cast.periodSec = 1.0f / rate;  // the combo cycle's own limit for a ComboDot
                    }
                    finishers.push_back(cast);
                    break;
                case Kind::Finisher:
                    finishers.push_back(cast);
                    break;
                case Kind::Swing:
                    swingCast = cast;
                    haveSwing = true;
                    break;
                case Kind::Dump:
                    fillers.push_back(cast);
                    break;
                default:
                    break;
            }
        }

        // [dps237] Slice and Dice before the damage finishers: its builders and its cast once per its duration, the
        // swings it hastens its damage. The steady state keeps it up on five points (wowsims rogue APLs, Rawr.Rogue);
        // a solo bot's fresh mob takes it once on the bot's SND_MIN_COMBO_POINTS (2) when the mob outlives the bot's
        // SND_MIN_LIFETIME_S (6 s) after it (RogueTriggers.cpp). Its haste is the swings' already where it wears it.
        // b426-b429: neither the formula nor the ideal had it
        if (haveBuilder && hasteFinisher && meleeWhite > 0.0f && !bot->HasAura(hasteFinisher->Id))
        {
            float hastePct = 0.0f;
            for (SpellEffectInfo const& effect : hasteFinisher->GetEffects())
                if (effect.ApplyAuraName == SPELL_AURA_MOD_MELEE_HASTE)
                    hastePct = std::max(hastePct, float(effect.CalcValue(bot)));
            // [dps44] the caster's own gate, from its one definition in EstimatedLifetimeValue.h (was a second
            // pair of literals here, which is why the comment above had to name RogueTriggers.cpp)
            uint8 const points = lifeSec > 0.0f ? SND_MIN_COMBO_POINTS : MAX_COMBO;
            int32 durationMs = hasteFinisher->GetDuration();
            int32 const maxMs = hasteFinisher->GetMaxDuration();
            if (durationMs > 0 && maxMs > durationMs)
                durationMs += (maxMs - durationMs) * points / MAX_COMBO;  // WorldObject::CalcSpellDuration
            bot->ApplySpellMod(hasteFinisher->Id, SPELLMOD_DURATION, durationMs);
            float const durationSec = float(std::max(durationMs, 0)) / float(IN_MILLISECONDS);
            float const builders = std::ceil(float(points) / std::max(1.0f, builder.comboGain));
            Cast cyc;
            cyc.id = hasteFinisher->Id;
            cyc.kind = Kind::ComboDot;
            cyc.power = POWER_ENERGY;
            cyc.executeSec = builders * builder.executeSec +
                             GlobalCooldownMs(bot, bot, hasteFinisher, m.bonus.spellHaste) / float(IN_MILLISECONDS);
            cyc.cost = builders * builder.cost +
                       float(std::max(0, hasteFinisher->CalcPowerCost(bot, hasteFinisher->GetSchoolMask())));
            float uptime = durationSec;
            float limit = durationSec > 0.0f ? 1.0f / durationSec : 0.0f;
            if (lifeSec > 0.0f)
            {
                float const after = lifeSec - cyc.executeSec;
                uptime = after >= SND_MIN_LIFETIME_S ? std::min(durationSec, after) : 0.0f;
                limit = 1.0f / lifeSec;
            }
            float const gain = meleeWhite * hastePct / 100.0f * uptime;
            if (hastePct > 0.0f && uptime > 0.0f)
            {
                cyc.damage = builders * builder.damage + gain;
                cyc.threat = builders * builder.threat + gain;
                spend(cyc, affordable(cyc, limit));
            }
        }

        // combo cycles: builders to five points, then the finisher (kept-up ones first)
        if (haveBuilder)
        {
            float const perBuilder = std::max(1.0f, builder.comboGain);
            float const builders = std::ceil(float(MAX_COMBO) / perBuilder);
            for (Cast const& finisher : finishers)
            {
                Cast cyc = finisher;
                cyc.executeSec = builders * builder.executeSec + finisher.executeSec;
                cyc.cost = builders * builder.cost + finisher.cost;
                cyc.damage = builders * builder.damage + finisher.damage;
                cyc.threat = builders * builder.threat + finisher.threat;
                float const limit = finisher.kind == Kind::ComboDot && finisher.periodSec > 0.0f
                                        ? 1.0f / finisher.periodSec
                                        : std::numeric_limits<float>::max();
                spend(cyc, affordable(cyc, limit));
            }
            spend(builder, affordable(builder, std::numeric_limits<float>::max()));
        }

        // fillers: the rune classes use each in turn (their runes differ), the others the best one
        if (!fillers.empty())
        {
            if (cls == CLASS_DEATH_KNIGHT)
            {
                for (Cast const& filler : fillers)
                    if (filler.kind == Kind::Filler)
                        spend(filler, affordable(filler, std::numeric_limits<float>::max()));
                for (Cast const& dump : fillers)
                    if (dump.kind == Kind::Dump)
                        spend(dump, affordable(dump, std::numeric_limits<float>::max()));
            }
            else
            {
                // [dpscal] the first it knows in the bot's priority, not the best one: the bot casts that one
                // ([rot604] fillerId: the stronger BestFiller one) (the ideal: the best one)
                Cast const* pick = &fillers.front();
                for (Cast const& filler : fillers)
                    if (filler.id == fillerId)
                    {
                        pick = &filler;
                        break;
                    }
                if (ideal)
                    for (Cast const& filler : fillers)
                        if (filler.damage / std::max(filler.executeSec, 0.1f) >
                            pick->damage / std::max(pick->executeSec, 0.1f))
                            pick = &filler;
                float rate = affordable(*pick, std::numeric_limits<float>::max());
                // [rot610] over a fight of lifeSec: the mob dies at an even chance anywhere in the filler cast under
                // way when another part of its damage (swings, ticks, cooldowns, its pet) kills it; that cast is lost,
                // half a cast on average, the kill falling to the parts by their share of the damage
                if (lifeSec > 0.0f && rate > 0.0f && pick->executeSec > 0.0f)
                {
                    float const others = damage + PetDps(m, bot->GetPet());
                    float const own = rate * pick->damage;
                    float const otherShare = others / (others + own);
                    rate = std::max(0.0f, rate - otherShare * 0.5f / lifeSec);
                }
                spend(*pick, rate);
            }
        }

        // the rage dump on the next swing: replaces a swing (and the rage it brought)
        if (haveSwing && mh.perSec > 0.0f)
        {
            Cast dump = swingCast;
            dump.cost = swingCast.cost + mh.rage;
            float rate = std::min(swingsLeft, affordable(dump, swingsLeft));
            if (swingCast.periodSec > 0.0f)
                rate = std::min(rate, 1.0f / swingCast.periodSec);  // [rot605] Raptor Strike's cooldown
            dump.damage = swingCast.damage - mh.damage;
            dump.threat = swingCast.threat - mh.damage;
            spend(dump, rate);
            // [rot611] the log's parts as the [dps] measure splits them: the strike in full under its name, the
            // white part without the swings it replaced (b341 Dekorhan: white 2.2 measured in melee against a formula
            // white that still had its Raptor Strike swings)
            if (rate > 0.0f && !p.parts.empty())
            {
                p.parts.back().second += rate * mh.damage;
                p.replacedWhite = rate * mh.damage;
                p.replacedHitsPerSec = std::min(p.whiteHitsPerSec, rate * mh.landed);  // [dps60]
                p.whiteHitsPerSec = std::max(0.0f, p.whiteHitsPerSec - rate * mh.landed);  // [dps238]
            }
        }

        // Seal of Righteousness on every landed main hand hit (white and weapon strikes)
        if (seal && mh.perSec > 0.0f)
            if (SpellInfo const* proc = sSpellMgr->GetSpellInfo(SPELL_SEAL_OF_RIGHTEOUSNESS_DAMAGE))
            {
                float const holy = float(bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_HOLY)) + m.spGain;
                float const base = (SEAL_RIGHTEOUSNESS_AP * (bot->GetTotalAttackPowerValue(BASE_ATTACK) + m.apGain) +
                                    SEAL_RIGHTEOUSNESS_SP * holy) *
                                   float(bot->GetAttackTime(BASE_ATTACK)) / float(IN_MILLISECONDS);
                float hit = float(bot->SpellDamageBonusDone(target, proc, uint32(base), SPELL_DIRECT_DAMAGE, EFFECT_0));
                hit =
                    WithCrit(m, bot, proc, TakenAndArmor(m, bot, proc, hit, SPELL_DIRECT_DAMAGE, EFFECT_0, BASE_ATTACK),
                             false, BASE_ATTACK);
                float const procs = mh.landed * mh.perSec + strikes;
                damage += hit * procs;
                threat += ThreatOf(bot, proc, hit * procs, procs);
            }
        p.lifeTap = cls == CLASS_WARLOCK && known["Life Tap"];
    }

    p.pet = PetDps(m, bot->GetPet());
    // [rot607] over a solo fight: the pet's damage from its arrival (b338 warlocks' demons 4.0-6.5 of 8-13 dps)
    if (lifeSec > 0.0f && !m.standIn && p.pet > 0.0f)
        p.pet *= std::max(0.0f, lifeSec - PetReachSec(bot->GetPet(), target)) / lifeSec;
    p.burst = damage + p.pet;
    p.white += p.pet;
    p.tps = threat;
    p.whiteTps = whiteTps;  // [lead1309]
    p.rageSpend = std::max(rageIncome - rage, 0.0f);
    p.ragePerSwing = mh.rage;
    p.offRagePerSwing = oh.rage;
    p.manaSpend = mana;
    p.pushHitsPerSec = m.pushHitsPerSec;  // [death457]
    p.rotationWhite = rotationWhite;
    // Player::Regenerate (POWER_MANA) while casting: the interrupted regeneration, boosted below level 15
    float rate = sWorld->getRate(RATE_POWER_MANA);
    if (sWorld->getBoolConfig(CONFIG_LOW_LEVEL_REGEN_BOOST) && bot->GetLevel() < 15)
        rate *= 2.066f - float(bot->GetLevel()) * 0.066f;
    // [dun230] without a food / drink aura's MP5: it breaks when the bot stands up for the fight (b444 wc-n: a
    // healer drinking at the five's forming read 36 mana/s instead of 1.4)
    p.manaRegen = (std::max(0.0f, bot->GetFloatValue(uint16(UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER) +
                                                     uint16(POWER_MANA)) -
                                      RestState::DrinkRegenPerSec(bot)) +
                   m.bonus.manaPerSec) *
                  rate;
    p.cycleSec = std::max(cycle, float(GCD_MAX_MS) / float(IN_MILLISECONDS));
    return p;
}

// [dpscal] A solo bot kills its mob alone: the formula is the one over that fight, the mob's full health over the
// steady state's damage (the judge weighs a fresh pull; the [dps] check opens at the first hit). A group or raid shares
// the kill: the steady state.
Profile Compute(Player* bot, Unit* target, BuffContext ctx, bool ideal)
{
    Profile p = ComputeAt(bot, target, ctx, 0.0f, ideal);
    if (ctx != BuffContext::Solo || !target || target == bot || !target->GetMaxHealth() || p.burst <= 0.0f)
        return p;
    return ComputeAt(bot, target, ctx, float(target->GetMaxHealth()) / p.burst, ideal);
}

// ---- cache ----------------------------------------------------------------------------------------------------

uint64 Signature(Player* bot, Unit* target, BuffContext ctx)
{
    uint64 h = 1469598103934665603ULL;
    auto const mix = [&h](uint64 v)
    {
        h ^= v;
        h *= 1099511628211ULL;
    };
    auto const mixF = [&mix](float v) { mix(uint64(int64(std::lround(v * 10.0f)))); };
    mix(bot->GetLevel());
    mix(uint64(ctx));
    mix(bot->GetSpellMap().size());
    mix(bot->GetActiveSpec());
    mix(bot->GetFreeTalentPoints());
    mix(bot->GetShapeshiftForm());
    // the core's stat outputs carry gear, enchants, gems, talents and auras
    mixF(bot->GetTotalAttackPowerValue(BASE_ATTACK));
    mixF(bot->GetTotalAttackPowerValue(RANGED_ATTACK));
    mixF(float(bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_MAGIC)));
    mixF(bot->GetFloatValue(UNIT_FIELD_MINDAMAGE));
    mixF(bot->GetFloatValue(UNIT_FIELD_MAXDAMAGE));
    mixF(bot->GetFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE));
    mixF(bot->GetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE));
    mixF(float(bot->GetAttackTime(BASE_ATTACK)));
    mixF(float(bot->GetAttackTime(RANGED_ATTACK)));
    mixF(bot->GetFloatValue(PLAYER_CRIT_PERCENTAGE));
    mixF(bot->GetFloatValue(uint16(PLAYER_SPELL_CRIT_PERCENTAGE1) + uint16(SPELL_SCHOOL_FIRE)));
    mixF(bot->GetFloatValue(UNIT_MOD_CAST_SPEED) * 1000.0f);
    mixF(bot->m_modMeleeHitChance);
    mixF(bot->m_modSpellHitChance);
    mixF(bot->GetFloatValue(uint16(UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER) + uint16(POWER_MANA)));
    // [deaths12] the energy it opens with (ComputeAt): the cache key in steps of 20 energy
    if (bot->getPowerType() == POWER_ENERGY)
        mix(bot->GetPower(POWER_ENERGY) / 20);
    mix(bot->GetGroup() ? bot->GetGroup()->GetMembersCount() : 0);
    // [deaths9] the members present (PlannedBuffs' providers: alive within the group reward distance)
    if (ctx == BuffContext::Group)
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    mix(member != bot && member->IsInWorld() && member->IsAlive() &&
                        member->IsAtGroupRewardDistance(bot));
    if (Pet* pet = bot->GetPet())
    {
        mix(pet->GetEntry());
        mix(pet->GetLevel());
        mix(pet->IsAlive());
    }
    mix(HunterInMelee(bot, target));  // [death446]
    mix(NaturesGraspDue(bot) != nullptr);  // [death457]
    if (target)
    {
        mix(target->GetEntry());
        mix(target->GetLevel());
        mix(target->GetArmor());
        mix(target->GetAppliedAuras().size());
    }
    return h;
}

struct CacheEntry
{
    uint64 signature;
    Profile profile;
};

std::mutex cacheLock;
std::unordered_map<uint64, std::vector<CacheEntry>> cache;  // bot guid -> its last profiles

// the profile for this signature: the cached one, else computed and cached
Profile ProfileOf(Player* bot, Unit* target, BuffContext ctx, bool ideal = false)
{
    uint64 signature = Signature(bot, target, ctx);
    if (ideal)
        signature = (signature ^ 0x1DEA1ULL) * 1099511628211ULL;  // FNV step: the ideal's own entry
    uint64 const guid = bot->GetGUID().GetRawValue();
    {
        std::lock_guard<std::mutex> guard(cacheLock);
        for (CacheEntry const& entry : cache[guid])
            if (entry.signature == signature)
                return entry.profile;
    }
    Profile profile = Compute(bot, target, ctx, ideal);
    std::lock_guard<std::mutex> guard(cacheLock);
    std::vector<CacheEntry>& entries = cache[guid];
    if (entries.size() >= CACHE_PER_BOT)
        entries.erase(entries.begin());
    entries.push_back({signature, profile});
    return profile;
}

// ---- [dpscal] the fight as it went ------------------------------------------------------------------------------

struct LiveFight
{
    uint64 raw = 0;  // its, its pet's and its totems' damage to creatures, the overkill of a killing hit included
    uint64 pet = 0, white = 0, spell = 0, dot = 0;
    uint32 firstDealtMs = 0, lastDealtMs = 0, firstTakenMs = 0;
    // [dps20] a mob of the fight took damage from someone else. It is a fact of the fight and no longer a verdict on
    // the sample: the damage this fight measures is the damage THIS bot dealt (every hit counted here passes
    // GetCharmerOrOwnerPlayerOrPlayerItself == bot), so an ally hitting the same mob takes nothing away from it. In a
    // five every mob is shared, which is why 75 055 of 75 074 rows were solo and not one was measured in a dungeon.
    // What a shared kill really costs the sample is a window cut short by someone else's killing blow, and the
    // measured window covering one rotation is the own-reason that already catches that.
    bool shared = false;
    uint64 taken = 0;  // [dps20] the mobs' damage on it this fight, each hit up to its health (overkill is no damage)
    // [dps20] the fight's own clocks, sampled at its damage events (the interval goes to the reading the event
    // before it took): the time it attacked, the white-swing gate's buckets, and the lead swing. They were
    // SoloSafety's and are the measurement's: a fight in a dungeon reaches no solo judge.
    uint32 sampleMs = 0;
    bool sampleAct = false;
    uint32 actMs = 0;
    uint32 ableMs = 0;
    uint32 blockMs[6] = {0, 0, 0, 0, 0, 0};  // [dps20] 6: busy with its own cast or charge
    uint8 swingBlock = 0;
    uint32 leadMs = 0;
    bool swam = false;    // [dps1] it swam at a damage event: no check of the formula
    bool looted = false;  // ... or looted
    // [dps20] where and in what company the fight OPENED. The row took the bot's map and position at the close, so
    // every one of the 23 rows on a dungeon map was an open-world fight the bot carried through a portal: map 43
    // (Wailing Caverns) held 8 fights against Mottled Raptors, Rabid Dire Wolves and Riverpaw Taskmasters, and the
    // "Wailing Caverns 84.6 %" read off them is of the Barrens, not of the dungeon.
    uint32 openMap = 0;
    float openX = 0.0f, openY = 0.0f, openZ = 0.0f;
    BuffContext openCtx = BuffContext::Solo;
    uint32 openEntry = 0;
    uint8 openLevel = 0;
    Result openResult;  // the formula's Result at the open: FormulaOver prices it over the measured window
    std::unordered_map<uint64, uint64> mobs;    // mob guid -> its damage on it (up to the mob's health)
    std::map<uint32, uint32> casts;             // spell id -> casts that went off
    uint32 timedCasts = 0;                      // its cast / channel / global cooldown spells at a mob (no auto-repeat)
    float timedMs = 0.0f;                       // ... and their execute time
    Profile formula;                            // the formula at its first hit
    Profile ideal;                              // the ideal rotation's at its first hit
    bool petOut = false;
    // [dps238] what splits a melee white gap and a caster's idle time: its landed white swings, its finishers and the
    // combo points they spent (b426-b429: melee white 0.66-0.85 of the formula's in fights spent in melee, casters'
    // casts 0.7-0.9 of the window when hit and ~1.0 when not)
    uint32 whiteHits = 0;
    uint32 finishers = 0, finisherPoints = 0;
    // [dps60] the on-next-swing strikes it cast (Heroic Strike, Cleave, Maul, Raptor Strike). Each one spends a
    // swing the timer produced and lands as itself, not as a white hit, so `whiteHits` alone is short of the
    // timer's output by this count. That a next-swing strike replaces a swing is both references' reading too:
    // wowsims AutoAttacks::MaybeReplaceMHSwing ("overridden for abilities like Heroic Strike") and Rawr 2.3.23's
    // MhActivates = FightDuration / MhEffectiveSpeed - HSOverridesOverDur - CLOverridesOverDur.
    //
    // Read off the core's own predicate (Spell::IsNextMeleeSwingSpell tests SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE),
    // not a list of spell ids, because what the core queues is what spends a swing here. SharedDefines records the
    // sibling SPELL_ATTR0_ON_NEXT_SWING as identical in handling and spell_druid.cpp agrees, so the core's single
    // test is incomplete - but not for a player: Spell.dbc carries 113 spells with the sibling bit alone and every
    // one is a creature ability (Pierce Armor, Head Crack, Tendon Rip, Gore...), while all 27 ranks of Heroic
    // Strike, all 50 player ranks of Cleave, all 16 of Maul and all 13 of Raptor Strike carry the bit the core
    // tests. So this count cannot silently miss a bot's strike. Slam carries neither bit, correctly: it delays the
    // swing instead of replacing it (wowsims keeps DelayMeleeBy for it alone). The core's own gap is written up in
    // docs/UPSTREAM_CANDIDATES.md under dps60 - fixing it would move 113 creature abilities' timing, so it is not
    // this pass's to make.
    uint32 nextSwings = 0;
};

// [dps20] the open fights, sharded. It was one mutex and one map, and that cost nothing while only a bot fighting
// alone in the open world was tracked: every other bot left NoteDealt before the lock. Measuring a five in a
// dungeon means every damage event of every bot reaches it, so a 25-bot raid's swings would queue on one mutex -
// against the module's own goal of thousands of bots. The critical sections are a few field adds each; sixteen
// shards keyed by the bot's guid keep them from meeting. One judge still: one store, sixteen doors into it.
constexpr size_t LIVE_SHARDS = 16;
struct LiveShard
{
    std::mutex lock;
    std::unordered_map<uint64, LiveFight> fights;
};
LiveShard liveShards[LIVE_SHARDS];
LiveShard& ShardOf(uint64 guid) { return liveShards[guid % LIVE_SHARDS]; }

// SoloSafety::SoloPlayerBot: a bot out in the world, alone. SoloSafety::CloseFight closes those fights and only
// those, so this is the one predicate that says whose open fight another judge will close.
// [deaths9] a grouped bot far from every living member too: b345 Guinudrond's fights logged "casts: -" and no ideal
bool SoloBot(Player* bot) { return bot && GET_PLAYERBOT_AI(bot) && DpsEstimate::FightsAlone(bot); }

// [dps20] whose fight is measured: every bot, in a group, in a raid, in a dungeon. This was `SoloBot` and that is
// the whole of why the judged quantity was never judged where it matters - the gate is read before a single damage
// event is counted, so a fight in a five opened no sample at all. 75 055 of 75 074 rows are solo, 17 are group, 2
// are raid, and the user's rule (90 % of wowsims for every spec, 100 % the aim) was never measured in a dungeon.
bool MeasuredBot(Player* bot) { return bot && bot->IsInWorld() && GET_PLAYERBOT_AI(bot); }

std::string SpellName(uint32 id)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
    return info ? std::string(info->SpellName[LOCALE_enUS]) : std::to_string(id);
}

// [dps41] why no white swing was landing on a mob of this fight, read off the core's own melee gate
// (Player::Update: UNIT_STATE_MELEE_ATTACKING, not casting and not charging, a victim, within melee reach, and
// facing it inside 120 degrees unless it stands in the boundary radius). The reasons are the core's, not a model of
// them: a swing the core refuses is a swing the formula counted and the bot never took.
// 0: it could swing. 1 range, 2 facing, 3 no swing under way, 4 taken off it (stun, fear, confusion), 5 swinging at
// a unit of no fight of its own, 6 busy with its own cast or charge - the order of the buckets below.
// [dps20] moved here from SoloSafety, which saw solo fights only. "Swinging elsewhere" now reads the mobs this
// fight has damage of (`f.mobs`, plus the mob of the event being sampled) where it read the mobs attacking the bot:
// in a five the bot's target is routinely not its attacker, and a swing at a second mob of the same pull is a swing
// this measurement counts (DpsEstimate counts every melee hit it lands on a creature).
// [dps20] casting is its own bucket, and that is a correction, not a refinement. One bucket held
// CASTING | CHARGING | STUNNED | FLEEING | CONFUSED under the name "controlled", so a caster read 97 % of its
// window as "not its own to spend" when it was simply casting - its whole job. A stun is a defect to chase and a
// cast is not, and a column that cannot tell them apart answers nothing. The reading of a melee bot is unchanged
// except that its own casts leave the control bucket, where they never belonged.
uint8 SwingBlock(Player* bot, LiveFight const& f, Unit* mob)
{
    if (!bot || !mob)
        return 0;
    if (bot->HasUnitState(UNIT_STATE_STUNNED) || bot->HasUnitState(UNIT_STATE_FLEEING) ||
        bot->HasUnitState(UNIT_STATE_CONFUSED))
        return 4;
    if (bot->HasUnitState(UNIT_STATE_CASTING) || bot->HasUnitState(UNIT_STATE_CHARGING))
        return 6;
    Unit* victim = bot->GetVictim();
    if (!victim || !bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        return 3;
    if (victim != mob && !f.mobs.count(victim->GetGUID().GetRawValue()))
        return 5;
    if (!bot->IsWithinMeleeRange(victim))
        return 1;
    if (!bot->IsWithinBoundaryRadius(victim) && !bot->HasInArc(2.0f * float(M_PI) / 3.0f, victim))
        return 2;
    return 0;
}

// [dps20] the fight's clocks at one damage event: the time it attacked (a victim or a cast under way) and the
// swing gate's reading, each interval charged to the reading the event before it took. Caller holds its shard's lock.
// [dps50] the fight's first sample charges the lead swing - the swing the bot was swinging for before that first
// event closed it - to the reading that sample takes, and carries it: the speed at the open, not at the close (a
// druid's shift changes it). The buckets tile span + lead, which is the window the allowance divides.
void SampleFight(Player* bot, Unit* mob, LiveFight& f, uint32 now)
{
    if (!bot)
        return;
    if (f.sampleMs)
    {
        uint32 const since = getMSTimeDiff(f.sampleMs, now);
        if (f.sampleAct)
            f.actMs += since;
        if (!f.swingBlock)
            f.ableMs += since;
        else
            f.blockMs[f.swingBlock - 1] += since;
    }
    bool const open = !f.sampleMs;
    f.swingBlock = SwingBlock(bot, f, mob);
    if (open)
    {
        f.leadMs = uint32(std::max(float(bot->GetAttackTime(BASE_ATTACK)) / float(IN_MILLISECONDS), 0.1f) *
                          float(IN_MILLISECONDS));
        if (!f.swingBlock)
            f.ableMs += f.leadMs;
        else
            f.blockMs[f.swingBlock - 1] += f.leadMs;
    }
    // [dps70] never 0: `sampleMs` 0 is what marks the fight's first sample, so a getMSTime() landing exactly on 0
    // would charge the lead swing a second time. One tick of 1 ms is the whole cost.
    f.sampleMs = now ? now : 1;
    f.sampleAct = bot->GetVictim() || bot->IsNonMeleeSpellCast(false);
    f.swam = f.swam || bot->IsInWater();  // [dps1]
    f.looted = f.looted || !bot->GetLootGUID().IsEmpty();
}

// [dps20] where and in what company the fight opens, taken once, at its first damage event either way. The row used
// to read the bot's map and position at the CLOSE, which is a different place entirely for a fight it walked out of.
void OpenFacts(Player* bot, LiveFight& f, Unit* mob)
{
    if (f.openMap || !bot)
        return;
    f.openMap = bot->GetMapId() + 1;  // +1: 0 is "not taken yet", map 0 is Eastern Kingdoms
    f.openX = bot->GetPositionX();
    f.openY = bot->GetPositionY();
    f.openZ = bot->GetPositionZ();
    f.openCtx = DpsEstimate::ContextOf(bot);
    f.openEntry = mob ? mob->GetEntry() : 0;
    f.openLevel = mob ? uint8(mob->GetLevel()) : uint8(0);
}
}  // namespace

namespace DpsEstimate
{
Result Estimate(Player* bot, Unit* target, BuffContext ctx, float fightSec, float pushHitsPerSec)
{
    Result out;
    if (!bot || !bot->IsInWorld() || (target && !target->IsInWorld()))
        return out;  // [dpszero1] `estimated` stays false: no estimate exists, and that is not a zero
    out.estimated = true;
    Profile profile = ProfileOf(bot, target, ctx);
    out.pushHitsPerSec = profile.pushHitsPerSec;
    // [death457] the hits of every mob on it (not its target's alone): each pushed-back cast's execute time again at
    // that rate, the rotation's casts and their mana stretched over the longer time (the weapon's and pet's are not)
    if (pushHitsPerSec >= 0.0f && !profile.pushCasts.empty())
    {
        float stretch = 0.0f;  // seconds a second of the rotation
        for (Profile::PushCast const& c : profile.pushCasts)
        {
            auto const execute = [&c](float hits)
            { return std::max(c.baseCastMs + PushedHits(hits, c.baseCastMs, c.delayMs) * c.delayMs, c.gcdMs); };
            stretch += c.rate * (execute(pushHitsPerSec) - execute(profile.pushHitsPerSec)) / float(IN_MILLISECONDS);
        }
        float const factor = 1.0f / std::max(1.0f + stretch, 0.01f);
        float const timed = profile.pet + profile.rotationWhite;
        profile.burst = std::max(0.0f, profile.burst - timed) * factor + timed;
        profile.tps = std::max(0.0f, profile.tps - profile.whiteTps) * factor + profile.whiteTps;
        profile.manaSpend *= factor;
        out.pushHitsPerSec = pushHitsPerSec;
    }

    out.burstDps = profile.burst;
    out.whiteDps = profile.white;
    out.petDps = profile.pet;
    out.tps = profile.tps;
    out.cycleSec = profile.cycleSec;
    out.spells = profile.spells;
    out.assumedBuffs = profile.assumedBuffs;  // [dps20]
    // a warlock taps life for mana when nothing attacks it (user rule: no Life Tap under attack, solo it is)
    bool const tapping = profile.lifeTap && ctx != BuffContext::Solo;
    if (bot->GetMaxPower(POWER_MANA) && !tapping)
        out.manaPerSec = std::max(0.0f, profile.manaSpend - profile.manaRegen);
    if (out.manaPerSec > 0.0f)
        out.oomSec = float(bot->GetPower(POWER_MANA)) / out.manaPerSec;
    // [dps20] the mana limit a measurement divides by: the same arithmetic with no Life Tap exemption. For every
    // bot but a tapping warlock this is `oomSec` itself; for that warlock `oomSec` is infinite by a decision's
    // choice, and a row that divided by it would credit the rotation with mana the bot never had. The decision's
    // own numbers above keep the exemption.
    if (bot->GetMaxPower(POWER_MANA))
    {
        float const drain = std::max(0.0f, profile.manaSpend - profile.manaRegen);
        if (drain > 0.0f)
            out.drainOomSec = float(bot->GetPower(POWER_MANA)) / drain;
    }
    out.dps = out.burstDps;
    if (fightSec > 0.0f && out.oomSec < fightSec)
        out.dps =
            (out.burstDps * out.oomSec + std::min(out.whiteDps, out.burstDps) * (fightSec - out.oomSec)) / fightSec;
    return out;
}

float MeleeHitsPerSec(Player* bot, Unit* mob) { return PushbackHitsPerSec(bot, mob, BuffContext::Solo, 0.0f); }

float EstimateDps(Player* bot, Unit* target, BuffContext ctx, float fightSec)
{
    return Estimate(bot, target, ctx, fightSec).dps;
}

float EstimateTps(Player* bot, Unit* target, BuffContext ctx) { return Estimate(bot, target, ctx).tps; }

namespace
{
// [lead1309] the share of `horizonSec` its rotation's spells run at their steady rate: 1 for mana and rune users; a
// rage user's spells held to the rage it has now, what its swings landing within the horizon bring (the next one at
// its attack timer, then one per attack time) and [lead1310] the rage of the damage it takes within it
// (`takenDamage`, Unit::RewardRage: damage / rage conversion x 2.5 x the rage income rate, core units x 10)
float SpellShareWithin(Player* bot, Profile const& p, float horizonSec, float takenDamage)
{
    if (!bot->HasActivePowerType(POWER_RAGE) || p.rageSpend <= 0.0f)
        return 1.0f;  // mana (a paladin's pool), runes (full at a pull): its rotation from the first GCD
    float const horizonMs = horizonSec * float(IN_MILLISECONDS);
    auto const swingsWithin = [bot, horizonMs](WeaponAttackType att) -> float
    {
        uint32 const attackMs = bot->GetAttackTime(att);
        if (!attackMs || (att == OFF_ATTACK && !bot->HasOffhandWeaponForAttack()))
            return 0.0f;
        float const nextMs = bot->isAttackReady(att) ? 0.0f : float(bot->getAttackTimer(att));
        return horizonMs < nextMs ? 0.0f : 1.0f + std::floor((horizonMs - nextMs) / float(attackMs));
    };
    float rage = float(bot->GetPower(POWER_RAGE)) + swingsWithin(BASE_ATTACK) * p.ragePerSwing +
                 swingsWithin(OFF_ATTACK) * p.offRagePerSwing;
    if (takenDamage > 0.0f)
    {
        float const level = float(bot->GetLevel());
        float conversion = 0.0091107836f * level * level + 3.225598133f * level + 4.2652911f;
        if (level > 70.0f)
            conversion += 13.27f * (level - 70.0f);  // Unit::RewardRage
        rage += takenDamage / conversion * 2.5f * sWorld->getRate(RATE_POWER_RAGE_INCOME) * 10.0f;
    }
    return std::min(1.0f, rage / (p.rageSpend * horizonSec));
}

float DegToRad(float degrees) { return degrees * float(M_PI) / 180.0f; }

// [lead1310] the core's cone of a cone-targeted effect (Spell::SelectImplicitConeTargets: spell_cone, else by target)
float ConeAngleOf(SpellInfo const* info, SpellImplicitTargetInfo const& target)
{
    if (SpellCone const* cone = sSpellMgr->GetSpellCone(info->Id))
        return DegToRad(float(cone->cone_degrees));
    switch (target.GetTarget())
    {
        case TARGET_UNIT_CONE_ENEMY_24:
            return DegToRad(24.0f);
        case TARGET_UNIT_CONE_ENEMY_54:
            return DegToRad(54.0f);
        case TARGET_UNIT_CONE_ENEMY_104:
            return DegToRad(104.0f);
        default:
            return DegToRad(60.0f);
    }
}

// [lead1310] an area spell of its rotation reaches `mob` from where the bot stands: within an area effect's radius
// (the mob's size and the caster's leeway left out: the safe side), in front within the cone of a cone; a capped
// spell only while the bot's attackers fit its cap (the core picks its targets at random)
bool AreaReaches(Player* bot, SpellInfo const* info, Unit* mob)
{
    if (info->MaxAffectedTargets && bot->getAttackers().size() > info->MaxAffectedTargets)
        return false;
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.IsTargetingArea() && effect.Effect != SPELL_EFFECT_PERSISTENT_AREA_AURA)
            continue;
        if (bot->GetExactDist(mob) > effect.CalcRadius(bot))
            continue;
        bool cone = false;
        for (SpellImplicitTargetInfo const& target : {effect.TargetA, effect.TargetB})
            if (target.GetSelectionCategory() == TARGET_SELECT_CATEGORY_CONE)
            {
                cone = true;
                if (bot->isInFront(mob, ConeAngleOf(info, target)))
                    return true;
            }
        if (!cone)
            return true;
    }
    return false;
}
}  // namespace

float ThreatWithin(Player* bot, Unit* target, BuffContext ctx, float horizonSec, float takenDamage)
{
    if (!bot || horizonSec <= 0.0f || !bot->IsInWorld() || (target && !target->IsInWorld()))
        return 0.0f;
    Profile const p = ProfileOf(bot, target, ctx);
    float const steady = p.tps * horizonSec;
    float const spellTps = std::max(p.tps - p.whiteTps, 0.0f);
    float const spell = spellTps * horizonSec * SpellShareWithin(bot, p, horizonSec, takenDamage);
    return std::min(steady, p.whiteTps * horizonSec + spell);
}

float AreaThreatWithin(Player* bot, Unit* target, Unit* mob, BuffContext ctx, float horizonSec, float areaDps,
                       float takenDamage)
{
    if (!bot || !mob || horizonSec <= 0.0f || areaDps <= 0.0f || !bot->IsInWorld() || !mob->IsInWorld() ||
        (target && !target->IsInWorld()))
        return 0.0f;
    Profile const p = ProfileOf(bot, target, ctx);
    float const own = p.burst - p.pet;  // the pet's damage is not in its tps
    if (own <= 0.0f || p.tps <= 0.0f)
        return 0.0f;
    // what its rotation's area spells deal a target a second (their parts), those reaching the mob
    float rotationArea = 0.0f;
    for (auto const& [spellId, dps] : p.parts)
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            info && dps > 0.0f && info->IsAffectingArea() && AreaReaches(bot, info, mob))
            rotationArea += dps;
    float const perTarget = std::min(areaDps, rotationArea);
    if (perTarget <= 0.0f)
        return 0.0f;
    // at its threat per damage (tps / dps), never above its rotation's spell threat
    float const spellTps = std::max(p.tps - p.whiteTps, 0.0f);
    float const areaTps = std::min(perTarget * p.tps / own, spellTps);
    return areaTps * horizonSec * SpellShareWithin(bot, p, horizonSec, takenDamage);
}

namespace
{
// the bot's state now against `target` (no planned buffs): what its decisions weigh
Model NowModel(Player* bot, Unit* target)
{
    BuffContext const ctx = ContextOf(bot);
    Model m;
    m.bot = bot;
    m.target = target;
    m.front = ctx == BuffContext::Solo || PlayerbotAI::IsTank(bot);
    m.pushHitsPerSec = PushbackHitsPerSec(bot, target, ctx, 0.0f);
    return m;
}
}  // namespace

bool CastPays(Player* bot, Unit* target, uint32 spellId, uint32 fillerSpellId)
{
    if (!bot || !target || !spellId || !bot->IsInWorld() || !target->IsInWorld())
        return false;
    if (!fillerSpellId)
        return true;
    // [rot610] the formula's cast read (was SpellPayoff's, a measured mean per hit once one landed)
    Cast cast;
    Ability const ability{"", Kind::Cooldown};
    if (!ReadCast(NowModel(bot, target), bot, spellId, ability, cast))
        return false;
    return cast.damage >= FillerDps(bot, target, fillerSpellId) * cast.executeSec;
}

float FillerDps(Player* bot, Unit* target, uint32 spellId)
{
    if (!bot || !target || !spellId || !bot->IsInWorld() || !target->IsInWorld())
        return 0.0f;
    Cast cast;
    Ability const ability{"", Kind::Filler};
    if (!ReadCast(NowModel(bot, target), bot, spellId, ability, cast))
        return 0.0f;
    return cast.damage / std::max(cast.executeSec, 0.1f);
}

float RemainingLifeSec(Player* bot, Unit* target)
{
    if (!bot || !target || !target->IsAlive())
        return 0.0f;
    BuffContext const ctx = ContextOf(bot);
    float const own = ProfileOf(bot, target, ctx).burst;
    if (own <= 0.0f)
        return std::numeric_limits<float>::max();
    uint32 on = 1;
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member != bot && member->IsAlive() && member->GetMapId() == bot->GetMapId() &&
                    (member->GetVictim() == target || member->GetTarget() == target->GetGUID()))
                    ++on;
    return float(target->GetHealth()) / (own * float(on));
}

bool DebuffPays(Player* bot, Unit* target, uint32 spellId)
{
    if (!bot || !target || !bot->IsInWorld() || !target->IsInWorld() || !target->IsAlive())
        return false;
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!info)
        return true;  // nothing to weigh (a talent's aura name): the cast's own checks decide
    float const lifeSec = RemainingLifeSec(bot, target);
    Cast dot;
    Ability const ability{"", Kind::Dot};
    // a debuff whose worth is not its own damage (a curse, a mark, Sunder Armor, Thunder Clap, Scorch, a snare or
    // crowd control with a hit): the target outlives the cast that puts it up
    if (!ReadCast(NowModel(bot, target), bot, spellId, ability, dot) || dot.periodic <= dot.direct)
    {
        float const ms = std::max(float(info->CalcCastTime(bot)), GlobalCooldownMs(bot, bot, info, 0.0f));
        return lifeSec * float(IN_MILLISECONDS) > ms;
    }
    // what lands before the death: the direct part once the cast lands, the ticks after it
    float const afterLandMs = lifeSec * float(IN_MILLISECONDS) - dot.castMs;
    if (afterLandMs < 0.0f)
        return false;
    float landed = dot.direct;
    if (dot.periodic > 0.0f && dot.tickMs > 0.0f && dot.durationMs > 0.0f)
        landed += dot.periodic * std::min(1.0f, std::floor(afterLandMs / dot.tickMs) * dot.tickMs / dot.durationMs);
    // against its rotation's alternative for the same rage / energy, or its filler in the same execute time
    Profile const p = ProfileOf(bot, target, ContextOf(bot));
    float alternative = 0.0f;
    if (dot.cost > 0.0f && dot.power == POWER_RAGE)
        alternative = p.altPerRage * dot.cost;
    else if (dot.cost > 0.0f && dot.power == POWER_ENERGY)
        alternative = p.altPerEnergy * dot.cost;
    else if (p.fillerId && p.fillerId != spellId)
        alternative = FillerDps(bot, target, p.fillerId) * dot.executeSec;
    return landed > 0.0f && landed >= alternative;
}

char const* ContextName(BuffContext ctx)
{
    switch (ctx)
    {
        case BuffContext::Group:
            return "group";
        case BuffContext::Raid:
            return "raid";
        default:
            return "solo";
    }
}

BuffContext ContextOf(Player* bot)
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group || group->GetMembersCount() <= 1 || FightsAlone(bot))
        return BuffContext::Solo;
    return group->isRaidGroup() ? BuffContext::Raid : BuffContext::Group;
}

bool FightsAlone(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->GetMap() || bot->GetMap()->Instanceable())
        return false;
    Group* group = bot->GetGroup();
    if (!group || group->GetMembersCount() <= 1)
        return true;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && member->IsInWorld() && member->IsAlive() &&
            member->IsAtGroupRewardDistance(bot))
            return false;
    }
    return true;
}

// [dps20] `drainOomSec`, not `oomSec`: a measurement divides by the mana the bot had, never by a Life Tap's
// endless ceiling. They are the same number for everything but a warlock in a group.
float FormulaOver(Result const& r, float sec)
{
    if (sec <= 0.0f || r.drainOomSec >= sec)
        return r.burstDps;
    return (r.burstDps * r.drainOomSec + std::min(r.whiteDps, r.burstDps) * (sec - r.drainOomSec)) / sec;
}

void RecordFight(Player* bot, FightSample const& sample)
{
    if (!bot)
        return;
    LiveFight f;
    {
        LiveShard& shard = ShardOf(bot->GetGUID().GetRawValue());
        std::lock_guard<std::mutex> guard(shard.lock);
        auto const found = shard.fights.find(bot->GetGUID().GetRawValue());
        if (found != shard.fights.end())
        {
            f = std::move(found->second);
            shard.fights.erase(found);
        }
    }
    // [dps20] the time it attacked is this clock's own reading now (f.actMs), not a number passed in: the one
    // measurement that sees every damage event of every fight, on every map, in or out of a group
    float const actedSec = float(f.actMs) / float(IN_MILLISECONDS) +
                           std::max(float(bot->GetAttackTime(BASE_ATTACK)) / float(IN_MILLISECONDS), 0.1f);
    float active = std::min(std::max(actedSec, 0.0f), std::max(sample.fightSec, 0.0f));
    float actual = active > 0.0f ? float(sample.dealt) / active : 0.0f;
    float const cappedActual = actual;
    // [dpscal] its own damage over the time it fought, measured as the formula counts it (b337: 0.73 of the formula
    // in the median, 17 % within 10 %):
    // - every hit in full: the hit that kills is dealt in full (at L1-9 a mob dies to 2-4 casts, half a cast of
    //   overkill on average);
    // - the time from its first action to its last hit: one interval of its damage before the first hit, the mean
    //   execute time of its casts for a caster, its ranged speed for a hunter, its swing else (one swing on top: a
    //   mage's 2.9 s staff for a 1.5-1.8 s Frostbolt), or from the first hit it took when that came first;
    // - [dps20] less the time THIS clock found it not attacking (fight_s - actedSec). It was the caller's reading;
    //   a fight in a dungeon has no caller that reads anything.
    // [rot609] A run too: SoloSafety's numbers stop at the run's start (its damage to then over a mage's 2.9 s staff
    // swing on top) while a bot that disengages can cast on (b341 Gilduck L9 frost: 94 dealt, 30 of them before the
    // disengage, logged 3.7-4.7 dps against 18.9); its hits after the run are in the window.
    float leadSec = 0.0f;
    bool const fled = !strcmp(sample.outcome, "fled");
    float staleSec = 0.0f;  // [dps233]
    if (f.firstDealtMs && f.raw)
    {
        SpecRotation const* rotation = RotationOf(bot);
        leadSec = std::max(float(bot->GetAttackTime(BASE_ATTACK)) / float(IN_MILLISECONDS), 0.1f);
        if (rotation && rotation->caster && f.timedCasts)
            leadSec = f.timedMs / float(f.timedCasts) / float(IN_MILLISECONDS);
        else if (bot->getClass() == CLASS_HUNTER && bot->GetWeaponForAttack(RANGED_ATTACK, true))
            leadSec = std::max(float(bot->GetAttackTime(RANGED_ATTACK)) / float(IN_MILLISECONDS), 0.1f);
        float before = leadSec;
        if (f.firstTakenMs)
        {
            uint32 const takenBefore = getMSTimeDiff(f.firstTakenMs, f.firstDealtMs);
            if (takenBefore < getMSTimeDiff(f.firstDealtMs, f.firstTakenMs))
            {
                // [dps40] the hit it took first leads its own only while it falls inside the fight SoloSafety closed:
                // `live` keeps the bot's entry until RecordFight, so a fight that never closed (a mob that despawned,
                // a group it joined) leaves its firstTakenMs behind and the next fight read it as that fight's lead.
                // [dps233] guards the hits it dealt, not the one it took, so the stale lead passed as a valid window:
                // b479 10 of 488 fights measured over up to 537.1 s of a 19.0 s fight (Sycina L25 combat 1.4 dps
                // against a formula of 40.4, Laaral L26 holy 4.4 of 43.3), and the ratio they carry is garbage
                float const takenSec = float(takenBefore) / float(IN_MILLISECONDS);
                if (takenSec <= sample.fightSec)
                    before = std::max(before, takenSec);
            }
        }
        // [dps233] its hits span more than the fight SoloSafety closes: the first ones came from a fight it never
        // closed (their formula and casts with them): b426-b429 273 fights measured over windows of up to 233 s for
        // 5-20 s fights (Orton L16 arcane 1.5 dps over 228.9 s against SoloSafety's 28.1); one global cooldown of
        // slack for the pull's first hit
        float const spanSec = float(getMSTimeDiff(f.firstDealtMs, f.lastDealtMs)) / float(IN_MILLISECONDS);
        float const slackSec = float(GCD_MAX_MS) / float(IN_MILLISECONDS);
        if (spanSec > sample.fightSec + before + slackSec)
            staleSec = spanSec - sample.fightSec;
        float const idle = fled ? 0.0f : std::max(0.0f, sample.fightSec - actedSec);
        float const window =
            float(getMSTimeDiff(f.firstDealtMs, f.lastDealtMs)) / float(IN_MILLISECONDS) + before - idle;
        if (window > 0.0f)
        {
            active = window;
            actual = float(f.raw) / window;
        }
    }
    // [dps20] the formula over the window the fight turned out to have, priced in ONE place. A caller used to pass
    // its own figure and only a fight with no caller was priced here, so the denominator of a solo row was the
    // formula over SoloSafety's clock and the denominator of a dungeon row was the formula over this one - two
    // windows, two numbers, one column. The Result is taken at the fight's first hit either way.
    float const formulaDps = FormulaOver(f.openResult, std::max(active, 0.0f));
    // [dps20] every reason here is the BOT'S OWN: what it did or failed to do, never what someone else did to the
    // mob. "a mob took damage from someone else" was the whole of why a fight in a five was never measured - in a
    // five every mob is hit by somebody else, so the rule threw away exactly the fights the 90 % rule is about. It
    // is gone; `f.shared` is kept as a fact of the row. The damage measured was always this bot's alone.
    std::string why = sample.invalidWhy;
    if (why.empty() && staleSec > 0.0f)
        why = Acore::StringFormat("its hits from {:.1f} s before the fight", staleSec);
    // [dps20] the fight opened on one map and closed on another: the bot walked, rode or was teleported out of it
    // with the fight still open, and nothing of the two places measures the other
    if (why.empty() && f.openMap && f.openMap != bot->GetMapId() + 1)
        why = Acore::StringFormat("the bot changed map during the fight (opened on {})", f.openMap - 1);
    if (why.empty() && f.swam)
        why = "swimming";
    if (why.empty() && f.looted)
        why = "looting";
    if (why.empty() && !f.raw)
        why = "never attacked";
    // [dpscal] the time it fought covers one pass of the rotation (was the fight's length: b337 Laaral L13 attacked
    // 5.3 s of an 89.9 s fight and passed).
    // [dps20] this is the honest guard a shared kill needs: a mob of a five's pull dies to the party's damage before
    // the bot's rotation comes round once, and the share such a window carries is of an opener, not of a rotation.
    // It is the bot's own window and its own cycle - no other member's doing is read.
    if (why.empty() && active < sample.cycleSec)
        why = Acore::StringFormat("shorter than one rotation ({:.1f} s)", sample.cycleSec);
    bool const valid = why.empty();
    // [dpscal] the ideal rotation's formula with the expected one's mana limit (its share of the burst)
    float const idealDps = f.formula.burst > 0.0f ? f.ideal.burst * formulaDps / f.formula.burst : 0.0f;
    std::string const spec = AiFactory::GetPlayerSpecName(bot);
    char const* ctx = ContextName(sample.ctx);
    // [dps20] the white-swing buckets, this clock's own. They tile `windowSec` by construction now - one clock
    // samples them and one clock bounds the window - so the `fit` scaling that used to squeeze a caller's buckets
    // into a caller's window is gone with the second clock that made it necessary.
    float const ableSec = float(f.ableMs) / float(IN_MILLISECONDS);
    float const rangeSec = float(f.blockMs[0]) / float(IN_MILLISECONDS);
    float const facingSec = float(f.blockMs[1]) / float(IN_MILLISECONDS);
    float const noSwingSec = float(f.blockMs[2]) / float(IN_MILLISECONDS);
    float const controlSec = float(f.blockMs[3]) / float(IN_MILLISECONDS);
    float const switchSec = float(f.blockMs[4]) / float(IN_MILLISECONDS);
    float const castingSec = float(f.blockMs[5]) / float(IN_MILLISECONDS);
    float const windowSec = ableSec + rangeSec + facingSec + noSwingSec + controlSec + switchSec + castingSec;
    // [dps20] the caster's side of the window, which the swing buckets cannot answer: the execute time of its own
    // casts (`timedMs`, the [dpsparts] busy share). A caster's `swings` and `swings_allowed` are 0 by
    // construction, so for every spec that casts, this column and `casting_s` are the whole of the diagnostic.
    float const castSec = f.timedMs / float(IN_MILLISECONDS);
    float const leadBucketSec = float(f.leadMs) / float(IN_MILLISECONDS);
    // [dps20] the shortfall stored beside the share. A share with no cause beside it says "sixty percent" and does
    // not say whether to fix it, so the row carries the reasons the [dpsparts] and [meleeup] lines print: the
    // rotation spells the formula planned and the bot never cast, and the time each reason of the core's own swing
    // gate held its swings. Ranked, biggest first, cut to the column's 255 characters.
    std::string shortfall;
    uint32 swings = 0;
    float swingsAllowed = 0.0f;
    LOG_INFO("playerbots",
             "[dps] {} L{} {}/{} ctx={} target={} L{} mobs={} formula={:.1f} ideal={:.1f} actual={:.1f} "
             "fight_s={:.1f} active_s={:.1f} taken={} outcome={} valid={} why={}",
             bot->GetName(), bot->GetLevel(), uint32(bot->getClass()), spec, ctx, sample.targetEntry,
             uint32(sample.targetLevel), sample.mobs, formulaDps, idealDps, actual, sample.fightSec, active,
             sample.taken, sample.outcome, valid ? 1 : 0, valid ? "-" : why);
    // [dpscal] the formula's parts at the first hit against what went off: no decision reads it
    {
        std::string plan;
        std::vector<std::pair<uint32, float>> parts = f.formula.parts;
        std::sort(parts.begin(), parts.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        for (auto const& [id, part] : parts)
            plan += Acore::StringFormat("{}{} {:.1f}", plan.empty() ? "" : ", ", SpellName(id), part);
        std::string idealPlan;
        std::vector<std::pair<uint32, float>> idealParts = f.ideal.parts;
        std::sort(idealParts.begin(), idealParts.end(),
                  [](auto const& a, auto const& b) { return a.second > b.second; });
        for (auto const& [id, part] : idealParts)
            idealPlan += Acore::StringFormat("{}{} {:.1f}", idealPlan.empty() ? "" : ", ", SpellName(id), part);
        std::string casts;
        for (auto const& [id, count] : f.casts)
            casts += Acore::StringFormat("{}{} x{}", casts.empty() ? "" : ", ", SpellName(id), count);
        // [dps20] the rotation's own part of the shortfall: a spell the formula priced into the plan that never went
        // off this fight. `parts` is already sorted by the damage per second it was priced at, so the first ones
        // named are the ones worth fixing. A spell cast at least once is no part of the gap here - how far short its
        // casts fell is the [dpsparts] line's plan-against-casts pair and not a single number.
        for (auto const& [id, part] : parts)
        {
            if (part <= 0.0f || f.casts.count(id))
                continue;
            if (shortfall.size() > 180)
                break;
            shortfall += Acore::StringFormat("{}no {} ({:.1f} dps planned)", shortfall.empty() ? "" : "; ",
                                             SpellName(id), part);
        }
        LOG_INFO("playerbots",
                 "[dpsparts] {} L{} {}/{} formula {:.1f} over {:.1f} s: {}{}white {:.1f}, pet {:.1f} ({}) | dealt {} "
                 "(spell {}, dot {}, white {}, pet {}) = {:.1f} dps over {:.1f} s (lead {:.1f} s), capped {:.1f} "
                 "| casts: {} | ideal {:.1f}: {} | white hits {} of {:.1f}, busy {:.0f} %, finishers {} at {:.1f} "
                 "points",
                 bot->GetName(), bot->GetLevel(), uint32(bot->getClass()), AiFactory::GetPlayerSpecName(bot),
                 formulaDps, f.formula.lifeSec, plan, plan.empty() ? "" : ", ",
                 f.formula.white - f.formula.pet - f.formula.replacedWhite, f.formula.pet, f.petOut ? "out" : "none",
                 f.raw, f.spell, f.dot,
                 f.white, f.pet, actual, active, leadSec, cappedActual, casts.empty() ? "-" : casts, idealDps,
                 idealPlan.empty() ? "-" : idealPlan, f.whiteHits, f.formula.whiteHitsPerSec * active,
                 active > 0.0f ? f.timedMs / (active * float(IN_MILLISECONDS)) * 100.0f : 0.0f, f.finishers,
                 f.finishers ? float(f.finisherPoints) / float(f.finishers) : 0.0f);
    }

    // [dps41] a melee bot's white swings: what it took, what the time it was free to swing allowed, and what the
    // formula counted - with the time each reason of the core's own swing gate held it. The formula has a melee
    // rotation in reach for the whole fight (`meleeShare` is 1 for all but the petless hunter), so the gap between
    // the swings the able time allowed and the swings the formula counted is that assumption's cost, and the gap
    // below it is the bot's. No decision reads it.
    // [dps50] all three figures are landed white hits over one window, and the allowance is the formula's own landed
    // rate over the able part of the window the buckets tile. It was main-hand swings attempted over the span of the
    // fight's damage events, which is wrong four ways against what `whiteHits` counts and against Rawr 2.3.23's own
    // white-hit denominator (Rawr.DPSWarr/Skills.Base.cs WhiteAttacks.LandedAtksOverDur = MhActivates *
    // MHAtkTable.AnyLand + OhActivates * OHAtkTable.AnyLand, each hand's activates being FightDuration / its own
    // effective speed over the whole fight; wowsims' sim/core/attack.go runs the same, one swing timer per hand from
    // t = 0 with only a cast pausing it): it dropped the off hand, so a dual wielder landed about twice the
    // allowance (b480 combat rogues read a median of 250 %, assassination 161 %); it counted a swing that missed, was
    // dodged or parried as a landed hit; it took the base attack speed, without the haste the rate carries and
    // without the swings a next-swing strike replaces; and its window was short by the lead swing [dps50] now
    // charged. A bot able for the whole window reads allowed == counted, the check the line could not make before.
    if (f.formula.whiteHitsPerSec > 0.0f && windowSec > 0.0f)
    {
        // the rate it lands while it is in melee: the able time holds no out-of-reach second already, so the petless
        // hunter's arrival share belongs to `counted`'s whole-fight assumption and not to the allowance
        float const inMelee = f.formula.meleeShare > 0.0f ? f.formula.whiteHitsPerSec / f.formula.meleeShare
                                                          : f.formula.whiteHitsPerSec;
        float const allowed = inMelee * ableSec;
        float const counted = f.formula.whiteHitsPerSec * windowSec;
        // [dps60] the swing timer's own output against what the able time allows it: a next-swing strike spends a
        // swing and lands as itself, so both sides add the replaced swings back. This is the pair that answers the
        // able time; the landed-white pair above answers the rotation on top of it. b480's prot warriors read 86 %
        // of their able time on landed white alone and 94 % with their 82 Heroic Strikes counted - 8 of the 14
        // points were the strike, and `replacedHitsPerSec` is what the rotation planned to spend against them.
        float const replaced = f.formula.meleeShare > 0.0f
                                   ? f.formula.replacedHitsPerSec / f.formula.meleeShare
                                   : f.formula.replacedHitsPerSec;
        float const timerAllowed = (inMelee + replaced) * ableSec;
        uint32 const timerSwings = f.whiteHits + f.nextSwings;
        swings = timerSwings;
        swingsAllowed = timerAllowed;
        // [dps20] the swing gate's part of the stored shortfall: every reason that held a swing for longer than a
        // tenth of the window, biggest first. These are the core's own readings, so the row says where the swings
        // went and the triage does not have to go back to the log for it.
        {
            std::vector<std::pair<char const*, float>> held = {{"out of reach", rangeSec},
                                                               {"not facing", facingSec},
                                                               {"no swing", noSwingSec},
                                                               {"controlled", controlSec},
                                                               {"swinging elsewhere", switchSec},
                                                               {"casting", castingSec}};
            std::sort(held.begin(), held.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
            for (auto const& [name, sec] : held)
            {
                if (sec <= 0.1f * windowSec || shortfall.size() > 200)
                    continue;
                shortfall += Acore::StringFormat("{}{} {:.1f} s ({:.0f} %)", shortfall.empty() ? "" : "; ", name, sec,
                                                 sec / windowSec * 100.0f);
            }
        }
        float const mhSec = std::max(float(bot->GetAttackTime(BASE_ATTACK)) / float(IN_MILLISECONDS), 0.1f);
        float const ohSec = bot->HasOffhandWeaponForAttack()
                                ? std::max(float(bot->GetAttackTime(OFF_ATTACK)) / float(IN_MILLISECONDS), 0.1f)
                                : 0.0f;
        float const swung = 1.0f / mhSec + (ohSec > 0.0f ? 1.0f / ohSec : 0.0f);
        // [dps70] a share of an allowance that is not there is not 0 %, and it is not a division either. The able
        // clock reads the core's gate at the fight's damage events and charges each interval forward, so a fight
        // with two events whose first reading was "no swing" puts its whole window in that bucket - and if the
        // bot's own swing landed at the second event, the reading taken there governs no interval at all, because
        // the window ends with it. b482 had two such fights of 73 (Almored L24 prot: 1 swing, able 0.0 of a 4.3 s
        // window, no swing 4.3; Aestymro L23 retrib: 1 swing, able 0.0 of 5.0, no swing 5.0). The window arithmetic
        // and the lead swing are sound in both - window == events + lead and nothing unaccounted, the [dps50]
        // check - so the zero is the gate's own honest reading and not a hole. It is still a contradiction: a swing
        // landed, so the bot was swinging, and the samples were too few to see it. The line says that instead of
        // printing a percent it does not have, and the triage counts these apart from its mean rather than letting
        // a false 0 % into it.
        auto const share = [](uint32 took, float of) -> std::string
        {
            if (of > 0.0f)
                return Acore::StringFormat("{:.0f} %", float(took) / of * 100.0f);
            return took ? Acore::StringFormat("no allowance: the able time measured 0 s, yet {} landed - the "
                                              "samples never caught it swinging",
                                              took)
                        : std::string("no allowance, nothing landed");
        };
        LOG_INFO("playerbots",
                 "[meleeup] {} L{} {}/{} swings {} of {:.1f} the able time allowed ({}), formula counted "
                 "{:.1f} ({}) | able {:.1f} s of {:.1f} s (events {:.1f} s + lead swing {:.1f} s; fight "
                 "{:.1f} s, dps window {:.1f} s): out of reach {:.1f}, not facing {:.1f}, no swing {:.1f}, "
                 "controlled {:.1f} (stun / fear / confusion only), casting {:.1f}, its own casts {:.1f} s, "
                 "swinging elsewhere {:.1f} | timer {} swings ({} white + {} next-swing "
                 "strikes) of {:.1f} allowed ({}), rotation planned {:.2f} replaced a second | rate {:.2f} "
                 "landed in melee ({:.2f} over the fight, melee share {:.2f}) of {:.2f} swung a second (main "
                 "{:.2f} s, off {:.2f} s) | white dealt {} of {:.0f} the formula",
                 bot->GetName(), bot->GetLevel(), uint32(bot->getClass()), spec, f.whiteHits, allowed,
                 share(f.whiteHits, allowed), counted, share(f.whiteHits, counted), ableSec, windowSec,
                 windowSec - leadBucketSec, leadBucketSec, sample.fightSec, active, rangeSec, facingSec, noSwingSec,
                 controlSec, castingSec, castSec, switchSec, timerSwings, f.whiteHits, f.nextSwings, timerAllowed,
                 share(timerSwings, timerAllowed), replaced, inMelee, f.formula.whiteHitsPerSec,
                 f.formula.meleeShare, swung, mhSec, ohSec, f.white,
                 (f.formula.white - f.formula.pet - f.formula.replacedWhite) * windowSec);
    }

    // async (the module pool's own thread), never read by a decision; the last 1000 fights per bot are kept
    std::string escapedName = bot->GetName();
    std::string escapedWhy = why;
    // [guild15] compile fix: the module pool has no EscapeString; \ and ' escaped by hand
    auto const escape = [](std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        for (char const c : text)
        {
            if (c == '\\' || c == '\'')
                out += '\\';
            out += c;
        }
        text.swap(out);
    };
    std::string escapedShortfall = shortfall.size() > 250 ? shortfall.substr(0, 250) : shortfall;
    escape(escapedName);
    escape(escapedWhy);
    escape(escapedShortfall);
    // [dps20] the map and the spot the fight OPENED on, not the ones the bot stands on now. Every one of the 23 rows
    // this table held on a dungeon map was an open-world fight carried through a portal: the close read
    // bot->GetMapId() and stamped the dungeon on a fight against a Mottled Raptor.
    uint32 const map = f.openMap ? f.openMap - 1 : bot->GetMapId();
    float const atX = f.openMap ? f.openX : bot->GetPositionX();
    float const atY = f.openMap ? f.openY : bot->GetPositionY();
    float const atZ = f.openMap ? f.openZ : bot->GetPositionZ();
    uint32 const guid = bot->GetGUID().GetCounter();
    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();
    trans->Append(Acore::StringFormat(
        "INSERT INTO playerbots_fight_history (guid, name, level, class, spec, ctx, target_entry, target_level, "
        "mobs, formula_dps, ideal_dps, actual_dps, fight_s, active_s, window_s, able_s, range_s, facing_s, "
        "no_swing_s, control_s, switch_s, casting_s, cast_s, assumed_buffs, swings, swings_allowed, valid, why, "
        // [fight21] `run_id` HAD A COLUMN, AN INDEX AND NO WRITER: empty in all 75 074 rows, so every join of
        // a measured fight to the run it happened in was a join BY TIME - and two fives measure fights in the
        // same second, so that join could not be honest. The fact was in this function's hand the whole time
        // (the bot), and the registry that answers it is built to be read from a map thread
        // (Leader::Runs::OfBot, [jrn40]). Empty still means "this bot is in no run", which is the ordinary
        // case for a bot levelling alone - never "we lost it".
        "shared, shortfall, taken, outcome, map, x, y, z, run_id) VALUES "
        "({}, '{}', {}, {}, '{}', '{}', {}, {}, {}, {:.2f}, {:.2f}, {:.2f}, {:.2f}, {:.2f}, {:.2f}, {:.2f}, {:.2f}, "
        "{:.2f}, {:.2f}, {:.2f}, {:.2f}, {:.2f}, {:.2f}, {}, {}, {:.2f}, {}, '{}', {}, '{}', {}, '{}', {}, {:.1f}, "
        "{:.1f}, {:.1f}, '{}')",
        guid, escapedName, uint32(bot->GetLevel()), uint32(bot->getClass()), spec, ctx, sample.targetEntry,
        uint32(sample.targetLevel), sample.mobs, formulaDps, idealDps, actual, sample.fightSec, active, windowSec,
        ableSec, rangeSec, facingSec, noSwingSec, controlSec, switchSec, castingSec, castSec,
        uint32(f.openResult.assumedBuffs), swings, swingsAllowed, valid ? 1 : 0, escapedWhy, f.shared ? 1 : 0,
        escapedShortfall, sample.taken, sample.outcome, map, atX, atY, atZ, Leader::Runs::OfBot(bot)));
    trans->Append(Acore::StringFormat(
        "DELETE h FROM playerbots_fight_history h JOIN (SELECT id FROM playerbots_fight_history WHERE guid = {} "
        "ORDER BY id DESC LIMIT 1 OFFSET 1000) old ON h.id <= old.id WHERE h.guid = {}",
        guid, guid));
    PlayerbotsDatabase.CommitTransaction(trans);
}

// [dps20] a fight nobody else closes. SoloSafety::CloseFight owns a solo bot's fight out in the world and closes it
// from its own judge; every other fight - in a party, in a raid, in any instance - reached no close at all, which is
// why `ctx=group` holds 17 of 75 074 rows and a dungeon holds none. This closes those, from the per-bot tick, once
// the bot is out of combat or dead. No timer and no threshold: the fight ends when the core says the bot is out of
// combat, and the sample is written then.
void CloseIfOut(Player* bot)
{
    if (!MeasuredBot(bot) || (bot->IsInCombat() && bot->IsAlive()))
        return;
    // a solo bot's open fight belongs to SoloSafety's judge, which closes it with its own facts (a survival run,
    // the engage it came from). One close per fight. Read before the lock: it walks the bot's group.
    if (SoloBot(bot))
        return;
    FightSample sample;
    {
        LiveShard& shard = ShardOf(bot->GetGUID().GetRawValue());
        std::lock_guard<std::mutex> guard(shard.lock);
        auto const found = shard.fights.find(bot->GetGUID().GetRawValue());
        if (found == shard.fights.end())
            return;
        LiveFight const& f = found->second;
        if (!f.sampleMs)
            return;  // an entry with no damage event of its own: nothing to measure
        sample.targetEntry = f.openEntry;
        sample.targetLevel = f.openLevel;
        sample.mobs = uint32(f.mobs.size());
        sample.cycleSec = f.openResult.cycleSec;
        sample.dealt = f.raw;
        sample.taken = f.taken;
        sample.ctx = f.openCtx;
        // the fight's length is the window its own samples tile: the span of its damage events plus the swing the
        // first of them closed. RecordFight holds the active part of it against this.
        sample.fightSec = float(f.ableMs + f.blockMs[0] + f.blockMs[1] + f.blockMs[2] + f.blockMs[3] + f.blockMs[4] +
                                f.blockMs[5]) /
                          float(IN_MILLISECONDS);
    }
    sample.outcome = bot->IsAlive() ? "won" : "died";
    RecordFight(bot, sample);
}

// [dps20] a bot leaving the world drops its open fight. Only a solo bot's fight was ever tracked and only
// RecordFight ever erased an entry, so a fight open at a logout stayed in the store for the server's life - with
// two Profiles and their vectors in it. Tracking every bot everywhere makes that a leak worth closing. There is
// nothing to measure in a fight that ended with the bot going away.
void ForgetFight(Player* bot)
{
    if (!bot)
        return;
    LiveShard& shard = ShardOf(bot->GetGUID().GetRawValue());
    std::lock_guard<std::mutex> guard(shard.lock);
    shard.fights.erase(bot->GetGUID().GetRawValue());
}

void NoteDealt(Unit* attacker, Unit* victim, uint32 damage, uint8 damageType)
{
    if (!attacker || !victim || !damage)
        return;
    uint32 const now = getMSTime();
    // a hit it takes from a mob (or the mob's pet / totem)
    if (Player* bot = victim->ToPlayer())
    {
        Unit* source = attacker->GetCharmerOrOwnerOrSelf();
        Creature* hitter = source ? source->ToCreature() : nullptr;
        if (!hitter || !MeasuredBot(bot))
            return;
        LiveShard& shard = ShardOf(bot->GetGUID().GetRawValue());
        std::lock_guard<std::mutex> guard(shard.lock);
        LiveFight& f = shard.fights[bot->GetGUID().GetRawValue()];
        OpenFacts(bot, f, hitter);
        if (!f.firstTakenMs)
            f.firstTakenMs = now ? now : 1;
        // [dps20] overkill is no damage taken: the hit that kills takes its health and no more, the same cap the
        // damage it deals carries per mob
        f.taken += std::min<uint64>(damage, bot->GetHealth());
        SampleFight(bot, hitter, f, now);
        return;
    }
    // a hit it (its pet, its totem) deals to a mob
    Creature* mob = victim->ToCreature();
    Player* bot = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!mob || !bot || mob->GetCharmerOrOwnerPlayerOrPlayerItself() || !MeasuredBot(bot))
        return;
    uint32 const health = mob->GetHealth();
    uint32 const maxHealth = mob->GetMaxHealth();
    bool first = false;
    {
        LiveShard& shard = ShardOf(bot->GetGUID().GetRawValue());
        std::lock_guard<std::mutex> guard(shard.lock);
        LiveFight& f = shard.fights[bot->GetGUID().GetRawValue()];
        OpenFacts(bot, f, mob);
        first = !f.firstDealtMs;
        if (first)
            f.firstDealtMs = now ? now : 1;
        f.lastDealtMs = now;
        f.raw += damage;
        if (attacker != bot)
            f.pet += damage;
        else if (damageType == DOT)
            f.dot += damage;
        else if (damageType == DIRECT_DAMAGE)
        {
            f.white += damage;
            ++f.whiteHits;  // [dps238]
        }
        else
            f.spell += damage;
        // its health (before this hit) below what this bot took off it: someone else hit it
        uint64& mine = f.mobs[mob->GetGUID().GetRawValue()];
        if (uint64(health) + mine < uint64(maxHealth))
            f.shared = true;
        mine += std::min(damage, health);
        // after the mob is in `f.mobs`: a swing at it is a swing of this fight, not one "elsewhere"
        SampleFight(bot, mob, f, now);
    }
    if (!first)
        return;
    // the formula at the fight's first hit (the profile SoloSafety's open reads: cached, the same signature)
    BuffContext const ctx = ContextOf(bot);
    Profile const formula = ProfileOf(bot, mob, ctx);
    Profile const ideal = ProfileOf(bot, mob, ctx, true);
    // [dps20] the same profile as a Result, for FormulaOver: the mana limit over the window the fight turns out to
    // have. A fight no solo judge closes has no other place to take it from.
    Result const open = Estimate(bot, mob, ctx);
    bool const petOut = bot->GetPet() && bot->GetPet()->IsAlive();
    LiveShard& shard = ShardOf(bot->GetGUID().GetRawValue());
    std::lock_guard<std::mutex> guard(shard.lock);
    LiveFight& f = shard.fights[bot->GetGUID().GetRawValue()];
    f.formula = formula;
    f.ideal = ideal;
    f.openResult = open;
    f.petOut = petOut;
    // the row's target is the mob the formula was priced against, not whichever one hit first
    f.openEntry = mob->GetEntry();
    f.openLevel = uint8(mob->GetLevel());
}

void NoteCast(Player* player, SpellInfo const* info)
{
    if (!player || !info || info->IsPassive() || !MeasuredBot(player))
        return;
    bool const harmful = !info->IsPositive();
    // a buff before the pull is no part of the fight; its opener (cast before it is in combat) is
    if (!harmful && !player->IsInCombat())
        return;
    float executeMs = 0.0f;
    if (harmful && !info->IsAutoRepeatRangedSpell())
    {
        float const castMs = info->IsChanneled() ? float(std::max(info->GetDuration(), 0))
                                                 : float(info->CalcCastTime(player));
        executeMs = std::max(castMs, float(info->StartRecoveryTime));
    }
    LiveShard& shard = ShardOf(player->GetGUID().GetRawValue());
    std::lock_guard<std::mutex> guard(shard.lock);
    LiveFight& f = shard.fights[player->GetGUID().GetRawValue()];
    ++f.casts[info->Id];
    if (info->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE))  // [dps60]
        ++f.nextSwings;
    if (info->NeedsComboPoints())  // [dps238] its points before the finisher takes them
    {
        ++f.finishers;
        f.finisherPoints += player->GetComboPoints();
    }
    if (executeMs > 0.0f)
    {
        ++f.timedCasts;
        f.timedMs += executeMs;
    }
}
}  // namespace DpsEstimate
