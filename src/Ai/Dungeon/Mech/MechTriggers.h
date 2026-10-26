/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MECHTRIGGERS_H
#define PLAYERBOTS_MECHTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "Trigger.h"

enum class MechanarIDs : uint32
{
    NPC_NETHERMANCER_SEPETHREA = 19221,
    NPC_RAGING_FLAMES          = 20481,
    NPC_RAGING_FLAMES_HEROIC   = 21538,
};

class SepethreaKiteFlameTrigger : public Trigger
{
public:
    SepethreaKiteFlameTrigger(PlayerbotAI* botAI) : Trigger(botAI, "sepethrea kite flame") {}
    bool IsActive() override;
};

class SepethreaAvoidFlameTrigger : public Trigger
{
public:
    SepethreaAvoidFlameTrigger(PlayerbotAI* botAI) : Trigger(botAI, "sepethrea avoid flame") {}
    bool IsActive() override;
};

class SepethreaTrailTrigger : public Trigger
{
public:
    SepethreaTrailTrigger(PlayerbotAI* botAI) : Trigger(botAI, "sepethrea trail") {}
    bool IsActive() override;
};

class SepethreaFocusBossTrigger : public Trigger
{
public:
    SepethreaFocusBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "sepethrea focus boss") {}
    bool IsActive() override;
};

// The bot's way leads over the lift between the two levels, or it stands on the lift (MechanarLift::ActiveLeg).
class MechanarLiftTrigger : public Trigger
{
public:
    MechanarLiftTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mechanar lift") {}
    bool IsActive() override;
};

// Capacitus heroic: the bot's charge and a member of the other charge within the pulse radius (MechanarCapacitus).
class CapacitusPolarityTrigger : public Trigger
{
public:
    CapacitusPolarityTrigger(PlayerbotAI* botAI) : Trigger(botAI, "capacitus polarity") {}
    bool IsActive() override;
};

// Capacitus normal: a caster casts a magic spell into Reflective Magic Shield, or a melee damage dealer hits him
// through Reflective Damage Shield.
class CapacitusReflectiveShieldTrigger : public Trigger
{
public:
    CapacitusReflectiveShieldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "capacitus reflective shield") {}
    bool IsActive() override;
};

// The pulling tank by the idle bridge pack before Pathaleon, the group gathered and ready (MechanarBridge).
class MechanarBridgePullTrigger : public Trigger
{
public:
    MechanarBridgePullTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mechanar bridge pull") {}
    bool IsActive() override;

private:
    uint32 _checkMs = 0;
    bool _due = false;
};

#endif
