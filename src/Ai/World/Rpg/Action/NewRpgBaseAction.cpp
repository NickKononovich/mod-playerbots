/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBaseAction.h"
#include "BotPath.h"  // [lead109]
#include "BroadcastHelper.h"
#include "GuildCoordinator.h"  // [guild50] the quest reward pick
#include "ChatHelper.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "GameObjectModel.h"
#include "GameEventMgr.h"  // [guild15] event spawns in the danger index
#include "DatabaseEnv.h"
#include "Formulas.h"
#include "Group.h"
#include "LFGMgr.h"
#include "LootObjectStack.h"
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
#include "SoloSafety.h"  // [guild13]
#include "PerfMonitor.h"  // [guild15]
#include "QuestStats.h"  // [guild90]
#include "World.h"  // [guild13]
#include "RouteSearch.h"  // [guild13] PointOnPath
#include "SpellMgr.h"     // [guild13]
#include "LeaderOrders.h"  // [guild14] Leader::Route
#include "DetourNavMeshQuery.h"  // [guild14] the nearest polygon
#include "MapDefines.h"         // [guild14] NAV_* flags
#include <map>
#include <tuple>
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
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

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

bool NewRpgBaseAction::SoloSafetyStep()
{
    if (bot->IsInCombat() || !bot->IsAlive() || bot->IsInFlight() || bot->GetTransport())
        return false;
    // the user's rule: after a death the bot does not go straight back - it goes to its hub, rests, plans again
    if (SoloSafety::TakeRecovery(bot))
    {
        // risen at the graveyard's spirit healer (below the resurrection-sickness level): it is in town already
        if (int32(bot->GetLevel()) < sWorld->getIntConfig(CONFIG_DEATH_SICKNESS_LEVEL))
        {
            LOG_INFO("playerbots", "[rpg] {} L{} RECOVER after a death: risen at the graveyard, rests to full, plans "
                     "again", bot->GetName(), bot->GetLevel());
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        WorldPosition const hub = NearestHub(bot);
        if (hub != WorldPosition())
        {
            LOG_INFO("playerbots", "[rpg] {} L{} RECOVER after a death: rests to full, walks to its hub ({:.0f} yd)",
                     bot->GetName(), bot->GetLevel(), bot->GetExactDist(hub));
            botAI->rpgInfo.ChangeToGoCamp(hub);
            return true;
        }
    }
    if (SoloSafety::WaitsToRegen(bot))
    {
        if (!bot->IsSitState())
        {
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            LOG_INFO("playerbots", "[rest] {} sits to regenerate: nothing to eat or drink (hp {:.0f} %, mana {:.0f} %)",
                     bot->GetName(), bot->GetHealthPct(),
                     bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f);
        }
        return true;
    }
    return TameFirstPet();
}

namespace
{
    // [guild90] a spent mark: the bot stood here and saw nothing of the quest within SPENT_RADIUS (no mob it
    // pulls, no object). The spawns there come back at their respawn time (creature / gameobject spawntimesecs,
    // the world's own clock): the mark holds until then. A mark with no spawn of the quest's sources near it (a
    // POI blend) holds until the bot's next level.
    struct SpentMark
    {
        float x, y;
        time_t until;  // 0: until the next level
        uint8 level;
    };
    constexpr float SPENT_RADIUS = 30.0f;
    std::mutex spentLock;
    // bot guid counter -> quest -> the spent marks
    std::unordered_map<uint32, std::unordered_map<uint32, std::vector<SpentMark>>> spentSpots;

    // [guild90] the shortest respawn (seconds) of the quest's objective sources spawned within SPENT_RADIUS of
    // (x, y) on the bot's map; 0: none there (defined with the spawn index below)
    uint32 ObjectiveRespawnNear(Player* bot, uint32 questId, float x, float y);

    void LogQuestObject(Player* bot, GameObject* go, std::string const& verdict);

    bool MarkHolds(SpentMark const& mark, Player* bot, time_t now)
    {
        return mark.until ? now < mark.until : mark.level == bot->GetLevel();
    }
}

void NewRpgBaseAction::NoteSpentSpot(Player* bot, uint32 questId, WorldPosition const& pos)
{
    uint32 const respawn = ObjectiveRespawnNear(bot, questId, pos.GetPositionX(), pos.GetPositionY());
    time_t const now = time(nullptr);
    if (respawn)
        LOG_INFO("playerbots", "[rpg] {} L{} spot of q{} spent at ({:.0f},{:.0f}): nothing to pull or loot within {:.0f} "
                 "yd, back at its respawn ({} s)", bot->GetName(), bot->GetLevel(), questId, pos.GetPositionX(),
                 pos.GetPositionY(), SPENT_RADIUS, respawn);
    else
        LOG_INFO("playerbots", "[rpg] {} L{} spot of q{} spent at ({:.0f},{:.0f}): nothing to pull or loot within {:.0f} "
                 "yd, no spawn of its sources there (held until the next level)", bot->GetName(), bot->GetLevel(),
                 questId, pos.GetPositionX(), pos.GetPositionY(), SPENT_RADIUS);
    std::lock_guard<std::mutex> guard(spentLock);
    auto& marks = spentSpots[bot->GetGUID().GetCounter()][questId];
    marks.erase(std::remove_if(marks.begin(), marks.end(),
                               [bot, now](SpentMark const& m) { return !MarkHolds(m, bot, now); }),
                marks.end());
    marks.push_back({pos.GetPositionX(), pos.GetPositionY(), respawn ? now + time_t(respawn) : 0, bot->GetLevel()});
}

bool NewRpgBaseAction::IsSpentSpot(Player* bot, uint32 questId, float x, float y)
{
    std::lock_guard<std::mutex> guard(spentLock);
    auto const quests = spentSpots.find(bot->GetGUID().GetCounter());
    if (quests == spentSpots.end())
        return false;
    auto const spots = quests->second.find(questId);
    if (spots == quests->second.end())
        return false;
    time_t const now = time(nullptr);
    // [guild90] the whole scanned circle is spent, not the one point the bot walked to
    for (SpentMark const& mark : spots->second)
        if ((mark.x - x) * (mark.x - x) + (mark.y - y) * (mark.y - y) <= SPENT_RADIUS * SPENT_RADIUS &&
            MarkHolds(mark, bot, now))
            return true;
    return false;
}

void NewRpgBaseAction::ClearSpentSpots(Player* bot, uint32 questId)
{
    std::lock_guard<std::mutex> guard(spentLock);
    auto const quests = spentSpots.find(bot->GetGUID().GetCounter());
    if (quests != spentSpots.end())
        quests->second.erase(questId);
}

bool NewRpgBaseAction::DestinationRefused(WorldPosition const& dest)
{
    if (dest.GetMapId() != bot->GetMapId() || !SoloSafety::Applies(bot) || bot->IsInCombat() ||
        bot->GetExactDist(dest) > sPlayerbotAIConfig.SightDistance)
        return false;
    if (CreatureTemplate const* danger = InDangerZone(bot, dest.GetPositionX(), dest.GetPositionY()))
    {
        LOG_INFO("playerbots", "[solo] {} L{} walk to ({:.0f},{:.0f}) refused: in its danger zone of {}",
                 bot->GetName(), bot->GetLevel(), dest.GetPositionX(), dest.GetPositionY(), danger->Name);
        return true;
    }
    // the idle mob whose reach (aggro radius, its wander area) covers the destination nearest to it: its pull as the
    // judge sees it (the pack it brings); one look per destination
    // (a refusal leaves a danger zone there: the check above answers it from then on)
    static std::mutex lookLock;
    // bot -> (destination, level) of its last look, and whether the walk there passes a refused reach
    static std::unordered_map<uint32, std::pair<std::tuple<int32, int32, uint8>, bool>> looked;
    std::tuple<int32, int32, uint8> const key{int32(dest.GetPositionX()), int32(dest.GetPositionY()),
                                              bot->GetLevel()};
    {
        std::lock_guard<std::mutex> guard(lookLock);
        auto& last = looked[bot->GetGUID().GetCounter()];
        if (last.first == key)
            return last.second;
        last = {key, false};
    }
    // [guild15] the walk itself: a destination inside a camp it must not fight has no walk round the camp (b323:
    // Baridrol L8 to Oben Rageclaw in Ban'ethil Barrow Den, Orton / Cassand L8 to Rolf's corpse at Stone Cairn
    // Lake, Ravelandy to Jerod's Landing: attacked by 2-6, dead)
    {
        Leader::RouteResult const route = Leader::Route(bot, dest);
        if (route.found && route.throughRefused)
        {
            LOG_INFO("playerbots", "[solo] {} L{} walk to ({:.0f},{:.0f}) refused: its way passes a pack it does not "
                     "fight and no walk goes round it", bot->GetName(), bot->GetLevel(), dest.GetPositionX(),
                     dest.GetPositionY());
            std::lock_guard<std::mutex> guard(lookLock);
            looked[bot->GetGUID().GetCounter()].second = true;
            return true;
        }
    }
    Creature* nearest = nullptr;
    float nearestGap = std::numeric_limits<float>::max();
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
            !mob->HasReactState(REACT_AGGRESSIVE))
            continue;
        float reach = mob->GetAggroRange(bot);
        if (reach <= 0.0f)
            continue;
        Position centre = mob->GetPosition();
        if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
        {
            centre = mob->GetHomePosition();
            reach += mob->GetWanderDistance();
        }
        float const gap = centre.GetExactDist2d(dest.GetPositionX(), dest.GetPositionY());
        if (gap <= reach && gap < nearestGap)
        {
            nearestGap = gap;
            nearest = mob;
        }
    }
    if (!nearest)
        return false;
    SoloSafety::Verdict const verdict = SoloSafety::Judge(bot, nearest);
    if (verdict.kind != SoloSafety::Verdict::Kind::Refuse)
        return false;
    LOG_INFO("playerbots", "[solo] {} L{} walk to ({:.0f},{:.0f}) refused: among {} L{}: {}", bot->GetName(),
             bot->GetLevel(), dest.GetPositionX(), dest.GetPositionY(), nearest->GetName(), nearest->GetLevel(),
             verdict.why);
    RememberDanger(bot, verdict.at.GetPositionX(), verdict.at.GetPositionY(), verdict.radius, nearest->GetEntry(),
                   verdict.topLevel);
    return true;
}

bool NewRpgBaseAction::TameFirstPet()
{
    constexpr uint32 SPELL_TAME_BEAST = 1515;
    if (bot->getClass() != CLASS_HUNTER || !bot->HasSpell(SPELL_TAME_BEAST) || bot->GetPet() ||
        bot->GetPetGUID() || bot->IsNonMeleeSpellCast(true) || !SoloSafety::Applies(bot))
        return false;
    if (PetStable* stable = bot->GetPetStable())
        if (stable->CurrentPet || stable->UnslottedPets.size())
            return false;
    SpellInfo const* tame = sSpellMgr->GetSpellInfo(SPELL_TAME_BEAST);
    if (!tame || bot->HasSpellCooldown(SPELL_TAME_BEAST))
        return false;
    // the nearest beast it may tame (CreatureTemplate::IsTameable), no higher than its level, idle and untapped
    Creature* beast = nullptr;
    float best = sPlayerbotAIConfig.SightDistance;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        Creature* creature = botAI->GetCreature(guid);
        if (!creature || !creature->IsAlive() || creature->IsInCombat() || creature->GetLevel() > bot->GetLevel() ||
            creature->hasLootRecipient() || !creature->GetCreatureTemplate()->IsTameable(bot->CanTameExoticPets()))
            continue;
        if (NewRpgBaseAction::InDangerZone(bot, creature->GetPositionX(), creature->GetPositionY()))
            continue;
        float const d = bot->GetExactDist(creature);
        if (d < best)
        {
            best = d;
            beast = creature;
        }
    }
    if (!beast)
        return false;
    float const range = tame->GetMaxRange(false, bot);
    if (best > range || !bot->IsWithinLOSInMap(beast))
        return MoveTo(beast, range - 5.0f);
    bot->StopMoving();
    bot->SetFacingToObject(beast);
    LOG_INFO("playerbots", "[rpg] {} L{} tames {} L{} (entry {})", bot->GetName(), bot->GetLevel(), beast->GetName(),
             beast->GetLevel(), beast->GetEntry());
    return bot->CastSpell(beast, SPELL_TAME_BEAST, false) == SPELL_CAST_OK;
}

