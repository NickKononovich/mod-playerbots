/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BuyAction.h"
#include "Bag.h"
#include "BudgetValues.h"
#include "DBCStores.h"
#include "Event.h"
#include "GuildGear.h"
#include "GuildHonest.h"
#include "ItemCountValue.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"
#include <algorithm>
#include <cmath>

bool BuyAction::Execute(Event event)
{
    bool buyUseful = false;
    ItemIds itemIds;
    std::string const link = event.getParam();

    if (link == "vendor")
        buyUseful = true;
    else
    {
        itemIds = chat->parseItems(link);
    }

    GuidVector vendors = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();

    bool vendored = false;
    bool result = false;
    for (GuidVector::iterator i = vendors.begin(); i != vendors.end(); ++i)
    {
        ObjectGuid vendorguid = *i;
        Creature* pCreature = bot->GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR);
        if (!pCreature)
            continue;

        vendored = true;

        if (buyUseful)
        {
            // Items are evaluated from high-level to low level.
            // For each item the bot checks again if an item is usefull.
            // Bot will buy until no usefull items are left.

            VendorItemData const* tItems = pCreature->GetVendorItems();
            if (!tItems)
                continue;

            VendorItemList m_items_sorted = tItems->m_items;

            m_items_sorted.erase(std::remove_if(m_items_sorted.begin(), m_items_sorted.end(),
                                                [](VendorItem* i)
                                                {
                                                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(i->item);
                                                    return !proto;
                                                }),
                                 m_items_sorted.end());

            if (m_items_sorted.empty())
                continue;

            StatsWeightCalculator calculator(bot);
            calculator.SetItemSetBonus(false);
            calculator.SetOverflowPenalty(false);

            // Score each item once; the comparator must be a strict weak ordering, and the old
            // "if either score is 0, compare item level" branch was not.
            std::unordered_map<uint32, std::pair<float, uint32>> itemScores;
            for (VendorItem const* vendorItem : m_items_sorted)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
                itemScores[vendorItem->item] = {calculator.CalculateItem(vendorItem->item),
                                                proto ? proto->ItemLevel : 0};
            }

            std::sort(m_items_sorted.begin(), m_items_sorted.end(),
                [&itemScores](VendorItem* i, VendorItem* j)
                {
                    auto const& score1 = itemScores.at(i->item);
                    auto const& score2 = itemScores.at(j->item);
                    if (score1.first != score2.first)
                        return score1.first > score2.first;
                    return score1.second > score2.second;
                });

            std::unordered_map<uint32, float> bestPurchasedItemScore;  // Track best item score per InventoryType

            for (auto& tItem : m_items_sorted)
            {
                uint32 maxPurchases = 1;  // Default to buying once
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(tItem->item);
                if (!proto)
                    continue;

                if (proto->Class == ITEM_CLASS_CONSUMABLE || proto->Class == ITEM_CLASS_PROJECTILE)
                {
                    maxPurchases = 10;  // Allow up to 10 purchases if it's a consumable or projectile
                }

                for (uint32 i = 0; i < maxPurchases; i++)
                {
                    // Stock and currency costs change between purchases; recheck each time.
                    if (!CanAfford(tItem, proto, pCreature))
                        break;

                    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", tItem->item);

                    uint32 invType = proto->InventoryType;

                    // Reuse the score computed for the sort.
                    float newScore = itemScores.at(proto->ItemId).first;

                    // Skip if we already bought a better item for this slot
                    if (bestPurchasedItemScore.find(invType) != bestPurchasedItemScore.end() &&
                        bestPurchasedItemScore[invType] > newScore)
                    {
                        break;  // Skip lower-scoring items
                    }

                    // Check the bot's currently equipped item for this slot
                    uint8 dstSlot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
                    Item* oldItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot);

                    float oldScore = 0.0f;
                    if (oldItem)
                    {
                        ItemTemplate const* oldItemProto = oldItem->GetTemplate();
                        if (oldItemProto)
                            oldScore = calculator.CalculateItem(oldItemProto->ItemId);
                    }

                    // Skip if the bot already has a better or equal item equipped
                    if (oldScore > newScore)
                        break;

                    uint32 price = proto->BuyPrice;
                    price = uint32(floor(price * bot->GetReputationPriceDiscount(pCreature)));

                    NeedMoneyFor needMoneyFor = NeedMoneyFor::none;
                    switch (usage)
                    {
                        case ITEM_USAGE_REPLACE:
                        case ITEM_USAGE_EQUIP:
                        case ITEM_USAGE_BAD_EQUIP:
                        case ITEM_USAGE_BROKEN_EQUIP:
                            needMoneyFor = NeedMoneyFor::gear;
                            break;
                        case ITEM_USAGE_AMMO:
                            needMoneyFor = NeedMoneyFor::ammo;
                            break;
                        case ITEM_USAGE_QUEST:
                            needMoneyFor = NeedMoneyFor::anything;
                            break;
                        case ITEM_USAGE_USE:
                            needMoneyFor = NeedMoneyFor::consumables;
                            break;
                        case ITEM_USAGE_SKILL:
                            needMoneyFor = NeedMoneyFor::tradeskill;
                            break;
                        default:
                            break;
                    }

                    if (needMoneyFor == NeedMoneyFor::none)
                        break;

                    // [equip] a guild-plan bot buys gear only on the global leader's verdict (b337: Thokhen bought
                    // Hunter's Boomstick on its own check, [alert] DOUBLE_DECISION source vendor buy)
                    std::string leaderDecision;
                    if (needMoneyFor == NeedMoneyFor::gear && GuildHonest::IsHonestBot(bot) &&
                        !GuildGear::ShouldBuy(bot, proto, leaderDecision))
                        break;

                    if (AI_VALUE2(uint32, "free money for", uint32(needMoneyFor)) < price)
                        break;

                    if (!BuyItem(tItems, vendorguid, proto))
                        break;
                    if (!leaderDecision.empty())
                        GuildGear::HistoryNoteDecisionIfNone(bot->GetGUID().GetCounter(), proto->ItemId, "leader",
                                                             leaderDecision);

                    // Store the best item score per InventoryType
                    bestPurchasedItemScore[invType] = newScore;

                    if (needMoneyFor == NeedMoneyFor::gear)
                    {
                        botAI->DoSpecificAction("equip upgrades packet action");
                    }
                }
            }
        }
        else
        {
            if (itemIds.empty())
                return false;

            for (ItemIds::iterator i = itemIds.begin(); i != itemIds.end(); i++)
            {
                uint32 itemId = *i;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                if (!proto)
                    continue;

                result |= BuyItem(pCreature->GetVendorItems(), vendorguid, proto);

                if (!result)
                {
                    std::ostringstream out;
                    out << "Nobody sells " << ChatHelper::FormatItem(proto) << " nearby";
                    botAI->TellMaster(out.str());
                    continue;
                }

                ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemId);
                if (usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_EQUIP ||
                    usage == ITEM_USAGE_BAD_EQUIP || usage == ITEM_USAGE_BROKEN_EQUIP)
                {
                    botAI->DoSpecificAction("equip upgrades packet action");
                    break;
                }
            }
        }
    }

    return vendored;
}

