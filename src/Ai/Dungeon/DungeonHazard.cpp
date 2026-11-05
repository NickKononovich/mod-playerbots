/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DungeonHazard.h"
#include "BotPath.h"  // [lead109]
#include "BossKiteRooms.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DungeonTactics.h"
#include "DynamicObject.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "KillOrder.h"  // [b32_engage]
#include "Map.h"
#include "MoveSpline.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "Timer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <unordered_map>
#include <utility>

namespace DungeonHazard
{
namespace
{
enum class HazardSource : uint8
{
    Creature,
    DynObject,
    GameObj,
};

enum class HazardRoles : uint8
{
    All,
    NonTank,
    NonMelee,
};

using HazardCondition = bool (*)(Creature* creature, Player* bot);
using GameObjectHazardCondition = bool (*)(GameObject* object, Player* bot);

struct HazardSpec
{
    uint32 mapId;
    HazardSource source;
    uint32 id;  // creature entry, persistent area aura spell or gameobject entry
    float radius;
    HazardRoles roles;
    HazardCondition condition;  // creatures only, nullptr = always
    bool hold = true;           // DungeonHazardHoldMultiplier keeps the bot out once it has left
    bool chaseToTank = false;   // a mob that chases its victim: the victim runs to the tank, not away
    // The hold only applies while the bot fights: out of combat a follower must be able to walk past the hazard.
    bool holdInCombatOnly = false;
    GameObjectHazardCondition objectCondition = nullptr;  // gameobjects only, nullptr = always
    // [boss_puddles] creatures only: the hazard's current radius when it changes over time (nullptr = `radius`)
    float (*radiusOf)(Creature* creature) = nullptr;
    // [b44_st] creatures only: a chaser nobody can hold or kill; its victim keeps KITE_VICTIM_EXTRA more away and
    // walks round it (Circle::kite)
    bool kiteVictim = false;
    // [lead1483] creatures only: the spell that hits inside (its normal rank; the caster's difficulty picks the
    // heroic one) and how its hits come (blastsOf: the ms they still come and between them), for the leader's verdict
    uint32 spell = 0;
    void (*blastsOf)(Creature* creature, int32& leftMs, uint32& periodMs) = nullptr;
};

enum DungeonHazardIds : uint32
{
    MAP_GNOMEREGAN = 90,
    MAP_SUNKEN_TEMPLE = 109,  // [b30_st_spirit]
    MAP_BLACKROCK_SPIRE = 229,
    MAP_MARAUDON = 349,
    MAP_BLOOD_FURNACE = 542,
    MAP_ARCATRAZ = 552,
    MAP_MECHANAR = 554,
    MAP_MANA_TOMBS = 557,
    MAP_UTGARDE_KEEP = 574,
    MAP_UTGARDE_PINNACLE = 575,
    MAP_HALLS_OF_STONE = 599,
    MAP_HALLS_OF_LIGHTNING = 602,
    MAP_GUNDRAK = 604,
    MAP_NEXUS = 576,
    MAP_AHNKAHET = 619,
    MAP_PIT_OF_SARON = 658,
    MAP_OCULUS = 578,
    MAP_STEAMVAULT = 545,
    MAP_AUCHENAI_CRYPTS = 558,  // [lead1483]
    // [boss_puddles] maps whose boss ground effects were added from the BOSS_PUDDLES.md audit
    MAP_SHATTERED_HALLS = 540,
    MAP_OLD_HILLSBRAD = 560,
    MAP_DRAKTHARON_KEEP = 600,
    MAP_AZJOL_NERUB = 601,
    MAP_FORGE_OF_SOULS = 632,

    // Mechanar, Capacitus: Nether Charge Timer 37670 fires Nether Detonation 35152 (15 yd) 16.5 s after spawn; the
    // charge wanders for its first 8.5 s, then stands still and pulses.
    NPC_NETHER_CHARGE = 20405,
    SPELL_NETHER_CHARGE_TIMER = 37670,

    // Arcatraz: the Sentinel carries Energy Discharge 36716 from its spawn (creature_template_addon), a 1 s periodic
    // trigger of 36717 (563-938 arcane, 15 yd, Spell.dbc radius index 18): it pulses while it still lies dormant
    // (Permanent Feign Death 31261), which is what pulled it on whoever walked past (wave 8: the hunter, 14.8 yd).
    // Runs at 1.43 (10 yd/s), so its victim cannot outrun it. At 10 % it is stunned by Explode 36719 (36722, 10 yd,
    // 6 s later), dies and leaves a Destroyed Sentinel 21761 with the same 36716 aura.
    NPC_ARCATRAZ_SENTINEL = 20869,
    NPC_DESTROYED_SENTINEL = 21761,
    SPELL_SENTINEL_EXPLODE = 36719,

    // Blackrock Spire, the Rookery: Rookery Egg is an environmental trap (type 6, diameter 3, 120 s respawn): any
    // player within 1.5 yd plus both sizes (3D) hatches it into a Rookery Whelp (15745) that attacks (heroic waves 8
    // and 9: 20-28 whelps, full wipes). Its trap search ignores creatures, so only the group's own steps hatch it.
    GO_ROOKERY_EGG = 175124,

    // Mana-Tombs: Mana Leech casts Arcane Explosion 34933 (10 yd) on death.
    NPC_MANA_LEECH = 19306,

    // Gnomeregan: Dark Iron Land Mine detonates (4043, 10 yd) on anything within 6 yd; Irradiated Slime drops
    // Radiation Cloud 10341 under itself.
    NPC_DARK_IRON_LAND_MINE = 8035,
    SPELL_RADIATION_CLOUD = 10341,

    // Maraudon: Noxious Slime drops Noxious Cloud 21070 under itself.
    SPELL_NOXIOUS_CLOUD = 21070,

    // Maraudon: Creeping Sludge (heroic copy 712222) carries Poison Shock 22638 from creature_template_addon, a 2 s
    // periodic trigger of Poison Shock 22595 (5 yd around the sludge). Heroic wave 8: four sludges stacked on the
    // group killed four members with it. It crawls at 0.29 speed, so stepping out is easy.
    NPC_CREEPING_SLUDGE = 12222,
    NPC_CREEPING_SLUDGE_HEROIC = 712222,
    SPELL_SLUDGE_POISON_SHOCK_AURA = 22638,

    // Blood Furnace: Shadowmoon Technician's Throw Proximity Bomb (30846 / heroic 32784) drops the trap 181877 /
    // 182607 within 3 yd of a random player for 120 s. It arms after 4 s (gameobject_template data7) and fires on
    // the first unit within 2.5 yd (data2 = diameter 5): Proximity Bomb Explode 30844 / 32786 hits everyone in 10 yd.
    GO_PROXIMITY_BOMB = 181877,
    GO_PROXIMITY_BOMB_HEROIC = 182607,

    // The Nexus, Ormorok: Crystal Spikes summons rings of Crystal Spike 27099 (5 yd apart, 4 per ring, random
    // directions, TEMPSUMMON_TIMED_DESPAWN 7 s); each casts Crystal Spike 47944 (3 yd, 250 ms) 2 s after it appears
    // (boss_ormorok.cpp npc_crystal_spike), then stands inert until it despawns.
    NPC_CRYSTAL_SPIKE = 27099,

    // Utgarde Pinnacle, Skadi gauntlet: the Flame Breath Triggers under Grauf's breath pass cast Freezing Cloud
    // 47579 / heroic 60020 (persistent area aura, 3 yd, 10 s) on themselves (boss_skadi.cpp).
    SPELL_SKADI_FREEZING_CLOUD = 47579,
    SPELL_SKADI_FREEZING_CLOUD_HEROIC = 60020,

    // Ahn'kahet, Prince Taldaram: Conjure Flame Sphere summons 1 (heroic 3) Flame Spheres at him; 3 s later each
    // starts Flame Sphere Periodic 55926 (every 200 ms Sear Beam 55927: 417-484 fire, 18 yd, radius index 19) and
    // walks 25 yd out (boss_prince_taldaram.cpp), then despawns. Wave 8: 17.5k Sear Beam on a shaman in 15 s.
    NPC_FLAME_SPHERE_1 = 30106,
    NPC_FLAME_SPHERE_2 = 31686,
    NPC_FLAME_SPHERE_3 = 31687,

    // [b33_ok] Ahn'kahet heroic, Amanitar: a Poisonous Mushroom 30435 that sees a player within 3 yd (SmartAI IC_LOS,
    // edge distance) casts Poison Cloud 57061 (3237 nature a tick for 8 s on everyone within 5 yd) and despawns.
    NPC_AMANITAR_POISONOUS_MUSHROOM = 30435,

    // Pit of Saron trash: Plagueborn Horror casts Blight Bomb 69582 (5 s, 20 yd) and dies with it.
    NPC_PLAGUEBORN_HORROR = 36879,
    SPELL_BLIGHT_BOMB = 69582,

