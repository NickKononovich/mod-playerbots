/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADERTRIGGERS_H
#define PLAYERBOTS_LEADERTRIGGERS_H

#include "Position.h"
#include "Trigger.h"

class PlayerbotAI;
class Player;

// Coordinator: a boss is close and the group has not passed a ready check for it, or a wipe is recovered.
class LeaderReadyCheckNeededTrigger : public Trigger
{
public:
    LeaderReadyCheckNeededTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader ready check needed", 2000) {}

    bool IsActive() override;
};

// Coordinator: the answers of the running ready check are in, or its time is up.
class LeaderReadyCheckDoneTrigger : public Trigger
{
public:
    LeaderReadyCheckDoneTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader ready check done", 1000) {}

    bool IsActive() override;
};

class LeaderEncounterEventTrigger : public Trigger
{
public:
    LeaderEncounterEventTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader encounter event") {}

    bool IsActive() override;
};

class LeaderTankFlagsTrigger : public Trigger
{
public:
    LeaderTankFlagsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader tank flags", 3000) {}

    bool IsActive() override;
};

// Member: a ready check runs and the bot is short on mana; it drinks its own water.
class ReadyCheckLowManaTrigger : public Trigger
{
public:
    ReadyCheckLowManaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ready check low mana", 2000) {}

    bool IsActive() override;
};

// [b31_food_buff] The flask and the food buff (Well Fed: ~10 s sitting) only while the group stands: out of combat,
// a ready check running or the group leader (the bot itself when it leads) at the same spot since the last look of
// this trigger (5 s) and not moving. mara-h 20261025-184229-mara-h-5A-52 20:02:50: the shaman sat down to eat while
// the group travelled, the tank walked on 46 yd, the far wait cycled.
class GroupStandsGate
{
protected:
    bool GroupStands(PlayerbotAI* botAI, Player* bot);

private:
    Position _leaderPos;
    uint32 _leaderPosMs = 0;
};

class NeedRaidFlaskTrigger : public Trigger, protected GroupStandsGate
{
public:
    NeedRaidFlaskTrigger(PlayerbotAI* botAI) : Trigger(botAI, "need raid flask", 5000) {}

    bool IsActive() override;
};

class NeedFoodBuffTrigger : public Trigger, protected GroupStandsGate
{
public:
    NeedFoodBuffTrigger(PlayerbotAI* botAI) : Trigger(botAI, "need food buff", 5000) {}

    bool IsActive() override;
};

class PrePullPotionTrigger : public Trigger
{
public:
    PrePullPotionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pre pull potion", 500) {}

    bool IsActive() override;
};

class SoulwellAvailableTrigger : public Trigger
{
public:
    SoulwellAvailableTrigger(PlayerbotAI* botAI) : Trigger(botAI, "soulwell available", 3000) {}

    bool IsActive() override;
};

class SoulPortalAvailableTrigger : public Trigger
{
public:
    SoulPortalAvailableTrigger(PlayerbotAI* botAI) : Trigger(botAI, "soul portal available", 2000) {}

    bool IsActive() override;
};

// Member: the leader called the fight lost and the bot has a way out of it ("wipe escape spell").
class WipeEscapeTrigger : public Trigger
{
public:
    WipeEscapeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "wipe escape") {}

    bool IsActive() override;
};

// Member out of a lost fight: the fight is over and the mobs went home, or it was found again.
class WipeEscapeOverTrigger : public Trigger
{
public:
    WipeEscapeOverTrigger(PlayerbotAI* botAI) : Trigger(botAI, "wipe escape over") {}

    bool IsActive() override;
};

// The time of an ordered "burst" is up.
class BurstOverTrigger : public Trigger
{
public:
    BurstOverTrigger(PlayerbotAI* botAI) : Trigger(botAI, "burst over", 1000) {}

    bool IsActive() override;
};

// [lead15] The dungeon leader's MoveTo / StandAt event for this member (LeaderOrders.h Events), its spot not reached.
class LeaderMoveOrderTrigger : public Trigger
{
public:
    LeaderMoveOrderTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader move order") {}

    bool IsActive() override;
};

// [lead76] The leader's "top up X": a member to heal named and below full.
class LeaderHealTargetTrigger : public Trigger
{
public:
    LeaderHealTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader heal target") {}

    bool IsActive() override;
};

// [lead15] The leader's Attack event: its target is in the fight and the member attacks something else / nothing.
class LeaderAttackOrderTrigger : public Trigger
{
public:
    LeaderAttackOrderTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader attack order") {}

    bool IsActive() override;
};

#endif
