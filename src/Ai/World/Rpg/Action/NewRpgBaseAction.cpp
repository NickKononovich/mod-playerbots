/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBaseAction.h"
#include "BotPath.h"  // [lead109]
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "GameObjectModel.h"
#include "DatabaseEnv.h"
#include "Formulas.h"
#include "Group.h"
#include "LFGMgr.h"
#include "GatherNodeMgr.h"
#include "GossipDef.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "QuestLevelRules.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "OutdoorPvPMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Position.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"
#include "Transport.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "WorldPacket.h"
#include "G3D/Ray.h"
#include "G3D/Vector2.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr float DECK_STEP = 1.5f;              // yards between deck probes on the way from the pier
    constexpr float DECK_RAY_ABOVE = 8.0f;          // probes start this far over the pier...
    constexpr float DECK_RAY_LENGTH = 20.0f;        // ...and look this far down for the deck
    constexpr float DECK_MAX_STEP = 6.0f;           // a deck higher or lower than this is not reachable from the pier
    constexpr uint32 DECK_SAMPLES_INSIDE = 3;       // stop a few probes past the rail
    constexpr uint32 UNPLANNED_RIDE_MS = MINUTE * IN_MILLISECONDS;  // a ride nobody planned ends at the next dock
    constexpr uint32 GROUP_BOARD_GRACE_MS = 12 * IN_MILLISECONDS;   // then the leader boards without stragglers
    constexpr uint32 PORTAL_GATHER_MS = MINUTE * IN_MILLISECONDS;
    constexpr float ENVOY_REACHED = 4.0f;
    constexpr float CRYSTAL_REACHED = 3.0f;
    constexpr float ENVOY_SEARCH_RANGE = 15.0f;

    float Distance2d(WorldPosition const& a, WorldPosition const& b)
    {
        return std::hypot(a.GetPositionX() - b.GetPositionX(), a.GetPositionY() - b.GetPositionY());
    }
}

bool NewRpgBaseAction::MoveFarTo(WorldPosition dest)
{
    if (dest == WorldPosition())
        return false;

    // Don't start a ground move while the bot is on a taxi or in a load screen: the MovePoint issued below would
    // fight the active flight spline. Returning true consumes the tick instead of returning false, because the
    // callers of MoveFarTo fall through to MoveRandomNear, which is also a ground move.
    if (bot->IsInFlight() || bot->IsBeingTeleported())
        return true;

    // Aboard a boat or zeppelin: stay on until it docks where the trip gets off.
    if (Transport* transport = bot->GetTransport())
        if (MotionTransport* motionTransport = transport->ToMotionTransport())
            return RideTransport(motionTransport);

    // A mage in the group opens a portal to Dalaran from anywhere.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot) && TravelRouteMgr::IsInDalaran(dest) &&
        !TravelRouteMgr::IsInDalaran(WorldPosition(bot)) && UseDalaranPortal())
        return true;

    if (TravelTrip(dest))
        return true;

    if (dest.GetMapId() != bot->GetMapId())
        return false;

    return WalkTo(dest);
}

bool NewRpgBaseAction::WalkTo(WorldPosition const& dest)
{
    // Random bots keep their old way of travel (walk, teleport when stuck); routing is for player-owned bots.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot) &&
        TravelRouteMgr::IsInDalaran(dest) != TravelRouteMgr::IsInDalaran(WorldPosition(bot)))
        return DalaranStep(dest);

    return MoveFarToDirect(dest);
}

bool NewRpgBaseAction::HoldPosition(uint32 duration)
{
    if (bot->isMoving())
        bot->StopMoving();

    return ForceToWait(duration);
}

// Straight line, no path finding: decks and gangways are not on the navmesh. On a transport the spline is
// computed in its local frame, so walking off a docked boat works the same way.
void NewRpgBaseAction::MovePointDirect(float x, float y, float z)
{
    MotionMaster* mm = bot->GetMotionMaster();
    if (!mm)
        return;

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    mm->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, false);
    float delay = IN_MILLISECONDS * MoveDelay(bot->GetExactDist(x, y, z));
    delay = std::clamp(delay, 0.0f, static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove));
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), x, y, z, bot->GetOrientation(), delay, MovementPriority::MOVEMENT_NORMAL);
}

// No living member of the group is left behind on this map.
bool NewRpgBaseAction::IsGroupGathered(float distance)
{
    Group* group = bot->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
            member->GetMapId() != bot->GetMapId())
            continue;

        if (bot->GetDistance(member) > distance)
            return false;
    }

    return true;
}

MotionTransport* NewRpgBaseAction::FindTransport(uint32 entry)
{
    for (Transport* transport : bot->GetMap()->GetAllTransports())
    {
        if (!transport || transport->GetEntry() != entry || !transport->IsInWorld())
            continue;

        if (MotionTransport* motionTransport = transport->ToMotionTransport())
        {
            sTravelRouteMgr.Observe(motionTransport);
            return motionTransport;
        }
    }

    return nullptr;
}

// Plans the boat, zeppelin or portal leg of a far move and runs it up to boarding. False hands the move back to
// plain walking (no leg needed, or the leg was given up).
bool NewRpgBaseAction::TravelTrip(WorldPosition const& dest)
{
    using Phase = NewRpgInfo::TripPhase;
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgInfo::Trip& trip = info.trip;
    if (sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    // Off the transport after the ride: the leg is done, the rest is planned from the pier next tick.
    if (trip.phase == Phase::Ride || trip.phase == Phase::Leave)
    {
        info.ClearTrip();
        return HoldPosition(IN_MILLISECONDS);
    }

    // A new destination drops a leg that has not boarded yet.
    if (trip.phase != Phase::None && trip.dest != dest)
        info.ClearTrip();

    if (trip.phase == Phase::None)
    {
        bool const otherMap = dest.GetMapId() != bot->GetMapId();
        if (!otherMap && bot->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY()) < TRIP_PLAN_DISTANCE)
            return false;

        if (trip.dest == dest && trip.plannedAt && GetMSTimeDiffToNow(trip.plannedAt) < TRIP_REPLAN_MS)
            return false;

        trip.dest = dest;
        trip.plannedAt = getMSTime();
        TravelRouteMgr::Leg leg;
        float seconds = 0.0f;
        if (!sTravelRouteMgr.FindRoute(bot, dest, leg, seconds) || leg.kind == TravelRouteMgr::LegKind::Walk)
            return false;

        trip.leg = leg;
        info.SetTripPhase(Phase::ToStart);
        LOG_DEBUG("playerbots", "[New RPG] {} travels by {} {} to map {} ({:.0f}s)", bot->GetName(),
                  leg.kind == TravelRouteMgr::LegKind::Portal ? "portal" : "transport",
                  leg.kind == TravelRouteMgr::LegKind::Portal ? leg.portalIndex : leg.lineEntry, dest.GetMapId(),
                  seconds);
    }

    if (trip.leg.kind == TravelRouteMgr::LegKind::Portal)
        return TravelByPortal(trip);

    return TravelByTransport(trip);
}

bool NewRpgBaseAction::TravelByTransport(NewRpgInfo::Trip& trip)
{
    using Phase = NewRpgInfo::TripPhase;
    NewRpgInfo& info = botAI->rpgInfo;
    TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(trip.leg.lineEntry);
    if (!line || trip.leg.fromDock >= line->docks.size() || trip.leg.toDock >= line->docks.size())
    {
        info.ClearTrip();
        return false;
    }

    TravelRouteMgr::Dock const& dock = line->docks[trip.leg.fromDock];
    if (bot->GetMapId() != dock.mapId)
    {
        info.ClearTrip();
        return false;
    }

    MotionTransport* transport = FindTransport(line->entry);
    uint32 remaining = 0;
    bool const docked = TravelRouteMgr::IsDocked(transport, dock, remaining);

    switch (trip.phase)
    {
        case Phase::ToStart:
            if (bot->GetExactDist(dock.pier) > PIER_REACHED)
                return WalkTo(dock.pier);

            info.SetTripPhase(Phase::Wait);
            [[fallthrough]];
        case Phase::Wait:
            if (docked && remaining >= BOARD_MIN_REMAINING_MS)
            {
                info.SetTripPhase(Phase::Board);
                return HoldPosition(IN_MILLISECONDS);
            }

            // A full loop without the transport turning up: something is wrong with it, walk on for a while.
            if (GetMSTimeDiffToNow(trip.phaseStart) > line->periodMs + WAIT_SPARE_MS)
            {
                WorldPosition const dest = trip.dest;
                info.ClearTrip();
                info.trip.dest = dest;
                info.trip.plannedAt = getMSTime();
                return false;
            }

            return HoldPosition(3 * IN_MILLISECONDS);
        case Phase::Board:
            if (!docked || GetMSTimeDiffToNow(trip.phaseStart) > BOARD_TIMEOUT_MS)
            {
                info.SetTripPhase(Phase::Wait);
                return HoldPosition();
            }

            // The followers board after the leader; give stragglers a moment to reach the pier.
            if (!IsGroupGathered(GROUP_GATHER_DISTANCE) && GetMSTimeDiffToNow(trip.phaseStart) < GROUP_BOARD_GRACE_MS)
                return HoldPosition();

            return BoardTransport(transport);
        default:
            info.ClearTrip();
            return false;
    }
}

bool NewRpgBaseAction::BoardTransport(MotionTransport* transport)
{
    Map* map = bot->GetMap();
    if (map->GetTransportForPos(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                bot) == transport)
    {
        transport->AddPassenger(bot, true);
        bot->StopMovingOnCurrentPos();
        botAI->rpgInfo.SetTripPhase(NewRpgInfo::TripPhase::Ride);
        LOG_DEBUG("playerbots", "[New RPG] {} boarded transport {}", bot->GetName(), transport->GetEntry());
        return true;
    }

    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return true;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!FindDeckPoint(transport, x, y, z))
        return HoldPosition();

    MovePointDirect(x, y, z);
    return true;
}