    // Utgarde Keep, Ingvar: Shadow Axe 42748 summons the Ingvar Throw Dummy (42749), which walks to a random player
    // and carries Throw Axe 42750, a 1 s periodic trigger of Shadow Axe 42751 (1750-2250 shadow, 5 yd) for 10 s,
    // then charges back to Ingvar (boss_ingvar_the_plunderer.cpp EVENT_AXE_RETURN). Wave 10: it killed the healer.
    NPC_INGVAR_THROW_DUMMY = 23997,

    // Gundrak, Living Mojo (npc_living_mojo, trash only): Mojo Puddle 55627 / heroic 58994 every 13 s, a persistent
    // area aura of 1615-1785 nature per second, 3 yd, 10 s.
    SPELL_MOJO_PUDDLE = 55627,
    SPELL_MOJO_PUDDLE_HEROIC = 58994,
    // Drakkari Colossus, heroic Drakkari Elemental 29573: Mojo Volley 59453 hits two players every 2 s (59452) and
    // each hit drops Mojo Puddle 59451 at that player's feet (808-893 nature per second, 3 yd, 10 s; the caster is the
    // player, deaths.jsonl names him as his own killer). Normal 54847 drops none. gd-h 20261025-150737: the healer
    // stood in 5.3k of it in 15 s and died 29 s into the Elemental, then all four ("environment").
    SPELL_MOJO_PUDDLE_ELEMENTAL_HEROIC = 59451,

    // Halls of Stone, Unrelenting Construct (SmartAI 27971): below 25 % it casts Short Circuit 51832 / heroic 61513
    // (3 s, 10 yd, 4.6-5.4k / 8.8-10.2k nature; PreventionType 0, so no kick) and dies 3.5 s later.
    NPC_UNRELENTING_CONSTRUCT = 27971,
    SPELL_SHORT_CIRCUIT = 51832,
    SPELL_SHORT_CIRCUIT_HEROIC = 61513,

    // Halls of Lightning, Slag (SmartAI 28585): Blast Wave 23113 / heroic 22424 (10 yd) on its own death.
    NPC_SLAG = 28585,
    // Ionar: below 50 % Disperse; five Sparks of Ionar follow random players (run speed 0.57 / 0.7, slower than a
    // player) and burn whoever stands within 5 yd every second (Arcing Burn 52671 / 59834, +2 % nature damage taken
    // per stack); 20 s later they run back to where he vanished, and he reforms there 5 s after that.
    NPC_IONAR = 28546,
    NPC_SPARK_OF_IONAR = 28926,

    // The Oculus, Drakos: Unstable Spheres (2 every 2 s, TEMPSUMMON_TIMED_DESPAWN 18 s) wander for 10 s, then stand
    // (boss_drakos.cpp npc_oculus_unstable_sphere); Unstable Sphere Timer 50758 fires Sphere Detonation 50759 (12 yd,
    // Spell.dbc radius index 32) 17 s after spawn. Wave 11: three of them killed the healer.
    NPC_UNSTABLE_SPHERE = 28166,

    // The Oculus, Ring-Lord Sorceress (SmartAI 27639, RESEARCH_TACTICS.md section 13): the Blizzard channel 50715 /
    // 59278 (8 yd, 903 frost every 2 s + slow, aimed at ranged and healers: the main killer of the dps there) and the
    // burning patch of Flamestrike 16102 / 61402 (5 yd); both are persistent area auras.
    SPELL_RING_LORD_BLIZZARD = 50715,
    SPELL_RING_LORD_BLIZZARD_HEROIC = 59278,
    SPELL_RING_LORD_FLAMESTRIKE = 16102,
    SPELL_RING_LORD_FLAMESTRIKE_HEROIC = 61402,
    // Ring-Lord Conjurer: Charged Skin 50717 / 59276 on aggro, every 3 s 760 arcane on every enemy within 5 yd.
    NPC_RING_LORD_CONJURER = 27640,
    SPELL_CHARGED_SKIN = 50717,
    SPELL_CHARGED_SKIN_HEROIC = 59276,

    // The Steamvault, Coilfang Sorceress (SmartAI 17722, section 12): Blizzard 39416 / heroic 31581 (8 yd).
    SPELL_COILFANG_BLIZZARD = 39416,
    SPELL_COILFANG_BLIZZARD_HEROIC = 31581,

    // The Steamvault, Bog Overlord 21694 (heroic 21914, creature_template_addon auras): Disease Cloud 37266 / heroic
    // 37863, a 2 s periodic trigger of 37267 (369 nature, 5 yd) / 37864 (739 nature, 7 yd, radius index 37) around
    // it, in and out of combat; with its melee, Poison Bolt and the 50 yd Trample it killed most of the sv-h groups
    // on the trash (deaths.jsonl 2026-10-23/24). Ranged and healers never stand in it.
    NPC_BOG_OVERLORD = 21694,
    SPELL_BOG_OVERLORD_DISEASE_CLOUD = 37266,
    SPELL_BOG_OVERLORD_DISEASE_CLOUD_HEROIC = 37863,

    // [boss_puddles] Boss ground effects from the BOSS_PUDDLES.md audit (Spell.dbc + boss scripts). The generic
    // "avoid aoe" already steps out of most of them once the bot stands inside; these rows add the hold (no walking
    // back in, a melee bot waits for the tank to drag the boss out) and the short, idle-pack-aware exit.

    // Blood Furnace, Broggok: Poison Cloud 30916 every 20 s summons a Broggok Poison Cloud 17662 at his feet (60 s,
    // not selectable, trigger) carrying Poison 30914 / heroic 38462, a 1 s pulse of 30915 / 38463 (602-698 /
    // 2313-2687 nature, 12 yd base). spell_broggok_poison_cloud scales the pulse radius with the aura's progress:
    // (tick / total * 0.9 + 0.1) * 2/3 of 12 yd, so a cloud grows from 0.8 to 8 yd over its minute. Up to three
    // stand at once: the tank walks him out of them (BossKiteRooms).
    NPC_BROGGOK_POISON_CLOUD = 17662,
    SPELL_BROGGOK_POISON = 30914,
    SPELL_BROGGOK_POISON_HEROIC = 38462,

    // The Steamvault, Hydromancer Thespia: Lightning Cloud 25033 on a random player, PAA 10 yd, 14 s, 1838-2362
    // nature every 2 s (both difficulties use the one id).
    SPELL_THESPIA_LIGHTNING_CLOUD = 25033,

    // Ahn'kahet, Jedoga Shadowseeker: Thundershock 56926 / heroic 60029 on a random player, PAA 5 yd, 10 s.
    SPELL_JEDOGA_THUNDERSHOCK = 56926,
    SPELL_JEDOGA_THUNDERSHOCK_HEROIC = 60029,

    // The Arcatraz, Wrath-Scryer Soccothrates: Felfire 35769 (PAA 5 yd, 10 s, 1045-1155 fire per second) along his
    // charge line. Zereketh the Unbound: Void Zone 36119 summons an Unbound Void Zone 21101 (not selectable, 25 s)
    // under a player with Consumption 36120, 1350-1650 shadow every second in 3 yd.
    SPELL_SOCCOTHRATES_FELFIRE = 35769,
    NPC_UNBOUND_VOID_ZONE = 21101,

    // Pit of Saron, Ick: Toxic Waste 69024 / heroic 70436 under a random player, PAA 4 yd, 10 s.
    SPELL_ICK_TOXIC_WASTE = 69024,
    SPELL_ICK_TOXIC_WASTE_HEROIC = 70436,
    // [b33_pos] Pit of Saron, Tyrannus: Rimefang's Icy Blast 69232 (a random player every 5 s) leaves Icy Blast 69238 /
    // heroic 69628, PAA 7 yd, 60 s, ~2.3k frost a tick and 75 % slow: up to 12 puddles at once on the platform
    // (pos-h 20261026-000434-5B-39 / pos-n 5H-54: rogue, hunter and healer died standing in them).
    SPELL_RIMEFANG_ICY_BLAST = 69238,
    SPELL_RIMEFANG_ICY_BLAST_HEROIC = 69628,

    // Utgarde Pinnacle, King Ymiron: the Spirit Fount 27339 (trigger, run 0.29) carries Spirit Fount 48380 / 59320,
    // 1414-1586 shadow every second in 7 yd, for the rest of the fight (boss_ymiron.cpp, Bjorn's boat).
    NPC_YMIRON_SPIRIT_FOUNT = 27339,

    // Halls of Stone, Maiden of Grief: Storm of Grief 50752 / heroic 59772 on her victim, PAA 9 yd, 20 s
    // (HoSStrategy.cpp left it to the generic "avoid aoe"): the tank drags her out.
    SPELL_STORM_OF_GRIEF = 50752,
    SPELL_STORM_OF_GRIEF_HEROIC = 59772,

    // Blackrock Spire: General Drakkisath's Flamestrike 16419 and the Firebrand Invokers' Flamestrike 12468 (both
    // PAA 5 yd, 8 s; 12468 was in 7 LBRS deaths of deaths.jsonl).
    SPELL_DRAKKISATH_FLAMESTRIKE = 16419,
    SPELL_INVOKER_FLAMESTRIKE = 12468,

