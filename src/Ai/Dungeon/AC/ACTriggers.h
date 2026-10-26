/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTRIGGERS_H
#define PLAYERBOTS_ACTRIGGERS_H

#include "Common.h"
#include "Trigger.h"

struct Position;

inline constexpr uint32 NPC_FOCUS_FIRE = 18374;
inline constexpr float FLARE_SEARCH_RADIUS = 20.0f;
// Inhibit Magic (boss_shirrak_the_dead_watcher.cpp) slows casting by distance to Shirrak: 4 stacks under 15 yd,
// 3 under 25, 2 under 35, 1 under 45 (GetDistance, i.e. edge to edge). Damage dealers hold their casting range
// (at most 2 stacks) instead of drifting in; healers hold 36 yd (1 stack) while the tank stays in heal range.
// Carnivorous Bite (36383 / heroic 39382) also hits everyone within 12 yd of him.
inline constexpr float SHIRRAK_HEALER_DISTANCE = 36.0f;
inline constexpr float SHIRRAK_DISTANCE_TOLERANCE = 2.0f;
inline Position const SHIRRAK_TANK_POSITION =   { -65.171f, -162.920f, 26.504f };
// [b35_lock] A damage dealer whose spells reach 36 yd or more (Destructive Reach, Arctic Reach / Flame Throwing,
// Shadow Reach, Nature's Reach, Elemental Reach, a hunter's weapon) holds the 1-stack band too: from 35 yd on, at
// 35.5 (destro audit: at SpellDistance 28.5 the Destruction warlocks stood in 2 stacks, casts 1.5-2.6x as long,
// 33.8 / 44.1 % of the sim). While 2+ stacks are up anyway (Attract Magic), its harmful instants go first.
inline constexpr uint32 SPELL_INHIBIT_MAGIC = 32264;
inline constexpr float SHIRRAK_ONE_STACK_DISTANCE = 35.0f;
inline constexpr float SHIRRAK_RANGE_MARGIN = 0.5f;
inline constexpr float SHIRRAK_INSTANT_PREFERENCE = 1.2f;

class PlayerbotAI;

// The edge-to-edge distance a ranged damage dealer holds from Shirrak: SHIRRAK_ONE_STACK_DISTANCE plus the margin
// when its own spells reach that far, else SpellDistance.
float ShirrakRangedDpsDistance(PlayerbotAI* botAI);

class ShirrakTankPositionBossTrigger : public Trigger
{
public:
    ShirrakTankPositionBossTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "shirrak tank position boss") {}
    bool IsActive() override;
};

class ShirrakFleeFocusFireTrigger : public Trigger
{
public:
    ShirrakFleeFocusFireTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "shirrak flee focus fire") {}
    bool IsActive() override;
};

class ShirrakRangedKeepDistanceTrigger : public Trigger
{
public:
    ShirrakRangedKeepDistanceTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "shirrak ranged keep distance") {}
    bool IsActive() override;
};

#endif
