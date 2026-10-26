/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BMTRIGGERS_H
#define PLAYERBOTS_BMTRIGGERS_H

#include "Trigger.h"

class Unit;

namespace BMShared
{
// The tank's keeper fetch applies: an un-engaged keeper is alive and Medivh can spare the tank.
Unit* GuardianToFetch(PlayerbotAI* botAI);
// The keeper the tank holds while it is still away from the hold spot.
Unit* GuardianToDrag(PlayerbotAI* botAI);
// A rift boss with Hasten / Enrage this bot can take off, or nullptr.
Unit* BossBuffToRemove(PlayerbotAI* botAI);
}

// An Infinite add walks to Medivh or drains his shield: the damage dealers take it first.
class BlackMorassMedivhThreatenedTrigger : public Trigger
{
public:
    BlackMorassMedivhThreatenedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bm medivh threatened") {}
    bool IsActive() override;
};

// No add channels and a keeper or rift boss fights at Medivh: the damage dealers kill it (its rift closes).
class BlackMorassKillRiftGuardianTrigger : public Trigger
{
public:
    BlackMorassKillRiftGuardianTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bm kill rift guardian") {}
    bool IsActive() override;
};

// A non-tank left Medivh during the portals (followed the tank to a rift, chased a keeper).
class BlackMorassHoldNearMedivhTrigger : public Trigger
{
public:
    BlackMorassHoldNearMedivhTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bm hold near medivh") {}
    bool IsActive() override;
};

class BlackMorassFetchRiftGuardianTrigger : public Trigger
{
public:
    BlackMorassFetchRiftGuardianTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bm fetch rift guardian") {}
    bool IsActive() override;
};

class BlackMorassDragRiftGuardianTrigger : public Trigger
{
public:
    BlackMorassDragRiftGuardianTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bm drag rift guardian") {}
    bool IsActive() override;
};

// Before the start, near Sa'at, without a Chrono-beacon.
class BlackMorassChronoBeaconFromSaatTrigger : public Trigger
{
public:
    BlackMorassChronoBeaconFromSaatTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bm chrono beacon from saat") {}
    bool IsActive() override;
};

// A hard rift (13-17) presses Medivh and the bot carries a beacon.
class BlackMorassUseChronoBeaconTrigger : public Trigger
{
public:
    BlackMorassUseChronoBeaconTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bm use chrono beacon") {}
    bool IsActive() override;
};

class BlackMorassBossBuffTrigger : public Trigger
{
public:
    BlackMorassBossBuffTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bm boss buff") {}
    bool IsActive() override;
};

// The tank's trash fight spot (FightHold, PullActions.h) is not Medivh's hold spot while the portals run: the event
// is one fight that never leaves combat, so the spot of the last pull before it stays (bm-n 20261025-153805: 95 yd
// out, 1681 "[fight hold]" caps walked the tank away from Medivh and his dragged keepers back to their rifts).
class BlackMorassFightSpotAtMedivhTrigger : public Trigger
{
public:
    BlackMorassFightSpotAtMedivhTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bm fight spot at medivh") {}
    bool IsActive() override;
};

#endif