bool NewRpgBaseAction::MoveFarTo(WorldPosition dest)
{
    if (dest == WorldPosition())
        return false;

    if (SoloSafetyStep())  // [guild13]
        return true;
    if (DestinationRefused(dest))  // [guild13]
    {
        NewRpgStatus const status = botAI->rpgInfo.GetStatus();
        // a quest whose spot (its objective or its ender: b302 Kianey's q45 hand-in at Rolf's corpse among seven
        // Murlocs, re-chosen every look) lies there is postponed: the planner takes the next one
        if (auto* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data))
        {
            if (botAI->lowPriorityQuest.insert(quest->questId).second)
            {
                LOG_INFO("playerbots", "[rpg] {} L{} POSTPONE q{} reason: its spot lies among mobs it does not pull",
                         bot->GetName(), bot->GetLevel(), quest->questId);
                NotePostpone(bot, quest->questId, "its spot lies among mobs it does not pull");
            }
        }
        // [guild14] a gather node / profession npc there: the state ends, its action marks the target failed and
        // takes the next one (b303 Gilduck, Gnetlibozz: DO_GATHER failed on a refused node every tick, STUCK)
        if (status == RPG_DO_QUEST || status == RPG_GO_CAMP || status == RPG_GO_GRIND || status == RPG_DO_GATHER ||
            status == RPG_DO_PROFESSION)
        {
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        return false;
    }
    // [guild15] the next stretch of the walk passes an idle mob whose fight it wins only at full health and mana: it
    // rests first, as a player tops up before walking on through a camp (b307/b308: 26 of 47 deaths were bots
    // attacked on the way, half of them below full)
    if (SoloSafety::RestBeforeWalk(bot, dest))
        return true;

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
    // [guild15] random bots without the teleport shortcut (the guild's group B) ride and step through like players
    if (sRandomPlayerbotMgr.IsRandomBot(bot) && sPlayerbotAIConfig.RandomBotTeleport)
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
        // [guild15] ... unless the destination lies on another landmass (the tree top and Rut'theran below)
        if (!otherMap && bot->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY()) < TRIP_PLAN_DISTANCE &&
            TravelRouteMgr::Landmass(WorldPosition(bot)) == TravelRouteMgr::Landmass(dest))
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
        // [guild15] an unplanned ride that already chose its stop keeps it: the Leave phase restarts phaseStart, and
        // the minute of riding counted from there skipped every dock - the bot "left" and came back on the same tick,
        // rode on and repeated it at every stop (b327/b329: Mytaeleas, Bierya 10+ round trips Rut'theran - Auberdine)
        if (plannedLeg ? d != trip.leg.toDock
                       : trip.phase != Phase::Leave && GetMSTimeDiffToNow(trip.phaseStart) < UNPLANNED_RIDE_MS)
            continue;

        if (TravelRouteMgr::IsDocked(transport, line->docks[d], remaining))
            exitDock = &line->docks[d];
    }

    // [guild15] the walk off takes the bot's own run time to the pier: a stop that ends before it would carry the bot
    // away on the deck's edge, where the transport check drops it into the sea (b324/b325 Thyllyn: off the
    // Rut'theran boat mid-sea, 3 drownings, 332 WALK_FAIL); it rides on to the next stop instead
    if (exitDock && trip.phase != Phase::Leave)
    {
        float const speed = bot->GetSpeed(bot->IsWalking() ? MOVE_WALK : MOVE_RUN);
        uint32 const walkMs =
            uint32(bot->GetExactDist(exitDock->pier) / (speed > 0.0f ? speed : 1.0f) * IN_MILLISECONDS);
        if (remaining < walkMs)
        {
            LOG_INFO("playerbots", "[rpg] {} L{} stays aboard {}: the stop ends in {} ms, the walk off takes {} ms",
                     bot->GetName(), bot->GetLevel(), line->entry, remaining, walkMs);
            exitDock = nullptr;
        }
    }

    if (!exitDock)
    {
        if (trip.phase == Phase::Leave)
        {
            // [guild15] the transport left while the bot walked off: back to the middle of the deck, not the edge
            info.SetTripPhase(Phase::Ride);
            LOG_INFO("playerbots", "[rpg] {} L{} back onto the deck of {}: it left the stop before the bot was off "
                     "(moving {}, walking {}, at ({:.1f},{:.1f},{:.1f}), offset on the transport ({:.1f},{:.1f},{:.1f}))",
                     bot->GetName(), bot->GetLevel(), line->entry, bot->isMoving() ? "yes" : "no",
                     bot->IsWalking() ? "yes" : "no", bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                     bot->GetTransOffsetX(), bot->GetTransOffsetY(), bot->GetTransOffsetZ());
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (FindDeckPoint(transport, x, y, z))
            {
                MovePointDirect(x, y, z);
                return true;
            }
        }

        return HoldPosition();
    }

    if (trip.phase != Phase::Leave)
    {
        info.SetTripPhase(Phase::Leave);
        LOG_INFO("playerbots", "[rpg] {} L{} leaves transport {} at map {}: pier {:.0f} yd, the stop ends in {} ms "
                 "(walking {}, speed {:.1f})",
                 bot->GetName(), bot->GetLevel(), line->entry, exitDock->mapId, bot->GetExactDist(exitDock->pier),
                 remaining, bot->IsWalking() ? "yes" : "no", bot->GetSpeed(bot->IsWalking() ? MOVE_WALK : MOVE_RUN));
    }

    // [guild15] a walk off that stopped (or never started: a last move still timed) is sent again at once
    if (!bot->isMoving() || !IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
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
        // [guild15] an area trigger is reached inside its radius: its centre can hang in the air (b327 the Deeprun
        // Tram entrances 2173 / 2175, radius 10, centre 4-5 yd above the floor: bots stood in the trigger walking to
        // its unreachable centre, gave up, STUCK; no bot ever rode the tram)
        AreaTrigger const* const at = portal.triggerId ? sObjectMgr->GetAreaTrigger(portal.triggerId) : nullptr;
        if (at ? !bot->IsInAreaTriggerRadius(at) : bot->GetExactDist(portal.from) > INTERACTION_DISTANCE - 1.0f)
            return WalkTo(portal.from);

        info.SetTripPhase(Phase::Wait);
    }

    // [guild15] an area trigger teleport: the bot walks into it (its centre) and the core's area trigger handler
    // checks the radius and teleports it, as a player's client reports the trigger
    if (portal.triggerId)
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(portal.triggerId);
        WorldPosition const dest = trip.dest;
        if (!trigger)
        {
            info.ClearTrip();
            return false;
        }
        if (!bot->IsInAreaTriggerRadius(trigger))
        {
            if (bot->GetExactDist(trigger->x, trigger->y, trigger->z) > 1.0f)
                return WalkTo(WorldPosition(trigger->map, trigger->x, trigger->y, trigger->z));
            info.ClearTrip();
            info.trip.dest = dest;
            info.trip.plannedAt = getMSTime();
            return false;
        }
        info.ClearTrip();
        LOG_INFO("playerbots", "[travel] {} L{} walks into area trigger {} -> ({:.0f},{:.0f},{:.0f})", bot->GetName(),
                 bot->GetLevel(), portal.triggerId, portal.to.GetPositionX(), portal.to.GetPositionY(),
                 portal.to.GetPositionZ());
        WorldPacket packet(CMSG_AREATRIGGER, 4);
        packet << portal.triggerId;
        packet.rpos(0);
        bot->GetSession()->HandleAreaTriggerOpcode(packet);
        return HoldPosition(2 * IN_MILLISECONDS);
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

bool NewRpgBaseAction::NpcOutOfWalk(WorldObject* npc)
{
    if (!npc || bot->isMoving() || npc->GetMapId() != bot->GetMapId())
        return false;
    BotPath const path = FindBotPath(bot, npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ());
    float const endGap = npc->GetExactDist(path.actualEnd.x, path.actualEnd.y, path.actualEnd.z);
    if ((path.type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) && endGap <= INTERACTION_DISTANCE)
        return false;
    LOG_INFO("playerbots", "[rpg] {} L{} npc {} out of walk: the route ends {:.0f} yd from it (type {:#x}), "
             "{:.0f} yd away", bot->GetName(), bot->GetLevel(), npc->GetName(), endGap, uint32(path.type),
             bot->GetExactDist(npc));
    return true;
}

// [guild13] one line per bot and destination when its far walk cannot start: what the route and the walk said
void NewRpgBaseAction::NoteWalkFailure(WorldPosition const& dest, std::string const& why)
{
    static std::mutex lock;
    static std::unordered_map<uint32, std::pair<int32, int32>> logged;
    std::pair<int32, int32> const key{int32(dest.GetPositionX()), int32(dest.GetPositionY())};
    {
        std::lock_guard<std::mutex> guard(lock);
        auto& last = logged[bot->GetGUID().GetCounter()];
        if (last == key)
            return;
        last = key;
    }
    LOG_INFO("playerbots", "[rpg] {} L{} WALK_FAIL at ({:.0f},{:.0f},{:.0f}) to ({:.0f},{:.0f},{:.0f}) {:.0f} yd: {}",
             bot->GetName(), bot->GetLevel(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
             dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), bot->GetExactDist(dest), why);
}

// [guild14] Player::HandleFall's fall damage (Player.cpp: FALL_DMG_EQU_SLOPE, FALL_DMG_EQU_INTERCEPT): the share of
// the maximum health a fall of `height` yards takes, safe fall auras counted
static float FallDamageShare(Player* bot, float height)
{
    float const safeFall = float(bot->GetTotalAuraModifier(SPELL_AURA_SAFE_FALL));
    return std::max(0.0f, 0.018f * (height - safeFall) - 0.2426f) * sWorld->getRate(RATE_DAMAGE_FALL);
}

