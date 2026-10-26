/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BWLHELPERS_H
#define PLAYERBOTS_BWLHELPERS_H

#include "Player.h"
#include "PlayerbotAI.h"

namespace BlackwingLairHelpers
{
    enum class BlackwingLairSpells : uint32
    {
        // General
        SPELL_ONYXIA_SCALE_CLOAK = 22683,
        SPELL_DISARM_TRAP = 1842,

        // Razorgore the Untamed
        SPELL_MINDCONTROL = 19832,
        SPELL_DESTROY_EGG = 19873,     // cast by the possessed Razorgore on an egg (3s, 10y)
        SPELL_MIND_EXHAUSTION = 23958, // on the orb user for 60s after the possession

        // Vaelastrasz the Corrupt
        SPELL_BURNING_ADRENALINE = 18173,

        // Chromaggus
        SPELL_BROOD_AFFLICTION_BRONZE = 23170,
        SPELL_HOURGLASS_SAND = 23645,

        // Nefarian
        SPELL_WILD_MAGIC = 23410
    };

    enum class BlackwingLairItems : uint32
    {
        ITEM_ONYXIA_SCALE_CLOAK = 15138
    };

    enum class BlackwingLairGameObjects : uint32
    {
        // General
        GO_SUPPRESSION_DEVICE = 179784,

        // Razorgore the Untamed
        GO_BLACK_DRAGON_EGG = 177807,
        GO_ORB_OF_DOMINATION = 177808
    };

    enum class BlackwingLairNPCs : uint32
    {
        // Razorgore the Untamed
        NPC_RAZORGORE = 12435,

        // Broodlord Lashlayer
        NPC_CORRUPTED_RED_WHELP = 14022,
        NPC_CORRUPTED_GREEN_WHELP = 14023,
        NPC_CORRUPTED_BLUE_WHELP = 14024,
        NPC_CORRUPTED_BRONZE_WHELP = 14025,

        // Trash
        NPC_DEATH_TALON_WYRMGUARD = 12460
    };

    constexpr float BROODLORD_SAFE_DISTANCE = 18.0f;

    constexpr uint32 BWL_MAP_ID = 469;
    // Razorgore's spawn, the middle of his room. The group leader entering this radius starts the
    // egg phase when nobody has pulled yet.
    inline Position const RAZORGORE_ROOM_CENTER = { -7570.6f, -1090.0f, 413.6f };
    constexpr float RAZORGORE_EVENT_START_RADIUS = 45.0f;
    constexpr float RAZORGORE_SEARCH_RADIUS = 150.0f;

    bool IsActiveSuppressionDeviceInRange(GameObject const* go, Player const* bot);
    bool AreRazorgoreEggsAlive(PlayerbotAI* botAI);
    bool IsRazorgoreOffTank(Player* bot);
    Unit* FindRazorgore(Player* bot);
    bool IsControllingRazorgore(Player* bot);
    bool CanUseOrbOfDomination(Player* member);
    Player* GetRazorgoreOrbController(Player* bot, Unit* razorgore);
    GameObject* FindNearestIntactEgg(Unit* razorgore);
    bool IsNonBABotNearPosition(Player const* bot, Position const& position, float distance);
    bool IsCorruptedWhelp(Unit const* unit);
}

#endif
