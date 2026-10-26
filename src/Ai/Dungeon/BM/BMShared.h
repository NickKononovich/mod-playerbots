/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BMSHARED_H
#define PLAYERBOTS_BMSHARED_H

#include "Common.h"
#include "ObjectGuid.h"

#include <string>

class Creature;
class InstanceScript;
class Player;
class PlayerbotAI;
class Unit;

// Caverns of Time: The Black Morass (Opening of the Dark Portal), map 269.
//
// What the AzerothCore scripts do (scripts/Kalimdor/CavernsOfTime/TheBlackMorass, smart_scripts, conditions):
// - A player within 20 yd of Medivh (15608) starts the event (npc_medivh_bm::MoveInLineOfSight). Medivh never fights.
// - Rifts (Time Rift 17838) open one at a time on 4 fixed spots 95-138 yd from Medivh; 6 s after a rift opens it
//   summons its keeper (Rift Keeper 21104/21148, Rift Lord 17839/21140; rift 6 Chrono Lord Deja 17879 / heroic repeat
//   21697, rift 12 Temporus 17880 / 21698, rift 18 Aeonus 17881) and channels Rift Channel on it. Keepers and the two
//   first bosses stay at the rift until pulled; they chase like any mob (no leash in a dungeon). Killing the keeper
//   despawns the rift wherever it dies (npc_time_rift::SummonedCreatureDies); the next rift follows 1 s later while at
//   most one other rift is open, else after 90 s (2 min from rift 13 on) anyway.
// - Every 15 s (from 16 s) a rift summons an add: Infinite Assassin / Whelp (x3) / Chronomancer / Executioner /
//   Vanquisher (17835 21137 21818 17892 21136 18994 21138 18995 21139). The add walks to a home point 14 yd from
//   Medivh (REACT_DEFENSIVE) and on reaching it (SAI On Reset) casts Corrupt Medivh 31326 on him: every 3rd tick takes
//   1 shield point (spell_black_morass_corrupt_medivh). Aeonus walks to Medivh by himself and casts 37853 (2 points).
//   At 0 the event fails, Medivh dies and everything despawns.
// - Chrono-beacon 24289 (one per player) comes from Sa'at (20201) at the entrance through gossip menu 8088: option 0
//   needs quest 10297 in the log (not rewarded), option 1 needs it rewarded, both need rift number 0 (conditions 15).
//   It casts 31479 (Time Keeper 17918, despawns after 30 s, Sand Breath). Deja, Temporus and Aeonus banish a Time
//   Keeper within 20 yd of them (31550): the guide's "all beacons on Aeonus" does nothing here.
// - Temporus: Hasten 31458 every 20 s, Mortal Wound, heroic Spell Reflection 38592. Aeonus: Enrage 37605 ("Frenzy")
//   every 30 s, Time Stop, Sand Breath.
namespace BMShared
{
constexpr uint32 MAP_ID = 269;

// the_black_morass.h DataTypes
constexpr uint32 DATA_AEONUS = 2;
constexpr uint32 DATA_MEDIVH = 10;
constexpr uint32 DATA_SHIELD_PERCENT = 13;
constexpr uint32 DATA_RIFT_NUMBER = 14;

constexpr uint32 RIFT_HARD_FIRST = 13;  // instance_the_black_morass: from 13 on the rifts repeat after 2 min
constexpr uint32 RIFT_AEONUS = 18;

enum BMNpcs : uint32
{
    NPC_MEDIVH = 15608,
    NPC_TIME_RIFT = 17838,
    NPC_TIME_KEEPER = 17918,
    NPC_SAAT = 20201,

    NPC_RIFT_KEEPER_WARLOCK = 21104,
    NPC_RIFT_KEEPER_MAGE = 21148,
    NPC_RIFT_LORD = 17839,
    NPC_RIFT_LORD_2 = 21140,
    NPC_CHRONO_LORD_DEJA = 17879,
    NPC_INFINITE_CHRONO_LORD = 21697,
    NPC_TEMPORUS = 17880,
    NPC_INFINITE_TIMEREAVER = 21698,
    NPC_AEONUS = 17881,

