/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ANMULTIPLIERS_H
#define PLAYERBOTS_ANMULTIPLIERS_H

#include "Multiplier.h"

class KrikthirMultiplier : public Multiplier
{
    public:
        KrikthirMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "krik'thir the gatewatcher") {}

    public:
        float GetValue(Action* action) override;
};

// Krik'thir's room (ANWatcherRoom in ANTriggers.h): the room tactic owns the pulls (the careful planner and its
// lone-mob minimum would split the timed event into slow single pulls), and nobody walks into, or casts an area
// spell next to, a pack that is not in the fight yet.
class KrikthirWatcherRoomMultiplier : public Multiplier
{
public:
    KrikthirWatcherRoomMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "krik'thir watcher room") {}

    float GetValue(Action* action) override;
};

#endif