bool BuyAction::CanAfford(VendorItem const* tItem, ItemTemplate const* proto, Creature* vendor) const
{
    // Limited stock: skip if the vendor has fewer than one purchase remaining.
    if (tItem->maxcount != 0 && vendor->GetVendorItemCurrentCount(tItem) < proto->BuyCount)
        return false;

    // Non-gold costs (honor / arena points, token items, personal arena rating).
    if (tItem->ExtendedCost)
    {
        ItemExtendedCostEntry const* iece = sItemExtendedCostStore.LookupEntry(tItem->ExtendedCost);
        if (!iece)
            return false;

        if (bot->GetHonorPoints() < iece->reqhonorpoints)
            return false;
        if (bot->GetArenaPoints() < iece->reqarenapoints)
            return false;

        for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)
            if (iece->reqitem[i] && !bot->HasItemCount(iece->reqitem[i], iece->reqitemcount[i]))
                return false;

        if (bot->GetMaxPersonalArenaRatingRequirement(iece->reqarenaslot) < iece->reqpersonalarenarating)
            return false;
    }

    return true;
}

bool BuyAction::BuyItem(VendorItemData const* tItems, ObjectGuid vendorguid, ItemTemplate const* proto)
{
    if (!tItems || !proto)
        return false;

    uint32 itemId = proto->ItemId;
    uint32 oldCount = bot->GetItemCount(itemId, false);

    for (uint32 slot = 0; slot < tItems->GetItemCount(); ++slot)
    {
        if (tItems->GetItem(slot)->item != itemId)
            continue;

        // Paid from the bot's own gold; the core refuses the purchase when it cannot afford it.
        bot->BuyItemFromVendorSlot(vendorguid, slot, itemId, 1, NULL_BAG, NULL_SLOT);

        uint32 newCount = bot->GetItemCount(itemId, false);
        if (newCount > oldCount)
        {
            std::ostringstream out;
            out << "Buying " << ChatHelper::FormatItem(proto);
            botAI->TellMaster(out.str());
            return true;
        }

        return false;
    }

    return false;
}

