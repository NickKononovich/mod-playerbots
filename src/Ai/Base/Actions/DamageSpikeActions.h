/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DAMAGESPIKEACTIONS_H
#define PLAYERBOTS_DAMAGESPIKEACTIONS_H

#include "GenericSpellActions.h"
#include "PlayerbotAI.h"

// External cooldown cast on the tank picked by the "tank damage spike" trigger. Class contexts register one
// per spell, e.g. new CastSpikeProtectSpellAction(botAI, "pain suppression").
class CastSpikeProtectSpellAction : public CastSpellAction
{
public:
    CastSpikeProtectSpellAction(PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell) {}

    std::string const GetTargetName() override { return "tank damage spike target"; }
    ActionThreatType getThreatType() override { return ActionThreatType::None; }

    bool isUseful() override
    {
        Unit* target = GetTarget();
        return target && !botAI->HasAura(spell, target);
    }
};

#endif
