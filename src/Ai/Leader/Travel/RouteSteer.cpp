/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "RouteSteer.h"

#include "LeaderReports.h"
#include "LeaderEvents.h"
#include "AggressiveTargetValue.h"
#include "AiObjectContext.h"
#include "AKTriggers.h"
#include "ANAddTactics.h"
#include "Bag.h"
#include "BotMoveEvents.h"
#include "CellImpl.h"
#include "Chat.h"
#include "CoSTriggers.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "DBCStores.h"
#include "Engine.h"
#include "EscortProtect.h"  // [b36_ohb] EscortGossipHeld
#include "EstimatedLifetimeValue.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GameTime.h"
#include "GDTriggers.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "RaidLeaderState.h"
#include "HoRTriggers.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MechShared.h"
#include "MgTShared.h"
#include "ClassicShared.h"  // [b32_smpull] Mograine's linked chapel trash
#include "MotionMaster.h"
#include "OCTriggers.h"
#include "MoveSpline.h"
#include "AttackerCountValues.h"
#include "MovementActions.h"
#include "LeaderOrders.h"  // [lead1] the tank's pull orders
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "LeaderKnowledge.h"
#include "RouteSearch.h"  // [lead99] the leader's route search
#include "PullFacts.h"    // [lead104] the leader's pull / fight facts
#include "LeaderFightChecks.h"  // [lead107] the leader's checks
#include "LeaderPlan.h"         // [lead107] the leader's planner tick
#include "Pet.h"  // [lead38] the pet redirect
#include <source_location>  // [lead76]
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "PullActions.h"  // [b39_strat] FightHold: the opening fight held on the use side of a key gate
#include "ChooseTargetActions.h"  // [b47_door] PullReadiness::GroupPreparing
#include "SmartAI.h"
#include "SmartScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StuckWatchdog.h"
#include "DamageTaken.h"
#include "DungeonHazard.h"   // [lead16] the leader's hazard view
#include "LeaderActions.h"   // [lead19] the member guard model (LeaderMoveOrderAction::WalkRefusal)
#include "RtiValue.h"        // [lead16] the leader's CC assignments
#include "DungeonTactics.h"  // [lead15] the leader's cleave dodge (InFrontalCleave)
#include "InstanceScript.h"
#include "InstanceSaveMgr.h"
#include "SelfTravelActions.h"
#include "TemporarySummon.h"
#include "TerrainHazard.h"
#include "TestRunnerStrategy.h"
#include "Transport.h"
#include "UKTriggers.h"
#include "UPTriggers.h"
#include "Timer.h"
#include <chrono>  // [lead15] the leader cadence cost
#include <functional>
#include <optional>  // [lead18]
#include "WaypointMgr.h"
#include "WorldPacket.h"
#include "World.h"
#include "WorldSession.h"
#include "G3D/Vector4.h"  // [b32_brs] HopelessDetourLeg discs
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "RouteTargets.h"

namespace Leader::Travel
{
    using namespace Leader::Fight;
    using namespace Leader::Knowledge;
    using Leader::Dungeon::PULL_ABORT_HOLD_MS;
    using Leader::Dungeon::CAREFUL_REFUSED_SKIP_MS;

    bool PullAfterHoldPasses(ObjectGuid leader, ObjectGuid blocker)
        {
            auto const found = pullAfterHoldGates.find(leader);
            return found != pullAfterHoldGates.end() && found->second.blocker == blocker &&
                   int32(found->second.untilMs - getMSTime()) > 0;
        }

    std::unordered_map<ObjectGuid, PlannerAsk> aoePullAsks;  // by leader

    bool AoePullAskRuns(ObjectGuid leaderGuid, ObjectGuid blocker, uint32 now)
        {
            PlannerAsk& ask = aoePullAsks[leaderGuid];
            if (ask.target != blocker || !ask.sinceMs || getMSTimeDiff(ask.sinceMs, now) > AOE_PULL_ASK_RESET_MS)
            {
                ask.target = blocker;
                ask.sinceMs = now ? now : 1;
            }
            return getMSTimeDiff(ask.sinceMs, now) < AOE_PULL_ASK_MS;
        }

    std::map<std::pair<uint32, ObjectGuid>, uint32> guardRefusals;

    std::set<std::pair<uint32, ObjectGuid>> guardApproachLogged;

    std::map<std::pair<uint32, ObjectGuid>, uint32> guardApproachSinceMs;

    std::set<std::pair<uint32, ObjectGuid>> steerRestLogged;

    std::unordered_map<uint32, float> starterNearestPath;   // instance id -> shortest remaining walk to the starter

    std::unordered_map<ObjectGuid, std::vector<DeadEndVisit>> deadEndVisits;  // per walker, world thread only

    std::unordered_map<ObjectGuid, DeadEndRefusals> deadEndRefusals;  // per leader, world thread only

    // [incomplete_route_deadend] Is walking to the end of a partial leg progress towards goal? The why of a no
    // (Steer then skips the target as "route end", rechecked once the leader stands elsewhere), empty for a yes or
    // when the target is near enough for the gap cross / own move / "pull from here". Records the ends the walker
    // stood at (DeadEndVisit) and forgets one once the walker got nearer to the goal than it was there.
    // Every Steer tick, whatever the leg: an end the walker got past (nearer to the goal by DEAD_END_PROGRESS than
    // it stood there - the Mechanar stairs climbed by the core's own move) was no dead end.
    void ForgetPassedDeadEnds(Player* walker, G3D::Vector3 const& goal)
    {
        auto const found = deadEndVisits.find(walker->GetGUID());
        if (found == deadEndVisits.end() || found->second.empty())
            return;
        G3D::Vector3 const here(walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ());
        float const distance = (goal - here).length();
        uint32 const now = getMSTime();
        std::vector<DeadEndVisit>& visits = found->second;
        visits.erase(std::remove_if(visits.begin(), visits.end(),
                                    [&](DeadEndVisit const& visit)
                                    {
                                        return getMSTimeDiff(visit.ms, now) > DEAD_END_MEMORY_MS ||
                                               ((visit.goal - goal).length() <= DEAD_END_SAME_GOAL &&
                                                distance + DEAD_END_PROGRESS < visit.distance);
                                    }),
                     visits.end());
    }

    DeadEndVerdict JudgeIncompleteEnd(Player* walker, G3D::Vector3 const& end, G3D::Vector3 const& goal,
                                      uint32 pathType, ObjectGuid target)
    {
        DeadEndVerdict verdict;
        G3D::Vector3 const here(walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ());
        verdict.distance = (goal - here).length();
        verdict.left = (goal - end).length();
        verdict.hgap = std::sqrt((goal.x - end.x) * (goal.x - end.x) + (goal.y - end.y) * (goal.y - end.y));
        verdict.dz = goal.z - end.z;
        verdict.surface = WaterSurfaceAt(walker, end);
        verdict.underwater = verdict.surface > INVALID_HEIGHT && verdict.surface > end.z + 1.0f;
        // A swimmer rises to the surface by itself: the climb counts from there.
        verdict.rise = goal.z - (verdict.surface > INVALID_HEIGHT ? std::max(end.z, verdict.surface) : end.z);
        float const walk = (end - here).length();
        uint32 const now = getMSTime();

        if (verdict.distance <= GAP_CROSS_MAX_DISTANCE)
            return verdict;
        // A known way on: the end or the goal at a route link (the link walk / the own move there carry on).
        for (RouteLinkDef const* link : RouteLinks(walker->GetMapId()))
        {
            G3D::Vector3 const linkFrom(link->fromX, link->fromY, link->fromZ);
            G3D::Vector3 const linkTo(link->toX, link->toY, link->toZ);
            if ((linkFrom - end).length() <= DEAD_END_LINK_RANGE ||
                (linkTo - end).length() <= DEAD_END_LINK_RANGE ||
                (linkFrom - goal).length() <= DEAD_END_LINK_RANGE ||
                (linkTo - goal).length() <= DEAD_END_LINK_RANGE)
                return verdict;
        }

        ForgetPassedDeadEnds(walker, goal);
        DeadEndVisit const* visited = nullptr;
        if (auto const found = deadEndVisits.find(walker->GetGUID()); found != deadEndVisits.end())
            for (DeadEndVisit const& visit : found->second)
                if ((visit.end - end).length() <= DEAD_END_SAME_END &&
                    (visit.target == target || (visit.goal - goal).length() <= DEAD_END_SAME_GOAL))
                    visited = &visit;

        // A target off the navmesh (FARFROMPOLY_END: hovering, on a model without polygons) says nothing about
        // the climb.
        bool const cliff = !(pathType & PATHFIND_FARFROMPOLY_END) && verdict.left > GAP_REACHABLE_LEFT &&
                           verdict.rise > DEAD_END_CLIFF_MIN_RISE &&
                           verdict.rise > DEAD_END_CLIFF_RATIO * verdict.hgap;
        // An end in water: a swim (the Ghaz'an lake shortcut, the swim to the Black Stalker) goes on in 3D - only
        // the cliff test.
        bool const water = verdict.surface > INVALID_HEIGHT;
        if (cliff)
            verdict.why = verdict.underwater ? "ends in water below a cliff" : "ends below a cliff";
        else if (!water && visited && walk > DEAD_END_SAME_END * 2.0f)
        {
            verdict.why = "same dead end again";
            verdict.visitSec = getMSTimeDiff(visited->ms, now) / IN_MILLISECONDS;
        }
        else if (!water && walk > DEAD_END_WALK_MIN &&
                 verdict.left + std::max(DEAD_END_MIN_GAIN, DEAD_END_MIN_GAIN_SHARE * verdict.distance) >=
                     verdict.distance)
            verdict.why = "ends no nearer";
        return verdict;
    }

    // [incomplete_route_deadend] The never-a-stall fallback: true while the leader walks partial routes the old
    // way. A refusal counts; the DEAD_END_FALLBACK_REFUSALS-th within the window starts it (started: this call).
    bool DeadEndFallback(Player* leader, bool refusal, bool& started)
    {
        started = false;
        uint32 const now = getMSTime();
        DeadEndRefusals& refusals = deadEndRefusals[leader->GetGUID()];
        if (refusals.fallbackUntilMs && int32(refusals.fallbackUntilMs - now) > 0)
            return true;
        refusals.fallbackUntilMs = 0;
        refusals.ms.erase(std::remove_if(refusals.ms.begin(), refusals.ms.end(),
                                         [now](uint32 ms)
                                         { return getMSTimeDiff(ms, now) > DEAD_END_FALLBACK_WINDOW_MS; }),
                          refusals.ms.end());
        if (!refusal)
            return false;
        refusals.ms.push_back(now);
        if (refusals.ms.size() < DEAD_END_FALLBACK_REFUSALS)
            return false;
        refusals.ms.clear();
        uint32 const until = now + DEAD_END_FALLBACK_MS;
        refusals.fallbackUntilMs = until ? until : 1;
        started = true;
        return true;
    }

    // The leader stands at the aggro edge of an unengaged mob of the target's pack: on its floor, in line of
    // sight, no farther than its aggro range plus the margin. The pull logic takes over from here.
    bool AtPackAggroEdge(Player* leader, Creature* target)
    {
        std::list<Creature*> pack;
        PackMobCheck check{leader, target};
        Acore::CreatureListSearcher<PackMobCheck> searcher(target, pack, check);
        Cell::VisitObjects(target, searcher, PACK_RADIUS);
        for (Creature* mob : pack)
            if (leader->GetExactDist(mob) <= mob->GetAggroRange(leader) + AGGRO_EDGE_MARGIN &&
                std::fabs(leader->GetPositionZ() - mob->GetPositionZ()) <= SAME_FLOOR_DZ &&
                leader->IsWithinLOSInMap(mob))
                return true;
        return false;
    }

