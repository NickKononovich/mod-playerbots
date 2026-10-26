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

// A selfbot leading a group holds still while a member is dead, eating/drinking, low on health or mana, still
// fighting or far behind. A member that never recovers (stuck, no water) only holds the group for a while.
class NewRpgGroupNotReadyTrigger : public Trigger
{
public:
    NewRpgGroupNotReadyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "new rpg group not ready", 2) {}
    bool IsActive() override;

private:
    bool IsMemberNotReady(Player* member);

    static constexpr uint32 MAX_WAIT_MS = 2 * MINUTE * IN_MILLISECONDS;
    static constexpr uint32 IGNORE_AFTER_TIMEOUT_MS = MINUTE * IN_MILLISECONDS;
    static constexpr float MAX_MEMBER_DISTANCE = 40.0f;

    uint32 _notReadySince = 0;
    uint32 _ignoreSince = 0;
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
    constexpr uint32 STALL_MS = 20 * IN_MILLISECONDS;
    constexpr float STILL_DISTANCE = 1.0f;

    void ForEachWatched(Player* bot, std::function<void(Player*, NewRpgInfo&)> const& fn);
    // Records where the player is; true once its taxi has not moved for STALL_MS.
    bool IsStalled(Player* player, NewRpgInfo& info);
}

class NewRpgTaxiStalledTrigger : public Trigger
{
public:
    NewRpgTaxiStalledTrigger(PlayerbotAI* botAI) : Trigger(botAI, "new rpg taxi stalled", 5) {}
    bool IsActive() override;
};

// The selfbot leader has been away (AFK) for a while: rest in an inn so the group gains rested experience.
class NewRpgLeaderAfkTrigger : public Trigger
{
public:
    NewRpgLeaderAfkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "new rpg leader afk", 5) {}
    bool IsActive() override;

private:
    static constexpr uint32 AFK_BEFORE_INN_MS = 10 * MINUTE * IN_MILLISECONDS;
    uint32 _afkSince = 0;
};

// A flight master within a few yards whose flight path the bot or a group bot next to it has not discovered.
class NewRpgUnknownFlightMasterTrigger : public Trigger
{
public:
    NewRpgUnknownFlightMasterTrigger(PlayerbotAI* botAI) : Trigger(botAI, "new rpg unknown flight master", 5) {}
    bool IsActive() override;
};

#endif
