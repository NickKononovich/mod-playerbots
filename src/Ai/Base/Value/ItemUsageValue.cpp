/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemUsageValue.h"
#include "AiFactory.h"
#include "ChatHelper.h"
#include "GuildCoordinator.h"  // [vendor13] the guild's keep-list
#include "GuildTaskMgr.h"
#include "Item.h"
#include "LootObjectStack.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "QuestAction.h"
#include "RandomItemMgr.h"
#include "ServerFacade.h"
#include "StatsWeightCalculator.h"

#include <unordered_set>

static bool IsRoguePoison(ItemTemplate const* proto);

ItemUsage ItemUsageValue::Calculate()
{
    ParsedItemUsage const parsed = GetItemIdFromQualifier();
    uint32 itemId = parsed.itemId;
    uint32 randomPropertyId = parsed.randomPropertyId;
    if (!itemId)
        return ITEM_USAGE_NONE;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return ITEM_USAGE_NONE;

    if (IsRealPlayer(botAI->GetMaster()))
    {
        if (IsItemUsefulForSkill(proto) || IsItemNeededForSkill(proto))
            return ITEM_USAGE_SKILL;

        if ((proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_MISC ||
             proto->Class == ITEM_CLASS_REAGENT) &&
            IsItemNeededForClassSpell(proto))
            return ITEM_USAGE_KEEP;
    }
    else
    {
        bool needItem = false;

        // [vendor13] econ2: a guild member keeps a skill reagent only for its own called skill-ups (or a quest in its
        // log) or the guild's wanted list - "a reagent of some skill it has" alone goes to the sale (b353: bags full
        // of materials no call of its uses). A bot outside the guild plan keeps its old rule
        if (sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter()))
        {
            if (IsItemNeededForSkill(proto) &&
                (sGuildCoordinator.NeedsForOwnCalls(bot, proto->ItemId) || sGuildCoordinator.IsWanted(proto->ItemId)))
                needItem = true;
            // [ench158] a tool of a profession it has (Runed Copper Rod, a Mining Pick: TotemCategory) is no reagent
            // and never on a call's list - it was sold as "no upgrade and not in the wanted list" after its craft
            if (proto->TotemCategory && proto->Class != ITEM_CLASS_WEAPON && IsItemNeededForSkill(proto))
                needItem = true;
            // [prof270] ... and the weapon-class tools: a Fishing Pole (subclass FISHING_POLE: no TotemCategory, the
            // fishing cast needs it in hand), a Mining Pick / Skinning Knife (TotemCategory 165 / 166). b429 Shalaeth
            // (a guild fisher, Fishing 225) sold its pole at Gubber Blump right after its fishing ([fish14] put it
            // away), then gave its fishing task back "no Fishing Pole"; b426-b429: 47 members bought 113 Mining Picks
            // and 52 Skinning Knives (Pherua 5 picks) - each sold again after its use; DB: 0 of 50 members with
            // Fishing hold a pole, Mining 26 of 41 a pick, Skinning 11 of 17 a knife. Kept while held (the leader's
            // tool call buys one: KEEP, never SKILL - an equipped pole is no bag stack), an equip upgrade stays the
            // gear plan's
            if (proto->Class == ITEM_CLASS_WEAPON && bot->GetItemCount(proto->ItemId, true) &&
                ((proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE && bot->HasSkill(SKILL_FISHING)) ||
                 (proto->TotemCategory && IsItemNeededForSkill(proto))))
            {
                ItemUsage const equip = QueryItemUsageForEquip(proto, randomPropertyId);
                if (equip == ITEM_USAGE_EQUIP || equip == ITEM_USAGE_REPLACE)
                    return equip;
                return ITEM_USAGE_KEEP;
            }
        }
        else if (IsItemNeededForSkill(proto))
            needItem = true;
        else
        {
            bool lowBagSpace = AI_VALUE(uint8, "bag space") > 50;

            if (proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_MISC ||
                proto->Class == ITEM_CLASS_REAGENT)
                needItem = IsItemNeededForUsefullSpell(proto, lowBagSpace);
            else if (proto->Class == ITEM_CLASS_RECIPE)
            {
                if (bot->HasSpell(proto->Spells[2].SpellId))
                    needItem = false;
                else
                    needItem = bot->CanUseItem(proto) == EQUIP_ERR_OK;
            }
        }

        if (needItem)
        {
            float stacks = CurrentStacks(proto);
            if (stacks < 1)
                return ITEM_USAGE_SKILL;  // Buy more.
            if (stacks <= 2)
                return ITEM_USAGE_KEEP;
        }
    }

    if (proto->Class == ITEM_CLASS_KEY)
        return ITEM_USAGE_USE;

    const uint32_t maxCount = proto->MaxCount;

    if (proto->Class == ITEM_CLASS_CONSUMABLE &&
        (maxCount == 0 || bot->GetItemCount(itemId, false) < maxCount))
    {
        std::string const foodType = GetConsumableType(proto, bot->GetPower(POWER_MANA));

        if (!foodType.empty() && bot->CanUseItem(proto) == EQUIP_ERR_OK)
        {
            float stacks = BetterStacks(proto, foodType);
            if (stacks < 2)
            {
                stacks += CurrentStacks(proto);

                if (stacks < 2)
                    return ITEM_USAGE_USE;  // Buy some to get to 2 stacks
                else if (stacks < 3)        // Keep the item if less than 3 stacks
                    return ITEM_USAGE_KEEP;
            }
        }
    }

    if (bot->GetGuildId() && GuildTaskMgr::instance().IsGuildTaskItem(itemId, bot->GetGuildId()))
        return ITEM_USAGE_GUILD_TASK;

    // Starters the bot can take stay in the bags and are used from there; the core deletes the
    // starting item on accept unless the quest also requires it, so they are never equipped.
    if (IsStartQuestItemUsefulFor(bot, proto))
        return ITEM_USAGE_QUEST;

    ItemUsage equip = QueryItemUsageForEquip(proto, randomPropertyId);
    if (equip != ITEM_USAGE_NONE)
        return equip;

    // Get item instance to check if it's soulbound
    Item* item = bot->GetItemByEntry(proto->ItemId);
    bool isSoulbound = item && item->IsSoulBound();

    if ((proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON) &&
        botAI->HasSkill(SKILL_ENCHANTING) &&
        proto->Quality >= ITEM_QUALITY_UNCOMMON)
    {
        // Retrieve the bot's Enchanting skill level
        uint32 enchantingSkill = bot->GetSkillValue(SKILL_ENCHANTING);

        // Only disenchant if skilled enough and binding allows it
        if (enchantingSkill >= proto->RequiredDisenchantSkill &&
            (proto->Bonding == BIND_WHEN_PICKED_UP || (proto->Bonding == BIND_WHEN_EQUIPPED && isSoulbound)))
            return ITEM_USAGE_DISENCHANT;
    }

    Player* master = botAI->GetMaster();
    bool botNeedsItemForQuest = IsItemUsefulForQuest(bot, proto);
    bool masterNeedsItemForQuest = master && sPlayerbotAIConfig.SyncQuestWithPlayer && IsItemUsefulForQuest(master, proto);

    // Identify the source of loot
    LootObject lootObject = AI_VALUE(LootObject, "loot target");

    // Get GUID of loot source
    ObjectGuid lootGuid = lootObject.guid;

    // Check if loot source is an item
    bool isLootFromItem = lootGuid.IsItem();

    // If the loot is from an item in the bot’s bags, ignore syncQuestWithPlayer
    if (isLootFromItem && botNeedsItemForQuest)
        return ITEM_USAGE_QUEST;

    // If this is not a SelfBot acting alone and the master needs this quest item, defer to the master
    if (!IsSelfBot(bot) && masterNeedsItemForQuest)
        return ITEM_USAGE_NONE;

    // If the bot itself needs the item for a quest, allow looting
    if (botNeedsItemForQuest)
        return ITEM_USAGE_QUEST;

    // If the bot has the exact quest requirement amount, keep it to prevent a sell/buy cycle
    for (auto const& questPair : botAI->GetCurrentQuestsRequiringItemId(proto->ItemId))
    {
        Quest const* quest = questPair.first;
        if (!quest)
            continue;

        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
        {
            if (quest->RequiredItemId[i] != proto->ItemId)
                continue;

            uint32 currentCount = AI_VALUE2(uint32, "item count", proto->Name1);
            uint32 requiredCount = quest->RequiredItemCount[i];

            if (currentCount == requiredCount)
                return ITEM_USAGE_KEEP;  // Have exact amount needed, keep it
        }
    }

    if (proto->Class == ITEM_CLASS_PROJECTILE && bot->CanUseItem(proto) == EQUIP_ERR_OK)
    {
        ItemUsage ammoUsage = QueryItemUsageForAmmo(proto);
        if (ammoUsage != ITEM_USAGE_NONE)
            return ammoUsage;
    }

    // Keep only the best stone/oil per weapon and the best poison per family; obsolete or
    // mismatched enhancements fall through to VENDOR/AH.
    if (proto->Class == ITEM_CLASS_CONSUMABLE && IsTemporaryWeaponEnchantment(proto) &&
        bot->CanUseItem(proto) == EQUIP_ERR_OK)
    {
        // Rogue poisons only: stones/oils share consumable subclass 8 but use family-less spells,
        // so they take the weapon branch below instead of being sold as obsolete poisons.
        if (IsRoguePoison(proto))
        {
            if (IsBestPoison(proto))
                return ITEM_USAGE_KEEP;
        }
        else
        {
            Item* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            Item* offHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);

            bool isBest = false;
            if (mainHand)
            {
                if (Item* stone = botAI->FindStoneFor(mainHand))
                    isBest = stone->GetEntry() == proto->ItemId;
                if (!isBest)
                    if (Item* oil = botAI->FindOilFor(mainHand))
                        isBest = oil->GetEntry() == proto->ItemId;
            }
            if (!isBest && offHand)
            {
                if (Item* stone = botAI->FindStoneFor(offHand))
                    isBest = stone->GetEntry() == proto->ItemId;
                if (!isBest)
                    if (Item* oil = botAI->FindOilFor(offHand))
                        isBest = oil->GetEntry() == proto->ItemId;
            }

            if (isBest)
                return CurrentStacks(proto) < 1 ? ITEM_USAGE_USE : ITEM_USAGE_KEEP;
        }
    }

    // Need to add something like free bagspace or item value.
    if (proto->SellPrice > 0)
    {
        if (proto->Quality >= ITEM_QUALITY_NORMAL && !isSoulbound && proto->Bonding != BIND_WHEN_PICKED_UP)
            return ITEM_USAGE_AH;
        else
            return ITEM_USAGE_VENDOR;
    }

    return ITEM_USAGE_NONE;
}