// [guild13] / [guild14] the bot's own search finds no way out from where it stands (a navmesh island: Aldrassil's
// upper floor, the Exodar's halls, a ledge, a building's upper floor in Darnassus): the core's path (its start polygon
// from the bot's small box, not the route's tall column, which can take the roof over a floor; never its straight
// shortcut, which glides through the air), the route found from the destination back to the bot walked the other way,
// and at last a step onto the nearest navmesh polygon in sight - walked when the ground carries it there, else a
// jump down when the fall (the core's fall damage) leaves the bot alive. Each one logs WALK_FAIL with the position.
bool NewRpgBaseAction::WalkOffIsland(WorldPosition const& dest, float disToDest)
{
    // [guild14] a solo bot walks such a path only as far as it sees, and only when that stretch passes no mob of
    // orange / red con (b307 Guinudrond L9: the core's 2887 yd path from Loch Modan to Elwynn crossed Searing Gorge)
    auto const SafeLeg = [&](Movement::PointsArray& points)
    {
        if (!SoloSafety::Applies(bot) || points.size() < 2)
            return true;
        float walked = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
        {
            if (DangerOnWay(bot, points[i - 1].x, points[i - 1].y, points[i].x, points[i].y, points[i - 1].z, points[i].z))
                return false;
            walked += (points[i] - points[i - 1]).length();
            if (walked >= sPlayerbotAIConfig.SightDistance)
            {
                points.resize(i + 1);
                break;
            }
        }
        return true;
    };
    // [guild15] the ground under a straight leg (PlayerbotAI::HasGroundSupport, a yard apart, as the airborne
    // spline guard samples it): b308 Dandolar L4 stood 2365 s at (10091,2243,1340) in Darnassus - the core's path
    // from there began with a leg over open air, the guard refused it on every tick and the jump down was never tried
    auto const carried = [&](G3D::Vector3 const& from, G3D::Vector3 const& to)
    {
        float const gap = (to - from).length();
        for (float walked = 1.0f; walked < gap; walked += 1.0f)
        {
            G3D::Vector3 const at = from + (to - from) * (walked / gap);
            if (!PlayerbotAI::HasGroundSupport(bot, at.x, at.y, at.z))
                return false;
        }
        return true;
    };
    G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    // every leg the bot sees of it (the walk is planned again from there)
    auto const firstLegCarried = [&](Movement::PointsArray const& points)
    {
        if (points.size() < 2 || !carried(here, points[0]))
            return false;
        float walked = 0.0f;
        for (size_t i = 1; i < points.size() && walked < sPlayerbotAIConfig.SightDistance; ++i)
        {
            if (!carried(points[i - 1], points[i]))
                return false;
            walked += (points[i] - points[i - 1]).length();
        }
        return true;
    };
    auto const logged = [&](std::string const& how)
    {
        // one line per bot and way out (a failing one is tried again on every walk)
        static std::mutex lock;
        static std::unordered_map<uint32, std::string> last;
        {
            std::lock_guard<std::mutex> guard(lock);
            std::string& previous = last[bot->GetGUID().GetCounter()];
            if (previous == how)
                return true;
            previous = how;
        }
        LOG_INFO("playerbots", "[rpg] {} L{} WALK_FAIL at ({:.1f},{:.1f},{:.1f}) map {} to ({:.0f},{:.0f},{:.0f}): {}",
                 bot->GetName(), bot->GetLevel(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                 bot->GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), how);
        return true;
    };
    {
        PathGenerator core(bot);
        core.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false);
        Movement::PointsArray points = core.GetPath();
        G3D::Vector3 const coreEnd = core.GetActualEndPosition();
        PathType const coreType = core.GetPathType();
        if ((coreType & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) &&
            !(coreType & (PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT)) && points.size() >= 2 &&
            dest.GetExactDist(coreEnd.x, coreEnd.y, coreEnd.z) + 5.0f < disToDest && firstLegCarried(points) &&
            SafeLeg(points) && WalkBotPoints(bot, points))
        {
            float const delay = std::clamp(float(IN_MILLISECONDS) * MoveDelay(core.getPathLength()), 0.0f,
                                           float(sPlayerbotAIConfig.MaxWaitForMove));
            AI_VALUE(LastMovement&, "last movement")
                .Set(bot->GetMapId(), coreEnd.x, coreEnd.y, coreEnd.z, bot->GetOrientation(), delay,
                     MovementPriority::MOVEMENT_NORMAL);
            return logged(Acore::StringFormat("no route out from here: walks the core's path ({:.0f} yd)",
                                              core.getPathLength()));
        }
    }
    BotPath back = FindBotPath(bot, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
                               bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    if (back.type == PATHFIND_NORMAL && back.points.size() >= 2)
    {
        std::reverse(back.points.begin(), back.points.end());
        if (firstLegCarried(back.points) && SafeLeg(back.points) && WalkBotPoints(bot, back.points))
        {
            float const delay = std::clamp(float(IN_MILLISECONDS) * MoveDelay(back.length), 0.0f,
                                           float(sPlayerbotAIConfig.MaxWaitForMove));
            G3D::Vector3 const& end = back.points.back();
            AI_VALUE(LastMovement&, "last movement")
                .Set(bot->GetMapId(), end.x, end.y, end.z, bot->GetOrientation(), delay,
                     MovementPriority::MOVEMENT_NORMAL);
            return logged(Acore::StringFormat("no way out from here: walks the route found from the destination "
                                              "back ({:.0f} yd)",
                                              back.length));
        }
    }
    // the nearest polygon: the core's search box (PathGenerator::GetPolyByLocation, 3 yd round, 50 yd tall), widened
    // round the bot up to its sight
    dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
    if (!query || bot->IsInWater() || bot->IsFlying())
        return false;
    dtQueryFilter filter;
    filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER));
    filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));
    // [guild15] the polygons round the bot: the core's search box (3 yd round, 50 yd tall) at the bot and at points
    // all round it out to its sight (b308 Bierya L7 on a ridge over Shadowglen, z 1410: the one nearest polygon lay
    // under a lethal fall while a lower ledge round it did not)
    struct Way
    {
        G3D::Vector3 to;
        float gap;
        float drop;
        float damage;
        bool carried;
    };
    std::vector<Way> ways;
    bool onPolygon = false;
    {
        // on a polygon already (the core's own box): a navmesh island - only a way whose landing walks on to the
        // destination counts (b310 Lyleria L6 in Darnassus at z 1343: an island of one polygon, the city floor 22 yd
        // off and 15 yd down)
        float const point[3] = {here.y, here.z, here.x};
        float const extents[3] = {3.0f, 50.0f, 3.0f};
        float nearest[3] = {0.0f, 0.0f, 0.0f};
        dtPolyRef ref = 0;
        onPolygon = dtStatusSucceed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) && ref &&
                    (G3D::Vector3(nearest[2], nearest[0], nearest[1]) - here).length() < bot->GetCombatReach();
    }
    {
        // one search per spot: a spot with no way off is not searched again until the bot has moved
        static std::mutex lock;
        static std::unordered_map<uint32, std::tuple<uint32, int32, int32, int32>> searched;
        std::tuple<uint32, int32, int32, int32> const key{bot->GetMapId(), int32(here.x), int32(here.y),
                                                          int32(here.z)};
        std::lock_guard<std::mutex> guard(lock);
        auto& last = searched[bot->GetGUID().GetCounter()];
        if (last == key)
            return false;
        last = key;
    }
    auto const probe = [&](float x, float y, float reach)
    {
        float const point[3] = {y, here.z, x};
        float const extents[3] = {reach, 50.0f, reach};
        float nearest[3] = {0.0f, 0.0f, 0.0f};
        dtPolyRef ref = 0;
        if (dtStatusFailed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) || !ref)
            return;
        G3D::Vector3 const to(nearest[2], nearest[0], nearest[1]);
        float const gap = (to - here).length();
        if (gap < bot->GetCombatReach() || gap > sPlayerbotAIConfig.SightDistance)
            return;
        for (Way const& way : ways)
            if ((way.to - to).length() < 1.0f)
                return;
        if (!bot->GetMap()->isInLineOfSight(here.x, here.y, here.z + bot->GetCollisionHeight(), to.x, to.y,
                                            to.z + bot->GetCollisionHeight(), bot->GetPhaseMask(),
                                            LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
            return;
        float const drop = here.z - to.z;
        ways.push_back({to, gap, drop, drop > 0.0f ? FallDamageShare(bot, drop) * float(bot->GetMaxHealth()) : 0.0f,
                        carried(here, to)});
    };
    for (float reach = 3.0f; reach <= sPlayerbotAIConfig.SightDistance; reach *= 2.0f)
    {
        probe(here.x, here.y, reach);
        if (reach == 3.0f)
            continue;
        // the ring at this reach: one probe per arc of the box's own width
        uint32 const arcs = std::max<uint32>(8, uint32(std::ceil(2.0f * float(M_PI) * reach / (2.0f * 3.0f))));
        for (uint32 i = 0; i < arcs; ++i)
        {
            float const angle = 2.0f * float(M_PI) * float(i) / float(arcs);
            probe(here.x + std::cos(angle) * reach, here.y + std::sin(angle) * reach, 3.0f);
        }
        if (!onPolygon && std::any_of(ways.begin(), ways.end(), [](Way const& way) { return way.carried; }))
            break;
    }
    // the landing's own walk to the destination: a complete one, or a partial one (a long walk is cut by the
    // search's length) that ends nearer the destination than the bot by more than the step there - an island's own
    // polygons have neither (b311 live: Dandolar, Bierya, Nelaeleas - the long walks were all partial)
    auto const leadsOn = [&](Way const& way)
    {
        BotPath const on = FindBotPath(bot, way.to.x, way.to.y, way.to.z, dest.GetPositionX(), dest.GetPositionY(),
                                       dest.GetPositionZ());
        if (on.type == PATHFIND_NORMAL)
            return true;
        return on.type == PATHFIND_INCOMPLETE && on.points.size() >= 2 &&
               dest.GetExactDist(on.actualEnd.x, on.actualEnd.y, on.actualEnd.z) + way.gap < disToDest;
    };
    if (onPolygon)
    {
        std::vector<Way> kept;
        std::vector<Way const*> order;
        for (Way const& way : ways)
            if (way.carried || (way.drop > 0.0f && way.damage < float(bot->GetHealth())))
                order.push_back(&way);
        std::sort(order.begin(), order.end(),
                  [](Way const* a, Way const* b)
                  {
                      // an island is left by a drop: the falls first (least damage), then the steps (nearest); the
                      // island's own polygons are steps
                      if (a->carried != b->carried)
                          return !a->carried;
                      return a->carried ? a->gap < b->gap : a->damage < b->damage;
                  });
        constexpr size_t PATH_TRIES = 16;  // search cost bound, not a behaviour rule
        for (size_t i = 0; i < order.size() && i < PATH_TRIES && kept.empty(); ++i)
            if (leadsOn(*order[i]))
                kept.push_back(*order[i]);
        if (kept.empty())
        {
            logged(Acore::StringFormat("on a navmesh island: {} polygons in sight, no step or survivable fall leads on",
                                       ways.size()));
            return false;
        }
        ways = kept;
    }
    if (ways.empty())
    {
        logged("off the navmesh, no polygon in sight");
        return false;
    }
    // a step the ground carries, the nearest; else a jump down the bot survives, the least fall damage
    Way const* best = nullptr;
    for (Way const& way : ways)
        if (way.carried && (!best || way.gap < best->gap))
            best = &way;
    if (best)
    {
        bot->GetMotionMaster()->MovePoint(0, best->to.x, best->to.y, best->to.z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                          false, true);
        return logged(Acore::StringFormat("off the navmesh: steps onto the nearest polygon ({:.1f},{:.1f},{:.1f}) "
                                          "{:.0f} yd off",
                                          best->to.x, best->to.y, best->to.z, best->gap));
    }
    for (Way const& way : ways)
        if (way.drop > 0.0f && way.damage < float(bot->GetHealth()) &&
            (!best || way.damage < best->damage || (way.damage == best->damage && way.gap < best->gap)))
            best = &way;
    if (!best)
    {
        Way const& first = ways.front();
        logged(Acore::StringFormat("off the navmesh: {} polygons in sight, none a step or a fall it survives (the "
                                   "first {:.0f} yd {} over open air, {:.0f} damage at {} health)",
                                   ways.size(), std::fabs(first.drop), first.drop > 0.0f ? "down" : "up", first.damage,
                                   bot->GetHealth()));
        return false;
    }
    // a player jumps down: the core's jump arc to the polygon, the fall damage the core's formula gives
    float const speedXY = bot->GetSpeed(MOVE_RUN);
    bot->GetMotionMaster()->MoveJump(best->to.x, best->to.y, best->to.z, speedXY, speedXY);
    if (uint32 const fallDamage = uint32(best->damage))
        bot->EnvironmentalDamage(DAMAGE_FALL, fallDamage);
    return logged(Acore::StringFormat("off the navmesh: jumps down {:.0f} yd onto a polygon ({:.1f},{:.1f},{:.1f}) "
                                      "{:.0f} yd off, fall damage {}",
                                      best->drop, best->to.x, best->to.y, best->to.z, best->gap,
                                      uint32(best->damage)));
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
        // [guild15] b317 Eraelor "route complete (335 yd, ends 0 yd short), the walk refused: IsWaitingForLastMove":
        // a new destination replaces the last walk; the old walk's clock no longer holds the new one (lead244)
        LastMovement& last = AI_VALUE(LastMovement&, "last movement");
        last.lastdelayTime = 0.0f;
        last.msTime = 0;
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

    // (after the stuck check above: a bot that cannot get on still gives the destination up)
    // [guild15] perf (b317 pmon: "new rpg do profession" 31 ms per call, max 1.4 s; "new rpg do quest" max 1.1 s):
    // the route search that found no walk from this spot is not asked again from the same spot - only after the bot
    // stands elsewhere (its random step, a fight, a fall) or the destination changes
    if (botAI->rpgInfo.moveFarFailed &&
        bot->GetExactDist(botAI->rpgInfo.failedFromX, botAI->rpgInfo.failedFromY, botAI->rpgInfo.failedFromZ) < 3.0f)
        return false;
    botAI->rpgInfo.moveFarFailed = false;
    float dis = bot->GetExactDist(dest);
    if (dis < pathFinderDis)
    {
        if (MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false, false,
                   false, true))
            return true;
        if (!s_moveRefusal.empty() && s_moveRefusal.find("IsWaitingForLastMove") == std::string::npos &&
            s_moveRefusal.find("IsDuplicateMove") == std::string::npos)
            NoteWalkFailure(dest, "near walk refused: " + s_moveRefusal);  // [guild13]
        return false;
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
    // [guild15] perf (b319 pmon: the leader route search was 78 % of all bot AI time, 4.9 ms a search, mostly the
    // far walks' re-plan of the whole way after every leg): on a complete route found before, the next leg is
    // searched only as far as two sights ahead along it - the mobs priced are the ones in sight either way
    if (SoloSafety::Applies(bot) && botAI->rpgInfo.farRoute.size() >= 2)
    {
        std::vector<G3D::Vector3> const& route = botAI->rpgInfo.farRoute;
        G3D::Vector3 const at(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        size_t nearest = route.size();
        float best = 8.0f * 8.0f;
        for (size_t i = 0; i < route.size(); ++i)
            if (float const d = (route[i] - at).squaredLength(); d < best)
            {
                best = d;
                nearest = i;
            }
        float const ahead = 2.0f * sPlayerbotAIConfig.SightDistance;
        if (nearest < route.size())
        {
            Movement::PointsArray rest(route.begin() + nearest, route.end());
            float restLength = 0.0f;
            for (size_t i = 1; i < rest.size(); ++i)
                restLength += (rest[i] - rest[i - 1]).length();
            if (restLength > ahead)
            {
                G3D::Vector3 const target = Leader::Travel::PointOnPath(rest, ahead);
                BotPath const local = FindBotPath(bot, target.x, target.y, target.z);
                if (local.type == PATHFIND_NORMAL && (local.actualEnd - target).length() < 5.0f &&
                    local.points.size() >= 2)
                {
                    G3D::Vector3 leg = local.length > sPlayerbotAIConfig.SightDistance
                                           ? Leader::Travel::PointOnPath(local.points, sPlayerbotAIConfig.SightDistance)
                                           : local.actualEnd;
                    if (MoveTo(bot->GetMapId(), leg.x, leg.y, leg.z, false, false, false, true))
                        return true;
                }
            }
        }
        botAI->rpgInfo.farRoute.clear();  // off the route, at its end, or the local leg failed: the whole way again
    }
    {
        BotPath const path =  // [lead109]
            FindBotPath(bot, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        if (path.type == PATHFIND_NORMAL && path.leaderRoute)  // [guild15] kept for the next legs
            botAI->rpgInfo.farRoute.assign(path.points.begin(), path.points.end());
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
                // [guild13] a solo bot commits the walk only as far as it sees: the idle mobs beyond are priced when
                // it gets there (the route search penalties are of the mobs in sight) - b299: 13 of 14 deaths were
                // mobs met on a walk planned through a camp the bot had not seen yet
                G3D::Vector3 leg = endPos;
                if (SoloSafety::Applies(bot) && path.length > sPlayerbotAIConfig.SightDistance)
                    leg = Leader::Travel::PointOnPath(path.points, sPlayerbotAIConfig.SightDistance);
                if (MoveTo(bot->GetMapId(), leg.x, leg.y, leg.z, false, false, false, true))
                    return true;
                NoteWalkFailure(dest, Acore::StringFormat("route {} ({:.0f} yd, ends {:.0f} yd short), the walk "
                                                          "refused: {}",
                                                          type == PATHFIND_NORMAL ? "complete" : "partial", path.length,
                                                          endDistToDest, s_moveRefusal));
                return false;
            }
            // [guild13] the search from where the bot stands finds no way out (a navmesh island: Aldrassil's upper
            // floor, the Exodar's halls): the same search from the destination back to the bot; a complete one is
            // walked the other way
            // the core's own path search first (its start polygon from the bot's small box, not the route's tall
            // column, which can take the roof over a floor)
            if (WalkOffIsland(dest, disToDest))  // [guild14]
                return true;
            NoteWalkFailure(dest, Acore::StringFormat("route {} ends {:.0f} yd short, no nearer than the bot",
                                                      type == PATHFIND_NORMAL ? "complete" : "partial",
                                                      endDistToDest));
            NoteFarSearchFailed();  // [guild15]
        }
        else
        {
            NoteWalkFailure(dest, Acore::StringFormat("no route (type {:#x})", uint32(type)));
            NoteFarSearchFailed();  // [guild15]
            // [guild14] the bot stands off the navmesh (PathGenerator's shortcut, NOT_USING_PATH: b303 Manue L7 on a
            // ledge over Elwynn, 20 STUCK alerts): the same ways off the island
            if (WalkOffIsland(dest, disToDest))
                return true;
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
    // [guild50] the global leader's pick for the guild (itself, a member through the bank, the wanted list, sale)
    if (int32 const guild = sGuildCoordinator.RewardChoice(bot, quest); guild >= 0)
        return uint32(guild);
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
    if (IsSpellUnlockClassQuest(quest))  // [guild13]
        return false;

    bool highLevelQuest = bot->GetLevel() + QUEST_MAX_LEVELS_ABOVE_BOT < bot->GetQuestLevel(quest);
    if (highLevelQuest)
        return false;

    // Dungeon, raid, PvP and the other special quest types stay out.
    if (quest->GetType() != 0 && quest->GetType() != QUEST_TYPE_ELITE)
        return false;

    // [guild14] a quest ended only at a script-spawned object is not taken (the bot does not run the script)
    if (EnderNeedsScript(quest->GetQuestId()))
        return false;

    // [guild12] a quest it failed once (timed, escort) is not taken again
    if (HasFailedQuest(bot, quest->GetQuestId()))
        return false;

    // [guild12] a timed quest only when the walk to its ender at the bot's run speed fits its time
    if (quest->GetTimeAllowed() && !TimedQuestFits(quest))
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

        // [guild90] spawned ones only: a used (despawned) nearest one hid the rest
        GameObject* go = bot->FindNearestGameObject(static_cast<uint32>(-required), QUEST_OBJECT_RANGE, true);
        if (go && go->isSpawned() && !go->HasGameObjectFlag(GO_FLAG_IN_USE) &&
            (!nearest || bot->GetDistance(go) < bot->GetDistance(nearest)))
            nearest = go;
    }

    if (!nearest)
    {
        // [guild12] an object that drops an item the quest still needs (herb, mushroom, crate): walk up, the loot
        // pipeline ("can loot" -> open loot -> store loot) opens and loots it
        GameObject* lootable = FindNeededQuestObject(bot);
        if (!lootable)
            return false;
        LootObject loot(bot, lootable->GetGUID());
        if (!loot.IsAtInteractDistance(bot))
        {
            // [guild15] the object inside the reach of a pack it does not fight (b326 Orenem L11: 4 Bloodmyst
            // Hatchlings at a Sand Pear; DangerOnWay looks at red / orange mobs on the line only)
            if (DestinationRefused(WorldPosition(lootable)))
            {
                LogQuestObject(bot, lootable, "skipped: it lies among mobs it does not pull");
                return false;
            }
            LogQuestObject(bot, lootable, "used: walks up to it");
            if (MoveWorldObjectTo(lootable->GetGUID(), 1.5f))
                return true;
            LogQuestObject(bot, lootable, "skipped: the walk to it was not started (the last walk is not done)");
            return false;
        }
        if (bot->isMoving())
            bot->StopMoving();
        LogQuestObject(bot, lootable, "used: loots it");
        return AI_VALUE(LootObjectStack*, "available loot")->Add(lootable->GetGUID());
    }

    // the Dalaran crystal teleports only its user: the group goes down with the bot
    if (nearest->GetEntry() == DalaranTravel::GO_CRYSTAL_DOWN)
        return UseDalaranCrystal(DalaranTravel::GO_CRYSTAL_DOWN);

    if (!nearest->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
    {
        if (DestinationRefused(WorldPosition(nearest)))  // [guild15] as above
        {
            LogQuestObject(bot, nearest, "skipped: it lies among mobs it does not pull");
            return false;
        }
        MoveWorldObjectTo(nearest->GetGUID(), 1.5f);
        return true;
    }

    if (bot->isMoving())
        bot->StopMoving();

    LogQuestObject(bot, nearest, "used: q" + std::to_string(questId) + " objective");
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
    if (MoveWorldObjectTo(npcOrGo))
        return true;
    // [guild90] the walk did not start: still walking the last one (the move is busy, the tick is taken), or no walk
    // reaches the giver (b317 Eraelor at Tallonkai Swiftroot on Dolanaar's upper floor, Emdyl / Thamesh at Marleth
    // Barleybrew: "attack anything" off for the giver and nothing else ran, STUCK until the one-minute give-up)
    if (bot->isMoving())
        return true;
    BotPath const path = FindBotPath(bot, object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
    float const shortBy = (path.type & PATHFIND_NOPATH)
                              ? object->GetExactDist(bot)
                              : object->GetExactDist(path.actualEnd.x, path.actualEnd.y, path.actualEnd.z);
    if ((path.type & PATHFIND_NOPATH) || shortBy > INTERACTION_DISTANCE)
    {
        LOG_INFO("playerbots", "[rpg] {} L{} SKIP quest giver {} reason: no walk reaches it (route ends {:.0f} yd short)",
                 bot->GetName(), bot->GetLevel(), object->GetName(), shortBy);
        info.ignoredQuestGivers.insert(npcOrGo);
        info.questGiver.Clear();
        // [guild15] its quests wait for the next level: the planner picks starters by spot, not by this guid (b321
        // Eraelor / Lyleria / Alnia took q932 and q2438 from Tallonkai Swiftroot on Dolanaar's upper floor again
        // after every skip: 10 STUCK at (9925,980))
        QuestRelationBounds const starts = object->IsCreature()
                                               ? sObjectMgr->GetCreatureQuestRelationBounds(object->GetEntry())
                                               : sObjectMgr->GetGOQuestRelationBounds(object->GetEntry());
        QuestRelationBounds const ends = object->IsCreature()
                                             ? sObjectMgr->GetCreatureQuestInvolvedRelationBounds(object->GetEntry())
                                             : sObjectMgr->GetGOQuestInvolvedRelationBounds(object->GetEntry());
        for (QuestRelationBounds const& bounds : {starts, ends})
            for (auto itr = bounds.first; itr != bounds.second; ++itr)
                if (bot->GetQuestStatus(itr->second) == QUEST_STATUS_NONE ||
                    bot->GetQuestStatus(itr->second) == QUEST_STATUS_COMPLETE)
                    botAI->lowPriorityQuest.insert(itr->second);
    }
    return false;
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
        uint32 respawn = 0;  // [guild90] the spawn's respawn time (spawntimesecs), 0 for enders and triggers
    };

    struct MobSpawn  // [guild12]
    {
        float x, y, z;  // [guild15] z: the core's aggro check is a 3D distance (Sewer Beast under Stormwind's bridges)
        CreatureTemplate const* tmpl;
        int16 event;  // [guild15] game_event_creature: > 0 spawned only while that event runs, < 0 only while it does not
    };

    // [guild15] spawn guid -> its game event (GameEventMgr's own lists: index = events + event - 1; > 0 spawned only
    // while that event runs, < 0 only while it does not), creatures and objects; built once at startup
    struct SpawnEventIndex
    {
        std::unordered_map<ObjectGuid::LowType, int16> creatures;
        std::unordered_map<ObjectGuid::LowType, int16> objects;
    };
    SpawnEventIndex const& SpawnEvents()
    {
        static SpawnEventIndex index;
        static std::once_flag built;
        std::call_once(built, []()
        {
            int32 const events = int32(sGameEventMgr->GetEventMap().size());
            auto const read = [events](auto const& lists, std::unordered_map<ObjectGuid::LowType, int16>& out)
            {
                for (size_t i = 0; i < lists.size(); ++i)
                    for (ObjectGuid::LowType const guid : lists[i])
                        out[guid] = int16(int32(i) - events + 1);
            };
            read(sGameEventMgr->GameEventCreatureGuids, index.creatures);
            read(sGameEventMgr->GameEventGameobjectGuids, index.objects);
        });
        return index;
    }
    int16 SpawnEvent(bool creature, ObjectGuid::LowType guid)
    {
        auto const& map = creature ? SpawnEvents().creatures : SpawnEvents().objects;
        auto const found = map.find(guid);
        return found == map.end() ? int16(0) : found->second;
    }
    // [guild15] a spawn of that event stands in the world now (map threads read the active set while the world
    // thread's event update does not run: events start and stop in World::Update, not during the map updates)
    bool EventSpawnPresent(int16 event)
    {
        if (!event)
            return true;
        bool const active = sGameEventMgr->IsActiveEvent(uint16(std::abs(int32(event))));
        return event > 0 ? active : !active;
    }
    // b321: the Scourge Invasion's Herald of the Lich King L80, event 17 not running, refused 21 trips and postponed
    // quests
    bool SpawnPresent(MobSpawn const& spawn) { return EventSpawnPresent(spawn.event); }

    // [guild12] the danger bins: map + cell of MAX_AGGRO_RADIUS (no aggro reaches past one cell)
    uint64 MobCellKey(uint32 mapId, float x, float y)
    {
        uint32 const cx = uint32(int32(std::floor(x / MAX_AGGRO_RADIUS)) + 0x8000) & 0xFFFF;
        uint32 const cy = uint32(int32(std::floor(y / MAX_AGGRO_RADIUS)) + 0x8000) & 0xFFFF;
        return (uint64(mapId) << 32) | (cx << 16) | cy;
    }

    struct QuestSpawnIndex
    {
        std::unordered_map<uint32, std::vector<QuestSpot>> enders;    // quest id -> ender spawns
        // [guild12] item objectives: quest item -> the object / creature entries that drop it
        std::unordered_map<uint32, std::vector<uint32>> itemObjects;
        std::unordered_map<uint32, std::vector<uint32>> itemCreatures;
        // [guild12] the spawns of creatures that can be attacked, binned per map (MobCellKey)
        std::unordered_map<uint64, std::vector<MobSpawn>> mobCells;
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
                index.creatures[data.id].push_back({data.mapid, data.posX, data.posY, data.posZ, data.spawntimesecs});
            for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
                index.objects[data.id].push_back({data.mapid, data.posX, data.posY, data.posZ,
                                                  uint32(std::max<int32>(0, data.spawntimesecs))});

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

            // [guild12] who drops each quest item: gameobject_loot_template -> gameobject_template (loot id, data1
            // of a chest) and gameobject_questitem; creature_loot_template -> creature_template.lootid
            std::unordered_set<uint32> questItems;
            for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (quest->RequiredItemId[i])
                        questItems.insert(quest->RequiredItemId[i]);

            std::unordered_map<uint32, std::vector<uint32>> objectsByLoot;
            for (auto const& [entry, goTemplate] : *sObjectMgr->GetGameObjectTemplates())
            {
                if (uint32 const lootId = goTemplate.GetLootId())
                    objectsByLoot[lootId].push_back(entry);
                if (GameObjectQuestItemList const* items = sObjectMgr->GetGameObjectQuestItemList(entry))
                    for (uint32 const item : *items)
                        if (questItems.count(item))
                            index.itemObjects[item].push_back(entry);
            }
            std::unordered_map<uint32, std::vector<uint32>> creaturesByLoot;
            for (auto const& [entry, creatureTemplate] : *sObjectMgr->GetCreatureTemplates())
                if (creatureTemplate.lootid)
                    creaturesByLoot[creatureTemplate.lootid].push_back(entry);

            auto const readLoot = [&questItems](char const* query,
                                                std::unordered_map<uint32, std::vector<uint32>> const& byLoot,
                                                std::unordered_map<uint32, std::vector<uint32>>& out)
            {
                QueryResult result = WorldDatabase.Query(query);
                if (!result)
                    return;
                do
                {
                    Field* fields = result->Fetch();
                    uint32 const item = fields[1].Get<uint32>();
                    if (!questItems.count(item))
                        continue;
                    auto const sources = byLoot.find(fields[0].Get<uint32>());
                    if (sources != byLoot.end())
                        out[item].insert(out[item].end(), sources->second.begin(), sources->second.end());
                } while (result->NextRow());
            };
            readLoot("SELECT Entry, Item FROM gameobject_loot_template WHERE Reference = 0", objectsByLoot,
                     index.itemObjects);
            readLoot("SELECT Entry, Item FROM creature_loot_template WHERE Reference = 0", creaturesByLoot,
                     index.itemCreatures);
            for (auto* sources : {&index.itemObjects, &index.itemCreatures})
                for (auto& [item, entries] : *sources)
                {
                    std::sort(entries.begin(), entries.end());
                    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
                }

            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
                if (!tmpl || tmpl->type == CREATURE_TYPE_CRITTER ||
                    (tmpl->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE)))
                    continue;
                index.mobCells[MobCellKey(data.mapid, data.posX, data.posY)].push_back(
                    {data.posX, data.posY, data.posZ, tmpl, SpawnEvent(true, spawnId)});
            }
        });
        return index;
    }

    // [guild90] why the bot last left or skipped a quest, and the quest it works on until it is done or postponed
    std::mutex questNoteLock;
    std::unordered_map<uint32, uint32> zoneTarget;  // [guild15] bot -> the zone of its last [zone] line
    std::unordered_map<uint32, std::unordered_map<uint32, std::string>> postponeReasons;  // bot -> quest -> why
    std::unordered_map<uint32, uint32> committedQuest;                                    // bot -> quest
    std::unordered_map<uint32, std::string> choiceNotes;  // bot -> the last ChooseBestQuest's why
    std::unordered_map<uint32, std::set<uint32>> deferredHandIns;  // bot -> hand-ins logged as grouped
    constexpr float GROUP_ENDER_RADIUS = 50.0f;     // enders this close are one hand-in visit (one camp of NPCs)
    constexpr float HAND_IN_PASS_DISTANCE = 60.0f;  // a done quest's ender this near is handed in now
    // [guild90] the spot each bot works for a quest (quest -> bot -> map, x, y): other bots' spots count as taken
    struct Claim
    {
        uint32 mapId;
        float x, y;
    };
    std::unordered_map<uint32, std::unordered_map<uint32, Claim>> claims;
    std::unordered_map<uint32, uint32> claimOf;  // bot -> the quest of its claim
    // [guild90] the last resolve of a bot's objective spots (map thread local: one bot resolves at a time per thread)
    struct ResolveStats
    {
        size_t spawns = 0, spent = 0, taken = 0, offered = 0;
    };
    thread_local ResolveStats resolveStats;

    bool TakenByOther(Player* bot, uint32 questId, float x, float y)
    {
        std::lock_guard<std::mutex> guard(questNoteLock);
        auto const found = claims.find(questId);
        if (found == claims.end())
            return false;
        for (auto const& [other, claim] : found->second)
            if (other != bot->GetGUID().GetCounter() && claim.mapId == bot->GetMapId() &&
                (claim.x - x) * (claim.x - x) + (claim.y - y) * (claim.y - y) <= SPENT_RADIUS * SPENT_RADIUS)
                return true;
        return false;
    }

    // inside the quest_poi polygon of the objective, or within SPENT_RADIUS of its border
    bool InPolygon(std::vector<QuestPOIPoint> const& poly, float x, float y)
    {
        bool inside = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        {
            float const xi = poly[i].x, yi = poly[i].y, xj = poly[j].x, yj = poly[j].y;
            if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
                inside = !inside;
            float const dx = xj - xi, dy = yj - yi;
            float const len = dx * dx + dy * dy;
            float t = len > 0.0f ? ((x - xi) * dx + (y - yi) * dy) / len : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            if (std::hypot(xi + dx * t - x, yi + dy * t - y) <= SPENT_RADIUS)
                return true;
        }
        return inside;
    }

    // [guild90] every spawn of the sources of one objective: the objective creature / object itself (kill, use) or
    // the creatures and objects that drop its item (loot tables, gameobject_questitem). Pointers into the index.
    void ObjectiveSources(Quest const* quest, uint32 objective, std::vector<QuestSpot const*>& out)
    {
        QuestSpawnIndex const& index = GetQuestSpawnIndex();
        auto const addEntry = [&out](std::unordered_map<uint32, std::vector<QuestSpot>> const& spawns, uint32 entry)
        {
            auto const found = spawns.find(entry);
            if (found != spawns.end())
                for (QuestSpot const& spot : found->second)
                    out.push_back(&spot);
        };
        if (objective < QUEST_OBJECTIVES_COUNT)
        {
            int32 const npcOrGo = quest->RequiredNpcOrGo[objective];
            if (npcOrGo > 0)
                addEntry(index.creatures, uint32(npcOrGo));
            else if (npcOrGo < 0)
                addEntry(index.objects, uint32(-npcOrGo));
            return;
        }
        if (objective >= QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
            return;
        uint32 const item = quest->RequiredItemId[objective - QUEST_OBJECTIVES_COUNT];
        if (!item)
            return;
        auto const objects = index.itemObjects.find(item);
        if (objects != index.itemObjects.end())
            for (uint32 const entry : objects->second)
                addEntry(index.objects, entry);
        auto const creatures = index.itemCreatures.find(item);
        if (creatures != index.itemCreatures.end())
            for (uint32 const entry : creatures->second)
                addEntry(index.creatures, entry);
    }

    uint32 ObjectiveRespawnNear(Player* bot, uint32 questId, float x, float y)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            return 0;
        uint32 best = 0;
        std::vector<QuestSpot const*> spots;
        for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT; ++objective)
            ObjectiveSources(quest, objective, spots);
        for (QuestSpot const* spot : spots)
            if (spot->mapId == bot->GetMapId() && spot->respawn &&
                (spot->x - x) * (spot->x - x) + (spot->y - y) * (spot->y - y) <= SPENT_RADIUS * SPENT_RADIUS &&
                (!best || spot->respawn < best))
                best = spot->respawn;
        return best;
    }

    // [guild90] up to this many objective spots per objective go to the walk check, nearest first
    constexpr size_t MAX_SPOT_CANDIDATES = 4;
    // [guild12] the spots of `spots` on the bot's map whose way is free of orange / red mobs, as POIs, nearest first;
    // [guild90] the spent ones are skipped (IsSpentSpot: seen empty, until their respawn) and up to
    // MAX_SPOT_CANDIDATES camps are taken (b317 Eraelor q919: 33 Timberling Sprout spawns, one spot, spent, POSTPONE).
    // 0: none on this map, 1: found, 2: every unspent spot on this map has such a mob (`blocker`), 3: all spent
    int NearestSafeSpot(Player* bot, uint32 questId, std::vector<QuestSpot const*> const& spots, int32 objectiveIdx,
                        std::vector<POIInfo>& poiInfo, CreatureTemplate const*& blocker)
    {
        std::vector<std::pair<float, QuestSpot const*>> onMap;
        for (QuestSpot const* spot : spots)
            if (spot->mapId == bot->GetMapId())
                onMap.push_back({bot->GetDistance2d(spot->x, spot->y), spot});
        if (onMap.empty())
            return 0;
        std::sort(onMap.begin(), onMap.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        size_t const first = poiInfo.size();
        size_t spent = 0;
        bool blocked = false;
        for (auto const& [distance, spot] : onMap)
        {
            ++resolveStats.spawns;
            if (NewRpgBaseAction::IsSpentSpot(bot, questId, spot->x, spot->y))
            {
                ++spent;
                ++resolveStats.spent;
                continue;
            }
            // [guild90] another bot works that camp for the quest: taken, like a player sees it
            if (TakenByOther(bot, questId, spot->x, spot->y))
            {
                ++spent;
                ++resolveStats.taken;
                continue;
            }
            // a spawn next to a spot already taken adds nothing (one camp)
            bool sameCamp = false;
            for (size_t i = first; i < poiInfo.size() && !sameCamp; ++i)
                sameCamp = std::hypot(poiInfo[i].pos.x - spot->x, poiInfo[i].pos.y - spot->y) < SPENT_RADIUS;
            if (sameCamp)
                continue;
            if (CreatureTemplate const* danger =
                    NewRpgBaseAction::DangerOnWay(bot, bot->GetPositionX(), bot->GetPositionY(), spot->x, spot->y))
            {
                blocker = danger;
                blocked = true;
                continue;
            }
            poiInfo.push_back({{spot->x, spot->y}, objectiveIdx, spot->z != 0.0f ? spot->z : INVALID_HEIGHT});
            ++resolveStats.offered;
            if (poiInfo.size() - first >= MAX_SPOT_CANDIDATES)
                break;
        }
        if (poiInfo.size() > first)
            return 1;
        return blocked || !spent ? 2 : 3;
    }

    int NearestSafeSpot(Player* bot, uint32 questId, std::vector<QuestSpot> const& spots, int32 objectiveIdx,
                        std::vector<POIInfo>& poiInfo, CreatureTemplate const*& blocker)
    {
        std::vector<QuestSpot const*> pointers;
        pointers.reserve(spots.size());
        for (QuestSpot const& spot : spots)
            pointers.push_back(&spot);
        return NearestSafeSpot(bot, questId, pointers, objectiveIdx, poiInfo, blocker);
    }


    // [guild12] one log line per bot, quest and level for a postponed quest
    void LogPostponed(Player* bot, Quest const* quest, std::string const& reason)
    {
        static std::mutex lock;
        static std::set<std::tuple<uint32, uint32, uint8>> logged;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (!logged.insert({bot->GetGUID().GetCounter(), quest->GetQuestId(), bot->GetLevel()}).second)
                return;
        }
        LOG_INFO("playerbots", "[rpg] {} L{} POSTPONE q{} {} reason: {}", bot->GetName(), bot->GetLevel(),
                 quest->GetQuestId(), quest->GetTitle(), reason);
    }

    // [guild90] a postponed quest holds until the next level (the planner's lowPriorityQuest, cleared on a level-up):
    // b317 Eraelor L6 postponed q919 for Gnarlpine Augur L9 and took it again at L6 from another spot
    void HoldPostponed(Player* bot, Quest const* quest, std::string const& reason)
    {
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            botAI->lowPriorityQuest.insert(quest->GetQuestId());
        NewRpgBaseAction::NotePostpone(bot, quest->GetQuestId(), reason);
        LogPostponed(bot, quest, reason);
    }

    // [guild12] where a bot died: the killer's spot and the death spot, the killer's level and aggro radius; the
    // bot keeps away until it outlevels the killer
    struct DeathZone
    {
        uint32 mapId;
        float killerX, killerY, deathX, deathY;
        uint32 entry;
        uint8 level;
        float radius;
        bool refused = false;  // [guild15] a refused pull's zone, not a death's
    };
    std::mutex deathZonesLock;
    std::unordered_map<uint32, std::vector<DeathZone>> deathZones;

    float SegmentDistSq(float px, float py, float fromX, float fromY, float toX, float toY)
    {
        float const dx = toX - fromX;
        float const dy = toY - fromY;
        float const lengthSq = dx * dx + dy * dy;
        float t = lengthSq > 0.0f ? ((px - fromX) * dx + (py - fromY) * dy) / lengthSq : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        float const ex = fromX + dx * t - px;
        float const ey = fromY + dy * t - py;
        return ex * ex + ey * ey;
    }

    std::mutex failedQuestsLock;  // [guild12] bot guid -> quests it failed (timed, escort): not taken again
    std::unordered_map<uint32, std::unordered_set<uint32>> failedQuests;
}

