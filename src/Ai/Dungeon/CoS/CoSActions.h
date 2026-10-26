/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COSACTIONS_H
#define PLAYERBOTS_COSACTIONS_H

#include "Action.h"
#include "AttackAction.h"
#include "CoSTriggers.h"
#include "GenericSpellActions.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

class ExplodeGhoulSpreadAction : public MovementAction
{
public:
    ExplodeGhoulSpreadAction(PlayerbotAI* botAI) : MovementAction(botAI, "explode ghoul spread") {}
    bool Execute(Event event) override;
};

class EpochStackAction : public MovementAction
{
public:
    EpochStackAction(PlayerbotAI* botAI) : MovementAction(botAI, "epoch stack") {}
    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // [b44_cos] the chosen stack spot, kept while the boss stays put (the navmesh test of the candidates is costly)
    bool FindStackSpot(Unit* boss, Unit* tank, float& x, float& y, float& z);
    uint32 _spotMs = 0;
    float _spotX = 0.0f;
    float _spotY = 0.0f;
    float _spotZ = 0.0f;
    float _spotBossX = 0.0f;
    float _spotBossY = 0.0f;
    uint32 _failLogMs = 0;
};

class CoSGetArcaneDisruptorAction : public MovementAction
{
public:
    CoSGetArcaneDisruptorAction(PlayerbotAI* botAI) : MovementAction(botAI, "cos get arcane disruptor") {}
    bool Execute(Event event) override;
    bool isUseful() override;

private:
    uint32 _failedTalkMs = 0;  // a talk that brought no Disruptor: the bot does other things for a while
};

class CoSDispelCrateAction : public MovementAction
{
public:
    CoSDispelCrateAction(PlayerbotAI* botAI) : MovementAction(botAI, "cos dispel crate") {}
    bool Execute(Event event) override;
};

class CoSStartArthasIntroAction : public MovementAction
{
public:
    CoSStartArthasIntroAction(PlayerbotAI* botAI) : MovementAction(botAI, "cos start arthas intro") {}
    bool Execute(Event event) override;
};

class CoSArthasGossipAction : public MovementAction
{
public:
    CoSArthasGossipAction(PlayerbotAI* botAI) : MovementAction(botAI, "cos arthas gossip") {}
    bool Execute(Event event) override;
};

class CoSEngageEventTargetAction : public AttackAction
{
public:
    CoSEngageEventTargetAction(PlayerbotAI* botAI) : AttackAction(botAI, "cos engage event target") {}
    bool Execute(Event event) override;

private:
    uint32 _defendLogMs = 0;
    uint32 _leashHoldSinceMs = 0;  // [b33_cos2] the tank holds at the edge of the healer's range since
};

class CoSFollowArthasAction : public MovementAction
{
public:
    CoSFollowArthasAction(PlayerbotAI* botAI) : MovementAction(botAI, "cos follow arthas") {}
    bool Execute(Event event) override;

private:
    uint32 _regroupLogMs = 0;
};

#endif
