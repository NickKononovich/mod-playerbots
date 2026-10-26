/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CLASSICSHARED_H
#define PLAYERBOTS_CLASSICSHARED_H

#include "Common.h"

#include <vector>

class Player;
class PlayerbotAI;
class Unit;

// Scarlet Monastery (map 189), Library: Arcanist Doan (SmartAI 6487).
namespace ScarletMonastery
{
inline constexpr char const* DOAN_NAME = "arcanist doan";

enum DoanSpells : uint32
{
    SPELL_DOAN_ARCANE_EXPLOSION = 9433,  // instant, 10 yd around him, every 6-10 s
    SPELL_DOAN_ARCANE_BUBBLE    = 9438,  // below 50 %, 8 s of immunity, linked to Detonation
    SPELL_DOAN_DETONATION       = 9435,  // 5 s cast, 30 yd around him; walls block it (no ignore-LoS attribute)
};

inline constexpr float ARCANE_EXPLOSION_RADIUS = 10.0f;
inline constexpr float DETONATION_RADIUS = 30.0f;

Unit* FindDoan(PlayerbotAI* botAI);
bool IsDetonationPending(Unit* doan);
// Out of Detonation: beyond its radius, or behind a wall from Doan.
bool IsSafeFromDetonation(Player* bot, Unit* doan, float x, float y, float z);

// Cathedral: Scarlet Commander Mograine's SmartAI sets instance data 1 to IN_PROGRESS one second into his fight, and
// instance_scarlet_monastery then sends every Monk, Abbot, Champion, Centurion, Wizard and Chaplain within 80 yd of
// him (CATHEDRAL_PULL_RANGE: the whole chapel) at his victim. Wave 34 (12:44): a hunter stepped to 12 yd of him
// while the tank fought a Champion 34 yd from him; six mobs engaged the hunter at 48-67 yd in the same tick and the
// group wiped to the rest of the chapel. Players clear the chapel first, pulling its pairs out toward the door, and
// keep out of Mograine's aggro (21 yd at level 39 against his 40) until he stands alone.
inline constexpr uint32 SCARLET_MONASTERY_MAP_ID = 189;
inline constexpr uint32 NPC_COMMANDER_MOGRAINE = 3976;
inline constexpr float MOGRAINE_CALL_RADIUS = 80.0f;  // instance_scarlet_monastery CATHEDRAL_PULL_RANGE
// Nobody comes closer to an idle Mograine while his chapel stands: his aggro radius, both reaches and a margin.
inline constexpr float MOGRAINE_KEEP_AWAY = 30.0f;

// One of the entries Mograine's engage sends at his victim.
bool IsMograineCallEntry(uint32 entry);
// Mograine alive and idle while at least one mob of his call is alive and idle (grid search, cached a second per
// bot); `idleCalled` gets that count. nullptr otherwise.
Unit* IdleMograineWithChapel(Player* bot, uint32* idleCalled = nullptr);
// The alive, idle mobs his engage would bring.
void CollectMograineCall(Unit* mograine, std::vector<Unit*>& out);
}

