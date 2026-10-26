/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HORACTIONS_H
#define PLAYERBOTS_HORACTIONS_H

#include "Action.h"
#include "AttackAction.h"
#include "HoRTriggers.h"
#include "MovementActions.h"

#include <vector>

class HoRStartIntroAction : public MovementAction
{
public:
    HoRStartIntroAction(PlayerbotAI* botAI) : MovementAction(botAI, "hor start intro") {}
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    Player* GetQuestStarter(Creature* leader);
};

class HoRMoveToWavePositionAction : public MovementAction
{
public:
    HoRMoveToWavePositionAction(PlayerbotAI* botAI) : MovementAction(botAI, "hor move to wave position") {}
    bool Execute(Event event) override;
};

// The last living member walks out of the waves' range so they reset (HoRHelpers::IsWaveLost).
class HoRLeaveWaveCircleAction : public MovementAction
{
public:
    HoRLeaveWaveCircleAction(PlayerbotAI* botAI) : MovementAction(botAI, "hor leave wave circle") {}
    bool Execute(Event event) override;

private:
    static constexpr uint32 WAVE_LOST_LOG_MS = 30 * IN_MILLISECONDS;
    uint32 _loggedMs = 0;
};

class HoRAttackPriorityAction : public AttackAction
{
public:
    HoRAttackPriorityAction(PlayerbotAI* botAI, std::string const name) : AttackAction(botAI, name) {}

protected:
    // Attacks the closest attacker of the best rank of the kill order (HoRHelpers::GetPriorityRank) unless the bot
    // is on one of them already.
    bool AttackFirst(HoRHelpers::PriorityList list);
};

class HoRAttackWavePriorityAction : public HoRAttackPriorityAction
{
public:
    HoRAttackWavePriorityAction(PlayerbotAI* botAI) : HoRAttackPriorityAction(botAI, "hor attack wave priority") {}
    bool Execute(Event event) override;
};

class HoREnterThroneRoomAction : public MovementAction
{
public:
    HoREnterThroneRoomAction(PlayerbotAI* botAI) : MovementAction(botAI, "hor enter throne room") {}
    bool Execute(Event event) override;
};

class HoRMoveToEscapeLeaderAction : public MovementAction
{
public:
    HoRMoveToEscapeLeaderAction(PlayerbotAI* botAI) : MovementAction(botAI, "hor move to escape leader") {}
    bool Execute(Event event) override;
};

class HoRStartEscapeAction : public MovementAction
{
public:
    HoRStartEscapeAction(PlayerbotAI* botAI) : MovementAction(botAI, "hor start escape") {}
    bool Execute(Event event) override;
};

class HoREscapeMoveToLeaderAction : public MovementAction
{
public:
    HoREscapeMoveToLeaderAction(PlayerbotAI* botAI) : MovementAction(botAI, "hor escape move to leader") {}
    bool Execute(Event event) override;
};

class HoRAttackEscapePriorityAction : public HoRAttackPriorityAction
{
public:
    HoRAttackEscapePriorityAction(PlayerbotAI* botAI)
        : HoRAttackPriorityAction(botAI, "hor attack escape priority") {}
    bool Execute(Event event) override;
};

class HoRStopAttackingLichKingAction : public Action
{
public:
    HoRStopAttackingLichKingAction(PlayerbotAI* botAI) : Action(botAI, "hor stop attacking lich king") {}
    bool Execute(Event event) override;
};

// Escape, tank: to the pick-up spot between the leader and the Lich King (HoRHelpers::GetEscapeTankSpot).
class HoREscapeTankPositionAction : public MovementAction
{
public:
    HoREscapeTankPositionAction(PlayerbotAI* botAI) : MovementAction(botAI, "hor escape tank position") {}
    bool Execute(Event event) override;
};

// Escape, tank: taunts the summon that is on a player who is not a tank and takes it as the target.
class HoREscapeTauntLooseMobAction : public AttackAction
{
public:
    HoREscapeTauntLooseMobAction(PlayerbotAI* botAI) : AttackAction(botAI, "hor escape taunt loose mob") {}
    bool Execute(Event event) override;
};

// Escape, healer: back to the leader, behind the tank, while summons are up.
class HoREscapeHealerHoldAction : public MovementAction
{
public:
    HoREscapeHealerHoldAction(PlayerbotAI* botAI) : MovementAction(botAI, "hor escape healer hold") {}
    bool Execute(Event event) override;
};

// Falric: Dispel Magic / Cleanse on Impending Despair, then on Quivering Strike (HoRHelpers::GetFalricDispelTarget).
class HoRShadowAuraAction : public Action
{
public:
    HoRShadowAuraAction(PlayerbotAI* botAI) : Action(botAI, "hor shadow aura") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

class HoRGeneralLeashAction : public MovementAction
{
public:
    HoRGeneralLeashAction(PlayerbotAI* botAI) : MovementAction(botAI, "hor general leash") {}
    bool Execute(Event event) override;
};

class HoRAttackReflectionAction : public AttackAction
{
public:
    HoRAttackReflectionAction(PlayerbotAI* botAI) : AttackAction(botAI, "hor attack reflection") {}
    bool Execute(Event event) override;
};

class HoRFalricDispelAction : public Action
{
public:
    HoRFalricDispelAction(PlayerbotAI* botAI) : Action(botAI, "hor falric dispel") {}
    bool Execute(Event event) override;
};

#endif
