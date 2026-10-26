/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_WLKTACTICLOG_H
#define PLAYERBOTS_WLKTACTICLOG_H

#include "Player.h"
#include "Timer.h"

#include <mutex>
#include <string>
#include <unordered_map>

// Rate limit for the "[<tag>] ..." lines the WotLK dungeon tactics print when a rule fires, so the harness logs show
// each rule at work without one line per tick: true at most once per interval for a (bot, tag) pair.
namespace WlkTacticLog
{
constexpr uint32 DEFAULT_INTERVAL_MS = 10 * IN_MILLISECONDS;

inline bool Allow(Player* bot, char const* tag, uint32 intervalMs = DEFAULT_INTERVAL_MS)
{
    static std::mutex lock;
    static std::unordered_map<std::string, uint32> lastLogged;

    std::string const key = std::to_string(bot->GetGUID().GetRawValue()) + ":" + tag;
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> guard(lock);
    auto const it = lastLogged.find(key);
    if (it != lastLogged.end() && getMSTimeDiff(it->second, now) < intervalMs)
        return false;

    lastLogged[key] = now;
    return true;
}
}  // namespace WlkTacticLog

#endif
