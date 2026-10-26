/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DAMAGESPIKETRIGGERS_H
#define PLAYERBOTS_DAMAGESPIKETRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

// The bot itself lost TankDamageSpikePct of its max health in the last two seconds (own survival cooldowns).
class DamageSpikeTrigger : public Trigger
{
public:
    DamageSpikeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "damage spike") {}

    bool IsActive() override;
};

// A tank under pressure, as a player reads its health bar: below TankCooldownHealthPct and still falling by
// TankCooldownLossPct in two seconds, or below LOW_HEALTH_SHARE of that whatever the trend. For the defensive
// cooldowns a tank rotates through a hard pull (Divine Protection, Shield Wall, Icebound Fortitude, Survival
// Instincts): "damage spike" (30% in two seconds) and "critical health" (25%) left them unused while wave 10 heroic
// tanks died under 3-4k damage per second.
class TankPressureTrigger : public Trigger
{
public:
    static constexpr float LOW_HEALTH_SHARE = 0.7f;

    TankPressureTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tank pressure") {}

    bool IsActive() override;
};

// A group tank lost TankDamageSpikePct of its max health in the last two seconds (external cooldowns).
class TankDamageSpikeTrigger : public Trigger
{
public:
    TankDamageSpikeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tank damage spike") {}

    std::string const GetTargetName() override { return "tank damage spike target"; }
    bool IsActive() override;
};

#endif