CreatureTemplate const* NewRpgBaseAction::DangerOnWay(Player* bot, float fromX, float fromY, float toX, float toY,
                                                      float fromZ, float toZ)
{
    QuestSpawnIndex const& index = GetQuestSpawnIndex();
    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    if (!botFaction)
        return nullptr;
    uint8 const level = bot->GetLevel();
    uint32 const mapId = bot->GetMapId();
    float const dx = toX - fromX;
    float const dy = toY - fromY;
    float const length = std::sqrt(dx * dx + dy * dy);
    bool const heights = fromZ != NO_WAY_HEIGHT && toZ != NO_WAY_HEIGHT;
    // [guild15] the walk's point nearest to (px, py): its share t along the walk; t == 0 = the start
    auto const nearestShare = [&](float px, float py)
    {
        float const t = length > 0.0f ? ((px - fromX) * dx + (py - fromY) * dy) / (length * length) : 0.0f;
        return std::clamp(t, 0.0f, 1.0f);
    };
    auto const gapSq = [&](float px, float py, float pz, float t)
    {
        float const ex = fromX + dx * t - px;
        float const ey = fromY + dy * t - py;
        float ez = 0.0f;
        if (heights && pz != NO_WAY_HEIGHT)
            ez = fromZ + (toZ - fromZ) * t - pz;
        return ex * ex + ey * ey + ez * ez;
    };

    // where it died, until it outlevels the killer
    {
        std::lock_guard<std::mutex> guard(deathZonesLock);
        auto const zones = deathZones.find(bot->GetGUID().GetCounter());
        if (zones != deathZones.end())
        {
            auto& list = zones->second;
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [level](DeathZone const& zone) { return level > zone.level; }),
                       list.end());
            for (DeathZone const& zone : list)
            {
                if (zone.mapId != mapId)
                    continue;
                float const radiusSq = zone.radius * zone.radius;
                for (auto const& [px, py] : {std::pair{zone.killerX, zone.killerY}, std::pair{zone.deathX, zone.deathY}})
                {
                    float const t = nearestShare(px, py);
                    if (t > 0.0f && gapSq(px, py, NO_WAY_HEIGHT, t) <= radiusSq)
                        if (CreatureTemplate const* killer = sObjectMgr->GetCreatureTemplate(zone.entry))
                            return killer;
                }
            }
        }
    }

    // the cells along the walk and their neighbours
    uint32 const steps = uint32(std::ceil(length / MAX_AGGRO_RADIUS));
    std::unordered_set<uint64> cells;
    for (uint32 step = 0; step <= steps; ++step)
    {
        float const t = steps ? float(step) / steps : 0.0f;
        for (int32 ox = -1; ox <= 1; ++ox)
            for (int32 oy = -1; oy <= 1; ++oy)
                cells.insert(MobCellKey(mapId, fromX + dx * t + ox * MAX_AGGRO_RADIUS,
                                        fromY + dy * t + oy * MAX_AGGRO_RADIUS));
    }

    for (uint64 const cell : cells)
    {
        auto const spawns = index.mobCells.find(cell);
        if (spawns == index.mobCells.end())
            continue;
        for (MobSpawn const& spawn : spawns->second)
        {
            CreatureTemplate const* tmpl = spawn.tmpl;
            XPColorChar const color = Acore::XP::GetColorCode(level, tmpl->maxlevel);
            if (color != XP_RED && color != XP_ORANGE)
                continue;
            if (!SpawnPresent(spawn))
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(tmpl->faction);
            if (!faction || !faction->IsHostileTo(*botFaction))
                continue;
            // Creature::GetAggroRange: the detection range grows a yard per level the mob is above the player
            float const aggro = std::clamp(tmpl->detection_range + float(int32(tmpl->maxlevel) - int32(level)),
                                           5.0f, MAX_AGGRO_RADIUS);
            float const t = nearestShare(spawn.x, spawn.y);
            if (t <= 0.0f)
                continue;
            float const flatSq = gapSq(spawn.x, spawn.y, NO_WAY_HEIGHT, t);
            if (flatSq > aggro * aggro)
                continue;
            // [guild15] the core's aggro (Creature::CanStartAttack): a mob that cannot fly starts no attack on a
            // player more than CREATURE_Z_ATTACK_RANGE above or below it, and the range is a 3D distance. Looked at
            // over the stretch of the walk inside its aggro circle (its ends and the nearest point): b321-b322 the
            // Sewer Beasts L50 at the bottom of Stormwind's canals (z 86, streets 96-100) refused 32 vendor trips
            if (heights && !tmpl->Movement.IsFlightAllowed() && length > 0.0f)
            {
                float const half = std::sqrt(std::max(0.0f, aggro * aggro - flatSq)) / length;
                float const lo = std::max(0.0f, t - half);
                float const hi = std::min(1.0f, t + half);
                // the walk's height is linear on the leg: its nearest height to the mob is at an end of the stretch
                // or where it crosses the mob's height
                float const cross = toZ != fromZ ? std::clamp((spawn.z - fromZ) / (toZ - fromZ), lo, hi) : t;
                bool reaches = false;
                for (float const at : {lo, t, hi, cross})
                {
                    float const walkZ = fromZ + (toZ - fromZ) * at;
                    if (std::fabs(walkZ - spawn.z) <= CREATURE_Z_ATTACK_RANGE &&
                        gapSq(spawn.x, spawn.y, spawn.z, at) <= aggro * aggro)
                        reaches = true;
                }
                if (!reaches)
                    continue;
            }
            return tmpl;
        }
    }
    return nullptr;
}

