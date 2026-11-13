/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestStats.h"  // [log28]
#include "NewRpgTriggers.h"
#include "NewRpgCoop.h"
#include "GuildHonest.h"  // [wait19]
#include "GuildCoordinator.h"  // [wait19] the guild fives
#include "Creature.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PlayerbotsUserPack.h"
#include "Timer.h"
#include "Transport.h"
#include "DBCStores.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "StringFormat.h"
#include "WaypointMovementGenerator.h"
#include "World.h"

bool NewRpgStatusTrigger::IsActive() { return status == botAI->rpgInfo.GetStatus(); }

std::string NewRpgGroupNotReadyTrigger::MemberNotReady(Player* member)
{
    if (!member->IsAlive())
        return "dead";

    if (member->GetMapId() != bot->GetMapId())
        return "";

    // [assist20] a fight is a mob on the member or the member on a mob: a combat flag with neither (a mob that cannot
    // reach it keeps it on its threat list) holds no wait. b470: Kianey waited 350 s on "Geron in a fight" by
    // Stonewatch Tower, 200 yd short of its camp, both standing until a Blackrock Scout killed Geron
    if (member->IsInCombat() && (!member->getAttackers().empty() || member->GetVictim()))
    {
        // [assist21] a party's member under attack in the leader's sight is no wait: the leader defends it (its grind
        // target is the member's attacker, GrindTargetValue) - the wait outranked every pull while Geron fought alone
        if (!member->getAttackers().empty() && NewRpgCoop::JudgesPulls(bot) &&
            bot->GetExactDist(member) <= sPlayerbotAIConfig.SightDistance)
            return "";
        return "in a fight";
    }

    // eating / drinking: its rest runs to full (a game mechanic)
    if (!member->IsStandState())
        return "resting";

    // [deaths21] Resurrection Sickness: a quarter of its stats until the aura ends (a game mechanic) - the party waits
    // for it as for a rest (the member holds where it rose, NewRpgStatusUpdateAction)
    if (member->HasAura(15007))
        return "under Resurrection Sickness";


    // the core's group reward distance: no kill credit and no XP share beyond it
    if (!member->IsAtGroupRewardDistance(bot))
    {
        float const speed = member->GetSpeed(MOVE_RUN);
        float const distance = bot->GetExactDist(member);
        return Acore::StringFormat("{:.0f} yd behind (group reward distance {:.0f} yd, eta {:.0f} s at its run speed)",
                                   distance, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE),
                                   speed > 0.0f ? distance / speed : 0.0f);
    }

    return "";
}