// Probes from the pier towards the middle of the transport for a deck at about pier height, using the
// transport's own collision model.
bool NewRpgBaseAction::FindDeckPoint(MotionTransport* transport, float& x, float& y, float& z)
{
    if (!transport->m_model)
        return false;

    float const startX = bot->GetPositionX();
    float const startY = bot->GetPositionY();
    float const startZ = bot->GetPositionZ();
    float const dx = transport->GetPositionX() - startX;
    float const dy = transport->GetPositionY() - startY;
    float const length = std::sqrt(dx * dx + dy * dy);
    if (length < DECK_STEP)
        return false;

    uint32 onDeck = 0;
    for (float travelled = DECK_STEP; travelled <= length; travelled += DECK_STEP)
    {
        float const px = startX + dx / length * travelled;
        float const py = startY + dy / length * travelled;
        G3D::Ray const ray(G3D::Vector3(px, py, startZ + DECK_RAY_ABOVE), G3D::Vector3(0.0f, 0.0f, -1.0f));
        float distance = DECK_RAY_LENGTH;
        if (!transport->m_model->intersectRay(ray, distance, false, bot->GetPhaseMask(),
                                              VMAP::ModelIgnoreFlags::Nothing))
        {
            if (onDeck)
                break;

            continue;
        }

        float const deckZ = startZ + DECK_RAY_ABOVE - distance;
        if (std::fabs(deckZ - startZ) > DECK_MAX_STEP)
            continue;

        x = px;
        y = py;
        z = deckZ;
        if (++onDeck >= DECK_SAMPLES_INSIDE)
            return true;
    }

    return onDeck > 0;
}

bool NewRpgBaseAction::RideTransport(MotionTransport* transport)
{
    using Phase = NewRpgInfo::TripPhase;
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgInfo::Trip& trip = info.trip;
    sTravelRouteMgr.Observe(transport);

    TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(transport->GetEntry());
    if (!line)
        return HoldPosition();

    bool const plannedLeg = trip.leg.kind == TravelRouteMgr::LegKind::Transport &&
                            trip.leg.lineEntry == line->entry && trip.leg.toDock < line->docks.size() &&
                            trip.phase != Phase::None;
    if (trip.phase != Phase::Ride && trip.phase != Phase::Leave)
    {
        if (!plannedLeg)
            trip.leg = TravelRouteMgr::Leg();

        info.SetTripPhase(Phase::Ride);
    }

    TravelRouteMgr::Dock const* exitDock = nullptr;
    uint32 remaining = 0;
    for (uint8 d = 0; d < line->docks.size() && !exitDock; ++d)
    {
        if (plannedLeg ? d != trip.leg.toDock : GetMSTimeDiffToNow(trip.phaseStart) < UNPLANNED_RIDE_MS)
            continue;

        if (TravelRouteMgr::IsDocked(transport, line->docks[d], remaining))
            exitDock = &line->docks[d];
    }

    if (!exitDock)
    {
        if (trip.phase == Phase::Leave)
            info.SetTripPhase(Phase::Ride);

        return HoldPosition();
    }

    if (trip.phase != Phase::Leave)
    {
        info.SetTripPhase(Phase::Leave);
        LOG_DEBUG("playerbots", "[New RPG] {} leaves transport {} at map {}", bot->GetName(), line->entry,
                  exitDock->mapId);
    }

    if (!IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        MovePointDirect(exitDock->pier.GetPositionX(), exitDock->pier.GetPositionY(), exitDock->pier.GetPositionZ());

    return true;
}

bool NewRpgBaseAction::TravelByPortal(NewRpgInfo::Trip& trip)
{
    using Phase = NewRpgInfo::TripPhase;
    NewRpgInfo& info = botAI->rpgInfo;
    std::vector<TravelRouteMgr::Portal> const& portals = sTravelRouteMgr.GetPortals();
    if (trip.leg.portalIndex >= portals.size() || bot->GetMapId() != portals[trip.leg.portalIndex].from.GetMapId())
    {
        info.ClearTrip();
        return false;
    }

    TravelRouteMgr::Portal const& portal = portals[trip.leg.portalIndex];
    if (trip.phase == Phase::ToStart)
    {
        if (bot->GetExactDist(portal.from) > INTERACTION_DISTANCE - 1.0f)
            return WalkTo(portal.from);

        info.SetTripPhase(Phase::Wait);
    }

    // Everyone steps through together: a member left on another continent has no way to catch up.
    if (!IsGroupGathered(GROUP_USE_DISTANCE) && GetMSTimeDiffToNow(trip.phaseStart) < PORTAL_GATHER_MS)
        return HoldPosition();

    GameObject* go = bot->FindNearestGameObject(portal.goEntry, 2.0f * INTERACTION_DISTANCE);
    WorldPosition const dest = trip.dest;
    info.ClearTrip();
    if (!go || !go->isSpawned())
    {
        info.trip.dest = dest;
        info.trip.plannedAt = getMSTime();
        return false;
    }

    UseGameObjectWithGroup(go);
    return HoldPosition(5 * IN_MILLISECONDS);
}

// Group bots next to the object use it first (it may close behind the first user), the bot last.
bool NewRpgBaseAction::UseGameObjectWithGroup(GameObject* go)
{
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() || member->IsInCombat() ||
                member->GetMapId() != bot->GetMapId() || member->IsBeingTeleported() || !GET_PLAYERBOT_AI(member) ||
                member->GetDistance(go) > GROUP_USE_DISTANCE)
                continue;

            go->Use(member);
        }
    }

    if (bot->isMoving())
        bot->StopMoving();

    WorldPacket use(CMSG_GAMEOBJ_USE, 8);
    use << go->GetGUID();
    bot->GetSession()->HandleGameObjectUseOpcode(use);
    return true;
}

bool NewRpgBaseAction::HasDalaranTeleportQuest(Player* player)
{
    using namespace DalaranTravel;
    return player->GetQuestStatus(QUEST_HORDE) == QUEST_STATUS_COMPLETE ||
           player->GetQuestStatus(QUEST_ALLIANCE) == QUEST_STATUS_COMPLETE ||
           player->GetQuestStatus(QUEST_DEATH_KNIGHT) == QUEST_STATUS_COMPLETE;
}

// Up to or down from Dalaran by its own ways: the Violet Stand crystals, a flight to Krasus' Landing, the
// Kirin Tor envoy teleport of the level 74 quest, or a group mage's portal (see MoveFarTo). Nothing walks or
// flies in over the city.
bool NewRpgBaseAction::DalaranStep(WorldPosition const& dest)
{
    using namespace DalaranTravel;
    WorldPosition const botPos(bot);

    if (TravelRouteMgr::IsInDalaran(botPos))
    {
        if (bot->GetQuestRewardStatus(QUEST_LEAVE_AND_RETURN) ||
            bot->GetQuestStatus(QUEST_LEAVE_AND_RETURN) == QUEST_STATUS_INCOMPLETE)
            return UseDalaranCrystal(GO_CRYSTAL_DOWN);

        // fly down to the discovered flight path nearest to the destination
        uint32 toNode = 0;
        float nearest = std::numeric_limits<float>::max();
        for (uint32 i = 0; i < sTaxiNodesStore.GetNumRows(); ++i)
        {
            TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
            if (!node || node->ID == TAXI_NODE_KRASUS_LANDING || node->map_id != dest.GetMapId() ||
                !bot->m_taxi.IsTaximaskNodeKnown(node->ID))
                continue;

            float const distance = std::hypot(node->x - dest.GetPositionX(), node->y - dest.GetPositionY());
            if (distance < nearest)
            {
                nearest = distance;
                toNode = node->ID;
            }
        }

        return FlyTowards(toNode);
    }

    enum class Way : uint8
    {
        None,
        Taxi,
        Crystal,
        Envoy
    };
    Way way = Way::None;
    float best = std::numeric_limits<float>::max();
    WorldPosition const city(MAP_ID, CENTER_X, CENTER_Y, MIN_Z);

    if (bot->m_taxi.IsTaximaskNodeKnown(TAXI_NODE_KRASUS_LANDING))
    {
        if (TravelMgr::FlightMasterInfo const* master = sTravelMgr.GetNearestFlightMasterInfo(bot))
        {
            float const seconds = Distance2d(botPos, master->pos) / TravelRouteMgr::WALK_SPEED +
                                  Distance2d(master->pos, city) / FLIGHT_SPEED;
            if (seconds < best)
            {
                best = seconds;
                way = Way::Taxi;
            }
        }
    }

    if (bot->GetQuestRewardStatus(QUEST_CRYSTAL_FLAG))
    {
        for (WorldPosition const& crystal : sTravelRouteMgr.GetObjectSpawns(GO_CRYSTAL_UP))
        {
            float const seconds = Distance2d(botPos, crystal) / TravelRouteMgr::WALK_SPEED;
            if (crystal.GetMapId() == bot->GetMapId() && seconds < best)
            {
                best = seconds;
                way = Way::Crystal;
            }
        }
    }

    if (HasDalaranTeleportQuest(bot))
    {
        for (uint32 envoy : QUEST_GIVERS)
        {
            for (WorldPosition const& spawn : sTravelRouteMgr.GetSpawns(envoy))
            {
                float const seconds = Distance2d(botPos, spawn) / TravelRouteMgr::WALK_SPEED;
                if (spawn.GetMapId() == bot->GetMapId() && seconds < best)
                {
                    best = seconds;
                    way = Way::Envoy;
                }
            }
        }
    }

    switch (way)
    {
        case Way::Taxi:
            return FlyTowards(TAXI_NODE_KRASUS_LANDING);
        case Way::Crystal:
            return UseDalaranCrystal(GO_CRYSTAL_UP);
        case Way::Envoy:
            return TeleportByDalaranEnvoy();
        default:
            return false;
    }
}

