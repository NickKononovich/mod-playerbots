/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TERRAINHAZARDACTIONS_H
#define PLAYERBOTS_TERRAINHAZARDACTIONS_H

#include "MovementActions.h"
#include "Trigger.h"

// User, Ragefire Chasm 2026-10-24: «обязательно танковать мобов на краю лавы?» A dungeon tank holding a mob within
// TERRAIN_HAZARD_FIGHT_RADIUS of lava, slime or a drop-off (TerrainHazard.h) drags it a few yards to open floor:
// away from the hazard, into no idle pack's aggro, in the healer's sight. At most FIGHT_HAZARD_MAX_MOVES per fight;
// "[hazard] <tank>: ..." says why.
class FightNearHazardTrigger : public Trigger
{
public:
    FightNearHazardTrigger(PlayerbotAI* botAI) : Trigger(botAI, "fight near hazard") {}

    bool IsActive() override;
};

class MoveFightOffHazardAction : public MovementAction
{
public:
    MoveFightOffHazardAction(PlayerbotAI* botAI) : MovementAction(botAI, "move fight off hazard") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
