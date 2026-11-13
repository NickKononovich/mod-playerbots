/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BUYACTION_H
#define PLAYERBOTS_BUYACTION_H

#include "InventoryAction.h"
#include "MovementActions.h"

class Creature;
class FindItemVisitor;
class ObjectGuid;
class Item;
class Player;
class PlayerbotAI;

struct ItemTemplate;
struct VendorItem;
struct VendorItemData;
class Creature;

class BuyAction : public InventoryAction
{
public:
    BuyAction(PlayerbotAI* botAI) : InventoryAction(botAI, "buy") {}

    bool Execute(Event event) override;

private:
    bool BuyItem(VendorItemData const* tItems, ObjectGuid vendorguid, ItemTemplate const* proto);
    /** True if the vendor has stock and the bot can pay the extended cost; gold is checked by the core. */
    bool CanAfford(VendorItem const* tItem, ItemTemplate const* proto, Creature* vendor) const;
    bool TradeItem(FindItemVisitor* visitor, int8 slot);
    bool TradeItem(Item const* item, int8 slot);
};

// Honest food/drink restocking: bots carry only real food bought with their own gold.
constexpr uint32 RESTOCK_FOOD_MIN = 20;     // restock below this many food (or drink) items
constexpr uint32 RESTOCK_FOOD_TARGET = 40;  // buy up to this many
constexpr float RESTOCK_VENDOR_RANGE = 25.0f;
constexpr uint32 RESTOCK_SPELL_CATEGORY_FOOD = 11;
constexpr uint32 RESTOCK_SPELL_CATEGORY_DRINK = 59;

// [supply41] the one test of "is this a restock food / drink": the item's class and subclass, its spell category and
// the real Food / Drink aura behind it. Every judge of the kind - what the bags hold, what a vendor is walked to for,
// what is bought there - asks this, so none of them can name an item the others would refuse.
bool IsRestockFood(ItemTemplate const* proto, uint32 spellCategory);
// Counts real food (category 11) or drink (category 59) the bot can use, in its bags. Bounded bag walk.
uint32 CountBagFood(Player* bot, uint32 spellCategory);
// [dun194] the most one usable item of that kind in the bags restores (RestoredPoints, at most the bot's max health /
// mana: a drink stops at a full pool); 0 when it holds none
float BestBagRestore(Player* bot, uint32 spellCategory);
// [craft399] the share of a full rest one item of this food gives the bot (its restored health, at most the bot's max
// health, over its max health); 0 for no food it can eat
float FoodItemRests(Player* bot, ItemTemplate const* proto);
// [craft399] the full rests the usable food in the bags covers: each item's restored health (at most the bot's max
// health) over its max health, summed - a low-level food counts its share of a rest, not a whole item
float BagFoodRests(Player* bot);
// [dun199] the conjured water a mage of a gathering guild five conjures up to before the run (its own priced line plus
// what the five's other mana users lack of theirs), set by the global leader (world thread), read by the mage's "no
// drink" trigger (map thread); 0: none (it conjures only when it holds no conjured water)
void SetConjureGoal(ObjectGuid mage, uint32 count);
uint32 ConjureGoal(ObjectGuid mage);

// Out of combat, low on food/drink and a vendor/innkeeper selling level-appropriate food is near:
// walk to it and buy with the bot's own gold (consumables budget, repair money stays reserved).
class RestockFoodAction : public MovementAction
{
public:
    RestockFoodAction(PlayerbotAI* botAI) : MovementAction(botAI, "restock food") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // [eco204] why: what kept a purchase from happening (logged by Execute when nothing was bought)
    // [eco206] target: the count it buys up to (RESTOCK_FOOD_TARGET, or a gathering five's [dun194] drinks)
    bool Buy(Creature* vendor, int32 vendorSlot, uint32 have, std::string& why, uint32 target);
    void LogDeclined(std::string const& why);
};

#endif
