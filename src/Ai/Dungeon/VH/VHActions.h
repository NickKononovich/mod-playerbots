/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_VHACTIONS_H
#define PLAYERBOTS_VHACTIONS_H

#include "Action.h"
#include "AttackAction.h"

class AttackErekemAction : public AttackAction
{
public:
    AttackErekemAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack erekem") {}
    bool Execute(Event event) override;
};

class AttackIchorGlobuleAction : public AttackAction
{
public:
    AttackIchorGlobuleAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack ichor globule") {}
    bool Execute(Event event) override;
};

class AttackVoidSentryAction : public AttackAction
{
public:
    AttackVoidSentryAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack void sentry") {}
    bool Execute(Event event) override;
};

class VHStartEventAction : public MovementAction
{
public:
    VHStartEventAction(PlayerbotAI* botAI) : MovementAction(botAI, "vh start event") {}
    bool Execute(Event event) override;
};

class VHAttackSealAttackerAction : public AttackAction
{
public:
    VHAttackSealAttackerAction(PlayerbotAI* botAI) : AttackAction(botAI, "vh attack seal attacker") {}
    bool Execute(Event event) override;
};

class VHAttackPortalKeeperAction : public AttackAction
{
public:
    VHAttackPortalKeeperAction(PlayerbotAI* botAI) : AttackAction(botAI, "vh attack portal keeper") {}
    bool Execute(Event event) override;
};

class VHEngageReleasedBossAction : public AttackAction
{
public:
    VHEngageReleasedBossAction(PlayerbotAI* botAI) : AttackAction(botAI, "vh engage released boss") {}
    bool Execute(Event event) override;
};

class VHRallyAction : public MovementAction
{
public:
    VHRallyAction(PlayerbotAI* botAI) : MovementAction(botAI, "vh rally") {}
    bool Execute(Event event) override;
};

// To Sinclari outside the seal and her "I'm not fighting, so send me in now!" (the server's SAI teleports inside).
class VHLateJoinAction : public MovementAction
{
public:
    VHLateJoinAction(PlayerbotAI* botAI) : MovementAction(botAI, "vh late join") {}
    bool Execute(Event event) override;
};

class XevozzKiteAction : public MovementAction
{
public:
    XevozzKiteAction(PlayerbotAI* botAI) : MovementAction(botAI, "xevozz kite") {}
    bool Execute(Event event) override;
};

#endif
