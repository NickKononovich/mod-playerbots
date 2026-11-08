/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGTRIGGERS_H
#define PLAYERBOTS_NEWRPGTRIGGERS_H

#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Trigger.h"

#include <functional>
#include <string>

class NewRpgStatusTrigger : public Trigger
{
public:
    NewRpgStatusTrigger(PlayerbotAI* botAI, NewRpgStatus status = RPG_IDLE)
        : Trigger(botAI, "new rpg status"), status(status)
    {
    }
    bool IsActive() override;

protected:
    NewRpgStatus status;
};

// A selfbot leading a group holds still while a member is not with it, by the member's facts only ([timers] no wait
// cap, no hp / mana line): dead (its corpse run or a res brings it back), fighting, sitting (eating / drinking: its
// rest runs to full), or beyond the core's group reward distance (Player::IsAtGroupRewardDistance: no kill credit, no
// XP share there). Whether the next pull is won with the members' hp / mana is the pull judge's call (GrindTarget).
class NewRpgGroupNotReadyTrigger : public Trigger
{
public:
    NewRpgGroupNotReadyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "new rpg group not ready", 2) {}
    bool IsActive() override;

private:
    // why the member holds the group ("" = it is with the group)
    std::string MemberNotReady(Player* member);

    std::string _waitReason;  // the member and reason kind last logged (one [rpg] GROUP_WAIT line per change)
    std::string _ownPlanLogged;  // [wait13] the members not waited for, last logged (one line per change)
};

// Taxi splines sometimes stop mid-route (#2863, #2664). The watch is shared by the trigger and the recovery
// action and covers the bot and the group bots on its map (they fly the same route).
constexpr float DISCOVER_FLIGHT_MASTER_DISTANCE = 10.0f;

// A quest game object a group member used is waiting to be used by this bot too (see UseQuestGameObjectAction).
class QuestGameObjectPendingTrigger : public Trigger
{
public:
    QuestGameObjectPendingTrigger(PlayerbotAI* botAI) : Trigger(botAI, "quest game object pending", 2) {}
    bool IsActive() override;
};

namespace TaxiStallWatch
{
    constexpr float STILL_DISTANCE = 1.0f;

    void ForEachWatched(Player* bot, std::function<void(Player*, NewRpgInfo&)> const& fn);
    // Records where the player is; true once its taxi is proven stalled ([timers] no stall timer): the flight's spline
    // ended short of the route's end, or the world's clock since the last sample covers the time the spline needs at
    // its own velocity to fly STILL_DISTANCE and the player has not moved that far
    bool IsStalled(Player* player, NewRpgInfo& info);
}

class NewRpgTaxiStalledTrigger : public Trigger
{
public:
    NewRpgTaxiStalledTrigger(PlayerbotAI* botAI) : Trigger(botAI, "new rpg taxi stalled", 5) {}
    bool IsActive() override;
};

// The selfbot leader is away (the client's AFK flag, a game fact; [timers] no AFK duration): rest in an inn so the
// group gains rested experience. Off until there is a real "session is ending" signal (see IsActive).
class NewRpgLeaderAfkTrigger : public Trigger
{
public:
    NewRpgLeaderAfkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "new rpg leader afk", 5) {}
    bool IsActive() override;
};

// A flight master within a few yards whose flight path the bot or a group bot next to it has not discovered.
class NewRpgUnknownFlightMasterTrigger : public Trigger
{
public:
    NewRpgUnknownFlightMasterTrigger(PlayerbotAI* botAI) : Trigger(botAI, "new rpg unknown flight master", 5) {}
    bool IsActive() override;
};

#endif
