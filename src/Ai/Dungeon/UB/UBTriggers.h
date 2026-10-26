/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_UBTRIGGERS_H
#define PLAYERBOTS_UBTRIGGERS_H

#include "Trigger.h"

class UBFoulSporesTrigger : public Trigger
{
public:
    UBFoulSporesTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ub foul spores") {}
    bool IsActive() override;
};

class UBSporeCloudDangerTrigger : public Trigger
{
public:
    UBSporeCloudDangerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ub spore cloud danger") {}
    bool IsActive() override;
};

class UBUnderbatLashTrigger : public Trigger
{
public:
    UBUnderbatLashTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ub underbat lash") {}
    bool IsActive() override;
};

// Ghaz'an's lake: the bot swims while fighting in the basin (cached liquid status only).
class UBGhazanLakeTrigger : public Trigger
{
public:
    UBGhazanLakeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ub ghazan lake") {}
    bool IsActive() override;
};

#endif
