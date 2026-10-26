/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HOSTRIGGERS_H
#define PLAYERBOTS_HOSTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Trigger.h"
#include <vector>

enum HallsOfStoneIDs
{
    // Krystallus
    SPELL_GROUND_SLAM               = 50827,  // 2 s on every enemy in 100 yd (pets too) + knockback
    DEBUFF_GROUND_SLAM              = 50833,  // next 5 s: Petrifying Grip 50836 stacks, Stoned 50812 at 5 stacks
    SPELL_STONED                    = 50812,  // stunned until Shatter; removed by the Shatter hit

    // Sjonnir The Ironshaper
    SPELL_LIGHTNING_RING_N          = 50840,
    SPELL_LIGHTNING_RING_H          = 59848,
};

#define SPELL_LIGHTNING_RING        DUNGEON_MODE(bot, SPELL_LIGHTNING_RING_N, SPELL_LIGHTNING_RING_H)

class Creature;

namespace HoSIds
{
// boss_sjonnir.cpp SPELL_LIGHTNING_RING_5S (below 20 % every 11 s), heroic pair from spelldifficulty_dbc
constexpr uint32 SPELL_LIGHTNING_RING_5S_N = 51849;
constexpr uint32 SPELL_LIGHTNING_RING_5S_H = 59861;

// halls_of_stone.h Encounter (boss state ids)
constexpr uint32 BOSS_TRIBUNAL_OF_AGES = 2;
constexpr uint32 BRANN_BRONZEBEARD = 4;  // escort
constexpr uint32 BRANN_DOOR = 5;         // Sjonnir's door

// halls_of_stone.h npcs (also the instance GetGuidData key for Brann)
constexpr uint32 NPC_BRANN = 28070;

// brann_bronzebeard.cpp GossipIDs: the menu Brann currently offers tells which step is next
constexpr uint32 GOSSIP_TRIBUNAL_BEFORE = 9669;  // start the escort
constexpr uint32 GOSSIP_TRIBUNAL_START = 9670;   // start the Tribunal of Ages
constexpr uint32 GOSSIP_TRIBUNAL_END = 10206;    // skip the lore
constexpr uint32 GOSSIP_SJONNIR_DOOR = 10012;    // open Sjonnir's door

// brann_bronzebeard.cpp NPCs: Searing Gaze trigger and Dark Matter target, both 5 yd explosions
constexpr uint32 NPC_SEARING_GAZE_TRIGGER = 28265;
constexpr uint32 NPC_DARK_MATTER_TARGET = 28237;
// brann_bronzebeard.cpp: the Tribunal of Ages adds, in waves
constexpr uint32 NPC_DARK_RUNE_PROTECTOR = 27983;
constexpr uint32 NPC_DARK_RUNE_STORMCALLER = 27984;
constexpr uint32 NPC_IRON_GOLEM_CUSTODIAN = 27985;

// Shatter effect 50811 / 61547: every stoned unit, pets included, hits its allies in 30 yd, falling off linearly
constexpr float SHATTER_RADIUS = 30.0f;

// halls_of_stone.h BOSS_KRYSTALLUS and his only spawn (creature 126790).
constexpr uint32 BOSS_KRYSTALLUS = 0;
constexpr float KRYSTALLUS_X = 1008.56f;
constexpr float KRYSTALLUS_Y = 759.914f;
constexpr float KRYSTALLUS_Z = 208.706f;
// A hunter dismisses the pet when this close to his room while he lives, and does not call it back before he dies.
constexpr float KRYSTALLUS_PET_RANGE = 70.0f;

// Short-lived summons that are dismissed (and not summoned) near a living Krystallus: each one is another unit that
// gets Stoned and Shatters on whoever stands near it, and most of them cannot be parked (no pet commands, or a
// ScriptedAI that keeps chasing its victim). Wave 12: Greater Fire Elemental 5162 + Water Elemental 4206 Shatter
// killed the stoned healer they stood next to. creature_template entries.
constexpr uint32 NPC_GREATER_FIRE_ELEMENTAL = 15438;  // Fire Elemental Totem 2894 (npc_pet_shaman_fire_elemental)
constexpr uint32 NPC_GREATER_EARTH_ELEMENTAL = 15352;
constexpr uint32 NPC_WATER_ELEMENTAL = 510;  // Summon Water Elemental 31687 (45 s, or permanent with the glyph)
constexpr uint32 NPC_WATER_ELEMENTAL_ALT = 37994;
constexpr uint32 NPC_SHADOWFIEND = 19668;
constexpr uint32 NPC_TREANT = 1964;            // Force of Nature 33831
constexpr uint32 NPC_SPIRIT_WOLF = 29264;      // Feral Spirit 51533
constexpr uint32 NPC_ARMY_GHOUL = 24207;       // Army of the Dead 42650
constexpr uint32 NPC_MIRROR_IMAGE = 31216;     // Mirror Image 55342
constexpr uint32 NPC_EBON_GARGOYLE = 27829;    // Summon Gargoyle 49206
constexpr uint32 NPC_RISEN_GHOUL = 26125;      // Raise Dead 46584

// The star (RESEARCH_TACTICS.md section 5): the tank holds Krystallus, every other member keeps its own spoke around
// him, 360 / (members) degrees apart, ranged and healers at this radius the whole fight. Ground Slam knocks everybody
// straight away from him, so members on different spokes land far apart (5 members: 72 degrees, neighbours 23.5 yd
// apart at 20 yd before the knock-back) and the Ground Slam step (shatter spread) has little left to do.
constexpr float STAR_RANGED_RADIUS = 21.0f;
constexpr float STAR_TOLERANCE = 5.0f;  // a member this close to its spot stays put (the tank keeps moving a bit)

// Tribunal of Ages (brann_bronzebeard.cpp InitializeEvent 422-446, ScheduleNextTransition 551-574): three phases of
// 100 s each, then EndTribunalFight; the encounter is survived, not killed. Brann stands at the console
// (ACTION_START_TRIBUNAL MovePoint 897.18, 331.77, 203.71) and every wave taunts him on spawn; his death resets the
// event (JustDied 949-959). The tank holds a fixed spot at his side, the healer stays at the tank, and nobody chases
// adds across the room (hos-h 20261025-062726: the tank ran 15-24 yd from the healer after each add and Dark Rune
// Protectors killed the healer).
// The hold spot: 4 yd from the console along Brann's own walk in from the escort point (939.6, 375.5), so it is on
// his path and the mesh.
constexpr float TRIBUNAL_HOLD_X = 900.0f;
constexpr float TRIBUNAL_HOLD_Y = 334.6f;
constexpr float TRIBUNAL_HOLD_Z = 203.7f;
constexpr float TRIBUNAL_HOLD_TOLERANCE = 4.0f;
// [b34_hos] The tank meets the waves 10 yd forward of the hold spot, on the Protectors' walk in from their spawn
// (943.1, 401.4); the healer (and the group) stays at the hold spot by Brann. hos-h 20261026-014521-hos-h-5D-34: with
// tank and healer stacked at the hold spot every Protector of every wave spawned already on the healer (heal threat
// from the Glare of the Tribunal heals, "Dark Rune Protector is 64 yd from it, on the healer"), walked straight to
// her and the bear's Swipe never took them off (healer died 2331 s, 3 Protectors on her 2 yd from the tank). Meeting
// them forward puts the tank's area threat and taunts on their path before they reach the healer. On the mesh: the
// 000434 healer's own walk crossed (905.3, 341.0, 203.7).
constexpr float TRIBUNAL_TANK_X = 905.4f;
constexpr float TRIBUNAL_TANK_Y = 343.0f;
constexpr float TRIBUNAL_TANK_Z = 203.7f;
// The tank leaves the spot for an add only this far; one farther away is taunted from the spot.
constexpr float TRIBUNAL_CHASE_LIMIT = 10.0f;
// Movement other than the tactic's own is cut for the tank beyond this from the spot (it walks back).
constexpr float TRIBUNAL_TANK_LEASH = 12.0f;
// The healer keeps within this of the tank (heals and the tank's threat both reach).
// [b34_hos] now around the hold spot (was 8 yd around the tank): 12 yd keeps the tank's forward spot (10.4 yd) inside,
// so a healer chased onto the tank ("move to tank") is not walked straight back.
constexpr float TRIBUNAL_HEALER_RADIUS = 12.0f;
constexpr float TRIBUNAL_HEALER_STAND = 5.0f;
// [b33_hos] The waves spawn ~80 yd from the hold spot (creature_summon_groups 28070: Protectors 943.1, 401.4,
// Stormcallers 967.0, 376.8, Golems 964.3, 381.9) and walk in. hos-h 20261026-000434 (1899 s): the tank's "reach
// melee" (60 yd), the holy paladin's "reach spell" (59.8 yd) and the casters' "move to ranged position" walked the
// whole group 25-38 yd out towards the wave; the Protectors Charged the healer (5k) and killed her 21 yd from the
// spot (12.2k melee, 3 on her). Nobody walks to an add this far from the spot, the adds come in.
// A damage dealer's walk to a target this far from the spot at most: Stormcallers cast from range (smart_scripts
// 27984 cast flag 64, combat move off) and stand up to ~40 yd from their victim at the spot; they must stay reachable.
constexpr float TRIBUNAL_DPS_REACH = 45.0f;
// [b33_hos] Challenging Shout / Roar once this many adds hit other members within its 10 yd.
constexpr uint32 TRIBUNAL_AOE_TAUNT_ADDS = 2;
// [b36_hos] A melee add this close to the hold spot keeps the melee damage dealers on the group (20 yd: the tank's
// forward spot 10.4 yd plus a Protector's walk-in and Charge distance to the healer). [b44_hos] moved here from
// HoSStrategy.cpp: also the melee's reach for a Stormcaller while a ranged damage dealer takes the far ones.
constexpr float TRIBUNAL_MELEE_GUARD = 20.0f;
}