    // Azjol-Nerub, Hadronox: Acid Cloud 53400 / heroic 59419 on a random player, PAA 5 yd, 90 s, 707-793 nature per
    // second (heroic ~4.6k): the clouds pile up on her web platform; 6 an-h deaths with it as the main damage.
    SPELL_HADRONOX_ACID_CLOUD = 53400,
    SPELL_HADRONOX_ACID_CLOUD_HEROIC = 59419,

    // Drak'Tharon Keep, The Prophet Tharon'ja: Poison Cloud 49548 / heroic 59969 on a random player, PAA 8 yd, 10 s.
    SPELL_THARONJA_POISON_CLOUD = 49548,
    SPELL_THARONJA_POISON_CLOUD_HEROIC = 59969,

    // The Forge of Souls, Devourer of Souls: Well of Souls 68820 leaves a Well of Souls 36536 (not selectable, for the
    // rest of the fight) with Well of Souls 68854, 2925-3075 shadow every second in 4 yd.
    NPC_WELL_OF_SOULS = 36536,

    // The Shattered Halls, Grand Warlock Nethekurse: Shadow Fissure 30496 summons a Lesser Shadow Fissure 17471 (not
    // selectable, 26 s) under a player with Consumption 30497, 1000 shadow every second in 3 yd.
    NPC_LESSER_SHADOW_FISSURE = 17471,

    // Old Hillsbrad, Captain Skarloc: Consecration 38385 at his own feet, PAA 10 yd, 20 s, 750 holy per second.
    SPELL_SKARLOC_CONSECRATION = 38385,

