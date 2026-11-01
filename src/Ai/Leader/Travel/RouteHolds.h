/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_ROUTEHOLDS_H
#define PLAYERBOTS_LEADER_ROUTEHOLDS_H

// [lead107] user: production code does not live in the test harness. The leader's holds on the route walk after the
// rest - the post-kill wave hold (a boss whose death sends the wave that opens the next door), the level-split rejoin
// (a member on another level close below / above, out of sight) and the no-walk watch (harness_hold "no walk",
// leader_unstuck) - moved out of TestHarnessMgr::TickTravel unchanged; the harness calls them through the group view.

#include "Common.h"
#include "LeaderGroup.h"

class Player;

namespace Leader::Travel
{
    // true: the group holds at the kill spot for the boss's wave (the leader's walk point is cleared)
    bool PostKillWaveHold(LeaderGroup& group, Player* leader, uint32 now);
    // true: the leader walks to a member split off on another level (its walk point holds)
    bool LevelSplitRejoin(LeaderGroup& group, Player* leader, uint32 now);
    // the no-walk watch: a target set, `traveling`, no walk point and the leader standing - logged with what holds
    // it; `compactPause`: the travel pause holds it
    void NoWalkWatch(LeaderGroup& group, Player* leader, uint32 now, bool compactPause, bool traveling);
}

#endif
