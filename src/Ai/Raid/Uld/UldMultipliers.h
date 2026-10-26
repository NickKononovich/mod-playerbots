/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ULDMULTIPLIERS_H
#define PLAYERBOTS_ULDMULTIPLIERS_H

#include "Action.h"
#include "Multiplier.h"
#include "PlayerbotAI.h"
#include "UldBossHelper.h"

// AC #27602: Laughing Skulls cast Lunatic Gaze through the illusion walls. They despawn with the last
// Influence Tentacle, so honest play ignores them: no attack or spell is aimed at a skull.
class YoggSaronLaughingSkullMultiplier : public Multiplier
{
public:
    YoggSaronLaughingSkullMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "yogg-saron laughing skull") {}

    float GetValue(Action* action) override
    {
        constexpr float BRAIN_LEVEL_MIN_Z = 230.0f;  // same band as YoggSaronTrigger::IsInBrainLevel()
        constexpr float BRAIN_LEVEL_MAX_Z = 250.0f;

        if (bot->GetMapId() != ULDUAR_MAP_ID || bot->GetPositionZ() < BRAIN_LEVEL_MIN_Z ||
            bot->GetPositionZ() > BRAIN_LEVEL_MAX_Z)
            return 1.0f;

        Unit* target = action->GetTarget();
        if (target && target->GetEntry() == NPC_LAUGHING_SKULL)
            return 0.0f;

        return 1.0f;
    }
};

// Tank swap (Kologarn Crunch Armor, Thorim Unbalancing Strike): the tank carrying the swap debuff does not taunt
// the boss back until it fades.
class UlduarTankSwapMultiplier : public Multiplier
{
public:
    UlduarTankSwapMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "ulduar tank swap") {}

    float GetValue(Action* action) override
    {
        if (bot->GetMapId() != ULDUAR_MAP_ID ||
            (!bot->HasAura(SPELL_CRUNCH_ARMOR) && !bot->HasAura(SPELL_UNBALANCING_STRIKE)))
            return 1.0f;

        std::string const& name = action->getName();
        if (name == "taunt" || name == "dark command" || name == "hand of reckoning" || name == "growl")
            return 0.0f;

        return 1.0f;
    }
};

#endif