ItemUsage ItemUsageValue::QueryItemUsageForEquip(ItemTemplate const* itemProto, int32 randomPropertyId)
{
    if (bot->CanUseItem(itemProto) != EQUIP_ERR_OK)
        return ITEM_USAGE_NONE;

    if (itemProto->InventoryType == INVTYPE_NON_EQUIP)
        return ITEM_USAGE_NONE;

    Item* pItem = Item::CreateItem(itemProto->ItemId, 1, bot);
    if (!pItem)
        return ITEM_USAGE_NONE;

    uint16 dest;
    InventoryResult result = botAI->CanEquipItem(NULL_SLOT, dest, pItem, true, true);
    pItem->RemoveFromUpdateQueueOf(bot);
    delete pItem;

    if (result != EQUIP_ERR_OK && result != EQUIP_ERR_CANT_CARRY_MORE_OF_THIS)
        return ITEM_USAGE_NONE;

    // Check if unique items are equipped or not
    bool needToCheckUnique = result == EQUIP_ERR_CANT_CARRY_MORE_OF_THIS ||
         itemProto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE);

    if (needToCheckUnique)
    {
        // Count the total number of the item (equipped + in bags)
        uint32 totalItemCount = bot->GetItemCount(itemProto->ItemId, true);

        // Count the number of the item in bags only
        uint32 bagItemCount = bot->GetItemCount(itemProto->ItemId, false);

        // Determine if the unique item is already equipped
        bool isEquipped = (totalItemCount > bagItemCount);

        if (isEquipped)
            return ITEM_USAGE_NONE;  // Item is already equipped
        // If not equipped, continue processing
    }

    if (itemProto->Class == ITEM_CLASS_QUIVER && bot->getClass() != CLASS_HUNTER)
        return ITEM_USAGE_NONE;

    if (itemProto->Class == ITEM_CLASS_CONTAINER)
    {
        if (itemProto->SubClass != ITEM_SUBCLASS_CONTAINER)
            return ITEM_USAGE_NONE;  // Todo add logic for non-bag containers. We want to look at professions/class and
                                     // only replace if non-bag is larger than bag.
        if (GetSmallestBagSize() >= itemProto->ContainerSlots)
            return ITEM_USAGE_NONE;

        return ITEM_USAGE_EQUIP;
    }

    if (itemProto->Class == ITEM_CLASS_WEAPON && itemProto->SubClass == ITEM_SUBCLASS_WEAPON_MISC)
        return ITEM_USAGE_NONE;

    bool shouldEquip = false;
    // uint32 statWeight = sRandomItemMgr.GetLiveStatWeight(bot, itemProto->ItemId);
    StatsWeightCalculator calculator(bot);
    calculator.SetItemSetBonus(false);
    calculator.SetOverflowPenalty(false);

    // Apply PvP weights if the bot is specced for PvP
    bool isPvp = sRandomPlayerbotMgr.IsSpecPvp(bot->GetGUID().GetCounter(), bot->getClass());
    if (isPvp)
        calculator.SetPvpSpec(true);

    float itemScore = calculator.CalculateItem(itemProto->ItemId, randomPropertyId);

    if (itemScore)
        shouldEquip = true;

    bool canWear = true;  // [guild12] the class may wear it at all (the tie-breaks below need it)
    // [loot11] a guild member wears what the guild's judge picks (GuildGear::EvaluateFor): no armor-type veto
    bool const guildJudge = sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter());
    if (itemProto->Class == ITEM_CLASS_WEAPON && !sRandomItemMgr.CanEquipWeapon(itemProto, bot->getClass()))
        canWear = false;
    if (itemProto->Class == ITEM_CLASS_ARMOR &&
        !sRandomItemMgr.CanEquipArmor(itemProto, bot->getClass(), bot->GetLevel(), guildJudge))
        canWear = false;
    if (!canWear)
        shouldEquip = false;

    uint8 possibleSlots = 1;
    uint8 dstSlot = botAI->FindEquipSlot(itemProto, NULL_SLOT, true);
    // Check if dest wasn't set correctly by CanEquipItem and use FindEquipSlot instead
    // This occurs with unique items that are already in the bots bags when CanEquipItem is called
    if (dest == 0 && dstSlot != NULL_SLOT)
    {
        // Construct dest from dstSlot
        dest = (INVENTORY_SLOT_BAG_0 << 8) | dstSlot;
    }

    if (dstSlot == EQUIPMENT_SLOT_FINGER1 || dstSlot == EQUIPMENT_SLOT_TRINKET1)
        possibleSlots = 2;

    // Check weapon case separately to keep things a bit cleaner
    bool have2HWeapon = false;
    bool isValidTGWeapon = false;

    if (dstSlot == EQUIPMENT_SLOT_MAINHAND)
    {
        Item* currentWeapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        have2HWeapon = currentWeapon && currentWeapon->GetTemplate()->InventoryType == INVTYPE_2HWEAPON;

        // Determine if the new weapon is a valid Titan Grip weapon
        isValidTGWeapon = (itemProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                           itemProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                           itemProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2);

        // If the bot can Titan Grip, ignore any 2H weapon that isn't a 2H sword, mace, or axe.
        // If this weapon is 2H but not one of the valid TG weapon types, do not equip it at all.
        if (bot->CanTitanGrip() && itemProto->InventoryType == INVTYPE_2HWEAPON && !isValidTGWeapon)
            return ITEM_USAGE_NONE;

        // Now handle the logic for equipping and possible offhand slots
        // If the bot can Dual Wield and:
        // - The weapon is not 2H and we currently don't have a 2H weapon equipped
        // OR
        // - The bot can Titan Grip and it is a valid TG weapon
        // Then we can consider the offhand slot as well.
        if (bot->CanDualWield() &&
            ((itemProto->InventoryType != INVTYPE_2HWEAPON && !have2HWeapon) ||
             (bot->CanTitanGrip() && isValidTGWeapon)))
        {
            possibleSlots = 2;
        }
    }

    for (uint8 i = 0; i < possibleSlots; i++)
    {
        bool shouldEquipInSlot = shouldEquip;
        Item* oldItem = bot->GetItemByPos(dest + i);

        // No item equipped
        if (!oldItem)
        {
            // [vendor13] an empty slot: anything the item adds over nothing is an upgrade - a weight score, armor, a
            // weapon the class uses; else it is sold (before: BAD_EQUIP, kept in the bags, neither worn nor sold). A
            // copy the guild holds (a member's upgrade, wanted material) stays in the bags for the global leader
            if (Item* copy = bot->GetItemByEntry(itemProto->ItemId); copy && sGuildCoordinator.IsHeld(bot, copy))
                return ITEM_USAGE_KEEP;
            if (shouldEquipInSlot ||
                (canWear && (itemProto->Armor > 0 || itemProto->Class == ITEM_CLASS_WEAPON)))
                return ITEM_USAGE_EQUIP;
            return ITEM_USAGE_VENDOR;
        }

        ItemTemplate const* oldItemProto = oldItem->GetTemplate();
        float oldScore = calculator.CalculateItem(oldItemProto->ItemId, oldItem->GetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID));
        if (oldItem)
        {
            // uint32 oldStatWeight = sRandomItemMgr.GetLiveStatWeight(bot, oldItemProto->ItemId);
            // [loot11] never over a piece the class should not wear (b456-b467: cloth Serpent Gloves were a NEED
            // "REPLACE" for shamans and paladins while the guild's equip refused them: 9 copies held unworn)
            if (itemScore || oldScore)
                shouldEquipInSlot = canWear && itemScore > oldScore * sPlayerbotAIConfig.EquipUpgradeThreshold;
        }

        // Bigger quiver
        if (itemProto->Class == ITEM_CLASS_QUIVER)
        {
            if (!oldItem || oldItemProto->ContainerSlots < itemProto->ContainerSlots)
                return ITEM_USAGE_EQUIP;
            else
                return ITEM_USAGE_NONE;
        }

        bool existingShouldEquip = true;
        if (oldItemProto->Class == ITEM_CLASS_WEAPON && !sRandomItemMgr.CanEquipWeapon(oldItemProto, bot->getClass()))
            existingShouldEquip = false;

        if (oldItemProto->Class == ITEM_CLASS_ARMOR &&
            !sRandomItemMgr.CanEquipArmor(oldItemProto, bot->getClass(), bot->GetLevel(), guildJudge))
            existingShouldEquip = false;

        // uint32 oldItemPower = sRandomItemMgr.GetLiveStatWeight(bot, oldItemProto->ItemId);
        // uint32 newItemPower = sRandomItemMgr.GetLiveStatWeight(bot, itemProto->ItemId);

        // Compare items based on item level, quality or itemId.
        bool isBetter = false;
        if (itemScore > oldScore)
            isBetter = true;
        // [guild12] equal weight (low-level gear without stats scores the same, often 0): as a player picks - more
        // armor, then the higher item level, then the better quality (Eraelor kept Ragged Leather Belt 18 armor on
        // with Blackened Leather Belt 19 armor in the bags)
        else if (itemScore == oldScore && canWear && itemProto->Class == oldItemProto->Class &&
                 (itemProto->Class != ITEM_CLASS_ARMOR || itemProto->SubClass >= oldItemProto->SubClass))
        {
            if (itemProto->Armor != oldItemProto->Armor)
                isBetter = itemProto->Armor > oldItemProto->Armor;
            else if (itemProto->ItemLevel != oldItemProto->ItemLevel)
                isBetter = itemProto->ItemLevel > oldItemProto->ItemLevel;
            else
                isBetter = itemProto->Quality > oldItemProto->Quality;
            if (isBetter)
                shouldEquipInSlot = true;
        }
        // else if (newItemPower == oldScore && itemProto->Quality > oldItemProto->Quality)
        //     isBetter = true;
        // else if (newItemPower == oldScore && itemProto->Quality == oldItemProto->Quality && itemProto->ItemId >
        // oldItemProto->ItemId)
        //     isBetter = true;

        Item* item = CurrentItem(itemProto);
        bool itemIsBroken =
            item && item->GetUInt32Value(ITEM_FIELD_DURABILITY) == 0 && item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) > 0;
        bool oldItemIsBroken =
            oldItem->GetUInt32Value(ITEM_FIELD_DURABILITY) == 0 && oldItem->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) > 0;

        if (itemProto->ItemId != oldItemProto->ItemId && (shouldEquipInSlot || !existingShouldEquip) && isBetter)
        {
            switch (itemProto->Class)
            {
                case ITEM_CLASS_ARMOR:
                    if (oldItemProto->SubClass <= itemProto->SubClass)
                    {
                        // Need to add some logic to check second slot before returning, but as it happens, all three of these
                        // return vals will result in an attempted equip action so it wouldn't have much effect currently
                        if (itemIsBroken && !oldItemIsBroken)
                            return ITEM_USAGE_BROKEN_EQUIP;
                        else if (shouldEquipInSlot)
                            return ITEM_USAGE_REPLACE;
                        else
                            return ITEM_USAGE_BAD_EQUIP;
                    }
                    // A lower-subclass armor piece is handled by the default case below.
                    [[fallthrough]];
                default:
                {
                    if (itemIsBroken && !oldItemIsBroken)
                        return ITEM_USAGE_BROKEN_EQUIP;
                    else if (shouldEquipInSlot)
                        return ITEM_USAGE_EQUIP;
                    else
                        return ITEM_USAGE_BAD_EQUIP;
                }
            }
        }

        // Item is not better but current item is broken and new one is not.
        if (oldItemIsBroken && !itemIsBroken)
            return ITEM_USAGE_EQUIP;
    }
    return ITEM_USAGE_NONE;
}

