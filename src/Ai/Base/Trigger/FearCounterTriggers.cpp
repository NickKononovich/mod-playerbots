/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FearCounterTriggers.h"

#include "FearCounterValues.h"
#include "Playerbots.h"

// [fear_counter]
bool PartyMemberFearedTrigger::IsActive()
{
    if (!bot->IsInCombat() || !bot->HasSpell(FearCounter::SPELL_TREMOR_TOTEM))
        return false;

    FearCounterState const state = AI_VALUE(FearCounterState, "fear counter state");
    Unit* victim = FearCounter::TremorAnchor(botAI, state, true);
    return victim && !FearCounter::TremorCovers(bot, victim);
}

bool FearThreatTrigger::IsActive()
{
    if (!bot->IsInCombat() || !bot->HasSpell(FearCounter::SPELL_TREMOR_TOTEM))
        return false;

    FearCounterState const state = AI_VALUE(FearCounterState, "fear counter state");
    if (!(state.threat & FearCounter::THREAT_TREMOR))
        return false;

    return !FearCounter::TremorCovers(bot, FearCounter::TremorAnchor(botAI, state, false));
}

bool FearWardNeededTrigger::IsActive()
{
    if (!bot->HasSpell(FearCounter::SPELL_FEAR_WARD) || bot->HasSpellCooldown(FearCounter::SPELL_FEAR_WARD))
        return false;

    FearCounterState const state = AI_VALUE(FearCounterState, "fear counter state");
    if (!(state.threat & FearCounter::THREAT_FEAR) || state.tank.IsEmpty())
        return false;

    Unit* tank = botAI->GetUnit(state.tank);
    return tank && tank->IsAlive() && !tank->HasAura(FearCounter::SPELL_FEAR_WARD);
}

bool FearIncomingTrigger::IsActive()
{
    if (!bot->IsInCombat() || !bot->HasSpell(FearCounter::SPELL_BERSERKER_RAGE) ||
        bot->HasAura(FearCounter::SPELL_BERSERKER_RAGE) || bot->HasSpellCooldown(FearCounter::SPELL_BERSERKER_RAGE))
        return false;

    FearCounterState const state = AI_VALUE(FearCounterState, "fear counter state");
    return (state.threat & FearCounter::THREAT_INCOMING) && (state.threat & FearCounter::THREAT_FEAR);
}
