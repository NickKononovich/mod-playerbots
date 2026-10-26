/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_UBACTIONS_H
#define PLAYERBOTS_UBACTIONS_H

#include "MovementActions.h"
#include "TbcHazard.h"

class UBRetreatFromFoulSporesAction : public MovementAction
{
public:
    UBRetreatFromFoulSporesAction(PlayerbotAI* botAI) : MovementAction(botAI, "ub retreat from foul spores") {}
    bool Execute(Event event) override;
};

class UBVacateSporeCloudAction : public MovementAction
{
public:
    UBVacateSporeCloudAction(PlayerbotAI* botAI) : MovementAction(botAI, "ub vacate spore cloud") {}
    bool Execute(Event event) override;
};

class UBClearUnderbatBackAction : public MovementAction
{
public:
    UBClearUnderbatBackAction(PlayerbotAI* botAI) : MovementAction(botAI, "ub clear underbat back") {}
    bool Execute(Event event) override;

private:
    uint32 _lastReposition = 0;
};

// Ghaz'an's lake: swim to the nearest dry, reachable spot (a shore spot nearby, a dry group member, the walkway or
// Ghaz'an's platform). The swim-only Frenzies cannot follow; Ghaz'an walks out to the tank.
class UBLeaveLakeAction : public TbcHazardMoveAction
{
public:
    UBLeaveLakeAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "ub leave lake") {}
    bool Execute(Event event) override;

private:
    bool PathOk(float x, float y, float z, float maxLength);
};

#endif
