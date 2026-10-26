/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgTriggers.h"
#include "Creature.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PlayerbotsUserPack.h"
#include "Timer.h"
#include "Transport.h"

bool NewRpgStatusTrigger::IsActive() { return status == botAI->rpgInfo.GetStatus(); }

bool NewRpgGroupNotReadyTrigger::IsMemberNotReady(Player* member)
{
    if (!member->IsAlive())
        return true;

    if (member->GetMapId() != bot->GetMapId())
        return false;

    if (member->IsInCombat() || !member->IsStandState())
        return true;

    if (member->GetHealthPct() < 50.0f)
        return true;

    if (member->getPowerType() == POWER_MANA && member->GetMaxPower(POWER_MANA) > 0 &&
        member->GetPowerPct(POWER_MANA) < 40.0f)
        return true;

    return bot->GetDistance(member) > MAX_MEMBER_DISTANCE;
}

bool NewRpgGroupNotReadyTrigger::IsActive()
{
    // The user's pack (Playerbots.UserPack) leads its group without him: it waits for its members like a SelfBot.
    if ((!IsSelfBot(bot) && !UserPack::IsAutonomousLeader(bot)) || bot->IsInCombat() || bot->IsInFlight())
        return false;

    // Boarding, riding and leaving a boat run on its schedule; the leader handles the group itself there.
    NewRpgInfo::TripPhase const phase = botAI->rpgInfo.trip.phase;
    if (bot->GetTransport() || phase == NewRpgInfo::TripPhase::Board || phase == NewRpgInfo::TripPhase::Ride ||
        phase == NewRpgInfo::TripPhase::Leave)
        return false;

    Group* group = bot->GetGroup();
    if (!group || group->GetLeaderGUID() != bot->GetGUID())
        return false;

    uint32 const now = getMSTime();
    if (_ignoreSince && getMSTimeDiff(_ignoreSince, now) < IGNORE_AFTER_TIMEOUT_MS)
        return false;

    bool notReady = false;
    for (GroupReference* ref = group->GetFirstMember(); ref && !notReady; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && IsMemberNotReady(member))
            notReady = true;
    }

    if (!notReady)
    {
        _notReadySince = 0;
        return false;
    }

    if (!_notReadySince)
        _notReadySince = now;

    if (getMSTimeDiff(_notReadySince, now) > MAX_WAIT_MS)
    {
        _notReadySince = 0;
        _ignoreSince = now;
        return false;
    }

    return true;
}

void TaxiStallWatch::ForEachWatched(Player* bot, std::function<void(Player*, NewRpgInfo&)> const& fn)
{
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        fn(bot, botAI->rpgInfo);

    Group* group = bot->GetGroup();
    if (!group || group->GetLeaderGUID() != bot->GetGUID())
        return;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;

        if (PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member))
            fn(member, memberAI->rpgInfo);
    }
}

bool TaxiStallWatch::IsStalled(Player* player, NewRpgInfo& info)
{
    if (!player->IsInFlight() || player->IsBeingTeleported())
    {
        info.taxiStillSince = 0;
        if (!player->IsInFlight())
            info.taxiRecoveries = 0;

        return false;
    }

    float const moved = player->GetExactDist(info.taxiLastX, info.taxiLastY, info.taxiLastZ);
    if (!info.taxiStillSince || moved > STILL_DISTANCE)
    {
        info.taxiLastX = player->GetPositionX();
        info.taxiLastY = player->GetPositionY();
        info.taxiLastZ = player->GetPositionZ();
        info.taxiStillSince = std::max(getMSTime(), 1u);
        return false;
    }

    return GetMSTimeDiffToNow(info.taxiStillSince) >= STALL_MS;
}

bool NewRpgTaxiStalledTrigger::IsActive()
{
    bool stalled = false;
    TaxiStallWatch::ForEachWatched(bot, [&stalled](Player* player, NewRpgInfo& info)
    {
        if (TaxiStallWatch::IsStalled(player, info))
            stalled = true;
    });

    return stalled;
}

bool QuestGameObjectPendingTrigger::IsActive()
{
    return !botAI->rpgInfo.pendingQuestObject.IsEmpty() && !bot->IsInCombat();
}

bool NewRpgLeaderAfkTrigger::IsActive()
{
    // The self-bot leader is the user's character played by the AI while the user only watches, so the client's
    // automatic AFK flag is its normal state and says nothing about the session ending. Off until there is a
    // real "session is ending" signal.
    if (constexpr bool restAtInnOnAfk = false; !restAtInnOnAfk)
        return false;

    if (!IsSelfBot(bot) || !bot->isAFK() || bot->IsInCombat() || bot->IsInFlight() || bot->GetTransport() ||
        bot->GetMap()->Instanceable())
    {
        _afkSince = 0;
        return false;
    }

    if (!_afkSince)
        _afkSince = std::max(getMSTime(), 1u);

    return GetMSTimeDiffToNow(_afkSince) >= AFK_BEFORE_INN_MS;
}

bool NewRpgUnknownFlightMasterTrigger::IsActive()
{
    if (bot->IsInCombat() || bot->IsInFlight() || !bot->IsAlive())
        return false;

    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest npcs"))
    {
        Creature* creature = botAI->GetCreature(guid);
        if (!creature || !creature->IsAlive() || !creature->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER) ||
            bot->GetDistance(creature) > DISCOVER_FLIGHT_MASTER_DISTANCE || creature->IsHostileTo(bot))
            continue;

        uint32 const node = sObjectMgr->GetNearestTaxiNode(creature->GetPositionX(), creature->GetPositionY(),
                                                           creature->GetPositionZ(), creature->GetMapId(),
                                                           bot->GetTeamId());
        if (node && !bot->m_taxi.IsTaximaskNodeKnown(node))
            return true;
    }

    return false;
}