// [econ71] what the bot's EQUIPPED ranged weapon fires. A thrown weapon is its own ammo in 3.3.5 and fires none, an
// empty ranged slot fires none, and a class with no ranged attack holds none: all three answer 0. This is the same
// map the core uses in Spell::CheckItems and that PullStrategy ([ammo84]), HunterTriggers ([rot608]), the vendor run
// (RequiredAmmoSubClass) and FindAmmoVisitor each carry their own copy of - the copies agree, so this one is the
// judge the acquire and disposal sides share.
uint32 ItemUsageValue::AmmoSubClassFor(Player* bot)
{
    if (!bot)
        return 0;
    Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    ItemTemplate const* proto = ranged ? ranged->GetTemplate() : nullptr;
    if (!proto || proto->Class != ITEM_CLASS_WEAPON)
        return 0;
    switch (proto->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_GUN:
            return ITEM_SUBCLASS_BULLET;
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return ITEM_SUBCLASS_ARROW;
        default:  // a thrown weapon needs no ammo, and nothing else in the ranged slot fires any
            return 0;
    }
}

// [econ71] the rounds of that subclass it holds, bags and inventory slots alike. Only that subclass: a bow user's
// bullets are not ammo, they are dead weight, and counting them as stock is what let 43 633 bullets sit in bags that
// then had no room (guild 21, 2026-11-13: 13 holders, not one gun in the guild).
uint32 ItemUsageValue::AmmoHeldRounds(Player* bot, uint32 subClass)
{
    if (!bot || !subClass)
        return 0;
    uint32 rounds = 0;
    auto const count = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (proto && proto->Class == ITEM_CLASS_PROJECTILE && proto->SubClass == subClass)
            rounds += item->GetCount();
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        count(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* container = bot->GetBagByPos(bag))
            for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                count(container->GetItemByPos(uint8(slot)));
    return rounds;
}

