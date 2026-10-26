/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MGTACTIONS_H
#define PLAYERBOTS_MGTACTIONS_H

#include "AttackAction.h"
#include "MgTShared.h"
#include "MovementActions.h"
#include "TbcHazard.h"

class MgTEscapeAction : public MovementAction
{
public:
    MgTEscapeAction(PlayerbotAI* botAI, std::string const name, std::string const value)
        : MovementAction(botAI, name), _value(value)
    {
    }

    bool Execute(Event event) override;

private:
    std::string const _value;
};

class MgTLeaveDampeningFieldAction : public MgTEscapeAction
{
public:
    MgTLeaveDampeningFieldAction(PlayerbotAI* botAI)
        : MgTEscapeAction(botAI, "mgt leave dampening field", "mgt dampening escape")
    {
    }
};

class MgTClearArcaneNovaAction : public MgTEscapeAction
{
public:
    MgTClearArcaneNovaAction(PlayerbotAI* botAI)
        : MgTEscapeAction(botAI, "mgt clear arcane nova", "mgt nova escape")
    {
    }
};

class MgTLeaveFlameStrikeAction : public MgTEscapeAction
{
public:
    MgTLeaveFlameStrikeAction(PlayerbotAI* botAI)
        : MgTEscapeAction(botAI, "mgt leave flame strike", "mgt flame strike escape")
    {
    }
};

class MgTLeavePhoenixBurnAction : public MgTEscapeAction
{
public:
    MgTLeavePhoenixBurnAction(PlayerbotAI* botAI)
        : MgTEscapeAction(botAI, "mgt leave phoenix burn", "mgt phoenix escape")
    {
    }
};

class MgTFocusAction : public AttackAction
{
public:
    MgTFocusAction(PlayerbotAI* botAI, std::string const name, std::string const value)
        : AttackAction(botAI, name), _value(value)
    {
    }

    bool Execute(Event event) override;

private:
    std::string const _value;
};

class MgTFocusTargetAction : public MgTFocusAction
{
public:
    MgTFocusTargetAction(PlayerbotAI* botAI) : MgTFocusAction(botAI, "mgt focus target", "mgt focus target") {}
};

class MgTDelrissaFocusTargetAction : public MgTFocusAction
{
public:
    MgTDelrissaFocusTargetAction(PlayerbotAI* botAI)
        : MgTFocusAction(botAI, "mgt delrissa focus target", "mgt delrissa focus target")
    {
    }
};

class MgTKaelFocusTargetAction : public MgTFocusAction
{
public:
    MgTKaelFocusTargetAction(PlayerbotAI* botAI)
        : MgTFocusAction(botAI, "mgt kael focus target", "mgt kael focus target")
    {
    }
};

class MgTKillCrystalAction : public AttackAction
{
public:
    MgTKillCrystalAction(PlayerbotAI* botAI) : AttackAction(botAI, "mgt kill crystal") {}
    bool Execute(Event event) override;
};

class MgTReturnToRoomAction : public MovementAction
{
public:
    MgTReturnToRoomAction(PlayerbotAI* botAI) : MovementAction(botAI, "mgt return to room") {}
    // [selin_final_entry] the group's walk in for the final pull goes into the centre pair's aggro on purpose
    bool SkipsIdleGuard() override;
    bool Execute(Event event) override;

private:
    uint32 _failLogMs = 0;  // [b31_dg] the last "[mgt return to room] ... cannot" line
};

class MgTCloseOnMageGuardAction : public MovementAction
{
public:
    MgTCloseOnMageGuardAction(PlayerbotAI* botAI) : MovementAction(botAI, "mgt close on mage guard") {}
    bool Execute(Event event) override;
};

class MgTPriorityInterruptAction : public Action
{
public:
    MgTPriorityInterruptAction(PlayerbotAI* botAI) : Action(botAI, "mgt priority interrupt") {}
    bool Execute(Event event) override;
};

class MgTTauntEnragedWretchedAction : public Action
{
public:
    MgTTauntEnragedWretchedAction(PlayerbotAI* botAI) : Action(botAI, "mgt taunt enraged wretched") {}
    bool Execute(Event event) override;
};

class MgTDelrissaInterruptAction : public Action
{
public:
    MgTDelrissaInterruptAction(PlayerbotAI* botAI) : Action(botAI, "mgt delrissa interrupt") {}
    bool Execute(Event event) override;
};

