/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_UKACTIONS_H
#define PLAYERBOTS_UKACTIONS_H

#include "Action.h"
#include "AttackAction.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "TbcHazard.h"
#include "UKTriggers.h"

class AttackFrostTombAction : public AttackAction
{
public:
    AttackFrostTombAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack frost tomb") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class AttackSkarvaldAction : public AttackAction
{
public:
    AttackSkarvaldAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack skarvald") {}
    bool Execute(Event event) override;
};

// Interrupts the living Dalronn's Shadow Bolt / Debilitate with the bot's interrupt (kick, counterspell, ...).
class DalronnInterruptAction : public Action
{
public:
    DalronnInterruptAction(PlayerbotAI* botAI) : Action(botAI, "dalronn interrupt") {}
    bool Execute(Event event) override;
};

// Leaves Smash / Dark Smash: to the nearest spot behind or beside Ingvar outside his boundary.
class IngvarAvoidSmashAction : public TbcHazardMoveAction
{
public:
    IngvarAvoidSmashAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "ingvar avoid smash") {}
    bool Execute(Event event) override;
};

class IngvarDodgeSmashAction : public MovementAction
{
public:
    IngvarDodgeSmashAction(PlayerbotAI* botAI) : MovementAction(botAI, "ingvar dodge smash") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class IngvarSmashReturnAction : public MovementAction
{
public:
    IngvarSmashReturnAction(PlayerbotAI* botAI) : MovementAction(botAI, "ingvar smash return") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
