/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EquipAction.h"
#include "Bag.h"
#include "GuildCoordinator.h"
#include "GuildGear.h"
#include "GuildHonest.h"
#include "Event.h"
#include "ItemCountValue.h"
#include "ItemPackets.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "Log.h"
#include "Playerbots.h"
#include "StatsWeightCalculator.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

bool EquipAction::Execute(Event event)
{
    std::string const text = event.getParam();
    ItemIds ids = chat->parseItems(text);
    EquipItems(ids);
    return true;
}

void EquipAction::EquipItems(ItemIds ids)
{
    for (ItemIds::iterator i = ids.begin(); i != ids.end(); i++)
    {
        FindItemByIdVisitor visitor(*i);
        EquipItem(&visitor);
    }
}

namespace
{
    bool GenericBag(ItemTemplate const* proto)
    {
        return proto && proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER;
    }

    // [inv375] a failed bag swap logged once per bot and reason (the equip pass runs again on every random tick)
    std::mutex bagFailLock;
    std::unordered_map<uint32, std::string> bagFailLogged;
}

// [inv375] b446: 5 members carried an 8-slot bag in their bags while wearing a 6-slot one (Rumanatol, Thokhen:
// Nolkai's Bag; Gilduck, Uraela, Jurdogs) - each time inside the very bag it was to replace (the last of the
// smallest, where the loot landed), and the core refuses to equip a bag into the slot of the bag it lies in
// (SwapItem: "prevent equipping bag in the same slot from its inside"), silently, every pass. A player empties the
// small bag first: its items (the bigger bag among them) go to free slots elsewhere (a special bag only takes what
// fits it), then the bigger bag takes its slot and the small one lands where the bigger lay. Special bags (soul,
// herb, ammo pouch) only go into a free bag slot and are never replaced by a generic one; a quiver is no generic bag
bool EquipAction::EquipBag(Item* item)
{
    ItemTemplate const* proto = item->GetTemplate();
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            bot->SwapItem(item->GetPos(), (INVENTORY_SLOT_BAG_0 << 8) | slot);
            return true;
        }
    if (!GenericBag(proto))
        return true;
    Bag* oldBag = nullptr;
    uint8 oldSlot = 0;
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (Bag* bag = bot->GetBagByPos(slot); bag && bag != item && GenericBag(bag->GetTemplate()) &&
                                                 bag->GetBagSize() < proto->ContainerSlots &&
                                                 (!oldBag || bag->GetBagSize() < oldBag->GetBagSize()))
        {
            oldBag = bag;
            oldSlot = slot;
        }
    if (!oldBag)
        return true;
    auto const fail = [&](std::string const& why)
    {
        std::string const key = std::to_string(proto->ItemId) + ":" + why;
        std::lock_guard<std::mutex> guard(bagFailLock);
        std::string& last = bagFailLogged[bot->GetGUID().GetCounter()];
        if (last == key)
            return;
        last = key;
        LOG_INFO("playerbots", "[inv375] {} L{} keeps {} ({} slots) in its bags: {}", bot->GetName(), bot->GetLevel(),
                 oldBag->GetTemplate()->Name1, oldBag->GetBagSize(), why);
    };
    // the free slots outside the old bag: the backpack and the other worn bags
    std::vector<std::pair<uint8, uint8>> room;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            room.emplace_back(INVENTORY_SLOT_BAG_0, slot);
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (Bag* bag = bot->GetBagByPos(slot); bag && slot != oldSlot)
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                if (!bag->GetItemByPos(uint8(j)))
                    room.emplace_back(slot, uint8(j));
    uint32 inside = 0;
    for (uint32 j = 0; j < oldBag->GetBagSize(); ++j)
        inside += oldBag->GetItemByPos(uint8(j)) ? 1 : 0;
    if (room.size() < inside)
    {
        fail("no room to empty " + std::string(oldBag->GetTemplate()->Name1) + " (" + std::to_string(inside) +
             " items, " + std::to_string(room.size()) + " free slots elsewhere)");
        return true;
    }
    uint32 moved = 0;
    for (uint32 j = 0; j < oldBag->GetBagSize(); ++j)
    {
        Item* content = oldBag->GetItemByPos(uint8(j));
        if (!content)
            continue;
        for (auto spot = room.begin(); spot != room.end(); ++spot)
        {
            ItemPosCountVec dest;
            if (bot->CanStoreItem(spot->first, spot->second, dest, content, false) != EQUIP_ERR_OK)
                continue;  // a special bag that does not take it
            bot->SwapItem(content->GetPos(), (spot->first << 8) | spot->second);
            room.erase(spot);
            break;
        }
        if (oldBag->GetItemByPos(uint8(j)))
        {
            fail(std::string(content->GetTemplate()->Name1) + " fits no free slot outside it");
            return true;
        }
        ++moved;
    }
    if (!oldBag->IsEmpty())
    {
        fail("it did not empty");
        return true;
    }
    std::string const oldName = oldBag->GetTemplate()->Name1;
    uint32 const oldSize = oldBag->GetBagSize();
    bot->SwapItem(item->GetPos(), (INVENTORY_SLOT_BAG_0 << 8) | oldSlot);
    if (bot->GetBagByPos(oldSlot) != item)
    {
        fail("the core refused the swap");
        return true;
    }
    {
        std::lock_guard<std::mutex> guard(bagFailLock);
        bagFailLogged.erase(bot->GetGUID().GetCounter());
    }
    LOG_INFO("playerbots", "[inv375] {} L{} equips {} ({} slots) in place of {} ({} slots): {} items moved out of it "
             "first", bot->GetName(), bot->GetLevel(), proto->Name1, proto->ContainerSlots, oldName, oldSize,
             moved);
    return true;
}

