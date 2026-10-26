/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDMARKOWNERSHIP_H
#define PLAYERBOTS_RAIDMARKOWNERSHIP_H

#include "Group.h"
#include "InstanceScript.h"
#include "Player.h"
#include "Timer.h"
#include <mutex>
#include <unordered_map>

// Per-instance "script owns the raid marks" flag. Encounter scripts place their Skull/Cross/... icons
// through SetIcon(); while the claim is live the generic "mark rti" logic (leader, SelfBot or any bot)
// leaves every icon alone, so a mechanic's mark is never overwritten by lowest-HP auto-marking.
namespace RaidMarkOwnership
{
// A claim outlives its last icon change by this long; it also holds for the rest of a boss encounter.
inline constexpr uint32 CLAIM_TTL_MS = 15000;
inline constexpr uint32 CLAIM_ENCOUNTER_MAX_MS = 15 * MINUTE * IN_MILLISECONDS;

struct Registry
{
    std::mutex lock;
    std::unordered_map<uint32, uint32> lastClaimMs;  // instance id -> getMSTime() of the last claim
};

inline Registry& GetRegistry()
{
    static Registry registry;
    return registry;
}

inline void Claim(Player* bot)
{
    if (!bot || !bot->GetInstanceId())
        return;

    Registry& registry = GetRegistry();
    std::lock_guard<std::mutex> guard(registry.lock);
    registry.lastClaimMs[bot->GetInstanceId()] = getMSTime();
}

inline bool IsOwned(Player* bot)
{
    if (!bot || !bot->GetInstanceId())
        return false;

    uint32 sinceClaim = 0;
    {
        Registry& registry = GetRegistry();
        std::lock_guard<std::mutex> guard(registry.lock);
        auto const it = registry.lastClaimMs.find(bot->GetInstanceId());
        if (it == registry.lastClaimMs.end())
            return false;

        sinceClaim = getMSTimeDiff(it->second, getMSTime());
        if (sinceClaim > CLAIM_ENCOUNTER_MAX_MS)
        {
            registry.lastClaimMs.erase(it);
            return false;
        }
    }

    if (sinceClaim <= CLAIM_TTL_MS)
        return true;

    InstanceScript* instance = bot->GetInstanceScript();
    return instance && instance->IsEncounterInProgress();
}

// Script-side replacement for Group::SetTargetIcon: claims ownership, then sets (or clears) the icon.
inline void SetIcon(Player* bot, Group* group, uint8 iconIndex, ObjectGuid targetGuid)
{
    if (!bot || !group)
        return;

    Claim(bot);
    group->SetTargetIcon(iconIndex, bot->GetGUID(), targetGuid);
}
}  // namespace RaidMarkOwnership

#endif