    NPC_INFINITE_ASSASSIN = 17835,
    NPC_INFINITE_ASSASSIN_2 = 21137,
    NPC_INFINITE_WHELP = 21818,
    NPC_INFINITE_CHRONOMANCER = 17892,
    NPC_INFINITE_CHRONOMANCER_2 = 21136,
    NPC_INFINITE_EXECUTIONER = 18994,
    NPC_INFINITE_EXECUTIONER_2 = 21138,
    NPC_INFINITE_VANQUISHER = 18995,
    NPC_INFINITE_VANQUISHER_2 = 21139,
};

enum BMSpells : uint32
{
    SPELL_CORRUPT_MEDIVH = 31326,
    SPELL_CORRUPT_MEDIVH_AEONUS = 37853,
    SPELL_HASTEN = 31458,
    SPELL_TEMPORUS_REFLECT = 38592,
    SPELL_AEONUS_ENRAGE = 37605,
};

constexpr uint32 ITEM_CHRONO_BEACON = 24289;
constexpr uint32 GOSSIP_MENU_SAAT = 8088;

// The group's spot beside Medivh (Medivh at -2025.3, 7119.6, 22.75): every add ring point (14 yd) is in range.
constexpr float HOLD_X = -2021.0f;
constexpr float HOLD_Y = 7121.5f;
constexpr float HOLD_Z = 22.7f;
constexpr float SAAT_X = -1547.06f;
constexpr float SAAT_Y = 7113.63f;

constexpr float THREAT_RADIUS = 45.0f;            // an add this close to Medivh walks to him or drains him
constexpr float GUARDIAN_SCAN_RADIUS = 160.0f;    // the farthest rift is 138 yd off, keepers spawn 10 yd from it
constexpr float GUARDIAN_ENGAGE_RADIUS = 30.0f;   // damage dealers fight a keeper only this close to Medivh
// A keeper that stands off (the mage Rift Keeper 21148: Frostbolt with SMARTCAST_COMBAT_MOVE) follows the dragging
// tank only to its cast range, 30-40 yd short of Medivh (bm-n 20261025-163739: "drags Rift Keeper (38 yd from
// Medivh) ... 6 yd to go" 51 times, nobody hit it, no keeper ever died): the damage dealers take it this far out.
constexpr float GUARDIAN_STANDOFF_RADIUS = 55.0f;  // 41-47 yd seen; ranged at the 25 yd leash reach it
constexpr float STANDOFF_GAP = 8.0f;              // a still keeper this far from its victim stands off (casts)
constexpr float HOLD_LEASH = 25.0f;               // non-tanks stay this close to Medivh during the event
constexpr float HOLD_SPREAD = 4.0f;
constexpr float DRAG_ARRIVED = 6.0f;              // the tank stops dragging a keeper this close to the hold spot
constexpr float FETCH_PULL_RANGE = 25.0f;         // the tank opens on a keeper from this far, in sight
constexpr int32 FETCH_SHIELD_PCT = 60;            // above this shield the tank fetches a keeper even with adds up
// Above this shield the tank leaves channeling adds to the damage dealers and fetches the keeper: with three or four
// rifts open an add channels nearly all the time, and a tank that tags every one never fetches (bm-n build 25: rift
// 5 -> 6 took 10.5 min at 100% shield, no keeper died).
constexpr int32 TANK_CHANNEL_SHIELD_PCT = 90;
constexpr float SAAT_DETOUR_RANGE = 100.0f;       // zone-in (-1496, 7035) is 94 yd from Sa'at: covers the entrance
constexpr float BEACON_USE_RANGE = 25.0f;         // the drake helps where it is summoned: at Medivh only
constexpr int32 BEACON_SHIELD_PCT = 50;
constexpr uint32 BEACON_MIN_THREATS = 2;
constexpr float TIME_KEEPER_SEARCH = 60.0f;

InstanceScript* Instance(Player* bot);
uint32 RiftNumber(Player* bot);
int32 ShieldPct(Player* bot);
// The portals run: rift 1+, shield above 0, Aeonus not dead.
bool IsEventInProgress(Player* bot);
// Before Medivh's start: Sa'at still hands out beacons (conditions 15/8088: instance data 14 == 0).
bool IsEventNotStarted(Player* bot);
Creature* Medivh(Player* bot);

bool IsAddEntry(uint32 entry);
bool IsGuardianEntry(uint32 entry);
bool IsBossKeeperEntry(uint32 entry);
// The unit's Corrupt Medivh aura sits on Medivh.
bool IsCorruptingMedivh(Creature* medivh, Unit* unit);

// Scans (cached by the "bm medivh threats" / "bm rift guardians" values; never call them from a trigger).
// Adds within THREAT_RADIUS of Medivh: the corrupting ones first, then by distance to him.
GuidVector ScanThreats(Player* bot);
// Living keepers and rift bosses, nearest to Medivh first.
GuidVector ScanGuardians(Player* bot);
// The adds of "bm medivh threats" whose Corrupt Medivh aura sits on him (no grid scan: Medivh's auras only).
GuidVector ScanCorrupting(PlayerbotAI* botAI);

// From the cached values.
Unit* FirstThreat(PlayerbotAI* botAI);
// An add of "bm corrupting adds" still channels Corrupt Medivh (the value is at most 500 ms old).
bool AnyCorrupting(PlayerbotAI* botAI);
// The add this bot attacks to defend Medivh. While adds channel Corrupt Medivh: a ranged damage dealer takes the one
// of its slot among the group's ranged (every channel breaks within one cast), a melee or the tank the nearest one.
// Without a channel: the first add of "bm medivh threats".
Unit* ThreatFor(PlayerbotAI* botAI);
Unit* NearestGuardian(PlayerbotAI* botAI);
// The nearest keeper or rift boss when it stands within GUARDIAN_ENGAGE_RADIUS of Medivh in a fight (dragged there,
// or Aeonus walking in): the damage dealers kill it whenever no add channels.
Unit* GuardianAtMedivh(PlayerbotAI* botAI);
bool IsThreat(PlayerbotAI* botAI, Unit* unit);
bool IsFarGuardian(Player* bot, Unit* unit);
// A keeper fighting the group from a standstill STANDOFF_GAP or more from its victim: it will not come closer.
bool IsStandingOff(Player* bot, Unit* unit);
// An alive main tank in this instance other than the asking bot (who fetches the keepers).
bool HasLivingTank(Player* bot);

// "[tag] bot: message", at most once per intervalMs per (instance, bot, tag, key). Grep "[bm ".
void Log(Player* bot, char const* tag, std::string const& key, std::string const& message, uint32 intervalMs = 10000);
}  // namespace BMShared

#endif
