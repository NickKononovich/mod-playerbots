/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaderTriggers.h"

#include "Group.h"
#include "LeaderOrders.h"  // [lead15] the leader's member events
#include "LeaderValues.h"
#include "Playerbots.h"
#include "RaidLeaderState.h"
#include "Timer.h"

using namespace RaidLeader;

namespace
{
    bool IsCoordinator(AiObjectContext* context, Player* bot)
    {
        return context->GetValue<ObjectGuid>("raid coordinator")->Get() == bot->GetGUID();
    }

    bool AnyMemberDead(Group* group)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->IsInWorld() && !member->IsAlive())
                    return true;

        return false;
    }

    uint32 BotsToAnswer(Player* coordinator)
    {
        uint32 count = 0;
        Group* group = coordinator->GetGroup();
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != coordinator && member->GetMap() == coordinator->GetMap() &&
                GET_PLAYERBOT_AI(member))
                ++count;
        }
        return count;
    }
}

bool LeaderReadyCheckNeededTrigger::IsActive()
{
    Group* group = bot->GetGroup();
    if (!group || bot->IsInCombat() || !bot->IsAlive() || !IsCoordinator(context, bot) ||
        AI_VALUE(bool, "encounter in progress"))
        return false;

    ReadyCheckPhase phase = ReadyCheckPhase::Idle;
    uint32 retryAtMs = 0;
    bool recovering = false;
    uint32 wipeMs = 0;
    ObjectGuid readyBoss;
    uint32 readyMs = 0;
    Registry::Instance().With(group->GetGUID(),
                              [&](GroupState& state)
                              {
                                  phase = state.phase;
                                  retryAtMs = state.retryAtMs;
                                  recovering = state.recovering;
                                  wipeMs = state.wipeMs;
                                  readyBoss = state.readyBoss;
                                  readyMs = state.readyMs;
                              });

    uint32 const now = getMSTime();
    if (phase == ReadyCheckPhase::Pending)
        return false;

    if (phase == ReadyCheckPhase::RetryWait)
        return int32(now - retryAtMs) >= 0;

    // After a wipe: resurrections first, then the ready check (which also rebuffs).
    if (recovering)
        return !AnyMemberDead(group) || getMSTimeDiff(wipeMs, now) > WIPE_RECOVERY_MAX_MS;

    ObjectGuid const boss = AI_VALUE(ObjectGuid, "leader nearby boss");
    if (!boss)
        return false;

    return readyBoss != boss || !readyMs || getMSTimeDiff(readyMs, now) > READY_VALID_MS;
}

bool LeaderReadyCheckDoneTrigger::IsActive()
{
    Group* group = bot->GetGroup();
    if (!group || !IsCoordinator(context, bot))
        return false;

    bool pending = false;
    uint32 startMs = 0;
    size_t answers = 0;
    Registry::Instance().With(group->GetGUID(),
                              [&](GroupState& state)
                              {
                                  pending = state.phase == ReadyCheckPhase::Pending;
                                  startMs = state.checkStartMs;
                                  answers = state.answers.size();
                              });

    if (!pending)
        return false;

    if (bot->IsInCombat())
        return true;  // the result action cancels the check

    uint32 const elapsed = getMSTimeDiff(startMs, getMSTime());
    if (elapsed < READY_CHECK_MIN_WAIT_MS)
        return false;

    return elapsed >= READY_CHECK_ANSWER_MS || answers >= BotsToAnswer(bot);
}

bool LeaderEncounterEventTrigger::IsActive()
{
    return AI_VALUE(LeaderEvent, "leader encounter event").type != LeaderEventType::None;
}

bool LeaderTankFlagsTrigger::IsActive()
{
    Group* group = bot->GetGroup();
    if (!group || !AI_VALUE(TankAssignment, "leader tank assignment").changed)
        return false;

    uint32 const now = getMSTime();
    return Registry::Instance().With(group->GetGUID(),
                                     [now](GroupState& state)
                                     {
                                         return !state.tankFlagsQueuedMs ||
                                                getMSTimeDiff(state.tankFlagsQueuedMs, now) > TANK_FLAGS_RETRY_MS;
                                     });
}

