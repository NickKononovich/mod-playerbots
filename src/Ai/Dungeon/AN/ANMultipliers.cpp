/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ANMultipliers.h"
#include "ANActions.h"
#include "ANTriggers.h"
#include "Action.h"
#include "ChooseTargetActions.h"
#include "GenericSpellActions.h"
#include "MovementActions.h"
#include "PullStrategy.h"
#include "TbcHazard.h"

float KrikthirMultiplier::GetValue(Action* action)
{
    if (!PlayerbotAI::IsDps(bot)) { return 1.0f; }

    // Target is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    Unit* boss = nullptr;
    Unit* watcher = nullptr;

    GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");
    for (auto& target : targets)
    {
        Unit* unit = botAI->GetUnit(target);
        if (!unit) { continue; }

        switch (unit->GetEntry())
        {
            case NPC_KRIKTHIR:
                boss = unit;
                continue;
            case NPC_WATCHER_SILTHIK:
            case NPC_WATCHER_GASHRA:
            case NPC_WATCHER_NARJIL:
            case NPC_WATCHER_SKIRMISHER:
            case NPC_WATCHER_SHADOWCASTER:
            case NPC_WATCHER_WARRIOR:
                watcher = unit;
                continue;
        }
    }

    if (boss && watcher)
    {
        // Do not target swap
        if (dynamic_cast<DpsAssistAction*>(action))
        {
            return 0.0f;
        }

        // Area spells only where they cannot reach a pack that is not in the fight (KrikthirWatcherRoomMultiplier);
        // the packs fought at the rally take them (wave 2026-10-24: no AoE at all on 2 Warriors + 2 Shadowcasters).
    }
    return 1.0f;
}

float KrikthirWatcherRoomMultiplier::GetValue(Action* action)
{
    if (bot->GetMapId() != ANWatcherRoom::MAP_AZJOL_NERUB)
        return 1.0f;

    // The planner's pull (careful plan, lone-mob minimum, approach) waits for the room tactic's pull.
    if (dynamic_cast<AggressiveTargetAction*>(action))
        return !bot->IsInCombat() && ANWatcherRoom::OwnsPull(botAI) ? 0.0f : 1.0f;

    bool const aoe = action->getThreatType() == Action::ActionThreatType::Aoe;
    bool const reposition = TbcHazard::IsRepositionAction(action) && !PlayerbotAI::IsHeal(bot);
    if (!aoe && !reposition)
        return 1.0f;

    ANWatcherRoom::Snapshot const& snapshot = ANWatcherRoom::Get(botAI);
    if (!snapshot.active || snapshot.idle.empty())
        return 1.0f;

    // The tank's own ranged pull: the walk to its cast spot, the opener on the pack, the way back
    // (KrikthirWatcherPullAction).
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (strategy && (strategy->HasTarget() || strategy->IsPullPendingToStart() || strategy->HasPullStarted()))
        return 1.0f;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (aoe)
    {
        bool const nearBot =
            ANWatcherRoom::IdleNear(snapshot, bot->GetPositionX(), bot->GetPositionY(), ANWatcherRoom::AOE_SAFE);
        bool const nearTarget = target && ANWatcherRoom::IdleNear(snapshot, target->GetPositionX(),
                                                                  target->GetPositionY(), ANWatcherRoom::AOE_SAFE);
        return nearBot || nearTarget ? 0.0f : 1.0f;
    }

    // Chasing a mob that stands in a pack's reach (a Skirmisher after its fixate, a Shadowcaster casting from
    // range) is what drew the other packs in; the rally stays put and the ranged kill it from there.
    return ANWatcherRoom::ApproachUnsafe(botAI, target) ? 0.0f : 1.0f;
}
