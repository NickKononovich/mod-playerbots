/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SVSHARED_H
#define PLAYERBOTS_SVSHARED_H

#include "Common.h"

// Coilfang Reservoir: The Steamvault (map 545): boss_warlord_kalithresh.cpp.
namespace Steamvault
{
inline constexpr uint32 MAP_STEAMVAULT = 545;

inline constexpr char const* KALITHRESH_NAME = "warlord kalithresh";

enum SteamvaultNpcs : uint32
{
    // Every 45 s Kalithresh walks to the nearest one; it turns attackable and channels Warlord's Rage
    // (31543 -> 37081: +damage for 12 s, then a proc) into him until it dies.
    NPC_NAGA_DISTILLER          = 17954,
    // SmartAI 21694 (heroic 21914): Poison Bolt 37272 on a random enemy every 5-7 s, Trample 40340 (2849 physical to
    // every enemy within 50 yd) every 12-15 s, Enrage 8599 at 20 %; Disease Cloud 37266 / 37863 around it
    // (creature_template_addon, DungeonHazard.cpp). Heroic: x14.7 health, x18.3 damage, 1.4 s swing: it must stay on
    // the tank (2026-10-23/24 sv-h: it killed tank, healer and dps on the trash before Thespia).
    NPC_BOG_OVERLORD            = 21694,
};

enum SteamvaultSpells : uint32
{
    // 8 s, reflects every spell back at the caster.
    SPELL_KALITHRESH_REFLECTION = 31534,
};

inline constexpr float DISTILLER_SEARCH_RADIUS = 60.0f;
}

#endif
