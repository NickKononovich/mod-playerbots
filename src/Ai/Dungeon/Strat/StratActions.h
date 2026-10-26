/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STRATACTIONS_H
#define PLAYERBOTS_STRATACTIONS_H

#include <vector>

#include "AttackAction.h"
#include "MovementActions.h"
#include "StratShared.h"

// Steps to the nearest spot around the tank that no idle mob would aggro on before the tank (StratShared.h), on the
// tank's side away from the pack: the tank stays the first one a wanderer meets.
class StratStepBehindTankAction : public MovementAction
{
public:
    StratStepBehindTankAction(PlayerbotAI* botAI) : MovementAction(botAI, "strat step behind tank") {}
    bool Execute(Event event) override;

private:
    bool ChooseSpot(Player* tank, std::vector<Stratholme::IdleMob> const& mobs);

    float _spotX = 0.0f;
    float _spotY = 0.0f;
    float _spotZ = 0.0f;
    uint32 _spotExpiresMs = 0;
};

// The tank takes the mob that hits a member while the tank itself is out of combat: Righteous Defense on the member
// (a paladin), else its taunt on the mob, then attacks it; the combat peel takes over from there.
class StratDefendMemberAction : public AttackAction
{
public:
    StratDefendMemberAction(PlayerbotAI* botAI) : AttackAction(botAI, "strat defend on attacked group member") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
