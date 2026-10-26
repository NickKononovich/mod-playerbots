/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MTSHARED_H
#define PLAYERBOTS_MTSHARED_H

#include "Common.h"

class Unit;

// Auchindoun: Mana-Tombs (map 557): boss_nexusprince_shaffar.cpp, boss_pandemonius.cpp and the Ethereal Beacon
// SmartAI.
namespace ManaTombs
{
inline constexpr uint32 MAP_MANA_TOMBS = 557;

inline constexpr char const* SHAFFAR_NAME = "nexus-prince shaffar";
inline constexpr char const* PANDEMONIUS_NAME = "pandemonius";

enum ManaTombsNpcs : uint32
{
    // Shaffar summons one every 10 s. It Arcane Bolts a random player and, if still alive 10 s (heroic) /
    // 20 s (normal) later, turns into an Ethereal Apprentice (32372) that keeps casting.
    NPC_ETHEREAL_BEACON         = 18431,
    NPC_ETHEREAL_APPRENTICE     = 18430,
};

enum ManaTombsSpells : uint32
{
    // Pandemonius: 6 s, reflects every spell and hits melee attackers (750 / heroic 1500 per hit).
    SPELL_DARK_SHELL            = 32358,
    SPELL_DARK_SHELL_HEROIC     = 38759,
};

inline constexpr float BEACON_SEARCH_RADIUS = 45.0f;

bool HasDarkShell(Unit* unit);
}

#endif
