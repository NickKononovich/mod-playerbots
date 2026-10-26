/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AKACTIONS_H
#define PLAYERBOTS_AKACTIONS_H

#include "AKTriggers.h"
#include "Action.h"
#include "AttackAction.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

class AttackNadoxGuardianAction : public AttackAction
{
public:
    AttackNadoxGuardianAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack nadox guardian") {}
    bool Execute(Event event) override;
};

class AttackJedogaVolunteerAction : public AttackAction
{
public:
    AttackJedogaVolunteerAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack jedoga volunteer") {}
    bool Execute(Event event) override;
};

class UseNerubianDeviceAction : public MovementAction
{
public:
    UseNerubianDeviceAction(PlayerbotAI* botAI) : MovementAction(botAI, "use nerubian device") {}
    bool Execute(Event event) override;

private:
    // [b35_ok] The walk to a device leads the group and leaves the device's own pack to the pull planner.
    bool HoldWalk(AKIds::NerubianDevice const* device);
    void StopOwnWalk();

    uint32 _laggerWaitMs = 0;  // since when the walk waits for laggers (DungeonInteract::GroupLaggers)
    uint32 _packWaitMs = 0;    // since when an idle pack at the device holds the walk
    uint32 _holdLogMs = 0;
    bool _walking = false;     // the bot's current move is this action's walk to the device
};

class AttackJedogaInitiateAction : public AttackAction
{
public:
    AttackJedogaInitiateAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack jedoga initiate") {}
    bool Execute(Event event) override;

private:
    // The group stands at the altar, or the tank waited GATHER_WAIT_MS and the healer is at its side.
    bool GroupReady();

    uint32 _gatherWaitMs = 0;
};

class AvoidShadowCrashAction : public MovementAction
{
public:
    AvoidShadowCrashAction(PlayerbotAI* botAI) : MovementAction(botAI, "avoid shadow crash") {}
    bool Execute(Event event) override;
};

// Volazj Insanity: fight the visages of the bot's own phase (the stunned boss and other phases are out of reach).
class VolazjAttackVisageAction : public AttackAction
{
public:
    VolazjAttackVisageAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack twisted visage") {}
    bool Execute(Event event) override;
};

// [b33_ok] Amanitar Mini: walk next to a Healthy Mushroom and kill it there, so its Potent Fungus (4 yd) lands on the
// bot and removes Mini.
class AmanitarPotentFungusAction : public AttackAction
{
public:
    AmanitarPotentFungusAction(PlayerbotAI* botAI) : AttackAction(botAI, "amanitar potent fungus") {}
    bool Execute(Event event) override;
};

#endif