CreatureTemplate const* NewRpgBaseAction::TripDanger(Player* bot, float x, float y, float z)
{
    PMON_SCOPE(tripScope, PERF_MON_VALUE, "solo trip danger");  // [guild15]
    if (!bot || !SoloSafety::Applies(bot))
        return nullptr;
    if (CreatureTemplate const* zone = InDangerZone(bot, x, y))
        return zone;
    // [guild15] a destination on another landmass of this map (Stormwind from Ironforge: the Deeprun Tram): the walk
    // to the route's first ride / portal and the walk from its last one, not a straight line across the continent
    if (TravelRouteMgr::Landmass(WorldPosition(bot)) !=
        TravelRouteMgr::Landmass(WorldPosition(bot->GetMapId(), x, y, z)))
    {
        TravelRouteMgr::Leg leg;
        float seconds = 0.0f;
        WorldPosition exit;
        if (!sTravelRouteMgr.FindRoute(bot, WorldPosition(bot->GetMapId(), x, y, z), leg, seconds, &exit))
            return nullptr;  // no way there at all: the walk itself fails and gives it up, no danger to name
        WorldPosition start;
        if (leg.kind == TravelRouteMgr::LegKind::Portal && leg.portalIndex < sTravelRouteMgr.GetPortals().size())
            start = sTravelRouteMgr.GetPortals()[leg.portalIndex].from;
        else if (leg.kind == TravelRouteMgr::LegKind::Transport)
            if (TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(leg.lineEntry))
                if (leg.fromDock < line->docks.size())
                    start = line->docks[leg.fromDock].pier;
        auto const walkDanger = [bot](G3D::Vector3 const& from, G3D::Vector3 const& to) -> CreatureTemplate const*
        {
            Leader::RouteResult const route = Leader::Route(bot, from, to);
            float lastX = from.x, lastY = from.y, lastZ = from.z;
            if (route.found && route.points.size() >= 2)
            {
                for (size_t i = 1; i < route.points.size(); ++i)
                    if (CreatureTemplate const* danger =
                            DangerOnWay(bot, route.points[i - 1].x, route.points[i - 1].y, route.points[i].x,
                                        route.points[i].y, route.points[i - 1].z, route.points[i].z))
                        return danger;
                if (route.complete)
                    return nullptr;
                lastX = route.points.back().x;
                lastY = route.points.back().y;
                lastZ = route.points.back().z;
            }
            return DangerOnWay(bot, lastX, lastY, to.x, to.y, lastZ, to.z);
        };
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (start.GetMapId() == bot->GetMapId() && start != WorldPosition())
            if (CreatureTemplate const* danger =
                    walkDanger(here, G3D::Vector3(start.GetPositionX(), start.GetPositionY(), start.GetPositionZ())))
                return danger;
        if (exit.GetMapId() == bot->GetMapId() && exit != WorldPosition(bot))
            return walkDanger(G3D::Vector3(exit.GetPositionX(), exit.GetPositionY(), exit.GetPositionZ()),
                              G3D::Vector3(x, y, z));
        return nullptr;
    }
    // the walk it would take: b303 Thokhen L3 died to a Root Thresher L8 on its 919 yd vendor walk, Laaral L10 to
    // Mutated Constrictors L15 on gather and bank walks across Bloodmyst - mobs no quest check had looked at
    Leader::RouteResult const route = Leader::Route(bot, Position(x, y, z));
    float lastX = bot->GetPositionX();
    float lastY = bot->GetPositionY();
    if (route.found && route.points.size() >= 2)
    {
        for (size_t i = 1; i < route.points.size(); ++i)
            if (CreatureTemplate const* danger = DangerOnWay(bot, route.points[i - 1].x, route.points[i - 1].y,
                                                             route.points[i].x, route.points[i].y,
                                                             route.points[i - 1].z, route.points[i].z))
                return danger;
        if (route.complete)
            return nullptr;
        lastX = route.points.back().x;
        lastY = route.points.back().y;
    }
    return DangerOnWay(bot, lastX, lastY, x, y);
}

