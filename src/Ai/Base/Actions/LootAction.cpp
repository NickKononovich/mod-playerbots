/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LootAction.h"
#include "GuildRiding.h"  // [mount7]
#include "QuestStats.h"  // [log28]
#include "ObjectMgr.h"  // [qeff31]
#include <mutex>          // [qeff31]
#include <unordered_map>  // [qeff31]

#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "Event.h"
#include "GuildCoordinator.h"  // [prof271] the guild's wanted materials
#include "GuildMgr.h"
#include "GuildTaskMgr.h"
#include "ItemUsageValue.h"
#include "LootObjectStack.h"
#include "LootStrategyValue.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "UseItemAction.h"

namespace
{
    // [qeff31] the loot lines: when each bot opened its loot (guid, ms), the quest an item serves, the corpse's name
    std::mutex lootLogLock;
    std::unordered_map<uint32, std::pair<ObjectGuid, uint32>> lootOpened;

    uint32 QuestOfItem(Player* bot, uint32 itemId)
    {
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(bot->GetQuestSlotQuestId(slot)))
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (quest->RequiredItemId[i] == itemId)
                        return quest->GetQuestId();
        return 0;
    }

    std::string LootSourceName(ObjectGuid guid)
    {
        if (guid.IsCreatureOrVehicle())
        {
            CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(guid.GetEntry());
            return Acore::StringFormat("corpse {} {}", guid.GetEntry(), t ? t->Name : "?");
        }
        if (guid.IsGameObject())
        {
            GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(guid.GetEntry());
            return Acore::StringFormat("object {} {}", guid.GetEntry(), t ? t->name : "?");
        }
        return guid.IsItem() ? "item" : "other";
    }

    // [bag66] b406: 20 quest items left on corpses "no bag room" (items 23859, 24043 ...). The item it destroys to
    // make room: of its bags' items (never equipped ones) that are no quest item, no item a quest in its log needs,
    // no container, ammunition or consumable (its food, drink, potions), the grey one of the lowest vendor value, else
    // the lowest vendor value of all (its count counted); a game action (destroy), logged
    bool MakeRoomFor(Player* bot, uint32 itemId, uint32 questId)
    {
        auto const needed = [bot](ItemTemplate const* proto)
        {
            if (proto->Class == ITEM_CLASS_QUEST || proto->StartQuest || proto->Class == ITEM_CLASS_CONTAINER ||
                proto->Class == ITEM_CLASS_PROJECTILE || proto->Class == ITEM_CLASS_QUIVER ||
                proto->Class == ITEM_CLASS_CONSUMABLE || proto->Class == ITEM_CLASS_KEY)
                return true;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(bot->GetQuestSlotQuestId(slot)))
                {
                    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                        if (quest->RequiredItemId[i] == proto->ItemId)
                            return true;
                    for (uint8 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
                        if (quest->ItemDrop[i] == proto->ItemId)
                            return true;
                }
            return false;
        };
        Item* worst = nullptr;
        bool worstGrey = false;
        uint64 worstValue = 0;
        auto const consider = [&](Item* item)
        {
            if (!item)
                return;
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || needed(proto))
                return;
            bool const grey = proto->Quality == ITEM_QUALITY_POOR;
            uint64 const value = uint64(proto->SellPrice) * item->GetCount();
            if (!worst || (grey && !worstGrey) || (grey == worstGrey && value < worstValue))
            {
                worst = item;
                worstGrey = grey;
                worstValue = value;
            }
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = bot->GetBagByPos(bag))
                for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                    consider(container->GetItemByPos(uint8(slot)));
        if (!worst)
            return false;
        // [loot20] questId 0: an item on the always-loot list (a five's own event item), no quest behind it
        LOG_INFO("playerbots", "[bag66] {} destroys {} x{} ({}, {} c) to loot {} item {}{} t={}", bot->GetName(),
                 worst->GetTemplate()->Name1, worst->GetCount(), worstGrey ? "grey" : "its lowest vendor value",
                 worstValue, questId ? "quest" : "[loot20] always-loot", itemId,
                 questId ? Acore::StringFormat(" for q{}", questId) : std::string(), QuestStats::NowMs());
        bot->DestroyItem(worst->GetBagSlot(), worst->GetSlot(), true);
        return true;
    }

    // seconds since the bot opened this loot (0 when it was not seen opening it)
    float LootSeconds(Player* bot, ObjectGuid guid)
    {
        std::lock_guard<std::mutex> guard(lootLogLock);
        auto const found = lootOpened.find(bot->GetGUID().GetCounter());
        if (found == lootOpened.end() || found->second.first != guid)
            return 0.0f;
        float const seconds = float(getMSTimeDiff(found->second.second, getMSTime())) / float(IN_MILLISECONDS);
        lootOpened.erase(found);
        return seconds;
    }
}

