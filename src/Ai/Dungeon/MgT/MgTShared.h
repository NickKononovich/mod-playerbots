/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MGTSHARED_H
#define PLAYERBOTS_MGTSHARED_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class Creature;
class Player;
class PlayerbotAI;
class SpellInfo;
class Unit;

namespace MagistersTerrace
{
constexpr uint32 MAP_MAGISTERS_TERRACE = 585;

constexpr float SELIN_LEASH_X   = 216.0f;
constexpr float SELIN_SAFE_X    = SELIN_LEASH_X + 4.0f;
constexpr float SELIN_REGROUP_X = 232.0f;
constexpr float SELIN_FLEE_SLACK = 6.0f;

constexpr float GROUND_TIER_STEP   = 3.0f;
constexpr float GROUND_SEARCH_UP   = 5.0f;
constexpr float GROUND_SEARCH_DOWN = 12.0f;

constexpr uint8 CONTROL_INTERRUPT_URGENCY = 2;

struct EscapeSpot
{
    float x;
    float y;
    float z;
};

using EscapeSpots = std::vector<EscapeSpot>;

struct LapseSpot
{
    float x;
    float y;
};

struct LapseWorld
{
    std::vector<Unit*> spheres;
    std::vector<LapseSpot> burns;
};

struct KiteState
{
    float turn = 0.0f;
    uint32 lastMs = 0;
};

enum class PhoenixRing : uint8
{
    None,
    Threatens,
    Covers
};

enum class FocusOrder
{
    None,
    Kael,
    Delrissa,
    Trash
};

Unit* GetSelin(Player* bot);
Unit* GetKaelthas(Player* bot);

Unit* GetActiveFelCrystal(Player* bot);

void CollectDampeningEscapes(Player* bot, EscapeSpots& out);

Unit* GetGlaiveThrowingMageGuard(Player* bot);

void CollectNovaEscapes(Player* bot, EscapeSpots& out);

std::string CastInterrupt(PlayerbotAI* botAI, Unit* target, uint8 urgency);

Unit* GetInterruptTarget(Player* bot);

uint8 GetInterruptUrgency(Unit* caster);
uint8 GetRetinueInterruptUrgency(Unit* caster);

Unit* GetFocusTarget(Player* bot, ObjectGuid& latched);

ObjectGuid ResolveFocusOrder(PlayerbotAI* botAI);

FocusOrder ResolveFocusOrderOwner(PlayerbotAI* botAI);

void CollectFocusExclusions(Player* bot, std::vector<ObjectGuid>& out);

Unit* GetEnragedWretched(Player* bot);

// Selin's Wretched are among the bot's attackers (the cached "attackers" value, entries only).
bool WretchedInFight(PlayerbotAI* botAI);

std::vector<std::string> const& TauntSpellNames();

void ClampIntoRoom(float& x, float& y);

bool ShouldHoldTremorTotem(Player* bot);

Unit* GetDelrissaFocusTarget(Player* bot, ObjectGuid& latched);

// The crowd control rank of a retinue member as it stands (boss_priestess_delrissa.cpp): its own danger rank, and
// first of all a melee lackey whose victim is a healer of the group (the lackeys re-pick their victims every 8-10 s
// by distance, health and armour: cloth healers). 0 = never.
uint8 DelrissaCcPriority(Unit* unit);

void CollectDelrissaInterruptPreference(Player* bot, std::vector<Unit*>& out);

void CollectDelrissaPets(Player* bot, std::vector<ObjectGuid>& out);

// Priestess Delrissa crowd control: a mage, hunter or warlock puts its own CC icon on one retinue member (melee
// lackeys first, never Delrissa, Apoko or the current kill target), at most two at a time; the class "cc" strategy
// then casts the spell on the icon and the dps leave it alone.
bool DelrissaRetinueEngaged(Player* bot);
std::string DelrissaCcSpell(Player* bot);

// [delrissa_retinue_left] boss_priestess_delrissa.cpp: the encounter is DONE only once Delrissa AND every one of her
// four lackeys are dead (HelpersKilled); a lackey that evades with her dead respawns her. mgt-h 2026-10-25
// (20261025-163739-mgt-h-5A-15): after the second wipe the lackeys went home, Delrissa chased a revived member to
// the entrance and died there - "dead, kill credit not observed", 25 minutes without a target, Kael's door shut.
// The nearest alive lackey that fights nobody, while Delrissa lies dead (nullptr: none, or she lives).
Unit* DelrissaRetinueLeft(Player* bot);
uint32 CountControlledRetinue(Player* bot);
Unit* PickDelrissaCcTarget(Player* bot, SpellInfo const* spellInfo);
// [b36_mgt] The retinue member this bot's own Hex / Polymorph mark sits on while it is not held (nullptr: no such
// mark, it is held, it backed off, or the bot's spell is another one): the mark action casts it again itself.
Unit* DelrissaOwnUnheldMark(Player* bot);
constexpr uint32 DELRISSA_MAX_CROWD_CONTROL = 2;
// [b44_mgt] A lackey of Delrissa (or Zelfan's sheep): its threat table is the script's (RecalculateThreat every
// 8-10 s: 1e7 x distance x armour for every player), so the group's threat rules mean nothing on it.
bool IsDelrissaScriptedThreat(Unit* unit);

// Healer spacing for Priestess Delrissa: her retinue re-picks targets every 8-10 s by distance, health and
// armour (boss_priestess_delrissa.cpp RecalculateThreat), so a healer standing close at the pull or later is
// the first one they all jump. Empty when nothing to do; one spot to walk to; or the healer's own position
// when it must simply hold (follow and formation moves are then suppressed).
void CollectDelrissaHealerSpot(Player* bot, EscapeSpots& out);

void CollectFlameStrikeEscapes(Player* bot, EscapeSpots& out);

void CollectPhoenixEscapes(Player* bot, EscapeSpots& out);

PhoenixRing GetPhoenixRing(Player* bot);

Unit* GetKaelInterruptTarget(Player* bot);

bool IsKaelUnattackable(Player* bot);

Unit* GetKaelFocusTarget(Player* bot);

bool IsGravityLapseActive(Player* bot);

void CollectLapseWorld(Player* bot, LapseWorld& out);

bool GetGravityLapseKite(Player* bot, LapseWorld const& world, LapseSpot& out, KiteState& state);

bool GetGravityLapseSpot(Player* bot, LapseWorld const& world, LapseSpot& out);

bool GetGravityLapseDodge(Player* bot, LapseWorld const& world, LapseSpot& out);

bool IsLapseMeleeSlot(Player* bot);

bool OnLapseStation(Player* bot, Unit* kael, LapseSpot const& spot);

bool LapseThreatened(Player* bot, LapseWorld const& world);
// [b44_mgt] a healer / ranged member inside the kite arena, out of every burn, with every sphere's sweep far enough
// that a cast ends before one reaches it: it stands and casts instead of taking its lapse spot
bool LapseCasterMayStand(Player* bot, LapseWorld const& world);

bool GetLapseApproach(Player* bot, LapseWorld const& world, LapseSpot const& station, bool allowBlocked,
                      LapseSpot& out);

void CollectMeleePhoenixExclusions(Player* bot, std::vector<ObjectGuid>& out);

// Selin Fireheart's room is cleared pack by pack from the corridor before she is pulled (boss_selin_fireheart.cpp,
// creature 96875, creature_formations 96825/96828/96831 groupAI 3, worldserver.conf CreatureFamilyAssistance*):
// - nothing links her Wretched to her in the database: three formations of their own (6 south at y -16..-23,
//   6 north at y +17..+24, 2 standing at 231.6,+-2), Selin in none, linked_respawn only ties their respawn to hers;
// - the link is the core's call for assistance (Creature::CallAssistance): a creature that engages, and every 3 s
//   while it fights, brings every idle creature of its faction within 10 yd + both combat reaches (Wretched 1.65,
//   Selin 2.25) in line of sight, 2 s later. The centre pair stands 11.6 yd from her (limit 13.9): pulling either
//   of them always brings Selin 2 s later (wave 26: pull at 10:32:21, Selin engaged at 10:32:23). The side packs
//   stand 23-28 yd from her and 15+ yd from the pair (Wretched-Wretched limit 13.3): each comes alone;
// - aggro (Creature::CanStartAttack): 19 yd 3D for the lvl 69 Wretched against a lvl 70 (no radii), and never
//   with a height gap over 3 yd: Selin on her dais (z 1.8, floor -2.9) is not aggroed from the floor. The door
//   (x 215, y -4..5) is 17-18 yd from the pair: whoever walks through it pulls the pair and Selin (wave 26: the
//   druid at 214.3,-3.1 was 18.3 yd from the Skulker when they came);
// - Selin cannot attack anybody at x <= 216 (CanAIAttack): engaged with nobody in the room she mostly evades, but
//   not always (Snapshot::selinInert: she stays engaged with no victim and the door stays shut).
// The tactic: the group waits at the corridor rally (200,-3), out of every pack's reach; the tank pulls one pack at
// a time with its ranged pull from a spot that sees the pack and stays out of every other idle pack's reach (a side
// pack when such a spot exists, else the centre pair, which brings Selin: she evades, as nobody is in her room),
// and the pack is fought at the rally. The door is 17-18 yd from the pair, so from the corridor a side pack is seen
// only through it, within 30 yd, from spots inside the pair's reach (navmesh 585 + wall model): in practice the pair
// comes first, then the side packs are pulled from the doorway. Selin is pulled by the usual planner once all 14
// Wretched are dead.
namespace SelinRoom
{
constexpr float RALLY_X = 200.0f;
constexpr float RALLY_Y = -3.0f;
constexpr float RALLY_Z = -2.8f;
constexpr float RALLY_ARRIVED = 3.0f;
// [selin_ac_final_pull] AzerothCore: Selin never evades with nobody to hit (CanAIAttack x > 216) and her room door
// (188065, DOOR_TYPE_ROOM) shuts on IN_PROGRESS, so the centre pair - whose pull always brings her (the core's call
// for assistance) - is pulled LAST, with the whole group at this stage spot just outside the pair's aggro distance
// (23 yd from both, heroic aggro 21 + margin), and everybody steps into the room (x >= SELIN_SAFE_X, 11 yd, under
// 2 s) as the pair comes, before she joins 2 s later: the door shuts with the group inside. The pair dies first, then
// Selin (her crystals as she channels them).
constexpr float STAGE_X = 208.5f;
constexpr float STAGE_Y = -1.0f;
constexpr float STAGE_Z = -2.8f;
// A member farther than this from the rally walks back to it while the room waits for the next pull.
constexpr float GATHER_STAND = 6.0f;
// Every member within this of the rally counts as gathered for the pull.
constexpr float GATHER_RADIUS = 10.0f;
constexpr float SPOT_ARRIVED = 1.0f;
// A member this far inside an idle mob's aggro range + this, in its sight, walks back to the rally.
constexpr float DRIFT_MARGIN = 1.0f;

struct IdleMob
{
    ObjectGuid guid;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float aggro = 0.0f;  // Creature::GetAggroRange against the bot
    uint8 pack = 0;
};

struct Pack
{
    std::vector<ObjectGuid> members;
    bool linked = false;  // within Selin's call for assistance: pulling it brings her
    bool fighting = false;
};

struct Snapshot
{
    uint32 ms = 0;
    bool active = false;      // the bot is near her room, Selin alive and idle, a Wretched of her room alive
    bool selinEngaged = false;  // Selin alive and in combat (the packs and `idle` are still filled in)
    // Selin engaged for INERT_MS with nobody to hit (CanAIAttack: x > 216) and not evading: the pair brought her and
    // she stays in combat (wave 30 13:10:44: 12+ minutes, the zone-wide combat keeps every member "in combat"). She
    // casts nothing then (BossAI::UpdateAI stops at UpdateVictim) and the door stays shut on IN_PROGRESS: see
    // `selinCommitted`.
    bool selinInert = false;
    uint32 selinTargetlessMs = 0;  // since when she is engaged with no victim (0: she is not)
    bool fighting = false;    // a room pack or the bot's group is in combat
    bool otherTrash = false;  // other idle trash stands near the rally: the usual planner pulls it first
    ObjectGuid selin;
    std::vector<Pack> packs;
    std::vector<IdleMob> idle;  // every alive member of the packs not in the fight
    // Bit per pack: an idle member of it sees the bot within its aggro range + DRIFT_MARGIN (Creature::CanStartAttack
    // needs line of sight; the walk round the corridor stairs passes 17 yd from the side packs behind the wall).
    uint32 reachPacks = 0;
    // Every alive Wretched of her floor (AREA_RADIUS round her), whether idle, fighting at the rally or chasing:
    // the room is cleared only once this is 0 (`packs` leaves out the ones fought beyond ROOM_RADIUS).
    uint32 wretchedAlive = 0;
    // Selin is engaged and not evading, with a group player as victim or inert with the room clear: her fight is on
    // and the room rules let go (no leash, no hold, no room pull, no rally); the members outside walk in and
    // everybody fights her (room entry order).
    bool selinCommitted = false;
    // [selin_ac_final_pull] Selin idle, every idle Wretched left belongs to a pack linked to her (the centre pair),
    // none fighting: the group gathers at the stage spot and the tank pulls the pair deliberately.
    bool finalPull = false;
    uint32 committedLogMs = 0;  // the last [selin engaged] line of this bot (lives on in the per-bot cache)
    // Selin is engaged and her victim is a player of the bot's group (somebody stands at x > 216 in her room).
    bool selinHasVictim = false;
    // Selin is engaged and her victim is a pet or guardian of the group (it is called back: "mgt selin pet out").
    ObjectGuid selinPetVictim;
};

// The room entry order (mgt-h 2026-10-25, 0/17: every second Selin engage came with the tank alone at x 218-228
// chasing a side-pack Wretched while the group stood at the rally; the room door 188065, DOOR_TYPE_ROOM, shut on
// IN_PROGRESS and the tank fought her alone for up to 300 s):
// - while any Wretched of her floor is alive nobody steps past LEASH_EDGE_X (she can only attack x > 216,
//   boss_selin_fireheart.cpp CanAIAttack): whatever brings her (the pair's call for assistance), she finds nobody
//   and evades, and the door opens again;
// - once the room is clear the whole group walks in ("mgt out of room") and nobody pulls or hits her until every
//   alive member stands at x >= SELIN_SAFE_X (EntryHolds);
// - once she is engaged and does not evade (a group player her victim, or INERT_MS with nobody to hit and her room
//   clear; a pet victim is called back, inert with Wretched alive the room is cleared first), every room rule lets go
//   (Snapshot::selinCommitted, [selin engaged]): the Wretched in a member's fight are fought where they stand, then
//   the members outside walk in ("mgt out of room") and everybody fights her and the crystal she channels. She does
//   not always evade with nobody in her room: mgt-h/mgt-n 2026-10-25 11:20 (build 24) both stood 180 s at the rally
//   with her engaged at 100 %, no victim, not evading, the door shut, the side packs out of every pull spot's sight;
//   the former split guard (the ones inside step to the doorway so she evades) ends in the same state.
constexpr float LEASH_EDGE_X = SELIN_LEASH_X - 0.5f;
// The cast spots of the room pulls stay this far out (the leash may not walk the tank back from its own spot).
constexpr float PULL_SPOT_MAX_X = SELIN_LEASH_X - 1.0f;

// A Wretched of her floor is alive, Selin is alive and the room rules hold (her fight not committed).
bool ClearingRoom(PlayerbotAI* botAI);

// Walking to `target` to fight it ends past LEASH_EDGE_X while the room is being cleared.
bool CrossesLeash(PlayerbotAI* botAI, Unit* target);

// Selin alive and idle, her room clear, and an alive member of the group still at x < SELIN_SAFE_X: nobody pulls or
// hits her yet. `outside` gets the first such member (may be nullptr).
bool EntryHolds(Player* bot, Player** outside = nullptr);

// [selin pet] While her room is cleared (ClearingRoom) or her victim is a pet of the group, no pet of the group stands
// in her room nor fights her or a Wretched at her door (TargetValueExclusionType::PetTarget, "mgt selin pet out").
bool KeepsPetsOut(PlayerbotAI* botAI);
void AppendPetExclusions(PlayerbotAI* botAI, GuidSet& exclusions);
// The controllable pet of the bot that stands in her room (x > LEASH_EDGE_X) or is her victim while KeepsPetsOut.
Creature* PetToCallBack(PlayerbotAI* botAI);

struct PullPlan
{
    ObjectGuid target;
    uint8 pack = 0;
    bool linked = false;
    float x = 0.0f;  // the tank's cast spot
    float y = 0.0f;
    float z = 0.0f;
    float margin = 0.0f;  // how far the spot (and the walk to it) stays outside the other idle packs' aggro
};

// The room as this bot sees it (cached per bot).
Snapshot const& Get(PlayerbotAI* botAI);

// The room tactic holds the group at the rally and decides the next pull.
bool HoldsGroup(PlayerbotAI* botAI);

// Where the group gathers for the next room pull: the stage spot for the final (centre pair) pull, else the rally.
void GatherPoint(PlayerbotAI* botAI, float& x, float& y, float& z);

// [selin_final_entry] The final pull is the whole group walking in together: the pulling tank starts it once the group
// stands ready at the stage spot, and for FINAL_ENTRY_MS every member walks into her room ("mgt out of room"), the
// leash letting go. Build 27 (20261025-150737-mgt-h-5A-14): a pull with members outside shut the door on the hunter
// and the shaman for 6 minutes (the bots never walk through a closed door). Per instance, shared by the map threads.
constexpr uint32 FINAL_ENTRY_MS = 30000;
// `entryY`: the y of the spot inside the room the group walks to (just past SELIN_SAFE_X).
void StartFinalEntry(Player* tank, float entryY);
bool FinalEntryActive(Player* bot, float* entryY = nullptr);

// [selin_heroic_entry_pull] Heroic: Selin never evades and a side pack's run to the corridor passes the centre pair
// within the core's call for assistance (13.2 yd of 13.3: build 27 heroic, build 29 normal), so the first side pack
// brings the pair and Selin. While the pair is idle on heroic, every room pull is an entry pull: the group gathers at
// the stage spot, ready in full, and walks into the room with the pull (StartFinalEntry) to the fight spot on the
// pulled pack's side, away from the other side pack; the door shuts with everybody inside.
bool HeroicEntryPull(PlayerbotAI* botAI);
// The fight spot inside the room for an entry pull of a pack on the `packY` side (sign) of the room.
constexpr float ENTRY_FIGHT_X = 221.0f;
constexpr float ENTRY_FIGHT_SIDE_Y = 6.0f;

// The room tactic decides the group's next pull: nothing else may start one (the careful planner's hook in
// AggressiveTargetAction::isUseful, like ANWatcherRoom::OwnsPull). False when no pull spot was found: the planner
// then keeps its pull.
bool OwnsPull(PlayerbotAI* botAI);

// The bot opens the room's pulls: the main tank, or a tank when the group has no main tank alive.
bool IsPullingTank(Player* bot);

// The next pull of the room (cached; computed for the pulling tank only). False when there is none.
bool GetPullPlan(PlayerbotAI* botAI, PullPlan& plan);

// (x, y) stands within an idle room mob's aggro range plus `margin` (the pack `ignorePack` left out, -1 for none).
bool InIdleReach(Snapshot const& snapshot, float x, float y, float margin, int16 ignorePack = -1);

// Walking to `target` to fight it would bring the bot into an idle pack's reach.
bool ApproachUnsafe(PlayerbotAI* botAI, Unit* target);

// The bot really fights: in combat, and not only through Selin's zone-wide combat while she is inert.
bool InRoomFight(PlayerbotAI* botAI);

// The index of the pack `member` belongs to in this snapshot (pack indices are per snapshot), -1 when none.
int16 PackOf(Snapshot const& snapshot, ObjectGuid member);

// Wretched in the bot's fight at or below this health: each drinks Fel Infusion at 20 % (see below).
uint32 CountLowWretched(PlayerbotAI* botAI, float healthPct);
}  // namespace SelinRoom

// Fel Infusion (smart_scripts 24688-24690 event 2: "Between 0-20% Health - Cast Drink Fel Infusion", 23-33 s
// repeat; Spell.dbc 44505: instant, not dispellable, 10 s of +2000 % damage done and +100 % melee haste): every
// Wretched at 20 % turns into a tank killer for 10 s. Wave 29 11:46: area spells brought the whole 6-pack to 1-17 %
// together, six drank at once and the tank took 21k from Skulker melee in 15 s. The group kills them one at a time:
// a drinking one first, then the lowest, never area spells while two of them are low.
Unit* GetWretchedFocus(Player* bot, ObjectGuid& latched);
bool IsDrinkingWretched(Unit* unit);
}

#endif
