/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOSSKITEROOMS_H
#define PLAYERBOTS_BOSSKITEROOMS_H

#include "Player.h"
#include "Position.h"
#include "Unit.h"
#include <algorithm>
#include <array>

// [boss_puddles] Per-boss kite rooms: bosses whose ground effects persist (5 s or more) where they fight, so the
// tank has to walk the boss out of them and keep the fight inside the boss's own room (no doorway, no idle pack).
// One row per boss: the centre and the radius of the largest open-floor circle of the boss's room.
//
// How the rows were made (C:\WoW\BOSS_PUDDLES.md, section "Kite rooms"): the server's own mmaps (data/mmaps,
// Detour tiles) rasterised at 1 yd around the boss's spawn (or the scripted fight spot where it differs, see the
// row comment), floor within 6 yd of the spawn height, flood-filled from the spawn (a step between cells of at
// most 1.6 yd), then the circle centre farthest from every non-floor cell (wall, pit, ledge, pillar) within 25 yd
// of the spawn and seen from it along open floor. Doors are game objects and not in the navmesh, so a doorway is
// open floor there, but the circle never crosses one: a doorway is narrower than the circle.
//
// Header-only on purpose: the module's sources are collected at CMake configure time, a new .cpp would need a
// re-configure. DungeonHazard.cpp registers Find with MovementAction::KiteRoomLookup() at static initialisation.
namespace BossKiteRooms
{
struct Room
{
    uint32 mapId;
    uint32 bossEntry;  // normal-mode entry (Creature::GetEntry() is the normal entry on every difficulty)
    float x;
    float y;
    float z;
    float floorRadius;  // centre to the nearest wall / edge of the open floor
};

// Kept between the kite ring and the walls: the tank's own size plus the boss's, so it never rubs a wall.
constexpr float WALL_MARGIN = 3.0f;
constexpr float MIN_RADIUS = 5.0f;

constexpr std::array<Room, 41> ROOMS = {{
    // ---- dungeons
    // Broggok (Blood Furnace): Poison Cloud 30916 at his feet every 20 s (60 s, grows to 8 yd). He comes out of
    // his own cell (spawn 455,-2) into the cell-block hall; the room is that round hall, not his cell.
    {542, 17380, 456.5f, 100.0f, 9.9f, 31.0f},
    // Captain Skarloc (Old Hillsbrad): Consecration 38385 at his feet (10 yd, 20 s); seed = end of his ride
    // (boss_captain_skarloc.cpp startPath[3]); open street in front of Durnholde.
    {560, 17862, 2034.8f, 239.0f, 61.8f, 22.5f},
    // Maiden of Grief (Halls of Stone): Storm of Grief 50752 / 59772 on her victim (9 yd, 20 s).
    {599, 27975, 844.6f, 666.0f, 190.3f, 28.0f},
    // Devourer of Souls (Forge of Souls): Well of Souls 36536 (4 yd, until the fight ends).
    {632, 36502, 5654.8f, 2498.4f, 709.0f, 30.5f},
    // Hadronox (Azjol-Nerub): Acid Cloud 53400 / 59419 (5 yd, 90 s) on her web platform.
    {601, 28921, 536.5f, 556.9f, 675.6f, 13.5f},
    // The Prophet Tharon'ja (Drak'Tharon Keep): Poison Cloud 49548 / 59969 (8 yd, 10 s).
    {600, 26632, -236.8f, -676.4f, 132.0f, 24.5f},
    // Hydromancer Thespia (Steamvault): Lightning Cloud 25033 (10 yd, 14 s).
    {545, 17797, 78.4f, -316.1f, -7.8f, 21.0f},
    // Lord Incendius (Blackrock Depths): Fire Storm 13899 (5 yd, 30 s).
    {230, 9017, 894.5f, -273.1f, -71.8f, 10.5f},
    // Jedoga Shadowseeker (Ahn'kahet): Thundershock 56926 / 60029 (5 yd, 10 s); seed = her ground spot
    // (JedogaPosition POINT_UP, z -16.2), she spawns hovering above it.
    {619, 29310, 385.3f, -705.3f, -16.0f, 15.5f},
    // King Ymiron (Utgarde Pinnacle): Spirit Fount 27339 (7 yd, wanders the hall); pillared hall, small circle.
    {575, 26861, 392.8f, -311.8f, 104.4f, 13.0f},
    // Grand Warlock Nethekurse (Shattered Halls): Lesser Shadow Fissure 17471 (3 yd, 26 s) under players.
    {540, 16807, 188.7f, 270.6f, -13.0f, 16.5f},
    // Zereketh the Unbound (Arcatraz): Unbound Void Zone 21101 (3 yd, 25 s).
    {552, 20870, 259.6f, -143.0f, -9.9f, 26.0f},
    // Wrath-Scryer Soccothrates (Arcatraz): Felfire 35769 along his charge (5 yd, 10 s).
    {552, 20886, 147.2f, 147.3f, 21.3f, 25.5f},
    // Drakkari Colossus / Elemental (Gundrak): Mojo Puddle 55627 / 59451 (3 yd, 10 s).
    {604, 29307, 1672.0f, 743.5f, 142.9f, 18.5f},
    // Hungarfen (Underbog): Underbog Mushrooms 17990 with Spore Cloud 34168 (8 yd, 20 s each, they keep coming).
    {546, 17770, -125.3f, -369.6f, 35.6f, 39.0f},
    // Lavanthor (Violet Hold): Lava Burn 54249 leaves a Lava Bomb trap 194952 (5 yd, 25 s).
    {608, 29312, 1860.6f, 767.7f, 39.5f, 16.5f},
    // Ick (Pit of Saron): Toxic Waste 69024 / 70436 (4 yd, 10 s).
    {658, 36476, 829.8f, 114.5f, 509.6f, 20.0f},
    // Viscous Fallout (Gnomeregan): Radiation Cloud 10341 (8 yd, 30 s) at its own feet.
    {90, 7079, -477.4f, 71.5f, -208.4f, 20.5f},
    // Jammal'an the Prophet (Sunken Temple): Flamestrike 12468 (5 yd, 8 s); narrow altar room.
    {109, 5710, -436.9f, -64.1f, -89.3f, 8.5f},
    // General Drakkisath (Blackrock Spire): Flamestrike 16419 (5 yd, 8 s).
    {229, 10363, 11.5f, -286.0f, 111.0f, 23.5f},
    // Illyanna Ravenoak (Dire Maul East): Volley 22908 (8 yd, 6 s).
    {429, 11488, -82.3f, 545.0f, 28.8f, 30.0f},
    // Grandmaster Vorpil (Shadow Labyrinth): Rain of Fire 33617 / 39363 at his feet after Draw Shadows (10 yd, 6 s).
    {555, 18732, -228.5f, -263.6f, 17.2f, 12.0f},
    // Kael'thas Sunstrider (Magisters' Terrace): Flame Strike 44190 / 46163 (6 yd, 8 s).
    {585, 24664, 148.5f, 170.0f, -16.6f, 18.5f},
    // Mechano-Lord Capacitus (Mechanar): Nether Charges 20405 (15 yd bombs, 16.5 s).
    {554, 19219, 203.2f, -5.0f, -1.9f, 30.5f},
    // Drakos the Interrogator (Oculus): Unstable Spheres 28166 (12 yd bombs, 17 s).
    {578, 27654, 959.8f, 1048.8f, 360.1f, 33.0f},
    // Prince Taldaram (Ahn'kahet): Flame Spheres 30106 / 31686 / 31687 (18 yd beams); seed on his floor (z 11.6).
    {619, 29308, 527.7f, -847.0f, 11.6f, 23.5f},
    // Ormorok the Tree-Shaper (Nexus): Crystal Spikes 27099 (3 yd, 7 s).
    {576, 26794, 268.0f, -227.4f, -8.6f, 23.0f},
    // ---- raids (their raid strategies still own positioning; this is only the room for an escape step)
    // Grobbulus (Naxxramas): Poison Cloud 28240 drops a Grobbulus Cloud 16363 at his feet (28158, grows, 60 s).
    {533, 15931, 3226.6f, -3385.3f, 311.2f, 13.5f},
    // High Priest Venoxis (Zul'Gurub): Poison Cloud 23861 at his feet (8 yd, 10 s).
    {309, 14507, -12029.8f, -1698.9f, 39.7f, 8.5f},
    // Supremus (Black Temple): Volcanic Geyser 42055 (15 yd, 18 s).
    {564, 22898, 702.1f, 675.7f, 73.8f, 34.5f},
    // Gathios the Shatterer (Illidari Council): Consecration 41541 at his feet (10 yd, 20 s).
    {564, 22949, 676.7f, 304.7f, 274.0f, 21.0f},
    // Hex Lord Malacrass (Zul'Aman): Consecration 43429 when he drains a paladin (10 yd, 20 s).
    {568, 24239, 118.4f, 946.6f, 30.8f, 16.5f},
    // Sapphiron (Naxxramas): Chill 28547 blizzards (3 yd, 10 s).
    {533, 15989, 3520.4f, -5238.8f, 137.7f, 66.5f},
    // Grand Widow Faerlina (Naxxramas): Rain of Fire 28794 (10 yd, 6 s).
    {533, 15953, 3353.2f, -3645.1f, 259.6f, 22.5f},
    // Koralon the Flame Watcher (Vault of Archavon): Flaming Cinder 66684 (4 yd, 20 s).
    {624, 35013, -218.5f, 82.4f, 91.7f, 28.5f},
    // Magmadar (Molten Core): Lava Bomb traps 177704 (5 yd, 30-60 s).
    {409, 11982, 1120.6f, -1015.5f, -186.4f, 26.5f},
    // Lord Kri (Bug Trio, AQ40): Toxic Vapors 25786 cloud where he dies (8 yd, 120 s).
    {531, 15511, -8575.9f, 2154.2f, -7.5f, 29.0f},
    // Gruul the Dragonkiller (Gruul's Lair): Cave In 36240 (8 yd, 15 s).
    {565, 19044, 230.9f, 363.1f, -4.7f, 41.5f},
    // Ignis the Furnace Master (Ulduar): Scorch 62548 ground patch under the tank (13 yd, until the fight ends).
    {603, 33118, 586.5f, 353.8f, 360.9f, 13.5f},
    // Lady Deathwhisper (Icecrown Citadel): Death and Decay 71001 (8 yd, 10 s).
    {631, 36855, -644.7f, 2211.4f, 51.8f, 20.0f},
    // Gehennas (Molten Core): Rain of Fire 19717 (10 yd, 6 s).
    {409, 12259, 887.1f, -565.9f, -203.8f, 30.5f},
}};

inline Room const* Lookup(uint32 mapId, uint32 bossEntry)
{
    auto const it = std::find_if(ROOMS.begin(), ROOMS.end(), [mapId, bossEntry](Room const& room)
                                 { return room.mapId == mapId && room.bossEntry == bossEntry; });
    return it != ROOMS.end() ? &*it : nullptr;
}

// MovementAction::KiteRoomLookupFn: true with the room's centre and the radius the tank may kite in (the open floor
// less WALL_MARGIN), for a boss of the table on the bot's map.
inline bool Find(Player* bot, Unit* boss, Position& centre, float& radius)
{
    if (!bot || !boss || boss->GetMapId() != bot->GetMapId())
        return false;

    Room const* room = Lookup(boss->GetMapId(), boss->GetEntry());
    if (!room)
        return false;

    centre.Relocate(room->x, room->y, room->z);
    radius = std::max(MIN_RADIUS, room->floorRadius - WALL_MARGIN);
    return true;
}
}  // namespace BossKiteRooms

#endif