    // [b30_st_spirit] Sunken Temple: an Atal'ai Deathwalker 5271 casts Summon Atal'ai Deathwalker's Spirit 12095 on
    // death (SmartAI). The spirit 8317 lives 15 s (SpellDuration 8), is immune to all damage (auras 34311/39804) and
    // to every school, runs at 0.29 (2 yd/s), hits at DamageModifier 7.5 and sets itself in combat with the zone.
    // It cannot be killed or held (taunts give no lasting threat: no ability damages it): players walk away from it
    // until it fades (st-n 20261025-184229-st-n-5F-11: rogue, druid healer twice and warlock died to it in melee).
    NPC_DEATHWALKER_SPIRIT = 8317,
    NPC_DEATHWALKER_SPIRIT_HEROIC = 708317,
    // [lead1483] Auchenai Crypts, Shirrak the Dead Watcher's Focus Fire (boss_shirrak_the_dead_watcher.cpp): a flare
    // summoned on a random player (TEMPSUMMON_TIMED_DESPAWN 7000 ms); 3, 3.5 and 4 s after it his Focus Cast (32300)
    // hits the flare and the flare casts Fiery Blast (32302, heroic 38382 by SpellDifficulty) on itself: 12 yd
    // (Spell.dbc radius), school damage. b269 ac-h 5G-15 382 s: the arms warrior died in it (killer 18374) - the
    // leader saw no hazard (no aura, no cast bar, no area aura: only the boss script knows the blasts)
    NPC_FOCUS_FIRE = 18374,
    SPELL_FIERY_BLAST = 32302,
};

constexpr int32 NETHER_CHARGE_ARMED_MS = 9500;  // timer aura left once the charge has stopped wandering
constexpr float SENTINEL_EXPLODE_PCT = 10.0f;
constexpr float MANA_LEECH_LOW_PCT = 25.0f;
constexpr float HAZARD_SCAN_RANGE = 30.0f;
constexpr float HAZARD_SAFETY = 1.0f;
constexpr float SENTINEL_DISCHARGE_RADIUS = 15.0f;
constexpr float SEAR_BEAM_RADIUS = 18.0f;
constexpr float SPHERE_DETONATION_RADIUS = 12.0f;
// Despawn timer left once the sphere has stopped wandering: 18000 ms lifetime - 10000 ms walk, plus some slack.
constexpr uint32 UNSTABLE_SPHERE_STANDING_TIMER_MS = 8500;
constexpr float CHARGED_SKIN_RADIUS = 5.0f;
// The heroic cloud's 7 yd (the normal one is 5 yd): one radius for both.
constexpr float BOG_OVERLORD_CLOUD_RADIUS = 7.0f;
// Melee kill the Conjurer after the Sorceresses: they hit it while healthy and step out of the pulse when hurt.
constexpr float CHARGED_SKIN_MELEE_LEAVE_PCT = 60.0f;
constexpr float CHARGED_SKIN_MELEE_RETURN_PCT = 85.0f;
// Melee keep hitting a pulsing Sentinel until this low, step out and come back once healed up to the second value
// (the position is the hysteresis: a bot already outside keeps the circle until it is healed).
constexpr float SENTINEL_MELEE_LEAVE_PCT = 70.0f;
constexpr float SENTINEL_MELEE_RETURN_PCT = 90.0f;
// The egg trap's 1.5 yd plus the egg's own size; Collect adds the bot's.
constexpr float ROOKERY_EGG_RADIUS = 2.0f;
constexpr float ROOKERY_EGG_MAX_DZ = 4.0f;  // the Rookery has eggs on its upper ledges too
constexpr float SLUDGE_POISON_SHOCK_RADIUS = 5.0f;
constexpr float SLUDGE_MELEE_LEAVE_PCT = 60.0f;
constexpr float SLUDGE_MELEE_RETURN_PCT = 85.0f;
constexpr float PROXIMITY_BOMB_BLAST_RADIUS = 10.0f;
constexpr float PROXIMITY_BOMB_TRIGGER_RADIUS = 2.5f;
// The Crystal Spike circle is the spell's 3 yd less a yard: Collect adds the bot's body and the trigger a yard of
// safety, so the trigger edge is exactly where 47944 hits (3 yd + the bot's body). A bot stacked in Ormorok's centre
// (4.7 yd from the 5 yd ring) stays put; the exit itself still clears the full radius by EXIT_MARGIN.
constexpr float CRYSTAL_SPIKE_RADIUS = 2.0f;
// Despawn timer left once the spike has fired: 7000 ms lifetime - 2000 ms delay - 250 ms cast, less some slack.
constexpr uint32 CRYSTAL_SPIKE_FIRED_TIMER_MS = 4600;
// A chased bot runs to the tank until this close (the tank takes the mob over with a taunt).
constexpr float CHASE_TANK_DISTANCE = 6.0f;
constexpr float CHASE_TANK_MAX_DISTANCE = 60.0f;
// [b44_st] The Deathwalker's Spirit walks 2 yd/s: its victim keeps 3 s of that walk more away than its reach, so a
// heal cast between two steps does not let it close in. st-h 20261026-075135-st-h-5F-7 (3195-3221 s): four spirits
// on the resto druid; it stepped 3 yd out of the 8 yd circle at a time, "move to tank" (chased away from tank) led it
// back past them twice, and it died from 77 % within a second.
constexpr float KITE_VICTIM_EXTRA = 6.0f;
// [lead1483] the flare's life and its blasts (boss_shirrak_the_dead_watcher.cpp: SummonCreature ... 7000, the Focus
// Cast schedules at 3000 / 3500 / 4000 ms) and Fiery Blast's radius (Spell.dbc 32302 / 38382: 12 yd)
constexpr uint32 FOCUS_FIRE_LIFE_MS = 7000;
constexpr uint32 FOCUS_FIRE_LAST_BLAST_MS = 4000;
constexpr uint32 FOCUS_FIRE_BLAST_EVERY_MS = 500;
constexpr float FIERY_BLAST_RADIUS = 12.0f;

bool NetherChargeArmed(Creature* charge, Player* /*bot*/)
{
    Aura* timer = charge->GetAura(SPELL_NETHER_CHARGE_TIMER);
    return !timer || timer->GetDuration() <= NETHER_CHARGE_ARMED_MS;
}

bool SentinelExploding(Creature* sentinel, Player* /*bot*/)
{
    return sentinel->HasAura(SPELL_SENTINEL_EXPLODE) || sentinel->GetHealthPct() <= SENTINEL_EXPLODE_PCT;
}

// A pulsing mob the melee have to hit: ranged and healers never stand in it; melee hit it until hurt below
// `leavePct`, then wait outside until healed up to `returnPct` (the position is the hysteresis: a bot already outside
// keeps the circle until it is healed).
bool MeleeKeepsOut(Creature* creature, Player* bot, float radius, float leavePct, float returnPct)
{
    if (!PlayerbotAI::IsMelee(bot))
        return true;

    float const health = bot->GetHealthPct();
    if (health < leavePct)
        return true;

    float const reach = radius + bot->GetCombatReach();
    return health < returnPct && bot->GetExactDist2d(creature) >= reach;
}

// Engaged: ranged and healers never stand in it; melee hit it until hurt, then wait outside for heals.
bool SentinelPulsingFor(Creature* sentinel, Player* bot)
{
    if (!sentinel->IsInCombat() || SentinelExploding(sentinel, bot))
        return false;

    // While a ranged damage dealer can kill it, the melee leave it alone (DungeonTactics melee hold, waves 7-9: a
    // melee leaving at 50 % still died to the 1 s pulse plus the Sentinel's melee).
    if (PlayerbotAI::IsMelee(bot) && DungeonTactics::RangedDpsAlive(bot))
        return true;

    return MeleeKeepsOut(sentinel, bot, SENTINEL_DISCHARGE_RADIUS, SENTINEL_MELEE_LEAVE_PCT,
                         SENTINEL_MELEE_RETURN_PCT);
}

// The Poison Shock aura pulses in and out of combat; out of combat nobody walks into it (that would pull the
// sludge), in combat the melee may stand in it while healthy, as with the Sentinel.
bool SludgePoisonShock(Creature* sludge, Player* bot)
{
    if (!sludge->HasAura(SPELL_SLUDGE_POISON_SHOCK_AURA))
        return false;

    if (!sludge->IsInCombat() || !bot->IsInCombat())
        return true;

    return MeleeKeepsOut(sludge, bot, SLUDGE_POISON_SHOCK_RADIUS, SLUDGE_MELEE_LEAVE_PCT, SLUDGE_MELEE_RETURN_PCT);
}

// Only until it has fired: afterwards the spike is inert for the rest of its 7 s.
bool CrystalSpikeUnfired(Creature* spike, Player* /*bot*/)
{
    TempSummon* summon = spike->ToTempSummon();
    return !summon || summon->GetTimer() > CRYSTAL_SPIKE_FIRED_TIMER_MS;
}

// In combat everybody keeps out of the blast (whoever steps on the trap blows up everyone in 10 yd); out of combat
// only the trigger circle matters, so following the group past a bomb stays possible.
bool ProximityBombInCombat(GameObject* bomb, Player* bot)
{
    return bomb->isSpawned() && bot->IsInCombat();
}

bool ProximityBombOutOfCombat(GameObject* bomb, Player* bot)
{
    return bomb->isSpawned() && !bot->IsInCombat();
}

bool RookeryEggArmed(GameObject* egg, Player* bot)
{
    return egg->isSpawned() && egg->getLootState() == GO_READY &&
           std::fabs(egg->GetPositionZ() - bot->GetPositionZ()) < ROOKERY_EGG_MAX_DZ;
}

// Dormant: everybody but the tank who pulls it keeps out of the pulse.
bool SentinelDormant(Creature* sentinel, Player* bot)
{
    return !sentinel->IsInCombat() && !SentinelExploding(sentinel, bot);
}

bool ManaLeechDying(Creature* leech, Player* /*bot*/)
{
    return leech->IsInCombat() && leech->GetHealthPct() <= MANA_LEECH_LOW_PCT;
}

bool BlightBombCast(Creature* horror, Player* /*bot*/)
{
    return horror->FindCurrentSpellBySpellId(SPELL_BLIGHT_BOMB) != nullptr;
}

constexpr float SHORT_CIRCUIT_PCT = 25.0f;
constexpr float SLAG_DYING_PCT = 15.0f;

bool ShortCircuitPending(Creature* construct, Player* /*bot*/)
{
    return construct->IsInCombat() && (construct->GetHealthPct() <= SHORT_CIRCUIT_PCT ||
                                       construct->FindCurrentSpellBySpellId(SPELL_SHORT_CIRCUIT) ||
                                       construct->FindCurrentSpellBySpellId(SPELL_SHORT_CIRCUIT_HEROIC));
}

// Ranged finish it from range; melee step back for the last part while a ranged damage dealer can.
bool SlagDying(Creature* slag, Player* bot)
{
    if (!slag->IsInCombat() || slag->GetHealthPct() > SLAG_DYING_PCT)
        return false;

    return !PlayerbotAI::IsMelee(bot) || DungeonTactics::RangedDpsAlive(bot);
}

bool IonarDispersed(Creature* ionar, Player* /*bot*/) { return ionar->IsInCombat() && !ionar->IsVisible(); }

// [b30_st_spirit] The spirit on the bot's floor (the temple's balcony rings stand 10-25 yd above the pit floors).
constexpr float DEATHWALKER_SPIRIT_RADIUS = 8.0f;  // its melee reach (5 yd + both bodies) and two seconds of its walk
constexpr float DEATHWALKER_SPIRIT_MAX_DZ = 6.0f;

bool DeathwalkerSpiritNear(Creature* spirit, Player* bot)
{
    return std::fabs(spirit->GetPositionZ() - bot->GetPositionZ()) <= DEATHWALKER_SPIRIT_MAX_DZ;
}

// Only the spheres that stand still: a wandering one cannot be dodged by a step and is not about to blow up yet.
bool UnstableSphereStanding(Creature* sphere, Player* /*bot*/)
{
    TempSummon* summon = sphere->ToTempSummon();
    return summon && summon->GetTimer() <= UNSTABLE_SPHERE_STANDING_TIMER_MS;
}

// Charged Skin: ranged and healers never stand in it, melee only while healthy.
bool ChargedSkinPulsing(Creature* conjurer, Player* bot)
{
    if (!conjurer->IsInCombat() ||
        (!conjurer->HasAura(SPELL_CHARGED_SKIN) && !conjurer->HasAura(SPELL_CHARGED_SKIN_HEROIC)))
        return false;

    return MeleeKeepsOut(conjurer, bot, CHARGED_SKIN_RADIUS, CHARGED_SKIN_MELEE_LEAVE_PCT,
                         CHARGED_SKIN_MELEE_RETURN_PCT);
}

// In combat only: out of combat the planner pulls it, and a follower must be able to walk past.
bool BogOverlordCloud(Creature* overlord, Player* bot)
{
    constexpr uint32 logMs = 10 * IN_MILLISECONDS;

    bool const cloud = overlord->IsInCombat() && (overlord->HasAura(SPELL_BOG_OVERLORD_DISEASE_CLOUD) ||
                                                  overlord->HasAura(SPELL_BOG_OVERLORD_DISEASE_CLOUD_HEROIC));
    if (!cloud || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsMelee(bot) ||
        bot->GetExactDist2d(overlord) > BOG_OVERLORD_CLOUD_RADIUS + bot->GetCombatReach())
        return cloud;

    // Map threads run in parallel: one log clock per thread, keyed by the bot.
    thread_local std::unordered_map<uint64, uint32> lastLog;
    uint32& at = lastLog[bot->GetGUID().GetRawValue()];
    uint32 const now = getMSTime();
    if (!at || getMSTimeDiff(at, now) >= logMs)
    {
        at = now;
        LOG_INFO("playerbots", "[sv disease cloud] {} stands {:.1f} yd from Bog Overlord's Disease Cloud: steps out",
                 bot->GetName(), bot->GetExactDist2d(overlord));
    }
    return cloud;
}

// [boss_puddles] Broggok's cloud grows with its Poison aura (spell_broggok_poison_cloud): 12 yd base times
// (tick / total * 0.9 + 0.1) * 2/3, so 0.8 yd at the start and 8 yd at the end of its minute.
constexpr float BROGGOK_CLOUD_BASE_RADIUS = 12.0f;
constexpr float BROGGOK_CLOUD_MAX_RADIUS = BROGGOK_CLOUD_BASE_RADIUS * 2.0f / 3.0f;

float BroggokCloudRadius(Creature* cloud)
{
    Aura* poison = cloud->GetAura(SPELL_BROGGOK_POISON);
    if (!poison)
        poison = cloud->GetAura(SPELL_BROGGOK_POISON_HEROIC);

    AuraEffect* pulse = poison ? poison->GetEffect(EFFECT_0) : nullptr;
    if (!pulse || pulse->GetTotalTicks() <= 0)
        return BROGGOK_CLOUD_MAX_RADIUS;

    // One tick ahead: the next pulse is the one the bot has to be out of.
    float const progress =
        std::min(1.0f, float(pulse->GetTickNumber() + 1) / float(pulse->GetTotalTicks()));
    return BROGGOK_CLOUD_BASE_RADIUS * (progress * 0.9f + 0.1f) * 2.0f / 3.0f;
}

// [lead1483] the flare's ms since its summon (its despawn timer counts down from its 7 s life)
uint32 FocusFireAgeMs(Creature* flare)
{
    TempSummon* summon = flare->ToTempSummon();
    uint32 const timer = summon ? summon->GetTimer() : 0;
    return timer < FOCUS_FIRE_LIFE_MS ? FOCUS_FIRE_LIFE_MS - timer : 0;
}

// a hazard until its last blast has landed
bool FocusFireArmed(Creature* flare, Player* /*bot*/) { return FocusFireAgeMs(flare) <= FOCUS_FIRE_LAST_BLAST_MS; }

// the blasts still to come: until the last one, every 500 ms (the first lands 3 s after the summon: the walk out
// starts now, the hits are counted to the last)
void FocusFireBlasts(Creature* flare, int32& leftMs, uint32& periodMs)
{
    leftMs = int32(FOCUS_FIRE_LAST_BLAST_MS) - int32(FocusFireAgeMs(flare));
    periodMs = FOCUS_FIRE_BLAST_EVERY_MS;
}

// [b33_pos] +2 Icy Blast, [b33_ok] +1 Poisonous Mushroom, [lead1483] +1 Focus Fire
constexpr std::array<HazardSpec, 66> HAZARDS = {{
    {MAP_AUCHENAI_CRYPTS, HazardSource::Creature, NPC_FOCUS_FIRE, FIERY_BLAST_RADIUS, HazardRoles::All, &FocusFireArmed,
     true, false, false, nullptr, nullptr, false, SPELL_FIERY_BLAST, &FocusFireBlasts},
    {MAP_MECHANAR, HazardSource::Creature, NPC_NETHER_CHARGE, 15.0f, HazardRoles::All, &NetherChargeArmed},
    // The exploding Sentinel turns into a Destroyed Sentinel on the spot: leave by the larger radius at once.
    {MAP_ARCATRAZ, HazardSource::Creature, NPC_ARCATRAZ_SENTINEL, SENTINEL_DISCHARGE_RADIUS, HazardRoles::All,
     &SentinelExploding},
    {MAP_ARCATRAZ, HazardSource::Creature, NPC_ARCATRAZ_SENTINEL, SENTINEL_DISCHARGE_RADIUS, HazardRoles::NonTank,
     &SentinelPulsingFor, true, true},
    {MAP_ARCATRAZ, HazardSource::Creature, NPC_ARCATRAZ_SENTINEL, SENTINEL_DISCHARGE_RADIUS, HazardRoles::NonTank,
     &SentinelDormant, false, false},
    {MAP_ARCATRAZ, HazardSource::Creature, NPC_DESTROYED_SENTINEL, SENTINEL_DISCHARGE_RADIUS, HazardRoles::All,
     nullptr},
    {MAP_MANA_TOMBS, HazardSource::Creature, NPC_MANA_LEECH, 10.0f, HazardRoles::NonTank, &ManaLeechDying},
    {MAP_GNOMEREGAN, HazardSource::Creature, NPC_DARK_IRON_LAND_MINE, 10.0f, HazardRoles::All, nullptr},
    {MAP_GNOMEREGAN, HazardSource::DynObject, SPELL_RADIATION_CLOUD, 8.0f, HazardRoles::All, nullptr},
    {MAP_MARAUDON, HazardSource::DynObject, SPELL_NOXIOUS_CLOUD, 5.0f, HazardRoles::All, nullptr},
    {MAP_MARAUDON, HazardSource::Creature, NPC_CREEPING_SLUDGE, SLUDGE_POISON_SHOCK_RADIUS, HazardRoles::NonTank,
     &SludgePoisonShock, true, false, true},
    {MAP_MARAUDON, HazardSource::Creature, NPC_CREEPING_SLUDGE_HEROIC, SLUDGE_POISON_SHOCK_RADIUS,
     HazardRoles::NonTank, &SludgePoisonShock, true, false, true},
    {MAP_BLOOD_FURNACE, HazardSource::GameObj, GO_PROXIMITY_BOMB, PROXIMITY_BOMB_BLAST_RADIUS, HazardRoles::All,
     nullptr, true, false, true, &ProximityBombInCombat},
    {MAP_BLOOD_FURNACE, HazardSource::GameObj, GO_PROXIMITY_BOMB_HEROIC, PROXIMITY_BOMB_BLAST_RADIUS,
     HazardRoles::All, nullptr, true, false, true, &ProximityBombInCombat},
    {MAP_BLOOD_FURNACE, HazardSource::GameObj, GO_PROXIMITY_BOMB, PROXIMITY_BOMB_TRIGGER_RADIUS, HazardRoles::All,
     nullptr, false, false, false, &ProximityBombOutOfCombat},
    {MAP_BLOOD_FURNACE, HazardSource::GameObj, GO_PROXIMITY_BOMB_HEROIC, PROXIMITY_BOMB_TRIGGER_RADIUS,
     HazardRoles::All, nullptr, false, false, false, &ProximityBombOutOfCombat},
    {MAP_NEXUS, HazardSource::Creature, NPC_CRYSTAL_SPIKE, CRYSTAL_SPIKE_RADIUS, HazardRoles::All,
     &CrystalSpikeUnfired, true, false, true},
    {MAP_UTGARDE_PINNACLE, HazardSource::DynObject, SPELL_SKADI_FREEZING_CLOUD, 3.0f, HazardRoles::All, nullptr, false},
    {MAP_UTGARDE_PINNACLE, HazardSource::DynObject, SPELL_SKADI_FREEZING_CLOUD_HEROIC, 3.0f, HazardRoles::All, nullptr,
     false},
    {MAP_AHNKAHET, HazardSource::Creature, NPC_FLAME_SPHERE_1, SEAR_BEAM_RADIUS, HazardRoles::All, nullptr},
    {MAP_AHNKAHET, HazardSource::Creature, NPC_FLAME_SPHERE_2, SEAR_BEAM_RADIUS, HazardRoles::All, nullptr},
    {MAP_AHNKAHET, HazardSource::Creature, NPC_FLAME_SPHERE_3, SEAR_BEAM_RADIUS, HazardRoles::All, nullptr},
    // [b33_ok] Amanitar's Poisonous Mushroom: 3 yd sight trigger plus its own size
    {MAP_AHNKAHET, HazardSource::Creature, NPC_AMANITAR_POISONOUS_MUSHROOM, 3.5f, HazardRoles::All, nullptr},
    {MAP_PIT_OF_SARON, HazardSource::Creature, NPC_PLAGUEBORN_HORROR, 20.0f, HazardRoles::All, &BlightBombCast},
    // Out of combat the hold is off, so following the group through the Rookery stays possible.
    {MAP_BLACKROCK_SPIRE, HazardSource::GameObj, GO_ROOKERY_EGG, ROOKERY_EGG_RADIUS, HazardRoles::All, nullptr, true,
     false, true, &RookeryEggArmed},
    {MAP_UTGARDE_KEEP, HazardSource::Creature, NPC_INGVAR_THROW_DUMMY, 5.0f, HazardRoles::All, nullptr},
    {MAP_GUNDRAK, HazardSource::DynObject, SPELL_MOJO_PUDDLE, 3.0f, HazardRoles::All, nullptr},
    {MAP_GUNDRAK, HazardSource::DynObject, SPELL_MOJO_PUDDLE_HEROIC, 3.0f, HazardRoles::All, nullptr},
    {MAP_GUNDRAK, HazardSource::DynObject, SPELL_MOJO_PUDDLE_ELEMENTAL_HEROIC, 3.0f, HazardRoles::All, nullptr},
    {MAP_HALLS_OF_STONE, HazardSource::Creature, NPC_UNRELENTING_CONSTRUCT, 10.0f, HazardRoles::All,
     &ShortCircuitPending},
    {MAP_HALLS_OF_LIGHTNING, HazardSource::Creature, NPC_SLAG, 10.0f, HazardRoles::NonTank, &SlagDying},
    // The Spark circle moves with it: the hold keeps a bot from walking into one, the trigger steps it away again.
    {MAP_HALLS_OF_LIGHTNING, HazardSource::Creature, NPC_SPARK_OF_IONAR, 6.0f, HazardRoles::All, nullptr},
    {MAP_HALLS_OF_LIGHTNING, HazardSource::Creature, NPC_IONAR, 10.0f, HazardRoles::All, &IonarDispersed},
    {MAP_OCULUS, HazardSource::Creature, NPC_UNSTABLE_SPHERE, SPHERE_DETONATION_RADIUS, HazardRoles::All,
     &UnstableSphereStanding, true, false, true},
    {MAP_OCULUS, HazardSource::DynObject, SPELL_RING_LORD_BLIZZARD, 8.0f, HazardRoles::All, nullptr},
    {MAP_OCULUS, HazardSource::DynObject, SPELL_RING_LORD_BLIZZARD_HEROIC, 8.0f, HazardRoles::All, nullptr},
    {MAP_OCULUS, HazardSource::DynObject, SPELL_RING_LORD_FLAMESTRIKE, 5.0f, HazardRoles::All, nullptr},
    {MAP_OCULUS, HazardSource::DynObject, SPELL_RING_LORD_FLAMESTRIKE_HEROIC, 5.0f, HazardRoles::All, nullptr},
    {MAP_OCULUS, HazardSource::Creature, NPC_RING_LORD_CONJURER, CHARGED_SKIN_RADIUS, HazardRoles::NonTank,
     &ChargedSkinPulsing},
    {MAP_STEAMVAULT, HazardSource::DynObject, SPELL_COILFANG_BLIZZARD, 8.0f, HazardRoles::All, nullptr},
    {MAP_STEAMVAULT, HazardSource::DynObject, SPELL_COILFANG_BLIZZARD_HEROIC, 8.0f, HazardRoles::All, nullptr},
    {MAP_STEAMVAULT, HazardSource::Creature, NPC_BOG_OVERLORD, BOG_OVERLORD_CLOUD_RADIUS, HazardRoles::NonMelee,
     &BogOverlordCloud, true, false, true},
    // [boss_puddles] boss ground effects (BOSS_PUDDLES.md); the rooms to kite in are in BossKiteRooms.h
    {MAP_BLOOD_FURNACE, HazardSource::Creature, NPC_BROGGOK_POISON_CLOUD, BROGGOK_CLOUD_MAX_RADIUS, HazardRoles::All,
     nullptr, true, false, false, nullptr, &BroggokCloudRadius},
    {MAP_STEAMVAULT, HazardSource::DynObject, SPELL_THESPIA_LIGHTNING_CLOUD, 10.0f, HazardRoles::All, nullptr},
    {MAP_AHNKAHET, HazardSource::DynObject, SPELL_JEDOGA_THUNDERSHOCK, 5.0f, HazardRoles::All, nullptr},
    {MAP_AHNKAHET, HazardSource::DynObject, SPELL_JEDOGA_THUNDERSHOCK_HEROIC, 5.0f, HazardRoles::All, nullptr},
    {MAP_ARCATRAZ, HazardSource::DynObject, SPELL_SOCCOTHRATES_FELFIRE, 5.0f, HazardRoles::All, nullptr},
    {MAP_ARCATRAZ, HazardSource::Creature, NPC_UNBOUND_VOID_ZONE, 3.0f, HazardRoles::All, nullptr},
    {MAP_PIT_OF_SARON, HazardSource::DynObject, SPELL_ICK_TOXIC_WASTE, 4.0f, HazardRoles::All, nullptr},
    {MAP_PIT_OF_SARON, HazardSource::DynObject, SPELL_ICK_TOXIC_WASTE_HEROIC, 4.0f, HazardRoles::All, nullptr},
    {MAP_PIT_OF_SARON, HazardSource::DynObject, SPELL_RIMEFANG_ICY_BLAST, 7.0f, HazardRoles::All, nullptr},  // [b33_pos]
    {MAP_PIT_OF_SARON, HazardSource::DynObject, SPELL_RIMEFANG_ICY_BLAST_HEROIC, 7.0f, HazardRoles::All, nullptr},
    {MAP_UTGARDE_PINNACLE, HazardSource::Creature, NPC_YMIRON_SPIRIT_FOUNT, 7.0f, HazardRoles::All, nullptr},
    {MAP_HALLS_OF_STONE, HazardSource::DynObject, SPELL_STORM_OF_GRIEF, 9.0f, HazardRoles::All, nullptr},
    {MAP_HALLS_OF_STONE, HazardSource::DynObject, SPELL_STORM_OF_GRIEF_HEROIC, 9.0f, HazardRoles::All, nullptr},
    {MAP_BLACKROCK_SPIRE, HazardSource::DynObject, SPELL_DRAKKISATH_FLAMESTRIKE, 5.0f, HazardRoles::All, nullptr},
    {MAP_BLACKROCK_SPIRE, HazardSource::DynObject, SPELL_INVOKER_FLAMESTRIKE, 5.0f, HazardRoles::All, nullptr},
    {MAP_AZJOL_NERUB, HazardSource::DynObject, SPELL_HADRONOX_ACID_CLOUD, 5.0f, HazardRoles::All, nullptr},
    {MAP_AZJOL_NERUB, HazardSource::DynObject, SPELL_HADRONOX_ACID_CLOUD_HEROIC, 5.0f, HazardRoles::All, nullptr},
    {MAP_DRAKTHARON_KEEP, HazardSource::DynObject, SPELL_THARONJA_POISON_CLOUD, 8.0f, HazardRoles::All, nullptr},
    {MAP_DRAKTHARON_KEEP, HazardSource::DynObject, SPELL_THARONJA_POISON_CLOUD_HEROIC, 8.0f, HazardRoles::All,
     nullptr},
    {MAP_FORGE_OF_SOULS, HazardSource::Creature, NPC_WELL_OF_SOULS, 4.0f, HazardRoles::All, nullptr},
    {MAP_SHATTERED_HALLS, HazardSource::Creature, NPC_LESSER_SHADOW_FISSURE, 3.0f, HazardRoles::All, nullptr},
    {MAP_OLD_HILLSBRAD, HazardSource::DynObject, SPELL_SKARLOC_CONSECRATION, 10.0f, HazardRoles::All, nullptr},
    // [b30_st_spirit] walked away from, not to the tank: it is slower than a walk and nobody can take it over
    {MAP_SUNKEN_TEMPLE, HazardSource::Creature, NPC_DEATHWALKER_SPIRIT, DEATHWALKER_SPIRIT_RADIUS, HazardRoles::All,
     &DeathwalkerSpiritNear, true, false, true, nullptr, nullptr, true},  // [b44_st] kiteVictim
    {MAP_SUNKEN_TEMPLE, HazardSource::Creature, NPC_DEATHWALKER_SPIRIT_HEROIC, DEATHWALKER_SPIRIT_RADIUS,
     HazardRoles::All, &DeathwalkerSpiritNear, true, false, true, nullptr, nullptr, true},  // [b44_st] kiteVictim
}};

// A mob that can follow its victim right now (not stunned, rooted, casting or dormant).
bool CanChase(Creature* creature)
{
    return creature->IsInCombat() && !creature->IsRooted() &&
           !creature->HasUnitState(UNIT_STATE_NOT_MOVE | UNIT_STATE_CASTING);
}

bool AppliesTo(Player* bot, HazardRoles roles)
{
    switch (roles)
    {
        case HazardRoles::NonTank:
            return !PlayerbotAI::IsTank(bot);
        case HazardRoles::NonMelee:
            return !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsMelee(bot);
        default:
            return true;
    }
}

// Spots in rising distance, 16 directions each: the first free one outside every circle wins. Indoors (Arcatraz
// walkways, wave 8: ranged stood 15 s at 2-4 yd from a Sentinel) most straight 20 yd lines end at a wall short of
// the ring, so the directions pointing away from the hazard are tried first, more of them are path-checked, and the
// spots where other members stand outside every circle are the fallback.
constexpr std::array<float, 8> EXIT_STEPS = {3.0f, 6.0f, 9.0f, 12.0f, 16.0f, 20.0f, 25.0f, 30.0f};
constexpr uint32 EXIT_DIRECTIONS = 16;
constexpr uint32 EXIT_MAX_PATH_CHECKS = 16;
constexpr uint32 EXIT_MAX_MEMBER_PATHS = 4;
constexpr float ANCHOR_WEIGHT = 0.5f;
constexpr float AWAY_WEIGHT = 3.0f;  // yards of score for a direction straight back towards the hazard
constexpr float MEMBER_SPOT_MAX_DISTANCE = 40.0f;
constexpr float MEMBER_PATH_MAX_LENGTH = 50.0f;
// An exit inside the aggro range of an idle pack pulls it into the fight (Mechanar wave 10: the Nether Charge
// escapes walked the Capacitus fight into the next room's trash): such spots are tried last.
constexpr float IDLE_MOB_SCAN_RANGE = 45.0f;
constexpr float IDLE_MOB_MARGIN = 3.0f;
constexpr float IDLE_MOB_MAX_DZ = 8.0f;
constexpr float IDLE_MOB_PENALTY = 50.0f;

struct IdleMob
{
    float x;
    float y;
    float reach;
};

std::vector<IdleMob> CollectIdleMobs(Player* bot)
{
    std::vector<IdleMob> mobs;
    std::list<Creature*> creatures;
    Acore::AnyUnitInObjectRangeCheck check(bot, IDLE_MOB_SCAN_RANGE);
    Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, creatures, check);
    Cell::VisitObjects(bot, searcher, IDLE_MOB_SCAN_RANGE);
    for (Creature* creature : creatures)
    {
        if (!creature->IsAlive() || creature->IsInCombat() || creature->IsCivilian() ||
            creature->IsTrigger() || creature->IsTotem() || !creature->HasReactState(REACT_AGGRESSIVE) ||
            creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) || !creature->IsHostileTo(bot) ||
            std::fabs(creature->GetPositionZ() - bot->GetPositionZ()) > IDLE_MOB_MAX_DZ)
            continue;

