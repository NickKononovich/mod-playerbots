/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildBankRun.h"

#include "DBCStores.h"
#include "GameObject.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace
{
    struct Vault
    {
        WorldPosition pos;
        uint32 zoneTeam{0};  // AREATEAM_* of its zone
    };
    std::vector<Vault> vaults;  // written once at startup

    bool Friendly(Player* bot, Vault const& vault)
    {
        if (vault.zoneTeam == AREATEAM_ALLY)
            return bot->GetTeamId() == TEAM_ALLIANCE;
        if (vault.zoneTeam == AREATEAM_HORDE)
            return bot->GetTeamId() == TEAM_HORDE;
        return true;  // neutral city (Booty Bay, Shattrath, Dalaran ...)
    }

    // [bank401] a withdrawal the bank refused (rights present, item there, bags free: the rank's daily withdraw slots
    // are used up): member -> (tab, its withdraw count then). Lifted by the game itself: the guild's daily reset drops
    // the member's withdraw count below that value. Bot threads and the coordinator read it.
    std::mutex refusedLock;
    std::unordered_map<uint32, std::pair<uint8, int32>> refused;

    int32 WithdrawnToday(Guild* guild, Player* bot, uint8 tab)
    {
        Guild::Member const* member = guild->GetMember(bot->GetGUID());
        return member ? member->GetBankWithdrawValue(tab) : 0;
    }

    // the first purchased tab where the bot's rank has these rights
    int32 TabWithRights(Guild* guild, Player* bot, uint32 rights)
    {
        for (uint8 tab = 0; tab < guild->GetPurchasedTabsSize(); ++tab)
            if (guild->MemberHasTabRights(bot->GetGUID(), tab, rights))
                return tab;
        return -1;
    }

    // [bank408] the first tab with deposit rights that takes `count` of the item: a full tab takes nothing (b460: the
    // only tab held 98/98 and members stood at the vault for 1400 s, "could not deposit" on every visit). [bank411]
    // as the core's auto-store (Guild::BankMoveItemData::CanStore): onto its partial stacks, then into empty slots.
    // itemId 0: any item, an empty slot
    int32 DepositTab(Guild* guild, Player* bot, uint32 itemId, uint32 count)
    {
        ItemTemplate const* proto = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
        uint32 const maxStack = proto ? std::max<uint32>(1, proto->GetMaxStackSize()) : 1;
        for (uint8 tab = 0; tab < guild->GetPurchasedTabsSize(); ++tab)
        {
            if (!guild->MemberHasTabRights(bot->GetGUID(), tab, GUILD_BANK_RIGHT_DEPOSIT_ITEM))
                continue;
            uint32 room = 0;
            for (uint8 slot = 0; slot < GUILD_BANK_MAX_SLOTS && room < count; ++slot)
            {
                Item* item = guild->GetBankItem(tab, slot);
                if (!item)
                    room += maxStack;
                else if (item->GetEntry() == itemId && item->GetCount() < maxStack)
                    room += maxStack - item->GetCount();
            }
            if (room >= count)
                return tab;
        }
        return -1;
    }
}