// Scholomance (map 289) caster packs (SmartAI, Spell.dbc). The Acolyte's Mana Burn 17615 is instant (no kick) and
// its SmartAI picks a random mana user within 20 yd (IsWithinCombatRange, so plus both combat reaches); the
// Spectral Researcher's Mana Burn 17630 picks within 30 yd and its Wail of Souls 17631 hits everyone within 30 yd.
// Wave 24: Mana Burn 951 + Wail 766 + Shadow Bolt 626 left the healer at 0 mana in 15 s. Ranged and healers keep
// out of the Acolytes' 20 yd (it fits inside heal and spell range); the Researchers' 30 yd is only a preference.
namespace Scholomance
{
inline constexpr uint32 MAP_SCHOLOMANCE = 289;

enum ScholomanceNpcs : uint32
{
    NPC_SCHOLOMANCE_ACOLYTE = 10471,
    NPC_SPECTRAL_RESEARCHER = 10499,
};

inline constexpr float ACOLYTE_MANA_BURN_REACH = 20.0f;
inline constexpr float RESEARCHER_REACH = 30.0f;

// The hall under the entrance ramp (floor z 115.8, x 160-243, y -19..60): its packs stand 5-15 yd apart and one
// chain runs through them. The ramp climbs west from its foot (236, 54, 116) to the top (205, 54, 129). Waves 31
// and 35: after a fight on the ramp foot the group stayed there; heroic 13:07: 4 s after a LoS pull's fight ended
// with the tank at (236, 54, 116), 27 mobs came with no pull at all (the next pack 22 yd off, the hall patrols'
// paths through the foot). Players pull the hall from the ramp and go back up after each fight.
inline constexpr float HALL_FLOOR_MAX_Z = 121.0f;  // below this, inside the box, a bot stands on the hall floor
inline constexpr float HALL_MIN_X = 150.0f;
inline constexpr float HALL_MAX_X = 252.0f;
inline constexpr float HALL_MIN_Y = -30.0f;
inline constexpr float HALL_MAX_Y = 64.0f;
inline constexpr float HALL_RALLY_X = 205.0f;      // the ramp top, where the LoS pulls gather the group
inline constexpr float HALL_RALLY_Y = 54.5f;
inline constexpr float HALL_RALLY_Z = 128.8f;
inline constexpr float HALL_RALLY_REACH = 32.0f;   // an idle hall mob this close: the floor is no place to stand
inline constexpr uint32 HALL_RALLY_AFTER_FIGHT_MS = 20000;  // only right after a fight (the travel may walk down)
// [rally_when_needed] the rally is a regroup: it runs only when one of these holds (ClassicTriggers.cpp)
inline constexpr float HALL_RALLY_LOW_HEALTH_PCT = 60.0f;   // a member under this health ...
inline constexpr float HALL_RALLY_LOW_MANA_PCT = 45.0f;     // ... or a mana user under this mana (or one dead)
inline constexpr float HALL_RALLY_SCATTERED = 30.0f;        // a member this far from the tank
inline constexpr float HALL_RALLY_WAKE_MARGIN = 3.0f;       // an idle hall mob's real aggro reach plus this ...
inline constexpr float HALL_RALLY_PATROL_MARGIN = 10.0f;    // ... or plus this for a walking patrol
inline constexpr uint32 HALL_RALLY_LEAD_MEMORY_MS = 60000;  // the tank's last harness waypoint counts this long

bool OnHallFloor(float x, float y, float z);
// An idle mob of the hall floor within HALL_RALLY_REACH of the bot (grid search), or nullptr.
Unit* IdleHallMobNear(Player* bot);

// [b32_brs] The Viewing Room (Dawn's Gambit, quest 4771): Vectus 10432 and Marduk Blackpool 10433 are faction 15
// (FactionTemplate 15: faction 7, no enemy mask - neutral) with npcflag 1 (gossip menus 3648 / 3649) in both
// difficulties, and no script ever makes them hostile: the Gambit (item 12368, spell 17045 at the spell focus 823 =
// GO 176111) summons GO 176110 whose SmartAI turns the 32 Scholomance Students 10475 into skeletons (spell 18115,
// Set Faction 21) and has Vectus say "What is this?! How dare you!" - then a player attacks the two himself (their
// On Aggro sets data 3 3: the other one and the students attack). The bots' aggressive target refuses a neutral
// unit with npc flags ("aggressive target: npc flags"), so the tank attacks them here once the Gambit has run.
inline constexpr uint32 NPC_VECTUS = 10432;
inline constexpr uint32 NPC_MARDUK_BLACKPOOL = 10433;
inline constexpr uint32 NPC_SCHOLOMANCE_STUDENT = 10475;
inline constexpr uint32 STUDENT_FACTION_BEFORE_GAMBIT = 15;
inline constexpr float VIEWING_ROOM_ATTACK_RANGE = 30.0f;  // the tank this close to the boss (the harness walk)
inline constexpr float VIEWING_ROOM_STUDENT_RANGE = 100.0f;  // the Gambit's own reach (smart_scripts 176110)
// The tank's attack target among Vectus / Marduk, or nullptr: the bot is a tank out of combat, the boss alive, idle,
// within VIEWING_ROOM_ATTACK_RANGE and in sight, and the Gambit has run (no living Student still faction 15 within
// VIEWING_ROOM_STUDENT_RANGE of the boss: they are skeletons or dead). Grid search: from a trigger / an action.
Unit* ViewingRoomBossToAttack(Player* bot);

// A mana user that is not the tank: ranged damage dealers and healers keep out of the Acolytes' pick.
bool KeepsOutOfManaBurn(Player* bot);
// The nearest Acolyte of the fight that has the bot inside its Mana Burn pick and is not fighting the bot itself
// (a mob meleeing the bot follows it: the flee-to-tank logic owns that), or nullptr.
Unit* ManaBurnThreat(PlayerbotAI* botAI);
}

