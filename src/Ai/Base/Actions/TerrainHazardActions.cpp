/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TerrainHazardActions.h"
#include "BotPath.h"  // [lead109]

#include "ChooseTargetActions.h"
#include "Group.h"
#include "Map.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "RangedPositionActions.h"
#include "TerrainHazard.h"

#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32 FIGHT_HAZARD_LOOK_MS = 1000;        // the tank's spot is looked at once a second
    constexpr uint32 FIGHT_HAZARD_FIGHT_GAP_MS = 5000;   // no look for this long: the next look is a new fight
    constexpr uint32 FIGHT_HAZARD_RETRY_MS = 5000;       // no safe spot found: the next search after this
    constexpr uint32 FIGHT_HAZARD_WALK_MS = 6000;        // a drag ends after this, arrived or not
    constexpr uint32 FIGHT_HAZARD_LOG_MS = 10000;        // "[hazard]" lines per tank at most this often
    constexpr uint8 FIGHT_HAZARD_MAX_MOVES = 2;          // drags per fight (a mob that does not follow: no loop)
    constexpr float FIGHT_HAZARD_HOLD_RANGE = 8.0f;      // the tank holds its victim this close (melee, or coming)
    constexpr float FIGHT_HAZARD_ARRIVED = 2.0f;
    constexpr float FIGHT_HAZARD_FLOOR = 3.0f;           // a spot on the tank's floor: ground within this of it
    constexpr float FIGHT_HAZARD_GROUND_SEARCH = 2.0f * FIGHT_HAZARD_FLOOR + 1.0f;
    constexpr float FIGHT_HAZARD_MOB_SCAN = 45.0f;       // idle mobs further than this from a spot are not asked
    constexpr float FIGHT_HAZARD_PATH_DETOUR = 2.0f;     // the drag's path is at most this many times the distance
    constexpr float FIGHT_HAZARD_PATH_SLACK = 3.0f;      // ... plus this
    constexpr float FIGHT_HAZARD_SAME_DIRECTION = 0.3f;  // radians: a base direction this close to one tried is skipped
    constexpr uint32 FIGHT_HAZARD_SPOT_BUDGET = 18;      // candidates tested in full (hazard, LoS, aggro, path)
    constexpr float FIGHT_HAZARD_DISTANCES[] = {6.0f, 8.0f, 10.0f};
    constexpr float FIGHT_HAZARD_OFFSETS[] = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f};  // radians off a base direction

    struct HazardWalk
    {
        Position spot;
        uint32 seenMs = 0;   // the last look (0: the next look starts a new fight)
        uint32 lookMs = 0;   // the last look at the tank's own spot
        uint32 walkMs = 0;   // the drag started (0: none)
        uint32 retryMs = 0;  // no safe spot was found then
        uint32 logMs = 0;
        uint8 moves = 0;     // drags this fight
        bool pending = false;  // the last look found a hazard: the action searches a spot
        TerrainHazardProbe probe;
    };

    std::mutex hazardWalkLock;
    std::unordered_map<uint64, HazardWalk> hazardWalks;

    bool WalkUnderWay(HazardWalk const& walk, Player* tank, uint32 now)
    {
        return walk.walkMs && getMSTimeDiff(walk.walkMs, now) < FIGHT_HAZARD_WALK_MS &&
               tank->GetExactDist(walk.spot) > FIGHT_HAZARD_ARRIVED;
    }

    bool NoIdleMobReaches(PlayerbotAI* botAI, Player* tank, GuidVector const& possible, Position const& spot)
    {
        for (ObjectGuid const& guid : possible)
        {
            Unit* mob = botAI->GetUnit(guid);
            if (!mob || mob->IsInCombat())
                continue;

            if (mob->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) > FIGHT_HAZARD_MOB_SCAN)
                continue;

            if (IdleMobReachesSpot(tank, mob, spot))
                return false;
        }

        return true;
    }

    bool PathFits(Player* tank, Position const& spot)
    {
        BotPath const path =  // [lead109]
            FindBotPath(tank, tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ(), spot.GetPositionX(),
                        spot.GetPositionY(), spot.GetPositionZ());
        return path.type == PATHFIND_NORMAL &&
               path.length <= tank->GetExactDist(spot) * FIGHT_HAZARD_PATH_DETOUR + FIGHT_HAZARD_PATH_SLACK;
    }

    // Open floor a few yards from the tank, first away from the hazard, then towards the healer, then towards the
    // group: on the tank's floor, no hazard within TERRAIN_HAZARD_FIGHT_RADIUS, seen by the healer, reached by no
    // idle mob (the careful pull's aggro test: aggro range plus margin, the floor gate, LoS), walkable by the navmesh.
    bool FindSafeFightSpot(PlayerbotAI* botAI, Player* tank, TerrainHazardProbe const& probe, Position& out,
                           std::string& via, std::string& rejected)
    {
        Map* map = tank->GetMap();
        struct BaseDirection
        {
            float angle;
            char const* name;
        };

        std::vector<BaseDirection> bases;
        auto addBase = [&bases](float dx, float dy, char const* name)
        {
            if (std::sqrt(dx * dx + dy * dy) < 0.5f)
                return;

            float const angle = std::atan2(dy, dx);
            for (BaseDirection const& base : bases)
            {
                float diff = std::fabs(Position::NormalizeOrientation(angle - base.angle));
                if (diff > float(M_PI))
                    diff = 2.0f * float(M_PI) - diff;

                if (diff < FIGHT_HAZARD_SAME_DIRECTION)
                    return;
            }

            bases.push_back({angle, name});
        };

        if (probe.awayX != 0.0f || probe.awayY != 0.0f)
            addBase(probe.awayX, probe.awayY, "away from it");
        else
            addBase(tank->GetPositionX() - probe.hazardX, tank->GetPositionY() - probe.hazardY, "away from it");

        if (Player* healer = FindGroupHealer(tank))
            addBase(healer->GetPositionX() - tank->GetPositionX(), healer->GetPositionY() - tank->GetPositionY(),
                    "towards the healer");

        float sumX = 0.0f;
        float sumY = 0.0f;
        uint32 members = 0;
        if (Group* group = tank->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != tank && member->IsAlive() && member->GetMap() == map &&
                    tank->GetExactDist(member) <= FIGHT_HAZARD_MOB_SCAN)
                {
                    sumX += member->GetPositionX();
                    sumY += member->GetPositionY();
                    ++members;
                }
            }
        }

        if (members)
            addBase(sumX / float(members) - tank->GetPositionX(), sumY / float(members) - tank->GetPositionY(),
                    "towards the group");

        GuidVector const possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets")->Get();
        uint32 budget = FIGHT_HAZARD_SPOT_BUDGET;
        uint32 noFloor = 0;
        uint32 hazard = 0;
        uint32 blind = 0;
        uint32 aggro = 0;
        uint32 noPath = 0;
        // Straight along every base direction first, then turned off it.
        for (float const offset : FIGHT_HAZARD_OFFSETS)
        {
            for (BaseDirection const& base : bases)
            {
                for (float const distance : FIGHT_HAZARD_DISTANCES)
                {
                    if (!budget)
                        break;

                    float const x = tank->GetPositionX() + distance * std::cos(base.angle + offset);
                    float const y = tank->GetPositionY() + distance * std::sin(base.angle + offset);
                    float const ground = map->GetHeight(tank->GetPhaseMask(), x, y,
                                                        tank->GetPositionZ() + FIGHT_HAZARD_FLOOR, true,
                                                        FIGHT_HAZARD_GROUND_SEARCH);
                    if (ground <= INVALID_HEIGHT || std::fabs(ground - tank->GetPositionZ()) > FIGHT_HAZARD_FLOOR)
                    {
                        ++noFloor;
                        break;  // the floor ends this way: further out is no better
                    }

                    --budget;
                    Position const spot(x, y, ground);
                    if (NearHazard(map, tank->GetPhaseMask(), x, y, ground, TERRAIN_HAZARD_FIGHT_RADIUS))
                    {
                        ++hazard;
                        continue;
                    }

                    if (!HealerSeesSpot(tank, spot))
                    {
                        ++blind;
                        continue;
                    }

                    if (!NoIdleMobReaches(botAI, tank, possible, spot))
                    {
                        ++aggro;
                        continue;
                    }

                    if (!PathFits(tank, spot))
                    {
                        ++noPath;
                        continue;
                    }

                    out = spot;
                    via = base.name;
                    return true;
                }
            }
        }

        rejected = "no floor " + std::to_string(noFloor) + ", hazard " + std::to_string(hazard) + ", healer blind " +
                   std::to_string(blind) + ", idle aggro " + std::to_string(aggro) + ", no path " +
                   std::to_string(noPath);
        return false;
    }
}

