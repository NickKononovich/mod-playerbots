/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERCONSUMABLES_H
#define PLAYERBOTS_LEADER_LEADERCONSUMABLES_H

// [lead344] user: «Стенд никого не двигает»: the buffing pass's flask and food are the leader's act, moved out of
// TestHarnessMgr::UseConsumables / UseItem unchanged, and so is the boss's buff pass (TestHarnessMgr::PrePot); the
// group's owner only names the member's items (LeaderGroup::MemberConsumables).

#include "Common.h"
#include "LeaderGroup.h"

class Creature;
class Player;
class Unit;

namespace Leader::Dungeon
{
    // the bot uses the item `itemId` from its bags on itself (its on-use spell), as the client sends it
    bool UseItem(Player* bot, uint32 itemId);
    // a flask's aura on the bot
    bool HasFlaskAura(Player* bot);
    // the buffing pass's flask (`food` false) or food step for one member; true: it used one
    bool UseConsumables(LeaderGroup& group, Player* bot, bool food);
    // [lead344] the route's boss within BOSS_BUFF_DISTANCE of the leader, not fighting: the group buffs before it
    // (once per boss; not for a rift boss or a scripted wave that just became attackable)
    void BossBuffPass(LeaderGroup& group);
    // [lead1090] the idle dungeon boss within BOSS_BUFF_DISTANCE of the leader with no buff pass yet that `pull` is
    // or brings (BossBroughtBy): the pull waits for his pass. nullptr: no boss's pass holds this pull
    Creature* BossBuffPassHolds(LeaderGroup& group, Player* leader, Unit* pull);
    // [lead1090] the leader opens the pass before `boss` itself, whatever the harness phase; false: it may not open
    // now (the leader fights, a fight is on or a pull order is open). true: opened, or a pass is under way
    bool OpenBossBuffPass(LeaderGroup& group, Creature* boss);
}

#endif
