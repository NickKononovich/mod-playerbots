/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDBANKRUN_H
#define PLAYERBOTS_GUILDBANKRUN_H

#include <map>
#include <string>
#include <vector>

#include "Define.h"
#include "TravelMgr.h"

class Guild;
class Player;

// [guild30] The honest guild bank walk: the nearest guild vault (gameobject type GUILD_BANK) in a friendly or neutral
// city on the bot's map, deposit / withdraw through the core's guild bank (as GuildBankAction does). Everything is a
// no-op without a guild, a purchased tab or the rank's tab rights.
namespace GuildBankRun
{
    void LoadVaults();  // world thread, startup

    Guild* BotGuild(Player* bot);
    // "" = the bank can be used for this, else why not («нет гильдии», «нет вкладки», «нет прав»)
    std::string BankState(Player* bot, bool deposit);
    bool NearestVault(Player* bot, WorldPosition& pos, float& dist);
    bool AtVault(Player* bot, WorldPosition const& pos);  // in interaction range of a vault there
    uint32 Deposit(Player* bot, std::vector<uint32> const& itemGuidLows);  // items moved
    // "" = taken, else the game's answer: not in the bank, no bag room, the bank refused ([bank401]: the rank's daily
    // withdraw slots are used up; BankState(withdraw) then says so until the guild's daily reset)
    std::string Withdraw(Player* bot, uint32 itemId);
    void CountBank(Guild* guild, std::map<uint32, uint32>& counts);
    uint32 VaultCount();
}

#endif