bool NewRpgBaseAction::TripRefused(Player* bot, float x, float y, float z, std::string const& what)
{
    if (!bot || !SoloSafety::Applies(bot))
        return false;
    if (InDangerZone(bot, x, y))
        return true;
    static std::mutex lock;
    // bot -> destination (map, x, y) -> (the level it was judged at, refused)
    static std::unordered_map<uint32, std::map<std::tuple<uint32, int32, int32>, std::pair<uint8, bool>>> judged;
    std::tuple<uint32, int32, int32> const key{bot->GetMapId(), int32(x), int32(y)};
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const& trips = judged[bot->GetGUID().GetCounter()];
        auto const found = trips.find(key);
        if (found != trips.end() && found->second.first == bot->GetLevel())
            return found->second.second;
    }
    CreatureTemplate const* danger = TripDanger(bot, x, y, z);
    if (danger)
        LOG_INFO("playerbots", "[solo] {} L{} TRIP_REFUSED {} at ({:.0f},{:.0f}) {:.0f} yd: its walk passes {} L{}-{}",
                 bot->GetName(), bot->GetLevel(), what, x, y, bot->GetExactDist(x, y, z), danger->Name,
                 danger->minlevel, danger->maxlevel);
    std::lock_guard<std::mutex> guard(lock);
    judged[bot->GetGUID().GetCounter()][key] = {bot->GetLevel(), danger != nullptr};
    return danger != nullptr;
}

void NewRpgBaseAction::RecordDeath(Player* bot, Creature* killer)
{
    if (!bot || !killer || !GET_PLAYERBOT_AI(bot))
        return;
    // the killer's aggro radius for this bot (Creature::GetAggroRange)
    float const radius = std::max(killer->GetAggroRange(bot), killer->GetCombatReach() + bot->GetCombatReach());
    {
        std::lock_guard<std::mutex> guard(deathZonesLock);
        deathZones[bot->GetGUID().GetCounter()].push_back({bot->GetMapId(), killer->GetPositionX(),
                                                           killer->GetPositionY(), bot->GetPositionX(),
                                                           bot->GetPositionY(), killer->GetEntry(),
                                                           uint8(killer->GetLevel()), radius});
    }
    LOG_INFO("playerbots", "[rpg] {} L{} DANGER remembered: {} L{} at ({:.0f},{:.0f}), {:.0f} yd, kept away until L{}",
             bot->GetName(), bot->GetLevel(), killer->GetName(), killer->GetLevel(), killer->GetPositionX(),
             killer->GetPositionY(), radius, killer->GetLevel() + 1);
    // the quest it was on goes back to the planner: its spots there are now judged dangerous
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        if (botAI->rpgInfo.GetStatus() == RPG_DO_QUEST)
            botAI->rpgInfo.ChangeToIdle();
    SoloSafety::OnDeath(bot);  // [guild13] back to the hub and rest to full after the revive
}

void NewRpgBaseAction::RememberDanger(Player* bot, float x, float y, float radius, uint32 entry, uint8 level)
{
    if (!bot)
        return;
    std::lock_guard<std::mutex> guard(deathZonesLock);
    auto& list = deathZones[bot->GetGUID().GetCounter()];
    for (DeathZone const& zone : list)
        if (zone.mapId == bot->GetMapId() && zone.entry == entry &&
            SegmentDistSq(zone.killerX, zone.killerY, x, y, x, y) <= zone.radius * zone.radius)
            return;
    list.push_back({bot->GetMapId(), x, y, x, y, entry, level, radius, true});
}

void NewRpgBaseAction::ForgetRefusals(Player* bot)
{
    if (!bot)
        return;
    std::lock_guard<std::mutex> guard(deathZonesLock);
    auto const found = deathZones.find(bot->GetGUID().GetCounter());
    if (found == deathZones.end())
        return;
    auto& list = found->second;
    size_t const before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [](DeathZone const& zone) { return zone.refused; }),
               list.end());
    if (before != list.size())
        LOG_INFO("playerbots", "[solo] {} L{} repaired: {} refused pulls judged again", bot->GetName(),
                 bot->GetLevel(), before - list.size());
}

CreatureTemplate const* NewRpgBaseAction::InDangerZone(Player* bot, float x, float y)
{
    if (!bot)
        return nullptr;
    uint8 const level = bot->GetLevel();
    std::lock_guard<std::mutex> guard(deathZonesLock);
    auto const zones = deathZones.find(bot->GetGUID().GetCounter());
    if (zones == deathZones.end())
        return nullptr;
    for (DeathZone const& zone : zones->second)
    {
        if (zone.mapId != bot->GetMapId() || level > zone.level)
            continue;
        float const radiusSq = zone.radius * zone.radius;
        if (SegmentDistSq(zone.killerX, zone.killerY, x, y, x, y) <= radiusSq ||
            SegmentDistSq(zone.deathX, zone.deathY, x, y, x, y) <= radiusSq)
            return sObjectMgr->GetCreatureTemplate(zone.entry);
    }
    return nullptr;
}

bool NewRpgBaseAction::EnderNeedsScript(uint32 questId)
{
    // quest id -> its enders are all objects the world keeps despawned (gameobject.spawntimesecs < 0: a script
    // spawns them - b303 q310 Bitter Rivals' Unguarded Thunder Ale Barrel appears only after Jarven Thunderbrew's
    // gossip; 8 bots stood 5 min each at its empty spot), no creature ends it
    static std::unordered_set<uint32> quests;
    static std::once_flag built;
    std::call_once(built, []()
    {
        std::unordered_map<uint32, bool> allHidden;  // quest -> every ender spawn script-spawned so far
        std::unordered_map<uint32, bool> hiddenEntry;  // object entry -> all its spawns despawned
        for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        {
            auto const found = hiddenEntry.find(data.id);
            bool const hidden = data.spawntimesecs < 0;
            if (found == hiddenEntry.end())
                hiddenEntry[data.id] = hidden;
            else
                found->second = found->second && hidden;
        }
        for (auto const& [entry, questId] : *sObjectMgr->GetGOQuestInvolvedRelationMap())
        {
            auto const found = hiddenEntry.find(entry);
            bool const hidden = found != hiddenEntry.end() && found->second;
            auto const quest = allHidden.find(questId);
            if (quest == allHidden.end())
                allHidden[questId] = hidden;
            else
                quest->second = quest->second && hidden;
        }
        for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
            allHidden[questId] = false;
        for (auto const& [questId, hidden] : allHidden)
            if (hidden)
                quests.insert(questId);
        LOG_INFO("playerbots", "[guild14] {} quests end only at script-spawned objects: not taken", quests.size());
    });
    return quests.count(questId) > 0;
}

bool NewRpgBaseAction::IsSpellUnlockClassQuest(Quest const* quest)
{
    if (!quest || !quest->GetRequiredClasses())
        return false;
    // the quest or the rest of its chain rewards a spell (RewardSpell cast / RewardDisplaySpell learned)
    std::unordered_set<uint32> seen;
    for (Quest const* step = quest; step && seen.insert(step->GetQuestId()).second;)
    {
        if (step->GetRewSpell() || step->GetRewSpellCast() > 0)
            return true;
        uint32 const next = step->GetNextQuestInChain() ? step->GetNextQuestInChain() : step->GetNextQuestId();
        step = next ? sObjectMgr->GetQuestTemplate(next) : nullptr;
    }
    return false;
}

bool NewRpgBaseAction::NearestEnderSpot(Player* bot, uint32 questId, WorldPosition& out)
{
    QuestSpawnIndex const& index = GetQuestSpawnIndex();
    auto const enders = index.enders.find(questId);
    if (enders == index.enders.end())
        return false;
    float best = std::numeric_limits<float>::max();
    for (QuestSpot const& spot : enders->second)
    {
        if (spot.mapId != bot->GetMapId())
            continue;
        float const d = bot->GetDistance2d(spot.x, spot.y);
        if (d < best)
        {
            best = d;
            out = WorldPosition(spot.mapId, spot.x, spot.y, spot.z);
        }
    }
    return best != std::numeric_limits<float>::max();
}

WorldPosition NewRpgBaseAction::NearestHub(Player* bot)
{
    WorldPosition best;
    float bestDist = std::numeric_limits<float>::max();
    QuestSpawnIndex const& index = GetQuestSpawnIndex();
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        auto const enders = questId ? index.enders.find(questId) : index.enders.end();
        if (enders == index.enders.end())
            continue;
        for (QuestSpot const& spot : enders->second)
        {
            if (spot.mapId != bot->GetMapId())
                continue;
            float const d = bot->GetDistance2d(spot.x, spot.y);
            if (d < bestDist && !DangerOnWay(bot, bot->GetPositionX(), bot->GetPositionY(), spot.x, spot.y))
            {
                bestDist = d;
                best = WorldPosition(spot.mapId, spot.x, spot.y, spot.z);
            }
        }
    }
    if (best != WorldPosition())
        return best;
    for (WorldLocation const& hub : sTravelMgr.GetTravelHubs(bot))
    {
        if (hub.GetMapId() != bot->GetMapId())
            continue;
        float const d = bot->GetExactDist(hub);
        if (d < bestDist)
        {
            bestDist = d;
            best = WorldPosition(hub);
        }
    }
    return best;
}

bool NewRpgBaseAction::TimedQuestFits(Quest const* quest) { return TimedQuestFits(bot, quest); }

bool NewRpgBaseAction::TimedQuestFits(Player* bot, Quest const* quest)
{
    QuestSpawnIndex const& spawnIndex = GetQuestSpawnIndex();
    auto const enders = spawnIndex.enders.find(quest->GetQuestId());
    float nearest = std::numeric_limits<float>::max();
    if (enders != spawnIndex.enders.end())
        for (QuestSpot const& spot : enders->second)
            if (spot.mapId == bot->GetMapId())
                nearest = std::min(nearest, bot->GetDistance2d(spot.x, spot.y));
    return nearest != std::numeric_limits<float>::max() &&
           nearest / bot->GetSpeed(MOVE_RUN) <= float(quest->GetTimeAllowed());
}

bool NewRpgBaseAction::HasFailedQuest(Player* bot, uint32 questId)
{
    std::lock_guard<std::mutex> guard(failedQuestsLock);
    auto const itr = failedQuests.find(bot->GetGUID().GetCounter());
    return itr != failedQuests.end() && itr->second.count(questId);
}

bool NewRpgBaseAction::AbandonFailedQuest(uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;
    {
        std::lock_guard<std::mutex> guard(failedQuestsLock);
        failedQuests[bot->GetGUID().GetCounter()].insert(questId);
    }
    std::string const kind =
        quest->GetTimeAllowed() ? "timed " + std::to_string(quest->GetTimeAllowed()) + " s" : "escort / event";
    LOG_INFO("playerbots", "[rpg] {} L{} ABANDON q{} {} reason: failed ({}), not taken again", bot->GetName(),
             bot->GetLevel(), questId, quest->GetTitle(), kind);
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        if (bot->GetQuestSlotQuestId(slot) != questId)
            continue;
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << slot;
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        botAI->RecordQuestDropped(questId, QuestDropReason::Failed);
        return true;
    }
    return false;
}

namespace
{
    // [guild90] one [guild12] quest-object line per bot, object and verdict (the scan runs every tick)
    std::mutex questObjectLogLock;
    std::unordered_map<uint32, std::unordered_map<ObjectGuid::LowType, std::string>> questObjectLogged;

    void LogQuestObject(Player* bot, GameObject* go, std::string const& verdict)
    {
        {
            std::lock_guard<std::mutex> guard(questObjectLogLock);
            auto& logged = questObjectLogged[bot->GetGUID().GetCounter()];
            if (logged.size() > 256)
                logged.clear();
            auto& last = logged[go->GetGUID().GetCounter()];
            if (last == verdict)
                return;
            last = verdict;
        }
        LOG_INFO("playerbots", "[guild12] {} L{} quest object {} ({:.0f} yd) {}", bot->GetName(), bot->GetLevel(),
                 go->GetName(), bot->GetDistance(go), verdict);
    }
}