// Hands over to the flight status: walk to the nearest flight master, fly a route over discovered nodes, pay.
bool NewRpgBaseAction::FlyTowards(uint32 toNode)
{
    TravelMgr::FlightMasterInfo const* master = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!master || !toNode || !master->taxiNodeId)
        return false;

    std::vector<uint32> path = sTravelNodeMap.FindKnownTaxiPath(bot, master->taxiNodeId, toNode);
    if (path.empty())
        return false;

    botAI->rpgInfo.ChangeToTravelFlight(master->templateEntry, master->pos, path);
    return true;
}

bool NewRpgBaseAction::UseDalaranCrystal(uint32 entry)
{
    WorldPosition crystal;
    float nearest = std::numeric_limits<float>::max();
    for (WorldPosition const& spawn : sTravelRouteMgr.GetObjectSpawns(entry))
    {
        if (spawn.GetMapId() == bot->GetMapId() && bot->GetExactDist(spawn) < nearest)
        {
            nearest = bot->GetExactDist(spawn);
            crystal = spawn;
        }
    }

    if (crystal == WorldPosition())
        return false;

    if (nearest > CRYSTAL_REACHED)
        return MoveFarToDirect(crystal);

    GameObject* go = bot->FindNearestGameObject(entry, 2.0f * INTERACTION_DISTANCE);
    if (!go || !go->isSpawned())
        return false;

    // The crystal closes for a second after each use: send the members over one by one, the bot last.
    if (go->HasGameObjectFlag(GO_FLAG_IN_USE))
        return HoldPosition(IN_MILLISECONDS);

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() || member->IsInCombat() ||
                member->GetMapId() != bot->GetMapId() || member->IsBeingTeleported() || !GET_PLAYERBOT_AI(member) ||
                member->GetDistance(go) > GROUP_USE_DISTANCE)
                continue;

            bool const canUse = entry == DalaranTravel::GO_CRYSTAL_UP
                                    ? member->GetQuestRewardStatus(DalaranTravel::QUEST_CRYSTAL_FLAG)
                                    : member->GetQuestRewardStatus(DalaranTravel::QUEST_LEAVE_AND_RETURN) ||
                                          member->GetQuestStatus(DalaranTravel::QUEST_LEAVE_AND_RETURN) ==
                                              QUEST_STATUS_INCOMPLETE;
            if (!canUse)
                continue;

            go->Use(member);
            return HoldPosition(IN_MILLISECONDS + IN_MILLISECONDS / 2);
        }
    }

    WorldPacket use(CMSG_GAMEOBJ_USE, 8);
    use << go->GetGUID();
    bot->GetSession()->HandleGameObjectUseOpcode(use);
    return HoldPosition(3 * IN_MILLISECONDS);
}

void NewRpgBaseAction::GossipTeleportToDalaran(Player* player, Creature* envoy)
{
    uint32 const menuId = envoy->GetCreatureTemplate()->GossipMenuId;
    WorldPacket hello(CMSG_GOSSIP_HELLO, 8);
    hello << envoy->GetGUID();
    player->GetSession()->HandleGossipHelloOpcode(hello);

    WorldPacket select(CMSG_GOSSIP_SELECT_OPTION, 8 + 4 + 4);
    select << envoy->GetGUID() << menuId << DalaranTravel::GOSSIP_OPTION_TELEPORT;
    player->GetSession()->HandleGossipSelectOptionOpcode(select);
}

// "I am ready to be teleported to Dalaran": the envoy only offers it to holders of the completed quest.
bool NewRpgBaseAction::TeleportByDalaranEnvoy()
{
    using namespace DalaranTravel;
    Creature* envoy = nullptr;
    for (uint32 entry : QUEST_GIVERS)
    {
        Creature* creature = bot->FindNearestCreature(entry, ENVOY_SEARCH_RANGE);
        if (creature && creature->IsAlive() && (!envoy || bot->GetDistance(creature) < bot->GetDistance(envoy)))
            envoy = creature;
    }

    if (!envoy)
    {
        WorldPosition target;
        float nearest = std::numeric_limits<float>::max();
        for (uint32 entry : QUEST_GIVERS)
        {
            for (WorldPosition const& spawn : sTravelRouteMgr.GetSpawns(entry))
            {
                if (spawn.GetMapId() == bot->GetMapId() && bot->GetExactDist(spawn) < nearest)
                {
                    nearest = bot->GetExactDist(spawn);
                    target = spawn;
                }
            }
        }

        if (target == WorldPosition())
            return false;

        return MoveFarToDirect(target);
    }

    if (!bot->IsWithinDistInMap(envoy, ENVOY_REACHED))
    {
        MoveWorldObjectTo(envoy->GetGUID(), 2.0f);
        return true;
    }

    // members next to the envoy who hold the quest too go up with the bot
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsInWorld() && member->IsAlive() && GET_PLAYERBOT_AI(member) &&
                member->GetMapId() == bot->GetMapId() && member->IsWithinDistInMap(envoy, INTERACTION_DISTANCE) &&
                HasDalaranTeleportQuest(member))
                GossipTeleportToDalaran(member, envoy);
        }
    }

    GossipTeleportToDalaran(bot, envoy);
    return HoldPosition(3 * IN_MILLISECONDS);
}

// Portal: Dalaran from a mage in the group (the bot itself or a group bot next to it).
bool NewRpgBaseAction::UseDalaranPortal()
{
    using namespace DalaranTravel;
    NewRpgInfo& info = botAI->rpgInfo;
    if (info.dalaranPortalFailedAt && GetMSTimeDiffToNow(info.dalaranPortalFailedAt) < PORTAL_RETRY_MS)
        return false;

    if (GameObject* portal = bot->FindNearestGameObject(GO_PORTAL_DALARAN, GROUP_GATHER_DISTANCE))
    {
        info.dalaranPortalAskedAt = 0;
        if (!bot->IsWithinDistInMap(portal, INTERACTION_DISTANCE - 1.0f))
        {
            MoveWorldObjectTo(portal->GetGUID(), 1.5f);
            return true;
        }

        return UseGameObjectWithGroup(portal);
    }

    if (info.dalaranPortalAskedAt)
    {
        if (GetMSTimeDiffToNow(info.dalaranPortalAskedAt) < PORTAL_CAST_WAIT_MS)
            return HoldPosition();

        info.dalaranPortalAskedAt = 0;
        info.dalaranPortalFailedAt = getMSTime();
        return false;
    }

    Player* mage = nullptr;
    auto canOpen = [this](Player* player)
    {
        return player && player->IsInWorld() && player->IsAlive() && !player->IsInCombat() &&
               player->getClass() == CLASS_MAGE && player->GetMapId() == bot->GetMapId() &&
               bot->GetDistance(player) < GROUP_GATHER_DISTANCE && player->HasSpell(SPELL_PORTAL_DALARAN) &&
               !player->HasSpellCooldown(SPELL_PORTAL_DALARAN) && player->HasItemCount(ITEM_RUNE_OF_PORTALS, 1) &&
               !player->IsNonMeleeSpellCast(false);
    };

    if (canOpen(bot))
        mage = bot;
    else if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref && !mage; ref = ref->next())
            if (canOpen(ref->GetSource()) && GET_PLAYERBOT_AI(ref->GetSource()))
                mage = ref->GetSource();

    if (!mage)
        return false;

    PlayerbotAI* mageAI = mage == bot ? botAI : GET_PLAYERBOT_AI(mage);
    if (!mageAI || !mageAI->CastSpell(SPELL_PORTAL_DALARAN, mage))
    {
        info.dalaranPortalFailedAt = getMSTime();
        return false;
    }

    LOG_DEBUG("playerbots", "[New RPG] {} asked {} for a portal to Dalaran", bot->GetName(), mage->GetName());
    info.dalaranPortalAskedAt = getMSTime();
    return HoldPosition();
}

