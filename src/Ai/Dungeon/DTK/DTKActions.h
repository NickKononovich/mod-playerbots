/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DTKACTIONS_H
#define PLAYERBOTS_DTKACTIONS_H

#include "Action.h"
#include "AttackAction.h"
#include "DTKTriggers.h"
#include "GenericSpellActions.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

const Position NOVOS_PARTY_POSITION = Position(-378.852f, -760.349f, 28.587f);

class CorpseExplodeSpreadAction : public MovementAction
{
public:
    CorpseExplodeSpreadAction(PlayerbotAI* botAI) : MovementAction(botAI, "corpse explode spread") {}
    bool Execute(Event event) override;
};

class TrollgoreHoldRoomAction : public MovementAction
{
public:
    TrollgoreHoldRoomAction(PlayerbotAI* botAI) : MovementAction(botAI, "trollgore hold room") {}
    bool Execute(Event event) override;
};

class AvoidArcaneFieldAction : public MovementAction
{
public:
    AvoidArcaneFieldAction(PlayerbotAI* botAI) : MovementAction(botAI, "avoid arcane field") {}
    bool Execute(Event event) override;
};

class NovosDefaultPositionAction : public MovementAction
{
public:
    NovosDefaultPositionAction(PlayerbotAI* botAI) : MovementAction(botAI, "novos default position") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class NovosTargetPriorityAction : public AttackAction
{
public:
    NovosTargetPriorityAction(PlayerbotAI* botAI) : AttackAction(botAI, "novos target priority") {}
    bool Execute(Event event) override;
    // bool isUseful() override;
};

class NovosPullAction : public AttackAction
{
public:
    NovosPullAction(PlayerbotAI* botAI) : AttackAction(botAI, "novos pull") {}
    bool Execute(Event event) override;
};

class NovosDropSummonTargetAction : public Action
{
public:
    NovosDropSummonTargetAction(PlayerbotAI* botAI) : Action(botAI, "novos drop summon target") {}
    bool Execute(Event event) override;
};

class KingDredFearGuardAction : public Action
{
public:
    KingDredFearGuardAction(PlayerbotAI* botAI) : Action(botAI, "king dred fear guard") {}
    bool Execute(Event event) override;
};

// The efficient direct heal of the class on the bitten member: Grievous Bite ticks until the target is at full.
class KingDredGrievousBiteHealAction : public Action
{
public:
    KingDredGrievousBiteHealAction(PlayerbotAI* botAI) : Action(botAI, "king dred grievous bite heal") {}
    bool Execute(Event event) override;
};

class CastSlayingStrikeAction : public CastMeleeSpellAction
{
public:
    CastSlayingStrikeAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "slaying strike") {}
};

class CastTauntAction : public CastSpellAction
{
public:
    CastTauntAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "taunt") {}
};

class CastBoneArmorAction : public CastSpellAction
{
public:
    CastBoneArmorAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "bone armor") {}
};

class CastTouchOfLifeAction : public CastSpellAction
{
public:
    CastTouchOfLifeAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "touch of life") {}
};

#endif
