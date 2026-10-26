/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DAMAGESPIKEVALUES_H
#define PLAYERBOTS_DAMAGESPIKEVALUES_H

#include <deque>
#include <unordered_map>

#include "ObjectGuid.h"
#include "Value.h"

class PlayerbotAI;
class Unit;

// Rolling health samples per unit. The loss is net of healing, measured from the highest health seen in
// the window, so it reads what the unit is actually about to die from.
class HealthLossTracker
{
public:
    static constexpr uint32 WINDOW_MS = 2000;
    static constexpr uint32 SAMPLE_INTERVAL_MS = 100;

    // Percent of max health lost within the window, sampling the unit's current health.
    float Sample(Unit const* unit, uint32 nowMs);

private:
    struct HealthSample
    {
        uint32 timeMs;
        uint32 health;
    };

    void ForgetStaleUnits(uint32 nowMs);

    std::unordered_map<ObjectGuid, std::deque<HealthSample>> _samples;
};

// Percent of the bot's own max health lost in the last two seconds.
class RecentHealthLossValue : public CalculatedValue<float>
{
public:
    RecentHealthLossValue(PlayerbotAI* botAI) : CalculatedValue<float>(botAI, "recent health loss") {}

    float Calculate() override;

private:
    HealthLossTracker _tracker;
};

// The group tank losing the most health in the last two seconds, when that loss reaches
// TankDamageSpikePct; target for external cooldowns (Pain Suppression, Guardian Spirit, Hand of Sacrifice).
class TankDamageSpikeTargetValue : public UnitCalculatedValue
{
public:
    static constexpr int32 CHECK_INTERVAL_MS = 200;

    TankDamageSpikeTargetValue(PlayerbotAI* botAI)
        : UnitCalculatedValue(botAI, "tank damage spike target", CHECK_INTERVAL_MS)
    {
    }

    Unit* Calculate() override;

private:
    HealthLossTracker _tracker;
};

#endif
