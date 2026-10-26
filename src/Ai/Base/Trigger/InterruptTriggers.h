/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_INTERRUPTTRIGGERS_H
#define PLAYERBOTS_INTERRUPTTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

// The group interrupt rotation gave this bot an enemy cast to stop.
class AssignedInterruptTrigger : public Trigger
{
public:
    AssignedInterruptTrigger(PlayerbotAI* botAI) : Trigger(botAI, "assigned interrupt") {}

    bool IsActive() override;
};

// [rot2] The dungeon leader assigns this bot an external save / buff now (LeaderCommand::AssignedExternal), the spell
// known and off cooldown.
class LeaderExternalTrigger : public Trigger
{
public:
    LeaderExternalTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader external") {}

    bool IsActive() override;
};

// [rot2] The tank: the leader names a mob to take back (LeaderCommand::AssignedThreat).
class LeaderThreatTrigger : public Trigger
{
public:
    LeaderThreatTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader threat") {}

    bool IsActive() override;
};

// [rot2] The leader assigns this bot a mob to crowd-control (LeaderCommand::AssignedCc) that is not held yet.
class LeaderCcTrigger : public Trigger
{
public:
    LeaderCcTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader cc") {}

    bool IsActive() override;
};

#endif
