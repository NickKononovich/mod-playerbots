/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DruidBearActions.h"
#include "Playerbots.h"
#include "TankAoeThreat.h"  // [b32_peel]
#include "TerrainHazard.h"

// [bear_aoe_threat] The gap closer ("enemy out of melee", ACTION_MOVE + 1) ran above "reach melee" and so past its
// guards: dme-n build 30 (20261025-184229-dme-n-5H-29, pull 9) the bear charged 21 yd onto a patrol's Warpwood
// Tangler and landed in the aggro reach of the 11-mob Warpwood Crusher pack the planner had refused: 4 -> 15
// attackers, wipe. A dungeon tank does not charge onto a spot an idle pack reaches (IdleMobReachingSpot, as the
// defend-member opener and "reach melee" judge it); the walk in or the taunts take the mob instead.
bool CastFeralChargeBearAction::isUseful()
{
    if (!CastReachTargetSpellAction::isUseful())
        return false;

    Unit* target = GetTarget();
    // [b30_oculus] no charge onto a target in the air or down a ledge (GapCloserLeavesGround: occ-h 184229-26)
    if (target && GapCloserLeavesGround(bot, target, "feral charge - bear"))
        return false;

    // [b32_peel] with mobs on the bear, no charge away from them onto a mob that is not at a group member
    if (!TankPeel::MayCloseGapOnTarget(bot, target))
        return false;

    return !target || !PlayerbotAI::IsTank(bot) || !botAI->IsInNonRaidDungeon() ||
           !IdleMobReachingSpot(botAI, bot, target);
}

// [b31_deep] wowsims feral tank APL queues Maul at 25+ rage. 30 keeps Mangle (15 with Ferocity) plus this Maul
// (10) affordable with a little slack; 45 starved Maul in the level 60-79 gear (6.5-9.6/min on bosses).
bool CastMaulAction::isUseful()
{
    constexpr uint8 MAUL_MIN_RAGE = 30;
    return CastMeleeSpellAction::isUseful() && AI_VALUE2(uint8, "rage", "self target") >= MAUL_MIN_RAGE;
}