bool NewRpgBaseAction::MoveFarToDirect(WorldPosition dest)
{
    if (dest == WorldPosition())
        return false;

    // Don't start a ground move while the bot is on a taxi: the MovePoint issued
    // below would fight the active flight spline. Returning true consumes the tick
    // instead of returning false, because the callers of MoveFarTo fall through to
    // MoveRandomNear, which is also a ground move.
    if (bot->IsInFlight())
        return true;

    bool newDest = dest != botAI->rpgInfo.moveFarPos;
    if (newDest)
    {
        // clear stuck information if it's a new dest
        botAI->rpgInfo.SetMoveFarTo(dest);
    }

    // performance optimization
    if (!newDest && IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    // Let previously committed movement finish before recomputing.
    //
    // MoveTo internally caps its stored delay at maxWaitForMove
    // (default 5s), but a long path (200+ yd routed around a
    // mountain) takes 30+ seconds to walk. After 5s
    // IsWaitingForLastMove returns false and MoveFarTo re-enters.
    // Without this gate, DoMovePoint would call mm->Clear() and
    // reissue MovePoint from the new bot position — and from a new
    // position mmap's partial-path endpoint often differs, so the
    // bot gets clobbered mid-walk and ends up oscillating (e.g.
    // cave entrance -> inside cave -> cave entrance -> mountain
    // base -> cave entrance...) around an unreachable destination.
    //
    // If the bot is still actively walking toward its last
    // committed point on the same map, just let the current spline
    // finish. The stuck counter below continues to track real
    // progress toward dest and triggers teleport recovery if the
    // committed paths genuinely aren't closing the gap.
    //
    // A changed destination (e.g. the gather state switching to a
    // live node it passes, or a quest POI change) skips this gate:
    // letting the old spline finish would keep walking the bot
    // toward a target it no longer has.
    if (!newDest)
    {
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && lastMove.lastMoveToMapId == bot->GetMapId())
        {
            float remaining = bot->GetExactDist(lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    // stuck check
    float disToDest = bot->GetDistance(dest);
    // Require a meaningful improvement (5yd) to reset the stuck counter.
    // The old 1yd threshold was small enough that bots oscillating back
    // and forth around an obstacle would keep "making progress" forever
    // and never trigger the teleport recovery below.
    if (disToDest + 5.0f < botAI->rpgInfo.nearestMoveFarDis)
    {
        botAI->rpgInfo.nearestMoveFarDis = disToDest;
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
    }
    else if (++botAI->rpgInfo.stuckAttempts >= 5 && GetMSTimeDiffToNow(botAI->rpgInfo.stuckTs) >= stuckTime)
    {
        // No meaningful progress toward dest for `stuckTime`: fall
        // back to teleporting directly so the bot can get on with
        // its RPG objective instead of oscillating indefinitely.
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;

        // Player-owned bots play like players: no teleport shortcut. Give the goal up so the planner picks
        // another one (the quest drops to low priority, see the callers' fallbacks).
        if (!sRandomPlayerbotMgr.IsRandomBot(bot) || !sPlayerbotAIConfig.RandomBotTeleport)  // [guild1]
        {
            LOG_DEBUG("playerbots", "[New RPG] {} gives up an unreachable destination ({},{},{},{})", bot->GetName(),
                      dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), dest.GetMapId());
            if (auto* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data))
                botAI->lowPriorityQuest.insert(quest->questId);

            botAI->rpgInfo.ChangeToIdle();
            return false;
        }

        AreaTableEntry const* entry = sAreaTableStore.LookupEntry(bot->GetZoneId());
        std::string zone_name = PlayerbotAI::GetLocalizedAreaName(entry);
        LOG_DEBUG(
            "playerbots",
            "[New RPG] Teleport {} from ({},{},{},{}) to ({},{},{},{}) as it stuck when moving far - Zone: {} ({})",
            bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(),
            dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), dest.GetMapId(), bot->GetZoneId(),
            zone_name);
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        return bot->TeleportTo(dest);
    }

    float dis = bot->GetExactDist(dest);
    if (dis < pathFinderDis)
    {
        return MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false, false,
                      false, true);
    }

    const uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

    // Primary strategy: ask mmap for a route to the TRUE destination.
    // If mmap can reach it directly (PATHFIND_NORMAL) or partially
    // (PATHFIND_INCOMPLETE — destinations beyond the smooth-path cap
    // of ~296 yards, or where local geometry blocks the final step),
    // walk to the furthest reachable waypoint mmap computed. This
    // lets bots follow the real route around obstacles (mountains,
    // cave walls, cliffs) instead of trying to cut straight through.
    // The spline system walks the whole returned path smoothly, so
    // subsequent ticks early-out via IsWaitingForLastMove and no
    // further PathGenerator calls fire until the bot arrives.
    {
        BotPath const path =  // [lead109]
            FindBotPath(bot, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        PathType type = path.type;
        bool canReach = !(type & (~typeOk));
        if (canReach)
        {
            G3D::Vector3 const& endPos = path.actualEnd;
            // Only commit if the mmap endpoint actually makes progress
            // toward the destination. For pathological INCOMPLETE
            // results (e.g. disconnected polys that still report
            // INCOMPLETE) the endpoint can land right under the bot;
            // fall through to cone sampling in that case.
            float endDistToDest = dest.GetExactDist(endPos.x, endPos.y, endPos.z);
            if (endDistToDest + 5.0f < disToDest)
            {
                return MoveTo(bot->GetMapId(), endPos.x, endPos.y, endPos.z, false, false, false, true);
            }
        }
    }

    // Fallback: mmap couldn't route to the destination. Sample the
    // forward cone for a reachable stepping stone so the bot keeps
    // moving and can try again from a new vantage point. Cap at 2
    // samples — we already spent one PathGenerator call above and at
    // 3000 bots every extra CalculatePath matters.
    float minDelta = M_PI;
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    const float baseAngle = bot->GetAngle(&dest);
    float rx, ry, rz;
    bool found = false;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        float delta = (rand_norm() - 0.5f) * static_cast<float>(M_PI);  // ±π/2, forward cone
        float sampleDis = (0.5f + rand_norm() * 0.5f) * pathFinderDis;
        float angle = baseAngle + delta;
        float dx = x + cos(angle) * sampleDis;
        float dy = y + sin(angle) * sampleDis;
        float dz = z + 0.5f;
        BotPath const path = FindBotPath(bot, dx, dy, dz);  // [lead109]
        PathType type = path.type;
        bool canReach = !(type & (~typeOk));

        if (canReach && fabs(delta) <= minDelta)
        {
            found = true;
            G3D::Vector3 const& endPos = path.actualEnd;
            rx = endPos.x;
            ry = endPos.y;
            rz = endPos.z;
            minDelta = fabs(delta);
        }
    }
    if (found)
    {
        return MoveTo(bot->GetMapId(), rx, ry, rz, false, false, false, true);
    }
    return false;
}

bool NewRpgBaseAction::MoveWorldObjectTo(ObjectGuid guid, float distance)
{
    // never walk off a boat or zeppelin at sea
    if (bot->GetTransport() && bot->GetTransport()->IsMotionTransport())
        return false;

    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    WorldObject* object = botAI->GetWorldObject(guid);
    if (!object)
        return false;
    float x = object->GetPositionX();
    float y = object->GetPositionY();
    float z = object->GetPositionZ();
    float mapId = object->GetMapId();
    float angle = 0.f;

    if (!object->ToUnit() || !object->ToUnit()->isMoving())
        angle = object->GetAngle(bot) + (M_PI * irand(-25, 25) / 100.0);  // Closest 45 degrees towards the target
    else
        angle = object->GetOrientation() +
                (M_PI * irand(-25, 25) / 100.0);  // 45 degrees infront of target (leading it's movement)

    float rnd = rand_norm();
    x += cos(angle) * distance * rnd;
    y += sin(angle) * distance * rnd;
    if (!object->GetMap()->CheckCollisionAndGetValidCoords(object, object->GetPositionX(), object->GetPositionY(),
                                                           object->GetPositionZ(), x, y, z))
    {
        x = object->GetPositionX();
        y = object->GetPositionY();
        z = object->GetPositionZ();
    }
    return MoveTo(mapId, x, y, z, false, false, false, true);
}

bool NewRpgBaseAction::MoveRandomNear(float moveStep, MovementPriority priority, WorldObject*)
{
    // never wander off a boat or zeppelin at sea
    if (bot->GetTransport() && bot->GetTransport()->IsMotionTransport())
        return HoldPosition();

    if (IsWaitingForLastMove(priority))
        return false;

    Map* map = bot->GetMap();
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    // Previously: attempts = 1. A single random sample often landed in
    // water / blocked geometry / unreachable poly, the function returned
    // false, and the caller had no fallback — bot stood still. Retry a
    // handful of times with a fresh distance each loop so a bad roll
    // doesn't lock the bot in place.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        float distance = (0.4f + rand_norm() * 0.6f) * moveStep;
        float angle = (float)rand_norm() * 2 * static_cast<float>(M_PI);
        float dx = x + distance * cos(angle);
        float dy = y + distance * sin(angle);
        float dz = z;

        BotPath const path = FindBotPath(bot, dx, dy, dz);  // [lead109]
        PathType type = path.type;
        uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
        bool canReach = !(type & (~typeOk));

        if (!canReach)
            continue;

        if (!map->CanReachPositionAndGetValidCoords(bot, dx, dy, dz))
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), dx, dy, dz, bot->GetCollisionHeight()))
            continue;

        bool moved = MoveTo(bot->GetMapId(), dx, dy, dz, false, false, false, true, priority);
        if (moved)
            return true;
    }

    return false;
}

bool NewRpgBaseAction::ForceToWait(uint32 duration, MovementPriority priority)
{
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(),
             duration, priority);
    return true;
}

/// @TODO: Fix redundant code
/// Quest related method refer to TalkToQuestGiverAction.h
bool NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest(ObjectGuid guid)
{
    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
    if (!object || !bot->CanInteractWithQuestGiver(object))
        return false;

    // Creature* creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
    // if (creature)
    // {
    //     WorldPacket packet(CMSG_GOSSIP_HELLO);
    //     packet << guid;
    //     bot->GetSession()->HandleGossipHelloOpcode(packet);
    // }

    RelayInteractPacketToGroupBots(guid);

    bot->PrepareQuestMenu(guid);
    QuestMenu const& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return true;

    // Accepting and turning in rebuild the giver's menu: walk a copy.
    std::vector<uint32> questIds;
    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
        questIds.push_back(menu.GetItem(idx).QuestId);

    for (uint32 questId : questIds)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            IsQuestWorthDoing(quest) && IsQuestCapableDoing(quest))
        {
            AcceptQuest(quest, guid);
            // Quests that are complete on acceptance and end here (auto-complete "turn-in" quests, #2495) are handed
            // in right away below; the relayed accept comes first so the group bots hold the quest by then.
            status = bot->GetQuestStatus(questId);
            if (status == QUEST_STATUS_COMPLETE && !object->hasInvolvedQuest(questId))
                status = QUEST_STATUS_INCOMPLETE;
            RelayQuestPacketToGroupBots(CMSG_QUESTGIVER_ACCEPT_QUEST, guid, quest->GetQuestId());
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_accepted",
                    "Quest accepted %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestAccepted(botAI, bot, quest);
            botAI->RecordQuestAccepted(quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} accept quest {}", bot->GetName(), quest->GetQuestId());
        }
        if (status == QUEST_STATUS_COMPLETE && object->hasInvolvedQuest(questId) &&
            bot->CanRewardQuest(quest, 0, false))
        {
            RelayQuestPacketToGroupBots(CMSG_QUESTGIVER_COMPLETE_QUEST, guid, quest->GetQuestId());
            TurnInQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_rewarded",
                    "Quest rewarded %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestTurnedIn(botAI, bot, quest);
            botAI->RecordQuestRewarded(quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} turned in quest {}", bot->GetName(), quest->GetQuestId());
        }
    }
    return true;
}