// [delrissa_retinue_left] the tank walks to the lackey that outlived Delrissa and pulls it (MgTShared.h)
class MgTDelrissaRetinueLeftAction : public MovementAction
{
public:
    MgTDelrissaRetinueLeftAction(PlayerbotAI* botAI) : MovementAction(botAI, "mgt delrissa retinue left") {}
    bool Execute(Event event) override;

private:
    uint32 _logMs = 0;
};

// [b36_mgt] a MovementAction: it also casts its own Hex / Polymorph on the mark, walking into range and sight first
class MgTDelrissaCcMarkAction : public MovementAction
{
public:
    MgTDelrissaCcMarkAction(PlayerbotAI* botAI) : MovementAction(botAI, "mgt delrissa cc mark") {}
    bool Execute(Event event) override;

private:
    bool CastOnMark(Unit* target, std::string const& spell);
    uint32 _castLogMs = 0;
};

class MgTDelrissaTremorTotemAction : public Action
{
public:
    MgTDelrissaTremorTotemAction(PlayerbotAI* botAI) : Action(botAI, "mgt delrissa tremor totem") {}
    bool Execute(Event event) override;
};

class MgTDelrissaHealerSpotAction : public MovementAction
{
public:
    MgTDelrissaHealerSpotAction(PlayerbotAI* botAI) : MovementAction(botAI, "mgt delrissa healer spot") {}
    bool Execute(Event event) override;
};

class MgTKaelInterruptAction : public Action
{
public:
    MgTKaelInterruptAction(PlayerbotAI* botAI) : Action(botAI, "mgt kael interrupt") {}
    bool Execute(Event event) override;
};

// Selin's room (MagistersTerrace::SelinRoom, MgTShared.h): once the group is gathered and topped up at the rally,
// the tank walks to the planned cast spot and pulls the pack with its ranged pull back to the rally.
class MgTSelinRoomPullAction : public MovementAction
{
public:
    MgTSelinRoomPullAction(PlayerbotAI* botAI) : MovementAction(botAI, "mgt selin room pull") {}
    bool Execute(Event event) override;

private:
    // Every other member alive, gathered at the rally (the stage spot for the final pull) and topped up (relaxed
    // after a while; `finalPull`: the centre pair that brings Selin - full health and mana, never relaxed).
    bool IsGroupReady(bool finalPull);
    // A side pack of AutoCcMarkMinPackSize or more: the group's pre-pull crowd control plan on it (registered once).
    bool HoldsPackInControl(MagistersTerrace::SelinRoom::PullPlan const& plan, Unit* target);

    uint32 _waitSinceMs = 0;
    uint32 _waitCheckMs = 0;
    uint32 _waitLogMs = 0;
    uint32 _readyMs = 0;  // the group was ready: the tank is on its way to the cast spot
    uint32 _lagLogMs = 0;  // [b31_dg] the last "[selin final entry] ... waits" line
};

// Fel Infusion (MgTShared.h): the tank's damage cooldown while a drinking Wretched is on it, the shaman's group haste
// once four or more Wretched fight.
class MgTWretchedInfusionAction : public Action
{
public:
    MgTWretchedInfusionAction(PlayerbotAI* botAI) : Action(botAI, "mgt wretched infusion") {}
    bool Execute(Event event) override;
};

// Back to the corridor rally (out of every idle pack's reach).
class MgTSelinRoomToRallyAction : public TbcHazardMoveAction
{
public:
    MgTSelinRoomToRallyAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "mgt selin room to rally") {}
    bool Execute(Event event) override;
};

// [selin pet] The bot's pet out of Selin's room, or off her, back to its owner (MgTShared.h, SelinRoom::KeepsPetsOut).
class MgTSelinPetOutAction : public Action
{
public:
    MgTSelinPetOutAction(PlayerbotAI* botAI) : Action(botAI, "mgt selin pet out") {}
    bool Execute(Event event) override;

private:
    uint32 _logMs = 0;
};

// Selin's room entry order (MgTShared.h): back behind the leash edge (to the rally) while a Wretched of hers lives
// and her fight is not committed.
class MgTSelinRoomLeashAction : public TbcHazardMoveAction
{
public:
    MgTSelinRoomLeashAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "mgt selin room leash") {}
    bool Execute(Event event) override;

private:
    uint32 _logMs = 0;
};

class MgTTakeLapseSpotAction : public MovementAction
{
public:
    MgTTakeLapseSpotAction(PlayerbotAI* botAI) : MovementAction(botAI, "mgt take lapse spot") {}
    bool Execute(Event event) override;

private:
    MagistersTerrace::KiteState _kite;
};

#endif