    std::unordered_map<uint64, uint32> tankRejectLogMs;  // mob -> last "approach ignores" line, world thread only

    bool PatrolWaitSpot(Player* leader, G3D::Vector3& spot, Creature*& nearMob)
    {
        std::list<Creature*> idle;
        ShadowMobCheck check{leader};
        Acore::CreatureListSearcher<ShadowMobCheck> searcher(leader, idle, check);
        Cell::VisitObjects(leader, searcher, PATROL_WAIT_SCAN);
        auto const reaches = [&](float x, float y, float z) -> Creature*
        {
            for (Creature* mob : idle)
                if (mob->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE &&
                    mob->GetExactDist(x, y, z) <= AggroReach(mob, leader) + PATROL_WAIT_ASSIST)
                    return mob;
            return nullptr;
        };
        nearMob = reaches(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        if (!nearMob)
            return false;
        float const away = nearMob->GetAngle(leader);
        for (float step = PATROL_WAIT_STEP; step <= PATROL_WAIT_STEP_MAX; step += PATROL_WAIT_STEP)
            for (float turn : {0.0f, 0.5f, -0.5f, 1.0f, -1.0f})
            {
                float const x = leader->GetPositionX() + std::cos(away + turn) * step;
                float const y = leader->GetPositionY() + std::sin(away + turn) * step;
                float const ground = leader->GetMap()->GetHeight(leader->GetPhaseMask(), x, y,
                                                                 leader->GetPositionZ() + 3.0f, true, 10.0f);
                if (ground <= INVALID_HEIGHT || !DirectLineOverGround(leader, x, y, ground) ||
                    reaches(x, y, ground))
                    continue;
                spot = G3D::Vector3(x, y, ground);
                return true;
            }

        // [patrol_wait_trail_back] No clear spot straight away from the nearest idle mob (a room full of packs,
        // a corridor whose sides are walls): the group steps back along the way it came, the tank's own trail
        // (MovementAction::TankTrailPoints, every 2 yd), to the first point out of every idle mob's reach and
        // assistance. Walked already, the way back pulls nothing new. scholo-h w24 (115805-scholo-h-5A-34,
        // pull 11), scholo-n w23 and brd-n w24 logged "no clear spot" and waited in the patrol's path next to the
        // neighbour pack; the patrol fight called it in (wipe analysis w24-25, cause C).
        std::vector<G3D::Vector3> const trail = MovementAction::TankTrailPoints(leader);
        G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        float walkedBack = 0.0f;
        G3D::Vector3 previous = here;
        for (auto point = trail.rbegin(); point != trail.rend(); ++point)
        {
            walkedBack += (*point - previous).length();
            previous = *point;
            if (walkedBack > PATROL_WAIT_TRAIL_MAX)
                break;
            if (walkedBack < PATROL_WAIT_STEP || std::fabs(point->z - here.z) > PATROL_WAIT_TRAIL_FLOOR ||
                reaches(point->x, point->y, point->z))
                continue;
            spot = *point;
            return true;
        }
        return false;
    }

    std::unordered_map<uint64, uint32> unseenRouteEndUntilMs;  // creature guid -> until (world thread)

    std::unordered_map<uint64, uint32> unseenRouteEndLogMs;

    void MarkUnseenRouteEnd(Creature* mob, uint32 untilMs)
    {
        if (ROUTE_UNSEEN_ROUTE_END_PASS && mob)
            unseenRouteEndUntilMs[mob->GetGUID().GetRawValue()] = untilMs ? untilMs : 1;
    }

    bool UnseenRouteEnd(Player* leader, Creature* mob)
    {
        if (!ROUTE_UNSEEN_ROUTE_END_PASS)
            return false;
        auto const found = unseenRouteEndUntilMs.find(mob->GetGUID().GetRawValue());
        if (found == unseenRouteEndUntilMs.end())
            return false;
        uint32 const now = getMSTime();
        if (int32(now - found->second) >= 0)
        {
            unseenRouteEndUntilMs.erase(found);
            return false;
        }
        uint32& loggedMs = unseenRouteEndLogMs[mob->GetGUID().GetRawValue()];
        if (!loggedMs || getMSTimeDiff(loggedMs, now) > 30 * IN_MILLISECONDS)
        {
            loggedMs = now ? now : 1;
            LOG_INFO("playerbots",
                     "[approach] {}: unseen_route_end_pass: {} ({:.1f} yd) stops no walk unseen - skipped as "
                     "route end out of the leader's sight",
                     leader->GetName(), mob->GetName(), leader->GetExactDist(mob));
        }
        return true;
    }

    bool BossEncounterMob(Player* leader, Creature* boss, Creature* mob)
    {
        if (!ROUTE_BOSS_OWN_ENCOUNTER_PACK || !leader || !boss || !mob || boss == mob)
            return false;
        char const* joins = JoinsBossFight(leader, boss, mob);
        return joins && (std::strcmp(joins, "joins boss: formation") == 0 ||
                         std::strcmp(joins, "joins boss: script") == 0);
    }

    bool BossBroughtBy(Player* leader, Creature* boss, Creature* mob)
    {
        if (!ROUTE_BOSS_BROUGHT_BY_PACK || !leader || !boss || !mob || boss == mob || !boss->IsAlive() ||
            !mob->IsAlive() || boss->IsInCombat() || boss->IsImmuneToPC() || !boss->IsHostileTo(leader))
            return false;
        if (BossEncounterMob(leader, boss, mob))
            return true;
        if (std::fabs(boss->GetPositionZ() - mob->GetPositionZ()) > APPROACH_OTHER_FLOOR_DZ)
            return false;
        float const reach = AggroReach(boss, leader);
        float const apart = std::min(boss->GetExactDist2d(mob), boss->GetHomePosition().GetExactDist2d(
                                                                    mob->GetPositionX(), mob->GetPositionY()));
        return apart <= reach && boss->IsWithinLOSInMap(mob);
    }

    bool BossRoomPack(Player* leader, Creature* boss, Creature* mob)
    {
        if (!ROUTE_BOSS_ROOM_PACK || !leader || !boss || !mob || boss == mob || !boss->IsAlive() ||
            !mob->IsAlive() || boss->IsInCombat() || mob->IsInCombat() ||
            !(boss->IsDungeonBoss() || boss->isWorldBoss()))
            return false;
        if (std::fabs(boss->GetPositionZ() - mob->GetPositionZ()) > APPROACH_OTHER_FLOOR_DZ ||
            boss->GetExactDist(mob) > BOSS_ROOM_TRASH_RADIUS)
            return false;
        return boss->IsWithinLOSInMap(mob) && !leader->IsWithinLOSInMap(mob);
    }

    bool ApproachStop(Player* leader, WorldObject* target, Movement::PointsArray const& points, float length,
                      G3D::Vector3& stop, Creature*& blocker, bool& unseenStop, Creature* exemptPack,
                      Leader::LeaderGroup* group)
    {
        unseenStop = false;
        std::list<Creature*> mobs;
        IdleAggroCheck check{leader};
        Acore::CreatureListSearcher<IdleAggroCheck> searcher(target, mobs, check);
        Cell::VisitObjects(target, searcher, APPROACH_SCAN_RADIUS);
        std::list<Creature*> routeMobs;
        Acore::CreatureListSearcher<IdleAggroCheck> routeSearcher(leader, routeMobs, check);
        Cell::VisitObjects(leader, routeSearcher, length + APPROACH_ROUTE_SCAN_MARGIN);
        for (Creature* mob : routeMobs)
            if (std::find(mobs.begin(), mobs.end(), mob) == mobs.end())
                mobs.push_back(mob);
        // A mob the tank rejected (another floor, no path) holds nothing: nobody would pull it (Gundrak wave 32:
        // the walk stopped at a Drakkari Lancer the tank listed as unreachable). Thread-safe registry.
        // The tank's reject list holds its height rejects too (AggressiveTargetValue: more than
        // TANK_REJECT_HEIGHT above or below it); "approach ignores" names each mob it kept from holding the walk,
        // once per TANK_REJECT_LOG_MS. (RFC 18:19, the trogg on the ledge was not one: the tank's "1 height" was
        // another mob, the trogg was out of its line of sight - approach to sight in Steer.)
        // [b30_mech_pos] ...except the walk's own target and its pack: the harness walks to it anyway, so a
        // reject (no full path: pos-n 20261025-184229, the ambush Ymirjar Wrathbringer summoned on a rock at
        // 911.9,63.4,547.7, the path ending 7.7 yd from it at 904.2,62.2,542.9) let the walk run into its pack
        // instead of stopping at the edge, and the tank pulled 7 mobs mounted. The edge is the pull's place.
        Creature* const walkTarget = target ? target->ToCreature() : nullptr;
        mobs.remove_if(
            [leader, walkTarget, group](Creature* mob)
            {
                if (!TankRejects::IsRejected(leader->GetGUID(), mob->GetGUID()))
                    return false;
                if (walkTarget && (mob == walkTarget || mob->GetExactDist(walkTarget) <= PACK_RADIUS))
                    return false;
                // A pack the tank's careful planner refused keeps stopping the walk even when the tank rejects it
                // as unseen: walked through, it is the mega-pull (wave 7 scholo-h / mt-h / mt-n / lbrs-n: the
                // harness walk into a refused 20-mob pack, no [pull plan] before the pull).
                bool accepted = true;
                uint32 planMobs = 0;
                // [lead5] the leader planner's own memory with LEADER_ONE_PICKER (PartyVerdict)
                if ((group ? Leader::Travel::PartyVerdict(*group, mob->GetGUID(), accepted, planMobs)
                           : CarefulPlanVerdict::Get(leader->GetGUID(), mob->GetGUID(), accepted, planMobs)) &&
                    !accepted)
                    return false;
                if (ROUTE_LOG_TANK_REJECTS && leader->GetExactDist(mob) < AggroReach(mob, leader))
                {
                    uint32 const now = getMSTime();
                    uint32& loggedMs = tankRejectLogMs[mob->GetGUID().GetRawValue()];
                    if (!loggedMs || getMSTimeDiff(loggedMs, now) > TANK_REJECT_LOG_MS)
                    {
                        loggedMs = now;
                        bool const height =
                            std::fabs(mob->GetPositionZ() - leader->GetPositionZ()) > TANK_REJECT_HEIGHT;
                        LOG_INFO("playerbots",
                                 "[approach] {}: approach ignores {}: tank rejects ({}) {:.1f} yd, dz {:.1f}",
                                 leader->GetName(), mob->GetName(),
                                 height ? "height" : "unseen / unreachable", leader->GetExactDist(mob),
                                 mob->GetPositionZ() - leader->GetPositionZ());
                    }
                }
                return true;
            });
        // A locked Forge Master (UKIds::IsForgeMasterLocked) stops no walk: engaged he only evades home, and
        // standing at his edge the tank's nearest pull is him (Utgarde Keep 10:44: the leader stopped 1 yd from
        // master 3 on the way to forge 2 and never left). Retarget keeps the route out of his reach instead.
        if (ROUTE_FORGE_ORDER)
            if (InstanceScript const* instance = leader->GetInstanceScript())
                mobs.remove_if([instance](Creature* mob) { return UKIds::IsForgeMasterLocked(mob, instance); });
        // Fast respawners (ROUTE_FAST_RESPAWN_PASS) are walked past.
        mobs.remove_if([](Creature* mob) { return FastRespawner(mob); });
        // exemptPack (a body pull's target): its own pack - the mobs within PACK_RADIUS of it and its formation -
        // is the pull and stops nothing; every other idle mob stops the walk.
        // [hleft-boss-own-pack] ... and a boss's encounter mobs (BossEncounterMob) with him.
        if (exemptPack)
            mobs.remove_if(
                [exemptPack, leader](Creature* mob)
                {
                    return mob == exemptPack || mob->GetExactDist(exemptPack) <= PACK_RADIUS ||
                           (exemptPack->GetFormation() && mob->GetFormation() == exemptPack->GetFormation()) ||
                           ((exemptPack->IsDungeonBoss() || exemptPack->isWorldBoss() ||
                             mob->GetExactDist(exemptPack) <= BOSS_ROOM_TRASH_RADIUS) &&
                            BossEncounterMob(leader, exemptPack, mob)) ||
                           // [b33_dme] a boss's walk: the mob his wander / aggro reach brings is his fight
                           ((exemptPack->IsDungeonBoss() || exemptPack->isWorldBoss()) &&
                            BossBroughtBy(leader, exemptPack, mob)) ||
                           BossRoomPack(leader, exemptPack, mob);  // [b33_harn]
                });
        if (mobs.empty())
            return false;

        // reach: AggroReach (real aggro plus the margin). holds: the distance inside which the mob stops the
        // walk - its reach, or the leader's own distance when it stands in the margin already: from there only a
        // step closer to the mob stops it. DM wave 37: the leader stood 22.4 yd from a refused Miner (reach 23),
        // and every route, also the ones leading away from it, stopped at its first sample - the leader's own
        // position - until the 4-minute stall. otherFloor: more than APPROACH_OTHER_FLOOR_DZ above or below the
        // leader (the tank).
        // unseenHolds: the same for the follower spread (APPROACH_FOLLOW_SPREAD, real aggro plus the spread, no
        // line of sight), only at route points on the mob's floor (SAME_FLOOR_DZ).
        struct MobReach
        {
            Creature* mob;
            float reach;
            float holds;
            float unseenHolds;
            bool otherFloor;
        };
        std::vector<MobReach> reaches;
        reaches.reserve(mobs.size());
        for (Creature* mob : mobs)
        {
            float const reach = AggroReach(mob, leader);
            float const distance = leader->GetExactDist(mob);
            reaches.push_back({mob, reach, std::min(reach, distance),
                               std::min(RealAggroRange(mob, leader) + APPROACH_FOLLOW_SPREAD, distance),
                               std::fabs(mob->GetPositionZ() - leader->GetPositionZ()) > APPROACH_OTHER_FLOOR_DZ});
        }

        // Only a mob that sees the point stops the walk: a creature aggroes in line of sight only
        // (Creature::MoveInLineOfSight), and the tank chooses its pull among the mobs it sees ("possible targets").
        // Wave 12: the leader stopped behind corners and doors at the reach of mobs it could not see, the tank had
        // no target and every levelling normal stalled at its entrance. A mob on another floor than the tank
        // counts only at the points of the route on its own floor.
        // The followers are not on the leader's eye line, though: a mob on the point's floor (SAME_FLOOR_DZ) stops
        // the walk at its real aggro range plus APPROACH_FOLLOW_SPREAD without the line of sight (Stockade wave
        // 40: a cell wall hid the Captive from the leader's point, its open door did not hide the shaman). A
        // mob the tank stood at and did not see is marked as not seen (TankRejects, tank_view) and stops no
        // later walk, as before. `unseen` tells which rule stopped it.
        auto inside = [&reaches, leader](G3D::Vector3 const& point, bool& unseen) -> Creature*
        {
            for (MobReach const& entry : reaches)
            {
                if ((!entry.otherFloor ||
                     std::fabs(point.z - entry.mob->GetPositionZ()) <= APPROACH_OTHER_FLOOR_DZ) &&
                    AggroesAtPoint(entry.mob, leader, point, entry.holds, true))
                {
                    unseen = false;
                    return entry.mob;
                }
                if (std::fabs(point.z - entry.mob->GetPositionZ()) <= SAME_FLOOR_DZ &&
                    AggroesAtPoint(entry.mob, leader, point, entry.unseenHolds, false) &&
                    !UnseenRouteEnd(leader, entry.mob))
                {
                    unseen = true;
                    return entry.mob;
                }
            }
            return nullptr;
        };

        // The route sampled every APPROACH_SAMPLE_STEP yd up to `length`, from the leader's own position.
        std::vector<G3D::Vector3> samples;
        samples.emplace_back(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        float walked = 0.0f;
        for (size_t index = 1; index < points.size() && walked < length; ++index)
        {
            G3D::Vector3 const from = index == 1 ? samples.front() : points[index - 1];
            G3D::Vector3 const segment = points[index] - from;
            float const segmentLength = segment.length();
            if (segmentLength <= 0.0f)
                continue;

            float along = 0.0f;
            while (along < segmentLength && walked + along < length)
            {
                along = std::min(along + APPROACH_SAMPLE_STEP, segmentLength);
                samples.push_back(from + segment * (along / segmentLength));
            }
            walked += segmentLength;
        }

        // [stop_only_real_aggro] User 2026-10-25 (scholo-h 153805, 15:56:04: approach_stop on a Necromancer pack
        // at 28.8 yd, insideRealAggro false, the tank stood with nothing to do): a mob whose real aggro range
        // (RealAggroRange plus STOP_REAL_SLACK; on its floor without line of sight, the follower rule, else seen)
        // this stretch of the route never enters stops no walk - its margin is only passed. The followers keep
        // to the tank's distance from an idle mob (MovementActions CollectIdleReaches [melee_behind_tank]) and
        // the pets out of every reach ([pet_idle_reach]). A route that enters the real range later stops there
        // on a later tick, at the margin point it stands in then.
        if (ROUTE_STOP_ONLY_REAL_AGGRO)
        {
            reaches.erase(std::remove_if(reaches.begin(), reaches.end(),
                                         [&samples, leader](MobReach const& entry)
                                         {
                                             float const real = RealAggroRange(entry.mob, leader) + STOP_REAL_SLACK;
                                             for (G3D::Vector3 const& point : samples)
                                             {
                                                 float const dz = std::fabs(point.z - entry.mob->GetPositionZ());
                                                 if (dz <= SAME_FLOOR_DZ &&
                                                     AggroesAtPoint(entry.mob, leader, point, real, false))
                                                     return false;
                                                 if ((!entry.otherFloor || dz <= APPROACH_OTHER_FLOOR_DZ) &&
                                                     AggroesAtPoint(entry.mob, leader, point, real, true))
                                                     return false;
                                             }
                                             return true;
                                         }),
                          reaches.end());
            if (reaches.empty())
                return false;
        }

        for (size_t index = 0; index < samples.size(); ++index)
        {
            bool unseen = false;
            blocker = inside(samples[index], unseen);
            if (!blocker)
                continue;

            // The spread stop is the last point before the spread reach (nothing to see from there anyway).
            unseenStop = unseen;
            size_t stopIndex =
                index > 0 && (unseen || SeesFromPoint(leader, blocker, samples[index - 1])) ? index - 1 : index;
            // The tank chooses no pull farther above or below it than AggressiveTargetValue's height limit: on a
            // ramp the stop moves on down within the margin, still outside every mob's real aggro range (ST
            // wave 15: stopped 12.3 yd above the Witch Doctor, the tank never saw it, the run stalled there).
            if (std::fabs(samples[stopIndex].z - blocker->GetPositionZ()) > TANK_PULL_HEIGHT)
                for (size_t next = stopIndex + 1; next < samples.size(); ++next)
                {
                    G3D::Vector3 const& point = samples[next];
                    bool aggroes = false;
                    for (MobReach const& entry : reaches)
                    {
                        bool zGated = false;  // [b34_petramp] the gate keeps only the tank idle there
                        aggroes |= AggroesAtPoint(entry.mob, leader, point, entry.reach - APPROACH_AGGRO_MARGIN,
                                                  false, &zGated) ||
                                   (ROUTE_STOP_IGNORES_Z_GATE && zGated);
                    }
                    if (aggroes)
                        break;
                    if (std::fabs(point.z - blocker->GetPositionZ()) <= TANK_PULL_HEIGHT &&
                        SeesFromPoint(leader, blocker, point))
                    {
                        stopIndex = next;
                        break;
                    }
                }
            // A corner that shows the mob only from inside its real aggro range (no margin) is no place to stop:
            // the mob sees the leader there and comes (Stratholme 09:36: the street corner at 3644.9,-3538.5 was
            // the stop with the cadaver it showed 15.6-18.9 yd away, inside its 18-19 yd real reach). The stop
            // goes back to the last point no idle mob both reaches and sees; the tank pulls from there or
            // the target is skipped as not seen (tank_view).
            auto const aggroesSeen = [&reaches, leader](G3D::Vector3 const& point)
            {
                for (MobReach const& entry : reaches)
                {
                    bool zGated = false;
                    if (AggroesAtPoint(entry.mob, leader, point, entry.reach - APPROACH_AGGRO_MARGIN, true,
                                       &zGated))
                        return true;
                    // [b34_petramp] inside the real 3D range, kept idle by the Z gate only: the mob sees the point
                    if (ROUTE_STOP_IGNORES_Z_GATE && zGated && SeesFromPoint(leader, entry.mob, point))
                        return true;
                }
                return false;
            };
            while (stopIndex > 0 && aggroesSeen(samples[stopIndex]))
                --stopIndex;
            stop = samples[stopIndex];
            // [b34_petramp] the leader stands inside such a reach already: back along its own trail (walked, it
            // pulls nothing new) to the first point no idle mob reaches and sees, on its floor
            if (ROUTE_STOP_IGNORES_Z_GATE && stopIndex == 0 && aggroesSeen(samples[0]))
            {
                std::vector<G3D::Vector3> const trail = MovementAction::TankTrailPoints(leader);
                G3D::Vector3 previous = samples[0];
                float walkedBack = 0.0f;
                for (auto point = trail.rbegin(); point != trail.rend(); ++point)
                {
                    walkedBack += (*point - previous).length();
                    previous = *point;
                    if (walkedBack > STOP_TRAIL_BACK_MAX)
                        break;
                    if (std::fabs(point->z - samples[0].z) > STOP_TRAIL_FLOOR || aggroesSeen(*point))
                        continue;
                    LOG_INFO("playerbots",
                             "[approach] {}: stop_out_of_real_reach: the leader stands where an idle mob reaches "
                             "and sees it (blocker {}, {:.1f} yd) - back {:.1f} yd along its trail to ({:.1f},"
                             "{:.1f},{:.1f}) [b34_petramp]",
                             leader->GetName(), blocker->GetName(), leader->GetExactDist(blocker), walkedBack,
                             point->x, point->y, point->z);
                    stop = *point;
                    break;
                }
            }
            return true;
        }
        return false;
    }

    // ROUTE_HARNESS_WALK_GUARD: the stop of a harness walk before the aggro reach of the idle mobs on its route
    // (ApproachStop around the leader, or around exemptPack, whose own pack is exempt). True: `point` is the stop,
    // `held`: the leader stands there already.
    bool HarnessWalkStop(Player* leader, Movement::PointsArray const& points, float length, Creature* exemptPack,
                         G3D::Vector3& point, Creature*& blocker, bool& held, Leader::LeaderGroup* group)
    {
        G3D::Vector3 stop;
        bool unseenStop = false;
        blocker = nullptr;
        held = false;
        WorldObject* const centre = exemptPack ? static_cast<WorldObject*>(exemptPack) : leader;
        if (!ApproachStop(leader, centre, points, length, stop, blocker, unseenStop, exemptPack, group) || !blocker)
            return false;
        G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        held = (stop - here).length() <= APPROACH_ARRIVED;
        point = stop;
        return true;
    }

    std::unordered_map<uint64, uint32> harnessHoldLogMs;  // blocker -> last harness_hold_refused_pack, world thread

    std::unordered_map<ObjectGuid, HoldReady> holdReady;

    std::unordered_map<ObjectGuid, PullAfterHoldGate> pullAfterHoldGates;  // by leader

    // [b33_bodypull] The classes of a planner refusal (CarefulPlanVerdict's why, ChooseTargetActions.cpp) the
    // harness reads before a walk-in. A READINESS refusal (the healer not with the tank / the group, a member
    // missing, dead, sitting below its line or buffing, not topped up) is a wait the walk-in can never end: no
    // pull_after_hold clock nor the planner's time-out overrides it, the group holds. An OVER-CAP refusal (the
    // pack does not fit, over the cap, a split search still running) is the planner's own [b32_smpull] split /
    // guarantee to take: the harness never walks the whole group into it (strat-h 20261026-000434-5D-1: a 4-6 mob
    // Ravaged Cadaver blocker the planner held as "does not fit" chained to 17 on the walk-in). Only the rest -
    // the stuck route / no plan from range class ("no pull of X from range ... the route's body pull decides",
    // "given up for range", a within-cap pack the stand's guarantee takes) - is the harness's walk-in to make.
    bool RefusalIsReadiness(std::string const& why)
    {
        static char const* const READINESS[] = {
            "not topped up", "not all here", "not with the group", "not with the tank", "from the tank",
            "rests below its line", " buffing", "the group rests or gathers", "group not ready",
            "a group member still fights", "the tank rests", "waits for ", "resurrection sickness", " dead",
            "no dps with the group", "% mana", "% health"};
        for (char const* const part : READINESS)
            if (why.find(part) != std::string::npos)
                return true;
        return false;
    }

    std::unordered_map<ObjectGuid, DoorWalk> doorWalks;  // by leader

    std::unordered_map<ObjectGuid, PlannerAsk> plannerAsks;

    // True while the planner still has its time (the caller holds the leader); `first`: the ask began now.
    bool AskPlannerFirst(ObjectGuid leaderGuid, Creature* target, uint32 now, bool& first)
    {
        first = false;
        PlannerAsk& ask = plannerAsks[leaderGuid];
        if (ask.target != target->GetGUID() || !ask.sinceMs ||
            getMSTimeDiff(ask.sinceMs, now) > 4 * BODY_PULL_PLANNER_MS)
        {
            ask.target = target->GetGUID();
            ask.sinceMs = now ? now : 1;
            first = true;
        }
        if (getMSTimeDiff(ask.sinceMs, now) >= BODY_PULL_PLANNER_MS)
            return false;
        GuaranteeRequest::Set(leaderGuid, target->GetGUID(), false);
        return true;
    }

    std::unordered_map<ObjectGuid, BehindRootTry> behindRootTries;

    std::unordered_map<ObjectGuid, uint32> pullEstimateSeen;

    // The first point of the route ahead (from the leader, up to APPROACH_SIGHT_PATH yd) from which the tank's eye
    // line reaches the blocker on a pullable height (TANK_PULL_HEIGHT) - the corner the stop lay before. A point
    // inside the real aggro range of the blocker is fine only for a pack the tank's planner accepted (keepOut
    // empty): walking into its sight is the pull. keepOut (ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED): the idle mobs of the
    // target's and the blocker's packs while no plan for them is accepted - a point inside the aggro reach
    // (AggroReach, the margin included) of one that sees it is a body pull of a pack the planner did not take:
    // the search ends there, heldAtEdge (the route only goes deeper from there). For a blocker outside the
    // target's pack, no point from which the target sees the leader inside its real aggro range (one pack at a
    // time). At most APPROACH_SIGHT_TRIES line-of-sight tests.
    bool FindSightPoint(Player* leader, Creature* blocker, Creature* target, bool ownPack,
                        Movement::PointsArray const& points, std::vector<Creature*> const& keepOut,
                        G3D::Vector3& sight, bool& heldAtEdge)
    {
        heldAtEdge = false;
        G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        float const targetAggro = RealAggroRange(target, leader);
        uint32 tries = 0;
        uint32 keepOutTries = 0;
        float walked = 0.0f;
        for (size_t index = 1; index < points.size() && walked < APPROACH_SIGHT_PATH; ++index)
        {
            G3D::Vector3 const from = index == 1 ? here : points[index - 1];
            G3D::Vector3 const segment = points[index] - from;
            float const segmentLength = segment.length();
            if (segmentLength <= 0.0f)
                continue;

            float along = 0.0f;
            while (along < segmentLength && walked + along < APPROACH_SIGHT_PATH)
            {
                along = std::min(along + APPROACH_SAMPLE_STEP, segmentLength);
                G3D::Vector3 const point = from + segment * (along / segmentLength);
                // Every point of the walk, not only the one that shows the blocker: the way there passes them. As
                // in ApproachStop, a leader standing in a mob's margin already is held only by a step closer to
                // it (or one into its real aggro range).
                for (Creature* mob : keepOut)
                {
                    // [sight-arc] the real aggro distance + the shared small margin (was AggroReach: +5 yd)
                    float const real = RealAggroRange(mob, leader);
                    float const holds = std::max(
                        std::min(real + MovementAction::SIGHT_IDLE_AGGRO_MARGIN, leader->GetExactDist(mob)), real);
                    bool zGated = false;  // [b34_petramp] the Z gate keeps only the tank idle there
                    if (!AggroesAtPoint(mob, leader, point, holds, false, &zGated) &&
                        !(ROUTE_STOP_IGNORES_Z_GATE && zGated))
                        continue;
                    // Out of tests: not shown to be safe, the leader holds.
                    if (keepOutTries++ >= APPROACH_SIGHT_KEEP_OUT_TRIES || SeesFromPoint(leader, mob, point))
                    {
                        heldAtEdge = true;
                        return false;
                    }
                }
                if (std::fabs(point.z - blocker->GetPositionZ()) > TANK_PULL_HEIGHT)
                    continue;
                if (tries++ >= APPROACH_SIGHT_TRIES)
                    return false;
                if (!SeesFromPoint(leader, blocker, point))
                    continue;
                if (!ownPack && AggroesAtPoint(target, leader, point, targetAggro, false))
                {
                    if (tries++ >= APPROACH_SIGHT_TRIES)
                        return false;
                    if (SeesFromPoint(leader, target, point))
                        return false;  // the target's sight comes first: no way to the blocker alone
                }
                sight = point;
                return true;
            }
            walked += segmentLength;
        }
        return false;
    }

    std::vector<Creature*> SightKeepOutOthers(Player* leader, Creature* target)
    {
        std::list<Creature*> mobs;
        OtherPackMobCheck check{leader, target};
        Acore::CreatureListSearcher<OtherPackMobCheck> searcher(leader, mobs, check);
        Cell::VisitObjects(leader, searcher, SIGHT_GIVE_WAY_SCAN);
        return std::vector<Creature*>(mobs.begin(), mobs.end());
    }

    // The idle hostiles (IdleAggroCheck) within PULL_IDLE_SCAN of the leader or of the pack's mob.
    std::vector<Creature*> PullIdleMobs(Player* leader, Creature* target)
    {
        std::vector<Creature*> result;
        IdleAggroCheck check{leader};
        for (WorldObject* centre : {static_cast<WorldObject*>(leader), static_cast<WorldObject*>(target)})
        {
            std::list<Creature*> found;
            Acore::CreatureListSearcher<IdleAggroCheck> searcher(centre, found, check);
            Cell::VisitObjects(centre, searcher, PULL_IDLE_SCAN);
            for (Creature* mob : found)
                if (std::find(result.begin(), result.end(), mob) == result.end())
                    result.push_back(mob);
        }
        if (check(target) && std::find(result.begin(), result.end(), target) == result.end())
            result.push_back(target);
        return result;
    }

    // The pack itself and its links: the target's pack (PACK_RADIUS), its formation, and what answers their
    // call for assistance (CanAnswerAssist with the faction test, within the assistance radius of a touched mob
    // plus ASSIST_LINK_MARGIN, in its sight), grown until nothing new comes.
    // build 27 (20261025-150737-mgt-h-5A-14): the assistance test had ASSIST_LINK_MARGIN on top (13 yd + both
    // sizes) and linked Selin's side packs to the centre pair (15.1 yd apart; the core's 10 yd + sizes = 13.3 does
    // not), so the "own pack" grew to the pair and every arc spot was "inside the pack's aggro". The core's own
    // radius now (AnyAssistCreatureInRangeCheck: IsWithinDistInMap, sizes included), and the own pack (`assist`
    // false: pack radius and formation) is told apart from the links it may call (`assist` true).
    void PullTouchClosure(Player* leader, std::vector<Creature*> const& idle, std::vector<Creature*>& touched,
                          bool withAssist)
    {
        float const assist =
            withAssist ? sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS) : 0.0f;
        for (size_t index = 0; index < touched.size() && touched.size() < 60; ++index)
        {
            Creature* source = touched[index];
            for (Creature* other : idle)
            {
                if (other == source || std::find(touched.begin(), touched.end(), other) != touched.end())
                    continue;
                bool const formation = source->GetFormation() && other->GetFormation() == source->GetFormation();
                bool const pack = other->GetExactDist(source) <= PACK_RADIUS && other->IsHostileTo(leader);
                bool const assists = assist > 0.0f && CanAnswerAssist(other, source, leader, true) &&
                                     source->IsWithinDistInMap(other, assist) &&
                                     source->IsWithinLOSInMap(other);
                if (formation || pack || assists)
                    touched.push_back(other);
            }
        }
    }

    // The mobs a pull of `target` brings when the leader walks `walk` (the spot at its end): the target and its
    // links, every idle mob whose real aggro distance (+ SIGHT_IDLE_AGGRO_MARGIN) a walk point enters and that
    // sees the point (AggroesAtPoint: the core's Z gate, 3D range, the eye line), and their links. `intended`
    // gets the target with its links alone; `isBoss` tells a boss.
    PullTouch PullTouches(Player* leader, Creature* target, Movement::PointsArray const& walk,
                          std::vector<Creature*> const& idle, std::function<bool(Creature*)> const& isBoss,
                          std::vector<Creature*>& intended)
    {
        intended.clear();
        intended.push_back(target);
        PullTouchClosure(leader, idle, intended, false);

        PullTouch result;
        result.mobs = intended;
        PullTouchClosure(leader, idle, result.mobs);  // what the pack itself calls
        std::vector<Creature*> woken;
        for (size_t i = 1; i < walk.size(); ++i)
        {
            G3D::Vector3 const from = walk[i - 1];
            G3D::Vector3 const segment = walk[i] - from;
            float const length = segment.length();
            uint32 const steps = std::max<uint32>(1, uint32(std::ceil(length / PULL_TOUCH_STEP)));
            for (uint32 step = 0; step <= steps; ++step)
            {
                G3D::Vector3 const point = from + segment * (float(step) / float(steps));
                for (Creature* mob : idle)
                {
                    if (std::find(result.mobs.begin(), result.mobs.end(), mob) != result.mobs.end() ||
                        std::find(woken.begin(), woken.end(), mob) != woken.end())
                        continue;
                    if (AggroesAtPoint(mob, leader, point,
                                       RealAggroRange(mob, leader) + MovementAction::SIGHT_IDLE_AGGRO_MARGIN,
                                       true))
                        woken.push_back(mob);
                }
            }
        }
        if (!woken.empty())
        {
            PullTouchClosure(leader, idle, woken);
            for (Creature* mob : woken)
                if (std::find(result.mobs.begin(), result.mobs.end(), mob) == result.mobs.end())
                    result.mobs.push_back(mob);
        }
        for (Creature* mob : result.mobs)
        {
            if (std::find(intended.begin(), intended.end(), mob) == intended.end())
            {
                if (!result.extra)
                    result.firstExtra = mob->GetName();
                ++result.extra;
            }
            if (!result.boss && isBoss(mob))
            {
                result.boss = true;
                result.bossName = mob->GetName();
            }
        }
        return result;
    }

    bool PullTouchBetter(PullTouch const& a, PullTouch const& b)
    {
        if (a.boss != b.boss)
            return !a.boss;
        return a.mobs.size() < b.mobs.size();
    }

    void FindPullSightSpotWith(Player* leader, Creature* target, std::vector<Creature*> const& idle,
                               std::function<bool(Creature*)> const& isBoss, float margin, PullSightSearch& out)
    {
        out = PullSightSearch();
        out.margin = margin;
        out.minRadius = RealAggroRange(target, leader) + margin;
        out.maxRadius = PULL_SIGHT_RANGE - PULL_SIGHT_RANGE_SLACK;
        if (out.minRadius > out.maxRadius)
            out.minRadius = out.maxRadius;

        // The own pack: the target, its pack radius and formation - not the packs its call may bring (those are
        // touched mobs, counted by PullTouches).
        std::vector<Creature*> intended;
        intended.push_back(target);
        PullTouchClosure(leader, idle, intended, false);

        // The arc round every member of the pack (the nearest to the leader first): a spot must stay outside
        // the aggro of all of them, and any member it sees within the pull range is the one to pull. Round the
        // target alone, every ring lay inside a neighbour member's aggro (build 27: 16-27 of 16-27 "inOwnAggro").
        std::vector<Creature*> centres = intended;
        std::sort(centres.begin(), centres.end(), [leader](Creature* a, Creature* b)
                  { return leader->GetExactDist(a) < leader->GetExactDist(b); });
        if (centres.size() > PULL_SIGHT_MAX_CENTRES)
            centres.resize(PULL_SIGHT_MAX_CENTRES);
        uint32 const losPerCentre = std::max<uint32>(12, PULL_SIGHT_MAX_LOS / uint32(centres.size()));
        std::vector<MovementAction::SightArcCandidate> spots;
        for (Creature* centre : centres)
        {
            float const minRadius = std::min(RealAggroRange(centre, leader) + margin, out.maxRadius);
            for (MovementAction::SightArcCandidate const& spot : MovementAction::SightArcCandidates(
                     leader, centre, minRadius, out.maxRadius, TANK_PULL_HEIGHT, losPerCentre))
            {
                bool const duplicate = std::any_of(spots.begin(), spots.end(), [&spot](auto const& other)
                                                   { return std::fabs(other.x - spot.x) < 0.5f &&
                                                            std::fabs(other.y - spot.y) < 0.5f; });
                if (!duplicate)
                    spots.push_back(spot);
            }
        }
        std::sort(spots.begin(), spots.end(),
                  [](auto const& a, auto const& b) { return a.straight < b.straight; });
        out.candidates = uint32(spots.size());
        uint32 paths = 0;
        auto const note = [&out](PullSightCandidate const& candidate)
        {
            if (out.top.size() < 3)
                out.top.push_back(candidate);
        };
        for (MovementAction::SightArcCandidate const& spot : spots)
        {
            PullSightCandidate candidate;
            candidate.at = G3D::Vector3(spot.x, spot.y, spot.z);
            candidate.straight = spot.straight;
            candidate.radius = spot.radius;
            candidate.angleOffDeg = spot.angleOffDeg;
            // A walk is never shorter than the straight line: nothing farther beats the best one.
            if (out.found && candidate.straight >= out.best.walk)
                break;

            // The spot itself: inside the pack's own aggro distance it is a body pull, not a sight pull;
            // inside another idle mob's (that sees it) it pulls that one.
            Creature* own = nullptr;
            for (Creature* mob : intended)
                if (AggroesAtPoint(mob, leader, candidate.at, RealAggroRange(mob, leader) + margin, false))
                {
                    own = mob;
                    break;
                }
            if (own)
            {
                ++out.inOwnAggro;
                candidate.verdict = "inside the pack's aggro (" + own->GetName() + ")";
                note(candidate);
                continue;
            }
            Creature* other = nullptr;
            for (Creature* mob : idle)
                if (std::find(intended.begin(), intended.end(), mob) == intended.end() &&
                    AggroesAtPoint(mob, leader, candidate.at, RealAggroRange(mob, leader) + margin, true))
                {
                    other = mob;
                    break;
                }
            if (other)
            {
                ++out.inOtherAggro;
                candidate.verdict = Acore::StringFormat("in the aggro of idle {} ({:.1f} yd of {:.1f})",
                                                        other->GetName(),
                                                        other->GetExactDist(candidate.at.x, candidate.at.y,
                                                                            candidate.at.z),
                                                        RealAggroRange(other, leader) + margin);
                note(candidate);
                continue;
            }

            if (paths >= PULL_SIGHT_MAX_PATHS)
                break;
            ++paths;
            PathResult const path = RoutePath(leader, candidate.at.x, candidate.at.y, candidate.at.z);
            if (!path.reachable || path.points.size() < 2 ||
                (path.end - candidate.at).length() > PULL_SIGHT_PATH_END ||
                path.length > std::max(PULL_SIGHT_WALK_RATIO * candidate.straight,
                                       candidate.straight + PULL_SIGHT_WALK_EXTRA))
            {
                ++out.noPath;
                candidate.verdict = path.reachable ? Acore::StringFormat("detour ({:.0f} yd walk)", path.length)
                                                   : std::string("no path");
                note(candidate);
                continue;
            }
            candidate.walk = path.length;
            Movement::PointsArray walk = path.points;
            walk.push_back(candidate.at);
            std::vector<Creature*> intendedHere;
            candidate.touch = PullTouches(leader, target, walk, idle, isBoss, intendedHere);
            if (candidate.touch.boss || candidate.touch.extra)
            {
                if (candidate.touch.boss)
                    ++out.touchesBoss;
                else
                    ++out.touchesMore;
                candidate.verdict =
                    candidate.touch.boss
                        ? "touches boss " + candidate.touch.bossName
                        : Acore::StringFormat("the walk touches {} more ({})", candidate.touch.extra,
                                              candidate.touch.firstExtra);
                if (!out.hasFallback || PullTouchBetter(candidate.touch, out.fallback.touch))
                {
                    out.hasFallback = true;
                    out.fallback = candidate;
                }
                note(candidate);
                continue;
            }

            candidate.verdict = "ok";
            note(candidate);
            if (!out.found || candidate.walk < out.best.walk)
            {
                out.found = true;
                out.best = candidate;
            }
        }
    }

    // The usual margin first, then the tight one; the fallback (a spot that touches more) of either pass.
    void FindPullSightSpot(Player* leader, Creature* target, std::vector<Creature*> const& idle,
                           std::function<bool(Creature*)> const& isBoss, PullSightSearch& out)
    {
        FindPullSightSpotWith(leader, target, idle, isBoss, MovementAction::SIGHT_IDLE_AGGRO_MARGIN, out);
        if (out.found)
            return;
        PullSightSearch tight;
        FindPullSightSpotWith(leader, target, idle, isBoss, PULL_SIGHT_TIGHT_MARGIN, tight);
        if (tight.found || (tight.hasFallback && !out.hasFallback))
            out = tight;
    }

    // The JSON of a search's candidates (sight_point_search / pull_option events).
    std::string PullCandidatesJson(std::vector<PullSightCandidate> const& top)
    {
        JsonArray array;
        for (PullSightCandidate const& candidate : top)
            array.Raw(JsonObject()
                          .Real("x", candidate.at.x).Real("y", candidate.at.y).Real("z", candidate.at.z)
                          .Real("straight", candidate.straight).Real("radius", candidate.radius)
                          .Real("angleOff", candidate.angleOffDeg).Real("walk", candidate.walk)
                          .Num("mobs", int64(candidate.touch.mobs.size())).Bool("boss", candidate.touch.boss)
                          .Str("verdict", candidate.verdict)
                          .Build());
        return array.Build();
    }

    std::map<std::pair<uint32, ObjectGuid>, uint32> bossAreaBodyPullHoldMs;  // {party seq, boss} -> first hold

    // The next boss of the party's last route (Retarget's RouteSnapshot), nullptr when none is alive.
    Creature* RouteNextBoss(uint32 seq, Map* map)
    {
        auto const found = routeSnapshots.find(seq);
        if (!map || found == routeSnapshots.end() || found->second.nextBoss.IsEmpty())
            return nullptr;
        Creature* boss = map->GetCreature(found->second.nextBoss);
        return boss && boss->IsAlive() ? boss : nullptr;
    }

    std::unordered_map<uint32, uint32> lockedInSince;

    // [dmw-guard-body-pull] A living guard of a guarded door that is still closed (PendingDoorGuards).
    bool IsPendingDoorGuard(Map* map, Creature const* creature)
    {
        if (!map || !creature)
            return false;
        for (GuardedDoorDef const* def : GuardedDoors(map->GetId()))
        {
            if (std::find(def->guardEntries.begin(), def->guardEntries.end(), creature->GetEntry()) ==
                def->guardEntries.end())
                continue;
            for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                if (go && go->isSpawned() && go->GetEntry() == def->doorEntry)
                {
                    std::vector<Creature*> const guards = PendingDoorGuards(map, go);
                    if (std::find(guards.begin(), guards.end(), creature) != guards.end())
                        return true;
                }
        }
        return false;
    }

    void KeepTankOutOfClosedAreas(Player* tank, std::vector<AreaGateDef const*> const& gates)
    {
        if (gates.empty() || !tank->IsInWorld())
            return;
        std::list<Creature*> mobs;
        ClosedAreaMobCheck check{tank, gates};
        Acore::CreatureListSearcher<ClosedAreaMobCheck> searcher(tank, mobs, check);
        Cell::VisitObjects(tank, searcher, CLOSED_AREA_SKIP_RANGE);
        if (mobs.empty())
            return;
        std::vector<Creature*> nearest(mobs.begin(), mobs.end());
        std::sort(nearest.begin(), nearest.end(), [tank](Creature* left, Creature* right)
                  { return tank->GetExactDistSq(left) < tank->GetExactDistSq(right); });
        if (nearest.size() > CLOSED_AREA_SKIP_MAX)
            nearest.resize(CLOSED_AREA_SKIP_MAX);
        for (Creature* mob : nearest)
            CarefulPullSkip::Mark(tank, mob->GetGUID());
    }

    // The bots' own raid-leader pull hold (LeaderValues PullHoldValue) holds the leader now.
    bool LeaderPullHeld(Player* leader)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(leader);
        Value<bool>* hold =
            botAI ? dynamic_cast<Value<bool>*>(botAI->GetAiObjectContext()->GetUntypedValue("pull hold")) : nullptr;
        return hold && hold->Get();
    }

