/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MECHACTIONS_H
#define PLAYERBOTS_MECHACTIONS_H

#include "AttackAction.h"
#include "MechTriggers.h"
#include "MovementActions.h"
#include "ObjectGuid.h"
#include "TbcHazard.h"

class SepethreaKiteFlameAction : public MovementAction
{
public:
    SepethreaKiteFlameAction(PlayerbotAI* botAI) : MovementAction(botAI, "sepethrea kite flame") {}
    bool Execute(Event event) override;

private:
    uint32 _turnUntilMs = 0;
    float _turnSide = 1.0f;
    float _turnEnd = 1.0f;
    bool _turnLateral = false;
    ObjectGuid _turnFlame;
};

class SepethreaAvoidFlameAction : public MovementAction
{
public:
    SepethreaAvoidFlameAction(PlayerbotAI* botAI) : MovementAction(botAI, "sepethrea avoid flame") {}
    bool Execute(Event event) override;
};

class SepethreaAvoidTrailAction : public MovementAction
{
public:
    SepethreaAvoidTrailAction(PlayerbotAI* botAI) : MovementAction(botAI, "sepethrea avoid trail") {}
    bool Execute(Event event) override;
};

class SepethreaFocusBossAction : public AttackAction
{
public:
    SepethreaFocusBossAction(PlayerbotAI* botAI) : AttackAction(botAI, "sepethrea focus boss") {}
    bool Execute(Event event) override;
};

// The lift between the two levels (MechanarLift): wait at the landing for the deck, step onto it, ride it as a
// passenger standing still, step off at the other landing.
class MechanarRideLiftAction : public MovementAction
{
public:
    MechanarRideLiftAction(PlayerbotAI* botAI) : MovementAction(botAI, "mechanar ride lift") {}
    bool Execute(Event event) override;

private:
    // A walk over the deck's rim as given (3 points, no pathfinding: the navmesh has no polygons on the deck).
    bool WalkOverRim(G3D::Vector3 const& rim, G3D::Vector3 const& end);
    uint32 _landingWalkLogMs = 0;  // last [w24b-mech-landing-walk] line
};

// Capacitus heroic: to the stack of the bot's own charge (MechanarCapacitus::PolaritySpot).
class CapacitusPolarityAction : public TbcHazardMoveAction
{
public:
    CapacitusPolarityAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "capacitus polarity") {}
    bool Execute(Event event) override;

private:
    uint32 _logMs = 0;
};

// Capacitus normal: the cast into the magic shield is stopped, the melee damage dealer stops hitting the damage shield.
class CapacitusHoldFireAction : public Action
{
public:
    CapacitusHoldFireAction(PlayerbotAI* botAI) : Action(botAI, "capacitus hold fire") {}
    bool Execute(Event event) override;
};

// The bridge pack before Pathaleon: the pulling tank's ranged pull, the pack fought where the group stands.
class MechanarBridgePullAction : public Action
{
public:
    MechanarBridgePullAction(PlayerbotAI* botAI) : Action(botAI, "mechanar bridge pull") {}
    bool Execute(Event event) override;
};

#endif
