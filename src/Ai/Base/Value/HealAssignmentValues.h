/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HEALASSIGNMENTVALUES_H
#define PLAYERBOTS_HEALASSIGNMENTVALUES_H

#include <list>
#include <unordered_map>

#include "DamageSpikeValues.h"
#include "ObjectGuid.h"
#include "Value.h"

class PlayerbotAI;
class Unit;

// Heal amount other group members are currently casting on each unit (the bot's own cast excluded).
// A unit present with amount 1 is being healed by a spell whose amount cannot be estimated.
typedef std::unordered_map<ObjectGuid, uint32> IncomingHealMap;

// Health percent the unit will have once the heals already being cast on it land (capped at 100).
float PredictedHealthPct(Unit const* unit, IncomingHealMap const& incoming);

class IncomingHealsValue : public CalculatedValue<IncomingHealMap>
{
public:
    static constexpr uint32 CHECK_INTERVAL_MS = 100;  // about one bot tick

    IncomingHealsValue(PlayerbotAI* botAI)
        : CalculatedValue<IncomingHealMap>(botAI, "incoming heals", CHECK_INTERVAL_MS)
    {
    }

    IncomingHealMap Calculate() override;
};

// Raid healer assignment computed the same way by every healer from the group roster: one healer per
// tank (two on the main tank with five or more healers), the rest split over the raid subgroups.
// Empty outside raid instances, with fewer than two healers, or when the bot is not a healer.
class AutoFocusHealTargetsValue : public CalculatedValue<std::list<ObjectGuid>>
{
public:
    AutoFocusHealTargetsValue(PlayerbotAI* botAI)
        : CalculatedValue<std::list<ObjectGuid>>(botAI, "auto focus heal targets", 5)
    {
    }

    std::list<ObjectGuid> Calculate() override;
};

// Percent of max health each group member lost in the last two seconds, net of healing (see HealthLossTracker).
typedef std::unordered_map<ObjectGuid, float> HealthLossMap;

// The unit's entry in the map; 0 for a unit losing nothing or not tracked (pets).
float RecentHealthLossPct(Unit const* unit, HealthLossMap const& losses);

// Healer triage: who is losing health fast. Empty when HealerHealthLossWeight and HealerFastHealthLossPct are 0.
class GroupHealthLossValue : public CalculatedValue<HealthLossMap>
{
public:
    static constexpr uint32 CHECK_INTERVAL_MS = HealthLossTracker::SAMPLE_INTERVAL_MS;

    GroupHealthLossValue(PlayerbotAI* botAI)
        : CalculatedValue<HealthLossMap>(botAI, "group health loss", CHECK_INTERVAL_MS)
    {
    }

    HealthLossMap Calculate() override;

private:
    HealthLossTracker _tracker;
};

#endif