        mobs.push_back({creature->GetPositionX(), creature->GetPositionY(),
                        creature->GetAggroRange(bot) + IDLE_MOB_MARGIN});
    }
    return mobs;
}

bool PullsIdleMob(float x, float y, std::vector<IdleMob> const& mobs)
{
    return std::any_of(mobs.begin(), mobs.end(),
                       [x, y](IdleMob const& mob) { return std::hypot(x - mob.x, y - mob.y) < mob.reach; });
}

// [b32_pull] Taldaram's Flame Spheres walk 25 yd out from him (sphere 1 towards the victim's spot taken at the cast,
// 2 and 3 at +-90 degrees; boss_prince_taldaram.cpp npc_taldaram_flamesphere) while Sear Beam pulses 18 yd round
// them. A circle at the sphere's current spot sent the escapes into its path: ok-h 20261025-221216-ok-h-5G-42, the
// ret paladin and the warrior left the boss towards the tank's side, the spheres walked after them, 20k Sear Beam in
// 15 s and both died 19 s into the pull. The whole walk is the hazard: circles every few yards from the sphere to
// where it stops (its spline end while it walks; before it starts, 3 s after the summon, the predicted end).
constexpr uint32 SPELL_FLAME_SPHERE_PERIODIC = 55926;
constexpr float FLAME_SPHERE_WALK = 25.0f;
constexpr float FLAME_SPHERE_SAMPLE_STEP = 5.0f;

