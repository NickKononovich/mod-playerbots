/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_OCTRIGGERS_H
#define PLAYERBOTS_OCTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Trigger.h"

#include <list>  // [b36_occ]
#include <string>

enum OculusIDs
{
    // Drakos the Interrogator
    NPC_UNSTABLE_SPHERE             = 28166,
    SPELL_UNSTABLE_SPHERE_PASSIVE   = 50756,
    SPELL_UNSTABLE_SPHERE_PULSE     = 50757,
    SPELL_UNSTABLE_SPHERE_TIMER     = 50758,

    // Drakes
    NPC_AMBER_DRAKE                 = 27755,
    NPC_EMERALD_DRAKE               = 27692,
    NPC_RUBY_DRAKE                  = 27756,
    ITEM_AMBER_ESSENCE              = 37859,
    ITEM_EMERALD_ESSENCE            = 37815,
    ITEM_RUBY_ESSENCE               = 37860,
    SPELL_AMBER_ESSENCE             = 49461,
    SPELL_EMERALD_ESSENCE           = 49345,
    SPELL_RUBY_ESSENCE              = 49462,
    // Abilities:
    // Amber
    SPELL_SHOCK_LANCE               = 49840,
    SPELL_SHOCK_CHARGE              = 49836,
    SPELL_STOP_TIME                 = 49838,
    SPELL_TEMPORAL_RIFT             = 49592,
    // Emerald
    SPELL_LEECHING_POISON           = 50328,
    SPELL_TOUCH_THE_NIGHTMARE       = 50341,
    SPELL_DREAM_FUNNEL              = 50344,
    // Ruby
    SPELL_SEARING_WRATH             = 50232,
    SPELL_EVASIVE_MANEUVERS         = 50240,
    SPELL_EVASIVE_CHARGES           = 50241,
    SPELL_MARTYR                    = 50253,

    // Varos Cloudstrider
    NPC_CENTRIFUGE_CORE             = 28183,

    // Mage-Lord Urom
    NPC_MAGE_LORD_UROM              = 27655,
    SPELL_TIME_BOMB_N               = 51121,
    SPELL_TIME_BOMB_H               = 59376,
    SPELL_EMPOWERED_ARCANE_EXPLOSION_N = 51110,
    SPELL_EMPOWERED_ARCANE_EXPLOSION_H = 59377,

    // Ley-Guardian Eregos
    SPELL_ENRAGED_ASSAULT           = 51170,
    SPELL_PLANAR_SHIFT              = 51162,
    NPC_GREATER_LEY_WHELP           = 28276,  // [b38_occ] boss_eregos.cpp NPC_LEY_GUARDIAN_WHELP (flies)
};

#define SPELL_EMPOWERED_ARCANE_EXPLOSION        DUNGEON_MODE(bot, SPELL_EMPOWERED_ARCANE_EXPLOSION_N, SPELL_EMPOWERED_ARCANE_EXPLOSION_H)
#define SPELL_TIME_BOMB                         DUNGEON_MODE(bot, SPELL_TIME_BOMB_N, SPELL_TIME_BOMB_H)

const std::vector<uint32> DRAKE_ITEMS = {ITEM_AMBER_ESSENCE, ITEM_EMERALD_ESSENCE, ITEM_RUBY_ESSENCE};
const std::vector<uint32> DRAKE_SPELLS = {SPELL_AMBER_ESSENCE, SPELL_EMERALD_ESSENCE, SPELL_RUBY_ESSENCE};
const uint32 OCULUS_MAP_ID = 578;

// Instance data ids and spawn points ported from oculus.h / the world DB (instance_oculus keeps its own
// encounter array, read through GetData()).
enum OculusInstanceData
{
    OCULUS_DATA_DRAKOS      = 0,
    OCULUS_DATA_VAROS       = 1,
    OCULUS_DATA_UROM        = 2,
    OCULUS_DATA_EREGOS      = 3,
    OCULUS_DATA_CC_COUNT    = 5,
};

enum OculusMiscIds
{
    NPC_CENTRIFUGE_CONSTRUCT        = 27641,
    OCULUS_CENTRIFUGE_CONSTRUCTS    = 10,

    // Drake givers (npc_oculus_drakegiver): Verdisa -> Emerald, Belgaristrasz -> Ruby, Eternos -> Amber Essence
    NPC_VERDISA                     = 27657,
    NPC_BELGARISTRASZ               = 27658,
    NPC_ETERNOS                     = 27659,
};

const Position OCULUS_VAROS_POSITION = Position(1285.6f, 1070.36f, 439.515f);
const Position OCULUS_CONSTRUCT_RING_CENTER = Position(1056.62f, 1047.61f, 436.89f);
const Position OCULUS_UROM_FIRST_PLATFORM = Position(1177.47f, 937.722f, 527.405f);
const Position OCULUS_EREGOS_POSITION = Position(1077.04f, 1086.21f, 655.497f);

