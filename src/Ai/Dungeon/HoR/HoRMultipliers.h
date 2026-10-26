/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HORMULTIPLIERS_H
#define PLAYERBOTS_HORMULTIPLIERS_H

#include "Multiplier.h"

// Marwyn: Shared Suffering (72368, magic) splits its remaining damage over the group when dispelled.
class HoRSharedSufferingMultiplier : public Multiplier
{
public:
    HoRSharedSufferingMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hor shared suffering") {}
    float GetValue(Action* action) override;
};

// Spirit waves, corner tactic: no reach-target movement towards a unit that has not come round the corner yet
// (ranged keep waiting out of line of sight, the tank picks the spirits up at the corner edge).
class HoRWaveHoldMultiplier : public Multiplier
{
public:
    HoRWaveHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hor wave hold") {}
    float GetValue(Action* action) override;
};

// Falric and Marwyn: the shadow paladin's other auras (Devotion from the buff strategy) stay off, or they swap
// Shadow Resistance Aura back on the next tick.
class HoRShadowAuraMultiplier : public Multiplier
{
public:
    HoRShadowAuraMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hor shadow aura") {}
    float GetValue(Action* action) override;
};

// Falric: Heroism / Bloodlust waits for his second Hopelessness (34 %): below it the group deals and heals -40/-50 %,
// below 11 % -60/-75 %, and every Defiling Horror there (4 x 4000 shadow on everybody, heroic) is a death check.
// The haste shortens the deadly part instead of the safe one (hor-h 20261025-153805: Falric died at 112 s with four
// dead; the hunter died with him at 1.9 %, the mage at 0.5 %).
class HoRFalricHeroismMultiplier : public Multiplier
{
public:
    HoRFalricHeroismMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hor falric heroism") {}
    float GetValue(Action* action) override;
};

// Frostsworn General: he evades past 30 yd from his home (boss_frostsworn_general::UpdateAI). No "move fight spot
// back" while he fights, and the tank does not open on him before the group is together (hor-h 20261025-163739
// pull 11: the tank's "reach melee" walked in with the mage 31-39 yd behind, then walked him back and he evaded).
class HoRFrostswornGeneralMultiplier : public Multiplier
{
public:
    HoRFrostswornGeneralMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hor frostsworn general") {}
    float GetValue(Action* action) override;
};

// Escape: the group meets every wall wave at the leader (the summons run to the players). A damage dealer or the
// healer does not walk or reach out to a summon farther than ESCAPE_MEET_RANGE from her: out there the Lich King
// comes up behind, and the next melee step back is towards him (user, hor-h 20261025-163739 16:55-16:57: the Enh
// shaman swung 2-15 yd off the tank, "move_conflict hor escape move to leader / reach melee" 3 flips in 20 s).
class HoREscapeMeetAtLeaderMultiplier : public Multiplier
{
public:
    HoREscapeMeetAtLeaderMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hor escape meet at leader") {}
    float GetValue(Action* action) override;
};

#endif
