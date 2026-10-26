/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_OCMULTIPLIERS_H
#define PLAYERBOTS_OCMULTIPLIERS_H

#include "Multiplier.h"
#include "Unit.h"

#include <string>

const float uromCoords[4][4] =
{   // Platform coordinates
    {1177.47f, 937.722f, 527.405f, 2.21657f},
    {968.66f, 1042.53f, 527.32f, 0.077f},
    {1164.02f, 1170.85f, 527.321f, 3.66f},
    {1118.31f, 1080.377f, 508.361f, 4.25f}      // Inner ring, actual boss fight
};

class MountingDrakeMultiplier : public Multiplier
{
    public:
        MountingDrakeMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "mounting drake") {}

    public:
        float GetValue(Action* action) override;
};

// The ring platforms before Varos: the tank starts no pull while a member is still flying in on its drake or has
// not caught up yet (occ-h 2026-10-25, builds 7 and 9: the tank dismounted first and pulled two Ring-Lord packs while
// the mage and the shaman were still mounting on the last ring; the whole group died to the second one).
class OccRingGatherMultiplier : public Multiplier
{
public:
    OccRingGatherMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "occ ring gather") {}

    float GetValue(Action* action) override;

private:
    uint32 _waitSinceMs = 0;  // since when a member is away (bounded wait, RING_GATHER_TIMEOUT_MS)
};

// No pull starts while the healer is not with the tank: "aggressive target" for everybody, and the tank's pull
// starters and attacks on idle mobs, while an alive healer is > 40 yd from the tank or still on its drake (occ-h
// 2026-10-25: pull_without_healer with the healer 87-187 yd away, three wipes on the Construct islands).
class OccHealerGatherMultiplier : public Multiplier
{
public:
    OccHealerGatherMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "occ healer gather") {}

    float GetValue(Action* action) override;

private:
    bool IsHealerAway();

    uint32 _checkedMs = 0;
    bool _healerAway = false;
    std::string _awayWhy;
};

// Landed before the rest of the group: every movement but the walk back to the landing spot and the hazard dodges
// waits (OculusShouldHoldLanding), so nobody wanders into an island pack's reach alone.
class OccLandingHoldMultiplier : public Multiplier
{
public:
    OccLandingHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "occ landing hold") {}

    float GetValue(Action* action) override;

private:
    uint32 _checkedMs = 0;
    bool _holding = false;
};

// The last 30 yd to the Nexus Portal are walked on foot: its teleport fails on a mounted clicker, and after a wipe
// "check mount state" mounted the tank again between two clicks (occ-h 2026-10-25 132016 / 115805: 4 min stall).
class OccPortalMountMultiplier : public Multiplier
{
public:
    OccPortalMountMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "occ portal mount") {}

    float GetValue(Action* action) override;
};

class OccFlyingMultiplier : public Multiplier
{
    public:
        OccFlyingMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "occ flying drake") {}

    public:
        float GetValue(Action* action) override;
};

class UromMultiplier : public Multiplier
{
    public:
        UromMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "mage-lord urom") {}

    public:
        float GetValue(Action* action) override;
    protected:
        uint8 GetPhaseByCurrentPosition(Unit* boss);
};

class EregosMultiplier : public Multiplier
{
    public:
        EregosMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "ley-guardian eregos") {}

    public:
        float GetValue(Action* action) override;
};

#endif