GameObject* NewRpgBaseAction::FindNeededQuestObject(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return nullptr;
    QuestSpawnIndex const& index = GetQuestSpawnIndex();

    // the objects that drop an item a quest in the log still needs; none: no grid search at all
    std::vector<uint32> entries;
    for (auto const& [questId, status] : bot->getQuestStatusMap())
    {
        if (status.Status != QUEST_STATUS_INCOMPLETE)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 const item = quest->RequiredItemId[i];
            if (!item || status.ItemCount[i] >= quest->RequiredItemCount[i])
                continue;
            auto const sources = index.itemObjects.find(item);
            if (sources != index.itemObjects.end())
                entries.insert(entries.end(), sources->second.begin(), sources->second.end());
        }
    }
    if (entries.empty())
        return nullptr;
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());

    // [guild90] every object of those entries in range, not FindNearestGameObject's one nearest: the one just looted
    // (despawned, still the nearest) hid all the others (b317 Eraelor stood among Timberling Sprouts, "spot spent")
    std::list<GameObject*> objects;
    bot->GetGameObjectListWithEntryInGrid(objects, entries, QUEST_OBJECT_RANGE);
    LootObjectStack* lootStack = botAI->GetAiObjectContext()->GetValue<LootObjectStack*>("available loot")->Get();
    GameObject* nearest = nullptr;
    float nearestDist = std::numeric_limits<float>::max();
    for (GameObject* go : objects)
    {
        if (!go->isSpawned() || go->GetGoState() != GO_STATE_READY || go->HasGameObjectFlag(GO_FLAG_IN_USE))
            continue;
        float const d = bot->GetDistance(go);
        if (d >= nearestDist)
            continue;
        if (!bot->IsWithinLOSInMap(go))
            continue;  // out of sight: the scan sees it from another step
        LootObject loot(bot, go->GetGUID());
        if (loot.IsEmpty())
        {
            LogQuestObject(bot, go, "skipped: nothing in it the log needs");
            continue;
        }
        // [guild90] the height gap is judged by the walk, not here (IsLootPossible's 3.5 yd height rule is for the
        // object in reach: on Teldrassil's slopes it hid every sprout more than a step up or down)
        if (!loot.IsLootPossible(bot, false))
        {
            LogQuestObject(bot, go, "skipped: not lootable for it (lock, skill, flags)");
            continue;
        }
        if (lootStack && !lootStack->CanAttemptLoot(go->GetGUID()))
        {
            LogQuestObject(bot, go, "skipped: its last loot try failed, retried after the loot retry delay");
            continue;
        }
        if (CreatureTemplate const* danger =
                DangerOnWay(bot, bot->GetPositionX(), bot->GetPositionY(), go->GetPositionX(), go->GetPositionY()))
        {
            LogQuestObject(bot, go, "skipped: " + danger->Name + " on the way");
            continue;
        }
        nearest = go;
        nearestDist = d;
    }
    return nearest;
}

namespace
{
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

    // [guild12] a quest of red or orange con for the bot (the core's level rule) waits until it is closer
    int32 const questLevel = std::max<int32>(1, bot->GetQuestLevel(quest));
    XPColorChar const questColor = Acore::XP::GetColorCode(bot->GetLevel(), uint8(std::min<int32>(questLevel, 255)));
    if (questColor == XP_RED || questColor == XP_ORANGE)
    {
        HoldPostponed(bot, quest,
                      "quest L" + std::to_string(questLevel) + (questColor == XP_RED ? " (red)" : " (orange)"));
        return false;
    }

    // [guild12] item objectives go where their droppers stand (world DB loot tables): the nearest spot whose way
    // is free of orange / red mobs; the POI blend only for an item nobody on this map drops.
    // [guild90] kill / use objectives likewise by the spawns of their creature / object, and every unspent camp
    // (up to MAX_SPOT_CANDIDATES per objective), not the single nearest spawn
    CreatureTemplate const* blocker = nullptr;
    std::set<int32> resolved;
    size_t spentObjectives = 0;
    resolveStats = ResolveStats();
    for (uint32 objective : incompleteObjectiveIdx)
    {
        std::vector<QuestSpot const*> spots;
        ObjectiveSources(quest, objective, spots);
        // [guild90] the objective's area: its spawns inside the quest_poi polygon when the quest has one (spawns of
        // the same mob far off are other quests' camps); none inside: all spawns
        for (QuestPOI const& qPoi : *poiVector)
        {
            if (qPoi.MapId != bot->GetMapId() || qPoi.ObjectiveIndex != int32(objective) || qPoi.points.size() < 3)
                continue;
            std::vector<QuestSpot const*> inside;
            for (QuestSpot const* spot : spots)
                if (spot->mapId != bot->GetMapId() || InPolygon(qPoi.points, spot->x, spot->y))
                    inside.push_back(spot);
            if (!inside.empty())
                spots.swap(inside);
            break;
        }
        int const found = NearestSafeSpot(bot, questId, spots, int32(objective), poiInfo, blocker);
        if (found)
            resolved.insert(int32(objective));
        if (found == 3)
            ++spentObjectives;
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
        if (!inComplete || resolved.count(qPoi.ObjectiveIndex))
            continue;
        if (qPoi.points.size() == 0)
            continue;
        // [guild90] no spawn of the objective's sources: the polygon's own points are the spots (swept one by one
        // like camps), no random blend of them
        std::vector<QuestSpot> corners;
        for (QuestPOIPoint const& point : qPoi.points)
            corners.push_back({qPoi.MapId, float(point.x), float(point.y), 0.0f});
        if (NearestSafeSpot(bot, questId, corners, qPoi.ObjectiveIndex, poiInfo, blocker) == 3)
            ++spentObjectives;
    }

    // [guild10] the explore trigger (the spawns of kill / use objectives are resolved above)
    if (poiInfo.empty() && quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
    {
        auto const triggers = spawnIndex.triggers.find(questId);
        if (triggers != spawnIndex.triggers.end())
            NearestSafeSpot(bot, questId, triggers->second, QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT,
                            poiInfo, blocker);
    }

    // [guild12] every objective spot among or behind orange / red mobs: the next quest instead
    // [guild90] held until the next level; every camp seen empty is no postpone: the quest comes back with the
    // respawn (the spent marks run out), the planner meanwhile takes another
    if (poiInfo.empty() && blocker)
        HoldPostponed(bot, quest,
                      "objective among " + blocker->Name + " L" + std::to_string(blocker->maxlevel) +
                          [&]()
                          {
                              // [guild15] a mob of lower con blocks only by its remembered death / refused-pull zone
                              XPColorChar const con = Acore::XP::GetColorCode(bot->GetLevel(), blocker->maxlevel);
                              return std::string(con == XP_RED      ? " (red)"
                                                 : con == XP_ORANGE ? " (orange)"
                                                                    : " (refused pull / death zone)");
                          }());
    else if (poiInfo.empty() && spentObjectives)
        NotePostpone(bot, questId, "every camp of its objectives seen empty (back at their respawn)");

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
        int16 event = 0;         // [guild15] the giver spawn's game event (b321: Day of the Dead givers, event 51 not
                                 // running, drew bots 1-17 km to an empty spot: q13952, q14167-14173)
    };

    std::vector<QuestStartPoint> const& GetQuestStartPoints()
    {
        static std::vector<QuestStartPoint> points;
        static std::once_flag built;
        std::call_once(built, []()
        {
            // [guild15] one spawn per entry, a permanent one before an event's
            std::unordered_map<uint32, std::pair<CreatureData const*, int16>> creatureSpawn;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                int16 const event = SpawnEvent(true, spawnId);
                auto const [it, added] = creatureSpawn.emplace(data.id, std::make_pair(&data, event));
                if (!added && it->second.second && !event)
                    it->second = {&data, event};
            }

            for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestRelationMap())
            {
                auto spawn = creatureSpawn.find(entry);
                CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(entry);
                if (spawn == creatureSpawn.end() || !creatureTemplate)
                    continue;

                CreatureData const* data = spawn->second.first;
                points.push_back({questId, data->mapid, data->posX, data->posY, data->posZ, creatureTemplate->faction,
                                  spawn->second.second});
            }

            std::unordered_map<uint32, std::pair<GameObjectData const*, int16>> objectSpawn;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
            {
                int16 const event = SpawnEvent(false, spawnId);
                auto const [it, added] = objectSpawn.emplace(data.id, std::make_pair(&data, event));
                if (!added && it->second.second && !event)
                    it->second = {&data, event};
            }

            for (auto const& [entry, questId] : *sObjectMgr->GetGOQuestRelationMap())
            {
                auto spawn = objectSpawn.find(entry);
                if (spawn == objectSpawn.end())
                    continue;

                GameObjectData const* data = spawn->second.first;
                points.push_back({questId, data->mapid, data->posX, data->posY, data->posZ, 0, spawn->second.second});
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
float NewRpgBaseAction::RemainingShare(Player* bot, Quest const* quest)
{
    auto const itr = bot->getQuestStatusMap().find(quest->GetQuestId());
    if (itr == bot->getQuestStatusMap().end())
        return 1.0f;
    uint32 required = 0, left = 0;
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredNpcOrGo[i] && quest->RequiredNpcOrGoCount[i])
        {
            required += quest->RequiredNpcOrGoCount[i];
            left += quest->RequiredNpcOrGoCount[i] - std::min<uint32>(itr->second.CreatureOrGOCount[i],
                                                                      quest->RequiredNpcOrGoCount[i]);
        }
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemId[i] && quest->RequiredItemCount[i])
        {
            required += quest->RequiredItemCount[i];
            left += quest->RequiredItemCount[i] -
                    std::min<uint32>(itr->second.ItemCount[i], quest->RequiredItemCount[i]);
        }
    return required ? float(left) / float(required) : 1.0f;
}

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

void NewRpgBaseAction::NotePostpone(Player* bot, uint32 questId, std::string const& reason)
{
    {
        std::lock_guard<std::mutex> guard(questNoteLock);
        std::string& last = postponeReasons[bot->GetGUID().GetCounter()][questId];
        // [guild90] one line per reason (the planner looks at every quest of the log on every pass: b319 Peny q87
        // x340 the same note)
        if (last == reason)
            return;
        last = reason;
    }
    QuestStats::LogEvent(bot, "postpone", questId, "reason=" + reason);
}

void NewRpgBaseAction::ClaimSpot(Player* bot, uint32 questId, WorldPosition const& pos)
{
    std::lock_guard<std::mutex> guard(questNoteLock);
    uint32 const counter = bot->GetGUID().GetCounter();
    if (questId)
        postponeReasons[counter].erase(questId);
    auto const old = claimOf.find(counter);
    if (old != claimOf.end() && old->second != questId)
        claims[old->second].erase(counter);
    if (!questId)
    {
        claimOf.erase(counter);
        return;
    }
    claimOf[counter] = questId;
    claims[questId][counter] = {pos.GetMapId(), pos.GetPositionX(), pos.GetPositionY()};
}

std::string NewRpgBaseAction::LastResolveStats()
{
    return "spawns=" + std::to_string(resolveStats.spawns) + " spent=" + std::to_string(resolveStats.spent) +
           " taken=" + std::to_string(resolveStats.taken) + " offered=" + std::to_string(resolveStats.offered);
}

uint32 NewRpgBaseAction::EarliestRespawn(Player* bot, uint32 questId)
{
    std::lock_guard<std::mutex> guard(spentLock);
    auto const quests = spentSpots.find(bot->GetGUID().GetCounter());
    if (quests == spentSpots.end())
        return 0;
    auto const marks = quests->second.find(questId);
    if (marks == quests->second.end())
        return 0;
    time_t const now = time(nullptr);
    time_t best = 0;
    for (SpentMark const& mark : marks->second)
        if (mark.until > now && (!best || mark.until < best))
            best = mark.until;
    return best ? uint32(best - now) : 0;
}

float NewRpgBaseAction::OtherQuestSeconds(uint32 exceptQuest, uint32& otherQuest)
{
    otherQuest = 0;
    float best = -1.0f;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || questId == exceptQuest || botAI->lowPriorityQuest.count(questId))
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        std::vector<POIInfo> poiInfo;
        if (!quest || !GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
            continue;
        float distance = std::numeric_limits<float>::max();
        for (POIInfo const& poi : poiInfo)
            distance = std::min(distance, bot->GetDistance2d(poi.pos.x, poi.pos.y));
        float seconds = distance / QUEST_TRAVEL_SPEED;
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            seconds += QUEST_OBJECTIVE_TIME * RemainingShare(bot, quest);
        if (best < 0.0f || seconds < best)
        {
            best = seconds;
            otherQuest = questId;
        }
    }
    return best;
}