    bool IsRouteBlocker(LeaderGroup& group, ObjectGuid guid)
    {
        GroupRouteState& routeState = group.Route();
        return ROUTE_BLOCKER_BODY_PULL && !guid.IsEmpty() && routeState.routeBlockerUntilMs &&
               int32(getMSTime() - routeState.routeBlockerUntilMs) < 0 &&
               std::find(routeState.routeBlockerPack.begin(), routeState.routeBlockerPack.end(), guid) !=
                   routeState.routeBlockerPack.end();
    }

    bool RouteBlockerBehindRequired(LeaderGroup& group, Map* map, WorldObject const* behind,
                                    std::string& why)
    {
        GroupRouteState& routeState = group.Route();
        if (!behind)
        {
            why = "nothing behind";
            return false;
        }
        Creature const* creature = behind->ToCreature();
        // a gate, a key object, an event's object: the run cannot go on without it
        if (!creature)
        {
            why = "object";
            return true;
        }
        if (behind->GetGUID() == routeState.target && (routeState.targetIsBoss || routeState.targetIsStarter))
        {
            why = routeState.targetIsBoss ? "boss" : "event starter";
            return true;
        }
        if (creature->IsDungeonBoss() || creature->isWorldBoss() ||
            routeState.bossCreatures.count(creature->GetGUID()) ||
            group.BossOrder(creature) >= 0)
        {
            why = "boss";
            return true;
        }
        if (IsPendingDoorGuard(map, creature))
        {
            why = "door guard";
            return true;
        }
        // [banished_boss_gate] the mobs by a boss immune to players (his banish keepers: Hellmaw's Cabal Ritualists)
        // open his fight: never "optional"
        {
            std::list<Creature*> nearMobs;
            PackMobCheck check{ObjectAccessor::FindPlayer(group.LeaderGuid()), const_cast<Creature*>(creature)};
            if (check.leader)
            {
                Acore::CreatureListSearcher<PackMobCheck> searcher(creature, nearMobs, check);
                Cell::VisitObjects(creature, searcher, PACK_RADIUS);
                for (Creature* mob : nearMobs)
                    if (mob != creature && (mob->IsDungeonBoss() || routeState.bossCreatures.count(mob->GetGUID())) &&
                        mob->IsImmuneToPC())
                    {
                        why = "banish keeper";
                        return true;
                    }
            }
        }
        // [boss_area_clear] trash round the next boss is cleared before him: never skipped as "optional"
        if (ROUTE_BOSS_AREA_CLEAR)
            if (Player* leader = ObjectAccessor::FindPlayer(group.LeaderGuid()))
                if (Creature* areaBoss = RouteNextBoss(group.Run().runSeq, map))
                    if (char const* area = BossAreaTrashOf(leader, areaBoss, const_cast<Creature*>(creature),
                                                           routeState.bossCreatures.count(creature->GetGUID()) != 0))
                    {
                        why = area;
                        return true;
                    }
        if (int32 const spotIdx = EventSpotIndex(group); spotIdx >= 0)
            if (EventSpotDef const* spot = EventSpots(group.MapId())[size_t(spotIdx)]; spot && spot->hold &&
                std::find(spot->holdAlways.begin(), spot->holdAlways.end(), creature->GetEntry()) !=
                    spot->holdAlways.end())
            {
                why = "event keeper";
                return true;
            }
        // the route rule that chose it (Retarget): an objective's key, the boss's own fight, the event's way
        if (behind->GetGUID() == routeState.target)
        {
            std::string const& rule = routeState.targetRouteRule;
            if (rule == "required" || rule == "boss" || rule == "event starter" || rule == "event chain" ||
                rule == "encounter" || rule == "fled survivor" || rule == "phantom mob hunt" ||
                rule.rfind("joins boss", 0) == 0 || rule.rfind("hold guard", 0) == 0)
            {
                why = rule;
                return true;
            }
            why = rule.empty() ? std::string("trash (no route rule)") : "trash (" + rule + ")";
            return false;
        }
        why = "trash (not the target)";
        return false;
    }