namespace HoSShared
{
Creature* GetBrann(Player* bot);
bool IsBrannEventActive(Player* bot);
uint32 NextBrannGossip(Player* bot, Creature* brann);
Unit* NearestBrannAttacker(Player* bot, Creature* brann);
Unit* NearestTribunalHazard(Player* bot);
// A Tribunal add hitting a member who is not a tank (the healer's first), nullptr when none. Reads "attackers".
Unit* LooseTribunalAdd(PlayerbotAI* botAI);
// The Tribunal of Ages is running (cheap: instance state).
bool IsTribunalActive(Player* bot);
// The first alive tank of the group in this instance other than the bot, or nullptr.
Player* GroupTank(Player* bot);
// A Dark Rune Protector on the group's healer (they killed it in every heroic wipe), nullptr when none.
Unit* ProtectorOnHealer(PlayerbotAI* botAI);
// [b44_hos] The bot's current target when it is a live Dark Rune Protector at the group (within TRIBUNAL_MELEE_GUARD
// of the hold spot) that the bot has already hit: once the tank took it off the healer the bot finishes it.
Unit* PeeledProtector(PlayerbotAI* botAI);
// [b44_hos] A living ranged damage dealer of the group on this map other than the bot (not a healer or tank).
bool GroupHasRangedDps(Player* bot);
// [b44_hos] A melee damage dealer leaves this add to the ranged: a Dark Rune Stormcaller (or an unknown unit) farther
// than TRIBUNAL_MELEE_GUARD from the hold spot while the group has a ranged damage dealer to take it.
bool LeftToRanged(Player* bot, Unit* add);
// A live Searing Gaze or Dark Matter (within 40 yd of the bot) lies closer than `radius` to (x, y) and within
// `radius` of z, Tribunal only.
bool IsNearTribunalHazard(Player* bot, float x, float y, float z, float radius);
// The clearance kept from a Searing Gaze or Dark Matter (both hit 5 yd).
constexpr float TRIBUNAL_HAZARD_RADIUS = 7.0f;
bool IsLightningRing(Player* bot, Unit* boss);
bool IsGroundSlamActive(Player* bot);
// The bot is near Krystallus' room and he is not dead yet (cheap: instance state and one distance).
bool IsKrystallusAhead(Player* bot);
bool IsShatterPending(Unit* unit);
void CollectShatterSources(Player* bot, bool includeOwnPets, std::vector<Position>& sources);
float NearestShatterSourceDistance(Player* bot, std::vector<Position> const& sources);
// One of the short-lived summons above.
bool IsKrystallusShortSummon(Unit* unit);
// The first alive short-lived summon the bot owns, or nullptr.
Unit* OwnedShortSummon(Player* bot);
// The bot's spoke of the star around the boss: false for the tank (it holds the boss) or when the group is unknown.
bool StarSpot(Player* bot, Unit* boss, float radius, float& x, float& y);
}

