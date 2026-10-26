/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTMOVEEVENTS_H
#define PLAYERBOTS_BOTMOVEEVENTS_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <utility>
#include <vector>

// [shside] Movement events a bot's own AI wants an observer (the test harness) to log: "obstacle_sidestep",
// "obstacle_sidestep_failed", "move_conflict", "waypoint_lane_veto". Bots push from their map threads, the harness
// drains on the world thread (TestHarnessMgr::DrainMoveReports, PATCH_shaman_sidestep.md). At most
// MAX_QUEUED_PER_BOT per bot are kept; the oldest go first. Every push is also a LOG_INFO line.
namespace BotMoveEvents
{
    struct Event
    {
        std::string type;
        std::vector<std::pair<std::string, std::string>> strs;
        std::vector<std::pair<std::string, double>> nums;

        Event& Str(std::string key, std::string value)
        {
            strs.emplace_back(std::move(key), std::move(value));
            return *this;
        }
        Event& Num(std::string key, double value)
        {
            nums.emplace_back(std::move(key), value);
            return *this;
        }
        Event& Real(std::string key, float value) { return Num(std::move(key), value); }
    };

    constexpr size_t MAX_QUEUED_PER_BOT = 16;

    void Push(ObjectGuid bot, Event event);
    // The oldest queued event of the bot; false when none.
    bool Take(ObjectGuid bot, Event& event);
    void ClearAll();
}

#endif
