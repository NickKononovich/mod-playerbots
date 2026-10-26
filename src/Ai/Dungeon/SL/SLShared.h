/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SLSHARED_H
#define PLAYERBOTS_SLSHARED_H

#include "Common.h"

class Creature;
class Player;
class Unit;

// Auchindoun: Shadow Labyrinth (map 555): boss_murmur.cpp, boss_ambassador_hellmaw.cpp,
// boss_blackheart_the_inciter.cpp.
namespace ShadowLabyrinth
{
inline constexpr uint32 MAP_SHADOW_LABYRINTH = 555;

inline constexpr char const* MURMUR_NAME = "murmur";
inline constexpr char const* HELLMAW_NAME = "ambassador hellmaw";

// boss_blackheart_the_inciter.cpp
inline constexpr uint32 NPC_BLACKHEART = 18667;
inline constexpr float BLACKHEART_SPREAD = 8.0f;  // ranged and healers keep this far from every other member
inline constexpr uint32 BLACKHEART_PET_RELEASE_MS = 10000;
// Healers keep this far from every other member: a charmed member (Incite Chaos) runs at whoever stands nearest.
inline constexpr float BLACKHEART_HEALER_SPREAD = 12.0f;
// JustEngagedWith: me->CallForHelp(100.0f) - every idle friendly mob within 100 yd that sees the group joins
// (CallOfHelpCreatureInRangeDo). Wave 6 (normal, 00:16:08): 18 mobs of the Hellmaw hall and his room came, wipe.
inline constexpr float BLACKHEART_CALL_RADIUS = 100.0f;
inline constexpr uint32 BLACKHEART_ROOM_RECHECK_MS = 1000;
// The hold is released when the count of mobs that would come has not dropped for this long (they cannot be reached
// or pulled): a pull with them beats a run that stands forever.
inline constexpr uint32 BLACKHEART_ROOM_STALL_MS = 240000;

enum ShadowLabyrinthSpells : uint32
{
    // 5 s cast; then 38795 / heroic 33666 hits everyone within 34 yd of Murmur for 80% of current health
    // (+ a 9 s dot and -90% speed on heroic).
    SPELL_SONIC_BOOM_CAST           = 33923,
    SPELL_SONIC_BOOM_CAST_HEROIC    = 38796,
    // 14 s debuff on a random player; on expiry Shockwave (33686) silences and hits everyone within 20 yd.
    SPELL_MURMURS_TOUCH             = 33711,
};

// Sonic Boom reach, measured from Murmur's center, plus the bot's body.
inline constexpr float SONIC_BOOM_RADIUS = 34.0f;
// Heroic Thundering Storm (39365) hits everyone farther than 25 yd from Murmur's center.
inline constexpr float THUNDERING_STORM_MIN_RANGE = 25.0f;
// Where ranged and healers stand on heroic: inside the storm's safe circle with some slack.
inline constexpr float MURMUR_RANGED_RING = 21.0f;
// Shockwave radius around the touched player, plus slack.
inline constexpr float SHOCKWAVE_SPREAD = 22.0f;
// Leave the group once the touch has this little time left.
inline constexpr int32 TOUCH_SPREAD_MS = 6000;

bool IsCastingSonicBoom(Unit* murmur);
// How far this ranged member or healer keeps from every other member during Blackheart.
float BlackheartSpread(Player* bot);
// Blackheart idle with mobs his engage call would bring: his pull waits until they are cleared (EncounterOrder).
bool IsBlackheartRoomLocked(Creature* boss);
float SonicBoomDangerRadius(Player* bot);
}

#endif