class KrystallusGroundSlamTrigger : public Trigger
{
public:
    KrystallusGroundSlamTrigger(PlayerbotAI* botAI) : Trigger(botAI, "krystallus ground slam") {}
    bool IsActive() override;
};

// The bot owns a pet or guardian that must be parked away from the group before Stoned, or brought back after.
class KrystallusPetSpreadTrigger : public Trigger
{
public:
    KrystallusPetSpreadTrigger(PlayerbotAI* botAI) : Trigger(botAI, "krystallus pet spread") {}
    bool IsActive() override;
};

// A hunter with its pet out near a living Krystallus (wave 9: a parked pet still Shattered next to its hunter for
// 8k). A player dismisses the pet for this fight.
class KrystallusDismissPetTrigger : public Trigger
{
public:
    KrystallusDismissPetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "krystallus dismiss pet") {}
    bool IsActive() override;
};

// The bot owns a short-lived summon (Fire Elemental, Water Elemental, Treants, ...) near a living Krystallus: a
// player dismisses it from the pet frame for this fight.
class KrystallusDismissSummonTrigger : public Trigger
{
public:
    KrystallusDismissSummonTrigger(PlayerbotAI* botAI) : Trigger(botAI, "krystallus dismiss summon") {}
    bool IsActive() override;
};