// Which drake leg the group is on, from the instance's encounter states (no human needed to decide it).
enum class OculusDrakeGoal : uint8
{
    NONE,
    VAROS,   // mounted: kill the Centrifuge Constructs, then Varos
    UROM,    // fly to Urom's first platform and fight on foot
    EREGOS,  // mounted again after Urom
};

OculusDrakeGoal GetOculusDrakeGoal(Player* bot);
bool OculusShouldMountDrake(PlayerbotAI* botAI);

namespace OCIds
{
constexpr uint32 GO_NEXUS_PORTAL = 189985;  // entrance -> Drakos' island (spell 49305)
// The portal's only spawn (gameobject guid 1046) and how close "check mount state" leaves a bot on foot near it.
constexpr float NEXUS_PORTAL_X = 1045.57f;
constexpr float NEXUS_PORTAL_Y = 1104.24f;
constexpr float NEXUS_PORTAL_FOOT_RANGE = 30.0f;
constexpr uint32 NPC_EREGOS = 27656;
// boss_varos.cpp: Energize Cores (50785) hits a 54 deg front cone (TARGET_UNIT_CONE_ENEMY_54); ZapAngle starts at
// 6.20 rad on every Reset and turns by pi/2 per cast, so the four zap directions are fixed world angles.
constexpr float VAROS_FIRST_ZAP_ANGLE = 6.20f;
constexpr float ENERGIZE_CORES_HALF_CONE = 0.47f + 0.09f;  // 27 deg + 5 deg margin
// [b36_occ] Heroic Eregos, Planar Shift at 60 % and 20 % (boss_eregos.cpp): three Planar Anomalies chase a random
// member's drake (MoveChase 0.01 yd), pulse Planar Distortion (59380, 9 yd) every second and cast Planar Blast (57976,
// 14249, 20 yd radius) 15.5 s after spawning (smart_scripts 30879), then despawn. A drake outflies them (~24 yd/s vs
// 10). occ-h 20261026-034959-occ-h-5A-18: the drakes hovered motionless, both blasts killed a drake next to the
// chased Ruby Drake, the riders fell.
constexpr uint32 NPC_PLANAR_ANOMALY = 30879;
constexpr float PLANAR_ANOMALY_SCAN_RANGE = 100.0f;
constexpr float PLANAR_ANOMALY_SAFE_DISTANCE = 30.0f;  // Planar Blast 20 yd + margin
}  // namespace OCIds

// Bot-led drake route (no person driving a member): the pilot (DungeonInteract::GetInteractor) picks the next
// island from the instance state; every bot lands on that island and fights on foot like a player would.
namespace OCRoute
{
struct LegTarget
{
    Position pos;  // where the group sets down (Varos legs: a spot out of every pack's reach)
    // Varos legs: where the leg's Construct pack stands; the group fighting it there is still on this leg.
    Position pack;
    bool hasPack = false;
    int8 packIndex = -1;  // [b34_occ] Varos legs: the Construct pack (VAROS_CONSTRUCT_PACKS index), -1 otherwise
    bool land = false;
    bool valid = false;
};

// The pilot waits this high above its landing spot until the group's drakes have caught up.
constexpr float LANDING_HOVER_HEIGHT = 15.0f;

Player* GetPilot(PlayerbotAI* botAI);
bool IsOnEntranceIsland(WorldObject const* object);
LegTarget GetLegTarget(Player* pilot);
bool IsAtLegTarget(WorldObject const* object, LegTarget const& leg);
// Over the leg's island in 2D, at any height a drake flies (the pilot hovering above it).
bool IsNearLegTarget2d(WorldObject const* object, LegTarget const& leg);
Position GetLandingSpot(PlayerbotAI* botAI, LegTarget const& leg);
bool IsGroupReadyToFly(Player* pilot);
// Every alive member's drake (or the member, still on foot) within the gather range of the pilot's landing spot; the
// pilot sets down only then (occ-h 2026-10-25: the tank landed and pulled with the healer 100-190 yd away). After
// a long wait the pilot lands anyway (logged). `missing` names the first member still away.
bool IsGroupGatheredForLanding(Player* pilot, LegTarget const& leg, std::string* missing = nullptr);
// Every alive member has stepped off its drake next to the pilot's landing spot (the landed ones wait for that).
bool IsGroupLandedAtLeg(PlayerbotAI* botAI, LegTarget const& leg, std::string* missing = nullptr);
// An alive member of the instance still stands on the entrance island (after a wipe: not through the portal yet).
bool IsMemberOnEntranceIsland(Player* bot, std::string* who = nullptr);
// A member of the group released his spirit and is on his way back (a ghost, on any map).
bool IsMemberRunningBack(Player* bot, std::string* who = nullptr);
// An alive member on foot below every island of the instance (the lowest floor is z 359.5 on Drakos' island): he fell
// or blinked off an edge and has no walk back (occ-h 2026-10-25 150737: the mage at z 157-207 from 439 s on,
// "member_stranded"). Nobody waits for him: not the landing, not the tank's pull.
bool IsStrandedBelowIslands(Player const* member);
// [b35_occ] The bot's current spline ends at the Nexus Portal.
bool IsWalkingToNexusPortal(Player* bot);
// Rate limit of the strategy's own log lines, per bot and key.
bool ShouldLog(Player* bot, char const* key, uint32 intervalMs);
Creature* GetBoss(Player* bot, uint32 dataId);
Creature* GetPullableBoss(Player* bot);
Creature* GetPullableEregos(Player* bot, Unit const* from);
// [b38_occ] The nearest alive Greater Ley-Whelp within `range` of `from` fighting a member of the bot's group or a
// member's drake (nullptr if none).
Creature* GetWhelpOnGroup(Player* bot, Unit const* from, float range);
// [b38_occ] A drake below LOW_DRAKE_HEALTH_PCT in the Eregos fight: the spot it flies to, low over the nearest floor
// (a dying drake drops its rider where it is). False when the drake is healthy or no floor is found.
bool GetLowDrakeRefuge(Player* bot, Unit* drake, Position& refuge);
bool IsInEnergizeCoresCone(Creature const* varos, WorldObject const* object);
// [b36_occ] Alive Planar Anomalies within `range` of `from` (empty outside the Oculus).
std::list<Creature*> GetPlanarAnomalies(WorldObject const* from, float range);
float GetEnergizeCoresSafeAngle(Creature const* varos, WorldObject const* object);
}  // namespace OCRoute