bool FightNearHazardTrigger::IsActive()
{
    if (!bot->IsInCombat() || !bot->IsAlive() || !PlayerbotAI::IsTank(bot) || !bot->GetGroup())
        return false;

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // A pull under way keeps its own spot; a walk back from the movers goes first (FightSpot).
    PullStrategy const* strategy = PullStrategy::Get(botAI);
    if (strategy && (strategy->HasTarget() || strategy->IsPullPendingToStart() || strategy->HasPullStarted()))
        return false;

    LosPull::Plan plan;
    if (LosPull::Get(bot, plan) && plan.tank == bot->GetGUID())
        return false;

    Position fightSpot;
    if (FightSpot::Walking(bot, fightSpot))
        return false;

    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> guard(hazardWalkLock);
        HazardWalk& walk = hazardWalks[bot->GetGUID().GetRawValue()];
        if (!walk.seenMs || getMSTimeDiff(walk.seenMs, now) > FIGHT_HAZARD_FIGHT_GAP_MS)
            walk = HazardWalk();  // a new fight

        walk.seenMs = now;
        if (WalkUnderWay(walk, bot, now))
            return true;

        walk.walkMs = 0;
        if (walk.pending)
            return true;  // the action has not searched yet

        bool const looked = walk.lookMs && getMSTimeDiff(walk.lookMs, now) < FIGHT_HAZARD_LOOK_MS;
        bool const failed = walk.retryMs && getMSTimeDiff(walk.retryMs, now) < FIGHT_HAZARD_RETRY_MS;
        if (walk.moves >= FIGHT_HAZARD_MAX_MOVES || looked || failed)
            return false;

        walk.lookMs = now;
    }

    // Only a fight the tank holds: a mob on it, in melee or coming (a mob that is not on it does not follow a drag).
    Unit* victim = bot->GetVictim();
    if (!victim || !victim->IsAlive() || victim->GetVictim() != bot ||
        bot->GetExactDist2d(victim) > FIGHT_HAZARD_HOLD_RANGE)
        return false;

    TerrainHazardProbe const probe = ProbeHazard(map, bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(),
                                                 bot->GetPositionZ(), TERRAIN_HAZARD_FIGHT_RADIUS);
    if (!probe.Found())
        return false;

    std::lock_guard<std::mutex> guard(hazardWalkLock);
    HazardWalk& walk = hazardWalks[bot->GetGUID().GetRawValue()];
    walk.pending = true;
    walk.probe = probe;
    return true;
}

