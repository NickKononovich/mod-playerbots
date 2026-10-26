/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SHActions.h"
#include "Playerbots.h"
#include "SHShared.h"

using namespace ShatteredHalls;

float KargathRingFor(Player* bot)
{
    return PlayerbotAI::IsHeal(bot) ? KARGATH_HEALER_RING : KARGATH_RANGED_RING;
}

bool KargathLeaveBladeDanceAction::Execute(Event /*event*/)
{
    Unit* kargath = TbcHazard::FindBoss(botAI, KARGATH_NAME);
    if (!IsBladeDancing(kargath))
        return false;

    return LeaveCircle(kargath->GetPosition(), BladeDanceDangerRadius(bot, kargath), true);
}

bool KargathKeepRingAction::isUseful()
{
    // a heal or a nuke under way finishes first; the step comes between casts
    return !bot->IsNonMeleeSpellCast(false, false, true);
}

bool KargathKeepRingAction::Execute(Event /*event*/)
{
    Unit* kargath = TbcHazard::FindBoss(botAI, KARGATH_NAME);
    if (!kargath || IsBladeDancing(kargath))
        return false;

    return MoveToRing(kargath->GetPosition(), KargathRingFor(bot), MovementPriority::MOVEMENT_COMBAT);
}
