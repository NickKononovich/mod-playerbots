/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GDTRIGGERS_H
#define PLAYERBOTS_GDTRIGGERS_H

#include "Creature.h"
#include "DungeonStrategyUtils.h"
#include "GameObject.h"
#include "GenericTriggers.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "PlayerbotAIConfig.h"
#include "Trigger.h"

enum GundrakIDs
{
    // Slad'ran
    SPELL_POISON_NOVA_N             = 55081,
    SPELL_POISON_NOVA_H             = 59842,
    SPELL_SNAKE_WRAP                = 55126,
    NPC_SNAKE_WRAP                  = 29742,
    NPC_SLADRAN_VIPER               = 29680,
    NPC_SLADRAN_CONSTRICTOR         = 29713,

    // Gal'darah
    SPELL_WHIRLING_SLASH_N          = 55250,
    SPELL_WHIRLING_SLASH_H          = 59824,
};

#define SPELL_POISON_NOVA           DUNGEON_MODE(bot, SPELL_POISON_NOVA_N, SPELL_POISON_NOVA_H)
#define SPELL_WHIRLING_SLASH        DUNGEON_MODE(bot, SPELL_WHIRLING_SLASH_N, SPELL_WHIRLING_SLASH_H)

namespace GundrakSladran
{
constexpr float STACK_LEASH_YD = 10.0f;
constexpr float STACK_CLOSE_TO_YD = 5.0f;
constexpr float TANK_PICKUP_YD = 12.0f;
constexpr uint32 SNAKE_WRAP_SCAN_INTERVAL = 200;
// The snakes spawn at the foot of the stairs and the west entrance (boss_slad_ran.cpp SpawnLoc): the tank keeps
// Slad'ran on his platform, so the group stacked on it is not where they come in (wave 10: the tank drifted 30 yd
// down the stairs, the healer stood 8 yd from a Constrictor spawn and died to the snakes).
constexpr float TANK_PARK_LEASH_YD = 8.0f;

bool IsAdd(Unit* unit);
ObjectGuid CalculateAssignedSnakeWrap(PlayerbotAI* botAI);
Unit* GetAssignedSnakeWrap(PlayerbotAI* botAI);
Player* GetStackTank(PlayerbotAI* botAI);
Unit* GetTankHoldTarget(PlayerbotAI* botAI);
// Slad'ran when the bot is the tank holding him away from his platform, or nullptr.
Unit* GetTankParkBoss(PlayerbotAI* botAI);

// The snakes never stop (boss_slad_ran.cpp: two Vipers every 8 s below 90 %, three Constrictors every 3-5 s below
// 50 % normal / 75 % heroic), so the boss is burned and one damage dealer kills the snakes that reach the healer
// (warcraft.wiki.gg: "assign someone to kill the small snakes"). Wave 6 (20261025-000027): every damage dealer
// chased the snakes for 80 s, 82 snakes died, the boss did not, and the group wiped.
// Slad'ran alive and in combat, or nullptr.
Unit* GetEngagedBoss(PlayerbotAI* botAI);
// The bot is the group's snake killer: the first ranged damage dealer by guid (any damage dealer when there is no
// ranged one), skipping members locked in a Snake Wrap.
bool IsSnakeKiller(PlayerbotAI* botAI);
// The live Viper or Constrictor on a healer.
bool IsOnHealer(Unit* unit);
// Every Viper and Constrictor among the bot's attackers.
void AppendSnakeExclusions(PlayerbotAI* botAI, GuidSet& exclusions);
}

namespace GundrakIds
{
// gundrak.h
constexpr uint32 MAP_ID = 604;
constexpr uint32 DATA_SLAD_RAN = 0;
constexpr uint32 DATA_MOORABI = 1;
constexpr uint32 DATA_DRAKKARI_COLOSSUS = 2;
constexpr uint32 DATA_GAL_DARAH = 3;
constexpr uint32 GO_ALTAR_OF_SLAD_RAN = 192518;
constexpr uint32 GO_ALTAR_OF_MOORABI = 192519;
constexpr uint32 GO_ALTAR_OF_DRAKKARI = 192520;
constexpr uint32 GO_GUNDRAK_COLLISION = 192633;
// boss_drakkari_colossus.cpp
constexpr uint32 NPC_DRAKKARI_COLOSSUS = 29307;
constexpr uint32 NPC_LIVING_MOJO = 29830;
}

