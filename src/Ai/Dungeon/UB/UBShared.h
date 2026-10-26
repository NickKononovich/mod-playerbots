/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_UBSHARED_H
#define PLAYERBOTS_UBSHARED_H

#include "Define.h"
#include "ObjectGuid.h"

class AiObjectContext;
class Creature;
class Player;
class Unit;

namespace UnderbogHungarfen
{
constexpr uint32 NPC_HUNGARFEN = 17770;
constexpr uint32 NPC_UNDERBOG_MUSHROOM = 17990;
constexpr uint32 NPC_UNDERBAT = 17724;

constexpr uint32 SPELL_FOUL_SPORES = 31673;
constexpr uint32 SPELL_GROW        = 31698;
constexpr uint32 SPELL_SPORE_CLOUD = 34168;

constexpr float SPORE_CLOUD_RADIUS_FALLBACK = 8.0f;
constexpr float FOUL_SPORES_RADIUS_FALLBACK = 20.0f;

constexpr float SPORE_CLOUD_MARGIN = 1.5f;
constexpr float FOUL_SPORES_MARGIN = 2.0f;

constexpr uint8 GROW_STACKS_DANGER = 8;

constexpr float MUSHROOM_SCAN_RANGE = 40.0f;

constexpr float UNDERBAT_LASH_RANGE = 5.0f;
constexpr float UNDERBAT_LASH_MARGIN = 1.5f;
constexpr float UNDERBAT_SAFE_ARC = 2.6f;
constexpr uint32 UNDERBAT_REPOSITION_COOLDOWN = 750;

constexpr float UNDERBAT_RALLY_TOLERANCE = 3.0f;
constexpr float UNDERBAT_RALLY_MAX_RANGE = 20.0f;

constexpr uint32 MUSHROOM_SCAN_INTERVAL = 1000;
constexpr uint8 MUSHROOM_IDLE_SCAN_SKIPS = 9;
constexpr uint32 MUSHROOM_LINGER_TIME = 30000;

float MaxEffectRadius(uint32 spellId, float fallback);

Unit* HungarfenTarget(AiObjectContext* context);

ObjectGuid FindHungarfenGuid(Player* bot);

bool HungarfenGone(Player* bot, ObjectGuid guid);

float MushroomDangerRange(Player* bot);

GuidVector FindMushroomGuids(Player* bot);

bool IsMushroom(Unit* unit);

bool AnyMushroomAlive(Player* bot, GuidVector const& mushrooms);

Creature* GetNearestDangerousMushroom(Player* bot, GuidVector const& mushrooms, float range);

bool RetreatPathUnsafe(Player* bot, GuidVector const& mushrooms, float destX, float destY);

bool AnyUnderbatInLashRange(Player* bot, GuidVector const& attackers);

Creature* GetNearestUnderbatInLashRange(Player* bot, GuidVector const& attackers);

Creature* GetLashingUnderbat(Player* bot, GuidVector const& attackers);

Unit* UnderbatRallyUnit(Player* bot, GuidVector const& attackers);

bool SpotClearOfUnderbats(Player* bot, GuidVector const& attackers, float x, float y, float z);
}

// Ghaz'an's lake (boss_ghazan.cpp, waypoint paths 1383920 / 1383921): he patrols the lake (surface z 49.4, bottom
// z 3.5) and climbs to the platform at the top of the ramp when the walkway area trigger 4302 fires. The water is
// full of swim-only Underbog Frenzies (20465, creature_template_movement Ground 0). Wave 8: twice the group fought
// in or under the water, the piranhas killed the tank and healer, then Ghaz'an the rest. A group fights him on land
// only: nobody enters the lake while fighting here and nobody chases a target into it.
namespace UnderbogGhazan
{
constexpr uint32 MAP_UNDERBOG = 546;
constexpr uint32 NPC_GHAZAN = 18105;
constexpr uint32 NPC_UNDERBOG_FRENZY = 20465;

// Box around the lake basin; the walkway (z 72) and the platform (z 81) lie above it.
constexpr float LAKE_X_MIN = 150.0f;
constexpr float LAKE_X_MAX = 310.0f;
constexpr float LAKE_Y_MIN = -530.0f;
constexpr float LAKE_Y_MAX = -360.0f;
constexpr float LAKE_Z_MAX = 60.0f;
// The lower cavern pool between the entrance and Hungarfen: 28 more Frenzies (creature spawns z -18 to -30). Wave 10:
// three hunters and shamans died in it. The same rule holds there: fight on its shore, never in it.
constexpr float POOL_X_MIN = -90.0f;
constexpr float POOL_X_MAX = 215.0f;
constexpr float POOL_Y_MIN = -445.0f;
constexpr float POOL_Y_MAX = -255.0f;
constexpr float POOL_Z_MAX = -12.0f;
// Water shallower than this is wading, not swimming (no Frenzy reaches it).
constexpr float WADE_DEPTH = 1.0f;

// Dry anchors checked at runtime: the platform where Ghaz'an ends his climb (waypoint 1383921 point 20) and the
// walkway at area trigger 4302.
constexpr float PLATFORM_X = 256.279f;
constexpr float PLATFORM_Y = -458.731f;
constexpr float PLATFORM_Z = 81.3728f;
constexpr float WALKWAY_X = 234.98f;
constexpr float WALKWAY_Y = -379.276f;
constexpr float WALKWAY_Z = 72.5221f;

// In Ghaz'an's basin or the lower Frenzy pool.
bool InLakeArea(Unit* unit);

// The unit swims (its cached liquid status, deeper than wading).
bool IsWet(Unit* unit);

// No swimmable water at the spot.
bool IsDrySpot(Player* bot, float x, float y, float z);

// The bot fights in Ghaz'an's basin.
bool LakeFight(Player* bot);
}

#endif
