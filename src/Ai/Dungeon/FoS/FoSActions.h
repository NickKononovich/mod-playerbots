/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FOSACTIONS_H
#define PLAYERBOTS_FOSACTIONS_H

#include "Action.h"
#include "AttackAction.h"
#include "FoSTriggers.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

const Position BRONJAHM_TANK_POSITION = Position(5297.920f, 2506.698f, 686.068f);

class MoveFromBronjahmAction : public MovementAction
{
public:
    MoveFromBronjahmAction(PlayerbotAI* botAI) : MovementAction(botAI, "move from bronjahm") {}
    bool Execute(Event event) override;
};

class AttackCorruptedSoulFragmentAction : public AttackAction
{
public:
    AttackCorruptedSoulFragmentAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack corrupted soul fragment") {}
    bool Execute(Event event) override;
};

class BronjahmGroupPositionAction : public AttackAction
{
public:
    BronjahmGroupPositionAction(PlayerbotAI* botAI) : AttackAction(botAI, "bronjahm group position") {}

    bool Execute(Event event) override;

    bool isUseful() override;

private:
    // Soulstorm: everyone, ranged and healers too, at Bronjahm's feet inside the 10 yd eye.
    bool StackInSoulstorm(Unit* boss);
};

// Mirrored Soul copies 45% of the damage the Devourer takes onto a player: everyone stops damaging it.
class DevourerOfSoulsAction : public Action
{
public:
    DevourerOfSoulsAction(PlayerbotAI* botAI) : Action(botAI, "devourer of souls") {}
    bool Execute(Event event) override;
};

// Wailing Souls sweeps a beam (104 degrees wide in heroic) up to 87 degrees from the Devourer's facing in a random
// direction: run to the narrow arc straight behind it that no remaining beam can reach.
class AvoidWailingSoulsAction : public MovementAction
{
public:
    AvoidWailingSoulsAction(PlayerbotAI* botAI) : MovementAction(botAI, "fos avoid wailing souls") {}
    bool Execute(Event event) override;
};

class FoSInterruptAction : public Action
{
public:
    FoSInterruptAction(PlayerbotAI* botAI) : Action(botAI, "fos interrupt") {}
    bool Execute(Event event) override;
};

#endif