bool LootAction::Execute(Event /*event*/)
{
    if (AI_VALUE(LootObjectStack*, "available loot")->IsLootPending() || bot->GetLootGUID() ||
        bot->IsNonMeleeSpellCast(false))
        return false;

    LootObject lootObject = AI_VALUE(LootObject, "nearest loot");
    if (lootObject.IsEmpty() || !AI_VALUE(LootObjectStack*, "available loot")->CanAttemptLoot(lootObject.guid))
        return false;

    LootObject prevLoot = AI_VALUE(LootObject, "loot target");
    if (!prevLoot.IsEmpty() && prevLoot.guid != lootObject.guid)
    {
        WorldPacket* packet = new WorldPacket(CMSG_LOOT_RELEASE, 8);
        *packet << prevLoot.guid;
        bot->GetSession()->QueuePacket(packet);
    }

    context->GetValue<LootObject>("loot target")->Set(lootObject);
    return true;
}

bool LootAction::isUseful()
{
    if (!sPlayerbotAIConfig.FreeMethodLoot && bot->GetGroup() && bot->GetGroup()->GetLootMethod() == FREE_FOR_ALL &&
        !IsSelfBot(bot))
        return false;

    LootObject lootTarget = AI_VALUE(LootObject, "loot target");
    LootObject nearest = AI_VALUE(LootObject, "nearest loot");
    if (lootTarget.IsEmpty() || nearest.IsEmpty() || nearest.guid == lootTarget.guid ||
        !AI_VALUE(LootObjectStack*, "available loot")->CanAttemptLoot(lootTarget.guid))
        return true;

    // Plain lookups: LootObject::GetWorldObject re-runs Refresh, which IsLootPossible below already pays.
    WorldObject* nearestObject = botAI->GetWorldObject(nearest.guid);
    WorldObject* currentObject = botAI->GetWorldObject(lootTarget.guid);
    if (!nearestObject || !currentObject)
        return true;

    // Leave a valid target only for a clearly closer one, so two equidistant corpses on either side
    // of an obstacle do not swap places on every tick.
    constexpr float lootSwitchMargin = 3.0f;
    if (bot->GetDistance(nearestObject) + lootSwitchMargin < bot->GetDistance(currentObject))
        return true;

    return !lootTarget.IsLootPossible(bot);
}

enum ProfessionSpells
{
    ALCHEMY = 2259,
    BLACKSMITHING = 2018,
    COOKING = 2550,
    ENCHANTING = 7411,
    ENGINEERING = 49383,
    FIRST_AID = 3273,
    FISHING = 7620,
    HERB_GATHERING = 2366,
    INSCRIPTION = 45357,
    JEWELCRAFTING = 25229,
    MINING = 2575,
    SKINNING = 8613,
    TAILORING = 3908
};

