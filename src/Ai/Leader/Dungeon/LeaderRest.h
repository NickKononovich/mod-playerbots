/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERREST_H
#define PLAYERBOTS_LEADER_LEADERREST_H

// [lead107 stage 5] user: production code does not live in the test harness. The leader's rest and far wait before
// the next pull (who the group waits for: the regen wait, the far / trailing members, the door-cut member, the
// regroup ping-pong hold, wait_timeout), the wait for dead / outside members and the travel pause for a trailing
// member, moved out of TestHarnessMgr::TickRun unchanged; the harness calls them through the group view.

#include "Common.h"
#include "LeaderGroup.h"

#include <string>
#include <vector>

class Player;

namespace Leader::Dungeon
{
    // Regen wait: a member that should sit and still stands this long is logged as a "[regen]" stall (a diagnostic
    // only: the wait for it is the leader's Rest, and what keeps it from resting ends the wait at once)
    constexpr uint32 REGEN_SIT_GRACE_MS = 8 * IN_MILLISECONDS;
    // [regen_wait_below_line] a sitter is waited for only below its own rest line, a standing member only below
    // the hard lines; false restores "every sitter until it stands up, every member below its line".
    constexpr bool REGEN_WAIT_BELOW_LINE = true;
    // Meet <-> route ping-pong (2026-10-24, *.bak_doorgate; BRS heroic 22:41: the far wait walked the leader back,
    // the route took it on again, for minutes). [lead128] the ping-pong is a fact of the decisions: since the group
    // was last together the decision came back to one it had taken (meet -> route -> meet); the meet is then held
    // until every living member stands within REGROUP_PING_PONG_TOGETHER of the leader on its map (FAR_RESUME_RANGE
    // while it holds) or a fight starts - no clock, no cooldown.
    constexpr bool REGROUP_PING_PONG_GUARD = true;
    constexpr uint8 DECISION_MEET = 1;
    constexpr uint8 DECISION_ROUTE = 2;
    constexpr float REGROUP_PING_PONG_TOGETHER = 30.0f;
    constexpr float REGROUP_PING_PONG_LEVEL_DZ = 6.0f;  // a member this far above / below is on another level
    constexpr float REGROUP_PING_PONG_MEET_RANGE = 45.0f;  // MeetMember's MEET_RANGE
    // [b31_far_threshold] mara-h 20261025-184229-mara-h-5A-52: the shaman followed at 10-25 yd, on the steep ramps
    // the 3D distance crossed the far line and the tank walked on (user: the tank waits for its group). Apart is
    // the horizontal distance plus the dz beyond FAR_SLOPE_DZ_ALLOWANCE (a ramp); far: beyond FAR_WAIT_RANGE, or
    // beyond FAR_TRAIL_RANGE and not coming (MemberClosing) while the leader stands; a member waited for stays far
    // until within FAR_RESUME_RANGE. While travelling the leader pauses for a member over FAR_TRAIL_RANGE behind
    // while it comes (CompactPause).
    constexpr float FAR_SLOPE_DZ_ALLOWANCE = 10.0f;
    constexpr float FAR_WAIT_RANGE = 40.0f;
    constexpr float FAR_TRAIL_RANGE = 25.0f;
    constexpr float FAR_RESUME_RANGE = 15.0f;

    // the apart metric: horizontal distance plus the dz beyond FAR_SLOPE_DZ_ALLOWANCE (a ramp)
    float ApartMetric(Player* bot, Player* leader);
    // Food (spell category 11) or water (59) in the bags, the items the bot's own "food" / "drink" use.
    uint32 CountRestItems(Player* bot, uint32 spellCategory);
    // [lead128] the member comes to the leader: it walks, and its walk ends nearer the leader than it stands
    bool MemberClosing(Player* bot, Player* leader);

    // the wait for the dead / outside members (`away`: their names)
    void WaitForAway(LeaderGroup& group, std::vector<Player*> const& players, Player* leader,
                     std::vector<std::string> const& away);
    // the rest / far wait before the next pull; true: the group travels on. ledGather: the leader's trailing set
    // decides the far / compact / ping-pong gates
    bool RestWait(LeaderGroup& group, std::vector<Player*> const& players, Player* leader, uint32 now, bool wingWalking,
                  bool& ledGather);
    // the travel pause for a trailing member; true: the leader pauses
    bool CompactPause(LeaderGroup& group, Player* leader, uint32 now, bool ledGather);
}

#endif
