/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOT_NEWRPGDOGATHER_H
#define PLAYERBOT_NEWRPGDOGATHER_H

#include "NewRpgBaseAction.h"

class NewRpgDoGatherAction : public NewRpgBaseAction
{
public:
    NewRpgDoGatherAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg do gather") {}
    bool Execute(Event event) override;
    // [guild14] its last gather run in this zone at this level found no node it may walk to (all refused or done):
    // no new gather run here until it levels or changes zone
    static bool NoSafeNodes(Player* bot);

protected:
    // Release the loot pipeline's target if it points at the given spawn.
    // Called whenever the gather action abandons a node, so a half-done
    // harvest can't leave a stale loot target behind (which would gate off
    // loot-target replacement and wedge all subsequent harvesting).
    void ClearLootTargetForNode(ObjectGuid::LowType spawnId);

    // Drop the current node target (releasing the loot target with it) and
    // reset the per-node state; markVisited writes the spawn off for this
    // gather session.
    void AbandonNode(NewRpgInfo::DoGather& data, bool markVisited = false);

    // [timers] class (b): cadence for the periodic target-liveness and switch checks (a node scan, expensive); the
    // checks decide by facts.
    const uint32 passiveCheckInterval = 2 * IN_MILLISECONDS;
    // A live node passed within this range takes priority over the current
    // (possibly unverified) target.
    const float nodeSwitchDistance = 80.0f;
    // Required distance gain before switching to a live node that is not
    // in immediate pickup range.
    const float switchMinGain = 20.0f;
    // A live node this close is always picked up (if strictly closer than
    // the current target) - the cost of grabbing it is negligible and
    // skipping it reads as "ran past a herb".
    const float nodePickupDistance = 30.0f;
};

#endif