void EquipAction::EquipItem(FindItemVisitor* visitor)
{
    IterateItems(visitor);
    std::vector<Item*> items = visitor->GetResult();
    if (!items.empty())
        EquipItem(*items.begin());
}

void EquipAction::EquipItem(Item* item)
{
    ItemTemplate const* itemProto = item->GetTemplate();
    uint32 itemId = itemProto->ItemId;
    uint8 invType = itemProto->InventoryType;

    // Handle ammunition separately
    if (invType == INVTYPE_AMMO)
    {
        bot->SetAmmo(itemId);
        std::ostringstream out;
        out << "equipping " << chat->FormatItem(itemProto);
        botAI->TellMaster(out);
        return;
    }

    // Handle bags first ([inv375] EquipBag)
    bool equippedBag = false;
    if (itemProto->Class == ITEM_CLASS_CONTAINER)
        equippedBag = EquipBag(item);

    // If we didn't equip as a bag, try to equip as gear
    if (!equippedBag)
    {
        // Ranged weapons aren't handled by the rest of the weapon equip logic
        // Handle them early here to avoid issues.
        if (invType == INVTYPE_RANGED || invType == INVTYPE_THROWN || invType == INVTYPE_RANGEDRIGHT)
        {
            WorldPacket packet(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
            ObjectGuid itemguid = item->GetGUID();
            packet << itemguid << uint8(EQUIPMENT_SLOT_RANGED);

            WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(packet));
            nicePacket.Read();
            bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);

            std::ostringstream out;
            out << "Equipping " << chat->FormatItem(itemProto) << " in ranged slot";
            botAI->TellMaster(out);
            return;
        }

        uint8 dstSlot = botAI->FindEquipSlot(itemProto, NULL_SLOT, true);

        // Check if the item is a weapon and whether the bot can dual wield or use Titan Grip
        bool isWeapon = (itemProto->Class == ITEM_CLASS_WEAPON);
        bool canTitanGrip = bot->CanTitanGrip();
        bool canDualWield = bot->CanDualWield();

        bool isTwoHander = (invType == INVTYPE_2HWEAPON);
        bool isValidTGWeapon = false;
        if (canTitanGrip && isTwoHander)
        {
            // Titan Grip-valid 2H weapon subclasses: Axe2, Mace2, Sword2
            isValidTGWeapon = (itemProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                               itemProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                               itemProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2);
        }

        // Check if the main hand currently has a 2H weapon equipped
        Item* currentMHItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        bool have2HWeaponEquipped = (currentMHItem && currentMHItem->GetTemplate()->InventoryType == INVTYPE_2HWEAPON);

        // bool canDualWieldOrTG = (canDualWield || (canTitanGrip && isTwoHander));
        bool canDualWieldOrTG = (canDualWield || isTwoHander);

        // If this is a weapon and we can dual wield or Titan Grip, check if we can improve main/off-hand setup
        if (isWeapon && canDualWieldOrTG)
        {
            // Fetch current main hand and offhand items
            Item* mainHandItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            Item* offHandItem  = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);

            // Set up the stats calculator once and reuse results for performance
            StatsWeightCalculator calculator(bot);
            calculator.SetItemSetBonus(false);
            calculator.SetOverflowPenalty(false);

            // Calculate item scores once and store them
            float newItemScore = calculator.CalculateItem(itemId, item->GetItemRandomPropertyId());
            float mainHandScore = mainHandItem
                ? calculator.CalculateItem(mainHandItem->GetTemplate()->ItemId, mainHandItem->GetItemRandomPropertyId()) : 0.0f;
            float offHandScore = offHandItem
                ? calculator.CalculateItem(offHandItem->GetTemplate()->ItemId, offHandItem->GetItemRandomPropertyId()) : 0.0f;

            // Determine where this weapon can go
            bool canGoMain = (invType == INVTYPE_WEAPON ||
                              invType == INVTYPE_WEAPONMAINHAND ||
                              isTwoHander);

            bool canTGOff = false;
            if (canTitanGrip && isTwoHander && isValidTGWeapon)
                canTGOff = true;

            bool canGoOff = (invType == INVTYPE_WEAPON ||
                             invType == INVTYPE_WEAPONOFFHAND ||
                             canTGOff);

            // Check if the main hand item can go to offhand if needed
            bool mainHandCanGoOff = false;
            if (mainHandItem)
            {
                ItemTemplate const* mhProto = mainHandItem->GetTemplate();
                bool mhIsValidTG = false;
                if (canTitanGrip && mhProto->InventoryType == INVTYPE_2HWEAPON)
                {
                    mhIsValidTG = (mhProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                                   mhProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                                   mhProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2);
                }

                mainHandCanGoOff = (mhProto->InventoryType == INVTYPE_WEAPON ||
                                    mhProto->InventoryType == INVTYPE_WEAPONOFFHAND ||
                                    (mhProto->InventoryType == INVTYPE_2HWEAPON && mhIsValidTG));
            }

            // Priority 1: Replace main hand if the new weapon is strictly better
            // and if conditions allow (e.g. no conflicting 2H logic)
            bool betterThanMH = (newItemScore > mainHandScore);
            // If a one-handed weapon is better, we can still use it instead of a two-handed weapon
            bool mhConditionOK = (invType != INVTYPE_2HWEAPON ||
                      (isTwoHander && !canTitanGrip) ||
                      (canTitanGrip && isValidTGWeapon));

            if (canGoMain && betterThanMH && mhConditionOK)
            {
                // Equip new weapon in main hand
                {
                    WorldPacket eqPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                    ObjectGuid newItemGuid = item->GetGUID();
                    eqPacket << newItemGuid << uint8(EQUIPMENT_SLOT_MAINHAND);
                    WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(eqPacket));
                    nicePacket.Read();
                    bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);
                }

                // Try moving old main hand weapon to offhand if beneficial
                if (mainHandItem && mainHandCanGoOff && (!offHandItem || mainHandScore > offHandScore))
                {
                    ItemTemplate const* oldMHProto = mainHandItem->GetTemplate();

                    WorldPacket offhandPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                    ObjectGuid oldMHGuid = mainHandItem->GetGUID();
                    offhandPacket << oldMHGuid << uint8(EQUIPMENT_SLOT_OFFHAND);
                    WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(offhandPacket));
                    nicePacket.Read();
                    bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);

                    std::ostringstream moveMsg;
                    moveMsg << "Main hand upgrade found. Moving " << chat->FormatItem(oldMHProto) << " to offhand";
                    botAI->TellMaster(moveMsg);
                }

                std::ostringstream out;
                out << "Equipping " << chat->FormatItem(itemProto) << " in main hand";
                botAI->TellMaster(out);
                return;
            }

            // Priority 2: If not better than main hand, check if better than offhand
            else if (canGoOff && newItemScore > offHandScore)
            {
                // Equip in offhand
                WorldPacket eqPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                ObjectGuid newItemGuid = item->GetGUID();
                eqPacket << newItemGuid << uint8(EQUIPMENT_SLOT_OFFHAND);
                WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(eqPacket));
                nicePacket.Read();
                bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);

                std::ostringstream out;
                out << "Equipping " << chat->FormatItem(itemProto) << " in offhand";
                botAI->TellMaster(out);
                return;
            }
            else
            {
                // No improvement, do nothing
                return;
            }
        }

        // If not a special dual-wield/TG scenario or no improvement found, fall back to original logic
        if (dstSlot == EQUIPMENT_SLOT_FINGER1 ||
            dstSlot == EQUIPMENT_SLOT_TRINKET1 ||
            (dstSlot == EQUIPMENT_SLOT_MAINHAND && canDualWield &&
                ((invType != INVTYPE_2HWEAPON && !have2HWeaponEquipped) || (canTitanGrip && isValidTGWeapon))))
        {
            // Handle ring/trinket dual-slot logic
            Item* const equippedItems[2] = {
                bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot),
                bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot + 1)
            };

            if (equippedItems[0])
            {
                if (equippedItems[1])
                {
                    // Both slots are full - pick the worst item to replace, but only if new item is better
                    StatsWeightCalculator calc(bot);
                    calc.SetItemSetBonus(false);
                    calc.SetOverflowPenalty(false);

                    // Calculate new item score with random properties
                    int32 newItemRandomProp = item->GetItemRandomPropertyId();
                    float newItemScore = calc.CalculateItem(itemId, newItemRandomProp);

                    // Calculate equipped items scores with random properties
                    int32 firstRandomProp = equippedItems[0]->GetItemRandomPropertyId();
                    int32 secondRandomProp = equippedItems[1]->GetItemRandomPropertyId();
                    float firstItemScore = calc.CalculateItem(equippedItems[0]->GetTemplate()->ItemId, firstRandomProp);
                    float secondItemScore = calc.CalculateItem(equippedItems[1]->GetTemplate()->ItemId, secondRandomProp);

                    // Determine which slot (if any) should be replaced
                    bool betterThanFirst = newItemScore > firstItemScore;
                    bool betterThanSecond = newItemScore > secondItemScore;

                    // Early return if new item is not better than either equipped item
                    if (!betterThanFirst && !betterThanSecond)
                        return;

                    if (betterThanFirst && betterThanSecond)
                    {
                        // New item is better than both - replace the worse of the two equipped items
                        if (firstItemScore > secondItemScore)
                            dstSlot++; // Replace second slot (worse)
                        // else: keep dstSlot as-is (replace first slot)
                    }
                    else if (betterThanSecond)
                        dstSlot++; // Only better than second slot - replace it
                }
                else
                {
                    // Second slot empty, use it
                    dstSlot++;
                }
            }
        }

        // Equip the item in the chosen slot
        {
            WorldPacket packet(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
            ObjectGuid itemguid = item->GetGUID();
            packet << itemguid << dstSlot;
            WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(packet));
            nicePacket.Read();
            bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);
        }
    }

    std::ostringstream out;
    out << "Equipping " << chat->FormatItem(itemProto);
    botAI->TellMaster(out);
}

