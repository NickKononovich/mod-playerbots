/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_POSACTIONS_H
#define PLAYERBOTS_POSACTIONS_H

#include "Action.h"
#include "AttackAction.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PoSTriggers.h"
#include "TbcHazard.h"  // [b33_pos]

const Position ICKANDKRICK_TANK_POSITION = Position(816.8508f, 102.331505f, 509.1586f);

class IckAndKrickAction : public AttackAction
{
public:
    IckAndKrickAction(PlayerbotAI* botAI) : AttackAction(botAI, "ick and krick") {}
    bool Execute(Event event) override;

    bool TankPosition(Unit* boss);
    bool Pursuit(bool pursuit, Unit* boss);
    bool PoisonNova(bool poisonNova, Unit* boss);
    bool ExplosiveBarrage(bool explosiveBarrage, Unit* boss);
};

class TyrannusAction : public AttackAction
{
public:
    TyrannusAction(PlayerbotAI* botAI) : AttackAction(botAI, "tyrannus") {}
    bool Execute(Event event) override;

    bool RangedSpread(bool rangedSpread);
};

class PoSSendAreaTriggerAction : public Action
{
public:
    PoSSendAreaTriggerAction(PlayerbotAI* botAI) : Action(botAI, "pos send area trigger") {}
    bool Execute(Event event) override;
};

class PoSHideFromPermafrostAction : public MovementAction
{
public:
    PoSHideFromPermafrostAction(PlayerbotAI* botAI) : MovementAction(botAI, "pos hide from permafrost") {}
    bool Execute(Event event) override;
};

class PoSDodgeSaroniteAction : public MovementAction
{
public:
    PoSDodgeSaroniteAction(PlayerbotAI* botAI) : MovementAction(botAI, "pos dodge saronite") {}
    bool Execute(Event event) override;
};

class PoSDodgeIcicleAction : public MovementAction
{
public:
    PoSDodgeIcicleAction(PlayerbotAI* botAI) : MovementAction(botAI, "pos dodge icicle") {}
    bool Execute(Event event) override;
};

class PoSBrandHoldFireAction : public Action
{
public:
    PoSBrandHoldFireAction(PlayerbotAI* botAI) : Action(botAI, "pos brand hold fire") {}
    bool Execute(Event event) override;
};

class PoSAvoidHoarfrostAction : public MovementAction
{
public:
    PoSAvoidHoarfrostAction(PlayerbotAI* botAI) : MovementAction(botAI, "pos avoid hoarfrost") {}
    bool Execute(Event event) override;
};

// [b33_pos] Away from the freed slaves' line: the tank drags Tyrannus to PoSIds::TYRANNUS_TANK_SPOT, the others
// walk out of the Fallen Warriors' way in.
// A hazard move (TbcHazardMoveAction): the hazard hold multiplier must not pin the tank beside Tyrannus standing in a
// puddle; the destination itself is picked outside every Icy Blast puddle.
class PoSTyrannusPositionAction : public TbcHazardMoveAction
{
public:
    PoSTyrannusPositionAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "pos tyrannus position") {}
    bool Execute(Event event) override;
};
#endif
