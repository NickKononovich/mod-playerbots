/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SHTRIGGERCONTEXT_H
#define PLAYERBOTS_SHTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "SHTriggers.h"

class TbcDungeonShatteredHallsTriggerContext : public NamedObjectContext<Trigger>
{
public:
    TbcDungeonShatteredHallsTriggerContext()
    {
        creators["kargath blade dance"] = &TbcDungeonShatteredHallsTriggerContext::kargath_blade_dance;
        creators["kargath ranged ring"] = &TbcDungeonShatteredHallsTriggerContext::kargath_ranged_ring;
    }

private:
    static Trigger* kargath_blade_dance(PlayerbotAI* botAI) { return new KargathBladeDanceTrigger(botAI); }
    static Trigger* kargath_ranged_ring(PlayerbotAI* botAI) { return new KargathRangedRingTrigger(botAI); }
};

#endif