std::string NewRpgBaseAction::QuestChoiceNote(Player* bot)
{
    std::lock_guard<std::mutex> guard(questNoteLock);
    auto const found = choiceNotes.find(bot->GetGUID().GetCounter());
    return found == choiceNotes.end() ? std::string() : found->second;
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
    // [guild90] the quest it works on stays its choice until it is done, postponed or has no spot left (finish the
    // active quest before switching: b317 Eraelor flitted q919 <-> q487 on every IDLE); a hand-in nearer than that
    // quest's spot is taken on the way
    uint32 const counter = bot->GetGUID().GetCounter();
    uint32 committed = 0;
    {
        std::lock_guard<std::mutex> guard(questNoteLock);
        auto const found = committedQuest.find(counter);
        if (found != committedQuest.end())
            committed = found->second;
    }
    float committedDistance = -1.0f;
    float committedSeconds = -1.0f;
    struct QuestCandidate
    {
        uint32 questId;
        float rate, distance, seconds;
        bool complete;
    };
    std::vector<QuestCandidate> candidates;
    float nearestHandIn = std::numeric_limits<float>::max();
    uint32 nearestHandInQuest = 0;
    std::string committedWhy;
    if (committed && bot->GetQuestStatus(committed) != QUEST_STATUS_INCOMPLETE)
        committedWhy = bot->GetQuestStatus(committed) == QUEST_STATUS_COMPLETE ? "done, hand it in"
                       : bot->GetQuestStatus(committed) == QUEST_STATUS_REWARDED ? "handed in"
                                                                                : "left the log";
    else if (committed && botAI->lowPriorityQuest.count(committed))
    {
        std::lock_guard<std::mutex> guard(questNoteLock);
        auto const& reasons = postponeReasons[counter];
        auto const why = reasons.find(committed);
        committedWhy = "postponed: " + (why != reasons.end() ? why->second : std::string("no progress or no walk"));
    }

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        // [guild12] a failed quest (timed, escort) never leaves the log by itself: it goes, and is not taken again
        if (questId && bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED)
        {
            AbandonFailedQuest(questId);
            continue;
        }
        if (!questId || botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;
        // [guild13] a spell-unlock class quest: its spell comes by AutoLearnQuestSpells, the quest leaves the log
        if (IsSpellUnlockClassQuest(quest))
        {
            LOG_INFO("playerbots", "[rpg] {} L{} ABANDON q{} {} reason: class quest for a spell (granted)",
                     bot->GetName(), bot->GetLevel(), questId, quest->GetTitle());
            bot->RemoveActiveQuest(questId, false);
            continue;
        }
        // [guild14] a quest ended only at a script-spawned object (no bot runs the script) leaves the log
        if (EnderNeedsScript(questId))
        {
            LOG_INFO("playerbots", "[rpg] {} L{} ABANDON q{} {} reason: its ender is an object a script spawns",
                     bot->GetName(), bot->GetLevel(), questId, quest->GetTitle());
            bot->RemoveActiveQuest(questId, false);
            continue;
        }

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
        // [guild90] the work left, not a whole objective's time for every quest: the quest nearest completion first
        if (!complete)
            time += QUEST_OBJECTIVE_TIME * RemainingShare(bot, quest) + QUEST_UNKNOWN_DISTANCE / QUEST_TRAVEL_SPEED;
        if (questId == committed && !complete)
        {
            committedDistance = distance;
            committedSeconds = time;
        }

        float const rate = QUEST_IN_LOG_BONUS * GetQuestWeight(quest, rested) * quest->XPValue(level) /
                           std::max(time, 1.0f);
        candidates.push_back({questId, rate, distance, time, complete});
    }

    // [guild90] hand-ins are grouped like a player's: a done quest whose ender also ends (within GROUP_ENDER_RADIUS)
    // a quest still being worked on waits for it - unless the bot passes the ender now (the giver search hands it
    // in on the way then anyway)
    std::vector<std::pair<uint32, WorldPosition>> openEnders;
    for (QuestCandidate const& c : candidates)
    {
        WorldPosition ender;
        if (!c.complete && NearestEnderSpot(bot, c.questId, ender))
            openEnders.push_back({c.questId, ender});
    }
    for (QuestCandidate const& c : candidates)
    {
        if (c.complete)
        {
            WorldPosition ender;
            bool deferred = false;
            uint32 waitsFor = 0;
            if (c.distance > HAND_IN_PASS_DISTANCE && NearestEnderSpot(bot, c.questId, ender))
                for (auto const& [open, openEnder] : openEnders)
                    if (openEnder.GetMapId() == ender.GetMapId() &&
                        openEnder.GetExactDist2d(ender.GetPositionX(), ender.GetPositionY()) <= GROUP_ENDER_RADIUS)
                    {
                        deferred = true;
                        waitsFor = open;
                        break;
                    }
            if (deferred)
            {
                std::lock_guard<std::mutex> guard(questNoteLock);
                auto& logged = deferredHandIns[counter];
                if (logged.insert(c.questId).second)
                    QuestStats::LogEvent(bot, "handin-grouped", c.questId,
                                         "waits for q" + std::to_string(waitsFor) + " (same ender)");
                continue;
            }
            if (c.distance < nearestHandIn)
            {
                nearestHandIn = c.distance;
                nearestHandInQuest = c.questId;
            }
            if (c.rate > bestCompleteRate)
            {
                bestCompleteRate = c.rate;
                bestCompleteQuest = c.questId;
            }
        }
        else if (c.rate > bestRate)
        {
            bestRate = c.rate;
            bestQuest = c.questId;
        }
    }
    auto const secondsOf = [&](uint32 questId) -> float
    {
        for (QuestCandidate const& c : candidates)
            if (c.questId == questId)
                return c.seconds;
        return -1.0f;
    };
    // [guild90] every switch away from the active quest: one line with the computed costs
    auto const logSwitch = [&](uint32 chosen, std::string const& why)
    {
        if (!committed || chosen == committed)
            return;
        QuestStats::LogEvent(bot, "target-switch", chosen,
                             "from=q" + std::to_string(committed) + " finish_s=" +
                                 (committedSeconds >= 0.0f ? std::to_string(uint32(committedSeconds)) : "none") +
                                 " other_s=" + std::to_string(int32(secondsOf(chosen))) + " reason=" + why);
    };

    auto const note = [&](uint32 chosen, std::string const& why)
    {
        std::lock_guard<std::mutex> guard(questNoteLock);
        choiceNotes[counter] = why;
        if (chosen && bot->GetQuestStatus(chosen) == QUEST_STATUS_INCOMPLETE)
            committedQuest[counter] = chosen;
        else if (!chosen || (committed && committedDistance < 0.0f))
            committedQuest.erase(counter);
        if (committed && (chosen != committed || bot->GetQuestStatus(committed) != QUEST_STATUS_INCOMPLETE))
        {
            auto const old = claimOf.find(counter);
            if (old != claimOf.end() && old->second == committed)
            {
                claims[committed].erase(counter);
                claimOf.erase(old);
            }
        }
    };
    auto const leaving = [&]() -> std::string
    {
        if (!committed)
            return "";
        if (!committedWhy.empty())
            return "; left q" + std::to_string(committed) + ": " + committedWhy;
        if (committedDistance < 0.0f)
            return "; left q" + std::to_string(committed) + ": no unspent objective spot it can walk to";
        return "";
    };

    // [guild90] the active quest first; a hand-in only when its ender is nearer than the active quest's spot
    if (committedDistance >= 0.0f && !(nearestHandInQuest && nearestHandIn < committedDistance))
    {
        note(committed, "continues the active quest (" + std::to_string(uint32(committedDistance)) + " yd to its spot)");
        return committed;
    }
    if (bestCompleteQuest)
    {
        uint32 const handIn = committedDistance >= 0.0f ? nearestHandInQuest : bestCompleteQuest;
        if (committedDistance < 0.0f)
            logSwitch(handIn, "hand-in first" + leaving());
        note(committedDistance >= 0.0f ? committed : 0,
             committedDistance >= 0.0f
                 ? "hand-in on the way (" + std::to_string(uint32(nearestHandIn)) + " yd), then back to q" +
                       std::to_string(committed)
                 : "hand-in first" + leaving());
        if (committedDistance >= 0.0f)
        {
            std::lock_guard<std::mutex> guard(questNoteLock);
            committedQuest[counter] = committed;
        }
        return handIn;
    }

    // [guild10] a new quest only once nothing in the log is left to do; every bot levels by quests
    if (bestQuest)
    {
        note(bestQuest, "best xp per second of the log" + leaving());
        logSwitch(bestQuest, "best xp per second of the log" + leaving());
        return bestQuest;
    }
    note(0, "nothing left in the log" + leaving());

    // [guild15] zone change by value (user queue item 4): every starter the bot may take now on the continents it can
    // reach is priced by the travel planner's honest route (walk, Deeprun Tram, boats, portals: a giver on another map
    // or landmass by the arrivals there), its quests are summed per zone, and the zone whose quests give the most
    // experience per second of travel + work wins; inside it the best single quest from the bot. A zone is exhausted
    // when it holds no starter the bot may take (computed, no timer). From 68 Northrend joins by its boats.
    uint32 const botLandmass = TravelRouteMgr::Landmass(WorldPosition(bot));
    constexpr uint32 LANDMASS_ZONE_KEY = 0x100000;  // above every AreaTable id
    std::map<uint32, std::vector<std::pair<WorldPosition, float>>> arrivals;  // map -> docks / portal exits, seconds
    auto const arrivalsOf = [&](uint32 mapId) -> std::vector<std::pair<WorldPosition, float>> const&
    {
        auto found = arrivals.find(mapId);
        if (found == arrivals.end())
        {
            found = arrivals.emplace(mapId, std::vector<std::pair<WorldPosition, float>>()).first;
            if (!bot->GetMap()->Instanceable())
                sTravelRouteMgr.GetArrivalCosts(bot, mapId, found->second);
        }
        return found->second;
    };
    auto const continent = [&](uint32 mapId)
    {
        return mapId == 0 || mapId == 1 || mapId == 530 ||
               (mapId == NORTHREND_MAP_ID && level >= NORTHREND_ENTRY_LEVEL);
    };

    struct StartCandidate
    {
        uint32 questId;
        uint32 zone;
        float travel, local, xp, rate;
        WorldPosition pos;
        int32 questLevel;
    };
    std::vector<StartCandidate> starts;
    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    for (QuestStartPoint const& start : GetQuestStartPoints())
    {
        if (!EventSpawnPresent(start.event))  // [guild15]
            continue;
        if (start.mapId != bot->GetMapId() && !continent(start.mapId))
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

        WorldPosition const giver(start.mapId, start.x, start.y, start.z);
        float travelSeconds = std::numeric_limits<float>::max();
        if (start.mapId == bot->GetMapId() && TravelRouteMgr::Landmass(giver) == botLandmass)
            travelSeconds = bot->GetDistance2d(start.x, start.y) / QUEST_TRAVEL_SPEED;  // [guild10] the whole landmass
        else
        {
            // another continent or landmass: the route's arrival there, then the walk to the giver
            uint32 const giverLandmass = TravelRouteMgr::Landmass(giver);
            for (auto const& [arrival, seconds] : arrivalsOf(start.mapId))
            {
                if (TravelRouteMgr::Landmass(arrival) != giverLandmass)
                    continue;
                float const fromArrival =
                    std::hypot(arrival.GetPositionX() - start.x, arrival.GetPositionY() - start.y);
                if (fromArrival <= QUEST_MAX_GIVER_DISTANCE)
                    travelSeconds = std::min(travelSeconds, seconds + fromArrival / QUEST_TRAVEL_SPEED);
            }
        }
        if (travelSeconds == std::numeric_limits<float>::max())
            continue;

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
        float const local = 2.0f * objectiveDistance / QUEST_TRAVEL_SPEED + QUEST_OBJECTIVE_TIME;
        float const xp = GetQuestWeight(quest, rested) * quest->XPValue(level);
        // the zone the quest belongs to (QuestSortID > 0); a class / profession / holiday quest (sort <= 0) counts
        // to the bot's own zone on its landmass, else to its giver's landmass (no terrain lookup of far grids)
        uint32 zone = quest->GetZoneOrSort() > 0 ? uint32(quest->GetZoneOrSort()) : 0;
        if (!zone)
            zone = start.mapId == bot->GetMapId() && TravelRouteMgr::Landmass(giver) == botLandmass
                       ? bot->GetZoneId()
                       : LANDMASS_ZONE_KEY + TravelRouteMgr::Landmass(giver);
        float const rate = xp / (travelSeconds + local);
        // one entry per quest (a quest with several givers: the cheapest)
        auto const same = std::find_if(starts.begin(), starts.end(),
                                       [&](StartCandidate const& c) { return c.questId == start.questId; });
        if (same != starts.end())
        {
            if (rate > same->rate)
                *same = {start.questId, zone, travelSeconds, local, xp, rate, giver, questLevel};
            continue;
        }
        starts.push_back({start.questId, zone, travelSeconds, local, xp, rate, giver, questLevel});
    }

    struct ZoneValue
    {
        float xp = 0.0f;
        float local = 0.0f;
        float travel = std::numeric_limits<float>::max();
        uint32 quests = 0;
        int32 minLevel = 255;
        int32 maxLevel = 0;
        float Rate() const { return xp / (travel + local); }
    };
    std::map<uint32, ZoneValue> zones;
    for (StartCandidate const& c : starts)
    {
        ZoneValue& z = zones[c.zone];
        z.xp += c.xp;
        z.local += c.local;
        z.travel = std::min(z.travel, c.travel);
        ++z.quests;
        z.minLevel = std::min(z.minLevel, c.questLevel);
        z.maxLevel = std::max(z.maxLevel, c.questLevel);
    }
    uint32 bestZone = 0;
    float bestZoneRate = 0.0f;
    for (auto const& [zone, value] : zones)
        if (!bestZone || value.Rate() > bestZoneRate)
        {
            bestZone = zone;
            bestZoneRate = value.Rate();
        }
    if (!bestZone)
        return 0;

    StartCandidate const* best = nullptr;
    for (StartCandidate const& c : starts)
        if (c.zone == bestZone && (!best || c.rate > best->rate))
            best = &c;
    if (!best)
        return 0;
    giverPos = best->pos;

    // [zone] one line per change of the bot's questing zone: from -> to, why and the route
    uint32 const hereZone = bot->GetZoneId();
    if (bestZone != hereZone)
    {
        bool fresh = false;
        {
            std::lock_guard<std::mutex> guard(questNoteLock);
            uint32& last = zoneTarget[counter];
            fresh = last != bestZone;
            last = bestZone;
        }
        if (fresh)
        {
            auto const zoneName = [&](uint32 id) -> std::string
            {
                if (id >= LANDMASS_ZONE_KEY)
                    return "landmass " + std::to_string(id - LANDMASS_ZONE_KEY);
                AreaTableEntry const* area = sAreaTableStore.LookupEntry(id);
                return area ? std::string(area->area_name[0]) : "zone " + std::to_string(id);
            };
            auto const here = zones.find(hereZone);
            std::string const why =
                here == zones.end()
                    ? std::string("exhausted: no quest here it may take now")
                    : "better value: here " + std::to_string(here->second.quests) + " quests " +
                          std::to_string(uint32(here->second.Rate() * 3600.0f)) + " xp/h";
            ZoneValue const& to = zones[bestZone];
            std::string route = "walk";
            TravelRouteMgr::Leg leg;
            float seconds = 0.0f;
            if (!sTravelRouteMgr.FindRoute(bot, giverPos, leg, seconds))
                route = "none";
            else if (leg.kind == TravelRouteMgr::LegKind::Transport)
                route = "transport " + std::to_string(leg.lineEntry);
            else if (leg.kind == TravelRouteMgr::LegKind::Portal)
                route = "portal " + std::to_string(leg.portalIndex);
            LOG_INFO("playerbots",
                     "[zone] {} L{} {} -> {}: {}; there {} quests L{}-{} {} xp/h (travel {} s), route {} {} s, "
                     "first q{} giver at map {} ({},{})",
                     bot->GetName(), level, zoneName(hereZone), zoneName(bestZone), why, to.quests, to.minLevel,
                     to.maxLevel, uint32(to.Rate() * 3600.0f), uint32(to.travel), route, uint32(seconds), best->questId,
                     giverPos.GetMapId(), int32(giverPos.GetPositionX()), int32(giverPos.GetPositionY()));
        }
    }
    return best->questId;
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
        if (start.mapId != bot->GetMapId() || !EventSpawnPresent(start.event))  // [guild15]
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

// [guild12] the shared quest indexes, built at server startup (PlayerbotsWorldScript::OnStartup)
void NewRpgBaseAction::BuildQuestIndexes()
{
    GetQuestSpawnIndex();
    GetQuestStartPoints();
    EnderNeedsScript(0);  // [guild14]
}