ItemIds EquipAction::SelectInventoryItemsToEquip()
{
    CollectItemsVisitor visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);

    ItemIds items;
    for (auto i = visitor.items.begin(); i != visitor.items.end(); ++i)
    {
        Item* item = *i;
        if (!item)
            continue;

        ItemTemplate const* itemTemplate = item->GetTemplate();
        if (!itemTemplate)
            continue;

        //TODO Expand to Glyphs and Gems, that can be placed in equipment
        //Pre-filter non-equipable items
        if (itemTemplate->InventoryType == INVTYPE_NON_EQUIP)
            continue;

        // [guild30] a BoE the global leader gives another member (or has not decided yet)
        if (sGuildCoordinator.IsHeld(bot, item))
            continue;

        int32 randomProperty = item->GetItemRandomPropertyId();
        uint32 itemId = item->GetTemplate()->ItemId;
        std::string itemUsageParam;
        if (randomProperty != 0)
            itemUsageParam = std::to_string(itemId) + "," + std::to_string(randomProperty);
        else
            itemUsageParam = std::to_string(itemId);

        ItemUsage usage = AI_VALUE2(ItemUsage, "item upgrade", itemUsageParam);
        if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_BAD_EQUIP)
            items.insert(itemId);
    }
    return items;
}

bool EquipAction::EquipLeaderUpgrades()
{
    CollectItemsVisitor visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);
    std::vector<Item*> candidates;
    for (Item* item : visitor.items)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP)
            continue;
        // ammo is no gear choice (no slot, no history): the bot's own ammo rule, as before
        if (proto->InventoryType == INVTYPE_AMMO)
        {
            ItemUsage const usage = AI_VALUE2(ItemUsage, "item upgrade", std::to_string(proto->ItemId));
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_BAD_EQUIP)
                EquipItem(item);
            continue;
        }
        candidates.push_back(item);
    }

    // one equip per pass, the biggest gain first: each equip changes what the rest are compared with (two rings,
    // a two-hander against a one-hander)
    bool equipped = false;
    while (!candidates.empty())
    {
        Item* best = nullptr;
        uint8 bestSlot = NULL_SLOT;
        float bestGain = -1.0f;
        std::string bestDecision;
        for (Item* item : candidates)
        {
            uint8 slot = NULL_SLOT;
            float gain = 0.0f;
            std::string decision;
            if (GuildGear::ShouldEquip(bot, item, slot, gain, decision) && gain > bestGain)
            {
                best = item;
                bestSlot = slot;
                bestGain = gain;
                bestDecision = std::move(decision);
            }
        }
        if (!best)
            break;
        candidates.erase(std::find(candidates.begin(), candidates.end(), best));
        // the leader's decision behind the equip (a pending BUY / QUEST_REWARD / transfer reason is kept)
        GuildGear::HistoryNoteDecisionIfNone(bot->GetGUID().GetCounter(), best->GetEntry(), "leader", bestDecision);
        if (bestSlot == NULL_SLOT)
        {
            EquipItem(best);  // a bag or quiver: the bag slot logic
        }
        else
        {
            // [loot15] the leader's pick must pass the core: a refusal here is a leader bug (its judge missed a limit)
            uint16 dest = 0;
            if (InventoryResult const refused = bot->CanEquipItem(bestSlot, dest, best, true);
                refused != EQUIP_ERR_OK)
            {
                LOG_INFO("playerbots", "[loot15] {} L{}: leader picked an item the bot cannot equip: {} slot {}: "
                         "inventory result {}", bot->GetName(), bot->GetLevel(), best->GetTemplate()->Name1,
                         uint32(bestSlot), uint32(refused));
                continue;
            }
            WorldPacket packet(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
            packet << best->GetGUID() << bestSlot;
            WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(packet));
            nicePacket.Read();
            bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);
            std::ostringstream out;
            out << "Equipping " << chat->FormatItem(best->GetTemplate());
            botAI->TellMaster(out);
        }
        equipped = true;
    }
    return equipped;
}

