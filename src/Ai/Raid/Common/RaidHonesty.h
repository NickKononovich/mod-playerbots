/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDHONESTY_H
#define PLAYERBOTS_RAIDHONESTY_H

#include <string>

class Player;

// Bots play without cheats. Where a raid script used to teleport, kill or buff for free and no honest
// replacement exists yet, it reports the case here instead (see the tactic's "нужна честная
// тактика" note in raid_tactics).
namespace RaidHonesty
{

// Logs at most once per bot and mechanic every ten minutes.
void LogNeedsHonestTactic(Player* bot, std::string const& mechanic);

}

#endif
