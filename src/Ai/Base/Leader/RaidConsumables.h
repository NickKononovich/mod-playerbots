/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDCONSUMABLES_H
#define PLAYERBOTS_RAIDCONSUMABLES_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>

class GameObject;
class Item;
class Player;
class PlayerbotAI;

// Picks raid consumables among the items a bot already carries; nothing here creates an item.
namespace RaidConsumables
{
    constexpr uint32 ITEM_POTION_OF_SPEED = 40211;
    constexpr uint32 ITEM_POTION_OF_WILD_MAGIC = 40212;
    constexpr uint32 ITEM_INDESTRUCTIBLE_POTION = 40093;

    constexpr uint32 GO_SOULWELL_RANK_1 = 181621;
    constexpr uint32 GO_SOULWELL_RANK_1_TALENT_1 = 183510;
    constexpr uint32 GO_SOULWELL_RANK_1_TALENT_2 = 183511;
    constexpr uint32 GO_SOULWELL_RANK_2 = 193169;
    constexpr uint32 GO_SOULWELL_RANK_2_TALENT_1 = 193170;
    constexpr uint32 GO_SOULWELL_RANK_2_TALENT_2 = 193171;
    constexpr uint32 GO_SOUL_PORTAL_RANK_1 = 181622;
    constexpr uint32 GO_SOUL_PORTAL_RANK_2 = 193168;
    // Ritual of Refreshment: the portal needs two more clicks, the table hands out Mana Strudel.
    constexpr uint32 GO_REFRESHMENT_PORTAL_RANK_1 = 186811;
    constexpr uint32 GO_REFRESHMENT_PORTAL_RANK_2 = 193062;
    constexpr uint32 GO_REFRESHMENT_TABLE_RANK_1 = 186812;
    constexpr uint32 GO_REFRESHMENT_TABLE_RANK_2 = 193061;
    constexpr uint32 ITEM_MANA_STRUDEL_RANK_1 = 43518;
    constexpr uint32 ITEM_MANA_STRUDEL_RANK_2 = 43523;

    constexpr float SOULWELL_SEARCH_DISTANCE = 30.0f;

    // Raid instances only: five-man runs do not spend flasks and pre-pull potions.
    bool Allowed(Player* bot);

    // A flask (or battle / guardian elixir) for the bot's role that it is not already under, else empty.
    ObjectGuid FindFlask(Player* bot);
    // Buff food for the bot's role when it is not Well Fed, else empty.
    ObjectGuid FindFoodBuff(Player* bot);
    // Potion of Speed / Wild Magic / Indestructible by role, else empty.
    ObjectGuid FindPrePullPotion(Player* bot);

    Item* FindHealthstone(Player* bot);

    // [lead402] the bag potion that restores mana (`mana`) or health most per use and can be used now: its item
    // entry and the mean amount its spell restores (base points plus half the dice); 0: none
    // [lead502] `withinMs`: a potion whose cooldown ends within these milliseconds counts too, the latest of the
    // picked one's remaining cooldown in `readyInMs` (0: ready now)
    uint32 FindRestorePotion(Player* bot, bool mana, uint32& amount, uint32 withinMs = 0,
                             uint32* readyInMs = nullptr);
    // [lead402] the bag potion whose buff serves the bot's role best (the flask scoring on its spell's auras), not a
    // restore: its item entry and its spell; 0: none. Any dungeon (not only where Allowed)
    uint32 FindBuffPotion(Player* bot, uint32& spell);
    bool IsSoulwell(GameObject* go);
    bool IsSoulPortal(GameObject* go);
    bool IsRefreshmentTable(GameObject* go);

    // [b43_class] Ritual of Souls / Refreshment (the caster's channel and the members' portal clicks) only at a real
    // rest or the run start: no member in combat, no pull of the bot's own, no "pull imminent", and no living enemy
    // within RITUAL_HOSTILE_RANGE. Empty: allowed; otherwise why it waits. Reads cached values only.
    constexpr float RITUAL_HOSTILE_RANGE = 40.0f;
    constexpr float RITUAL_GROUP_COMBAT_RANGE = 100.0f;
    std::string RitualBlockedReason(PlayerbotAI* botAI);
}

#endif
