/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SVMULTIPLIERS_H
#define PLAYERBOTS_SVMULTIPLIERS_H

#include "Multiplier.h"

// Warlord Kalithresh: no hostile cast aimed at him while Spell Reflection is up (melee keeps swinging).
class KalithreshSpellReflectionMultiplier : public Multiplier
{
public:
    KalithreshSpellReflectionMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "kalithresh spell reflection") {}
    float GetValue(Action* action) override;
};

#endif