namespace GuildBankRun
{
    void LoadVaults()
    {
        vaults.clear();
        for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        {
            GameObjectTemplate const* proto = sObjectMgr->GetGameObjectTemplate(data.id);
            if (!proto || proto->type != GAMEOBJECT_TYPE_GUILD_BANK)
                continue;
            MapEntry const* map = sMapStore.LookupEntry(data.mapid);
            if (!map || map->Instanceable())
                continue;
            Vault vault;
            vault.pos = WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation);
            uint32 const zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ);
            if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId))
                vault.zoneTeam = zone->team;
            vaults.push_back(vault);
        }
        LOG_INFO("playerbots", "[guild30] guild bank: {} vaults on the continents", vaults.size());
    }

    uint32 VaultCount() { return uint32(vaults.size()); }

    Guild* BotGuild(Player* bot) { return bot->GetGuildId() ? sGuildMgr->GetGuildById(bot->GetGuildId()) : nullptr; }

    std::string BankState(Player* bot, bool deposit)
    {
        Guild* guild = BotGuild(bot);
        if (!guild)
            return "no guild";
        if (!guild->GetPurchasedTabsSize())
            return "no guild bank tab";
        if (TabWithRights(guild, bot, deposit ? GUILD_BANK_RIGHT_DEPOSIT_ITEM : GUILD_BANK_RIGHT_VIEW_TAB) < 0)
            return "no rights to the bank tab";
        if (deposit)
            return "";  // the room is the item's own question: DepositState
        if (!bot->GetFreeInventorySpace())
            return "no free bag slot for a withdrawal";
        std::lock_guard<std::mutex> guard(refusedLock);
        auto const itr = refused.find(bot->GetGUID().GetCounter());
        if (itr == refused.end())
            return "";
        if (WithdrawnToday(guild, bot, itr->second.first) < itr->second.second)
        {
            // [bank402] the guild's daily reset gave the rank's withdraw slots back
            LOG_INFO("playerbots", "[bank402] {}: the bank withdraw slots of tab {} are back (daily reset)",
                     bot->GetName(), uint32(itr->second.first) + 1);
            refused.erase(itr);
            return "";
        }
        return "the bank refused a withdrawal: no withdraw slots left today on tab " +
               std::to_string(uint32(itr->second.first) + 1);
    }

    std::string DepositState(Player* bot, uint32 itemId, uint32 count)
    {
        std::string why = BankState(bot, true);
        if (why.empty() && DepositTab(BotGuild(bot), bot, itemId, std::max<uint32>(1, count)) < 0)
            why = "the guild bank is full";  // [bank408] [bank411] no empty slot, no partial stack of it with room
        return why;
    }

    bool NearestVault(Player* bot, WorldPosition& pos, float& dist,
                      std::function<bool(WorldPosition const&)> const& refused)
    {
        dist = FLT_MAX;
        for (Vault const& vault : vaults)
        {
            if (vault.pos.GetMapId() != bot->GetMapId() || !Friendly(bot, vault))
                continue;
            // [town20] a vault whose walk the bot's own trip judge already refused is no candidate: the nearest
            // *reachable* one is the pick, as every other town picker does it (TownRefused). The straight line alone
            // sent L22-26 bots to Everlook (Winterspring, 55+) and Gadgetzan (Tanaris, 40+) because those vaults are
            // geometrically nearer Ashenvale / Stonetalon than Darnassus' is
            if (refused && refused(vault.pos))
                continue;
            float const d = bot->GetExactDist(vault.pos.GetPositionX(), vault.pos.GetPositionY(),
                                              vault.pos.GetPositionZ());
            if (d < dist)
            {
                dist = d;
                pos = vault.pos;
            }
        }
        return dist < FLT_MAX;
    }

    bool AtVault(Player* bot, WorldPosition const& pos)
    {
        if (bot->GetMapId() != pos.GetMapId() ||
            bot->GetExactDist(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()) > 15.0f)
            return false;
        GameObject* go = bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_GUILD_BANK, 15.0f);
        return go && bot->GetGameObjectIfCanInteractWith(go->GetGUID(), GAMEOBJECT_TYPE_GUILD_BANK);
    }

    // GuildBankAction::MoveFromCharToBank: SwapItemsWithInventory into the first tab with deposit rights
    uint32 Deposit(Player* bot, std::vector<uint32> const& itemGuidLows)
    {
        Guild* guild = BotGuild(bot);
        if (!guild)
            return 0;
        uint32 moved = 0;
        for (uint32 low : itemGuidLows)
        {
            Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(low));
            if (!item || item->IsSoulBound())
                continue;
            // [bank408] each item into a tab that still has room for it
            int32 const tab = DepositTab(guild, bot, item->GetEntry(), item->GetCount());
            if (tab < 0)
            {
                LOG_INFO("playerbots", "[bank408] {} could not deposit item {}: the guild bank is full",
                         bot->GetName(), item->GetEntry());
                continue;
            }
            uint32 const entry = item->GetEntry();
            uint32 const before = bot->GetItemCount(entry, false);
            guild->SwapItemsWithInventory(bot, false, uint8(tab), NULL_SLOT, item->GetBagSlot(), item->GetSlot(), 0);
            bool const ok = bot->GetItemCount(entry, false) < before;
            moved += ok ? 1 : 0;
            LOG_INFO("playerbots", "[bank405] {} {} item {} into tab {}", bot->GetName(),
                     ok ? "deposits" : "could not deposit", entry, tab + 1);
        }
        return moved;
    }

    std::string Withdraw(Player* bot, uint32 itemId, uint32* itemGuidLow)
    {
        Guild* guild = BotGuild(bot);
        if (!guild)
            return "no guild";
        for (uint8 tab = 0; tab < guild->GetPurchasedTabsSize(); ++tab)
        {
            if (!guild->MemberHasTabRights(bot->GetGUID(), tab, GUILD_BANK_RIGHT_VIEW_TAB))
                continue;
            for (uint8 slot = 0; slot < GUILD_BANK_MAX_SLOTS; ++slot)
            {
                Item* item = guild->GetBankItem(tab, slot);
                if (!item || item->GetEntry() != itemId)
                    continue;
                ItemPosCountVec dest;
                if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
                {
                    LOG_INFO("playerbots", "[bank403] {} cannot take item {} from tab {}: no bag room", bot->GetName(),
                             itemId, uint32(tab) + 1);
                    return "no bag room";
                }
                uint32 const before = bot->GetItemCount(itemId, false);
                uint32 const movedGuid = item->GetGUID().GetCounter();  // [bank410] a whole stack keeps its item
                int32 const withdrawn = WithdrawnToday(guild, bot, tab);
                guild->SwapItemsWithInventory(bot, true, tab, slot, NULL_BAG, NULL_SLOT, 0);
                if (bot->GetItemCount(itemId, false) > before)
                {
                    LOG_INFO("playerbots", "[bank400] {} took item {} from tab {} slot {} (withdrawals today {})",
                             bot->GetName(), itemId, uint32(tab) + 1, uint32(slot), withdrawn + 1);
                    Item* taken = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(movedGuid));
                    if (itemGuidLow)
                        *itemGuidLow = taken ? movedGuid : 0;
                    return "";
                }
                // the core moves nothing without a word when the rank has no withdraw slots left on the tab
                // (Guild::BankMoveItemData::HasWithdrawRights)
                {
                    std::lock_guard<std::mutex> guard(refusedLock);
                    refused[bot->GetGUID().GetCounter()] = {tab, withdrawn};
                }
                LOG_INFO("playerbots",
                         "[bank401] {} refused item {} by the bank on tab {}: no withdraw slots left today for its "
                         "rank (withdrawals today {}), no bank withdraw until the daily reset",
                         bot->GetName(), itemId, uint32(tab) + 1, withdrawn);
                return "the bank refused: no withdraw slots left today on tab " + std::to_string(uint32(tab) + 1);
            }
        }
        LOG_INFO("playerbots", "[bank404] {} finds item {} in no bank tab it may view", bot->GetName(), itemId);
        return "not in the bank";
    }

    void CountBank(Guild* guild, std::map<uint32, uint32>& counts)
    {
        if (!guild)
            return;
        for (uint8 tab = 0; tab < guild->GetPurchasedTabsSize(); ++tab)
            for (uint8 slot = 0; slot < GUILD_BANK_MAX_SLOTS; ++slot)
                if (Item* item = guild->GetBankItem(tab, slot))
                    counts[item->GetEntry()] += item->GetCount();
    }
}
