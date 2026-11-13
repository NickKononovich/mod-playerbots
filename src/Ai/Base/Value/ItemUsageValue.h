/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ITEMUSAGEVALUE_H
#define PLAYERBOTS_ITEMUSAGEVALUE_H

#include "NamedObjectContext.h"
#include "Value.h"

class Item;
class Player;
class PlayerbotAI;

struct ItemTemplate;
struct ParsedItemUsage
{
    uint32 itemId = 0;
    int32 randomPropertyId = 0;
};
enum ItemUsage : uint32
{
    ITEM_USAGE_NONE = 0,
    ITEM_USAGE_EQUIP = 1,
    ITEM_USAGE_REPLACE = 2,
    ITEM_USAGE_BAD_EQUIP = 3,
    ITEM_USAGE_BROKEN_EQUIP = 4,
    ITEM_USAGE_QUEST = 5,
    ITEM_USAGE_SKILL = 6,
    ITEM_USAGE_USE = 7,
    ITEM_USAGE_GUILD_TASK = 8,
    ITEM_USAGE_DISENCHANT = 9,
    ITEM_USAGE_AH = 10,
    ITEM_USAGE_KEEP = 11,
    ITEM_USAGE_VENDOR = 12,
    ITEM_USAGE_AMMO = 13
};

class ItemUsageValue : public CalculatedValue<ItemUsage>, public Qualified
{
public:
    ItemUsageValue(PlayerbotAI* botAI, std::string const name = "item usage") : CalculatedValue<ItemUsage>(botAI, name)
    {
    }

    ItemUsage Calculate() override;

protected:
    ItemUsage QueryItemUsageForEquip(ItemTemplate const* proto, int32 randomPropertyId = 0);
    ItemUsage QueryItemUsageForAmmo(ItemTemplate const* proto);
    ParsedItemUsage GetItemIdFromQualifier();

private:
    uint32 GetSmallestBagSize();
    bool IsItemUsefulForQuest(Player* player, ItemTemplate const* proto);
    bool IsStartQuestItemUsefulFor(Player* player, ItemTemplate const* proto);
    bool IsItemNeededForSkill(ItemTemplate const* proto);
    bool IsItemUsefulForSkill(ItemTemplate const* proto);
    bool IsItemNeededForClassSpell(ItemTemplate const* proto);
    /** True for temporary weapon enhancements (stones, oils, poisons). */
    bool IsTemporaryWeaponEnchantment(ItemTemplate const* proto);
    bool HasItemsNeededForSpell(uint32 spellId, ItemTemplate const* proto);
    Item* CurrentItem(ItemTemplate const* proto);
    float CurrentStacks(ItemTemplate const* proto);
    float BetterStacks(ItemTemplate const* proto, std::string const usageType = "");

public:
    bool IsItemNeededForUsefullSpell(ItemTemplate const* proto, bool checkAllReagents = false);
    /** True if this poison is the highest item level of its family (use-spell SpellFamilyFlags). */
    bool IsBestPoison(ItemTemplate const* proto);

    // [econ71] the one judge of how much ammo a bot carries. AmmoSubClassFor is what its EQUIPPED ranged weapon
    // fires (ITEM_SUBCLASS_ARROW for a bow or crossbow, ITEM_SUBCLASS_BULLET for a gun, 0 for a thrown weapon, an
    // empty ranged slot or a class with no ranged attack - and 0 means it takes no ammo at all). AmmoHeldRounds is
    // what it already carries of that subclass; AmmoCapRounds is the derived ceiling, see the comment on its body.
    static uint32 AmmoSubClassFor(Player* bot);
    static uint32 AmmoHeldRounds(Player* bot, uint32 subClass);
    static uint32 AmmoCapRounds(Player* bot, ItemTemplate const* ammoProto);

    static std::vector<uint32> SpellsUsingItem(uint32 itemId, Player* bot);
    static bool SpellGivesSkillUp(uint32 spellId, Player* bot);

    static std::string const GetConsumableType(ItemTemplate const* proto, bool hasMana);
};

class ItemUpgradeValue : public ItemUsageValue
{
public:
    ItemUpgradeValue(PlayerbotAI* botAI, std::string const name = "item upgrade") : ItemUsageValue(botAI, name)
    {
    }

    ItemUsage Calculate() override;
};

#endif
