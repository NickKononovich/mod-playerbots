/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STRATTRIGGERS_H
#define PLAYERBOTS_STRATTRIGGERS_H

#include "Trigger.h"

// Out of combat, a member (not the tank) stands where an idle street mob would aggro on it before the tank: inside
// its reach and not behind the tank (StratShared.h). Only while the tank stands still, the waiting at a pack's edge.
class StratAggroEdgeTrigger : public Trigger
{
public:
    StratAggroEdgeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "strat aggro edge") {}
    bool IsActive() override;
};

// The tank is out of combat while a mob hits a member of its group: a wanderer or a patrol found the member at the
// pack's edge (waves 16-34: 4-7 s with nobody on the mob, until the member died).
class StratMemberSwarmedTrigger : public Trigger
{
public:
    StratMemberSwarmedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "strat member swarmed") {}
    bool IsActive() override;
};

#endif
