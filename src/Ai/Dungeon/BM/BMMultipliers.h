/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BMMULTIPLIERS_H
#define PLAYERBOTS_BMMULTIPLIERS_H

#include "Multiplier.h"

// On an add at Medivh no generic target chooser (dps assist, rti, aoe, tank assist) takes the bot off it. A damage
// dealer keeps any add of "bm medivh threats", the tank one still channeling Corrupt Medivh (bm-n 2026-10-25: "bm
// defend medivh" and "dps assist" swapped the target every tick for 100 s, no spell landed, the shield fell at rift 4).
class BlackMorassDefendFocusMultiplier : public Multiplier
{
public:
    BlackMorassDefendFocusMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "bm defend focus") {}
    float GetValue(Action* action) override;
};

// Non-tanks during the portals: no walk after a keeper that stands far from Medivh (the tank brings it).
class BlackMorassStayAtMedivhMultiplier : public Multiplier
{
public:
    BlackMorassStayAtMedivhMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "bm stay at medivh") {}
    float GetValue(Action* action) override;
};

// The tank dragging a keeper to Medivh: no other walk or target switch until it is there.
class BlackMorassDragRiftGuardianMultiplier : public Multiplier
{
public:
    BlackMorassDragRiftGuardianMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "bm drag rift guardian") {}
    float GetValue(Action* action) override;
};

// [b32_bm] No eating / drinking while a rift boss (Deja, Temporus, Aeonus) lives during the portals: he is the next
// fight and Aeonus walks to Medivh by himself (REACT_DEFENSIVE, drains 2 shield points a tick from his home spot).
// bm-h 20261025-223301-bm-h-5H-72: the tank drank at 81 % mana while Aeonus walked past it to Medivh, the shield fell.
class BlackMorassNoRestForRiftBossMultiplier : public Multiplier
{
public:
    BlackMorassNoRestForRiftBossMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "bm no rest for rift boss") {}
    float GetValue(Action* action) override;
};

// Heroic Temporus' Spell Reflection: casters hold their spells on him while it is up.
class BlackMorassTemporusReflectMultiplier : public Multiplier
{
public:
    BlackMorassTemporusReflectMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "bm temporus reflect") {}
    float GetValue(Action* action) override;
};

#endif
