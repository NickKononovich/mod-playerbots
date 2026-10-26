/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CLASSICMULTIPLIERS_H
#define PLAYERBOTS_CLASSICMULTIPLIERS_H

#include "Multiplier.h"

// Arcanist Doan: during Detonation only the escape moves a bot still in reach; otherwise ranged and healers do not
// walk back into Arcane Explosion.
class DoanMultiplier : public Multiplier
{
public:
    DoanMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "arcanist doan") {}
    float GetValue(Action* action) override;
};

// Scarlet Monastery Cathedral: while an idle Commander Mograine's chapel stands, no reposition (a ranged spot, a
// reach, a follow) carries a bot into his reach or toward a target that stands in it (ClassicShared.h).
class MograineHoldMultiplier : public Multiplier
{
public:
    MograineHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "mograine hold") {}
    float GetValue(Action* action) override;
};

#endif