bool MoveFightOffHazardAction::isUseful()
{
    return botAI->CanMove() && !bot->IsRooted() && !bot->HasUnitState(UNIT_STATE_LOST_CONTROL) &&
           !bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
}

bool MoveFightOffHazardAction::Execute(Event /*event*/)
{
    uint64 const key = bot->GetGUID().GetRawValue();
    uint32 const now = getMSTime();
    TerrainHazardProbe probe;
    Position spot;
    {
        std::lock_guard<std::mutex> guard(hazardWalkLock);
        HazardWalk& walk = hazardWalks[key];
        if (WalkUnderWay(walk, bot, now))
            spot = walk.spot;
        else if (!walk.pending)
            return false;
        else
        {
            walk.pending = false;
            probe = walk.probe;
        }
    }

    Unit* victim = bot->GetVictim();
    std::string const victimName = victim ? victim->GetName() : std::string("its mobs");
    if (probe.Found())
    {
        std::string via;
        std::string rejected;
        bool const found = FindSafeFightSpot(botAI, bot, probe, spot, via, rejected);
        std::lock_guard<std::mutex> guard(hazardWalkLock);
        HazardWalk& walk = hazardWalks[key];
        bool const log = !walk.logMs || getMSTimeDiff(walk.logMs, now) >= FIGHT_HAZARD_LOG_MS;
        if (log)
            walk.logMs = now;

        if (!found)
        {
            walk.retryMs = now;
            if (log)
                LOG_INFO("playerbots", "[hazard] {}: {} {:.0f} yd from the fight with {} at ({:.1f} {:.1f} {:.1f}), "
                         "no safe spot to drag it to ({})", bot->GetName(), HazardKindName(probe.kind), probe.distance,
                         victimName, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), rejected);
            return false;
        }

        walk.spot = spot;
        walk.walkMs = now;
        ++walk.moves;
        if (log)
            LOG_INFO("playerbots", "[hazard] {}: {} {:.0f} yd from the fight with {} at ({:.1f} {:.1f} {:.1f}) -> "
                     "drags it {:.1f} yd {} to ({:.1f} {:.1f} {:.1f}), drag {} of {}", bot->GetName(),
                     HazardKindName(probe.kind), probe.distance, victimName, bot->GetPositionX(), bot->GetPositionY(),
                     bot->GetPositionZ(), bot->GetExactDist(spot), via, spot.GetPositionX(), spot.GetPositionY(),
                     spot.GetPositionZ(), uint32(walk.moves), uint32(FIGHT_HAZARD_MAX_MOVES));
    }

    if (!MoveTo(bot->GetMapId(), spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), false, false, false,
                false, MovementPriority::MOVEMENT_COMBAT, true))
        return false;

    NoteMoveReason("hazard: the fight off lava/slime/a drop-off");
    return true;
}
