/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BFSHARED_H
#define PLAYERBOTS_BFSHARED_H

#include "Common.h"
#include "DungeonStrategyUtils.h"
#include "Player.h"

class PlayerbotAI;
class Unit;

// Hellfire Citadel: The Blood Furnace (map 542): boss_kelidan_the_breaker.cpp, boss_broggok.cpp,
// instance_blood_furnace.cpp and the Shadowmoon Technician SmartAI.
namespace BloodFurnace
{
inline constexpr uint32 MAP_BLOOD_FURNACE = 542;

inline constexpr char const* KELIDAN_NAME = "keli'dan the breaker";

// Instance boss index of Broggok (blood_furnace.h DATA_BROGGOK).
inline constexpr uint32 BOSS_DATA_BROGGOK = 1;

enum BloodFurnaceSpells : uint32
{
    // Keli'dan puts it on himself (5 s, stuns him), on heroic also casts Vortex 37370 (pulls everyone in),
    // and 5 s later Fire Nova 33132 (heroic 37371).
    SPELL_BURNING_NOVA          = 30940,
};

enum BloodFurnaceNpcs : uint32
{
    // Broggok's four prison cells hold Nascent Fel Orcs; a cell opens when the previous one is cleared.
    NPC_NASCENT_FEL_ORC         = 17398,
};

// Shadowmoon Technician's Proximity Bombs (gameobjects 181877 / 182607) are a DungeonHazard.cpp entry.

// Fire Nova radius (33132: 15 yd, heroic 37371: 20 yd) plus both bodies.
inline float BurningNovaDangerRadius(Player* bot, Unit* kelidan)
{
    constexpr float normalRadius = 15.0f;
    constexpr float heroicRadius = 20.0f;
    return DUNGEON_MODE(bot, normalRadius, heroicRadius) + bot->GetCombatReach() + kelidan->GetCombatReach();
}

// A ranged bot or healer this close to a cell orc the tank has not picked up yet backs off.
inline constexpr float BROGGOK_LOOSE_ADD_RADIUS = 12.0f;

// During Broggok's prisoner waves: the nearest cell orc within BROGGOK_LOOSE_ADD_RADIUS whose victim is not a
// tank. Reads the cached attackers list only.
Unit* FindLooseCellOrc(PlayerbotAI* botAI);
}

#endif