// [econ71] USER DECISION 2026-11-13 («посмотреть расход и примерно оценивать, сколько нужно, запас x2 или x3, не
// нужно носить с собой триллион этих снарядов»): how many rounds a bot carries is DERIVED, never chosen. The old
// rule was "8 stacks for a hunter, 2 for anyone else" (QueryItemUsageForAmmo), and an ammo stack is 1000, so a
// hunter's target was 8000 rounds - which is exactly what the guild held (Ainne 8091, Feette 7976, Thokhen 6992).
//
// The derivation, all three inputs facts and all three logged by the caller that acts on it:
//   * one round per ranged swing - the core spends exactly one (Spell::TakeAmmo), so the shots an activity costs are
//     its seconds divided by the weapon's own swing time, bot->GetAttackTime(RANGED_ATTACK), the same interval
//     DpsEstimate uses as a hunter's lead;
//   * the activity it must cover without reaching a vendor is one run - the project's own run length, 20 minutes
//     (USER RULE "test run length": a verification run is <= 20 min, and a five runs on until the dungeon has a
//     result, which is the same order); the bot restocks at a vendor between runs (VENDOR_LOW_AMMO);
//   * the reserve the user approved, x3 of that.
// Rounded up to a full stack, because a vendor sells and a bag holds whole stacks. For a 2.8 s bow that is
// 1200 / 2.8 = 429 shots, x3 = 1286, one stack up = 2000 rounds; for a 1.5 s gun 2400 shots, x3 = 7200 -> 8000.
// 0 when its ranged weapon fires nothing: a thrown user carries no ammo at all, which is the other 15 575 pieces.
uint32 ItemUsageValue::AmmoCapRounds(Player* bot, ItemTemplate const* ammoProto)
{
    uint32 const subClass = AmmoSubClassFor(bot);
    if (!subClass || !ammoProto || ammoProto->SubClass != subClass)
        return 0;
    constexpr float RUN_SECONDS = 20.0f * MINUTE;  // the project's own run length, see above
    constexpr float RESERVE = 3.0f;                // the reserve the user approved ("запас x2 или x3")
    float const swingSec = std::max(float(bot->GetAttackTime(RANGED_ATTACK)) / float(IN_MILLISECONDS), 0.1f);
    uint32 const rounds = uint32(std::ceil(RUN_SECONDS / swingSec * RESERVE));
    uint32 const stack = std::max<uint32>(ammoProto->GetMaxStackSize(), 1);
    return ((rounds + stack - 1) / stack) * stack;
}