// A selfbot acts server-side, so its client never sends the packets its own bots react to (accept the same
// quest, sync the turn-in, use the same object, follow onto a taxi). Feed them the packet a client would send.
void NewRpgBaseAction::RelayPacketToGroupBots(WorldPacket const& packet)
{
    if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(bot))
        mgr->HandleMasterIncomingPacket(packet);
}

void NewRpgBaseAction::RelayQuestPacketToGroupBots(uint16 opcode, ObjectGuid guid, uint32 questId)
{
    WorldPacket packet(opcode, 8 + 4 + 4);
    packet << guid << questId;
    if (opcode == CMSG_QUESTGIVER_ACCEPT_QUEST)
        packet << uint32(0);

    RelayPacketToGroupBots(packet);
}

void NewRpgBaseAction::RelayInteractPacketToGroupBots(ObjectGuid guid)
{
    WorldPacket packet(guid.IsGameObject() ? CMSG_GAMEOBJ_USE : CMSG_GOSSIP_HELLO, 8);
    packet << guid;
    RelayPacketToGroupBots(packet);
}
bool NewRpgBaseAction::IsWithinInteractionDist(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that only keep the distance check
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            // unit checks
            if (!guid)
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            if (!creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->IsWithinDistInMap(bot))
                {
                    return true;
                }
            }
            return false;
        }
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::AcceptQuest(Quest const* quest, ObjectGuid guid)
{
    WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
    uint32 unk1 = 0;
    p << guid << quest->GetQuestId() << unk1;
    p.rpos(0);
    bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);

    return true;
}

bool NewRpgBaseAction::TurnInQuest(Quest const* quest, ObjectGuid guid)
{
    uint32 questID = quest->GetQuestId();

    if (bot->GetQuestRewardStatus(questID))
    {
        return false;
    }

    if (!bot->CanRewardQuest(quest, false))
    {
        return false;
    }

    bot->PlayDistanceSound(621);

    WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
    p << guid << quest->GetQuestId();
    if (quest->GetRewChoiceItemsCount() <= 1)
    {
        p << 0;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }
    else
    {
        uint32 bestId = BestRewardIndex(quest);
        p << bestId;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }

    return true;
}

uint32 NewRpgBaseAction::BestRewardIndex(Quest const* quest)
{
    ItemIds returnIds;
    ItemUsage bestUsage = ITEM_USAGE_NONE;
    if (quest->GetRewChoiceItemsCount() <= 1)
        return 0;
    else
    {
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                bestUsage = ITEM_USAGE_EQUIP;
            else if (usage == ITEM_USAGE_BAD_EQUIP && bestUsage != ITEM_USAGE_EQUIP)
                bestUsage = usage;
            else if (usage != ITEM_USAGE_NONE && bestUsage == ITEM_USAGE_NONE)
                bestUsage = usage;
        }
        StatsWeightCalculator calc(bot);
        uint32 best = 0;
        float bestScore = 0;
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == bestUsage || usage == ITEM_USAGE_REPLACE)
            {
                uint32 const itemId = quest->RewardChoiceItemId[i];
                ItemTemplate const* rewardProto = sObjectMgr->GetItemTemplate(itemId);
                uint8 rewardSlot = NULL_SLOT;
                if (rewardProto)
                    rewardSlot = botAI->FindEquipSlot(rewardProto, NULL_SLOT, true);
                float score = calc.CalculateItem(itemId, 0, rewardSlot);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
        }
        return best;
    }
}

bool NewRpgBaseAction::IsQuestWorthDoing(Quest const* quest)
{
    bool isLowLevelQuest =
        bot->GetLevel() > (bot->GetQuestLevel(quest) + sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF));

    if (isLowLevelQuest)
        return false;

    if (quest->IsRepeatable())
        return false;

    if (quest->IsSeasonal())
        return false;

    return true;
}

bool NewRpgBaseAction::IsQuestCapableDoing(Quest const* quest)
{
    bool highLevelQuest = bot->GetLevel() + QUEST_MAX_LEVELS_ABOVE_BOT < bot->GetQuestLevel(quest);
    if (highLevelQuest)
        return false;

    // Dungeon, raid, PvP and the other special quest types stay out.
    if (quest->GetType() != 0 && quest->GetType() != QUEST_TYPE_ELITE)
        return false;

    // Elite and group quests are fine for a group of three or more around the bot's level (a five-man party).
    bool const groupQuest = quest->GetType() == QUEST_TYPE_ELITE || quest->GetSuggestedPlayers() >= 2;
    if (groupQuest && (quest->GetSuggestedPlayers() > MAX_GROUP_QUEST_PLAYERS || !HasCapableGroup()))
        return false;

    return true;
}

bool NewRpgBaseAction::HasCapableGroup()
{
    NewRpgInfo& info = botAI->rpgInfo;
    if (info.groupCheckedAt && GetMSTimeDiffToNow(info.groupCheckedAt) < GROUP_CHECK_MS)
        return info.capableGroup;

    info.groupCheckedAt = std::max(getMSTime(), 1u);
    uint32 nearLevel = 0;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsInWorld() && member->GetMapId() == bot->GetMapId() &&
                std::abs(static_cast<int32>(member->GetLevel()) - static_cast<int32>(bot->GetLevel())) <=
                    GROUP_LEVEL_SPREAD)
                ++nearLevel;
        }
    }

    info.capableGroup = nearLevel >= MIN_GROUP_FOR_ELITE;
    return info.capableGroup;
}

// The quests of the level 74 Dalaran chain end up in the city, outside the zone filter of the quest POIs.
bool NewRpgBaseAction::GetDalaranQuestTarget(uint32 questId, WorldPosition& pos)
{
    using namespace DalaranTravel;
    QuestStatus const status = bot->GetQuestStatus(questId);
    uint32 creature = 0;
    uint32 object = 0;
    if (IsTeleportQuest(questId) && status == QUEST_STATUS_COMPLETE)
        creature = NPC_CELINDRA;
    else if (questId == QUEST_LEAVE_AND_RETURN)
    {
        if (status == QUEST_STATUS_INCOMPLETE)
            object = GO_CRYSTAL_DOWN;
        else if (status == QUEST_STATUS_COMPLETE)
            creature = NPC_CELINDRA;
    }

    std::vector<WorldPosition> const& spawns =
        creature ? sTravelRouteMgr.GetSpawns(creature) : sTravelRouteMgr.GetObjectSpawns(object);
    if ((!creature && !object) || spawns.empty())
        return false;

    pos = spawns.front();
    return pos.GetMapId() == bot->GetMapId();
}

// A quest objective that is a game object to use (a goober, the Dalaran crystal): walk up and use it. The core
// credits the whole group at reward distance; relaying the use lets group bots use their own copy too (#2725).
bool NewRpgBaseAction::UseQuestGameObject(uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    auto itr = bot->getQuestStatusMap().find(questId);
    if (!quest || itr == bot->getQuestStatusMap().end() || itr->second.Status != QUEST_STATUS_INCOMPLETE)
        return false;

    GameObject* nearest = nullptr;
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        int32 const required = quest->RequiredNpcOrGo[i];
        if (required >= 0 || itr->second.CreatureOrGOCount[i] >= quest->RequiredNpcOrGoCount[i])
            continue;

        GameObject* go = bot->FindNearestGameObject(static_cast<uint32>(-required), QUEST_OBJECT_RANGE);
        if (go && go->isSpawned() && !go->HasGameObjectFlag(GO_FLAG_IN_USE) &&
            (!nearest || bot->GetDistance(go) < bot->GetDistance(nearest)))
            nearest = go;
    }

    if (!nearest)
        return false;

    // the Dalaran crystal teleports only its user: the group goes down with the bot
    if (nearest->GetEntry() == DalaranTravel::GO_CRYSTAL_DOWN)
        return UseDalaranCrystal(DalaranTravel::GO_CRYSTAL_DOWN);

    if (!nearest->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
    {
        MoveWorldObjectTo(nearest->GetGUID(), 1.5f);
        return true;
    }

    if (bot->isMoving())
        bot->StopMoving();

    LOG_DEBUG("playerbots", "[New RPG] {} uses quest object {} for quest {}", bot->GetName(), nearest->GetEntry(),
              questId);
    WorldPacket use(CMSG_GAMEOBJ_USE, 8);
    use << nearest->GetGUID();
    RelayInteractPacketToGroupBots(nearest->GetGUID());
    bot->GetSession()->HandleGameObjectUseOpcode(use);
    return ForceToWait(3 * IN_MILLISECONDS);
}

bool NewRpgBaseAction::OrganizeQuestLog()
{
    int32 freeSlotNum = 0;

    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            freeSlotNum++;
    }

    // it's ok if we have two more free slots
    if (freeSlotNum >= 2)
        return false;

    // Quests of player-owned characters (selfbots, altbots) are never thrown away to make room.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    int32 dropped = 0;
    // remove quests that not worth doing or not capable of doing
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        bool const worthDoing = IsQuestWorthDoing(quest);
        bool const capableDoing = IsQuestCapableDoing(quest);
        bool const failed = bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED;
        if (!worthDoing || !capableDoing || failed)
        {
            QuestDropReason const reason = !worthDoing    ? QuestDropReason::NotWorth
                                           : !capableDoing ? QuestDropReason::NotCapable
                                                           : QuestDropReason::Failed;
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->RecordQuestDropped(questId, reason);
            dropped++;
        }
    }

    // drop more than 8 quests at once to avoid repeated accept and drop
    if (dropped >= 8)
        return true;

    // remove festival/class quests and quests in different zone
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        const int64_t botZoneId = this->bot->GetZoneId();

        int32 const zoneOrSort = quest->GetZoneOrSort();
        if (zoneOrSort < 0 || (zoneOrSort > 0 && zoneOrSort != botZoneId))
        {
            QuestDropReason const reason = zoneOrSort < 0 ? QuestDropReason::ClassOrHoliday
                                                          : QuestDropReason::OtherZone;
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->RecordQuestDropped(questId, reason);
            dropped++;
        }
    }

    if (dropped >= 8)
        return true;

    // clear quests log
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << (uint8)i;
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        if (botAI->GetMaster())
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "new_rpg_quest_dropped",
                "Quest dropped %quest",
                {{"%quest", ChatHelper::FormatQuest(quest)}}));
        botAI->RecordQuestDropped(questId, QuestDropReason::LogFull);
    }

    return true;
}

