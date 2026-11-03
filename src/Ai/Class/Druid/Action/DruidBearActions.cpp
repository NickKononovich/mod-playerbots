/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DruidBearActions.h"
#include "Playerbots.h"
#include "TankAoeThreat.h"  // [b32_peel]
#include "TerrainHazard.h"
#include "Creature.h"  // [rot334]
#include "Log.h"       // [rot334]

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

// [rot334] b220 rfc-h: Lacerate's stack uptime on the bosses could only be inferred from its damage (no stack or
// time data in the harness). One debug line per Lacerate on a dungeon boss: the bear's own stacks and seconds left
// before and after the cast, the ms since the first and the previous Lacerate on that boss (a stack count of 0 before
// a cast after the first = a drop), rage after the cast and Mangle's cooldown, so the time to 5 stacks, the share of
// the fight at 5 and the drops can be summed from the log.
bool CastLacerateAction::Execute(Event event)
{
    Unit* target = GetTarget();
    Aura const* before = target ? botAI->GetAura("lacerate", target, true, false) : nullptr;
    uint8 const stacksBefore = before ? before->GetStackAmount() : 0;
    int32 const leftBeforeMs = before ? before->GetDuration() : 0;

    bool const cast = CastMeleeSpellAction::Execute(event);

    Creature* boss = target ? target->ToCreature() : nullptr;
    if (!cast || !boss || !boss->IsDungeonBoss() || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return cast;

    uint32 const now = getMSTime();
    if (traceTarget != boss->GetGUID() || !traceFirstMs)
    {
        traceTarget = boss->GetGUID();
        traceFirstMs = now;
        traceLastMs = 0;
    }

    Aura const* after = botAI->GetAura("lacerate", boss, true, false);
    uint32 const mangle = AI_VALUE2(uint32, "spell id", "mangle (bear)");
    LOG_DEBUG("playerbots",
              "[bear lacerate] {} on {}: stacks {} -> {}, {:.1f} -> {:.1f} s left | {} ms after the first Lacerate "
              "on it, {} ms after the last | rage {} | mangle cd {} ms | boss {:.0f}% hp, attackers {}",
              bot->GetName(), boss->GetName(), stacksBefore, after ? after->GetStackAmount() : 0,
              leftBeforeMs / float(IN_MILLISECONDS), after ? after->GetDuration() / float(IN_MILLISECONDS) : 0.0f,
              getMSTimeDiff(traceFirstMs, now), traceLastMs ? getMSTimeDiff(traceLastMs, now) : 0,
              bot->GetPower(POWER_RAGE) / 10,
              mangle && bot->HasSpellCooldown(mangle) ? bot->GetSpellCooldownDelay(mangle) : 0, boss->GetHealthPct(),
              AI_VALUE(GuidVector, "attackers").size());
    traceLastMs = now;
    return cast;
}

// [b31_deep] [rot31] wowsims feral_tank_druid__default queues Maul at 25+ rage (was 30 here)
bool CastMaulAction::isUseful()
{
    constexpr uint8 MAUL_MIN_RAGE = 25;
    return CastMeleeSpellAction::isUseful() && AI_VALUE2(uint8, "rage", "self target") >= MAUL_MIN_RAGE;
}
