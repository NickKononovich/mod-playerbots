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
    // Regen wait: a member who waits this long without sitting down to eat / drink is a "[regen]" stall.
    constexpr uint32 REGEN_SIT_GRACE_MS = 8 * IN_MILLISECONDS;
    // Wave 21 (*.bak_w21idle): a member still standing this long after its regen_stall is no longer waited for.
    constexpr uint32 REGEN_STALL_RELEASE_MS = 20 * IN_MILLISECONDS;
    // [regen_wait_below_line] a sitter is waited for only below its own rest line, a standing member only below
    // the hard lines; false restores "every sitter until it stands up, every member below its line".
    constexpr bool REGEN_WAIT_BELOW_LINE = true;
    // Meet <-> route ping-pong (2026-10-24, *.bak_doorgate), false restores the plain alternation. BRS heroic
    // 22:41: the tank above (z 66.6 at 76,-327, 4x bot_cannot_move), the others below (z 49-54) 45-61 yd away; the
    // far wait walked the leader back (meet_member), the leader came within 45 yd, the route took over and picked
    // the next target (Legionnaire <-> Warlock, both "blocks route" and unseen by the tank), the leader walked off,
    // the far wait came again - for minutes. After REGROUP_PING_PONG_FLIPS changes of decision with no gap over
    // REGROUP_PING_PONG_GAP_MS, the first REGROUP_PING_PONG_MS ago, the meet is held: no route until every living
    // member stands within REGROUP_PING_PONG_TOGETHER of the leader on its map (MeetMember: the members' own
    // follow first - the group gathers on the leader's level - then the leader's walk back over the route), at
    // most REGROUP_PING_PONG_HOLD_MS, then REGROUP_PING_PONG_COOLDOWN_MS without a new hold.
    constexpr bool REGROUP_PING_PONG_GUARD = true;
    constexpr uint8 DECISION_MEET = 1;
    constexpr uint8 DECISION_ROUTE = 2;
    constexpr uint32 REGROUP_PING_PONG_FLIPS = 3;
    constexpr uint32 REGROUP_PING_PONG_MS = 20 * IN_MILLISECONDS;
    constexpr uint32 REGROUP_PING_PONG_GAP_MS = 45 * IN_MILLISECONDS;
    // At most 60 s (ok-n wave 8: two members 41 yd away on the lower ramp held the group 279 s over 90 s holds).
    constexpr uint32 REGROUP_PING_PONG_HOLD_MS = 60 * IN_MILLISECONDS;
    constexpr uint32 REGROUP_APART_IGNORE_MS = 3 * MINUTE * IN_MILLISECONDS;  // after a timed-out hold
    // [b30_topped_up] wait_timeouts in a row with the same member far: then no far wait for it (and no
    // GroupFullyReady check while it stays far) for FAR_TIMEOUTS_IGNORE_MS. dmw-n 20261025-184229-dmw-n-5C-16: 5
    // wait_timeouts of 60 s for "Rlpriam(far)", stranded in Immol'thar's pit, each 45 s after the last.
    constexpr uint32 FAR_TIMEOUTS_DROP = 2;
    constexpr uint32 FAR_TIMEOUTS_IGNORE_MS = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 REGROUP_PING_PONG_COOLDOWN_MS = 2 * MINUTE * IN_MILLISECONDS;
    constexpr float REGROUP_PING_PONG_TOGETHER = 30.0f;
    constexpr float REGROUP_PING_PONG_LEVEL_DZ = 6.0f;  // a member this far above / below is on another level
    constexpr float REGROUP_PING_PONG_MEET_RANGE = 45.0f;  // MeetMember's MEET_RANGE (TestHarnessRunner.cpp)
    // [meet_hysteresis] MeetMember's MEET_END_RANGE: the member being met counts far until this close - now
    // FAR_RESUME_RANGE ([b31_far_threshold]; [meet_end_follow_trail] had 30, before it 20)
    // [b31_far_threshold] mara-h 20261025-184229-mara-h-5A-52: the shaman followed at 10-25 yd, on the steep ramps
    // the 3D distance (horizontal + dz) crossed the far line, the group held 1-13 s, the shaman came to 5-9 yd,
    // the tank walked on, the next ramp the same; user: «чё танк не подождал его? чё он полез вперед? Мы ж не
    // эгоисты, должны подождать». Apart is measured as the horizontal distance plus the dz beyond
    // FAR_SLOPE_DZ_ALLOWANCE (a ramp); far: beyond FAR_WAIT_RANGE, or beyond FAR_TRAIL_RANGE without closing in
    // for FAR_NOT_CLOSING_MS while the leader stands; a member waited for (a far wait, a meet, a ping-pong hold)
    // stays far until within FAR_RESUME_RANGE - the group goes on together. While travelling the leader pauses
    // (COMPACT_PAUSE_MAX_MS at most) for a member over FAR_TRAIL_RANGE behind, before a far wait comes of it.
    constexpr float FAR_SLOPE_DZ_ALLOWANCE = 10.0f;
    constexpr float FAR_WAIT_RANGE = 40.0f;
    constexpr float FAR_TRAIL_RANGE = 25.0f;
    constexpr float FAR_RESUME_RANGE = 15.0f;
    constexpr float FAR_CLOSING_STEP = 2.0f;
    constexpr uint32 FAR_NOT_CLOSING_MS = 5 * IN_MILLISECONDS;
    constexpr uint32 COMPACT_PAUSE_MAX_MS = 10 * IN_MILLISECONDS;
    constexpr uint32 COMPACT_PAUSE_GAP_MS = 20 * IN_MILLISECONDS;  // no new pause this soon after one ran out
    // [b33_harn] vh-h 20261026-001930-vh-h-5D-67: a pause begun at 754 s was not evaluated through the fight and
    // the drinking (other phases), "timed out" at its first look back at 803 s ("sec 49", 22.7 yd) and the tank
    // walked on with the healer still drinking 20 yd behind; he body-pulled at 815 s, the healer 31 yd off, and
    // the group wiped. A pause not evaluated for COMPACT_PAUSE_STALE_MS starts its clock anew, and while the
    // trailing member is a healer drinking / eating, out of the leader's sight or beyond FAR_RESUME_RANGE, the
    // pause holds up to COMPACT_PAUSE_HEALER_MS (bounded: then the far wait takes over as before).
    constexpr uint32 COMPACT_PAUSE_STALE_MS = 3 * IN_MILLISECONDS;
    constexpr uint32 COMPACT_PAUSE_HEALER_MS = 30 * IN_MILLISECONDS;

    // the apart metric: horizontal distance plus the dz beyond FAR_SLOPE_DZ_ALLOWANCE (a ramp)
    float ApartMetric(Player* bot, Player* leader);
    // Food (spell category 11) or water (59) in the bags, the items the bot's own "food" / "drink" use.
    uint32 CountRestItems(Player* bot, uint32 spellCategory);

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