// const float uromCoords[4][4] =
// {
//     {1177.47f, 937.722f, 527.405f, 2.21657f},
//     {968.66f, 1042.53f, 527.32f, 0.077f},
//     {1164.02f, 1170.85f, 527.321f, 3.66f},
//     {1118.31f, 1080.377f, 508.361f, 4.25f}      // Inner ring, actual boss fight
// };

class DrakeMountTrigger : public Trigger
{
public:
    DrakeMountTrigger(PlayerbotAI* botAI) : Trigger(botAI, "drake mount") {}
    bool IsActive() override;
};

class DrakeDismountTrigger : public Trigger
{
public:
    DrakeDismountTrigger(PlayerbotAI* botAI) : Trigger(botAI, "drake dismount") {}
    bool IsActive() override;
};

class GroupFlyingTrigger : public Trigger
{
public:
    GroupFlyingTrigger(PlayerbotAI* botAI) : Trigger(botAI, "drake fly") {}
    bool IsActive() override;
};

class DrakeCombatTrigger : public Trigger
{
public:
    DrakeCombatTrigger(PlayerbotAI* botAI) : Trigger(botAI, "drake combat") {}
    bool IsActive() override;
};

// [b36_occ] On a drake with a Planar Anomaly alive near it (Eregos' Planar Shift): fly clear of it.
class OcPlanarAnomalyTrigger : public Trigger
{
public:
    OcPlanarAnomalyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "oc planar anomaly") {}
    bool IsActive() override;
};

class VarosCloudstriderTrigger : public Trigger
{
public:
    VarosCloudstriderTrigger(PlayerbotAI* botAI) : Trigger(botAI, "varos cloudstrider") {}
    bool IsActive() override;
};

class UromArcaneExplosionTrigger : public Trigger
{
public:
    UromArcaneExplosionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "urom arcane explosion") {}
    bool IsActive() override;
};

class UromTimeBombTrigger : public Trigger
{
public:
    UromTimeBombTrigger(PlayerbotAI* botAI) : Trigger(botAI, "urom time bomb") {}
    bool IsActive() override;
};

class OcNexusPortalTrigger : public Trigger
{
public:
    OcNexusPortalTrigger(PlayerbotAI* botAI) : Trigger(botAI, "oc nexus portal") {}
    bool IsActive() override;
};

class OcPullBossTrigger : public Trigger
{
public:
    OcPullBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "oc pull boss") {}
    bool IsActive() override;
};

// A bot that stepped off its drake on the leg's island while the rest of the group is still flying in: back to its
// landing spot (it waits there, OccLandingHoldMultiplier).
class OcLandingHoldTrigger : public Trigger
{
public:
    OcLandingHoldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "oc landing hold") {}
    bool IsActive() override;
};

// Shared by OcLandingHoldTrigger and OccLandingHoldMultiplier: on foot on the leg's island, out of combat, with the
// group not landed yet. `spot` is the bot's own landing spot on the floor.
bool OculusShouldHoldLanding(PlayerbotAI* botAI, Position* spot = nullptr);

#endif
