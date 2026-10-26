/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MTACTIONS_H
#define PLAYERBOTS_MTACTIONS_H

#include "Action.h"
#include "AttackAction.h"

// Nexus-Prince Shaffar: damage dealers kill each Ethereal Beacon as it appears (nearest first), then any
// apprentice one turned into, before going back to Shaffar.
class ShaffarKillBeaconAction : public AttackAction
{
public:
    ShaffarKillBeaconAction(PlayerbotAI* botAI) : AttackAction(botAI, "shaffar kill beacon") {}
    bool Execute(Event event) override;
    bool isUseful() override;

private:
    Unit* PickAdd();
};

// Pandemonius: stop melee swings and casts at him while Dark Shell reflects and burns attackers.
class PandemoniusStopAttackAction : public Action
{
public:
    PandemoniusStopAttackAction(PlayerbotAI* botAI) : Action(botAI, "pandemonius stop attack") {}
    bool Execute(Event event) override;
};

#endif