ItemUsage ItemUsageValue::QueryItemUsageForAmmo(ItemTemplate const* proto)
{
    if (bot->getClass() != CLASS_HUNTER && bot->getClass() != CLASS_ROGUE && bot->getClass() != CLASS_WARRIOR)
        return ITEM_USAGE_NONE;

    // [econ71] the ammo its equipped ranged weapon fires, and nothing else. A thrown user and an empty ranged slot
    // answer 0, so neither has any use for a round.
    uint32 const requiredSubClass = AmmoSubClassFor(bot);
    if (!requiredSubClass || proto->SubClass != requiredSubClass)
        return ITEM_USAGE_NONE;

    uint32 currentAmmoId = bot->GetUInt32Value(PLAYER_AMMO_ID);

    // Check if the bot has an ammo type assigned
    if (currentAmmoId == 0)
        return ITEM_USAGE_EQUIP;  // Equip the ammo if no ammo
    // Compare new ammo vs current equipped ammo
    ItemTemplate const* currentAmmoProto = sObjectMgr->GetItemTemplate(currentAmmoId);
    if (currentAmmoProto)
    {
        uint32 currentAmmoDPS =
            (currentAmmoProto->Damage[0].DamageMin + currentAmmoProto->Damage[0].DamageMax) * 1000 / 2;
        uint32 newAmmoDPS = (proto->Damage[0].DamageMin + proto->Damage[0].DamageMax) * 1000 / 2;

        if (newAmmoDPS > currentAmmoDPS) // New ammo meets upgrade condition
            return ITEM_USAGE_EQUIP;

        if (newAmmoDPS < currentAmmoDPS) // New ammo is worse
            return ITEM_USAGE_NONE;
    }

    // [econ71] the derived load, in rounds, against the rounds it holds of that subclass. The old test was in
    // STACKS of the same item ("8 stacks for a hunter") and so had no relation to what the bot shoots.
    uint32 const cap = AmmoCapRounds(bot, proto);
    uint32 const held = AmmoHeldRounds(bot, requiredSubClass);
    if (held >= cap)
        return ITEM_USAGE_NONE;  // it carries its full load: nothing to buy, nothing to loot
    // [econ71] it buys when it holds less than ONE run's worth - the cap is one run x the reserve of 3, so a third
    // of the cap is that run - and otherwise keeps what it has rather than topping up at every vendor it passes.
    // No new number: the same two inputs the cap is derived from say both lines.
    return held * 3 < cap ? ITEM_USAGE_AMMO : ITEM_USAGE_KEEP;
}

ParsedItemUsage ItemUsageValue::GetItemIdFromQualifier()
{
    ParsedItemUsage parsed;

    size_t const pos = qualifier.find(",");
    if (pos != std::string::npos)
    {
        parsed.itemId = atoi(qualifier.substr(0, pos).c_str());
        parsed.randomPropertyId = atoi(qualifier.substr(pos + 1).c_str());
        return parsed;
    }
    else
        parsed.itemId = atoi(qualifier.c_str());
    return parsed;
}
// Return smaltest bag size equipped
// [inv375] of the generic bags in all four bag slots (0: a free slot): the first slot was skipped, and a quiver or a
// special bag counted as the bag a generic one replaces (EquipAction::EquipBag never replaces those)
uint32 ItemUsageValue::GetSmallestBagSize()
{
    uint32 curSlots = 0;
    bool generic = false;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Bag const* pBag = bot->GetBagByPos(bag);
        if (!pBag)
            return 0;
        ItemTemplate const* proto = pBag->GetTemplate();
        if (proto->Class != ITEM_CLASS_CONTAINER || proto->SubClass != ITEM_SUBCLASS_CONTAINER)
            continue;
        if (!generic || pBag->GetBagSize() < curSlots)
            curSlots = pBag->GetBagSize();
        generic = true;
    }
    return generic ? curSlots : UINT32_MAX;  // only special bags worn: nothing a generic bag can replace
}

bool ItemUsageValue::IsItemUsefulForQuest(Player* player, ItemTemplate const* proto)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
        return false;

    // Core loot gating predicate: also covers quest source items (ItemDrop,
    // e.g. quest keys). The local objective check below is kept because this
    // predicate skips non-raid quests for raid groups.
    if (player->HasQuestForItem(proto->ItemId))
        return true;

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 entry = player->GetQuestSlotQuestId(slot);
        Quest const* quest = sObjectMgr->GetQuestTemplate(entry);
        if (!quest)
            continue;

        // Check if the item itself is needed for the quest
        for (uint8 i = 0; i < 4; i++)
        {
            if (quest->RequiredItemId[i] == proto->ItemId)
            {
                if (player->GetItemCount(proto->ItemId, false) >= quest->RequiredItemCount[i])
                    continue;

                return true; // Item is directly required for a quest
            }
        }

        // Check if the item has spells that create a required quest item
        for (uint8 i = 0; i < MAX_ITEM_SPELLS; i++)
        {
            uint32 spellId = proto->Spells[i].SpellId;
            if (!spellId)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo)
                continue;

            for (uint8 effectIndex = 0; effectIndex < MAX_SPELL_EFFECTS; effectIndex++)
            {
                if (spellInfo->Effects[effectIndex].Effect == SPELL_EFFECT_CREATE_ITEM)
                {
                    uint32 createdItemId = spellInfo->Effects[effectIndex].ItemType;

                    // Check if the created item is required for a quest
                    for (uint8 j = 0; j < 4; j++)
                    {
                        if (quest->RequiredItemId[j] == createdItemId)
                        {
                            if (player->GetItemCount(createdItemId, false) >= quest->RequiredItemCount[j])
                                continue;

                            return true; // Item is useful because it creates a required quest item
                        }
                    }
                }
            }
        }
    }

    return false; // Item is not useful for any active quests
}

