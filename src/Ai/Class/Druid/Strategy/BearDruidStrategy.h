/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BEARDRUIDSTRATEGY_H
#define PLAYERBOTS_BEARDRUIDSTRATEGY_H

#include "FeralDruidStrategy.h"

class PlayerbotAI;

// [rot_b31_pdd] Survival Instincts and Frenzied Regeneration (3 min each) are a boss's cooldowns: on dungeon trash
// only while the bear's health really drops (below BEAR_TRASH_COOLDOWN_PCT and still falling by
// Playerbots.TankCooldownLossPct in two seconds) or is below BEAR_TRASH_LAST_STAND_PCT. an-h build 30: both went
// on the trash pull at 506-560 s ("damage spike" at 71 % health) and were down for Hadronox at 589 s. Barkskin
// (1 min): [b33_vh] on trash below "medium health" or while health falls. A boss fight is a boss target or a boss
// among the attackers, not IsEncounterInProgress() alone (true for a whole Violet Hold event).
class BearCooldownPacingMultiplier : public Multiplier
{
public:
    static constexpr float BEAR_TRASH_COOLDOWN_PCT = 40.0f;
    static constexpr float BEAR_TRASH_LAST_STAND_PCT = 25.0f;

    BearCooldownPacingMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "bear cooldown pacing") {}

    float GetValue(Action* action) override;
};

class BearDruidStrategy : public FeralDruidStrategy
{
public:
    BearDruidStrategy(PlayerbotAI* botAI);

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "bear"; }
    std::vector<NextAction> getDefaultActions() override;
    uint32 GetType() const override { return STRATEGY_TYPE_TANK | STRATEGY_TYPE_MELEE; }
};

#endif
