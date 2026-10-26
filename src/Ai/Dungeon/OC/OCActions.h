/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_OCACTIONS_H
#define PLAYERBOTS_OCACTIONS_H

#include "Action.h"
#include "AttackAction.h"
#include "GenericSpellActions.h"
#include "PlayerbotAI.h"
#include "UseItemAction.h"

const Position uromSafePositions[3] =
{
    Position(1138.88f, 1052.22f, 508.36f),
    Position(1084.62f, 1079.71f, 508.36f),
    Position(1087.42f, 1020.132f, 508.36f)
};

class MountDrakeAction : public UseItemAction
{
public:
    MountDrakeAction(PlayerbotAI* botAI) : UseItemAction(botAI, "mount drake") {}
    bool Execute(Event event) override;
    bool isPossible() override;

private:
    uint32 GetDrakeEssenceForRole();
    bool RequestDrakeEssence(uint32 itemId);
};

class DismountDrakeAction : public Action
{
public:
    DismountDrakeAction(PlayerbotAI* botAI) : Action(botAI, "dismount drake") {}
    bool Execute(Event event) override;
};

class OccFlyDrakeAction : public MovementAction
{
public:
    OccFlyDrakeAction(PlayerbotAI* botAI) : MovementAction(botAI, "occ fly drake") {}
    bool Execute(Event event) override;

private:
    bool SteerTowardsGoal(Unit* vehicleBase);
    bool FlyTo(Unit* vehicleBase, Position const& destination);
    bool FollowFormationLeader(Unit* vehicleBase, Player* leader);
    Position _lastSteerDestination;
    bool _hasSteerDestination = false;
};

class OccDrakeAttackAction : public Action
{
public:
    OccDrakeAttackAction(PlayerbotAI* botAI) : Action(botAI, "occ drake attack") {}
    bool Execute(Event event) override;

protected:
    Unit* vehicleBase;
    bool CastDrakeSpellAction(Unit* target, uint32 spellId, uint32 cooldown);
    bool AmberDrakeAction(Unit* target);
    bool EmeraldDrakeAction(Unit* target);
    bool RubyDrakeAction(Unit* target);
};

// [b36_occ] Eregos' Planar Anomalies: the drake flies to the spot round it farthest from every anomaly (Planar Blast
// 20 yd) while staying near the fight, and holds the motion until they are gone (the boss is immune meanwhile).
class AvoidPlanarAnomalyAction : public MovementAction
{
public:
    AvoidPlanarAnomalyAction(PlayerbotAI* botAI) : MovementAction(botAI, "avoid planar anomaly") {}
    bool Execute(Event event) override;

private:
    Position _fleeDestination;
    bool _hasFleeDestination = false;
};

class AvoidArcaneExplosionAction : public MovementAction
{
public:
    AvoidArcaneExplosionAction(PlayerbotAI* botAI) : MovementAction(botAI, "avoid arcane explosion") {}
    bool Execute(Event event) override;
};

class TimeBombSpreadAction : public MovementAction
{
public:
    TimeBombSpreadAction(PlayerbotAI* botAI) : MovementAction(botAI, "time bomb spread") {}
    bool Execute(Event event) override;
};

class OcUseNexusPortalAction : public MovementAction
{
public:
    OcUseNexusPortalAction(PlayerbotAI* botAI) : MovementAction(botAI, "oc use nexus portal") {}
    bool Execute(Event event) override;
};

class OcPullBossAction : public AttackAction
{
public:
    OcPullBossAction(PlayerbotAI* botAI) : AttackAction(botAI, "oc pull boss") {}
    bool Execute(Event event) override;
};

// Landed before the rest of the group: walk back to the own landing spot and wait there (no wandering into a pack).
class OcHoldLandingSpotAction : public MovementAction
{
public:
    OcHoldLandingSpotAction(PlayerbotAI* botAI) : MovementAction(botAI, "oc hold landing spot") {}
    bool Execute(Event event) override;
};

class AvoidEnergizeCoresAction : public MovementAction
{
public:
    AvoidEnergizeCoresAction(PlayerbotAI* botAI) : MovementAction(botAI, "avoid energize cores") {}
    bool Execute(Event event) override;
};

#endif