bool ReadyCheckLowManaTrigger::IsActive()
{
    return bot->IsAlive() && !bot->IsInCombat() && AI_VALUE2(bool, "has mana", "self target") &&
           AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig.HighMana &&
           AI_VALUE(bool, "ready check in progress");
}

bool GroupStandsGate::GroupStands(PlayerbotAI* botAI, Player* bot)
{
    // [b31_food_buff] at the same spot (within STILL_RANGE) at two looks STILL_MS apart or more, and not moving now
    constexpr float STILL_RANGE = 1.0f;
    constexpr uint32 STILL_MS = 3 * IN_MILLISECONDS;
    if (bot->IsInCombat() || bot->isMoving())
        return false;
    if (botAI->GetAiObjectContext()->GetValue<bool>("ready check in progress")->Get())
        return true;

    Player* leader = botAI->GetGroupLeader();
    if (!leader || leader->GetMap() != bot->GetMap())
        leader = bot;

    uint32 const now = getMSTime();
    bool const same = _leaderPosMs && leader->GetExactDist(&_leaderPos) <= STILL_RANGE;
    if (!same)
    {
        _leaderPos = leader->GetPosition();
        _leaderPosMs = now ? now : 1;
        return false;
    }
    return !leader->isMoving() && getMSTimeDiff(_leaderPosMs, now) >= STILL_MS;
}

bool NeedRaidFlaskTrigger::IsActive()
{
    return !AI_VALUE(ObjectGuid, "raid flask").IsEmpty() && GroupStands(botAI, bot);  // [b31_food_buff]
}

bool NeedFoodBuffTrigger::IsActive()
{
    return !AI_VALUE(ObjectGuid, "raid food buff").IsEmpty() && GroupStands(botAI, bot);  // [b31_food_buff]
}

bool PrePullPotionTrigger::IsActive()
{
    return AI_VALUE(uint32, "pre pull window") && !AI_VALUE(ObjectGuid, "pre pull potion").IsEmpty();
}

bool SoulwellAvailableTrigger::IsActive() { return !AI_VALUE(ObjectGuid, "nearby soulwell").IsEmpty(); }

bool SoulPortalAvailableTrigger::IsActive() { return !AI_VALUE(ObjectGuid, "nearby soul portal").IsEmpty(); }

bool WipeEscapeTrigger::IsActive() { return !AI_VALUE(std::string, "wipe escape spell").empty(); }

bool WipeEscapeOverTrigger::IsActive()
{
    return (AI_VALUE(uint32, "wipe escape at") || bot->HasAura(SPELL_DIVINE_INTERVENTION_AURA)) &&
           !AI_VALUE(bool, "wipe escape hold");
}

// [lead15] The leader's member events (the members decide only how: their rotation and their own threat).
bool LeaderMoveOrderTrigger::IsActive()
{
    if (!Leader::LEADER_MEMBER_EVENTS || !bot->IsAlive())
        return false;
    Leader::MemberEvents events;
    if (!Leader::Events::Get(bot->GetGUID(), events) ||
        (events.move != Leader::MoveEvent::MoveTo && events.move != Leader::MoveEvent::StandAt))
        return false;
    return bot->GetExactDist2d(events.spot.GetPositionX(), events.spot.GetPositionY()) > events.tolerance ||
           !bot->IsStandState();
}

bool LeaderAttackOrderTrigger::IsActive()
{
    if (!Leader::LEADER_MEMBER_EVENTS || !Leader::LEADER_ATTACK_EVENTS || !bot->IsAlive())
        return false;
    Leader::MemberEvents events;
    if (!Leader::Events::Get(bot->GetGUID(), events) || events.attack != Leader::AttackEvent::Attack)
        return false;
    Unit* target = botAI->GetUnit(events.target);
    // only a mob already in the fight (the leader's pull opened it): the event never opens a pull of its own
    return target && target->IsAlive() && target->IsInCombat() && bot->GetVictim() != target &&
           target->GetMap() == bot->GetMap();
}

bool BurstOverTrigger::IsActive()
{
    uint32 const until = AI_VALUE(uint32, "burst until");
    return until && int32(getMSTime() - until) >= 0;
}
