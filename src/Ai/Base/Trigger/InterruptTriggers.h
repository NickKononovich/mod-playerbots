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

#endif
