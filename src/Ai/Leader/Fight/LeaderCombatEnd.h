/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERCOMBATEND_H
#define PLAYERBOTS_LEADER_LEADERCOMBATEND_H

// [lead107 stage 5] user: production code does not live in the test harness. The leader's end of a combat that is no
// fight (the phantom-combat end and its mob hunt) and its drop of a stale idle victim, moved out of
// TestHarnessMgr::TickRun unchanged; the harness calls them through the group view.

#include "Common.h"
#include "LeaderGroup.h"

#include <vector>

class Player;

namespace Leader::Fight
{

    // anyInCombat: the group's pull state, false while a phantom combat counts out (activeBoss: a boss attempt runs)
    void TickPhantomCombat(LeaderGroup& group, std::vector<Player*> const& players, Player* leader, uint32 now,
                           bool activeBoss, bool& anyInCombat);
    void TickStaleVictim(LeaderGroup& group, Player* leader, uint32 now, bool anyInCombat, bool activeBoss);
}

#endif
