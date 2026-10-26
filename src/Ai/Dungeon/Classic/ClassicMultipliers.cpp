/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ClassicMultipliers.h"
#include "ClassicActions.h"
#include "ClassicShared.h"
#include "FollowActions.h"
#include "MovementActions.h"
#include "Playerbots.h"
#include "TbcHazard.h"

using namespace ScarletMonastery;

float MograineHoldMultiplier::GetValue(Action* action)
{
    if (bot->GetMapId() != SCARLET_MONASTERY_MAP_ID || !TbcHazard::IsRepositionAction(action))
        return 1.0f;

    Unit* mograine = IdleMograineWithChapel(bot);
    if (!mograine)
        return 1.0f;

    // [b38_sm] A follow toward a master inside the hold band waits where the keep-away left the bot (its exit spot,
    // past the band): ShouldHoldOutside lets every reposition from beyond the band, the follow walked back in and the
    // keep-away out again. sm-cath-n 20261026-050341-sm-cath-n-5B-19 (1076-1091 s): Rlpalbm, Rlshabk, Rtrogay
    // "'follow' and 'mograine keep away' pull it opposite ways, 3 flips within 20 s" (151-176 deg) while the tank
    // walked to Mograine. Only near the band: a follower farther off still closes up to it.
    constexpr float holdEdge = MOGRAINE_KEEP_AWAY + 1.0f + TbcHazard::HOLD_BUFFER;
    constexpr float followHoldReach = 10.0f;
    if (dynamic_cast<FollowAction*>(action) && bot->GetExactDist2d(mograine) < holdEdge + followHoldReach)
    {
        Player* anchor = botAI->GetMaster() ? botAI->GetMaster() : botAI->GetGroupLeader();
        if (anchor && anchor != bot && anchor->IsInWorld() && anchor->GetMapId() == bot->GetMapId() &&
            anchor->GetExactDist2d(mograine) < holdEdge)
            return 0.0f;
    }

    return TbcHazard::ShouldHoldOutside(botAI, mograine->GetPosition(), MOGRAINE_KEEP_AWAY) ? 0.0f : 1.0f;
}

float DoanMultiplier::GetValue(Action* action)
{
    bool const movement = dynamic_cast<MovementAction*>(action) != nullptr;
    if (!movement && !TbcHazard::IsRepositionAction(action))
        return 1.0f;

    Unit* doan = FindDoan(botAI);
    if (!doan || !doan->IsAlive())
        return 1.0f;

    if (IsDetonationPending(doan))
    {
        if (dynamic_cast<DoanEscapeDetonationAction*>(action))
            return 1.0f;

        bool const safe =
            IsSafeFromDetonation(bot, doan, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        return !safe && movement ? 0.0f : (safe && TbcHazard::IsRepositionAction(action) ? 0.0f : 1.0f);
    }

    if (PlayerbotAI::IsMelee(bot) || PlayerbotAI::IsTank(bot) || dynamic_cast<DoanKeepRangeAction*>(action) ||
        !TbcHazard::IsRepositionAction(action))
        return 1.0f;

    return TbcHazard::ShouldHoldOutside(botAI, doan->GetPosition(), ARCANE_EXPLOSION_RADIUS + bot->GetCombatReach())
               ? 0.0f
               : 1.0f;
}