namespace
{
// Real food heals over time (aura 20), real drink restores mana (aura 21); this skips alcohol and novelty items.
bool IsRestockFood(ItemTemplate const* proto, uint32 spellCategory)
{
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE ||
        (proto->SubClass != ITEM_SUBCLASS_CONSUMABLE && proto->SubClass != ITEM_SUBCLASS_FOOD) ||
        proto->Spells[0].SpellCategory != spellCategory)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId);
    if (!spellInfo)
        return false;

    // [supply41] the real Food / Drink spells: SPELL_AURA_MOD_REGEN (433, 1131) / SPELL_AURA_MOD_POWER_REGEN (430-432,
    // 1133, 22734) - only the OBS_MOD auras of a few buff foods were taken, so a "buy drink" run ([supply36]) reached
    // the vendor and bought none
    AuraType const wanted =
        spellCategory == RESTOCK_SPELL_CATEGORY_FOOD ? SPELL_AURA_OBS_MOD_HEALTH : SPELL_AURA_OBS_MOD_POWER;
    AuraType const wantedRegen =
        spellCategory == RESTOCK_SPELL_CATEGORY_FOOD ? SPELL_AURA_MOD_REGEN : SPELL_AURA_MOD_POWER_REGEN;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (spellInfo->Effects[i].ApplyAuraName == wanted || spellInfo->Effects[i].ApplyAuraName == wantedRegen)
            return true;

    return false;
}

// Best (highest required level the bot meets) gold-priced food/drink in the vendor list, or -1.
int32 PickVendorFood(Player* bot, Creature* vendor, uint32 spellCategory)
{
    VendorItemData const* items = vendor->GetVendorItems();
    if (!items)
        return -1;

    int32 bestSlot = -1;
    uint32 bestLevel = 0;
    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* vItem = items->GetItem(slot);
        if (!vItem || vItem->ExtendedCost)
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vItem->item);
        if (!IsRestockFood(proto, spellCategory) || proto->BuyPrice == 0)
            continue;

        if (proto->RequiredLevel > bot->GetLevel() || bot->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;

        if (vItem->maxcount && vendor->GetVendorItemCurrentCount(vItem) == 0)
            continue;

        if (bestSlot < 0 || proto->RequiredLevel > bestLevel)
        {
            bestSlot = static_cast<int32>(slot);
            bestLevel = proto->RequiredLevel;
        }
    }

    return bestSlot;
}
}  // namespace

uint32 CountBagFood(Player* bot, uint32 spellCategory)
{
    uint32 count = 0;
    auto countItem = [&](Item* item)
    {
        if (!item)
            return;
        ItemTemplate const* proto = item->GetTemplate();
        if (proto && proto->Class == ITEM_CLASS_CONSUMABLE &&
            (proto->SubClass == ITEM_SUBCLASS_CONSUMABLE || proto->SubClass == ITEM_SUBCLASS_FOOD) &&
            proto->Spells[0].SpellCategory == spellCategory && bot->CanUseItem(proto) == EQUIP_ERR_OK)
            count += item->GetCount();
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        countItem(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        if (!bag)
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            countItem(bag->GetItemByPos(static_cast<uint8>(slot)));
    }

    return count;
}

bool RestockFoodAction::isUseful()
{
    // The user's own character (selfbot) never spends gold on its own.
    // [supply26] ... and only with money it may spend on consumables: with none, Buy fails every time and the action
    // is picked again at each trigger (b375 Jurdogs: "restock food - PREREQ / FAILED", two STUCK alerts)
    return bot->IsAlive() && !bot->IsInCombat() && !bot->InBattleground() && !bot->IsInFlight() &&
           !bot->IsBeingTeleported() && !bot->GetVehicle() && !IsSelfBot(bot) &&
           AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::consumables)) > 0;
}