namespace GundrakEvent
{
constexpr uint32 EVENT_CHECK_INTERVAL = 1000;
constexpr uint32 ALTAR_SCAN_INTERVAL = 2000;
constexpr float ALTAR_SEARCH_YD = 200.0f;
constexpr float BRIDGE_SEARCH_YD = 150.0f;
constexpr float COLOSSUS_SEARCH_YD = 45.0f;
constexpr float MOJO_SEARCH_YD = 30.0f;
constexpr float GATHER_YD = 30.0f;
constexpr float START_MIN_HEALTH_PCT = 90.0f;
constexpr float START_MIN_MANA_PCT = 70.0f;

bool IsReadyToStart(Player* bot, WorldObject const* anchor, float radius);
ObjectGuid FindAltarToUse(PlayerbotAI* botAI);
GameObject* GetAltarToUse(PlayerbotAI* botAI);
Creature* GetDormantColossus(PlayerbotAI* botAI);
Creature* FindLivingMojo(Player* bot, Creature* colossus);
bool IsOnBridge(Player* bot);
bool IsBridgeOpen(Player* bot);
bool ShouldCrossBridge(PlayerbotAI* botAI);
Position const* GetBridgeStart();
Position const* GetNextBridgeWaypoint(Player* bot);

// [b34_gd] The moat under the bridge (water surface z 110.1). The deck has no navmesh, so a mob that chases a member
// onto it gets a path that starts in the water below (PathGenerator's 50 yd tall nearest-poly box) and drops in.
// gd-h 20261026-014521-gd-h-5A-40: a Drakkari Raider, a Rhino and a Frenzy fell in at 1566 s and stood 11 yd under
// the east landing (1801,746.5,108.6). The moat is joined to the west landing and the boss platforms (6061 polys),
// not to the east landing (861 polys). The mobs stayed in combat with every member (the tank's Righteous Defense and
// the heals kept their threat lists alive, so the core's no-path evade never came). Out of sight and 11 yd lower
// they were no "attackers" (AttackersValue: no LoS, dz > 6), so nobody had a target and the run failed "trash fight
// idle" after 180 s. A player jumps into the water beside the deck and kills them there.
constexpr float MOAT_SURFACE_Z = 110.1f;
constexpr uint32 MOAT_DROP_HOLD_MS = 5 * IN_MILLISECONDS;
bool IsInMoat(WorldObject const* object);
// A live mob in combat with the bot, in the moat under the bridge, while the bot stands on the deck or the east
// landing with no attacker it can fight: nullptr otherwise.
Creature* FindMoatAttacker(PlayerbotAI* botAI);
// The bot's drop off the deck into the moat is under way (GundrakMoatDropAction); the bridge walk leaves it alone.
void NoteMoatDrop(Player* bot, uint32 durationMs);
bool IsDroppingIntoMoat(Player* bot);
// Out of combat in the moat with Gal'darah ahead: back to the bridge's west start (a navmesh walk through the water
// and up the ramp at 1735,759), where the crossing begins again.
bool ShouldLeaveMoat(PlayerbotAI* botAI);
}

// The central circle's pool (surface z 123.5): its bottom is a navmesh part of its own, 153 polygons joined to no
// other, the surface layer 1.9 yd above its rim (tools/navmesh_probe.py 604 1710.9 670.0 121.4 1775.1 675.0 129.3).
// A fight or a follow that ends down there leaves no path anywhere (2026-10-25 gd-n / gd-h: boss_unreachable
// Slad'ran / Moorabi, pathType 4, 2-3 points, from 1688-1712, 652-695, z 117-121).
namespace GundrakCircle
{
// vertex bounds of the bottom part: x 1648.5-1715.2, y 644.8-695.5, z 113.6-121.6
constexpr float POOL_MIN_X = 1647.0f;
constexpr float POOL_MAX_X = 1716.0f;
constexpr float POOL_MIN_Y = 644.0f;
constexpr float POOL_MAX_Y = 696.0f;
// above the moat's surface (z 110.1) that reaches into the box's north-east corner, below the pool's (123.5)
constexpr float FLOOR_MIN_Z = 113.0f;
constexpr float FLOOR_MAX_Z = 122.5f;
constexpr uint32 CHECK_INTERVAL = 1000;

bool IsOnPoolFloor(Player* bot);
// Slad'ran, Moorabi, the Drakkari Colossus or Gal'darah still to kill.
bool HasBossAhead(Player* bot);
}

class SladranPoisonNovaTrigger : public Trigger
{
public:
    SladranPoisonNovaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "slad'ran poison nova") {}
    bool IsActive() override;
};

class SladranSnakeWrapTrigger : public Trigger
{
public:
    SladranSnakeWrapTrigger(PlayerbotAI* botAI) : Trigger(botAI, "slad'ran snake wrap") {}
    bool IsActive() override;
};

class SladranStackOnTankTrigger : public Trigger
{
public:
    SladranStackOnTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "slad'ran stack on tank") {}
    bool IsActive() override;
};

class SladranTankHoldTrigger : public Trigger
{
public:
    SladranTankHoldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "slad'ran tank hold") {}
    bool IsActive() override;
};

class SladranTankParkTrigger : public Trigger
{
public:
    SladranTankParkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "slad'ran tank park") {}
    bool IsActive() override;
};

class GaldarahWhirlingSlashTrigger : public Trigger
{
public:
    GaldarahWhirlingSlashTrigger(PlayerbotAI* botAI) : Trigger(botAI, "gal'darah whirling slash") {}
    bool IsActive() override;
};

class GundrakAltarReadyTrigger : public Trigger
{
public:
    GundrakAltarReadyTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "gundrak altar ready", GundrakEvent::EVENT_CHECK_INTERVAL)
    {
    }
    bool IsActive() override;
};

class DrakkariColossusDormantTrigger : public Trigger
{
public:
    DrakkariColossusDormantTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "drakkari colossus dormant", GundrakEvent::EVENT_CHECK_INTERVAL)
    {
    }
    bool IsActive() override;
};

class GundrakBridgeFormedTrigger : public Trigger
{
public:
    GundrakBridgeFormedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "gundrak bridge formed") {}
    bool IsActive() override;
};

// [b34_gd] a mob fighting the group from the moat under the bridge, for MOAT_DROP_HOLD_MS (or the drop under way)
class GundrakMoatFightTrigger : public Trigger
{
public:
    GundrakMoatFightTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "gundrak moat fight", GundrakEvent::EVENT_CHECK_INTERVAL)
    {
    }
    bool IsActive() override;

private:
    uint32 _seenMs = 0;
};

// [b34_gd] out of combat in the moat with the bridge still to cross
class GundrakMoatExitTrigger : public Trigger
{
public:
    GundrakMoatExitTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "gundrak moat exit", GundrakEvent::EVENT_CHECK_INTERVAL)
    {
    }
    bool IsActive() override;
};

class GundrakCircleRouteTrigger : public Trigger
{
public:
    GundrakCircleRouteTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "gundrak circle route", GundrakCircle::CHECK_INTERVAL)
    {
    }
    bool IsActive() override;
};

#endif
