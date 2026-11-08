/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_WORLDBOSSSTRATEGY_H
#define PLAYERBOTS_WORLDBOSSSTRATEGY_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include "WorldBossTactics.h"

// [world_boss] on the open-world maps (0, 1, 530: PlayerbotAI::ApplyInstanceStrategies); every trigger returns at
// once out of combat or without a world boss in the fight (one cached picture per bot, WorldBossTactics.cpp)
class WorldBossStrategy : public Strategy
{
public:
    WorldBossStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "world-boss"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

class WorldBossTriggerContext : public NamedObjectContext<Trigger>
{
public:
    WorldBossTriggerContext()
    {
        creators["world boss tank swap"] = [](PlayerbotAI* ai) -> Trigger* { return new WorldBossTankSwapTrigger(ai); };
        creators["world boss threat reset"] = [](PlayerbotAI* ai) -> Trigger*
        { return new WorldBossThreatResetTrigger(ai); };
        creators["world boss loose add"] = [](PlayerbotAI* ai) -> Trigger* { return new WorldBossLooseAddTrigger(ai); };
        creators["world boss add heal cast"] = [](PlayerbotAI* ai) -> Trigger*
        { return new WorldBossHealerAddCastTrigger(ai); };
        creators["world boss priority add"] = [](PlayerbotAI* ai) -> Trigger*
        { return new WorldBossPriorityAddTrigger(ai); };
        creators["world boss dispel"] = [](PlayerbotAI* ai) -> Trigger* { return new WorldBossDispelTrigger(ai); };
        creators["world boss carrier"] = [](PlayerbotAI* ai) -> Trigger* { return new WorldBossCarrierTrigger(ai); };
        creators["world boss spread"] = [](PlayerbotAI* ai) -> Trigger* { return new WorldBossSpreadTrigger(ai); };
        creators["world boss move out"] = [](PlayerbotAI* ai) -> Trigger* { return new WorldBossMoveOutTrigger(ai); };
        creators["world boss position"] = [](PlayerbotAI* ai) -> Trigger* { return new WorldBossPositionTrigger(ai); };
        creators["world boss raid cooldown"] = [](PlayerbotAI* ai) -> Trigger*
        { return new WorldBossRaidCooldownTrigger(ai); };
    }
};

class WorldBossActionContext : public NamedObjectContext<Action>
{
public:
    WorldBossActionContext()
    {
        creators["world boss taunt"] = [](PlayerbotAI* ai) -> Action* { return new WorldBossTauntAction(ai); };
        creators["world boss taunt add"] = [](PlayerbotAI* ai) -> Action* { return new WorldBossTauntAddAction(ai); };
        creators["world boss interrupt add"] = [](PlayerbotAI* ai) -> Action*
        { return new WorldBossInterruptAddAction(ai); };
        creators["world boss attack priority add"] = [](PlayerbotAI* ai) -> Action*
        { return new WorldBossAttackPriorityAddAction(ai); };
        creators["world boss dispel"] = [](PlayerbotAI* ai) -> Action* { return new WorldBossDispelAction(ai); };
        creators["world boss carrier move"] = [](PlayerbotAI* ai) -> Action*
        { return new WorldBossCarrierMoveAction(ai); };
        creators["world boss spread out"] = [](PlayerbotAI* ai) -> Action* { return new WorldBossSpreadAction(ai); };
        creators["world boss move out of area"] = [](PlayerbotAI* ai) -> Action*
        { return new WorldBossMoveOutAction(ai); };
        creators["world boss position"] = [](PlayerbotAI* ai) -> Action* { return new WorldBossPositionAction(ai); };
        creators["world boss raid cooldown"] = [](PlayerbotAI* ai) -> Action*
        { return new WorldBossRaidCooldownAction(ai); };
    }
};

#endif