bool OpenLootAction::Execute(Event /*event*/)
{
    LootObjectStack* availableLoot = AI_VALUE(LootObjectStack*, "available loot");
    if (availableLoot->IsLootPending() || bot->GetLootGUID() || bot->IsNonMeleeSpellCast(false))
        return false;

    LootObject lootObject = AI_VALUE(LootObject, "loot target");
    if (!availableLoot->CanAttemptLoot(lootObject.guid))
        return false;

    // DoLoot stops the bot first, which means "try again", not a failure; deferring after that
    // stop would make the bot walk away and come back. Only a dry failure at rest defers.
    bool const wasMoving = bot->isMoving();
    bool const wasMounted = bot->IsMounted();

    bool const result = DoLoot(lootObject);
    if (result)
    {
        availableLoot->BeginLoot(lootObject.guid);
        std::lock_guard<std::mutex> guard(lootLogLock);  // [qeff31] the loot's start
        lootOpened[bot->GetGUID().GetCounter()] = {lootObject.guid, getMSTime()};
    }
    else if (!lootObject.IsEmpty() && !wasMoving && !wasMounted)
    {
        availableLoot->DeferLoot(lootObject.guid);
        if (AI_VALUE(LootObject, "loot target").guid == lootObject.guid)
            context->GetValue<LootObject>("loot target")->Set(LootObject());
    }

    return result;
}

bool OpenLootAction::DoLoot(LootObject& lootObject)
{
    if (lootObject.IsEmpty() || !lootObject.IsLootPossible(bot))
        return false;

    Creature* creature = botAI->GetCreature(lootObject.guid);
    if (creature && !bot->IsWithinDistInMap(creature, INTERACTION_DISTANCE))
        return false;

    if (bot->isMoving())
    {
        bot->StopMoving();
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.LootDelay);
        return false;
    }

    if (bot->IsMounted())
    {
        GuildRiding::NoteDismount(bot, "loot / gather");  // [mount7]
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.ReactDelay);
        return false;
    }

    if (creature && creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
    {
        WorldPacket* packet = new WorldPacket(CMSG_LOOT, 8);
        *packet << lootObject.guid;
        bot->GetSession()->QueuePacket(packet);
        // bot->GetSession()->HandleLootOpcode(packet);
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.LootDelay);
        return true;
    }

    if (creature)
    {
        SkillType skill = creature->GetCreatureTemplate()->GetRequiredLootSkill();
        if (!CanOpenLock(skill, lootObject.reqSkillValue))
            return false;

        bool cast = false;
        char const* kind = "skin";
        switch (skill)
        {
            case SKILL_ENGINEERING:
                kind = "salvage";
                cast = botAI->HasSkill(SKILL_ENGINEERING) && botAI->CastSpell(ENGINEERING, creature);
                break;
            case SKILL_HERBALISM:
                kind = "herb";
                cast = botAI->HasSkill(SKILL_HERBALISM) && botAI->CastSpell(32605, creature);
                break;
            case SKILL_MINING:
                kind = "ore";
                cast = botAI->HasSkill(SKILL_MINING) && botAI->CastSpell(32606, creature);
                break;
            default:
                cast = botAI->HasSkill(SKILL_SKINNING) && botAI->CastSpell(SKINNING, creature);
                break;
        }
        if (cast)  // [log28] one line per node opened
            LOG_INFO("playerbots", "[gather] node {} {} {} opened t={}", bot->GetName(), kind, creature->GetEntry(),
                     QuestStats::NowMs());
        return cast;
    }

    GameObject* go = botAI->GetGameObject(lootObject.guid);
    if (go && !go->IsAtInteractDistance(bot))
        return false;

    if (go && (go->GetGoState() != GO_STATE_READY))
        return false;

    // This prevents dungeon chests like Tribunal Chest (Halls of Stone) from being ninja'd by the bots.
    // Quest objects carry the same flag but are gated on quest state, which ActivateToQuest answers.
    if (go && go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND) && !go->ActivateToQuest(bot))
        return false;

    // This prevents raid chests like Gunship Armory (ICC) from being ninja'd by the bots
    if (go && go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE))
        return false;

    if (lootObject.reqItem)
    {
        Item* key = bot->GetItemByEntry(lootObject.reqItem);
        if (!key)
            return false;

        UseItemAction useItem(botAI);
        return useItem.UseItemOnGameObject(key, lootObject.guid);
    }

    if (lootObject.skillId == SKILL_MINING || lootObject.skillId == SKILL_HERBALISM)
    {
        bool const ore = lootObject.skillId == SKILL_MINING;
        bool const cast = botAI->HasSkill(SkillType(lootObject.skillId)) &&
                          botAI->CastSpell(ore ? MINING : HERB_GATHERING, bot);
        if (cast)  // [log28] one line per node opened
            LOG_INFO("playerbots", "[gather] node {} {} {} opened t={}", bot->GetName(), ore ? "ore" : "herb",
                     go ? go->GetEntry() : 0, QuestStats::NowMs());
        return cast;
    }

    uint32 spellId = GetOpeningSpell(lootObject);
    if (!spellId)
        return false;

    return botAI->CastSpell(spellId, bot);
}

