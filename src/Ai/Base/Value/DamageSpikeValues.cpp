/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DamageSpikeValues.h"

#include <algorithm>

#include "Group.h"
#include "Playerbots.h"
#include "Timer.h"

namespace
{
    constexpr size_t STALE_UNIT_SWEEP_SIZE = 8;  // more tracked units than this triggers a sweep
}

float HealthLossTracker::Sample(Unit const* unit, uint32 nowMs)
{
    if (!unit || !unit->IsAlive() || !unit->GetMaxHealth())
    {
        if (unit)
            _samples.erase(unit->GetGUID());

        return 0.0f;
    }

    if (_samples.size() > STALE_UNIT_SWEEP_SIZE)
        ForgetStaleUnits(nowMs);

    std::deque<HealthSample>& samples = _samples[unit->GetGUID()];
    while (!samples.empty() && getMSTimeDiff(samples.front().timeMs, nowMs) > WINDOW_MS)
        samples.pop_front();

    uint32 const health = unit->GetHealth();
    if (samples.empty() || getMSTimeDiff(samples.back().timeMs, nowMs) >= SAMPLE_INTERVAL_MS)
        samples.push_back({nowMs, health});

    uint32 peak = health;
    for (HealthSample const& sample : samples)
        peak = std::max(peak, sample.health);

    return static_cast<float>(peak - health) * 100.0f / unit->GetMaxHealth();
}

void HealthLossTracker::ForgetStaleUnits(uint32 nowMs)
{
    for (auto itr = _samples.begin(); itr != _samples.end();)
    {
        if (itr->second.empty() || getMSTimeDiff(itr->second.back().timeMs, nowMs) > WINDOW_MS)
            itr = _samples.erase(itr);
        else
            ++itr;
    }
}

float RecentHealthLossValue::Calculate() { return _tracker.Sample(bot, getMSTime()); }

Unit* TankDamageSpikeTargetValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group || !bot->IsInCombat())
        return nullptr;

    uint32 const now = getMSTime();
    float bestLoss = sPlayerbotAIConfig.TankDamageSpikePct;
    Unit* result = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMapId() != bot->GetMapId() || !member->IsAlive())
            continue;

        if (!PlayerbotAI::IsTank(member) || bot->GetDistance2d(member) > sPlayerbotAIConfig.SightDistance)
            continue;

        float const loss = _tracker.Sample(member, now);
        if (loss >= bestLoss)
        {
            bestLoss = loss;
            result = member;
        }
    }

    return result;
}