bool NewRpgBaseAction::SearchQuestGiverAndAcceptOrReward()
{
    OrganizeQuestLog();
    ObjectGuid npcOrGo = AI_VALUE(ObjectGuid, "new rpg quest giver");
    if (!npcOrGo)
        return false;

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, npcOrGo);
    if (!object)
        return false;

    NewRpgInfo& info = botAI->rpgInfo;
    if (info.questGiver != npcOrGo || GetMSTimeDiffToNow(info.questGiverLastTry) > questGiverRetryGap)
    {
        info.questGiver = npcOrGo;
        info.questGiverSince = getMSTime();
    }
    info.questGiverLastTry = getMSTime();

    // Unreachable, or an offer the interaction never clears: let the status action and grinding resume.
    if (GetMSTimeDiffToNow(info.questGiverSince) > questGiverGiveUpTime)
    {
        LOG_DEBUG("playerbots", "[New RPG] {} gives up on quest giver {}", bot->GetName(), npcOrGo.ToString());
        info.ignoredQuestGivers.insert(npcOrGo);
        info.questGiver.Clear();
        return false;
    }

    if (bot->CanInteractWithQuestGiver(object))
    {
        InteractWithNpcOrGameObjectForQuest(npcOrGo);
        ForceToWait(5000);
        return true;
    }
    return MoveWorldObjectTo(npcOrGo);
}

static std::vector<float> GenerateRandomWeights(int n)
{
    std::vector<float> weights(n);
    float sum = 0.0;

    for (int i = 0; i < n; ++i)
    {
        weights[i] = rand_norm();
        sum += weights[i];
    }
    for (int i = 0; i < n; ++i)
    {
        weights[i] /= sum;
    }
    return weights;
}

namespace
{
    // [guild10] Where quests end and where their objectives stand, straight from the world DB: every spawn of
    // each quest ender, of each objective creature / object, and the explore area triggers. Built once, shared
    // by all bots; used where a quest has no POI to go by (talk, delivery, explore, objectives without POI).
    struct QuestSpot
    {
        uint32 mapId;
        float x, y, z;
    };

    struct QuestSpawnIndex
    {
        std::unordered_map<uint32, std::vector<QuestSpot>> enders;    // quest id -> ender spawns
        std::unordered_map<uint32, std::vector<QuestSpot>> creatures;  // creature entry -> spawns
        std::unordered_map<uint32, std::vector<QuestSpot>> objects;    // object entry -> spawns
        std::unordered_map<uint32, std::vector<QuestSpot>> triggers;   // quest id -> explore area triggers
    };

    QuestSpawnIndex const& GetQuestSpawnIndex()
    {
        static QuestSpawnIndex index;
        static std::once_flag built;
        std::call_once(built, []()
        {
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                index.creatures[data.id].push_back({data.mapid, data.posX, data.posY, data.posZ});
            for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
                index.objects[data.id].push_back({data.mapid, data.posX, data.posY, data.posZ});

            for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
            {
                auto spawns = index.creatures.find(entry);
                if (spawns != index.creatures.end())
                    index.enders[questId].insert(index.enders[questId].end(), spawns->second.begin(),
                                                 spawns->second.end());
            }
            for (auto const& [entry, questId] : *sObjectMgr->GetGOQuestInvolvedRelationMap())
            {
                auto spawns = index.objects.find(entry);
                if (spawns != index.objects.end())
                    index.enders[questId].insert(index.enders[questId].end(), spawns->second.begin(),
                                                 spawns->second.end());
            }

            if (QueryResult result = WorldDatabase.Query("SELECT id, quest FROM areatrigger_involvedrelation"))
            {
                do
                {
                    Field* fields = result->Fetch();
                    if (AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(fields[0].Get<uint32>()))
                        index.triggers[fields[1].Get<uint32>()].push_back(
                            {trigger->map, trigger->x, trigger->y, trigger->z});
                } while (result->NextRow());
            }
        });
        return index;
    }

    // The spot of `spots` on the bot's map nearest to it, as a POI; false if none is on this map.
    bool NearestSpotOnMap(Player* bot, std::vector<QuestSpot> const& spots, int32 objectiveIdx,
                          std::vector<POIInfo>& poiInfo)
    {
        QuestSpot const* best = nullptr;
        float bestDist = std::numeric_limits<float>::max();
        for (QuestSpot const& spot : spots)
        {
            if (spot.mapId != bot->GetMapId())
                continue;
            float const d = bot->GetDistance2d(spot.x, spot.y);
            if (d < bestDist)
            {
                bestDist = d;
                best = &spot;
            }
        }
        if (!best)
            return false;
        poiInfo.push_back({{best->x, best->y}, objectiveIdx});
        return true;
    }
}

bool NewRpgBaseAction::GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;

    static QuestPOIVector const noPoi;
    QuestPOIVector const* poiVector = sObjectMgr->GetQuestPOIVector(questId);
    if (!poiVector)
        poiVector = &noPoi;

    auto const statusItr = bot->getQuestStatusMap().find(questId);
    if (statusItr == bot->getQuestStatusMap().end())
        return false;
    QuestStatusData const& q_status = statusItr->second;
    QuestSpawnIndex const& spawnIndex = GetQuestSpawnIndex();

    if (toComplete && q_status.Status == QUEST_STATUS_COMPLETE)
    {
        for (QuestPOI const& qPoi : *poiVector)
        {
            if (qPoi.MapId != bot->GetMapId())
                continue;

            // not the poi pos to reward quest
            if (qPoi.ObjectiveIndex != -1)
                continue;

            if (qPoi.points.size() == 0)
                continue;

            float dx = 0, dy = 0;
            std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
            for (size_t i = 0; i < qPoi.points.size(); i++)
            {
                QuestPOIPoint const& point = qPoi.points[i];
                dx += point.x * weights[i];
                dy += point.y * weights[i];
            }

            if (bot->GetDistance2d(dx, dy) >= 1500.0f)
                continue;

            float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

            if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
                continue;

            if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
                continue;

            poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
        }

        // [guild10] no hand-in POI: straight to the quest ender's spawn (creature / gameobject_questender)
        if (poiInfo.empty())
        {
            auto const enders = spawnIndex.enders.find(questId);
            if (enders != spawnIndex.enders.end())
                NearestSpotOnMap(bot, enders->second, -1, poiInfo);
        }

        return !poiInfo.empty();
    }

    if (q_status.Status != QUEST_STATUS_INCOMPLETE)
        return false;

    // Get incomplete quest objective index
    std::vector<int32> incompleteObjectiveIdx;
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; i++)
    {
        int32 npcOrGo = quest->RequiredNpcOrGo[i];
        if (!npcOrGo)
            continue;

        if (q_status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
            incompleteObjectiveIdx.push_back(i);
    }
    for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
    {
        uint32 itemId = quest->RequiredItemId[i];
        if (!itemId)
            continue;

        if (q_status.ItemCount[i] < quest->RequiredItemCount[i])
            incompleteObjectiveIdx.push_back(QUEST_OBJECTIVES_COUNT + i);
    }

    // Get POIs to go
    for (QuestPOI const& qPoi : *poiVector)
    {
        if (qPoi.MapId != bot->GetMapId())
            continue;

        bool inComplete = false;
        for (uint32 objective : incompleteObjectiveIdx)
        {
            if (qPoi.ObjectiveIndex == static_cast<int32>(objective))
            {
                inComplete = true;
                break;
            }
        }
        if (!inComplete)
            continue;
        if (qPoi.points.size() == 0)
            continue;
        float dx = 0, dy = 0;
        std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
        for (size_t i = 0; i < qPoi.points.size(); i++)
        {
            QuestPOIPoint const& point = qPoi.points[i];
            dx += point.x * weights[i];
            dy += point.y * weights[i];
        }

        if (bot->GetDistance2d(dx, dy) >= 1500.0f)
            continue;

        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            continue;

        if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
            continue;

        poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
    }

    // [guild10] no objective POI: the spawns of the objective creatures / objects, else the explore trigger
    if (poiInfo.empty())
    {
        for (uint32 objective : incompleteObjectiveIdx)
        {
            if (objective >= QUEST_OBJECTIVES_COUNT)
                continue;
            int32 const npcOrGo = quest->RequiredNpcOrGo[objective];
            auto const& source = npcOrGo > 0 ? spawnIndex.creatures : spawnIndex.objects;
            auto const spawns = source.find(npcOrGo > 0 ? uint32(npcOrGo) : uint32(-npcOrGo));
            if (spawns != source.end())
                NearestSpotOnMap(bot, spawns->second, int32(objective), poiInfo);
        }
    }
    if (poiInfo.empty() && quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
    {
        auto const triggers = spawnIndex.triggers.find(questId);
        if (triggers != spawnIndex.triggers.end())
            NearestSpotOnMap(bot, triggers->second, QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT, poiInfo);
    }

    return !poiInfo.empty();
}

WorldPosition NewRpgBaseAction::SelectRandomGrindPos(Player* bot)
{
    std::vector<WorldLocation> const& locs = sTravelMgr.GetLocsPerLevelCache(bot->GetLevel());
    float hiRange = 500.0f;
    float loRange = 2500.0f;
    if (bot->GetLevel() < 5)
    {
        hiRange /= 3;
        loRange /= 3;
    }
    std::vector<WorldLocation> lo_prepared_locs, hi_prepared_locs;

    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        if (bot->GetExactDist(loc) > 2500.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        if (bot->GetExactDist(loc) < hiRange)
        {
            hi_prepared_locs.push_back(loc);
        }

        if (bot->GetExactDist(loc) < loRange)
        {
            lo_prepared_locs.push_back(loc);
        }
    }
    WorldPosition dest{};
    if (urand(1, 100) <= 50 && !hi_prepared_locs.empty())
    {
        uint32 idx = urand(0, hi_prepared_locs.size() - 1);
        dest = hi_prepared_locs[idx];
    }
    else if (!lo_prepared_locs.empty())
    {
        uint32 idx = urand(0, lo_prepared_locs.size() - 1);
        dest = lo_prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random grind pos Map:{} X:{} Y:{} Z:{} ({}+{} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              hi_prepared_locs.size(), lo_prepared_locs.size() - hi_prepared_locs.size(), locs.size());
    return dest;
}

WorldPosition NewRpgBaseAction::SelectRandomCampPos(Player* bot)
{
    const std::vector<WorldLocation> locs = sTravelMgr.GetTravelHubs(bot);

    bool inCity = false;

    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    std::vector<WorldLocation> prepared_locs;
    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        float range = bot->GetLevel() <= 5 ? 500.0f : 2500.0f;
        if (bot->GetExactDist(loc) > range)
            continue;

        if (bot->GetExactDist(loc) < 50.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        prepared_locs.push_back(loc);
    }
    WorldPosition dest{};
    if (!prepared_locs.empty())
    {
        uint32 idx = urand(0, prepared_locs.size() - 1);
        dest = prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random inn keeper pos Map:{} X:{} Y:{} Z:{} ({} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              prepared_locs.size(), locs.size());
    return dest;
}

bool NewRpgBaseAction::SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path)
{
    TravelMgr::FlightMasterInfo const* info = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!info)
        return false;

    std::vector<std::vector<uint32>> availablePaths = sTravelMgr.GetOptimalFlightDestinations(bot);
    if (availablePaths.empty())
        return false;

    flightMasterEntry = info->templateEntry;
    flightMasterPos = info->pos;
    path = availablePaths[urand(0, availablePaths.size() - 1)];
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random flight taxi node from:{} (node {}) to:{} ({} available)",
              bot->GetName(), flightMasterEntry, path[0], path[path.size() - 1], availablePaths.size());
    return true;
}