bool EquipUpgradesPacketAction::Execute(Event event)
{
    if (!sPlayerbotAIConfig.AutoEquipUpgradeLoot && !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;
    std::string const source = event.GetSource();
    if (source == "trade status")
    {
        WorldPacket p(event.getPacket());
        p.rpos(0);
        uint32 status;
        p >> status;

        if (status != TRADE_STATUS_TRADE_ACCEPT)
            return false;
    }

    else if (source == "item push result")
    {
        WorldPacket p(event.getPacket());
        p.rpos(0);
        ObjectGuid playerGuid;
        uint32 received, created, sendChatMessage, itemSlot, itemId;
        uint8 bagSlot;

        p >> playerGuid;
        p >> received;
        p >> created;
        p >> sendChatMessage;
        p >> bagSlot;
        p >> itemSlot;
        p >> itemId;

        ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemId);
        if (item->InventoryType == INVTYPE_NON_EQUIP)
            return false;
    }

    if (GuildHonest::IsHonestBot(bot))
    {
        EquipLeaderUpgrades();
        return true;
    }
    ItemIds items = SelectInventoryItemsToEquip();
    EquipItems(items);
    return true;
}

bool EquipUpgradeAction::Execute(Event /*event*/)
{
    if (GuildHonest::IsHonestBot(bot))
    {
        EquipLeaderUpgrades();
        return true;
    }
    ItemIds items = SelectInventoryItemsToEquip();
    EquipItems(items);
    return true;
}
