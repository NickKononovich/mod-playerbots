/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InterruptTriggers.h"
#include "InterruptValues.h"
#include "Playerbots.h"

bool AssignedInterruptTrigger::IsActive()
{
    return !AI_VALUE(InterruptAssignment, "interrupt assignment").caster.IsEmpty();
}
