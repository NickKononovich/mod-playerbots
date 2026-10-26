/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPACTIONS_H
#define PLAYERBOTS_SPACTIONS_H

#include "AttackAction.h"
#include "TbcHazard.h"

// Mennu the Betrayer

// Everyone (the tank too, dragging Mennu with it) walks out of the nova radius of a live Corrupted Nova Totem.
class MennuLeaveNovaTotemAction : public TbcHazardMoveAction
{
public:
    MennuLeaveNovaTotemAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "mennu leave nova totem") {}
    bool Execute(Event event) override;
};

// Damage dealers kill, in order: a nova totem nobody stands next to (ranged only, its death nova then hits no
// one), the Healing Ward, the Earthgrab Totem that roots people inside the nova.
class MennuKillTotemsAction : public AttackAction
{
public:
    MennuKillTotemsAction(PlayerbotAI* botAI) : AttackAction(botAI, "mennu kill totems") {}
    bool Execute(Event event) override;
    bool isUseful() override;

private:
    Unit* PickTotem();
};

#endif