// [b31_ubrs_door] Upper Blackrock Spire (map 229), the stadium: Rend's six waves spawn in the drake pen east of the
// wave portcullis 175185 (Doodad_Drake_Rider_portcullis, acore_world 87843 at 194.2,-420.2,110.8, facing west;
// creature_summon_groups 10429 at x 201-214) and boss_rend_blackhand SummonWave / boss_gyth EVENT_SUMMONED_1 open
// it with UseDoorOrButton - it closes again 10 s later (gameobject_template data2 10000). Nothing opens it after
// Gyth: a bot still in the pen then is locked in for good (the exit 175186 lies on the stadium's far side).
// ubrs-n 20261025-184229-10B-43: the Prot Warrior met a wave in the doorway (x 194.8) as the gate closed, the
// group followed him, wave 6 and Gyth (summoned at 211.8,-397.6) were fought inside, and after Rend's DONE the
// group stood 45 min in the pen (x 191.8-224.5, y -447..-391); 10A-8 115805 the same. A player meets the waves on
// the stadium side and steps back out while the gate is up.
namespace BlackrockSpire
{
// MAP_BLACKROCK_SPIRE (229) comes from AreaDefines.h; a namespace copy made it ambiguous under using-namespace.
inline constexpr uint32 GO_WAVE_PORTCULLIS = 175185;
inline constexpr float PEN_DOOR_X = 194.2f;      // the portcullis plane (it faces west, x constant)
inline constexpr float PEN_ENTERED_X = 194.6f;   // east of this the bot stands on the pen side of the bars
inline constexpr float PEN_DEEP_X = 196.5f;      // a mob east of this is inside the pen, not in the doorway
inline constexpr float PEN_MIN_Y = -450.0f;
inline constexpr float PEN_MAX_Y = -388.0f;
inline constexpr float PEN_MIN_Z = 105.0f;
inline constexpr float PEN_MAX_Z = 118.0f;
inline constexpr float PEN_EXIT_X = 187.0f;      // the stadium side of the gate, on its axis
inline constexpr float PEN_EXIT_Y = -420.3f;
inline constexpr float PEN_EXIT_Z = 110.6f;

bool InPenBand(float y, float z);
bool InDrakePen(float x, float y, float z);       // east of PEN_ENTERED_X
// The wave portcullis near the bot stands open (GO_STATE_ACTIVE); a grid search, so only for a bot near it.
bool WavePortcullisOpen(Player* bot);
}

// Event starters a player performs in the older dungeons: a gameobject to use, an NPC gossip, or an item used on a
// gameobject, each only while its instance state says it is due. The interactor of the group does them out of
// combat, through its own session (DungeonInteract.h), so every server-side check applies. Each step that needs an
// item (a key, a mallet, the incendiary bombs) is done only by a bot that carries it.
#include "ObjectGuid.h"

class WorldObject;

namespace DungeonEvents
{
enum class StepKind : uint8
{
    UseObject,
    Gossip,
    UseItemOnObject,
};

struct Step;

// The step the interactor does next and its object (grid search: call from a slow trigger or an action).
// skip: objects this bot is done with or cannot walk to for now (the action's own memory).
Step const* NextStep(PlayerbotAI* botAI, WorldObject*& target, GuidSet const* skip = nullptr);
bool Perform(PlayerbotAI* botAI, Step const& step, WorldObject* target);
// Members within this of the object, all out of combat, before a step that starts a fight (0 = no wait).
float GatherRadius(Step const& step);
StepKind Kind(Step const& step);
char const* Describe(Step const& step);
}

// [b32_st] Sunken Temple (map 109), the Shade of Hakkar event (Egg of Hakkar at the Evil God Spell Focus): the
// Nightmare Suppressors 8497 (smart_scripts 8440 id 21-22, two every 70 s from the side doors) walk to the Shade and,
// out of combat, add 1 a second to its counter 2 within 40 yd (8497 id 5, after 4 s); at 25 the Shade despawns and
// the event resets (8440 id 9-10, instance data 2 NOT_STARTED). They are civilian (flags_extra 2), never attack, and
// the Hakkari Bloodkeepers / Minions keep the group in combat meanwhile. A group kills each one as it comes: a damage
// dealer (not the tank, not a healer) leaves its target for the nearest idle Suppressor.
namespace SunkenTemple
{
inline constexpr uint32 ST_MAP_ID = 109;
inline constexpr uint32 NPC_NIGHTMARE_SUPPRESSOR = 8497;
inline constexpr float SUPPRESSOR_ATTACK_RANGE = 60.0f;
inline constexpr uint32 SUPPRESSOR_CACHE_MS = 1000;
// The nearest Suppressor out of combat within SUPPRESSOR_ATTACK_RANGE for a damage dealer of the group, or nullptr
// (the grid search at most every SUPPRESSOR_CACHE_MS per bot).
Unit* SuppressorToAttack(Player* bot);

// [b33_st] Jammal'an's room: the Atal'ai Deathwalker 5271 is a paladin-class SmartAI mob whose only ranged damage is
// Shadow Word: Pain 11639 cast WITHOUT SMARTCAST_COMBAT_MOVE (smart_scripts 5271 id 0; Fear 12096 id 1), so it chases
// and melees - yet MobRoles' ranged-caster rule (paladin/mage class + a ranged magic damage spell) made the careful
// planner's KeepsRange true for it: every room pack counted 17-24 mobs through "reaches #N, which keeps its range"
// chains ("careful pull hopeless"), all 27 room mobs were skipped, the boss hold emptied and the harness body-pulled
// Jammal'an with the room standing; his On Aggro Call For Help (5710 id 8, 90 yd) brought 20 adds, 4 of 5 died
// (st-h 20261026-000434-st-h-5A-8, 00:49:20). The room cleared pack by pack afterwards (pulls 80-83: 9/2/5/1 kills)
// and he died in 41 s with no death. true: the mob fights in melee whatever its class (KeepsRange false).
bool FightsInMelee(Unit const* unit);
}

#endif
