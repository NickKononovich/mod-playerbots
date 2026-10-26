/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RangedCombatStrategy.h"
#include "FollowActions.h"
#include "Log.h"
#include "Playerbots.h"
#include "RangedPositionActions.h"
#include "Timer.h"

void RangedCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    CombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("enemy too close for spell",
                                        { NextAction("flee", ACTION_MOVE + 4) }));
}

void RangedPositionStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Stepping out of a cleave beats a critical heal (30-34): two cleaves kill the healer and then everyone.
    triggers.push_back(new TriggerNode("ranged in melee danger",
                                       { NextAction("move to ranged position", ACTION_MOVE + 6) }));
    // A mob meleeing the bot: to the tank before stepping out of anyone's melee, and before the heals.
    triggers.push_back(new TriggerNode("chased away from tank", { NextAction("move to tank", ACTION_MOVE + 7) }));
    // A healer still walking one of those steps while its heal target needs it now stops: a cast-time heal cannot
    // start on the move, and the walks above wait for the heal (HealComesFirst in RangedPositionActions.cpp).
    triggers.push_back(new TriggerNode("healer stops to heal", { NextAction("stop to heal", ACTION_MOVE + 8) }));
    // Walking back into heal range waits for critical heals but not for low and medium ones (20-22).
    triggers.push_back(new TriggerNode("healer too far from tank",
                                       { NextAction("move to ranged position", ACTION_HIGH + 5) }));
    // [prepull_sight_spot] Out of combat, before the tank's first hit: to a spot that will see the fight (above
    // "follow", below the los pull gather and the pull moves).
    triggers.push_back(new TriggerNode("prepull sight spot",
                                       { NextAction("move to prepull sight spot", ACTION_MOVE + 1.0f) }));
}

void RangedPositionStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new RangedPositionFollowMultiplier(botAI));
}

float RangedPositionFollowMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<FollowAction*>(action))
        return 1.0f;

    RangedPositionInfo const info = AI_VALUE(RangedPositionInfo, "group ranged position");
    if (!info.fighting)
        return 1.0f;

    // [rjoin-follow] A bot that lost the fight walks to it with "move to ranged position"; when every walk of that
    // approach fails (only unsafe spots, no path) it stood there for good with follow held: Sunken Temple 2026-10-25,
    // the hunter 18 s idle 16 yd from the fight (pull 48), the mage 27 s idle 74 yd behind (pull 2). After
    // APPROACH_STALL_RELEASE_MS of failed walks it follows its leader back to the group instead.
    uint32 const stall = AI_VALUE(uint32, "ranged approach stall");
    if (!info.approach)
    {
        if (stall)
            context->GetValue<uint32>("ranged approach stall")->Set(0);
        return 0.0f;
    }

    if (!stall || getMSTimeDiff(stall, getMSTime()) < RangedApproachStallValue::APPROACH_STALL_RELEASE_MS)
        return 0.0f;

    uint32 const now = getMSTime();
    if (!_loggedMs || getMSTimeDiff(_loggedMs, now) >= FOLLOW_RELEASE_LOG_MS)
    {
        _loggedMs = now;
        LOG_INFO("playerbots", "[rjoin-follow] {} follows: its approach failed for {} ms ({}, {:.1f} yd from the tank)",
                 bot->GetName(), getMSTimeDiff(stall, now), info.reason, info.tankDistance);
    }

    return 1.0f;
}
