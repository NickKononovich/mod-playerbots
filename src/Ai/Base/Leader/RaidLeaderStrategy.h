/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDLEADERSTRATEGY_H
#define PLAYERBOTS_RAIDLEADERSTRATEGY_H

#include "LeaderOrders.h"  // [lead18] MemberEvents
#include "Multiplier.h"
#include "Strategy.h"

#include <vector>

class PlayerbotAI;

// Every bot carries it: the coordinator's triggers fire only on the coordinator, the multiplier on everyone.
class RaidLeaderStrategy : public Strategy
{
public:
    RaidLeaderStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "raid leader"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

// Out of combat in raid instances: own flask, buff food, pre-pull potion, Soulwell.
class RaidConsumablesStrategy : public Strategy
{
public:
    RaidConsumablesStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "raid consumables"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

class Unit;
class NextAction;

// [lead15] LEADER_OWN_DECISION_CHECK: after the engine executed `action` for a member the dungeon leader leads, notes
// an own decision (Leader::Events::NoteOwn) when it moved, changed the target, sat to rest, cast an external / CC /
// burn cooldown or a buff on another without a matching leader command. Personal saves and the rotation's spells on
// the commanded target are exempt.
namespace RaidLeader
{
    void CheckOwnDecision(PlayerbotAI* botAI, Action* action, Unit* victimBefore);
    // [lead18] LEADER_EXECUTOR_TRIGGER_GATE: every handler of a trigger (or a default action) is vetoed for this led
    // member under `events`, decided by name and class only (no target, no value computed)
    bool GateVetoes(PlayerbotAI* botAI, Leader::MemberEvents const& events, std::vector<NextAction> const& handlers);
}

// Holds attacks before the ready check passes, for a few seconds after the boss pull, and after a wipe call.
class RaidLeaderHoldMultiplier : public Multiplier
{
public:
    RaidLeaderHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "raid leader hold") {}

    float GetValue(Action* action) override;
    // [lead15] the executor's vetoes are the leader's: no no-damage release of them
    bool KeepsVeto(Action* action) override;
};

#endif
