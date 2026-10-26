/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MTActions.h"
#include "MTShared.h"
#include "Playerbots.h"
#include "TbcHazard.h"

using namespace ManaTombs;

Unit* ShaffarKillBeaconAction::PickAdd()
{
    // Beacons before apprentices: a beacon left alive becomes one more caster.
    for (uint32 entry : { NPC_ETHEREAL_BEACON, NPC_ETHEREAL_APPRENTICE })
    {
        Unit* best = nullptr;
        float bestDist = BEACON_SEARCH_RADIUS;
        for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
        {
            if (guid.GetEntry() != entry)
                continue;

            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || !bot->IsWithinLOSInMap(unit))
                continue;

            float const dist = bot->GetExactDist2d(unit);
            if (dist < bestDist)
            {
                best = unit;
                bestDist = dist;
            }
        }

        if (best)
            return best;
    }

    return nullptr;
}

bool ShaffarKillBeaconAction::isUseful()
{
    Unit* current = AI_VALUE(Unit*, "current target");
    // Already on a beacon: finish it rather than hopping to a nearer one.
    if (current && current->IsAlive() && current->GetEntry() == NPC_ETHEREAL_BEACON)
        return false;

    Unit* add = PickAdd();
    return add && add != current;
}

bool ShaffarKillBeaconAction::Execute(Event /*event*/)
{
    Unit* add = PickAdd();
    if (!add || AI_VALUE(Unit*, "current target") == add)
        return false;

    return Attack(add);
}

bool PandemoniusStopAttackAction::Execute(Event /*event*/)
{
    bot->AttackStop();
    bot->InterruptNonMeleeSpells(false);
    return true;
}
