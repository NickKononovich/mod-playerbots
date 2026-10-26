/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LOOTROLLACTION_H
#define PLAYERBOTS_LOOTROLLACTION_H

#include "Group.h"
#include "QueryItemUsageAction.h"

class PlayerbotAI;

struct ItemTemplate;

// What a bot wants from one roll, before the roll-level config and the group claim compare.
struct LootRollDecision
{
    RollVote vote = PASS;
    bool mainSpec = false;  // a need for the bot's own spec: competes with the other bots' main-spec needs
    float priority = 0.0f;  // upgrade size scaled down by the bot's recent need wins
};

class LootRollAction : public QueryItemUsageAction
{
public:
    LootRollAction(PlayerbotAI* botAI, std::string const name = "loot roll") : QueryItemUsageAction(botAI, name) {}

    bool Execute(Event event) override;

protected:
    LootRollDecision DecideRoll(ItemTemplate const* proto, int32 randomProperty, uint8 voteMask);

private:
    RollVote CalculateRollVote(ItemTemplate const* proto, ItemUsage usage);
    float CalculateUpgradeGain(ItemTemplate const* proto, int32 randomProperty);
    bool CompletesSetBonus(ItemTemplate const* proto);
};

bool CanBotUseToken(ItemTemplate const* proto, Player* bot);
bool RollUniqueCheck(ItemTemplate const* proto, Player* bot);

// SMSG_LOOT_START_ROLL: publishes this bot's claim at once so the other bots can compare against it; the
// vote itself is cast by "loot roll" once every claim had time to arrive.
class MasterLootRollAction : public LootRollAction
{
public:
    MasterLootRollAction(PlayerbotAI* botAI) : LootRollAction(botAI, "master loot roll") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// SMSG_LOOT_ROLL_WON: counts this bot's need wins for loot fairness.
class RecordLootWinAction : public Action
{
public:
    RecordLootWinAction(PlayerbotAI* botAI) : Action(botAI, "record loot win") {}

    bool Execute(Event event) override;
};

class RollAction : public Action
{
public:
    RollAction(PlayerbotAI* botAI) : Action(botAI, "roll") {}

    bool Execute(Event event) override;
};

#endif
