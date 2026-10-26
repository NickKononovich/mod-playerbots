/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoSMultipliers.h"
#include "Action.h"
#include "CheckMountStateAction.h"
#include "ChooseTargetActions.h"
#include "CoSActions.h"
#include "CoSTriggers.h"
#include "GenericSpellActions.h"
#include "MovementActions.h"
#include "RangedPositionActions.h"  // [b44_cos] MoveToRangedPositionAction (EpochMultiplier)
#include "RtiAction.h"  // [b39_cos] AutoCcMarkAction, CastAutoCcAction, PrePullCcAction

float EpochMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "chrono-lord epoch");
    if (!boss) { return 1.0f; }

    // [b44_cos] In the fight the ranged members' place is the "epoch stack" band (just outside the danger edge, in
    // the boss' sight): "move to ranged position" (ranged in melee danger 7 yd out, the 20 yd pack spot, the healer's
    // approach) pulled them off it and the stack pulled them back (cos-h: 25 move_conflicts in 12 runs). The stack
    // itself steps out of his reach and walks into his sight.
    if (dynamic_cast<MoveToRangedPositionAction*>(action) && !PlayerbotAI::IsMelee(bot) && boss->IsAlive() &&
        boss->IsInCombat() && boss->GetMap() == bot->GetMap())
        return 0.0f;

    if (bot->getClass() == CLASS_HUNTER) { return 1.0f; }

    if (dynamic_cast<FleeAction*>(action)) { return 0.0f; }

    return 1.0f;
}

// cos-n/h 2026-10-25: "check mount state" (100) mounted the carrier between two crates, the mount's cast broke the
// Arcane Disruption (23214 / 23228) and the crate had to be tried again; a player walks the few yards on foot.
float CoSCrateMountMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<CheckMountStateAction*>(action))
        return 1.0f;

    if (bot->IsInCombat() || CoSHelpers::GetProgress(bot) != CoSIds::COS_PROGRESS_NOT_STARTED ||
        !bot->HasItemCount(CoSIds::ITEM_ARCANE_DISRUPTOR, 1))
        return 1.0f;

    return 0.0f;
}

// cos-h 2026-10-25 062726: 17 pulls in 6 min of the last city (mostly "unplanned (tank's reach melee)" after
// "aggressive target" picked an idle Risen Zombie 27737, and "careful pull hopeless (Risen Zombie brings 25 mobs)")
// ate the last 3 min of the Guardian of Time timer; the Corruptor failed at 07:01:09.
float CoSLastCityIdlePullMultiplier::GetValue(Action* action)
{
    constexpr uint32 LOG_INTERVAL_MS = 10 * IN_MILLISECONDS;

    if (!dynamic_cast<AggressiveTargetAction*>(action) || bot->GetMapId() != CoSIds::MAP_CULLING_OF_STRATHOLME)
        return 1.0f;

    uint32 const progress = CoSHelpers::GetProgress(bot);
    if (progress < CoSIds::COS_PROGRESS_FINISHED_CITY_INTRO || progress > CoSIds::COS_PROGRESS_BEFORE_MALGANIS)
        return 1.0f;

    Unit* target = AI_VALUE(Unit*, "aggressive target");
    if (!target || target->IsInCombat() || target->GetEntry() == CoSIds::NPC_INFINITE_CORRUPTOR)
        return 1.0f;

    // The city waves (heroic, Guardian of Time running): only the idle mobs that are no part of Arthas' event
    // (cos-h 121320: Meathook stood 3 min while the tank took one idle Risen Zombie after another). From the Town
    // Hall on, every idle mob, on both difficulties, as before.
    if (progress < CoSIds::COS_PROGRESS_REACHED_TOWN_HALL && !CoSHelpers::IsSkippableEventTrash(bot, target))
        return 1.0f;

    uint32 const now = getMSTime();
    if (!_logMs || getMSTimeDiff(_logMs, now) >= LOG_INTERVAL_MS)
    {
        _logMs = now;
        LOG_INFO("playerbots", "[cos-idle-pull-veto] {}: leaves idle {} ({}) {:.0f} yd away, keeps with Arthas "
                 "(progress {})", bot->GetName(), target->GetName(), target->GetEntry(), bot->GetDistance(target),
                 progress);
    }
    return 0.0f;
}

// [b39_cos] cos-h 20261026-050341-cos-h-5H-18 wave 6 (05:18:59 -> 05:20:10, pull 19, 6 mobs): the planner refused
// the wave as it stood, "[pre-pull cc] Rtdruag plans Risen Zombie (6 mobs, refused as it stands)", the mage claimed the
// Dark Necromancer for Polymorph, the hunter the Tomb Stalker for Freezing Arrow; the group then stood 25 s
// (05:19:26 -> 05:19:51, nobody casting) next to the held mobs and the wave took 71 s for 6 kills (every wave of the
// clean 20261025-221216-cos-h-5D-23: 36-47 s for 6-7). Waves 6-10 took 7:34 against the clean run's 5:57, trash
// fights 203 s against 146 s. A player group AoEs the wave down; nothing is held while the timer runs.
float CoSTimedRunCcMultiplier::GetValue(Action* action)
{
    if (bot->GetMapId() != CoSIds::MAP_CULLING_OF_STRATHOLME ||
        (!dynamic_cast<AutoCcMarkAction*>(action) && !dynamic_cast<CastAutoCcAction*>(action) &&
         !dynamic_cast<PrePullCcAction*>(action)))
        return 1.0f;

    return CoSHelpers::IsGuardianTimerRunning(bot) ? 0.0f : 1.0f;
}
