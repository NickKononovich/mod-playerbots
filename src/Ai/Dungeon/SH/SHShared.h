/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SHSHARED_H
#define PLAYERBOTS_SHSHARED_H

#include "Common.h"

class Player;
class Unit;

// Hellfire Citadel: The Shattered Halls (map 540): boss_warchief_kargath_bladefist.cpp.
//
// Warchief Kargath Bladefist (16808). 30 s after the pull, then every 32.85-41.35 s, Blade Dance: he turns passive
// and casts 30738 (radius 30) at a Blade Dance Target dummy (20709) 5-16 yd from him - two times in three one that
// has a player within 15 yd - charges to it (30751) and hits everyone within 15 yd of where he lands (30739,
// Spell.dbc radius index 18 = 15 yd; 5.7k a hit at level 80 in sh-h w24). Nine charges, then he fights again.
// The Warchief Portal adds a Heathen (17621), a Reaver (17623) or a Sharpshooter (17622) Guard every 20.6 s; each
// returns 20 s after it dies. Four Shattered Hand Assassins (17695) come at the engage. He evades 42 yd from his
// spawn (231.25, -83.64).
//
// sh-h w24 (20261025-115805-sh-h-5A-37, pull 46): no tactic, the healer stood 7-10 yd from the tank and the boss and
// died to Blade Dance 31 s in (20.9k in 15 s); the hunter, the shaman and the mage followed, all to Blade Dance.
namespace ShatteredHalls
{
inline constexpr uint32 MAP_SHATTERED_HALLS = 540;

inline constexpr char const* KARGATH_NAME = "warchief kargath bladefist";

enum ShatteredHallsNpcs : uint32
{
    NPC_KARGATH = 16808,
    NPC_BLADE_DANCE_TARGET = 20709,
    NPC_HEATHEN_GUARD = 17621,
    NPC_SHARPSHOOTER_GUARD = 17622,
    NPC_REAVER_GUARD = 17623,
    NPC_SHATTERED_HAND_ASSASSIN = 17695,
};

enum ShatteredHallsSpells : uint32
{
    SPELL_BLADE_DANCE_TARGETING = 30738,
    SPELL_BLADE_DANCE_DMG = 30739,
    SPELL_BLADE_DANCE_CHARGE = 30751,
};

// Blade Dance hits 15 yd around each spot he lands on (30739).
inline constexpr float BLADE_DANCE_RADIUS = 15.0f;
// The farthest dummy he charges to (spell_blade_dance_targeting: 5-16 yd).
inline constexpr float BLADE_DANCE_CHARGE_MAX = 16.0f;
// Out of the charges' reach altogether: no dummy he can pick is within 15 yd of the bot, so the bot neither draws a
// charge nor is hit by the first one. The healer stands here all fight (the tank at Kargath is ~31 yd off, in heal
// range).
inline constexpr float KARGATH_HEALER_RING = BLADE_DANCE_RADIUS + BLADE_DANCE_CHARGE_MAX;
// Ranged damage dealers: as far as their 30 yd spells reach (a dummy on their side may still draw a charge; the
// dance rule below moves them out of the landing spot then).
inline constexpr float KARGATH_RANGED_RING = 27.0f;
// The band inside the ring a ranged bot may stand before it steps back out.
inline constexpr float KARGATH_RING_SLACK = 3.0f;

// Kargath is dancing: passive between charges, or charging (spell_blade_dance_targeting, MovementInform).
bool IsBladeDancing(Unit* kargath);

// Where a Blade Dance hit reaches the bot from where Kargath stands now, with both bodies.
float BladeDanceDangerRadius(Player* bot, Unit* kargath);
}  // namespace ShatteredHalls

#endif