class KrystallusRangedSpreadTrigger : public Trigger
{
public:
    KrystallusRangedSpreadTrigger(PlayerbotAI* botAI) : Trigger(botAI, "krystallus ranged spread") {}
    bool IsActive() override;
};

class SjonnirLightningRingTrigger : public Trigger
{
public:
    SjonnirLightningRingTrigger(PlayerbotAI* botAI) : Trigger(botAI, "sjonnir lightning ring") {}
    bool IsActive() override;
};

class HoSBrannGossipTrigger : public Trigger
{
public:
    HoSBrannGossipTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hos brann gossip") {}
    bool IsActive() override;
};

class HoSStayWithBrannTrigger : public Trigger
{
public:
    HoSStayWithBrannTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hos stay with brann") {}
    bool IsActive() override;
};

class HoSBrannAttackedTrigger : public Trigger
{
public:
    HoSBrannAttackedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hos brann attacked") {}
    bool IsActive() override;
};

class HoSTribunalHazardTrigger : public Trigger
{
public:
    HoSTribunalHazardTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hos tribunal hazard") {}
    bool IsActive() override;
};

// The tank in the Tribunal of Ages fight while an add hits another member (HoSShared::LooseTribunalAdd).
class HoSTribunalLooseAddTrigger : public Trigger
{
public:
    HoSTribunalLooseAddTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hos tribunal loose add") {}
    bool IsActive() override;
};

// The tank during the Tribunal of Ages, away from its hold spot next to Brann.
class HoSTribunalHoldPointTrigger : public Trigger
{
public:
    HoSTribunalHoldPointTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hos tribunal hold point") {}
    bool IsActive() override;
};

// The healer during the Tribunal of Ages, farther than TRIBUNAL_HEALER_RADIUS from the hold spot by Brann
// ([b34_hos]: was from the tank, who now meets the adds forward).
class HoSTribunalHealerHoldTrigger : public Trigger
{
public:
    HoSTribunalHealerHoldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hos tribunal healer hold") {}
    bool IsActive() override;
};

// A damage dealer during the Tribunal of Ages while a Dark Rune Protector hits the healer and is not its target.
class HoSTribunalProtectorTrigger : public Trigger
{
public:
    HoSTribunalProtectorTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hos tribunal protector") {}
    bool IsActive() override;
};

#endif