uint32 OpenLootAction::GetOpeningSpell(LootObject& lootObject)
{
    if (GameObject* go = botAI->GetGameObject(lootObject.guid))
        if (go->isSpawned())
            return GetOpeningSpell(lootObject, go);

    return 0;
}

uint32 OpenLootAction::GetOpeningSpell(LootObject& lootObject, GameObject* go)
{
    for (PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
    {
        uint32 spellId = itr->first;

        if (itr->second->State == PLAYERSPELL_REMOVED || !itr->second->Active)
            continue;

        if (spellId == MINING || spellId == HERB_GATHERING)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        if (CanOpenLock(lootObject, spellInfo, go))
            return spellId;
    }

    for (uint32 spellId : PlayerbotSpellRepository::Instance().GetOpeningSpells(lootObject.GetLockType()))
    {
        if (spellId == MINING || spellId == HERB_GATHERING)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (spellInfo && CanOpenLock(lootObject, spellInfo, go))
            return spellId;
    }

    return sPlayerbotAIConfig.OpenGoSpell;
}

bool OpenLootAction::CanOpenLock(LootObject& lootObject, SpellInfo const* spellInfo, GameObject* /*go*/)
{
    if (!lootObject.GetLockType())
        return false;

    for (uint8 effIndex = 0; effIndex <= EFFECT_2; effIndex++)
    {
        if (spellInfo->Effects[effIndex].Effect != SPELL_EFFECT_OPEN_LOCK &&
            spellInfo->Effects[effIndex].Effect != SPELL_EFFECT_SKINNING)
            continue;

        if (uint32(spellInfo->Effects[effIndex].MiscValue) == lootObject.GetLockType())
            return CanOpenLock(lootObject.skillId, lootObject.reqSkillValue);
    }

    return false;
}

bool OpenLootAction::CanOpenLock(uint32 skillId, uint32 reqSkillValue)
{
    if (skillId == SKILL_NONE)
        return true;

    uint32 skillValue = bot->GetSkillValue(skillId);
    return skillValue >= reqSkillValue || !reqSkillValue;
}

/*
uint32 StoreLootAction::RoundPrice(double price)
{
    if (price < 100)
    {
        return (uint32)price;
    }

    if (price < 10000)
    {
        return (uint32)(price / 100.0) * 100;
    }

    if (price < 100000)
    {
        return (uint32)(price / 1000.0) * 1000;
    }

    return (uint32)(price / 10000.0) * 10000;
}

bool StoreLootAction::AuctionItem(uint32 itemId)
{
    ItemTemplate const* proto = sItemStorage.LookupEntry<ItemTemplate>(itemId);
    if (!proto)
        return false;

    if (!proto || proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM)
        return false;

    Item* oldItem = bot->GetItemByEntry(itemId);
    if (!oldItem)
        return false;

    AuctionHouseEntry const* ahEntry = AuctionHouseMgr::GetAuctionHouseEntry(unit->getFaction());
    if (!ahEntry)
        return false;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(ahEntry);

    uint32 price = oldItem->GetCount() * proto->BuyPrice * sRandomPlayerbotMgr.GetBuyMultiplier(bot);

uint32 stackCount = urand(1, proto->GetMaxStackSize());
    if (!price || !stackCount)
        return false;

    if (!stackCount)
        stackCount = 1;

    if (urand(0, 100) <= sAhBotConfig.underPriceProbability * 100)
        price = price * 100 / urand(100, 200);

    uint32 bidPrice = RoundPrice(stackCount * price);
    uint32 buyoutPrice = RoundPrice(stackCount * urand(price, 4 * price / 3));

    Item* item = Item::CreateItem(proto->ItemId, stackCount);
    if (!item)
        return false;

    uint32 auction_time = uint32(urand(8, 24) * HOUR * sWorld->getConfig(CONFIG_FLOAT_RATE_AUCTION_TIME));

    AuctionEntry* auctionEntry = new AuctionEntry;
    auctionEntry->Id = sObjectMgr->GenerateAuctionID();
    auctionEntry->itemGuidLow = item->GetGUID().GetCounter();
    auctionEntry->itemTemplate = item->GetEntry();
    auctionEntry->itemCount = item->GetCount();
    auctionEntry->itemRandomPropertyId = item->GetItemRandomPropertyId();
    auctionEntry->owner = bot->GetGUID().GetCounter();
    auctionEntry->startbid = bidPrice;
    auctionEntry->bidder = 0;
    auctionEntry->bid = 0;
    auctionEntry->buyout = buyoutPrice;
    auctionEntry->expireTime = time(nullptr) + auction_time;
    //auctionEntry->moneyDeliveryTime = 0;
    auctionEntry->deposit = 0;
    auctionEntry->auctionHouseEntry = ahEntry;

    auctionHouse->AddAuction(auctionEntry);

    sAuctionMgr.AddAItem(item);

    item->SaveToDB();
    auctionEntry->SaveToDB();

    LOG_ERROR("playerbots", "AhBot {} added {} of {} to auction {} for {}..{}", bot->GetName().c_str(), stackCount,
proto->Name1.c_str(), 1, bidPrice, buyoutPrice);

    if (oldItem->GetCount() > stackCount)
        oldItem->SetCount(oldItem->GetCount() - stackCount);
    else
        bot->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);

    return true;
}
*/

bool StoreLootAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());  // (8+1+4+1+1+4+4+4+4+4+1)
    ObjectGuid guid;
    uint8 loot_type;
    uint32 gold = 0;
    uint8 items = 0;
    uint8 autostored = 0;

    p.rpos(0);
    if (p.size() < 9)
        return false;

    p >> guid;       // 8 corpse/gameobject guid
    p >> loot_type;  // 1 loot type

    LootObjectStack* availableLoot = AI_VALUE(LootObjectStack*, "available loot");
    ObjectGuid currentLoot = bot->GetLootGUID();
    if (loot_type == LOOT_NONE)
    {
        // [qeff31] the loot window did not open for it (another's tap, a group's roll, gone)
        LOG_INFO("playerbots", "[loot] {} finds no loot on {} ({:.1f} s) t={}", bot->GetName(), LootSourceName(guid),
                 LootSeconds(bot, guid), QuestStats::NowMs());
        availableLoot->RetryLoot(guid);
        if (AI_VALUE(LootObject, "loot target").guid == guid)
            context->GetValue<LootObject>("loot target")->Set(LootObject());
        return false;
    }

    if (currentLoot && currentLoot != guid)
    {
        availableLoot->CancelLoot(guid);
        return false;
    }

    if (p.size() < 14)
        return false;

    p >> gold;
    p >> items;
    if (p.size() - p.rpos() < size_t(items) * 22)
        return false;

    availableLoot->LootOpened(guid);

    if (!currentLoot)
        bot->SetLootGUID(guid);

    if (gold > 0)
    {
        WorldPacket* packet = new WorldPacket(CMSG_LOOT_MONEY, 0);
        bot->GetSession()->QueuePacket(packet);
        // bot->GetSession()->HandleLootMoneyOpcode(packet);
    }

    // [qeff31] the quest items offered and taken, and why one was left
    uint32 lootQuest = 0;
    uint32 questOffered = 0, questTaken = 0;
    std::string questLeft;
    for (uint8 i = 0; i < items; ++i)
    {
        uint32 itemid;
        uint32 itemcount;
        uint8 lootslot_type;
        uint8 itemindex;

        p >> itemindex;
        p >> itemid;
        p >> itemcount;
        p.read_skip<uint32>();  // display id
        p.read_skip<uint32>();  // randomSuffix
        p.read_skip<uint32>();  // randomPropertyId
        p >> lootslot_type;     // 0 = can get, 1 = look only, 2 = master get

        uint32 const itemQuest = QuestOfItem(bot, itemid);  // [qeff31]
        if (itemQuest)
        {
            ++questOffered;
            lootQuest = itemQuest;
        }
        // [loot20] a loot the bot meant to take and did not says so, quest item or not. b475 dm-n: the Defias
        // Gunpowder was left in its crate in 7 of 14 opens ("[loot] ... loots object 17155 Defias Gunpowder
        // items=0/1"), the Iron Clad Door 16397 stayed shut behind it and four bosses with it - and not one line in
        // the whole log said why, because `leave` wrote only for an item some member had a quest for and the
        // gunpowder is a quest-class item nobody has a quest for. The loot strategy's own "no thanks" stays
        // quest-only (it fires on every grey on every corpse); a slot the game would not let it take and a bag with
        // no room are what this is for: the bot wanted the item.
        auto const leave = [&](char const* why, bool always = false)
        {
            if (itemQuest || always)
                questLeft += Acore::StringFormat("{}item {} {}", questLeft.empty() ? "" : ", ", itemid, why);
        };
        if (lootslot_type != LOOT_SLOT_TYPE_ALLOW_LOOT && lootslot_type != LOOT_SLOT_TYPE_OWNER)
        {
            leave(lootslot_type == LOOT_SLOT_TYPE_ROLL_ONGOING ? "under a group roll"
                  : lootslot_type == LOOT_SLOT_TYPE_MASTER     ? "for the master looter"
                                                               : "look only",
                  true);
            continue;
        }

        // [craft405] the loot of its own item (its Disenchant, Milling, Prospecting, a container it opened) is its own
        // product, taken whatever its loot strategy: b458 "[loot] Thamesh loots item q0 items=0/1" after "[ench151]
        // Thamesh disenchants Outrunner's Gloves" - the Strange Dust left "not allowed by its loot strategy", the
        // release kept the gloves (not looted: no destroy) and no dust ever reached the guild's enchanters
        if (loot_type != LOOT_SKINNING && !guid.IsItem() && !IsLootAllowed(itemid, botAI))
        {
            leave("not allowed by its loot strategy");
            continue;
        }

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemid);
        if (!proto)
            continue;

        // Skip loot that isn't useful when bags are nearly full.
        if (!IsRealPlayer(botAI->GetMaster()) && AI_VALUE(uint8, "bag space") > 80)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemid);
            // [bag66] an item a quest in its log needs is useful whatever the item usage value says (b411: Jurdogs left
            // q38's items on 4 Harvest Watchers / Golems with no line - the not-useful branch's 'no free stack' skip)
            // [prof271] ... and a material the guild leader wants (the wanted list: a fisher's GATHER fish, a crafter's
            // reagent) or its own called skill-ups need: b428 the guild fishers' catches were offered and left -
            // 4182 of 4188 bobbers "items=0/1", 271 Lesser Sagefish School loots 0/1 (Shalaeth, bags 97 % full; a
            // raw fish's usage is AH) - not one fish reached the guild in 6.4 h of fishing
            // [dm1] ... and an item on its always-loot list (the five's event items: the Defias Gunpowder 5397 its
            // run needs for the Defias Cannon). Its usage is ITEM_USAGE_NONE, so without this the full-bag branch
            // drops what IsLootAllowed just allowed
            bool const alwaysLoot = AI_VALUE(std::set<uint32>&, "always loot list").count(itemid) != 0;
            bool const isUsefulItem =
                itemQuest || proto->StartQuest || guid.IsItem() ||  // [craft406] its own item's loot
                alwaysLoot ||
                (usage != ITEM_USAGE_NONE && usage != ITEM_USAGE_VENDOR && usage != ITEM_USAGE_AH) ||
                (sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter()) &&
                 (sGuildCoordinator.IsWanted(itemid) || sGuildCoordinator.NeedsForOwnCalls(bot, itemid)));

            if (isUsefulItem)
            {
                ItemPosCountVec dest;
                // [bag66] a quest item never stays on the corpse for want of room: the lowest-value item of its bags
                // (a grey one first) makes way for it
                // [loot20] ... and an item on the always-loot list makes room the same way a quest item does. For a
                // guild five that list is the run's own event items (TestHarnessMgr::PrepareEventItemLoot, [dm1]):
                // the Defias Gunpowder the Defias Cannon needs for its lock. It is a quest-class item no member has
                // a quest for, so `itemQuest` is 0 and the full-bag branch dropped it where MakeRoomFor would have
                // made way for it - and these bots carry near-full bags all run ("[econ20] full-bags material
                // relief", "bags 97 % full"). MakeRoomFor never touches a quest item, a container, a quiver,
                // ammunition, a consumable or a key, so the drink and food this rest is built on stay put: it gives
                // up the lowest-value thing in the bags, a grey first.
                // [dm30] ... and a refusal no free slot can lift is not a bag-room refusal. Player::CanStoreItem
                // asks CanTakeMoreSimilarItems FIRST: an item the game caps per player (item_template.maxcount, or
                // its ItemLimitCategory) that the bot already holds that many of comes back
                // EQUIP_ERR_CANT_CARRY_MORE_OF_THIS before a single bag is looked at. The old code read every
                // non-OK result as "bags full", so it destroyed an item to make room that could never be used and
                // then reported the honest failure as "no bag room". b479 dm-n 20261113-194805-dm-n-five1-1:
                // "[bag66] Ravelandy destroys Red Defias Mask x1 (grey, 81 c) to loot [loot20] always-loot item
                // 5397" and in the same millisecond "[loot] Ravelandy loots object 17155 Defias Gunpowder
                // items=0/1 ...; left: item 5397 no bag room" - the Defias Gunpowder is maxcount 1 and Ravelandy
                // was already carrying the one it had kept from an earlier run, so the mask was destroyed for
                // nothing (user rule: nothing a member needs is destroyed or left behind) and the log named the
                // wrong cause, which is what made a loot bug of a run that then blew the door and finished 7/7.
                InventoryResult store =
                    bot->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, dest, itemid, itemcount);
                if (store == EQUIP_ERR_CANT_CARRY_MORE_OF_THIS ||
                    store == EQUIP_ERR_ITEM_MAX_LIMIT_CATEGORY_COUNT_EXCEEDED)
                {
                    leave("already holds the most of it the game allows (no room would help)", true);
                    continue;
                }
                if (store != EQUIP_ERR_OK && (itemQuest || alwaysLoot) && MakeRoomFor(bot, itemid, itemQuest))
                {
                    dest.clear();
                    store = bot->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, dest, itemid, itemcount);
                }
                if (store != EQUIP_ERR_OK)
                {
                    leave(Acore::StringFormat("no bag room (store refusal {})", uint32(store)).c_str(), true);
                    continue;
                }
            }
            else
            {
                uint32 maxStack = proto->GetMaxStackSize();
                if (maxStack == 1)
                    continue;

                std::vector<Item*> found = parseItems(chat->FormatItem(proto));
                bool hasFreeStack = false;
                for (auto stack : found)
                {
                    if (stack->GetCount() + itemcount < maxStack)
                    {
                        hasFreeStack = true;
                        break;
                    }
                }

                if (!hasFreeStack)
                    continue;
            }
        }

        Player* master = botAI->GetMaster();
        if (sRandomPlayerbotMgr.IsRandomBot(bot) && master)
        {
            uint32 price = itemcount * proto->BuyPrice * sRandomPlayerbotMgr.GetBuyMultiplier(bot) + gold;
            if (price)
                sRandomPlayerbotMgr.AddTradeDiscount(bot, master, price);

            if (Group* group = bot->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (ref->GetSource() != bot)
                        GuildTaskMgr::instance().CheckItemTask(itemid, itemcount, ref->GetSource(), bot);
        }

        WorldPacket* packet = new WorldPacket(CMSG_AUTOSTORE_LOOT_ITEM, 1);
        *packet << itemindex;
        bot->GetSession()->QueuePacket(packet);
        // bot->GetSession()->HandleAutostoreLootItemOpcode(packet);
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.LootDelay);
        ++autostored;
        if (itemQuest)
            ++questTaken;

        if (proto->Quality > ITEM_QUALITY_NORMAL && !urand(0, 50) &&
            botAI->HasStrategy("emote", BOT_STATE_NON_COMBAT) && sPlayerbotAIConfig.RandomBotEmote)
            botAI->PlayEmote(TEXT_EMOTE_CHEER);

        if (proto->Quality >= ITEM_QUALITY_RARE && !urand(0, 1) && botAI->HasStrategy("emote", BOT_STATE_NON_COMBAT) &&
            sPlayerbotAIConfig.RandomBotEmote)
            botAI->PlayEmote(TEXT_EMOTE_CHEER);

        BroadcastHelper::BroadcastLootingItem(botAI, bot, proto);
    }

    // [qeff31] one line per loot: what it was, the quest its items serve, how long it took from its open
    LOG_INFO("playerbots", "[loot] {} loots {} q{} ({:.1f} s) items={}/{} quest_items={}/{}{}{} t={}",
             bot->GetName(), LootSourceName(guid), lootQuest, LootSeconds(bot, guid), uint32(autostored),
             uint32(items), questTaken, questOffered, questLeft.empty() ? "" : "; left: ", questLeft,
             QuestStats::NowMs());

    // Nothing storable: defer instead of reopening the window in a loop.
    if (autostored == 0 && guid.IsGameObject())
        availableLoot->DeferLoot(guid);
    else
        availableLoot->Remove(guid);

    if (AI_VALUE(LootObject, "loot target").guid == guid)
        context->GetValue<LootObject>("loot target")->Set(LootObject());

    // release loot
    WorldPacket* packet = new WorldPacket(CMSG_LOOT_RELEASE, 8);
    *packet << guid;
    bot->GetSession()->QueuePacket(packet);
    // bot->GetSession()->HandleLootReleaseOpcode(packet);
    return true;
}