    // [queue-blocker-pull-gate] WIPE_ANALYSIS_w24_25: scholo-n w24 walked the tank to 17 yd of a 20 yd aggro radius
    // into a pack the planner had REFUSED ("refused, 97 mobs") once the edge hold "gave way" after 8 s; the body pull
    // and the blocker retarget had their own, different checks. One gate now:
    // - the planner's chain count did not refuse the pack as hopeless (CarefulPlanVerdict::GetRefusal: its smallest
    //   pull is over the over-cap scale - "no safe pull: X brings N mobs, over M");
    // - the group is ready (GroupFullyReady: everyone here, health and healer mana up) - else hold, no skip;
    // - a body pull only for a blocker on the only way to a boss / event / required target (IsRouteBlocker, or
    //   RouteBlockerBehindRequired - [boss_area_clear] trash included).
    // The sight pull ([pull_option] (1), FindPullSightSpot) runs before any of these and is not gated here.
    std::string BlockerPullGate(LeaderGroup& group, Player* leader, Creature* blocker,
                                WorldObject const* behind, bool bodyPull, bool& hold)
    {
        GroupRouteState& routeState = group.Route();
        hold = false;
        if (!blocker)
            return "no blocker";
        // [cos-event-trash] the idle citizens outside Arthas' event are walked past while the Guardian of Time runs
        if (leader->GetMapId() == CoSIds::MAP_CULLING_OF_STRATHOLME &&
            CoSHelpers::IsSkippableEventTrash(leader, blocker))
            return "idle event trash (Culling heroic, Guardian of Time running)";
        bool hopeless = false;
        uint32 waitMs = 0;
        std::string refusalWhy;
        bool refused =
            PartyRefusal(group, blocker->GetGUID(), hopeless, waitMs, refusalWhy);  // [lead5]
        // [b34_strat] the tank keeps ONE verdict, about the mob it planned: strat-h 20261026-014521-strat-h-5E-1
        // (299 s) refused the target BEHIND ("Skeletal Guardian: no safe pull: Ravaged Cadaver brings 20 mobs, over
        // 18"), the lookup by the blocker's own guid found nothing and the Ravaged Cadaver became the route target
        // (302 s, "packMobs": 1 by PACK_RADIUS). The behind's over-cap refusal that names the blocker is its refusal.
        if (!refused && behind && behind != blocker && behind->ToCreature())
        {
            bool behindHopeless = false;
            uint32 behindWaitMs = 0;
            std::string behindWhy;
            if (PartyRefusal(group, behind->GetGUID(), behindHopeless, behindWaitMs,  // [lead5]
                                               behindWhy) &&
                RefusalIsOverCap(behindWhy) && behindWhy.find(blocker->GetName()) != std::string::npos)
            {
                refused = true;
                hopeless = behindHopeless;
                waitMs = behindWaitMs;
                refusalWhy = behindWhy;
            }
        }
        // [banished_boss_gate] the pack's only "boss" is banished / immune to players (its keepers' channel): the
        // planner's "brings a boss" refusal waits for a fight that only this pull can start (slabs-n 153805). Not a
        // refusal, for [drift_body_pull_planner_hold] below either: that hold stays for every real fresh refusal.
        if (refused && refusalWhy.find("brings a boss") != std::string::npos)
        {
            std::list<Creature*> pack;
            PackMobCheck check{leader, blocker};
            Acore::CreatureListSearcher<PackMobCheck> searcher(blocker, pack, check);
            Cell::VisitObjects(blocker, searcher, PACK_RADIUS);
            bool anyBoss = false;
            bool banishedOnly = true;
            for (Creature* mob : pack)
                if (mob->IsDungeonBoss() || routeState.bossCreatures.count(mob->GetGUID()))
                {
                    anyBoss = true;
                    banishedOnly = banishedOnly && mob->IsImmuneToPC();
                }
            if (anyBoss && banishedOnly)
                refused = false;
        }
        // [b35_bfd] the planner's boss hold ("the pack does not fit (a boss pull: no fallback plan)": its target was
        // the boss, which it never pulls, ChooseTargetActions.cpp [b30_topped_up]) is no over-cap verdict on this
        // trash: the boss's pull is the harness's (the body-pull walk stops at him, boss_on_the_way_first
        // [b30_stalls] makes him the target with his buffs and ready check). bfd-n 20261026-024524-bfd-n-5B-29: the
        // last Blindlight Muckdweller of Gelihast's room (40 yd, Gelihast 25 yd on the way) was held "over the
        // planner's cap (its split / guarantee)" by [b33_bodypull] every 10 s from 2041 s, stalled 2/7 at 2283 s
        // (build 31/33 bfd runs took this same walk into boss_on_the_way_first). Readiness stays checked below.
        if (refused && refusalWhy.find("(a boss pull: no fallback plan)") != std::string::npos && leader->GetMap())
            for (ObjectGuid const& bossGuid : routeState.bossCreatures)
                if (Creature* boss = leader->GetMap()->GetCreature(bossGuid);
                    boss && boss != blocker && boss->IsAlive() && !boss->IsInCombat() &&
                    (BossBroughtBy(leader, boss, blocker) || JoinsBossFight(leader, boss, blocker)))
                {
                    refused = false;
                    break;
                }
        // [b33_aoepull] a refused (or aoe-planned) pack on the only way to a required target is marked for the
        // planner's aoe_pull_plan; TargetRouteBlocker (bodyPull false) checked the required way before the gate
        bool const aoePlanned = PartyAoePlanned(group, blocker->GetGUID());  // [lead5]
        bool requiredBlocker = false;
        if ((refused || aoePlanned) && !blocker->IsDungeonBoss() && !routeState.bossCreatures.count(blocker->GetGUID()))
        {
            std::string requiredWhy;
            requiredBlocker = !bodyPull || IsRouteBlocker(group, blocker->GetGUID()) ||
                              RouteBlockerBehindRequired(group, leader->GetMap(), behind ? behind : blocker,
                                                         requiredWhy);
            if (requiredBlocker)
                group.MarkRequired(blocker->GetGUID());  // [lead5]
        }
        if (refused && hopeless && requiredBlocker &&
            AoePullAskRuns(group.LeaderGuid(), blocker->GetGUID(), getMSTime()))
        {
            hold = true;  // the planner weighs its aoe_pull_plan first; the dead end after AOE_PULL_ASK_MS
            return "held: the planner weighs an aoe_pull_plan of the required blocker (" + refusalWhy + ")";
        }
        if (refused && hopeless)
        {
            NoteHopelessPack(leader, blocker);  // [b32_brs] the walks go round it (HopelessDetourLeg)
            return "refused by the chain count (" + refusalWhy + ")";
        }
        // [drift_body_pull_planner_hold] scholo-n 20261025-150737-7 pull 12 (437 s): the planner held the Necromancer
        // pack "for now" (an overlap wait: the Risen Protector patrol crossed it, the planner waits to take the pack
        // without it), the gate let the body pull through because the refusal was not hopeless, the tank walked in
        // and ~15 mobs came (patrol, Adepts, Tutors), wipe at 517 s. A body pull never goes against a fresh planner
        // refusal: while the planner's own wait runs (its split, its stand's guarantee) the group holds; a refusal
        // still standing once that wait is over is refused here too (skipped, the route comes back to it).
        // [pull_after_hold] the group stood ready PULL_AFTER_HOLD_MS at this pack: the planner's wait is over
        // [b33_bodypull] ...but never over a readiness refusal (the healer not with the tank, sitters, not topped up)
        // nor an over-cap one (the planner's split / guarantee takes it, or calls it hopeless after its split search
        // and the detour above takes over): the walk-in of the whole group into a pack the planner says does not fit
        // is the strat-h 20261026-000434-5D-1 wipe (4-6 mobs at the walk-in, 17 in the fight)
        bool const readinessRefusal = refused && RefusalIsReadiness(refusalWhy);
        bool const overCapRefusal = refused && !readinessRefusal && RefusalIsOverCap(refusalWhy);
        if (refused && bodyPull && !readinessRefusal && !overCapRefusal &&
            PullAfterHoldPasses(group.LeaderGuid(), blocker->GetGUID()))
            refused = false;
        // [b34_strat] ...and the over-cap hold for the route TARGET too (TargetRouteBlocker, bodyPull false): it
        // cleared the pack's skips and walked the leader to the edge of a refused 17-26 mob chain (strat-h
        // 20261026-014521-5E-1, 302 s). The group holds where it stands while the planner splits or calls it
        // hopeless (then the chain-count branch above: the aoe ask, then the dead end and the skip).
        if (refused && !bodyPull && overCapRefusal)
        {
            hold = true;
            return "held: over the planner's cap, no route target (" + refusalWhy + ")";
        }
        if (refused && bodyPull && (readinessRefusal || overCapRefusal))
        {
            hold = true;  // nothing skipped: the group gets ready / the planner plans where it stands
            return Acore::StringFormat("{} ({})",
                                       readinessRefusal ? "held: the planner waits for the group"
                                                        : "held: over the planner's cap (its split / guarantee)",
                                       refusalWhy);
        }
        if (refused && bodyPull)
        {
            if (waitMs > 0)
            {
                hold = true;
                return Acore::StringFormat("held by the planner for {} s ({})", waitMs / IN_MILLISECONDS, refusalWhy);
            }
            return "refused by the planner (" + refusalWhy + ")";
        }
        if (!group.GroupFullyReady(leader))
        {
            hold = true;
            return "the group is not ready";
        }
        // [b33_bodypull] the planner's sitter gate for the walk-in too: nobody eats / drinks when the pack comes
        if (bodyPull)
            for (LeaderGroup::MemberInfo const& member : group.Members())
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid); bot && bot->IsAlive() && bot->IsSitState())
                {
                    hold = true;
                    return "the group is not ready: " + bot->GetName() + " sits";
                }
        if (bodyPull && !IsRouteBlocker(group, blocker->GetGUID()))
        {
            std::string requiredWhy;
            if (!RouteBlockerBehindRequired(group, leader->GetMap(), behind ? behind : blocker, requiredWhy))
                return "not on the only way to a boss or event (" + requiredWhy + ")";
        }
        return std::string();
    }

    bool TargetRouteBlocker(LeaderGroup& group, Player* leader, Creature* blocker, WorldObject const* behind,
                            std::string const& why)
    {
        GroupRouteState& routeState = group.Route();
        if (!ROUTE_BLOCKER_BODY_PULL || !blocker || !blocker->IsAlive() || blocker->IsInCombat() ||
            blocker->IsDungeonBoss() || routeState.bossCreatures.count(blocker->GetGUID()) ||
                group.BossOrder(blocker) >= 0)
            return false;

        // [route_blocker_required_only] User rule 2026-10-25: a pack on the only way to the next REQUIRED target
        // (boss, event, gate, key) is never skipped; a side / optional trash target never triggers it - the target
        // behind is skipped as before and the route takes another one. scholo-n wave 23 (112011, 473-529 s): a
        // 3-mob Scholomance Necromancer pack (the planner: 8 mobs at cost 2.38) was body-pulled for the trash target
        // Risen Aberration ("blocks route") and the group wiped.
        if (std::string requiredWhy; !RouteBlockerBehindRequired(group, leader->GetMap(), behind, requiredWhy))
        {
            LOG_INFO("playerbots", "[route_blocker_required_only] {}: {} stands on the way to {} - {}: no body pull, "
                     "the target is skipped as before", leader->GetName(), blocker->GetName(),
                     behind ? behind->GetName() : std::string("?"), requiredWhy);
            group.LogEvent("route_blocker_required_only",
                     JsonObject().Str("action", "not required, skipped as before").Str("blocker", blocker->GetName())
                         .Num("blockerEntry", blocker->GetEntry())
                         .Str("behind", behind ? behind->GetName() : std::string()).Str("behindRule", requiredWhy)
                         .Str("why", why));
            return false;
        }

        // [queue-blocker-pull-gate]
        bool gateHold = false;
        if (std::string const gate = BlockerPullGate(group, leader, blocker, behind, false, gateHold); !gate.empty())
        {
            group.LogEvent("blocker_pull_gate", JsonObject().Str("pull", "route blocker target")
                                                     .Str("blocker", blocker->GetName())
                                                     .Num("blockerEntry", blocker->GetEntry())
                                                     .Str("behind", behind ? behind->GetName() : std::string())
                                                     .Str("why", gate).Bool("hold", gateHold));
            if (gateHold)
            {
                TestRunner::ClearWaypoint(group.LeaderGuid());
                return true;  // the group gets ready where it stands, nothing skipped
            }
            return false;
        }

        std::list<Creature*> pack;
        PackMobCheck check{leader, blocker};
        Acore::CreatureListSearcher<PackMobCheck> searcher(blocker, pack, check);
        Cell::VisitObjects(blocker, searcher, PACK_RADIUS);
        if (std::find(pack.begin(), pack.end(), blocker) == pack.end())
            pack.push_back(blocker);
        // a boss in the pack is the boss's pull (the boss logic), not a trash blocker's
        for (Creature* mob : pack)
            if (mob->IsDungeonBoss() || routeState.bossCreatures.count(mob->GetGUID()) || group.BossOrder(mob) >= 0)
                return false;

        // [b32_pull] ... and so is a pack that comes into a boss's fight by itself (his formation, a script call):
        // the walk to it is the boss's pull without the boss's preparation. dmn-n 20261025-221216-dmn-n-5H-48: a
        // 2-mob Gordok Mastiff pack ("required: joins boss: script") became the target at 190 s, the leader walked to
        // it while the hunter still drank, Guard Mol'dar came at 200 s and his room chained 1 -> 9 -> 15.
        if (Map* map = leader->GetMap())
            for (ObjectGuid const& guid : routeState.bossCreatures)
            {
                Creature* boss = map->GetCreature(guid);
                if (!boss || !boss->IsAlive() || boss->IsInCombat())
                    continue;
                for (Creature* mob : pack)
                {
                    if (!BossEncounterMob(leader, boss, mob))
                        continue;
                    LOG_INFO("playerbots", "[route_blocker_body_pull] {}: {} joins {}'s fight: his pull, not a route "
                             "blocker's", leader->GetName(), mob->GetName(), boss->GetName());
                    group.LogEvent("route_blocker_boss_encounter",
                             JsonObject().Str("blocker", blocker->GetName()).Num("blockerEntry", blocker->GetEntry())
                                 .Str("mob", mob->GetName()).Str("boss", boss->GetName())
                                 .Str("behind", behind ? behind->GetName() : std::string()).Str("why", why));
                    return false;
                }
            }

        uint32& tries = routeState.routeBlockerTries[blocker->GetGUID()];
        if (tries >= ROUTE_BLOCKER_MAX_TRIES)
            return false;
        ++tries;

        uint32 const now = getMSTime();
        routeState.routeBlockerPack.clear();
        for (Creature* mob : pack)
        {
            routeState.routeBlockerPack.push_back(mob->GetGUID());
            routeState.skipUntil.erase(mob->GetGUID());
            routeState.skipReason.erase(mob->GetGUID());
        }
        routeState.routeBlockerUntilMs = now + NEIGHBOUR_TARGET_MS;
        LOG_INFO("playerbots", "[route_blocker_body_pull] {}: {} ({} mobs, {:.1f} yd) stands on the way to {}: the "
                 "target, pulled by sight or walked in ({}, try {})", leader->GetName(), blocker->GetName(),
                 pack.size(), leader->GetExactDist(blocker), behind ? behind->GetName() : std::string("?"), why,
                 tries);
        group.LogEvent("route_blocker_body_pull",
                 JsonObject().Str("action", "target").Str("blocker", blocker->GetName())
                     .Num("blockerEntry", blocker->GetEntry()).Num("packMobs", uint32(pack.size()))
                     .Str("behind", behind ? behind->GetName() : std::string()).Str("why", why).Num("try", tries)
                     .Real("blockerDistance", leader->GetExactDist(blocker)));
        sightStops.erase(group.LeaderGuid());
        sightHolds.erase(group.LeaderGuid());
        routeState.neighbourTarget = blocker->GetGUID();
        routeState.neighbourTargetUntilMs = now + NEIGHBOUR_TARGET_MS;
        routeState.target = blocker->GetGUID();
        routeState.targetIsBoss = false;
        routeState.targetIsStarter = false;
        routeState.targetRouteRule.clear();
        routeState.targetSinceMs = now;
        routeState.guaranteeHolding = false;
        routeState.arrivedMs = 0;
        routeState.approach = 0;
        routeState.approachBlocker.Clear();
        routeState.stopSinceMs = 0;
        return true;
    }

    void RouteBlockerBodyPull(LeaderGroup& group, Player* leader, Creature* target, std::string const& why)
    {
        GroupRouteState& routeState = group.Route();
        // [lead3] LEADER_NO_BODY_FALLBACK: no harness walk-in. The route blocker is the leader's to clear: asked of
        // its planner whole (the walk-in priced by the planner's own count, the aoe_pull_plan allowed: required), a
        // boss's pack as the boss pull. Its answer is the leader's (TickLeaderPlanner).
        if (Leader::Leads(Leader::LEADER_NO_BODY_FALLBACK) && group.LeaderDecides() && target)
        {
            routeState.approach = 0;
            if (target->IsDungeonBoss() || routeState.bossCreatures.count(target->GetGUID()))
                Leader::Dungeon::LeaderBossPull(group, leader, target, "route blocker: " + why);
            else
                Leader::Dungeon::LeaderWholePack(group, leader, target, "route blocker: " + why, true, true);
            return;
        }
        // the group gets ready first (the user rule: full health and mana, everyone here), then the walk in; the
        // leader holds where it stands meanwhile (TestHarnessMgr's regen wait tops the group up)
        // [queue-blocker-pull-gate] and never into a pack the chain count refused, nor off the required way
        bool gateHold = false;
        if (std::string const gate = BlockerPullGate(group, leader, target, nullptr, true, gateHold); !gate.empty())
        {
            TestRunner::ClearWaypoint(group.LeaderGuid());
            uint32 const nowMs = getMSTime();
            // [b33_bodypull] a hold is logged too (it was silent: the trace showed no reason for the stand)
            if (!routeState.blockerGateLogMs ||
                getMSTimeDiff(routeState.blockerGateLogMs, nowMs) > 10 * IN_MILLISECONDS)
            {
                routeState.blockerGateLogMs = nowMs ? nowMs : 1;
                group.LogEvent("blocker_pull_gate", JsonObject().Str("pull", "body pull")
                                                         .Str("blocker", target->GetName())
                                                         .Num("blockerEntry", target->GetEntry()).Str("why", gate)
                                                         .Str("was", why).Bool("hold", gateHold));
            }
            if (gateHold)
                return;
            routeState.approach = 0;
            group.SkipTarget(target->GetGUID(), CAREFUL_REFUSED_SKIP_MS, "body pull refused: " + gate);
            return;
        }

        // [b33_aoepull] the planner's issued aoe_pull_plan is the pull: the tank takes the chain to its stack spot,
        // the leader is not walked in while the plan is fresh and younger than AOE_PULL_WALK_MS
        if (uint32 aoeSinceMs = 0; PartyAoePlanned(group, target->GetGUID(), &aoeSinceMs) &&  // [lead5]
                                   getMSTimeDiff(aoeSinceMs, getMSTime()) < AOE_PULL_WALK_MS)
        {
            TestRunner::ClearWaypoint(group.LeaderGuid());
            uint32 const nowMs = getMSTime();
            if (!routeState.blockerGateLogMs ||
                getMSTimeDiff(routeState.blockerGateLogMs, nowMs) > 10 * IN_MILLISECONDS)
            {
                routeState.blockerGateLogMs = nowMs ? nowMs : 1;
                group.LogEvent("aoe_pull_plan_accepted",
                         JsonObject().Str("blocker", target->GetName()).Num("blockerEntry", target->GetEntry())
                             .Num("planSec", getMSTimeDiff(aoeSinceMs, nowMs) / IN_MILLISECONDS).Str("was", why));
            }
            return;
        }

        std::list<Creature*> pack;
        PackMobCheck check{leader, target};
        Acore::CreatureListSearcher<PackMobCheck> searcher(target, pack, check);
        Cell::VisitObjects(target, searcher, PACK_RADIUS);
        if (std::find(pack.begin(), pack.end(), target) == pack.end())
            pack.push_back(target);
        // [body_pull_via_planner] the planner first
        if (BODY_PULL_VIA_PLANNER && !target->IsDungeonBoss() && !routeState.bossCreatures.count(target->GetGUID()))
        {
            bool askBegan = false;
            if (AskPlannerFirst(group.LeaderGuid(), target, getMSTime(), askBegan))
            {
                if (askBegan)
                    group.LogEvent("body_pull_planner_request",
                             JsonObject().Str("name", target->GetName()).Num("entry", target->GetEntry())
                                 .Num("packMobs", uint32(pack.size())).Str("why", why)
                                 .Num("waitSec", BODY_PULL_PLANNER_MS / IN_MILLISECONDS));
                TestRunner::ClearWaypoint(group.LeaderGuid());
                return;
            }
            group.LogEvent("body_pull_planner_timeout",
                     JsonObject().Str("name", target->GetName()).Num("entry", target->GetEntry())
                         .Num("packMobs", uint32(pack.size())));
        }
        // [b49_voone] No walk-in to a pack an idle boss's own aggro reach covers (BossBroughtBy): the walk is his pull,
        // and the planner did not price it (refused, or no answer in time). brs-h 20261026-111724-brs-h-5A-10
        // (build 47): the planner refused the Smolderthorn Mystic pack at (-4,-462) ("refused for now: no pull of
        // Smolderthorn Mystic from range"), the body pull walked in at 1342-1346 s ("unplanned (harness body pull)"),
        // War Master Voone 13 yd from it came at 1353 s with his room (Berserkers, Shadow Priests, Headhunters), 3
        // deaths 1437-1444 s. The pack is skipped; the boss logic pulls him as his fight. Not when the planner's own
        // verdict was the boss's pull ("(a boss pull: no fallback plan)", [b35_bfd]): that walk is meant to stop at
        // him.
        bool const bossPullVerdict = why.find("(a boss pull: no fallback plan)") != std::string::npos;
        Map* const bossMap = bossPullVerdict ? nullptr : leader->GetMap();
        if (Map* map = bossMap)
            for (ObjectGuid const& guid : routeState.bossCreatures)
            {
                Creature* boss = map->GetCreature(guid);
                if (!boss || !boss->IsAlive() || boss->IsInCombat())
                    continue;
                for (Creature* mob : pack)
                {
                    if (!BossBroughtBy(leader, boss, mob))
                        continue;
                    LOG_INFO("playerbots", "[b49_voone] {}: no body pull of {} ({} mobs): {} ({:.1f} yd from {}) "
                             "comes with it, the planner did not price it ({})", leader->GetName(),
                             target->GetName(), pack.size(), boss->GetName(), boss->GetExactDist(mob),
                             mob->GetName(), why);
                    group.LogEvent("blocker_pull_gate",
                             JsonObject().Str("pull", "body pull").Str("blocker", target->GetName())
                                 .Num("blockerEntry", target->GetEntry())
                                 .Str("why", "[b49_voone] brings boss " + boss->GetName() + " (unpriced)")
                                 .Str("was", why).Bool("hold", false));
                    // [b51_dme] User rule 2026-10-26 (open the way, no detours): a pack that is the way on and stands
                    // in an idle boss's reach is no skip - the boss is pulled deliberately first, as his own fight (the
                    // boss logic: his buffs, ready check and the planner's boss pull, which prices him with what he
                    // brings). dme-h 20261026-130307-5E-11 stalled 0/16: "body pull refused: brings boss Hydrospawn"
                    // x3, "behind skipped Wildspawn Hellcaller" x8. The Voone intent stays: no blind walk-in to the
                    // pack; a class-gated boss keeps the old skip.
                    if (!ClassGatedBoss(group.MapId(), group.Difficulty(), boss->GetEntry()) &&
                        boss->GetGUID() != routeState.target)
                    {
                        group.LogEvent("boss_on_the_way_first",
                                 JsonObject().Str("first", boss->GetName()).Num("firstEntry", boss->GetEntry())
                                     .Real("firstDistance", leader->GetExactDist(boss)).Str("via", target->GetName())
                                     .Num("viaEntry", target->GetEntry())
                                     .Str("why", "[b51_dme] the pack on the way brings this boss: he is pulled first"));
                        LOG_INFO("playerbots", "[b51_dme] {}: {} brings boss {}: the boss is the next pull (his "
                                 "fight), no skip",
                                 leader->GetName(), target->GetName(), boss->GetName());
                        routeState.target = boss->GetGUID();
                        routeState.targetIsBoss = true;
                        routeState.targetIsStarter = false;
                        routeState.targetRouteRule = "boss on the way";
                        routeState.targetSinceMs = getMSTime();
                        routeState.approach = 0;
                        routeState.arrivedMs = 0;
                        routeState.stopSinceMs = 0;
                        routeState.approachBlocker.Clear();
                        routeState.orderHoldSinceMs.erase(boss->GetGUID());
                        TestRunner::ClearWaypoint(group.LeaderGuid());
                        return;
                    }
                    TestRunner::ClearWaypoint(group.LeaderGuid());
                    routeState.approach = 0;
                    group.SkipTarget(target->GetGUID(), CAREFUL_REFUSED_SKIP_MS,
                               "body pull refused: brings boss " + boss->GetName());
                    return;
                }
            }
        sightStops.erase(group.LeaderGuid());
        sightHolds.erase(group.LeaderGuid());
        routeState.approach = 1;
        routeState.arrivedMs = 0;
        routeState.guaranteeHolding = false;
        LOG_INFO("playerbots", "[route_blocker_body_pull] {}: walks into {} ({} mobs, {:.1f} yd): {}",
                 leader->GetName(), target->GetName(), pack.size(), leader->GetExactDist(target), why);
        group.LogEvent("route_blocker_body_pull",
                 JsonObject().Str("action", "body pull").Str("blocker", target->GetName())
                     .Num("blockerEntry", target->GetEntry()).Num("packMobs", uint32(pack.size())).Str("why", why)
                     .Real("blockerDistance", leader->GetExactDist(target)));
    }

    void LogHarnessHold(LeaderGroup& group, Player* leader, Creature* blocker, char const* walk, float x,
                        float y, float z)
    {
        if (!leader || !blocker)
            return;
        uint32 const now = getMSTime();
        uint32& loggedMs = harnessHoldLogMs[blocker->GetGUID().GetRawValue()];
        if (loggedMs && getMSTimeDiff(loggedMs, now) < HARNESS_HOLD_LOG_MS)
            return;
        loggedMs = now ? now : 1;
        bool accepted = true;
        uint32 mobs = 0;
        bool const known = PartyVerdict(group, blocker->GetGUID(), accepted, mobs);
        char const* verdict = !known ? "none" : accepted ? "accepted" : "refused";
        // [b30_mech_pos] packMobs was the planner's count only: 0 with no verdict (zf-h 20261025-184229: "Sandfury
        // Blood Drinker, none, 0 mobs" with ~20 Sandfury round the pool). The idle pack round the blocker is counted
        // too (realPack, PackMobCheck within PACK_RADIUS, the blocker included).
        std::list<Creature*> pack;
        PackMobCheck packCheck{leader, blocker};
        Acore::CreatureListSearcher<PackMobCheck> packSearcher(blocker, pack, packCheck);
        Cell::VisitObjects(blocker, packSearcher, PACK_RADIUS);
        uint32 const realPack =
            uint32(pack.size()) + (std::find(pack.begin(), pack.end(), blocker) == pack.end() ? 1u : 0u);
        LOG_INFO("playerbots",
                 "[approach] {}: harness_hold_refused_pack: {} stops at {} ({}, {} mobs, {} idle round it) {:.1f} yd",
                 leader->GetName(), walk, blocker->GetName(), verdict, mobs, realPack, leader->GetExactDist(blocker));
        group.LogEvent("harness_hold_refused_pack",
                 JsonObject().Str("walk", walk).Str("blocker", blocker->GetName()).Num("entry", blocker->GetEntry())
                     .Str("verdict", verdict).Num("packMobs", mobs).Num("realPack", realPack)
                     .Real("distance", leader->GetExactDist(blocker)).Real("x", x).Real("y", y).Real("z", z));
    }

    // [lead5] the tank's view of `unit` (AggressiveTargetValue::Explain) with the leader's own route target
    std::string PartyTankView(LeaderGroup& group, Player* leader, Unit* unit)
    {
        GroupRouteState& routeState = group.Route();
        if (!LeaderPicksFor(group))
            return AggressiveTargetValue::Explain(leader, unit);
        return AggressiveTargetValue::Explain(leader, unit,
                                              routeState.targetIsStarter ? ObjectGuid::Empty : routeState.target);
    }

    PlanVerdict TankPlanVerdict(LeaderGroup& group, Player* leader, std::vector<Creature*> const& pack)
    {
        GroupRouteState& routeState = group.Route();
        // The planner's own published verdict (CarefulPlanVerdict, fresh for a few seconds) on any mob of the
        // pack comes first; a refusal of one of them is a refusal of the pack.
        bool anyAccepted = false;
        for (Creature* mob : pack)
        {
            bool accepted = false;
            uint32 mobs = 0;
            if (!PartyVerdict(group, mob->GetGUID(), accepted, mobs))  // [lead5]
                continue;
            if (!accepted)
                return PlanVerdict::Refused;
            anyAccepted = true;
        }
        if (anyAccepted)
            return PlanVerdict::Accepted;

        for (Creature* mob : pack)
        {
            auto const refused = routeState.carefulRefused.find(mob->GetGUID());
            if (refused != routeState.carefulRefused.end() && refused->second > 0)
                return PlanVerdict::Refused;
        }
        PrePullCc::Plan plan;
        if (!PrePullCc::Get(leader, plan) || plan.tank != leader->GetGUID() || plan.pulledMs)
            return PlanVerdict::Unknown;
        for (Creature* mob : pack)
            if (plan.pullTarget == mob->GetGUID() ||
                std::find(plan.pack.begin(), plan.pack.end(), mob->GetGUID()) != plan.pack.end())
                return plan.heavy ? PlanVerdict::Refused : PlanVerdict::Accepted;
        return PlanVerdict::Unknown;
    }

    // The idle mobs of the target's pack and of the blocker's (PackMobCheck, PACK_RADIUS around each), bosses
    // left out (their body pull is the boss pull): what approach to sight keeps out of while no plan is accepted.
    std::vector<Creature*> SightKeepOutPack(LeaderGroup& group, Player* leader, Creature* target, Creature* blocker)
    {
        GroupRouteState& routeState = group.Route();
        std::vector<Creature*> result;
        for (Creature* center : {target, blocker})
        {
            std::list<Creature*> pack;
            PackMobCheck check{leader, center};
            Acore::CreatureListSearcher<PackMobCheck> searcher(center, pack, check);
            Cell::VisitObjects(center, searcher, PACK_RADIUS);
            if (center->IsAlive() && !center->IsInCombat() &&
                std::find(pack.begin(), pack.end(), center) == pack.end())
                pack.push_back(center);
            for (Creature* mob : pack)
                if (!routeState.bossCreatures.count(mob->GetGUID()) && !mob->IsDungeonBoss() &&
                    std::find(result.begin(), result.end(), mob) == result.end())
                    result.push_back(mob);
        }
        return result;
    }

    // [lead5] the leader planner's aoe_pull_plan (its own memory, Decision::aoePlan) and the leader's required
    // marks it hands the planner (LeaderInputs::required), else the AoePull registry
    bool PartyAoePlanned(LeaderGroup& group, ObjectGuid mob, uint32* sinceMs)
    {
        if (!LeaderPicksFor(group))
            return AoePull::Planned(group.LeaderGuid(), mob, sinceMs);
        auto const& lead = group.State();
        if (!lead.aoePlanMs || getMSTimeDiff(lead.aoePlanMs, getMSTime()) > LEADER_VERDICT_TTL_MS ||
            std::find(lead.aoePlan.begin(), lead.aoePlan.end(), mob) == lead.aoePlan.end())
            return false;
        if (sinceMs)
            *sinceMs = lead.aoePlanSinceMs;
        return true;
    }
}
