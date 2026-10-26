/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CLASSICACTIONS_H
#define PLAYERBOTS_CLASSICACTIONS_H

#include "AttackAction.h"
#include "ObjectGuid.h"
#include "SelfTravelActions.h"
#include "TbcHazard.h"

#include <map>

// Arcanist Doan's Detonation: the Library is too small to always get 30 yd, so the bot runs to the spot with the
// shortest walk that is out of range or behind a wall from Doan. The spot is chosen once per cast.
class DoanEscapeDetonationAction : public TbcHazardMoveAction
{
public:
    DoanEscapeDetonationAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "doan escape detonation") {}
    bool Execute(Event event) override;

private:
    bool ChooseSpot(Unit* doan);

    uint32 _spotExpiresMs = 0;
    float _spotX = 0.0f;
    float _spotY = 0.0f;
    float _spotZ = 0.0f;
};

// Ranged and healers stay out of Arcane Explosion.
class DoanKeepRangeAction : public TbcHazardMoveAction
{
public:
    DoanKeepRangeAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "doan keep range") {}
    bool Execute(Event event) override;
};

// Scholomance: steps out of every Acolyte's Mana Burn pick to the nearest spot that keeps the tank (healer) or the
// current target (ranged) in range and sight, stays off melee and out of idle mobs' aggro, and if it can also out
// of the Spectral Researchers' 30 yd. The spot is kept a few seconds so the bot does not wander.
class ScholoAvoidManaBurnAction : public TbcHazardMoveAction
{
public:
    ScholoAvoidManaBurnAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "scholo avoid mana burn") {}
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    bool ChooseSpot();
    bool SpotIsClear(float x, float y) const;

    uint32 _spotExpiresMs = 0;
    float _spotX = 0.0f;
    float _spotY = 0.0f;
    float _spotZ = 0.0f;
};

// Scarlet Monastery Cathedral: steps out of an idle Commander Mograine's reach while his chapel stands.
class MograineKeepAwayAction : public TbcHazardMoveAction
{
public:
    MograineKeepAwayAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "mograine keep away") {}
    bool Execute(Event event) override;
};

// Scholomance hall: back up the entrance ramp to its top after a fight on the hall floor, out of the next packs'
// reach; the next pull is planned from there.
class ScholoHallRallyAction : public MovementAction
{
public:
    ScholoHallRallyAction(PlayerbotAI* botAI) : MovementAction(botAI, "scholo hall rally") {}
    bool Execute(Event event) override;
};

// [b31_ubrs_door] UBRS stadium: out of the drake pen to the stadium side while the wave portcullis is up, in combat
// too - the mobs follow, and the gate locks a bot inside for good once Gyth is out (ClassicShared.h BlackrockSpire).
class BrsLeaveDrakePenAction : public MovementAction
{
public:
    BrsLeaveDrakePenAction(PlayerbotAI* botAI) : MovementAction(botAI, "brs leave drake pen") {}
    bool Execute(Event event) override;
};

// [b32_brs] Scholomance Viewing Room: the tank attacks Vectus / Marduk Blackpool (neutral, npc flags: the aggressive
// target never takes them) once Dawn's Gambit turned the students (Scholomance::ViewingRoomBossToAttack).
class ScholoAttackViewingRoomBossAction : public AttackAction
{
public:
    ScholoAttackViewingRoomBossAction(PlayerbotAI* botAI) : AttackAction(botAI, "scholo attack viewing room boss") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

// [b32_st] Sunken Temple, the Shade of Hakkar event: a damage dealer attacks the nearest idle Nightmare Suppressor
// (SunkenTemple::SuppressorToAttack) before it drains the Shade.
class StAttackSuppressorAction : public AttackAction
{
public:
    StAttackSuppressorAction(PlayerbotAI* botAI) : AttackAction(botAI, "st attack suppressor") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

// Walks to the object of the next event step, waits for the group where the step starts a fight, then does it.
class DungeonEventStepAction : public MovementAction
{
public:
    DungeonEventStepAction(PlayerbotAI* botAI) : MovementAction(botAI, "dungeon event step") {}
    bool Execute(Event event) override;

private:
    GuidSet _done;                               // objects this bot used its item on (planted barrels)
    std::map<ObjectGuid, uint32> _pausedUntil;   // no walk there, or used a moment ago
    ObjectGuid _castOn;                          // the item cast under way and its object
    uint32 _castStartMs = 0;
    uint32 _castMs = 0;
    ObjectGuid _pathChecked;
    uint32 _pathCheckedMs = 0;
    // The walk to the step's object when PathGenerator's path is cut short (SelfTravel::NextRouteLeg).
    SelfTravel::RouteWalk _route;
    bool _routeWalk = false;
    // [b32_pull] the leader's walk waits for the group (DungeonInteract::GroupLaggers)
    uint32 _groupWaitSinceMs = 0;
    uint32 _groupWaitLogMs = 0;
    bool _stepWalking = false;
};

#endif