bool StoreLootAction::IsLootAllowed(uint32 itemid, PlayerbotAI* botAI)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    LootStrategy* lootStrategy = AI_VALUE(LootStrategy*, "loot strategy");

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemid);
    if (!proto)
        return false;

    std::set<uint32>& lootItems = AI_VALUE(std::set<uint32>&, "always loot list");
    if (lootItems.find(itemid) != lootItems.end())
        return true;

    uint32 max = proto->MaxCount;
    if (max > 0 && botAI->GetBot()->HasItemCount(itemid, max, true))
        return false;

    if (proto->StartQuest)
    {
        return true;
    }

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 entry = botAI->GetBot()->GetQuestSlotQuestId(slot);
        Quest const* quest = sObjectMgr->GetQuestTemplate(entry);
        if (!quest)
            continue;

        for (uint8 i = 0; i < 4; i++)
        {
            if (quest->RequiredItemId[i] == itemid)
            {
                // if (AI_VALUE2(uint32, "item count", proto->Name1) < quest->RequiredItemCount[i])
                // {
                //     if (botAI->GetMaster() && sPlayerbotAIConfig.SyncQuestWithPlayer)
                //         return false; //Quest is autocomplete for the bot so no item needed.
                // }

                return true;
            }
        }
    }

    // if (proto->Bonding == BIND_QUEST_ITEM ||  //Still testing if it works ok without these lines.
    //     proto->Bonding == BIND_QUEST_ITEM1 || //Eventually this has to be removed.
    //     proto->Class == ITEM_CLASS_QUEST)
    //{

    bool canLoot = lootStrategy->CanLoot(proto, context);
    // if (canLoot && proto->Bonding == BIND_WHEN_PICKED_UP && IsRealPlayer(botAI->GetMaster()))
    // canLoot = sPlayerbotAIConfig.IsInRandomAccountList(botAI->GetBot()->GetSession()->GetAccountId());

    return canLoot;
}

bool ReleaseLootAction::Execute(Event /*event*/)
{
    ObjectGuid guid = bot->GetLootGUID();
    if (!guid)
        return false;

    WorldPacket* packet = new WorldPacket(CMSG_LOOT_RELEASE, 8);
    *packet << guid;
    bot->GetSession()->QueuePacket(packet);
    return true;
}
