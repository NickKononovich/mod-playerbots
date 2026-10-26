/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TANKTARGETVALUE_H
#define PLAYERBOTS_TANKTARGETVALUE_H

#include "ObjectGuid.h"
#include "RtiTargetValue.h"

#include <string>

class Creature;
class PlayerbotAI;
class Player;
class Unit;

// User rules (Stockade 2026-10-24, «мобов не добили, танк уже за другими побежал»): a dungeon tank fights only mobs
// already in the group's fight, and starts no new pull (an idle mob or pack) while the fight is not over or the
// group is not ready. A patrol walking into the fight is the exception: it comes anyway. Read on the tank's own map
// thread (group members of a 5-man share it): unit state only, no other bot's AI values.
namespace TankPullRules
{
    // In combat with the group: hits a member (or a member's pet or totem), or is in combat with a member.
    bool IsEngagedWithGroup(Player* bot, Unit* unit);
    // An idle patrol on its waypoints that walks into the tank's reach right now.
    bool IsPatrolComing(Player* bot, Creature* creature);
    // Mobs of the current fight still up: engaged with the group, alive, not crowd controlled, `except` left out.
    uint32 FightMobsLeft(PlayerbotAI* botAI, Unit* except);
    // Empty when the group may take a new pull; else why not ("Rlprial drinking 45 %", "healer Rlpalbx dead").
    std::string GroupNotReady(Player* bot);
    // Why the tank does not take this idle mob now, empty when it may (not idle, a patrol coming, fight over and
    // group ready). `checkReady`: also the group's readiness (the pull planner checks it itself).
    std::string RefusesIdle(PlayerbotAI* botAI, Unit* unit, bool checkReady);
    // "[tank target] <bot> skips idle <mob>: <reason>", one line per tank and mob per 5 s.
    void LogSkip(Player* bot, Unit* unit, std::string const& reason);
}

class TankTargetValue : public RtiTargetValue
{
public:
    // A mob under a crowd control icon (any but Skull and Cross) that hits the group this long without being held is
    // the tank's: the crowd control had its chance (wave 11 Shadow Labyrinth: a free moon mob killed a shaman in
    // 82 s, nobody targeted it and the tank never entered its combat engine).
    static constexpr uint32 FREE_MARK_TAKE_MS = 3000;
    static constexpr uint8 ICON_COUNT = 8;  // TARGETICONCOUNT (Group.h), checked in the .cpp

    TankTargetValue(PlayerbotAI* botAI, std::string const name = "tank target") : RtiTargetValue(botAI, "rti", name) {}

    Unit* Calculate() override;

private:
    // [rot_b31_cc] per raid icon: the crowd control mob seen hitting the group unheld, and since when
    ObjectGuid _freeMarks[ICON_COUNT];
    uint32 _freeMarkSinceMs[ICON_COUNT] = {};
};

#endif