bool ItemUsageValue::IsStartQuestItemUsefulFor(Player* player, ItemTemplate const* proto)
{
    if (!proto->StartQuest)
        return false;

    Quest const* quest = sObjectMgr->GetQuestTemplate(proto->StartQuest);
    return quest && player->GetQuestStatus(proto->StartQuest) == QUEST_STATUS_NONE &&
        QuestAction::CanAcceptQuest(player, quest);
}

bool ItemUsageValue::IsItemNeededForSkill(ItemTemplate const* proto)
{
    switch (proto->ItemId)
    {
        case 756:  // Tunnel Pick
            return botAI->HasSkill(SKILL_MINING);
        case 778:  // Kobold Excavation Pick
            return botAI->HasSkill(SKILL_MINING);
        case 1819:  // Gouging Pick
            return botAI->HasSkill(SKILL_MINING);
        case 1893:  // Miner's Revenge
            return botAI->HasSkill(SKILL_MINING);
        case 1959:  // Cold Iron Pick
            return botAI->HasSkill(SKILL_MINING);
        case 2901:  // Mining Pick
            return botAI->HasSkill(SKILL_MINING);
        case 9465:  // Digmaster 5000
            return botAI->HasSkill(SKILL_MINING);
        case 20723:  // Brann's Trusty Pick
            return botAI->HasSkill(SKILL_MINING);
        case 40772:  // Gnomish Army Knife
            return botAI->HasSkill(SKILL_MINING) || botAI->HasSkill(SKILL_ENGINEERING) || botAI->HasSkill(SKILL_BLACKSMITHING) || botAI->HasSkill(SKILL_COOKING) || botAI->HasSkill(SKILL_SKINNING);
        case 40892:  // Hammer Pick
            return botAI->HasSkill(SKILL_MINING) || botAI->HasSkill(SKILL_BLACKSMITHING);
        case 40893:  // Bladed Pickaxe
            return botAI->HasSkill(SKILL_MINING) || botAI->HasSkill(SKILL_SKINNING);
        case 5956:  // Blacksmith Hammer
            return botAI->HasSkill(SKILL_BLACKSMITHING) || botAI->HasSkill(SKILL_ENGINEERING);
        case 6219:  // Arclight Spanner
            return botAI->HasSkill(SKILL_ENGINEERING);
        case 6218:  // Runed copper rod
            return botAI->HasSkill(SKILL_ENCHANTING);
        case 6339:  // Runed silver rod
            return botAI->HasSkill(SKILL_ENCHANTING);
        case 11130:  // Runed golden rod
            return botAI->HasSkill(SKILL_ENCHANTING);
        case 11145:  // Runed truesilver rod
            return botAI->HasSkill(SKILL_ENCHANTING);
        case 16207:  // Runed Arcanite Rod
            return botAI->HasSkill(SKILL_ENCHANTING);
        case 7005:  // Skinning Knife
            return botAI->HasSkill(SKILL_SKINNING);
        case 12709:
            return botAI->HasSkill(SKILL_SKINNING);
        case 19901:
            return botAI->HasSkill(SKILL_SKINNING);
        case 4471:  // Flint and Tinder
            return botAI->HasSkill(SKILL_COOKING);
        case 4470:  // Simple Wood
            return botAI->HasSkill(SKILL_COOKING);
        case 6256:  // Fishing Rod
            return botAI->HasSkill(SKILL_FISHING);
    }

    return false;
}

bool ItemUsageValue::IsItemUsefulForSkill(ItemTemplate const* proto)
{
    switch (proto->Class)
    {
        case ITEM_CLASS_TRADE_GOODS:
        case ITEM_CLASS_MISC:
        case ITEM_CLASS_REAGENT:
        case ITEM_CLASS_GEM:
        {
            if (botAI->HasSkill(SKILL_TAILORING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_TAILORING))
                return true;
            if (botAI->HasSkill(SKILL_LEATHERWORKING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_LEATHERWORKING))
                return true;
            if (botAI->HasSkill(SKILL_ENGINEERING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_ENGINEERING))
                return true;
            if (botAI->HasSkill(SKILL_BLACKSMITHING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_BLACKSMITHING))
                return true;
            if (botAI->HasSkill(SKILL_ALCHEMY) && RandomItemMgr::IsUsedBySkill(proto, SKILL_ALCHEMY))
                return true;
            if (botAI->HasSkill(SKILL_ENCHANTING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_ENCHANTING))
                return true;
            if (botAI->HasSkill(SKILL_FISHING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_FISHING))
                return true;
            if (botAI->HasSkill(SKILL_FIRST_AID) && RandomItemMgr::IsUsedBySkill(proto, SKILL_FIRST_AID))
                return true;
            if (botAI->HasSkill(SKILL_COOKING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_COOKING))
                return true;
            if (botAI->HasSkill(SKILL_JEWELCRAFTING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_JEWELCRAFTING))
                return true;
            if (botAI->HasSkill(SKILL_MINING) && (RandomItemMgr::IsUsedBySkill(proto, SKILL_MINING) ||
                                                  RandomItemMgr::IsUsedBySkill(proto, SKILL_BLACKSMITHING) ||
                                                  RandomItemMgr::IsUsedBySkill(proto, SKILL_JEWELCRAFTING) ||
                                                  RandomItemMgr::IsUsedBySkill(proto, SKILL_ENGINEERING)))
                return true;
            if (botAI->HasSkill(SKILL_SKINNING) && (RandomItemMgr::IsUsedBySkill(proto, SKILL_SKINNING) ||
                                                    RandomItemMgr::IsUsedBySkill(proto, SKILL_LEATHERWORKING)))
                return true;
            if (botAI->HasSkill(SKILL_HERBALISM) && (RandomItemMgr::IsUsedBySkill(proto, SKILL_HERBALISM) ||
                                                     RandomItemMgr::IsUsedBySkill(proto, SKILL_ALCHEMY)))
                return true;
            // [insc141] a scribe keeps its reagents (pigments, inks, parchment) and the herbs it mills itself (the
            // core's milling check: ITEM_FLAG_IS_MILLABLE, RequiredSkillRank within its Inscription), Herbalism or not
            if (botAI->HasSkill(SKILL_INSCRIPTION) &&
                (RandomItemMgr::IsUsedBySkill(proto, SKILL_INSCRIPTION) ||
                 (proto->HasFlag(ITEM_FLAG_IS_MILLABLE) &&
                  bot->GetPureSkillValue(SKILL_INSCRIPTION) >= proto->RequiredSkillRank)))
                return true;

            return false;
        }
        case ITEM_CLASS_RECIPE:
        {
            if (bot->HasSpell(proto->Spells[2].SpellId))
                break;

            switch (proto->SubClass)
            {
                case ITEM_SUBCLASS_LEATHERWORKING_PATTERN:
                    return botAI->HasSkill(SKILL_LEATHERWORKING);
                case ITEM_SUBCLASS_TAILORING_PATTERN:
                    return botAI->HasSkill(SKILL_TAILORING);
                case ITEM_SUBCLASS_ENGINEERING_SCHEMATIC:
                    return botAI->HasSkill(SKILL_ENGINEERING);
                case ITEM_SUBCLASS_BLACKSMITHING:
                    return botAI->HasSkill(SKILL_BLACKSMITHING);
                case ITEM_SUBCLASS_COOKING_RECIPE:
                    return botAI->HasSkill(SKILL_COOKING);
                case ITEM_SUBCLASS_ALCHEMY_RECIPE:
                    return botAI->HasSkill(SKILL_ALCHEMY);
                case ITEM_SUBCLASS_FIRST_AID_MANUAL:
                    return botAI->HasSkill(SKILL_FIRST_AID);
                case ITEM_SUBCLASS_ENCHANTING_FORMULA:
                    return botAI->HasSkill(SKILL_ENCHANTING);
                case ITEM_SUBCLASS_FISHING_MANUAL:
                    return botAI->HasSkill(SKILL_FISHING);
            }
        }
    }

    return false;
}