bool RestockFoodAction::Execute(Event /*event*/)
{
    bool const usesMana = bot->GetMaxPower(POWER_MANA) > 0;
    uint32 const food = CountBagFood(bot, RESTOCK_SPELL_CATEGORY_FOOD);
    uint32 const drink = usesMana ? CountBagFood(bot, RESTOCK_SPELL_CATEGORY_DRINK) : RESTOCK_FOOD_TARGET;
    bool const needFood = food < RESTOCK_FOOD_MIN;
    bool const needDrink = drink < RESTOCK_FOOD_MIN;
    if (!needFood && !needDrink)
        return false;

    Creature* vendor = nullptr;
    int32 foodSlot = -1;
    int32 drinkSlot = -1;
    float bestDist = RESTOCK_VENDOR_RANGE;

    GuidVector const npcs = AI_VALUE(GuidVector, "nearest npcs");
    for (ObjectGuid const& guid : npcs)
    {
        Creature* creature = botAI->GetCreature(guid);
        if (!creature || !creature->IsAlive() || !creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) ||
            creature->IsHostileTo(bot))
            continue;

        float const dist = bot->GetExactDist(creature);
        if (dist > bestDist)
            continue;

        int32 const f = needFood ? PickVendorFood(bot, creature, RESTOCK_SPELL_CATEGORY_FOOD) : -1;
        int32 const d = needDrink ? PickVendorFood(bot, creature, RESTOCK_SPELL_CATEGORY_DRINK) : -1;
        if (f < 0 && d < 0)
            continue;

        vendor = creature;
        foodSlot = f;
        drinkSlot = d;
        bestDist = dist;
    }

    if (!vendor)
        return false;

    if (!bot->GetNPCIfCanInteractWith(vendor->GetGUID(), UNIT_NPC_FLAG_VENDOR))
        return MoveNear(vendor, INTERACTION_DISTANCE - 1.0f);

    bool bought = false;
    if (drinkSlot >= 0)
        bought |= Buy(vendor, drinkSlot, drink);
    if (foodSlot >= 0)
        bought |= Buy(vendor, foodSlot, food);

    return bought;
}

bool RestockFoodAction::Buy(Creature* vendor, int32 vendorSlot, uint32 have)
{
    if (have >= RESTOCK_FOOD_TARGET)
        return false;

    VendorItemData const* items = vendor->GetVendorItems();
    VendorItem const* vItem = items ? items->GetItem(static_cast<uint32>(vendorSlot)) : nullptr;
    ItemTemplate const* proto = vItem ? sObjectMgr->GetItemTemplate(vItem->item) : nullptr;
    if (!proto)
        return false;

    uint32 const perBuy = std::max<uint32>(1, proto->BuyCount);
    uint32 const unitPrice =
        std::max<uint32>(1, uint32(std::floor(proto->BuyPrice * bot->GetReputationPriceDiscount(vendor))));
    uint32 const budget = AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::consumables));

    uint32 purchases = (RESTOCK_FOOD_TARGET - have + perBuy - 1) / perBuy;
    purchases = std::min(purchases, budget / unitPrice);
    if (!purchases)
        return false;

    uint32 const stackPurchases = std::max<uint32>(1, proto->GetMaxStackSize() / perBuy);
    uint32 const before = bot->GetItemCount(proto->ItemId, false);
    while (purchases > 0)
    {
        uint32 const chunk = std::min<uint32>(std::min(purchases, stackPurchases), 255);
        uint32 const was = bot->GetItemCount(proto->ItemId, false);
        bot->BuyItemFromVendorSlot(vendor->GetGUID(), static_cast<uint32>(vendorSlot), proto->ItemId,
                                   static_cast<uint8>(chunk), NULL_BAG, NULL_SLOT);
        if (bot->GetItemCount(proto->ItemId, false) <= was)
            break;  // no money, no bag space or sold out
        purchases -= chunk;
    }

    uint32 const gained = bot->GetItemCount(proto->ItemId, false) - before;
    if (!gained)
        return false;

    std::ostringstream out;
    out << "Buying " << gained << "x " << ChatHelper::FormatItem(proto);
    botAI->TellMaster(out.str());
    // [supply41] one line per purchase (the restock's own proof in the log)
    LOG_INFO("playerbots", "[supply41] {} L{} bought {} {} at {} ({}c left)", bot->GetName(), bot->GetLevel(), gained,
             proto->Name1, vendor->GetName(), bot->GetMoney());
    return true;
}
