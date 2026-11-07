/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDGEAR_H
#define PLAYERBOTS_GUILDGEAR_H

#include <string>
#include <vector>

#include "Define.h"

class Player;
class StatsWeightCalculator;
struct ItemTemplate;

// [guild50] The upgrade index of the global leader: for every equippable item, where it comes from (quest reward,
// dungeon boss drop, craft, vendor), built once at startup from the world DB and the in-memory templates. Per-member
// queries (class, spec, level) filter it at plan time: the class / armor / weapon rules of the core (CanUseItem) and
// of the module (RandomItemMgr::CanEquipArmor / CanEquipWeapon), the spec through StatsWeightCalculator.
namespace GuildGear
{
    enum SourceKind : uint8
    {
        SRC_QUEST = 0,   // id = quest, level = quest level, aux = choice index (0..5) or 100 + fixed reward index
        SRC_DROP = 1,    // id = boss creature entry, level = dungeon min level, mapId, chance (%)
        SRC_CRAFT = 2,   // id = recipe spell, level = skill the recipe needs, aux = skill line
        SRC_VENDOR = 3,  // id = vendor creature entry, aux = buy price (copper)
        SRC_BANK = 4     // the guild bank holds it (plan time only)
    };

    struct Source
    {
        uint8 kind{SRC_QUEST};
        uint32 id{0};
        uint32 level{0};
        uint32 mapId{0};
        float chance{0.0f};
        uint32 aux{0};
    };

    struct VendorSpawn
    {
        uint32 entry{0};
        uint16 mapId{0};
        float x{0}, y{0}, z{0};
    };

    struct CraftRecipe
    {
        uint32 spellId{0};
        uint32 skillLine{0};
        uint32 reqSkill{0};
        uint32 itemId{0};
        uint32 made{1};
        std::vector<std::pair<uint32, uint32>> reagents;
    };

    // startup (world thread); the lines are the craft skill lines the coordinator plans
    void Load(std::vector<uint32> const& craftLines);

    std::vector<Source> const* SourcesOf(uint32 itemId);
    // equippable items with a source, of one equip group (EquipGroup), sorted by RequiredLevel
    std::vector<uint32> const& ItemsOfGroup(uint8 group);
    std::vector<VendorSpawn> const* VendorsOf(uint32 itemId);
    CraftRecipe const* Recipe(uint32 spellId);
    std::vector<uint32> const& BagRecipes();  // recipe spells that make a generic bag
    std::string DungeonName(uint32 mapId);

    // The equip group of an inventory type (one group per equipment slot family), 0xFF = not planned
    uint8 EquipGroup(uint8 inventoryType);
    constexpr uint8 GROUP_COUNT = 16;  // the slot groups EquipGroup returns: 0 .. GROUP_COUNT - 1
    char const* GroupName(uint8 group);
    // The equipment slots a member may put an item of this group into (both rings, main+off hand for dual wield)
    std::vector<uint8> SlotsFor(Player* member, ItemTemplate const* proto);

    struct Eval
    {
        float score{0.0f};
        float current{0.0f};
        float gain{0.0f};  // (score - current) / current; an empty slot = 1; a [guild12] tie-break = 0.001
        uint8 slot{0};
        uint32 worn{0};    // item entry in the weakest slot (0 = empty)
    };

    // Whether the item is an upgrade for the member in its weakest slot of the group: the core's CanUseItem, the
    // module's class armor / weapon rules, the StatsWeightCalculator score against the worn item scaled by
    // EquipUpgradeThreshold (as ItemUsageValue), equal scores broken as [guild12] (armor, item level, quality).
    // `calc` belongs to the member. ahead: the item's level is still ahead of the member (a plan): only that check
    // of CanUseItem is waived.
    bool Evaluate(Player* member, StatsWeightCalculator& calc, ItemTemplate const* proto, Eval& out,
                  bool ahead = false);

    // The count of each source kind and of indexed items, for the startup log and the checks.
    std::string Stats();
}

#endif
