/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#include "ForceRebuff.h"
#include "Common.h"
#include "GenericSpellActions.h"
#include "HealthTriggers.h"  // [lead152] RestState::PredictNextFight
#include "LeaderOrders.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Timer.h"
#include <algorithm>

// [lead44] user rule (no forcing timers): the window has no time limit - it closes when its buffs are done ("ready
// reply", ReadyReplyAction, once a cycle runs no buff)

void ForceRebuffState::Begin(bool reply)
{
    replyToReadyCheck = reply || (IsPending() && replyToReadyCheck);
    pending = true;
    buffPendingThisCycle = true;
    beginMs = getMSTime();
}

bool ForceRebuffState::IsOnGlobalCooldown() const
{
    return bot && lastGcdSpell && bot->GetGlobalCooldownMgr().HasGlobalCooldown(lastGcdSpell);
}

bool ForceRebuffState::IsPending() const
{
    return pending;
}

void ForceRebuffState::NoteBuffWork()
{
    if (!bot || !IsPending() || bot->IsInCombat())
        return;

    buffPendingThisCycle = true;
}

void ForceRebuffState::NoteBuffProposed()
{
    if (!bot || !IsPending() || bot->IsInCombat())
        return;

    buffProposedThisCycle = true;
}

bool ForceRebuffState::BuffBelowRefreshTarget(int32 remaining, int32 maxDuration, uint32 baseBeforeDuration) const
{
    if (bot && IsPending() && !bot->IsInCombat())
    {
        if (remaining <= 0 || maxDuration <= 0)
            return false;

        // [lead86] b128: every boss prep ran to the harness's 15 s cap ("Rtpalaa rebuff window open") - every buff
        // that had lost ForceRebuffMarginSecs was recast, a 10-minute blessing with 9 minutes left included. A buff
        // needs the pass only when it runs out within the margin (the buff's own remaining time): the window closes
        // once every member has every buff it needs
        (void)maxDuration;
        // [lead152] b171 20261101-144503 before Oggleflint: the healer's pass ("Rtdruaa buffing" 30 s) recast buffs
        // with minutes left and drank after it. A led bot recasts only a buff that runs out before the next fight
        // ends (the planner's estimate of its length, RestState::PredictNextFight)
        RestState::NextFight fight;
        if (Leader::LedBot(bot) && RestState::PredictNextFight(bot, fight) && fight.fightSec > 0.0f)
            return float(remaining) < fight.fightSec * float(IN_MILLISECONDS);
        return uint32(remaining) < sPlayerbotAIConfig.ForceRebuffMarginSecs * IN_MILLISECONDS;
    }

    return baseBeforeDuration && uint32(remaining) < baseBeforeDuration;
}

// During a force-rebuff, healing yields to buffing: heals are suppressed while
// buff work is proposed or the last cast's GCD runs, so the pass is not stretched
// by interleaved heals.
class ForceRebuffBuffFirstMultiplier : public Multiplier
{
public:
    ForceRebuffBuffFirstMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "force rebuff buff first") {}

    float GetValue(Action* action) override
    {
        ForceRebuffState const& rebuff = botAI->forceRebuff;
        if (!action || !rebuff.IsPending() || botAI->GetBot()->IsInCombat())
            return 1.0f;

        if (!dynamic_cast<CastHealingSpellAction*>(action))
            return 1.0f;

        return rebuff.IsBuffProposedThisCycle() || rebuff.IsOnGlobalCooldown() ? 0.0f : 1.0f;
    }
};

void ForceRebuffStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("force rebuff pending", { NextAction("ready reply", 6.0f) }));
}

void ForceRebuffStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new ForceRebuffBuffFirstMultiplier(botAI));
}
