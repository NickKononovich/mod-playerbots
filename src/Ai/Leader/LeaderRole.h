/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LEADERROLE_H
#define _PLAYERBOT_LEADERROLE_H

// [lead31b] The leader is a ROLE of every group (user 2026-10-31): every bot has a Leader; a solo bot is a group of
// one led by itself; when bots group, one Leader leads and the others stand down (their bots execute); a bot that
// leaves gets its own Leader back. Harness runs drive it (TestHarness is one caller), world groups through their
// leader bot, later maybe a human-led group with tools.
//
// THIS STEP: the role registry only (who is the active leader of a bot's group, stand-down / handoff on join and
// leave). Nothing calls OnJoin / OnLeave yet and no world or solo bot runs on it: their behaviour is unchanged. The
// migration is a separate, approved stage (C:/WoW/LEADER_REFACTOR_PLAN.md "Leader as a role").

#include "ObjectGuid.h"

class Group;
class Player;

namespace Leader
{
    namespace Role
    {
        // The active leader of the bot's group: the group's leader bot, the bot itself when solo (a group of one).
        ObjectGuid ActiveLeaderOf(Player* bot);
        bool IsActiveLeader(Player* bot);
        // The bot's own Leader stands down while another leads its group.
        bool StoodDown(Player* bot);
        // Handoff hooks (not wired yet): on a join the joiner's Leader stands down to the group's; on a leave the
        // bot's own Leader is active again.
        void OnJoin(Player* bot, Group* group);
        void OnLeave(Player* bot);
    }
}

#endif
