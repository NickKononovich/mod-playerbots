/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ANACTIONS_H
#define PLAYERBOTS_ANACTIONS_H

#include "ANTriggers.h"
#include "Action.h"
#include "AttackAction.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SelfTravelActions.h"
#include "TbcHazard.h"

class AttackWebWrapAction : public AttackAction
{
public:
    AttackWebWrapAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack web wrap") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class WatchersTargetAction : public AttackAction
{
public:
    WatchersTargetAction(PlayerbotAI* botAI) : AttackAction(botAI, "krik'thir priority") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class AnubarakFlankAction : public MovementAction
{
public:
    AnubarakFlankAction(PlayerbotAI* botAI) : MovementAction(botAI, "anub'arak flank") {}
    bool Execute(Event event) override;
};

class AnubarakDodgePoundAction : public AttackAction
{
public:
    AnubarakDodgePoundAction(PlayerbotAI* botAI) : AttackAction(botAI, "anub'arak dodge pound") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class AvoidAnubarakImpaleAction : public MovementAction
{
public:
    AvoidAnubarakImpaleAction(PlayerbotAI* botAI) : MovementAction(botAI, "avoid anub'arak impale") {}
    bool Execute(Event event) override;
};

class HadronoxReturnToPlatformAction : public MovementAction
{
public:
    HadronoxReturnToPlatformAction(PlayerbotAI* botAI) : MovementAction(botAI, "hadronox return to platform") {}
    bool Execute(Event event) override;
};

// The tank of Krik'thir's room: waits at the rally for the topped-up group, then opens on the nearest untouched
// Watcher pack at range and walks back to the rally (the "pull back" sequence), where the pack and every Watcher
// Krik'thir sends after it are fought.
class KrikthirWatcherPullAction : public MovementAction
{
public:
    KrikthirWatcherPullAction(PlayerbotAI* botAI) : MovementAction(botAI, "krik'thir watcher pull") {}
    bool Execute(Event event) override;

private:
    // Every member alive, gathered at the rally and topped up (relaxed after READY_WAIT_MS, and between packs).
    bool IsGroupReady(bool eventRunning);

    uint32 _waitSinceMs = 0;
    uint32 _waitCheckMs = 0;
    uint32 _waitLogMs = 0;
};

// Back to the rally out of an untouched pack's reach.
// [b34_an2] Also "krik'thir event rally" (KrikthirWatcherRallyTrigger): the word "event" exempts the walk from the
// harness waypoint lane (MoveConflict::ExemptFromLane), which vetoes a dungeon action's move of more than 10 yd
// against a live waypoint out of combat - the rally walk is the encounter's own position, not a detour.
class KrikthirBackToRallyAction : public TbcHazardMoveAction
{
public:
    KrikthirBackToRallyAction(PlayerbotAI* botAI, std::string const name = "krik'thir back to rally")
        : TbcHazardMoveAction(botAI, name)
    {
    }
    bool Execute(Event event) override;
};

class DropToLowerKingdomAction : public MovementAction
{
public:
    DropToLowerKingdomAction(PlayerbotAI* botAI) : MovementAction(botAI, "drop to lower kingdom") {}
    bool Execute(Event event) override;

private:
    // The fall from the rim's height at (x, y) ends in water deep enough to swim in.
    bool IsPoolBelow(float x, float y) const;
    // Landed on the pool's bottom: up to the surface, where the navmesh of the lower kingdom begins.
    bool SurfaceFromPool();

    // The walk down to the rim over the pool (steep slopes, longer than one PathGenerator path).
    SelfTravel::RouteWalk _rimRoute;
};

#endif
