/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ARCSHARED_H
#define PLAYERBOTS_ARCSHARED_H

#include "Common.h"
#include "Player.h"

class PlayerbotAI;

// Tempest Keep: The Arcatraz (map 552): boss_dalliah_the_doomsayer.cpp.
namespace Arcatraz
{
inline constexpr uint32 MAP_ARCATRAZ = 552;

inline constexpr char const* DALLIAH_NAME = "dalliah the doomsayer";

enum ArcatrazSpells : uint32
{
    // 6 s aura; every tick hits everyone within 8 yd (36175, weapon damage + 1750). Followed by a heal.
    SPELL_DALLIAH_WHIRLWIND     = 36142,
};

// arcatraz.cpp npc_warden_mellichar: hitting the warden starts the prison event (boss state 3). The four released
// prisoners must die in the room: one that evades sets FAIL, and so does the warden when no player is within 100 yd.
inline constexpr uint32 BOSS_WARDEN_MELLICHAR = 3;
inline constexpr float WARDEN_ROOM_X = 445.8f;
inline constexpr float WARDEN_ROOM_Y = -169.0f;
inline constexpr float WARDEN_ROOM_Z = 43.6f;
inline constexpr float WARDEN_ROOM_LEASH = 32.0f;  // the cells sit 25-35 yd out from the warden
inline constexpr float WARDEN_ROOM_RETURN = 15.0f;

enum ArcatrazNpcs : uint32
{
    NPC_BLAZING_TRICKSTER       = 20905,
    NPC_PHASE_HUNTER            = 20906,
    NPC_AKKIRIS                 = 20908,
    NPC_SULFURON                = 20909,
    NPC_TWILIGHT_DRAKONAAR      = 20910,
    NPC_BLACKWING_DRAKONAAR     = 20911,
    NPC_GARGANTUAN_ABYSSAL      = 20898,
};

// Gargantuan Abyssal trash: 3 s cast on a random player, 10 yd, damage split between everyone hit
// (spell_custom_attr SHARE_DAMAGE): the whole group stacks on the landing point.
inline constexpr uint32 SPELL_ABYSSAL_METEOR = 36837;
inline constexpr float METEOR_STACK_DISTANCE = 3.0f;
inline constexpr float METEOR_JOIN_RANGE = 35.0f;

// [b34_arc] Defender Corpse / Warder Corpse: rooted traps (smart_scripts 21303/21304 -> 2130400): on aggro, or on a
// player within 8 yd in its line of sight out of combat, Bloody Explosion + Corpse Burst (Protean Spawns), despawn
// 5 s later. They never walk to the tank, so a ranged "pull back" brings nothing; worse, a shot that makes no threat
// ref leaves the corpse in combat but never engaged (no burst, no despawn) - arc-h 20261026-014521-arc-h-5F-31 held
// the group 20 min on one. Walked to, out of combat, the corpse bursts on its own rule.
inline constexpr uint32 NPC_DEFENDER_CORPSE = 21303;
inline constexpr uint32 NPC_WARDER_CORPSE = 21304;

inline bool IsCorpseTrap(Unit const* unit)
{
    if (!unit || unit->GetMapId() != MAP_ARCATRAZ || !unit->IsCreature())
        return false;
    uint32 const entry = unit->GetEntry();
    return entry == NPC_DEFENDER_CORPSE || entry == NPC_WARDER_CORPSE;
}

bool IsWardenEventActive(Player* bot);
Unit* FindReleasedPrisoner(PlayerbotAI* botAI);
// The meteor's landing point while a Gargantuan Abyssal in combat with the bot is casting it.
bool FindMeteorPoint(PlayerbotAI* botAI, Position& point);

inline float WhirlwindDangerRadius(Player* bot, Unit* dalliah)
{
    constexpr float radius = 8.0f;
    return radius + bot->GetCombatReach() + dalliah->GetCombatReach();
}
}

#endif