namespace
{
    // Where every quest in the database can be picked up: the first spawn of each creature or object that
    // starts it. Built once, shared by all bots.
    struct QuestStartPoint
    {
        uint32 questId;
        uint16 mapId;
        float x, y, z;
        uint32 creatureFaction;  // 0 for game objects
    };

    std::vector<QuestStartPoint> const& GetQuestStartPoints()
    {
        static std::vector<QuestStartPoint> points;
        static std::once_flag built;
        std::call_once(built, []()
        {
            std::unordered_map<uint32, CreatureData const*> creatureSpawn;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                creatureSpawn.emplace(data.id, &data);

            for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestRelationMap())
            {
                auto spawn = creatureSpawn.find(entry);
                CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(entry);
                if (spawn == creatureSpawn.end() || !creatureTemplate)
                    continue;

                CreatureData const* data = spawn->second;
                points.push_back({questId, data->mapid, data->posX, data->posY, data->posZ, creatureTemplate->faction});
            }

            std::unordered_map<uint32, GameObjectData const*> objectSpawn;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
                objectSpawn.emplace(data.id, &data);

            for (auto const& [entry, questId] : *sObjectMgr->GetGOQuestRelationMap())
            {
                auto spawn = objectSpawn.find(entry);
                if (spawn == objectSpawn.end())
                    continue;

                GameObjectData const* data = spawn->second;
                points.push_back({questId, data->mapid, data->posX, data->posY, data->posZ, 0});
            }
        });
        return points;
    }

    constexpr float QUEST_TRAVEL_SPEED = 7.0f;           // yards per second on foot
    constexpr float QUEST_OBJECTIVE_TIME = 180.0f;       // seconds to clear one quest objectives
    constexpr float QUEST_UNKNOWN_DISTANCE = 150.0f;     // assumed giver-to-objective distance without POI data
    constexpr float QUEST_MAX_GIVER_DISTANCE = 3000.0f;  // farther givers are left for a later trip
    constexpr float QUEST_IN_LOG_BONUS = 1.25f;          // accepted quests cost nothing to start
    constexpr float REST_KILL_QUEST_BONUS = 1.15f;       // rested experience doubles kill experience only
    constexpr float DALARAN_QUEST_BONUS = 4.0f;          // opens the city: portals, the crystal, the flight path
    constexpr float ZONE_IN_ORDER_BONUS = 1.3f;
    constexpr float ZONE_TOO_EARLY = 0.6f;
    constexpr float ZONE_OUTLEVELED = 0.8f;
    constexpr uint8 NORTHREND_ENTRY_LEVEL = 68;
    constexpr uint32 NORTHREND_MAP_ID = 571;

    // Northrend levelling order (both factions, from their own docks): Borean Tundra or Howling Fjord,
    // Dragonblight, Grizzly Hills, Zul'Drak, Sholazar Basin, then the Storm Peaks and Icecrown.
    struct ZoneBand
    {
        uint32 zoneId;
        uint8 low;
        uint8 high;
    };
    constexpr ZoneBand NORTHREND_ZONE_ORDER[] = {
        {3537, 68, 72},  // Borean Tundra
        {495, 68, 72},   // Howling Fjord
        {65, 72, 74},    // Dragonblight
        {394, 74, 76},   // Grizzly Hills
        {66, 76, 77},    // Zul'Drak
        {3711, 77, 78},  // Sholazar Basin
        {67, 79, 80},    // The Storm Peaks
        {210, 79, 80},   // Icecrown
    };

    bool IsKillQuest(Quest const* quest)
    {
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredNpcOrGo[i] > 0)
                return true;

        return false;
    }
}

// Soft preferences on top of experience per second: kill quests while rested, the Northrend zone order, the
// Dalaran attunement at 74.
float NewRpgBaseAction::GetQuestWeight(Quest const* quest, bool rested)
{
    uint8 const level = bot->GetLevel();
    float weight = 1.0f;
    if (rested && IsKillQuest(quest))
        weight *= REST_KILL_QUEST_BONUS;

    if (level >= NORTHREND_ENTRY_LEVEL && quest->GetZoneOrSort() > 0)
    {
        for (ZoneBand const& band : NORTHREND_ZONE_ORDER)
        {
            if (band.zoneId != static_cast<uint32>(quest->GetZoneOrSort()))
                continue;

            if (level < band.low)
                weight *= ZONE_TOO_EARLY;
            else if (level > band.high)
                weight *= ZONE_OUTLEVELED;
            else
                weight *= ZONE_IN_ORDER_BONUS;
            break;
        }
    }

    if (DalaranTravel::IsTeleportQuest(quest->GetQuestId()) ||
        quest->GetQuestId() == DalaranTravel::QUEST_LEAVE_AND_RETURN)
        weight *= DALARAN_QUEST_BONUS;

    return weight;
}

// Kills, object uses and items collected so far towards the quest objectives.
uint32 NewRpgBaseAction::GetQuestProgress(uint32 questId)
{
    auto itr = bot->getQuestStatusMap().find(questId);
    if (itr == bot->getQuestStatusMap().end())
        return 0;

    uint32 progress = 0;
    for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        progress += itr->second.CreatureOrGOCount[i];
    for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        progress += itr->second.ItemCount[i];

    return progress;
}

