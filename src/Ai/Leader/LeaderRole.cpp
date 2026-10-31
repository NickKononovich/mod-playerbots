/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaderRole.h"

#include "Group.h"
#include "Player.h"

#include <mutex>
#include <unordered_map>

namespace Leader
{
    namespace Role
    {
        namespace
        {
            std::mutex roleLock;
            std::unordered_map<ObjectGuid, ObjectGuid> standDown;  // bot -> the leader it stands down to
        }

        ObjectGuid ActiveLeaderOf(Player* bot)
        {
            if (!bot)
                return ObjectGuid::Empty;
            {
                std::lock_guard<std::mutex> lock(roleLock);
                auto const found = standDown.find(bot->GetGUID());
                if (found != standDown.end())
                    return found->second;
            }
            Group* group = bot->GetGroup();
            return group ? group->GetLeaderGUID() : bot->GetGUID();
        }

        bool IsActiveLeader(Player* bot) { return bot && ActiveLeaderOf(bot) == bot->GetGUID(); }

        bool StoodDown(Player* bot) { return bot && !IsActiveLeader(bot); }

        void OnJoin(Player* bot, Group* group)
        {
            if (!bot || !group || group->GetLeaderGUID() == bot->GetGUID())
                return;
            std::lock_guard<std::mutex> lock(roleLock);
            standDown[bot->GetGUID()] = group->GetLeaderGUID();
        }

        void OnLeave(Player* bot)
        {
            if (!bot)
                return;
            std::lock_guard<std::mutex> lock(roleLock);
            standDown.erase(bot->GetGUID());
        }
    }
}
