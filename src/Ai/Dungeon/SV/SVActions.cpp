/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SVActions.h"
#include "Playerbots.h"
#include "SVShared.h"
#include "SVTriggers.h"
#include <array>
#include <list>

using namespace Steamvault;

// Idle distillers are unselectable; the script clears that flag on the one it starts channelling.
Unit* KalithreshKillDistillerAction::FindActiveDistiller()
{
    std::list<Creature*> distillers;
    bot->GetCreatureListWithEntryInGrid(distillers, NPC_NAGA_DISTILLER, DISTILLER_SEARCH_RADIUS);
    for (Creature* distiller : distillers)
    {
        if (!distiller->IsAlive() || distiller->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) ||
            !bot->IsValidAttackTarget(distiller) || !bot->IsWithinLOSInMap(distiller))
            continue;

        return distiller;
    }

    return nullptr;
}

bool KalithreshKillDistillerAction::isUseful()
{
    Unit* current = AI_VALUE(Unit*, "current target");
    if (current && current->IsAlive() && current->GetEntry() == NPC_NAGA_DISTILLER)
        return false;

    return FindActiveDistiller() != nullptr;
}

bool KalithreshKillDistillerAction::Execute(Event /*event*/)
{
    Unit* distiller = FindActiveDistiller();
    if (!distiller || AI_VALUE(Unit*, "current target") == distiller)
        return false;

    return Attack(distiller);
}

bool KalithreshStopCastingAction::Execute(Event /*event*/)
{
    bot->InterruptNonMeleeSpells(false);
    return true;
}

bool SvTauntBogOverlordAction::Execute(Event /*event*/)
{
    static std::array<char const*, 4> const TAUNTS = {"hand of reckoning", "taunt", "growl", "dark command"};

    Unit* overlord = LooseBogOverlord(botAI);
    if (!overlord)
        return false;

    Unit* victim = overlord->GetVictim();
    char const* used = nullptr;
    if (victim && botAI->CanCastSpell("righteous defense", victim) && botAI->CastSpell("righteous defense", victim))
        used = "righteous defense";

    for (char const* taunt : TAUNTS)
    {
        if (used)
            break;
        if (botAI->CanCastSpell(taunt, overlord) && botAI->CastSpell(taunt, overlord))
            used = taunt;
    }

    LOG_INFO("playerbots", "[sv bog overlord] {} takes Bog Overlord ({:.0f} %) back from {}: {}", bot->GetName(),
             overlord->GetHealthPct(), victim ? victim->GetName() : std::string("?"), used ? used : "attack");

    return Attack(overlord) || used;
}
