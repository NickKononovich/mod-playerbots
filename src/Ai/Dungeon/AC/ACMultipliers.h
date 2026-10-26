/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACMULTIPLIERS_H
#define PLAYERBOTS_ACMULTIPLIERS_H

#include "Multiplier.h"

class ShirrakFleeFocusFireMultiplier : public Multiplier
{
public:
    ShirrakFleeFocusFireMultiplier(PlayerbotAI* botAI)
        : Multiplier(botAI, "shirrak flee focus fire") {}
    float GetValue(Action* action) override;
};

// [b35_lock] A long-range damage dealer at its 1-stack band is in reach of Shirrak: "reach spell" (SpellDistance)
// would walk it back into the second stack.
class ShirrakHoldLongRangeMultiplier : public Multiplier
{
public:
    ShirrakHoldLongRangeMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "shirrak hold long range") {}
    float GetValue(Action* action) override;
};

// [b35_lock] With 2+ Inhibit Magic stacks every cast takes 1.5-2.6x as long: harmful instants go first.
class ShirrakInhibitMagicInstantsMultiplier : public Multiplier
{
public:
    ShirrakInhibitMagicInstantsMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "shirrak inhibit magic instants") {}
    float GetValue(Action* action) override;
};

#endif