bool FlameSphereEnd(Creature* sphere, float& endX, float& endY)
{
    uint32 const entry = sphere->GetEntry();
    if (entry != NPC_FLAME_SPHERE_1 && entry != NPC_FLAME_SPHERE_2 && entry != NPC_FLAME_SPHERE_3)
        return false;

    if (sphere->HasAura(SPELL_FLAME_SPHERE_PERIODIC))
    {
        if (!sphere->movespline->Initialized() || sphere->movespline->Finalized())
            return false;  // arrived: it despawns a second later

        auto const dest = sphere->movespline->FinalDestination();
        endX = dest.x;
        endY = dest.y;
        return true;
    }

    TempSummon* summon = sphere->ToTempSummon();
    Unit* prince = summon ? summon->GetSummonerUnit() : nullptr;
    Unit* victim = prince ? prince->GetVictim() : nullptr;
    if (!victim)
        return false;

    float angle = sphere->GetAngle(victim);
    if (entry == NPC_FLAME_SPHERE_2)
        angle += static_cast<float>(M_PI) / 2.0f;
    else if (entry == NPC_FLAME_SPHERE_3)
        angle -= static_cast<float>(M_PI) / 2.0f;
    endX = sphere->GetPositionX() + FLAME_SPHERE_WALK * std::cos(angle);
    endY = sphere->GetPositionY() + FLAME_SPHERE_WALK * std::sin(angle);
    return true;
}