// The bot knows every quest in the world and picks the one that gives the most experience per second of
// travel and work: quests already in the log and quests it could still pick up, near or far.
// giverPos is set when the winner still has to be picked up.
uint32 NewRpgBaseAction::ChooseBestQuest(WorldPosition& giverPos)
{
    uint8 const level = bot->GetLevel();
    float bestRate = 0.0f;
    uint32 bestQuest = 0;
    float bestCompleteRate = 0.0f;  // [guild10] hand-ins first, then active objectives, then starters
    uint32 bestCompleteQuest = 0;
    giverPos = WorldPosition();
    bool const rested = bot->GetRestBonus() > 0.0f;

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        float distance = std::numeric_limits<float>::max();
        std::vector<POIInfo> poiInfo;
        WorldPosition dalaranTarget;
        if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            for (POIInfo const& poi : poiInfo)
                distance = std::min(distance, bot->GetDistance2d(poi.pos.x, poi.pos.y));
        }
        else if (GetDalaranQuestTarget(questId, dalaranTarget))
            distance = bot->GetDistance2d(dalaranTarget.GetPositionX(), dalaranTarget.GetPositionY());
        else
            continue;

        bool const complete = bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE;
        float time = distance / QUEST_TRAVEL_SPEED;
        if (!complete)
            time += QUEST_OBJECTIVE_TIME + QUEST_UNKNOWN_DISTANCE / QUEST_TRAVEL_SPEED;

        float const rate = QUEST_IN_LOG_BONUS * GetQuestWeight(quest, rested) * quest->XPValue(level) /
                           std::max(time, 1.0f);
        if (complete)
        {
            if (rate > bestCompleteRate)
            {
                bestCompleteRate = rate;
                bestCompleteQuest = questId;
            }
        }
        else if (rate > bestRate)
        {
            bestRate = rate;
            bestQuest = questId;
        }
    }

    if (bestCompleteQuest)
        return bestCompleteQuest;

    // [guild10] a new quest only once nothing in the log is left to do; every bot levels by quests
    if (bestQuest)
        return bestQuest;

    // From 68 the planner also looks at Northrend, priced by the boats and zeppelins that get there.
    std::vector<std::pair<WorldPosition, float>> northrendArrivals;
    if (level >= NORTHREND_ENTRY_LEVEL && bot->GetMapId() != NORTHREND_MAP_ID && !bot->GetMap()->Instanceable())
        sTravelRouteMgr.GetArrivalCosts(bot, NORTHREND_MAP_ID, northrendArrivals);

    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    for (QuestStartPoint const& start : GetQuestStartPoints())
    {
        float travelSeconds = 0.0f;
        if (start.mapId == bot->GetMapId())
        {
            // [guild10] the whole continent: a zone out of quests sends the bot on to the next one
            float const giverDistance = bot->GetDistance2d(start.x, start.y);
            travelSeconds = giverDistance / QUEST_TRAVEL_SPEED;
        }
        else if (start.mapId == NORTHREND_MAP_ID && !northrendArrivals.empty())
        {
            travelSeconds = std::numeric_limits<float>::max();
            for (auto const& [arrival, seconds] : northrendArrivals)
            {
                float const fromArrival =
                    std::hypot(arrival.GetPositionX() - start.x, arrival.GetPositionY() - start.y);
                if (fromArrival <= QUEST_MAX_GIVER_DISTANCE)
                    travelSeconds = std::min(travelSeconds, seconds + fromArrival / QUEST_TRAVEL_SPEED);
            }

            if (travelSeconds == std::numeric_limits<float>::max())
                continue;
        }
        else
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(start.questId);
        if (!quest || quest->GetMinLevel() > level)
            continue;

        int32 const questLevel = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : level;
        // [guild10] down to the last level that still gives experience (green), not only the current band
        if (questLevel > level + 2 || questLevel <= Acore::XP::GetGrayLevel(level))
            continue;

        if (bot->GetQuestStatus(start.questId) != QUEST_STATUS_NONE || botAI->lowPriorityQuest.count(start.questId))
            continue;

        if (start.creatureFaction && botFaction)
        {
            FactionTemplateEntry const* giverFaction = sFactionTemplateStore.LookupEntry(start.creatureFaction);
            if (giverFaction && giverFaction->IsHostileTo(*botFaction))
                continue;
        }

        if (!IsQuestWorthDoing(quest) || !IsQuestCapableDoing(quest) || !bot->CanTakeQuest(quest, false))
            continue;

        float objectiveDistance = QUEST_UNKNOWN_DISTANCE;
        if (QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(start.questId))
        {
            for (QuestPOI const& poi : *pois)
            {
                if (poi.ObjectiveIndex < 0 || poi.MapId != start.mapId || poi.points.empty())
                    continue;

                float cx = 0.0f;
                float cy = 0.0f;
                for (QuestPOIPoint const& point : poi.points)
                {
                    cx += point.x;
                    cy += point.y;
                }
                cx /= poi.points.size();
                cy /= poi.points.size();
                objectiveDistance = std::sqrt((cx - start.x) * (cx - start.x) + (cy - start.y) * (cy - start.y));
                break;
            }
        }

        // walk to the giver, out to the objective and back to hand it in, plus the work itself
        float const time = travelSeconds + 2.0f * objectiveDistance / QUEST_TRAVEL_SPEED + QUEST_OBJECTIVE_TIME;
        float const rate = GetQuestWeight(quest, rested) * quest->XPValue(level) / time;
        if (rate > bestRate)
        {
            bestRate = rate;
            bestQuest = start.questId;
            giverPos = WorldPosition(start.mapId, start.x, start.y, start.z);
        }
    }

    return bestQuest;
}

uint32 NewRpgBaseAction::ChooseNextLevelQuest(WorldPosition& giverPos, WorldPosition& grindPos)
{
    uint8 const level = bot->GetLevel();
    uint32 bestMinLevel = std::numeric_limits<uint32>::max();
    float bestDistance = std::numeric_limits<float>::max();
    uint32 bestQuest = 0;
    giverPos = WorldPosition();
    grindPos = WorldPosition();

    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    for (QuestStartPoint const& start : GetQuestStartPoints())
    {
        if (start.mapId != bot->GetMapId())
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(start.questId);
        if (!quest || quest->GetMinLevel() <= level || quest->GetMinLevel() > bestMinLevel)
            continue;

        if (bot->GetQuestStatus(start.questId) != QUEST_STATUS_NONE || botAI->lowPriorityQuest.count(start.questId))
            continue;

        if (start.creatureFaction && botFaction)
        {
            FactionTemplateEntry const* giverFaction = sFactionTemplateStore.LookupEntry(start.creatureFaction);
            if (giverFaction && giverFaction->IsHostileTo(*botFaction))
                continue;
        }

        if (!bot->SatisfyQuestClass(quest, false) || !bot->SatisfyQuestRace(quest, false) ||
            !bot->SatisfyQuestPreviousQuest(quest, false) || !bot->SatisfyQuestSkill(quest, false))
            continue;

        float const distance = bot->GetDistance2d(start.x, start.y);
        if (quest->GetMinLevel() == bestMinLevel && distance >= bestDistance)
            continue;

        bestMinLevel = quest->GetMinLevel();
        bestDistance = distance;
        bestQuest = start.questId;
        giverPos = WorldPosition(start.mapId, start.x, start.y, start.z);
    }

    if (!bestQuest)
        return 0;

    // the grind spot of its level nearest the giver, other than the one the bot already stands on
    float bestGrind = std::numeric_limits<float>::max();
    for (WorldLocation const& loc : sTravelMgr.GetLocsPerLevelCache(level))
    {
        if (loc.GetMapId() != bot->GetMapId() || bot->GetExactDist(loc) < 10.0f)
            continue;

        float const d = giverPos.GetExactDist(loc);
        if (d < bestGrind)
        {
            bestGrind = d;
            grindPos = WorldPosition(loc);
        }
    }
    return bestQuest;
}

void NewRpgBaseAction::LogDungeonCandidates()
{
    uint8 const level = bot->GetLevel();
    uint8 const expansion = bot->GetSession()->Expansion();
    for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(i);
        if (!dungeon || dungeon->TypeID != lfg::LFG_TYPE_DUNGEON || dungeon->Difficulty != DUNGEON_DIFFICULTY_NORMAL ||
            dungeon->ExpansionLevel > expansion || level < dungeon->MinLevel || level > dungeon->MaxLevel)
            continue;

        LOG_INFO("playerbots", "[rpg] {} L{} dungeon candidate: {} (map {}, levels {}-{})", bot->GetName(), level,
                 dungeon->Name[0], dungeon->MapID, dungeon->MinLevel, dungeon->MaxLevel);
    }
}

bool NewRpgBaseAction::RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus)
{
    std::vector<NewRpgStatus> availableStatus;
    uint32 probSum = 0;
    for (NewRpgStatus status : candidateStatus)
    {
        if (sPlayerbotAIConfig.RpgStatusProbWeight[status] == 0)
            continue;

        if (CheckRpgStatusAvailable(status))
        {
            availableStatus.push_back(status);
            probSum += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        }
    }
    // Safety check. Default to "rest" if all RPG weights = 0
    if (availableStatus.empty() || probSum == 0)
    {
        botAI->rpgInfo.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    uint32 rand = urand(1, probSum);
    uint32 accumulate = 0;
    NewRpgStatus chosenStatus = RPG_STATUS_END;
    for (NewRpgStatus status : availableStatus)
    {
        accumulate += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        if (accumulate >= rand)
        {
            chosenStatus = status;
            break;
        }
    }

    switch (chosenStatus)
    {
        case RPG_WANDER_RANDOM:
        {
            botAI->rpgInfo.ChangeToWanderRandom();
            return true;
        }
        case RPG_WANDER_NPC:
        {
            botAI->rpgInfo.ChangeToWanderNpc();
            return true;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoGrind(pos);
                return true;
            }
            return false;
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoCamp(pos);
                return true;
            }
            return false;
        }
        case RPG_DO_QUEST:
        {
            WorldPosition giverPos;
            uint32 questId = ChooseBestQuest(giverPos);
            Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
            if (!quest)
                return false;

            botAI->rpgInfo.ChangeToDoQuest(questId, quest, giverPos);
            return true;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            if (SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
            {
                botAI->rpgInfo.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, path);
                return true;
            }
            return false;
        }
        case RPG_DO_GATHER:
        {
            botAI->rpgInfo.ChangeToDoGather();
            return true;
        }
        case RPG_IDLE:
        {
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        case RPG_REST:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
        case RPG_OUTDOOR_PVP:
        {
            botAI->rpgInfo.ChangeToOutdoorPvp();
            return true;
        }
        default:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
    }
    return false;
}

bool NewRpgBaseAction::CheckRpgStatusAvailable(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_IDLE:
        case RPG_REST:
            return true;
        case RPG_WANDER_RANDOM:
        {
            Unit* target = AI_VALUE(Unit*, "grind target");
            return target != nullptr;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            return pos != WorldPosition();
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            return pos != WorldPosition();
        }
        case RPG_WANDER_NPC:
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            return possibleTargets.size() >= 3;
        }
        case RPG_DO_QUEST:
        {
            if (!sRandomPlayerbotMgr.IsRandomBot(bot))
            {
                WorldPosition giverPos;
                return ChooseBestQuest(giverPos) != 0;
            }

            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    return true;
                }
            }
            return false;
        }
        case RPG_DO_GATHER:
        {
            if (!botAI->HasSkill(SKILL_HERBALISM) && !botAI->HasSkill(SKILL_MINING))
                return false;

            // No point farming with (nearly) full bags - the loot pipeline
            // can't store the harvest (see StoreLootAction).
            if (AI_VALUE(uint8, "bag space") > 80)
                return false;

            return sGatherNodeMgr.HasUsableNodes(bot);
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            return SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path);
        }
        case RPG_OUTDOOR_PVP:
        {
            if (!bot->IsPvP())
                return false;
            uint32 zoneId = bot->GetZoneId();
            if (zoneId == AREA_NAGRAND)
                return false;

            OutdoorPvP* outdoorPvP = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
            return outdoorPvP != nullptr;
        }
        default:
            return false;
    }
    return false;
}
