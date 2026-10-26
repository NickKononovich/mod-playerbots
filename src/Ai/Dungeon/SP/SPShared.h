/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPSHARED_H
#define PLAYERBOTS_SPSHARED_H

#include "Common.h"
#include "DungeonStrategyUtils.h"
#include "Player.h"

// Coilfang Reservoir: The Slave Pens (map 547), boss_mennu_the_betrayer.cpp + Corrupted Nova Totem SmartAI.
namespace SlavePens
{
inline constexpr uint32 MAP_SLAVE_PENS = 547;

inline constexpr char const* MENNU_NAME = "mennu the betrayer";

enum SlavePensNpcs : uint32
{
    // Summoned by Mennu (31991) at his feet every 26 s. Casts Fire Nova 33132 (heroic 37371, 20 yd, 9k base)
    // 5 s (normal) / 15 s (heroic) after it appears AND when it dies, then despawns.
    NPC_CORRUPTED_NOVA_TOTEM    = 18179,
    // Roots everyone around it (31981): a rooted bot cannot walk out of the nova.
    NPC_TAINTED_EARTHGRAB_TOTEM = 18176,
    // 34980 at 60% health: heals Mennu.
    NPC_MENNU_HEALING_WARD      = 20208,
};

// Fire Nova radius (33132: 15 yd, heroic 37371: 20 yd) plus the bot's own reach.
inline float NovaDangerRadius(Player* bot)
{
    constexpr float normalRadius = 15.0f;
    constexpr float heroicRadius = 20.0f;
    return DUNGEON_MODE(bot, normalRadius, heroicRadius) + bot->GetCombatReach();
}

// Grid search radius for the totems; the room is small.
inline constexpr float TOTEM_SEARCH_RADIUS = 40.0f;
}

#endif