void AppendFlameSpherePath(Creature* sphere, float radius, bool hold, std::vector<Circle>& circles)
{
    float endX = 0.0f;
    float endY = 0.0f;
    if (!FlameSphereEnd(sphere, endX, endY))
        return;

    float const x0 = sphere->GetPositionX();
    float const y0 = sphere->GetPositionY();
    float const length = std::hypot(endX - x0, endY - y0);
    uint32 const samples = static_cast<uint32>(std::ceil(length / FLAME_SPHERE_SAMPLE_STEP));
    for (uint32 i = 1; i <= samples; ++i)
    {
        float const t = static_cast<float>(i) / samples;
        circles.push_back({x0 + (endX - x0) * t, y0 + (endY - y0) * t, radius, hold, false});
    }
}

// Chasing circles move with the bot and are never part of an exit test.
bool OutsideAll(float x, float y, std::vector<Circle> const& circles, float margin)
{
    for (Circle const& circle : circles)
        if (!circle.chasing && std::hypot(x - circle.x, y - circle.y) < circle.radius + margin)
            return false;

    return true;
}

// The escapable circle (grown by `margin`) holding (x, y) whose centre is nearest, or nullptr.
Circle const* NearestContaining(float x, float y, std::vector<Circle> const& circles, float margin)
{
    Circle const* nearest = nullptr;
    float nearestDist = 0.0f;
    for (Circle const& circle : circles)
    {
        float const dist = std::hypot(x - circle.x, y - circle.y);
        if (circle.chasing || dist >= circle.radius + margin)
            continue;

        if (!nearest || dist < nearestDist)
        {
            nearest = &circle;
            nearestDist = dist;
        }
    }
    return nearest;
}
}  // namespace

std::vector<Circle> Collect(Player* bot)
{
    std::vector<Circle> circles;
    uint32 const mapId = bot->GetMapId();
    if (std::none_of(HAZARDS.begin(), HAZARDS.end(), [mapId](HazardSpec const& spec) { return spec.mapId == mapId; }))
        return circles;

    // The Oculus' drakes fly above the ring platforms: a ground circle there is not where the bot is, and a drake is
    // steered by the drake actions, not by a hazard step.
    if (mapId == MAP_OCULUS && bot->GetVehicle())
        return circles;

    std::vector<WorldObject*> objects;
    // [b32_pull] a Flame Sphere's walk ends 25 yd out and burns 18 yd round that end: seen from 45 yd
    float const scanRange = mapId == MAP_AHNKAHET ? HAZARD_SCAN_RANGE + 15.0f : HAZARD_SCAN_RANGE;
    Acore::AllWorldObjectsInRange check(bot, scanRange);
    Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(
        bot, objects, check,
        GRID_MAP_TYPE_MASK_CREATURE | GRID_MAP_TYPE_MASK_DYNAMICOBJECT | GRID_MAP_TYPE_MASK_GAMEOBJECT);
    Cell::VisitObjects(bot, searcher, scanRange);

    float const size = bot->GetCombatReach();
    bool const inCombat = bot->IsInCombat();
    for (WorldObject* object : objects)
    {
        Creature* creature = object->ToCreature();
        DynamicObject* dynObject = object->ToDynObject();
        GameObject* gameObject = object->ToGameObject();
        if ((creature && !creature->IsAlive()) || (!creature && !dynObject && !gameObject))
            continue;

        for (HazardSpec const& spec : HAZARDS)
        {
            if (spec.mapId != mapId || !AppliesTo(bot, spec.roles))
                continue;

            bool const hold = spec.hold && (!spec.holdInCombatOnly || inCombat);
            if (creature && spec.source == HazardSource::Creature && creature->GetEntry() == spec.id &&
                (!spec.condition || spec.condition(creature, bot)))
            {
                bool const chasing = spec.chaseToTank && creature->GetVictim() == bot && CanChase(creature);
                // [b44_st] the victim of a chaser nobody holds keeps farther and walks round it
                bool const kite = spec.kiteVictim && creature->GetVictim() == bot;
                float const radius = (spec.radiusOf ? spec.radiusOf(creature) : spec.radius) +
                                     (kite ? KITE_VICTIM_EXTRA : 0.0f);
                circles.push_back({creature->GetPositionX(), creature->GetPositionY(), radius + size, hold,
                                   chasing, kite});
                // [lead1483] the spell that hits inside (the caster's difficulty rank) and when, for the verdict
                if (spec.spell)
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spec.spell))
                    {
                        SpellInfo const* rank = sSpellMgr->GetSpellForDifficultyFromSpell(info, creature);
                        circles.back().spellId = (rank ? rank : info)->Id;
                        circles.back().caster = creature->GetGUID();
                        if (spec.blastsOf)
                            spec.blastsOf(creature, circles.back().leftMs, circles.back().periodMs);
                    }
                AppendFlameSpherePath(creature, radius + size, hold, circles);  // [b32_pull]
                break;
            }

            if (dynObject && spec.source == HazardSource::DynObject && dynObject->GetSpellId() == spec.id)
            {
                float const radius = dynObject->GetRadius() > 0.0f ? dynObject->GetRadius() : spec.radius;
                circles.push_back({dynObject->GetPositionX(), dynObject->GetPositionY(), radius + size, hold, false});
                break;
            }

            if (gameObject && spec.source == HazardSource::GameObj && gameObject->GetEntry() == spec.id &&
                (!spec.objectCondition || spec.objectCondition(gameObject, bot)))
            {
                circles.push_back({gameObject->GetPositionX(), gameObject->GetPositionY(), spec.radius + size, hold,
                                   false});
                break;
            }
        }
    }
    return circles;
}

bool IsInside(Player* bot, std::vector<Circle> const& circles, float extra)
{
    return !OutsideAll(bot->GetPositionX(), bot->GetPositionY(), circles, extra);
}

Player* ChasedTowardsTank(Player* bot, std::vector<Circle> const& circles)
{
    if (PlayerbotAI::IsTank(bot) ||
        std::none_of(circles.begin(), circles.end(), [](Circle const& circle) { return circle.chasing; }))
        return nullptr;

    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    Player* tank = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId() ||
            !PlayerbotAI::IsTank(member) || bot->GetExactDist(member) > CHASE_TANK_MAX_DISTANCE)
            continue;

        if (!tank || bot->GetExactDist(member) < bot->GetExactDist(tank))
            tank = member;
    }
    return tank && bot->GetExactDist2d(tank) > CHASE_TANK_DISTANCE ? tank : nullptr;
}
}  // namespace DungeonHazard

// [boss_puddles] The movement side's room lookup (MovementAction::EscapeRoom) reads the per-boss kite rooms. Set once
// at static initialisation from this translation unit, which is always linked (BossKiteRooms is header-only, so no
// CMake re-configure is needed for it); KiteRoomLookup() is a function-local static, safe to reach from here.
namespace
{
[[maybe_unused]] bool const bossKiteRoomsRegistered = (MovementAction::KiteRoomLookup() = &BossKiteRooms::Find, true);
}

