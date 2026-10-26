/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROGUEFINISHINGACTIONS_H
#define PLAYERBOTS_ROGUEFINISHINGACTIONS_H

#include "GenericSpellActions.h"

class PlayerbotAI;

// Finishers are refreshed inside this window rather than after they drop
constexpr uint32 SLICE_AND_DICE_REFRESH_MS = 1000;
constexpr uint32 RUPTURE_REFRESH_MS = 1000;
// [b31_wr] Hunger for Blood's self buff (60 s) is refreshed below this
constexpr uint32 HUNGER_FOR_BLOOD_REFRESH_MS = 5000;
// Rupture (14-16 s at 4-5 points, cannot crit) only beats Eviscerate/Envenom when most of its ticks land; a
// shorter-lived target gets the direct finisher instead (the strategies fall through to it)
constexpr float RUPTURE_MIN_TARGET_LIFETIME = 12.0f;

class CastEviscerateAction : public CastMeleeSpellAction
{
public:
    CastEviscerateAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "eviscerate") {}
};

class CastSliceAndDiceAction : public CastMeleeSpellAction
{
public:
    CastSliceAndDiceAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "slice and dice") {}
};

class CastExposeArmorAction : public CastDebuffSpellAction
{
public:
    CastExposeArmorAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "expose armor", false, 25.0f) {}
};

class CastRuptureAction : public CastDebuffSpellAction
{
public:
    CastRuptureAction(PlayerbotAI* botAI)
        : CastDebuffSpellAction(botAI, "rupture", true, RUPTURE_MIN_TARGET_LIFETIME)
    {
        beforeDuration = RUPTURE_REFRESH_MS;
    }
};

class CastKidneyShotAction : public CastMeleeSpellAction
{
public:
    CastKidneyShotAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "kidney shot") {}
};

#endif