bool NewRpgGroupNotReadyTrigger::IsActive()
{
    // [stuck8] the leader's wait is a hold with a reason: the global leader's STUCK check reads it (b343 Lanbuzz,
    // Emdyl, Alnia: "reason=none" while each waited for a member thousands of yards behind, GROUP_WAIT logged once)
    botAI->rpgInfo.groupWaiting = false;
    botAI->rpgInfo.groupWaitBehind.Clear();  // [wait14]
    // [log28] a wait the trigger stops looking at (a fight, a flight, a transport, the group gone) ends with a line:
    // GROUP_READY logs only the end the members' facts give
    auto const stops = [this](char const* why)
    {
        if (!_waitReason.empty())
        {
            LOG_INFO("playerbots", "[rpg] {} L{} GROUP_WAIT ends: {} t={}", bot->GetName(), bot->GetLevel(), why,
                     QuestStats::NowMs());
            _waitReason.clear();
        }
        return false;
    };
    // The user's pack (Playerbots.UserPack) leads its group without him: it waits for its members like a SelfBot.
    if (bot->IsInCombat() || bot->IsInFlight())
        return stops(bot->IsInCombat() ? "in a fight" : "on a flight");
    bool const ownGroup = IsSelfBot(bot) || UserPack::IsAutonomousLeader(bot);
    // [regress10] a co-op party's leader waits for its partner as well (NewRpgCoop's plan counts on this hold): without
    // it the leader walked to the camp it cannot solo, refused it alone and left the party (b347 Kylanor q315: 13
    // parties with Namwe, Tuihgrelfim, Ganna broken within 4-15 s, "its own plan moved on: IDLE")
    if (!ownGroup && !bot->GetGroup())
        return stops("no group");
    if (!ownGroup && !NewRpgCoop::JudgesPulls(bot))
        return stops("no longer the party's leader");

    // Boarding, riding and leaving a boat run on its schedule; the leader handles the group itself there.
    NewRpgInfo::TripPhase const phase = botAI->rpgInfo.trip.phase;
    if (bot->GetTransport() || phase == NewRpgInfo::TripPhase::Board || phase == NewRpgInfo::TripPhase::Ride ||
        phase == NewRpgInfo::TripPhase::Leave)
        return stops("on a transport");

    Group* group = bot->GetGroup();
    if (!group || (ownGroup && group->GetLeaderGUID() != bot->GetGUID()))
        return stops("no group it leads");

    // [timers] the members' facts alone hold the group: no wait cap, no ignore window
    std::string reason;
    std::string key;  // the member and the kind of its reason (a distance changes every check)
    std::string ownPlan;  // [wait13]
    for (GroupReference* ref = group->GetFirstMember(); ref && reason.empty(); ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot)
            continue;
        // [wait13] the leader waits only for a member that comes to it: a bot member that plays its own New RPG plan
        // (no co-op / help party that has it follow the leader) walks its own route and never comes - a wait for it
        // has no end. b353: Ravelandy (Rollothan), Orton (Febineysh), Lanbuzz (Geefeelli) and Emdyl (Thamesh, Idunk)
        // stood in GROUP_WAIT for 1129-1239 s, the whole run, while those members quested alone
        if (PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
            memberAI && !memberAI->HasGameClientMaster() && memberAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT) &&
            !NewRpgCoop::InParty(member->GetGUID().GetCounter()))
        {
            ownPlan += (ownPlan.empty() ? "" : ", ") + member->GetName();
            // [group57] a group whose bot member plays its own plan is no group of the global leader's: no co-op /
            // help party holds the two, no guild five - the member leaves it. Its combat still acted for the group
            // (a tank's peel and incoming opener for the other, assists) in fights neither member's solo judge had
            // priced: b397 Laaral L17 opened on Matis the Cruel L18 coming at Dekorhan ([lead96], [peel diag]),
            // both died to him later; Emdyl L14 / Idunk L13 grouped across two continents all run. group43 split
            // only a group whose member follows, and these members never run the follow
            if (GuildHonest::IsHonestBot(bot) && GuildHonest::IsHonestBot(member) && !bot->GetMap()->Instanceable() &&
                !NewRpgCoop::InParty(bot->GetGUID().GetCounter()) &&
                !sGuildCoordinator.InSameActiveFive(bot->GetGUID().GetCounter(), member->GetGUID().GetCounter()))
            {
                static std::mutex splitLock;
                // member -> the group its leave is queued for (one queued leave per group)
                static std::unordered_map<uint32, ObjectGuid> splitQueued;
                bool fresh = false;
                {
                    std::lock_guard<std::mutex> guard(splitLock);
                    ObjectGuid& queued = splitQueued[member->GetGUID().GetCounter()];
                    fresh = queued != group->GetGUID();
                    queued = group->GetGUID();
                }
                if (fresh)
                    NewRpgCoop::LeaveStaleGroup(
                        member, Acore::StringFormat("[group57] it plays its own plan, as its leader {} does ({:.0f} "
                                                    "yd, {}): no co-op / help party or guild five holds the two",
                                                    bot->GetName(), bot->GetExactDist2d(member),
                                                    member->GetMapId() != bot->GetMapId() ? "another map"
                                                    : member->GetZoneId() == bot->GetZoneId() ? "same zone"
                                                                                              : "another zone"));
            }
            continue;
        }
        // [wait19] a guild member no party of this leader holds (a group an earlier session left): no wait - it
        // leaves the group ([group18] / [group19]); b363's leaders waited and walked thousands of yards for such
        if (ObjectGuid partyLeader; GuildHonest::IsHonestBot(bot) && GuildHonest::IsHonestBot(member) &&
                                    !bot->GetMap()->Instanceable() &&
                                    !(NewRpgCoop::PartyLeaderOf(member->GetGUID().GetCounter(), partyLeader) &&
                                      partyLeader == bot->GetGUID()) &&
                                    !sGuildCoordinator.InSameActiveFive(bot->GetGUID().GetCounter(),
                                                                        member->GetGUID().GetCounter()))
            continue;  // (a guild five gathering or heading in keeps its wait)
        std::string const why = MemberNotReady(member);
        if (!why.empty())
        {
            reason = member->GetName() + " " + why;
            key = member->GetName() + (why.find(" yd behind") != std::string::npos ? std::string(" behind") : why);
            if (why.find(" yd behind") != std::string::npos)
                botAI->rpgInfo.groupWaitBehind = member->GetGUID();  // [wait14]
        }
    }

    if (ownPlan != _ownPlanLogged)
    {
        if (!ownPlan.empty())
            LOG_INFO("playerbots", "[wait13] {} L{} does not wait for {}: each plays its own plan (not coming to it)",
                     bot->GetName(), bot->GetLevel(), ownPlan);
        _ownPlanLogged = ownPlan;
    }

    if (key != _waitReason)
    {
        if (reason.empty())
            LOG_INFO("playerbots", "[rpg] {} L{} GROUP_READY reason: every member is with the group", bot->GetName(),
                     bot->GetLevel());
        else
            LOG_INFO("playerbots", "[rpg] {} L{} GROUP_WAIT reason: {} t={}", bot->GetName(), bot->GetLevel(), reason,
                     QuestStats::NowMs());
        _waitReason = key;
    }

    botAI->rpgInfo.groupWaiting = !reason.empty();
    return !reason.empty();
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
    info.taxiStalled = false;
    if (!player->IsInFlight() || player->IsBeingTeleported())
    {
        info.taxiStillSince = 0;
        if (!player->IsInFlight())
            info.taxiRecoveries = 0;

        return false;
    }

    // [timers] the stall is proven by the flight's own facts, no stall timer
    FlightPathMovementGenerator* flight = nullptr;
    if (MotionMaster* mm = player->GetMotionMaster(); mm && mm->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE)
        flight = dynamic_cast<FlightPathMovementGenerator*>(mm->top());
    std::string why;
    if (player->movespline->Finalized())
    {
        // the spline ended short of the route's end; a next node on another map is the cross-map leg
        // (NewRpgTravelFlightAction::ContinueCrossMapTaxi), no stall
        uint32 const next = player->m_taxi.GetTaxiDestination();
        TaxiNodesEntry const* node = next ? sTaxiNodesStore.LookupEntry(next) : nullptr;
        if (flight && !flight->HasArrived() && !(node && node->map_id != player->GetMapId()))
            why = "the flight's spline ended short of the route's end";
    }
    else
    {
        float const moved = player->GetExactDist(info.taxiLastX, info.taxiLastY, info.taxiLastZ);
        uint32 const now = std::max(getMSTime(), 1u);
        if (!info.taxiStillSince || moved > STILL_DISTANCE)
        {
            info.taxiLastX = player->GetPositionX();
            info.taxiLastY = player->GetPositionY();
            info.taxiLastZ = player->GetPositionZ();
            info.taxiStillSince = now;
            return false;
        }
        // the time the spline needs at its own velocity to fly STILL_DISTANCE, on the world's clock
        float const velocity = player->movespline->Velocity();
        if (velocity <= 0.0f)
            return false;
        uint32 const needMs = uint32(STILL_DISTANCE / velocity * IN_MILLISECONDS) + 1;
        uint32 const sinceMs = getMSTimeDiff(info.taxiStillSince, now);
        if (sinceMs < needMs)
            return false;
        why = Acore::StringFormat("moved {:.1f} yd in {} ms, its spline flies {:.1f} yd/s", moved, sinceMs, velocity);
    }
    if (why.empty())
        return false;

    LOG_INFO("playerbots", "[rpg] {} TAXI_STALL reason: {}", player->GetName(), why);
    info.taxiStalled = true;
    return true;
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

    // [timers] the AFK flag itself is the fact, no AFK duration
    return IsSelfBot(bot) && bot->isAFK() && !bot->IsInCombat() && !bot->IsInFlight() && !bot->GetTransport() &&
           !bot->GetMap()->Instanceable();
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