std::vector<DungeonHazard::Circle> DungeonHazardCirclesValue::Calculate() { return DungeonHazard::Collect(bot); }

bool DungeonHazardTrigger::IsActive()
{
    std::vector<DungeonHazard::Circle> const& circles =
        context->GetValue<std::vector<DungeonHazard::Circle>>("dungeon hazard circles")->RefGet();
    if (circles.empty())
        return false;

    return DungeonHazard::IsInside(bot, circles, DungeonHazard::HAZARD_SAFETY) ||
           DungeonHazard::ChasedTowardsTank(bot, circles);
}

bool AvoidDungeonHazardAction::Execute(Event /*event*/)
{
    std::vector<DungeonHazard::Circle> const circles =
        context->GetValue<std::vector<DungeonHazard::Circle>>("dungeon hazard circles")->Get();
    if (circles.empty())
        return false;

    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const botZ = bot->GetPositionZ();
    float const margin = TbcHazard::EXIT_MARGIN;

    DungeonHazard::Circle const* hazard =
        DungeonHazard::NearestContaining(botX, botY, circles, DungeonHazard::HAZARD_SAFETY);
    if (!hazard)
    {
        // Out of every circle but chased by a hazard mob: the tank is the only one who can take it away.
        Player* tank = DungeonHazard::ChasedTowardsTank(bot, circles);
        return tank && MoveNear(tank, 2.0f, MovementPriority::MOVEMENT_FORCED);
    }

    // A melee bot prefers a spot from which it can come back to its target first.
    Unit* target = AI_VALUE(Unit*, "current target");
    Unit* const anchor = PlayerbotAI::IsMelee(bot) ? target : nullptr;
    // Straight away from the centre of the circle the bot is in is the shortest way out.
    float const awayAngle = std::hypot(botX - hazard->x, botY - hazard->y) > 0.5f
                                ? std::atan2(botY - hazard->y, botX - hazard->x)
                                : bot->GetOrientation() + static_cast<float>(M_PI);

    std::vector<DungeonHazard::IdleMob> const idleMobs = DungeonHazard::CollectIdleMobs(bot);
    std::vector<std::pair<float, std::pair<float, float>>> candidates;  // score, (x, y)
    for (float step : DungeonHazard::EXIT_STEPS)
    {
        for (uint32 i = 0; i < DungeonHazard::EXIT_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / DungeonHazard::EXIT_DIRECTIONS;
            float const x = botX + std::cos(angle) * step;
            float const y = botY + std::sin(angle) * step;
            if (!DungeonHazard::OutsideAll(x, y, circles, margin))
                continue;

            float score = step + DungeonHazard::AWAY_WEIGHT * 0.5f * (1.0f - std::cos(angle - awayAngle));
            if (anchor)
                score += DungeonHazard::ANCHOR_WEIGHT * std::max(0.0f, anchor->GetExactDist2d(x, y) - step);
            if (DungeonHazard::PullsIdleMob(x, y, idleMobs))
                score += DungeonHazard::IDLE_MOB_PENALTY;
            candidates.push_back({score, {x, y}});
        }
    }
    std::sort(candidates.begin(), candidates.end());

    bool const stopCast = DungeonHazard::IsInside(bot, circles, 0.0f);
    // [b32_engage] A healer takes the first way out that still sees its tank; only when none does, the first way
    // out. bf-h-5H-84 20261025-223902 803-822 s: the priest stood out of the Proximity Bombs 25-27 yd from the tank
    // with no sight of it for 20 s, the tank died with the healer at 90 % mana.
    Player* healTank = PlayerbotAI::IsHeal(bot) ? KillOrder::FindFightTank(bot) : nullptr;
    if (healTank && (!healTank->IsAlive() || healTank->GetMap() != bot->GetMap()))
        healTank = nullptr;
    bool blindFound = false;
    float blindX = 0.0f;
    float blindY = 0.0f;
    float blindZ = 0.0f;
    uint32 pathChecks = 0;
    for (auto const& candidate : candidates)
    {
        if (++pathChecks > DungeonHazard::EXIT_MAX_PATH_CHECKS)
            break;

        float x = candidate.second.first;
        float y = candidate.second.second;
        float z = botZ;
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, botX, botY, botZ, x, y, z))
            continue;

        // A wall may cut the line short; the shortened spot still counts when it is out.
        if (!DungeonHazard::OutsideAll(x, y, circles, margin - DungeonHazard::HAZARD_SAFETY))
            continue;

        if (healTank && !healTank->IsWithinLOS(x, y, z + bot->GetCollisionHeight()))
        {
            if (!blindFound)
            {
                blindFound = true;
                blindX = x;
                blindY = y;
                blindZ = z;
            }
            continue;
        }

        if (stopCast)
            bot->CastStop();

        return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED, true);
    }

    if (blindFound)
    {
        if (stopCast)
            bot->CastStop();

        return MoveTo(bot->GetMapId(), blindX, blindY, blindZ, false, false, false, false,
                      MovementPriority::MOVEMENT_FORCED, true);
    }

    // No straight line leads out: walk (pathfinding around the walls) to a member who stands outside every circle.
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    uint32 memberPaths = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId() ||
            bot->GetExactDist(member) > DungeonHazard::MEMBER_SPOT_MAX_DISTANCE ||
            !DungeonHazard::OutsideAll(member->GetPositionX(), member->GetPositionY(), circles,
                                       margin - DungeonHazard::HAZARD_SAFETY))
            continue;

        if (++memberPaths > DungeonHazard::EXIT_MAX_MEMBER_PATHS)
            break;

        BotPath const path =  // [lead109]
            FindBotPath(bot, member->GetPositionX(), member->GetPositionY(), member->GetPositionZ());
        if (!path.valid || (path.type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) ||
            path.length > DungeonHazard::MEMBER_PATH_MAX_LENGTH)
            continue;

        if (stopCast)
            bot->CastStop();

        return MoveTo(bot->GetMapId(), member->GetPositionX(), member->GetPositionY(), member->GetPositionZ(), false,
                      false, false, false, MovementPriority::MOVEMENT_FORCED, true);
    }
    return false;
}

float DungeonHazardHoldMultiplier::GetValue(Action* action)
{
    if (!TbcHazard::IsRepositionAction(action))
        return 1.0f;

    std::vector<DungeonHazard::Circle> const& circles =
        context->GetValue<std::vector<DungeonHazard::Circle>>("dungeon hazard circles")->RefGet();
    if (circles.empty())
        return 1.0f;

    // Moving to a target well clear of the hazard leads away from it; anything else would walk back in. A chasing
    // mob's circle moves with the bot, and a hazard that is only passed by must not stop the bot following its group.
    Unit* target = AI_VALUE(Unit*, "current target");
    float const holdBand = DungeonHazard::HAZARD_SAFETY + TbcHazard::HOLD_BUFFER;
    // [b32_engage] A healer's way is to its tank, not to its enemy target: a healer that lost sight of the tank,
    // or whose tank is below HEALER_TANK_DANGER_PCT, may walk to it, the brief step through a hazard band
    // included (bf-h-5H-84 20261025-223902: 20 s pinned out of sight while the tank died).
    constexpr float HEALER_TANK_DANGER_PCT = 50.0f;
    if (PlayerbotAI::IsHeal(bot))
    {
        Player* tank = KillOrder::FindFightTank(bot);
        if (tank && tank->IsAlive() && tank->GetMap() == bot->GetMap())
        {
            if (tank->GetHealthPct() < HEALER_TANK_DANGER_PCT && !bot->IsWithinLOSInMap(tank))
                return 1.0f;

            target = tank;
        }
    }

    for (DungeonHazard::Circle const& circle : circles)
    {
        if (!circle.hold || circle.chasing || bot->GetExactDist2d(circle.x, circle.y) >= circle.radius + holdBand)
            continue;

        if (!target || target->GetExactDist2d(circle.x, circle.y) < circle.radius + holdBand)
            return 0.0f;

        // [b44_st] a chaser nobody holds: no walk to a target on its far side either (the straight way passes it;
        // the healer's walk to its tank through four spirits, st-h 20261026-075135-st-h-5F-7)
        if (circle.kite)
        {
            float const ax = bot->GetPositionX();
            float const ay = bot->GetPositionY();
            float const dx = target->GetPositionX() - ax;
            float const dy = target->GetPositionY() - ay;
            float const lengthSq = dx * dx + dy * dy;
            float share = lengthSq > 0.0f ? ((circle.x - ax) * dx + (circle.y - ay) * dy) / lengthSq : 0.0f;
            share = std::clamp(share, 0.0f, 1.0f);
            if (std::hypot(ax + dx * share - circle.x, ay + dy * share - circle.y) < circle.radius)
                return 0.0f;
        }
    }
    return 1.0f;
}
