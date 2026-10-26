/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BMACTIONS_H
#define PLAYERBOTS_BMACTIONS_H

#include "AttackAction.h"
#include "MovementActions.h"
#include "UseItemAction.h"

// Attacks the first add of "bm medivh threats" (a Corrupt Medivh caster first) unless already on one of them.
class BlackMorassDefendMedivhAction : public AttackAction
{
public:
    BlackMorassDefendMedivhAction(PlayerbotAI* botAI) : AttackAction(botAI, "bm defend medivh") {}
    bool Execute(Event event) override;
};

// Attacks the keeper or rift boss fighting at Medivh (BMShared::GuardianAtMedivh).
class BlackMorassKillRiftGuardianAction : public AttackAction
{
public:
    BlackMorassKillRiftGuardianAction(PlayerbotAI* botAI) : AttackAction(botAI, "bm kill rift guardian") {}
    bool Execute(Event event) override;
};

// Back to the hold spot beside Medivh; drops a keeper target far from him first.
class BlackMorassHoldNearMedivhAction : public MovementAction
{
public:
    BlackMorassHoldNearMedivhAction(PlayerbotAI* botAI) : MovementAction(botAI, "bm hold near medivh") {}
    bool Execute(Event event) override;
};

// The tank walks to the keeper and opens on it from range (a taunt or a ranged pull).
class BlackMorassFetchRiftGuardianAction : public AttackAction
{
public:
    BlackMorassFetchRiftGuardianAction(PlayerbotAI* botAI) : AttackAction(botAI, "bm fetch rift guardian") {}
    bool Execute(Event event) override;
};

// The tank holding a keeper walks back to Medivh; the keeper follows it there.
class BlackMorassDragRiftGuardianAction : public MovementAction
{
public:
    BlackMorassDragRiftGuardianAction(PlayerbotAI* botAI) : MovementAction(botAI, "bm drag rift guardian") {}
    bool Execute(Event event) override;
};

// Sa'at's gossip (menu 8088) as a client sends it; the server's conditions decide whether a beacon is offered.
class BlackMorassChronoBeaconFromSaatAction : public MovementAction
{
public:
    BlackMorassChronoBeaconFromSaatAction(PlayerbotAI* botAI) : MovementAction(botAI, "bm chrono beacon from saat") {}
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    uint32 _retryAtMs = 0;
};

class BlackMorassUseChronoBeaconAction : public UseItemAction
{
public:
    BlackMorassUseChronoBeaconAction(PlayerbotAI* botAI) : UseItemAction(botAI, "bm use chrono beacon", true) {}
    bool isPossible() override { return true; }
    bool isUseful() override;
    bool Execute(Event event) override;
};

// Temporus' Hasten (purge, spellsteal, dispel magic, tranquilizing shot) and Aeonus' Enrage (tranquilizing shot).
class BlackMorassRemoveBossBuffAction : public Action
{
public:
    BlackMorassRemoveBossBuffAction(PlayerbotAI* botAI) : Action(botAI, "bm remove boss buff") {}
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    uint32 _retryAtMs = 0;
};

// Moves the tank's trash fight spot (FightHold) to Medivh's hold spot: its chase walks then end by Medivh, where the
// adds come and the keepers are dragged to (BlackMorassFightSpotAtMedivhTrigger). No movement of its own.
class BlackMorassFightSpotAtMedivhAction : public Action
{
public:
    BlackMorassFightSpotAtMedivhAction(PlayerbotAI* botAI) : Action(botAI, "bm fight spot at medivh") {}
    bool Execute(Event event) override;
};

#endif
