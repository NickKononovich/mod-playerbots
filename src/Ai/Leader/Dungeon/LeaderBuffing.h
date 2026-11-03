/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERBUFFING_H
#define PLAYERBOTS_LEADER_LEADERBUFFING_H

// [lead107 stage 5] user: production code does not live in the test harness. The leader's buffing pass before a pull
// (the buffs the group gives, the flask and food steps, the ready check and its end), moved out of
// TestHarnessMgr::TickBuffing unchanged; the pass's test setup (the run start's full health / mana, the test
// consumables' items) and the run phase stay with the group's owner (LeaderGroup::BuffPassSetup / MemberConsumables /
// NoteActivity).

#include "Common.h"
#include "LeaderGroup.h"

#include <string>
#include <vector>

class Player;

namespace Leader::Dungeon
{
    // an aura of the bot whose spell name contains `text`; the Well Fed aura
    bool HasAuraNamed(Player* bot, char const* text);
    bool HasWellFed(Player* bot);
    // [lead136] an aura named like `text` that lasts `needMs` more (or has no end)
    bool HasAuraLasting(Player* bot, char const* text, int32 needMs);

    // one step of the buffing pass while the group needs its buffs (LeaderGroup::NeedBuff)
    void TickBuffing(LeaderGroup& group, std::vector<Player*> const& players, Player* leader);
    // [lead775] the buffs the group can give now that are not on every member they are for (lasting the next
    // fight): "<member> <buff>, ..."; empty: all present
    std::string GivableBuffsMissing(LeaderGroup& group, std::vector<Player*> const& players, Player* leader);
}

#endif
