/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDBOTDUTY_H
#define PLAYERBOTS_RAIDBOTDUTY_H

#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Timer.h"
#include <mutex>
#include <string>
#include <unordered_map>

// Helpers for encounter scripts that upstream wrote around a human master ("the player handles it",
// "follow the master's drake"). The leader here may be a SelfBot (AI-driven) or there may be no client at
// all, so a duty is only left to the master when a person really drives that character.
namespace RaidBotDuty
{
// The master when a person drives it through the game client; nullptr for a SelfBot or no master.
inline Player* GetHumanMaster(PlayerbotAI* botAI)
{
    Player* master = botAI->GetMaster();
    return IsRealPlayer(master) ? master : nullptr;
}

inline bool IsUsableAnchor(Player* bot, Player* member)
{
    return member && member != bot && member->IsAlive() && member->IsInWorld() &&
           member->GetMapId() == bot->GetMapId() && member->GetInstanceId() == bot->GetInstanceId();
}

// Who to regroup on (fall-through-floor rescue, stacking): the human master, else the group leader, else
// the lowest-GUID alive member on the same map. Never the bot itself; nullptr when nobody qualifies.
inline Player* GetRegroupAnchor(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (Player* human = GetHumanMaster(botAI); IsUsableAnchor(bot, human))
        return human;

    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    if (Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID()); IsUsableAnchor(bot, leader))
        return leader;

    Player* anchor = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (IsUsableAnchor(bot, member) && (!anchor || member->GetGUID() < anchor->GetGUID()))
            anchor = member;
    }
    return anchor;
}

// Leader of a vehicle formation (Oculus drakes). A human master on a vehicle keeps the lead (upstream
// behaviour); otherwise the group leader if mounted, else the lowest-GUID mounted member. May return the
// bot itself, which then steers for the group. nullptr when nobody of the group sits on a vehicle.
inline Player* GetVehicleFormationLeader(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (Player* human = GetHumanMaster(botAI))
        return human->GetVehicleBase() ? human : nullptr;

    Group* group = bot->GetGroup();
    if (!group)
        return bot->GetVehicleBase() ? bot : nullptr;

    auto const mounted = [bot](Player* member)
    {
        return member && member->IsAlive() && member->IsInWorld() && member->GetMapId() == bot->GetMapId() &&
               member->GetInstanceId() == bot->GetInstanceId() && member->GetVehicleBase();
    };

    if (Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID()); mounted(leader))
        return leader;

    Player* first = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (mounted(member) && (!first || member->GetGUID() < first->GetGUID()))
            first = member;
    }
    return first;
}

// One log line per (instance, key) and per minute at most: used when a workaround skips a core-bugged step.
inline void LogWorkaroundOnce(Player* bot, std::string const& key, std::string const& message)
{
    static std::mutex lock;
    static std::unordered_map<std::string, uint32> lastLogged;
    constexpr uint32 LOG_INTERVAL_MS = MINUTE * IN_MILLISECONDS;

    std::string const fullKey = std::to_string(bot->GetInstanceId()) + ":" + key;
    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const it = lastLogged.find(fullKey);
        if (it != lastLogged.end() && getMSTimeDiff(it->second, now) < LOG_INTERVAL_MS)
            return;
        lastLogged[fullKey] = now;
    }

    LOG_INFO("playerbots", "[raid workaround] {} ({}): {}", bot->GetName(), key, message);
}
}  // namespace RaidBotDuty

#endif