namespace
{
    // Crafting chains can be cyclic: Remember which items are already being evaluated
    // further up the stack and stop as soon as a cycle closes.
    thread_local std::unordered_set<uint32> itemsBeingEvaluated;

    class UsefullSpellRecursionGuard
    {
    public:
        explicit UsefullSpellRecursionGuard(uint32 itemId)
            : _itemId(itemId), _entered(itemsBeingEvaluated.insert(itemId).second)
        {
        }

        ~UsefullSpellRecursionGuard()
        {
            if (_entered)
                itemsBeingEvaluated.erase(_itemId);
        }

        UsefullSpellRecursionGuard(UsefullSpellRecursionGuard const&) = delete;
        UsefullSpellRecursionGuard& operator=(UsefullSpellRecursionGuard const&) = delete;

        bool Entered() const { return _entered; }

    private:
        uint32 _itemId;
        bool _entered;
    };
}

bool ItemUsageValue::IsItemNeededForUsefullSpell(ItemTemplate const* proto, bool checkAllReagents)
{
    UsefullSpellRecursionGuard guard(proto->ItemId);
    if (!guard.Entered())
        return false;

    for (auto spellId : SpellsUsingItem(proto->ItemId, bot))
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        if (checkAllReagents && !HasItemsNeededForSpell(spellId, proto))
            continue;

        // Class spells consume reagents without creating items; generic profession spells
        // must not make every trainer recipe's reagent worth hoarding.
        if (spellInfo->SpellFamilyName != SPELLFAMILY_GENERIC)
            return true;

        if (SpellGivesSkillUp(spellId, bot))
            return true;

        uint32 newItemId = spellInfo->Effects[EFFECT_0].ItemType;
        if (newItemId && newItemId != proto->ItemId)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", newItemId);

            if (usage != ITEM_USAGE_REPLACE && usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_AMMO &&
                usage != ITEM_USAGE_QUEST && usage != ITEM_USAGE_SKILL && usage != ITEM_USAGE_USE)
                continue;

            return true;
        }
    }

    return false;
}

bool ItemUsageValue::IsItemNeededForClassSpell(ItemTemplate const* proto)
{
    for (uint32 spellId : SpellsUsingItem(proto->ItemId, bot))
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (spellInfo && spellInfo->SpellFamilyName != SPELLFAMILY_GENERIC)
            return true;
    }

    return false;
}

static SpellInfo const* GetTemporaryEnchantSpell(ItemTemplate const* proto)
{
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        uint32 spellId = proto->Spells[i].SpellId;
        if (!spellId)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        for (uint8 j = 0; j < MAX_SPELL_EFFECTS; ++j)
            if (spellInfo->Effects[j].Effect == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                return spellInfo;
    }

    return nullptr;
}

// True for rogue poisons: their temporary enchant spell is the rogue family and carries a
// per-poison family flag (stones/oils share the subclass but use family-less spells).
static bool IsRoguePoison(ItemTemplate const* proto)
{
    if (proto->SubClass != ITEM_SUBCLASS_CONSUMABLE_OTHER)
        return false;

    SpellInfo const* useSpell = GetTemporaryEnchantSpell(proto);
    return useSpell && useSpell->SpellFamilyName == SPELLFAMILY_ROGUE && !useSpell->SpellFamilyFlags.IsEqual(0, 0, 0);
}

bool ItemUsageValue::IsTemporaryWeaponEnchantment(ItemTemplate const* proto)
{
    return GetTemporaryEnchantSpell(proto) != nullptr;
}

bool ItemUsageValue::IsBestPoison(ItemTemplate const* proto)
{
    // Family comes from the use spell's SpellFamilyFlags (per poison type), not item ids.
    if (!IsRoguePoison(proto))
        return false;

    SpellInfo const* useSpell = GetTemporaryEnchantSpell(proto);

    // Keep only the highest-item-level poison per family (one family per weapon); lower ranks
    // fall through to VENDOR/AH.
    for (Item* item : botAI->GetInventoryItems())
    {
        ItemTemplate const* other = item->GetTemplate();
        if (!other || other->ItemId == proto->ItemId)
            continue;
        if (!IsRoguePoison(other))
            continue;

        // Unusable higher ranks (level or class restriction) must not block a usable lower rank.
        if (bot->CanUseItem(other) != EQUIP_ERR_OK)
            continue;

        SpellInfo const* otherUseSpell = GetTemporaryEnchantSpell(other);
        if (otherUseSpell && otherUseSpell->SpellFamilyFlags == useSpell->SpellFamilyFlags &&
            other->ItemLevel > proto->ItemLevel)
            return false;
    }

    return true;
}

bool ItemUsageValue::HasItemsNeededForSpell(uint32 spellId, ItemTemplate const* proto)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return false;

    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; i++)
    {
        // Same validity check Spell::CheckItems uses: invalid/sentinel reagent ids are skipped.
        if (spellInfo->Reagent[i] <= 0)
            continue;

        if (proto && proto->ItemId == uint32(spellInfo->Reagent[i]) &&
            spellInfo->ReagentCount[i] == 1)  // If we only need 1 item then current item does not need to be
                                              // checked since we are looting/buying or already have it.
            continue;

        ItemTemplate const* reqProto = sObjectMgr->GetItemTemplate(spellInfo->Reagent[i]);
        if (!reqProto)
            continue;

        uint32 count = AI_VALUE2(uint32, "item count", reqProto->Name1);

        if (count < spellInfo->ReagentCount[i])
            return false;
    }

    return true;
}

