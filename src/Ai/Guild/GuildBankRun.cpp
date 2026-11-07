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

    // the first purchased tab where the bot's rank has these rights
    int32 TabWithRights(Guild* guild, Player* bot, uint32 rights)
    {
        for (uint8 tab = 0; tab < guild->GetPurchasedTabsSize(); ++tab)
            if (guild->MemberHasTabRights(bot->GetGUID(), tab, rights))
                return tab;
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
            return "нет гильдии";
        if (!guild->GetPurchasedTabsSize())
            return "нет вкладки банка";
        if (TabWithRights(guild, bot, deposit ? GUILD_BANK_RIGHT_DEPOSIT_ITEM : GUILD_BANK_RIGHT_VIEW_TAB) < 0)
            return "нет прав на вкладку";
        return "";
    }

    bool NearestVault(Player* bot, WorldPosition& pos, float& dist)
    {
        dist = FLT_MAX;
        for (Vault const& vault : vaults)
        {
            if (vault.pos.GetMapId() != bot->GetMapId() || !Friendly(bot, vault))
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
        int32 const tab = TabWithRights(guild, bot, GUILD_BANK_RIGHT_DEPOSIT_ITEM);
        if (tab < 0)
            return 0;
        uint32 moved = 0;
        for (uint32 low : itemGuidLows)
        {
            Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(low));
            if (!item || item->IsSoulBound())
                continue;
            uint32 const entry = item->GetEntry();
            uint32 const before = bot->GetItemCount(entry, false);
            guild->SwapItemsWithInventory(bot, false, uint8(tab), NULL_SLOT, item->GetBagSlot(), item->GetSlot(), 0);
            if (bot->GetItemCount(entry, false) < before)
                ++moved;
        }
        return moved;
    }

    bool Withdraw(Player* bot, uint32 itemId)
    {
        Guild* guild = BotGuild(bot);
        if (!guild)
            return false;
        for (uint8 tab = 0; tab < guild->GetPurchasedTabsSize(); ++tab)
        {
            if (!guild->MemberHasTabRights(bot->GetGUID(), tab, GUILD_BANK_RIGHT_VIEW_TAB))
                continue;
            for (uint8 slot = 0; slot < GUILD_BANK_MAX_SLOTS; ++slot)
            {
                Item* item = guild->GetBankItem(tab, slot);
                if (!item || item->GetEntry() != itemId)
                    continue;
                uint32 const before = bot->GetItemCount(itemId, false);
                guild->SwapItemsWithInventory(bot, true, tab, slot, NULL_BAG, NULL_SLOT, 0);
                return bot->GetItemCount(itemId, false) > before;
            }
        }
        return false;
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
