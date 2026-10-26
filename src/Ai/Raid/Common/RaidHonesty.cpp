/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidHonesty.h"
#include "Log.h"
#include "Player.h"
#include <ctime>
#include <mutex>
#include <unordered_map>

namespace RaidHonesty
{

namespace
{

constexpr time_t LOG_INTERVAL_SECONDS = 600;

// Raid maps update on different threads, so the throttle table is shared and locked.
std::mutex logMutex;
std::unordered_map<std::string, time_t> lastLogTime;

} // namespace

void LogNeedsHonestTactic(Player* bot, std::string const& mechanic)
{
    if (!bot)
        return;

    std::string const key = bot->GetGUID().ToString() + "|" + mechanic;
    time_t const now = std::time(nullptr);
    {
        std::lock_guard<std::mutex> lock(logMutex);
        auto const [it, inserted] = lastLogTime.try_emplace(key, now);
        if (!inserted)
        {
            if (now - it->second < LOG_INTERVAL_SECONDS)
                return;

            it->second = now;
        }
    }

    LOG_INFO("playerbots", "Bot {} (map {}): no honest tactic yet for '{}', needs a manual fix",
             bot->GetName(), bot->GetMapId(), mechanic);
}

}
