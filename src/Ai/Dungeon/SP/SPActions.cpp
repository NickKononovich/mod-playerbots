/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SPActions.h"
#include "Playerbots.h"
#include "SPShared.h"

using namespace SlavePens;

bool MennuLeaveNovaTotemAction::Execute(Event /*event*/)
{
    float const danger = NovaDangerRadius(bot);
    Creature* totem = bot->FindNearestCreature(NPC_CORRUPTED_NOVA_TOTEM, danger + 1.0f);
    if (!totem)
        return false;

    return LeaveCircle(totem->GetPosition(), danger, true);
}

Unit* MennuKillTotemsAction::PickTotem()
{
    float const danger = NovaDangerRadius(bot);
    Creature* nova = bot->FindNearestCreature(NPC_CORRUPTED_NOVA_TOTEM, TOTEM_SEARCH_RADIUS);

    // Killing the nova totem sets the nova off at once, so only from range and only when nobody is next to it.
    constexpr float spellReach = 30.0f;
    if (nova && PlayerbotAI::IsRanged(bot) && bot->GetExactDist2d(nova) > danger + TbcHazard::EXIT_MARGIN &&
        bot->IsWithinDistInMap(nova, spellReach) && bot->IsWithinLOSInMap(nova) &&
        !TbcHazard::AnyGroupMemberWithin(bot, nova->GetPosition(), danger + TbcHazard::EXIT_MARGIN))
    {
        return nova;
    }

    for (uint32 entry : { NPC_MENNU_HEALING_WARD, NPC_TAINTED_EARTHGRAB_TOTEM })
    {
        Creature* totem = bot->FindNearestCreature(entry, TOTEM_SEARCH_RADIUS);
        if (!totem || !bot->IsWithinLOSInMap(totem))
            continue;

        // A melee bot does not walk into a live nova to reach it.
        if (nova && PlayerbotAI::IsMelee(bot) && totem->GetExactDist2d(nova) < danger + TbcHazard::EXIT_MARGIN)
            continue;

        return totem;
    }

    return nullptr;
}

bool MennuKillTotemsAction::isUseful()
{
    Unit* totem = PickTotem();
    return totem && AI_VALUE(Unit*, "current target") != totem;
}

bool MennuKillTotemsAction::Execute(Event /*event*/)
{
    Unit* totem = PickTotem();
    if (!totem || AI_VALUE(Unit*, "current target") == totem)
        return false;

    return Attack(totem);
}