Item* ItemUsageValue::CurrentItem(ItemTemplate const* proto)
{
    Item* bestItem = nullptr;
    std::vector<Item*> found = AI_VALUE2(std::vector<Item*>, "inventory items", chat->FormatItem(proto));
    for (auto item : found)
    {
        if (bestItem && item->GetUInt32Value(ITEM_FIELD_DURABILITY) < bestItem->GetUInt32Value(ITEM_FIELD_DURABILITY))
            continue;

        if (bestItem && item->GetCount() < bestItem->GetCount())
            continue;

        bestItem = item;
    }

    return bestItem;
}

float ItemUsageValue::CurrentStacks(ItemTemplate const* proto)
{
    uint32 maxStack = proto->GetMaxStackSize();

    std::vector<Item*> found = AI_VALUE2(std::vector<Item*>, "inventory items", chat->FormatItem(proto));

    float itemCount = 0;

    for (auto stack : found)
    {
        itemCount += stack->GetCount();
    }

    return itemCount / maxStack;
}

float ItemUsageValue::BetterStacks(ItemTemplate const* proto, std::string const itemType)
{
    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", itemType);

    float stacks = 0;

    for (auto& otherItem : items)
    {
        ItemTemplate const* otherProto = otherItem->GetTemplate();

        if (otherProto->Class != proto->Class || otherProto->SubClass != proto->SubClass)
            continue;

        if (otherProto->ItemLevel < proto->ItemLevel)
            continue;

        if (otherProto->ItemId == proto->ItemId)
            continue;

        stacks += CurrentStacks(otherProto);
    }

    return stacks;
}

std::vector<uint32> ItemUsageValue::SpellsUsingItem(uint32 itemId, Player* bot)
{
    std::vector<uint32> retSpells;

    PlayerSpellMap const& spellMap = bot->GetSpellMap();

    for (auto& spell : spellMap)
    {
        uint32 spellId = spell.first;

        if (spell.second->State == PLAYERSPELL_REMOVED || !spell.second->Active)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        // Skip only spells the server itself will not charge reagents for (Glyph of Renewed Life);
        // SPELL_ATTR_PASSIVE alone does not mean the reagent is unused.
        if (bot->CanNoReagentCast(spellInfo))
            continue;

        // Profession spells are generic-family; only their crafting spells (CREATE_ITEM) count.
        // Class spells (poisons, oils, water walking, ...) consume reagents without creating items.
        if (spellInfo->Effects[EFFECT_0].Effect != SPELL_EFFECT_CREATE_ITEM &&
            spellInfo->SpellFamilyName == SPELLFAMILY_GENERIC)
            continue;

        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; i++)
            if (spellInfo->ReagentCount[i] > 0 && uint32(spellInfo->Reagent[i]) == itemId)
                retSpells.push_back(spellId);
    }

    return retSpells;
}

inline int32 SkillGainChance(uint32 SkillValue, uint32 GrayLevel, uint32 GreenLevel, uint32 YellowLevel)
{
    if (SkillValue >= GrayLevel)
        return sWorld->getIntConfig(CONFIG_SKILL_CHANCE_GREY) * 10;

    if (SkillValue >= GreenLevel)
        return sWorld->getIntConfig(CONFIG_SKILL_CHANCE_GREEN) * 10;

    if (SkillValue >= YellowLevel)
        return sWorld->getIntConfig(CONFIG_SKILL_CHANCE_YELLOW) * 10;

    return sWorld->getIntConfig(CONFIG_SKILL_CHANCE_ORANGE) * 10;
}

bool ItemUsageValue::SpellGivesSkillUp(uint32 spellId, Player* bot)
{
    SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);

    for (SkillLineAbilityMap::const_iterator _spell_idx = bounds.first; _spell_idx != bounds.second; ++_spell_idx)
    {
        SkillLineAbilityEntry const* skill = _spell_idx->second;
        if (skill->SkillLine)
        {
            uint32 SkillValue = bot->GetPureSkillValue(skill->SkillLine);

            if (SkillGainChance(SkillValue, skill->TrivialSkillLineRankHigh,
                                (skill->TrivialSkillLineRankHigh + skill->TrivialSkillLineRankLow) / 2,
                                skill->TrivialSkillLineRankLow) > 0)
                return true;
        }
    }

    return false;
}

std::string const ItemUsageValue::GetConsumableType(ItemTemplate const* proto, bool hasMana)
{
    std::string const foodType = "";

    if ((proto->SubClass == ITEM_SUBCLASS_CONSUMABLE || proto->SubClass == ITEM_SUBCLASS_FOOD))
    {
        if (proto->Spells[0].SpellCategory == 11)
            return "food";
        else if (proto->Spells[0].SpellCategory == 59 && hasMana)
            return "drink";
    }

    if (proto->SubClass == ITEM_SUBCLASS_POTION || proto->SubClass == ITEM_SUBCLASS_FLASK)
    {
        for (int j = 0; j < MAX_ITEM_PROTO_SPELLS; j++)
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(proto->Spells[j].SpellId);
            if (spellInfo)
                for (int i = 0; i < 3; i++)
                {
                    if (spellInfo->Effects[i].Effect == SPELL_EFFECT_ENERGIZE && hasMana)
                        return "mana potion";

                    if (spellInfo->Effects[i].Effect == SPELL_EFFECT_HEAL)
                        return "healing potion";
                }
        }
    }

    if (proto->SubClass == ITEM_SUBCLASS_BANDAGE)
    {
        return "bandage";
    }

    return "";
}

ItemUsage ItemUpgradeValue::Calculate()
{
    ParsedItemUsage parsed = GetItemIdFromQualifier();
    uint32 itemId = parsed.itemId;
    uint32 randomPropertyId = parsed.randomPropertyId;
    if (!itemId)
        return ITEM_USAGE_NONE;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return ITEM_USAGE_NONE;

    ItemUsage equip = QueryItemUsageForEquip(proto, randomPropertyId);
    if (equip != ITEM_USAGE_NONE)
        return equip;

    if (proto->Class == ITEM_CLASS_PROJECTILE && bot->CanUseItem(proto) == EQUIP_ERR_OK)
        return QueryItemUsageForAmmo(proto);

    return ITEM_USAGE_NONE;
}
