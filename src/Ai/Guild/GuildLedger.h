/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDLEDGER_H
#define PLAYERBOTS_GUILDLEDGER_H

#include <string>

#include "Define.h"

class Player;
struct ItemTemplate;

// [eco190] coordinator 2026-11-11 (b426-b429: the guild lost 1.5 - 7.6 g an hour and no log said where): every money
// change of a guild-plan bot (Player::ModifyMoney -> OnPlayerMoneyChanged) is booked to its cause. The cause is the
// core's own step in front of the change where it has a hook (loot money, a quest's reward, a repair, a vendor buy of
// that item, a sale, a sent letter), else the module's call that makes it (Cause, scoped on the map thread: a class
// trainer, a profession trainer, a flight, a talent reset, a mail or trade transfer). The guild's totals by cause,
// in and out, log as "[eco190]" with the [health] line's minute (on change).
namespace GuildLedger
{
// the cause of the money changes made inside its scope, on this thread (the bot's map thread)
class Cause
{
public:
    explicit Cause(char const* what);
    ~Cause();
    Cause(Cause const&) = delete;
    Cause& operator=(Cause const&) = delete;

private:
    char const* prev_;
};

// what a vendor purchase of this item is for (the ledger's buy causes): food / drink, potion, ammo, gear, ...
char const* BuyCause(ItemTemplate const* proto);

// the guild's totals by cause since the server start, one line (empty: nothing booked)
std::string Text();
}  // namespace GuildLedger

void AddGuildLedgerScripts();

#endif
