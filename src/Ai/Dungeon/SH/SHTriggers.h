/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SHTRIGGERS_H
#define PLAYERBOTS_SHTRIGGERS_H

#include "Trigger.h"

// Warchief Kargath Bladefist dances (Blade Dance) and the bot stands where a hit from where he is now reaches it.
// Every role, the tank too: he is passive while he dances, nobody holds him.
class KargathBladeDanceTrigger : public Trigger
{
public:
    KargathBladeDanceTrigger(PlayerbotAI* botAI) : Trigger(botAI, "kargath blade dance") {}
    bool IsActive() override;
};

// Kargath fights (not dancing) and a healer or a ranged damage dealer stands inside its ring round him (the healer
// out of every charge's reach, the ranged at the edge of their spells).
class KargathRangedRingTrigger : public Trigger
{
public:
    KargathRangedRingTrigger(PlayerbotAI* botAI) : Trigger(botAI, "kargath ranged ring") {}
    bool IsActive() override;
};

#endif
