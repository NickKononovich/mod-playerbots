/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GDACTIONS_H
#define PLAYERBOTS_GDACTIONS_H

#include "Action.h"
#include "AttackAction.h"
#include "GDTriggers.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

class AvoidPoisonNovaAction : public MovementAction
{
public:
    AvoidPoisonNovaAction(PlayerbotAI* botAI) : MovementAction(botAI, "avoid poison nova") {}
    bool Execute(Event event) override;
};

class AttackSnakeWrapAction : public AttackAction
{
public:
    AttackSnakeWrapAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack snake wrap") {}
    bool Execute(Event event) override;
};

class SladranStackOnTankAction : public MovementAction
{
public:
    SladranStackOnTankAction(PlayerbotAI* botAI) : MovementAction(botAI, "slad'ran stack on tank") {}
    bool Execute(Event event) override;
};

class SladranTankHoldAction : public AttackAction
{
public:
    SladranTankHoldAction(PlayerbotAI* botAI) : AttackAction(botAI, "slad'ran tank hold") {}
    bool Execute(Event event) override;
};

// The tank walks Slad'ran back onto his platform.
class SladranTankParkAction : public MovementAction
{
public:
    SladranTankParkAction(PlayerbotAI* botAI) : MovementAction(botAI, "slad'ran tank park") {}
    bool Execute(Event event) override;
};

class AvoidWhirlingSlashAction : public MovementAction
{
public:
    AvoidWhirlingSlashAction(PlayerbotAI* botAI) : MovementAction(botAI, "avoid whirling slash") {}
    bool Execute(Event event) override;
};

class GundrakUseAltarAction : public MovementAction
{
public:
    GundrakUseAltarAction(PlayerbotAI* botAI) : MovementAction(botAI, "gundrak use altar") {}
    bool Execute(Event event) override;
};

class EngageLivingMojoAction : public AttackAction
{
public:
    EngageLivingMojoAction(PlayerbotAI* botAI) : AttackAction(botAI, "engage living mojo") {}
    bool Execute(Event event) override;
};

class GundrakCrossBridgeAction : public MovementAction
{
public:
    GundrakCrossBridgeAction(PlayerbotAI* botAI) : MovementAction(botAI, "gundrak cross bridge") {}
    bool Execute(Event event) override;

private:
    bool WalkStraightTo(Position const& point);
};

// [b34_gd] Into the moat after the mobs fighting the group from it (GundrakEvent::FindMoatAttacker): along the deck
// to its open stretch between the statue island and the east landing, off its side, down to the water.
class GundrakMoatDropAction : public MovementAction
{
public:
    GundrakMoatDropAction(PlayerbotAI* botAI) : MovementAction(botAI, "gundrak moat drop") {}
    bool Execute(Event event) override;
};

// [b34_gd] Out of the moat to the bridge's west start (GundrakEvent::ShouldLeaveMoat).
class GundrakMoatExitAction : public MovementAction
{
public:
    GundrakMoatExitAction(PlayerbotAI* botAI) : MovementAction(botAI, "gundrak moat exit") {}
    bool Execute(Event event) override;
};

// Off the central circle's pool bottom (GundrakCircle) as a player swims out: straight up to a surface point of the
// pool over the bottom, then the navmesh path to its shore, in one spline.
class GundrakCircleRouteAction : public MovementAction
{
public:
    GundrakCircleRouteAction(PlayerbotAI* botAI) : MovementAction(botAI, "gundrak circle route") {}
    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool IsLeavingPool() const;

    uint32 _swimStartMs = 0;
    uint32 _swimDurationMs = 0;
    uint32 _refusedMs = 0;
};

#endif
