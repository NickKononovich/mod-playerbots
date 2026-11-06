/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "RouteSteer.h"
#include "RouteMeet.h"

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
    using Leader::Dungeon::DOOR_WALK_MS;
    using Leader::Dungeon::TankPullState;

    bool PullAfterHoldPasses(ObjectGuid leader, ObjectGuid blocker)
        {
            auto const found = pullAfterHoldGates.find(leader);
            return found != pullAfterHoldGates.end() && found->second.blocker == blocker &&
                   int32(found->second.untilMs - getMSTime()) > 0;
        }

    std::unordered_map<ObjectGuid, PlannerAsk> aoePullAsks;  // by leader

    bool AoePullAskRuns(LeaderGroup& group, ObjectGuid blocker)
        {
            // [lead134] the planner weighed the aoe pull when it answered: its aoe_pull_plan for the blocker holds the
            // leader, none is the dead end (AOE_PULL_ASK_MS 15 s / AOE_PULL_ASK_RESET_MS 5 min were the old clocks)
            return PartyAoePlanned(group, blocker, nullptr);
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
        std::vector<DeadEndVisit>& visits = found->second;
        visits.erase(std::remove_if(visits.begin(), visits.end(),
                                    [&](DeadEndVisit const& visit)
                                    {
                                        // [lead132] forgotten when passed, not after DEAD_END_MEMORY_MS
                                        return (visit.goal - goal).length() <= DEAD_END_SAME_GOAL &&
                                               distance + DEAD_END_PROGRESS < visit.distance;
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
    bool DeadEndFallback(Player* leader, ObjectGuid target, bool refusal, bool& started)
    {
        // [lead132] user: no elapsed-time rule decides (DEAD_END_FALLBACK_WINDOW_MS 3 min / DEAD_END_FALLBACK_MS
        // 10 min were the old clocks): DEAD_END_FALLBACK_REFUSALS refusals for the same target put the leader on the
        // old walk for that target; another target starts the count anew
        started = false;
        uint32 const now = getMSTime();
        DeadEndRefusals& refusals = deadEndRefusals[leader->GetGUID()];
        if (refusals.target != target)
        {
            refusals = DeadEndRefusals();
            refusals.target = target;
        }
        if (refusals.fallbackUntilMs)
            return true;
        if (!refusal)
            return false;
        refusals.ms.push_back(now);
        if (refusals.ms.size() < DEAD_END_FALLBACK_REFUSALS)
            return false;
        refusals.ms.clear();
        refusals.fallbackUntilMs = now ? now : 1;  // on (since)
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

    std::map<InstanceGuid, uint32> tankRejectLogMs;  // mob -> last "approach ignores" line, world thread only

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

    std::map<InstanceGuid, uint32> unseenRouteEndUntilMs;  // creature guid -> until (world thread)

    std::map<InstanceGuid, uint32> unseenRouteEndLogMs;

    void MarkUnseenRouteEnd(Creature* mob, uint32 untilMs)
    {
        if (ROUTE_UNSEEN_ROUTE_END_PASS && mob)
            unseenRouteEndUntilMs[InstanceKey(mob)] = untilMs ? untilMs : 1;
    }

    bool UnseenRouteEnd(Player* leader, Creature* mob)
    {
        if (!ROUTE_UNSEEN_ROUTE_END_PASS)
            return false;
        auto const found = unseenRouteEndUntilMs.find(InstanceKey(mob));
        if (found == unseenRouteEndUntilMs.end())
            return false;
        uint32 const now = getMSTime();
        if (int32(now - found->second) >= 0)
        {
            unseenRouteEndUntilMs.erase(found);
            return false;
        }
        uint32& loggedMs = unseenRouteEndLogMs[InstanceKey(mob)];
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
            !mob->IsAlive() || boss->IsInCombat() || boss->IsImmuneToPC() || !boss->IsHostileTo(leader) ||
            CreatureLeavesWhenDoorOpens(boss))  // [lead1565]
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
                    uint32& loggedMs = tankRejectLogMs[InstanceKey(mob)];
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

    std::map<InstanceGuid, uint32> harnessHoldLogMs;  // blocker -> last harness_hold_refused_pack, world thread

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
            "rests below its line", " buffing", " summons its pet", "the group rests or gathers", "group not ready",
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
        if (ask.target != target->GetGUID() || !ask.sinceMs)
        {
            ask.target = target->GetGUID();
            ask.sinceMs = now ? now : 1;
            first = true;
        }
        // [lead131] user: the planner's verdict, not BODY_PULL_PLANNER_MS 8 s: the walk-in waits while the planner
        // has no verdict on the pack or takes it; its refusal hands the pack to the walk-in
        bool accepted = false;
        uint32 mobs = 0;
        if (CarefulPlanVerdict::Get(leaderGuid, target->GetGUID(), accepted, mobs) && !accepted)
            return false;
        GuaranteeRequest::Set(leaderGuid, target->GetGUID(), false);
        return true;
    }

    std::map<InstanceGuid, BehindRootTry> behindRootTries;

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
        // [lead129] while the route's target is of the blocker pack (NEIGHBOUR_TARGET_MS 4 min was the old clock)
        auto const inPack = [&routeState](ObjectGuid mob)
        {
            return std::find(routeState.routeBlockerPack.begin(), routeState.routeBlockerPack.end(), mob) !=
                   routeState.routeBlockerPack.end();
        };
        return ROUTE_BLOCKER_BODY_PULL && !guid.IsEmpty() && inPack(guid) && inPack(routeState.target);
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
    // [lead1562] The starter is a closed door (a key door, a lever's gate) and the idle mob stands on its far side:
    // the line from the mob to the walk's stop runs through the door's own model (RouteThroughClosedDoor, the
    // route's door test). The walk ends on this side; what the opening wakes on the far side is the opening's own
    // pull (the key pass prices it: door_far_side_priced), no blocker of the walk. b277 strat-n
    // 20261106-105644-5D-12 642-887 s: the Skeletal Guardian 3 yd past the closed King's Square Gate 175353 became
    // the route target ("[b47_plan] stands on the walk to the event starter"), the leader held at the gate with
    // the key unused to the stall (105514-5A-9 1545-1790 s the same).
    bool BehindStartersDoor(WorldObject const* starter, Creature const* mob, G3D::Vector3 const& stop)
    {
        GameObject* door = starter ? const_cast<GameObject*>(starter->ToGameObject()) : nullptr;
        if (!door || !mob || door->GetGoType() != GAMEOBJECT_TYPE_DOOR || door->GetGoState() != GO_STATE_READY ||
            !door->m_model)
            return false;
        Movement::PointsArray const line = {G3D::Vector3(mob->GetPositionX(), mob->GetPositionY(),
                                                         mob->GetPositionZ()),
                                            stop};
        return RouteThroughClosedDoor(line, {door}, mob->GetPhaseMask()) != nullptr;
    }

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
            AoePullAskRuns(group, blocker->GetGUID()))
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
                group.ClearWaypoint(group.LeaderGuid());
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
            group.ClearWaypoint(group.LeaderGuid());
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
        // the leader is not walked in while the plan stands (lead134: not AOE_PULL_WALK_MS 30 s)
        if (uint32 aoeSinceMs = 0; PartyAoePlanned(group, target->GetGUID(), &aoeSinceMs))  // [lead5]
        {
            group.ClearWaypoint(group.LeaderGuid());
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
                                 .Str("until", "the planner's verdict"));
                group.ClearWaypoint(group.LeaderGuid());
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
                        group.ClearWaypoint(group.LeaderGuid());
                        return;
                    }
                    group.ClearWaypoint(group.LeaderGuid());
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
        uint32& loggedMs = harnessHoldLogMs[InstanceKey(blocker)];
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

    void Steer(LeaderGroup& group, Player* leader)
    {
        GroupRouteState& routeState = group.Route();
        // The tank's trail for the guarded follows of its members: the harness walks (MovePoint, its waypoint) are no
        // MoveTo of the tank's own (MovementAction::RecordTankTrail samples it every 2 yd).
        MovementAction::RecordTankTrail(leader);

        // [lead27] rfc-h 20261031-122130: the engage order was issued at 69.8 s and the route's approach stop kept
        // stepping the tank back ("stop_out_of_real_reach", its waypoint) - the tank never took the order, the run
        // stood. An issued leader order (or its committed pull) wins: no route walk of the tank meanwhile
        // [lead33] ... and for any committed pull (its gate, window and order are the leader's), the route target
        // being the commitment's mob (one target)
        if (group.LeaderDecides() && (Leader::Orders::IsOpen(group.LeaderGuid()) || group.State().commit))
        {
            // [lead880] b224 rfc-h 5G 928.4 s: the commitment's Searing Blade Cultist ("brings Bazzalan", pulled
            // as one mob at range) took the route target while targetIsBoss stayed Bazzalan's - the buff pass ran
            // "before Searing Blade Cultist" and its end ordered the prepull Army for the cultist as "the boss"
            // (Rtdekad, 30.8 s before Bazzalan, on cooldown for him). The flag follows the mob the target now is
            if (group.State().commit && !group.State().commitMob.IsEmpty() &&
                routeState.target != group.State().commitMob)
            {
                Creature* committed = leader->GetMap() ? leader->GetMap()->GetCreature(group.State().commitMob)
                                                       : nullptr;
                bool const isBoss = committed && (committed->IsDungeonBoss() || committed->isWorldBoss() ||
                                                  routeState.bossCreatures.count(committed->GetGUID()));
                if (routeState.targetIsBoss && !isBoss)
                    LOG_INFO("playerbots", "[lead880] {}: the committed pull's {} is the route target, no boss",
                             leader->GetName(), committed ? committed->GetName() : std::string("?"));
                routeState.target = group.State().commitMob;
                routeState.targetIsBoss = isBoss;
            }
            group.ClearWaypoint(group.LeaderGuid());
            return;
        }

        // [ledge_drop_link] plan29 follow-up 5: while the group crosses a route link (the leader over it, the members
        // following), its step down is the route - the ledge guard (JudgeLedgeDrop) lets the drops pass.
        // [link_stale_end] a link chosen and never walked (WalkToLink runs only with no target: dmw-n 20261025-
        // 163739-4 kept "walking to it, 1527 s, 0 crossed" for Immol'thar long after his kill from the ledge) ends
        // after LINK_STALE_MS.
        // [lead135] the ledge-drop permit is renewed on every look while the crossing runs (its 5 s is the window
        // between two looks, no decision); a link is stale when what it was chosen for is dead, gone or no longer
        // the route's target (LINK_STALE_MS 10 min was the old clock)
        constexpr uint32 LINK_DROP_PERMIT_MS = 5 * IN_MILLISECONDS;
        if (routeState.linkIdx >= 0)
        {
            if (routeState.linkLeaderCrossed)
                for (LeaderGroup::MemberInfo const& member : group.Members())
                    AllowLedgeDrop(member.guid, LINK_DROP_PERMIT_MS);
            else if (!routeState.linkFor.IsEmpty())
            {
                WorldObject* linkGoal = ObjectAccessor::GetWorldObject(*leader, routeState.linkFor);
                Unit const* linkUnit = linkGoal ? linkGoal->ToUnit() : nullptr;
                if (!linkGoal || (linkUnit && !linkUnit->IsAlive()) ||
                    (!routeState.target.IsEmpty() && routeState.target != routeState.linkFor))
                    EndLink(group, "stale (its target gone or changed)");
            }
        }

        // [pull_estimate_vs_actual] each planned pull, once over: the planner's estimate against what engaged
        {
            PullEstimate::Result result;
            uint32& seen = pullEstimateSeen[group.LeaderGuid()];
            if (PullEstimate::Take(group.LeaderGuid(), seen, result))
            {
                seen = result.id;
                Creature* pulled = leader->GetMap() ? leader->GetMap()->GetCreature(result.target) : nullptr;
                group.LogEvent("pull_estimate_vs_actual",
                         JsonObject().Num("pull", group.PullSeq())
                             .Str("target", pulled ? pulled->GetName() : std::string())
                             .Num("targetEntry", result.target.GetEntry()).Str("kind", result.kind)
                             .Str("via", result.via).Num("estimated", result.estimated).Real("cost", result.cost)
                             .Num("engaged", result.engaged)
                             .Num("diff", int64(result.engaged) - int64(result.estimated))
                             .Num("deaths", result.deaths).Num("durationSec", result.durationSec)
                             .Bool("fought", result.fought).Str("plan", result.reason));
            }
        }

        // [pull_after_hold] (4) the walk to the door that hides the blocker: on until it is open
        if (auto walk = doorWalks.find(group.LeaderGuid()); walk != doorWalks.end())
        {
            Map* doorMap = leader->GetMap();
            GameObject* door = doorMap ? doorMap->GetGameObject(walk->second.door) : nullptr;
            Creature* behind = doorMap ? doorMap->GetCreature(walk->second.blocker) : nullptr;
            uint32 const nowMs = getMSTime();
            if (!door || door->GetGoState() != GO_STATE_READY || !behind || !behind->IsAlive() ||
                behind->IsInCombat() || leader->IsInCombat() || int32(walk->second.untilMs - nowMs) <= 0)
                doorWalks.erase(walk);
            else
            {
                // [lead11] a door that closes again by itself: the whole group at the opener first, no cap
                if (leader->GetExactDist(door) <= door->GetInteractionDistance() &&
                    DoorGatherHolds(group, leader, door))
                {
                    group.ClearWaypoint(group.LeaderGuid());
                    return;
                }
                if (OpenDoorByUse(leader, door))
                {
                    group.LogEvent("pull_after_hold",
                             JsonObject().Str("action", "opened the door").Str("door", door->GetName())
                                 .Num("doorEntry", door->GetEntry()).Str("blocker", behind->GetName())
                                 .Num("blockerEntry", behind->GetEntry())
                                 .Real("blockerDistance", leader->GetExactDist(behind)));
                    doorWalks.erase(walk);
                }
                else
                    group.SetLeaderWaypoint(leader, walk->second.point.x, walk->second.point.y,
                                      walk->second.point.z);
                return;
            }
        }

        // [hleft-hor-reentry-hold] Halls of Reflection: after a Falric / Marwyn wave wipe the waves resume once every
        // player INSIDE stands alive within 40 yd of the hall centre (instance_halls_of_reflection.cpp
        // MAX_DIST_FROM_CENTER_TO_START), and nobody else gets in while they run (TRANSFER_ABORT_ZONE_IN_COMBAT).
        // hor-h 2026-10-25 (20261025-065720-hor-h-5A-73): the first member back walked to the centre, the waves
        // restarted for one or two, they died, the others bounced off areatrigger 5636 for ten minutes. The leader
        // holds at the entrance until every member is inside and alive.
        if (leader->GetMapId() == HOR_MAP && leader->IsAlive() && !leader->IsInCombat())
        {
            InstanceScript const* horScript = leader->GetInstanceScript();
            bool waveWiped = false;
            for (BossFacts const& boss : group.Bosses())
                waveWiped |= (boss.entry == HOR_FALRIC_ENTRY || boss.entry == HOR_MARWYN_ENTRY) && boss.wipes > 0;
            if (horScript && waveWiped && horScript->GetBossState(HOR_MARWYN_SLOT) != DONE)
            {
                std::string away;
                for (LeaderGroup::MemberInfo const& member : group.Members())
                {
                    Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    if (!bot || !bot->IsAlive() || bot->GetMap() != leader->GetMap())
                        away += (away.empty() ? "" : ", ") + member.name;
                }
                if (!away.empty())
                {
                    float const fromCentre = leader->GetExactDist2d(HOR_CENTRE_X, HOR_CENTRE_Y);
                    uint32 const nowMs = getMSTime();
                    if (!routeState.horHoldLogMs || getMSTimeDiff(routeState.horHoldLogMs, nowMs) > HOR_HOLD_LOG_MS)
                    {
                        routeState.horHoldLogMs = nowMs ? nowMs : 1;
                        group.LogEvent("hor_entrance_hold", JsonObject().Str("waitingFor", away)
                                                                 .Real("fromCentre", fromCentre));
                    }
                    routeState.target.Clear();
                    group.NoteProgress(nowMs);
                    if (fromCentre < HOR_HOLD_FROM_CENTRE)
                        group.SetLeaderWaypoint(leader, HOR_ENTRANCE_X, HOR_ENTRANCE_Y, HOR_ENTRANCE_Z);
                    else
                        group.ClearWaypoint(group.LeaderGuid());
                    return;
                }
            }
        }

        // [hleft-swim-back] Blackfathom Deeps heroic 2026-10-25 (20261025-062726-bfd-h-5A-19): after Old Serra'kis the
        // group surfaced in the moat round Twilight Lord Kelris's platform (-787,-196,-31.4); the moat is a navmesh
        // part of its own (tools/navmesh_probe.py 48 -787.3 -195.6 -31.4 -797.2 -223.8 -25.6: NOT CONNECTED, the
        // ledge 6.7 yd above the water), the route "ended beside a gap" under the platform and the leader stood
        // "own path makes no progress, no straight walk over ground" for 8 minutes. A swimmer leaves the water the
        // way it came in: the trail of its swim back to the last dry ground, straight swims point to point.
        if (ROUTE_SWIM_BACK && leader->IsAlive())
        {
            uint32 const nowMs = getMSTime();
            bool const inWater = leader->IsInWater();
            if (!inWater && !leader->IsFalling() && !leader->IsFlying())
            {
                routeState.swimDry = leader->GetPosition();
                routeState.swimDryKnown = true;
                routeState.swimTrail.clear();
                if (routeState.swimBack)
                    group.LogEvent("swim_back", JsonObject().Str("action", "on dry ground")
                                                     .Real("x", leader->GetPositionX())
                                                     .Real("y", leader->GetPositionY())
                                                     .Real("z", leader->GetPositionZ()));
                routeState.swimBack = false;
            }
            else if (inWater)
            {
                if (routeState.swimTrail.empty() && routeState.swimDryKnown)
                    routeState.swimTrail.push_back(routeState.swimDry);
                if (routeState.swimTrail.empty() ||
                    leader->GetExactDist(&routeState.swimTrail.back()) >= SWIM_TRAIL_STEP)
                {
                    if (routeState.swimTrail.size() >= SWIM_TRAIL_MAX)
                        routeState.swimTrail.erase(routeState.swimTrail.begin() + 1);  // the dry spot stays first
                    routeState.swimTrail.push_back(leader->GetPosition());
                }
            }
            // [lead135] given up when the trail back is used up (SWIM_BACK_MAX_MS 2 min was the old clock)
            if (routeState.swimBack && routeState.swimTrail.size() < 2)
            {
                group.LogEvent("swim_back",
                         JsonObject().Str("action", "given up").Num("trail", uint32(routeState.swimTrail.size())));
                routeState.swimBack = false;
            }
            if (routeState.swimBack && inWater && !leader->IsInCombat())
            {
                // The point nearest the dry spot the swimmer sees from here (a shortcut over the trail).
                for (Position const& point : routeState.swimTrail)
                {
                    if (leader->GetExactDist(&point) <= WAYPOINT_REACHED_SWIM)
                        continue;
                    if (!leader->IsWithinLOS(point.GetPositionX(), point.GetPositionY(), point.GetPositionZ() + 1.0f))
                        continue;
                    group.SetLeaderWaypoint(leader, point.GetPositionX(), point.GetPositionY(),
                                      point.GetPositionZ(), true);
                    routeState.steerSinceMs = nowMs;
                    group.NoteProgress(nowMs);
                    return;
                }
            }
        }

        // A regroup after a revive goes on past the far-member wait (its 60 s wait_timeout): the group stays on
        // "stay" until the returning side is with it, the route waits; MeetMember ends it when everybody is together.
        if (group.Regrouping())
        {
            std::vector<Player*> players;
            for (LeaderGroup::MemberInfo const& member : group.Members())
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                    if (bot->IsInWorld())
                        players.push_back(bot);
            if (MeetMember(group, players, leader))
                return;
        }

        // A wing closed to this run (AreaGateDef::onlyContent): the tank's own pull choice stays out of it
        // (KeepTankOutOfClosedAreas) and the walk never leads through it (below).
        std::vector<AreaGateDef const*> steerClosedAreas;
        for (AreaGateDef const* gate : AreaGates(group.MapId()))
            if (AreaGateForContent(*gate, group.ContentKey().c_str()) &&
                (gate->afterDead.empty() || !EntriesDead(group, gate->afterDead)))
                steerClosedAreas.push_back(gate);
        steerClosedAreas = KeepOutAreas(steerClosedAreas);
        KeepTankOutOfClosedAreas(leader, steerClosedAreas);

        // Inside such a wing out of combat (a fight ran in, a chase): out through its exit before the route goes on.
        // The routes from in there pass its packs (LBRS run 13:04: 6 minutes in the UBRS hall, as deep as x 225).
        for (AreaGateDef const* gate : steerClosedAreas)
        {
            if (gate->exitZ <= -100000.0f || !leader->IsAlive() || leader->IsInCombat() ||
                !InAreaGate(*gate, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()))
                continue;
            G3D::Vector3 const goal(gate->exitX, gate->exitY, gate->exitZ);
            PathResult leg;
            SteerLeg(leader, goal, leg);
            if (leg.type & PATHFIND_NOPATH)
                break;
            G3D::Vector3 const waypoint =
                FloorWaypoint(leader, leg, RouteWaypoint(leg, goal, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP));
            if (routeState.lastEvent.rfind("closed area exit", 0) != 0)
            {
                routeState.lastEvent = std::string("closed area exit: ") + gate->note;
                group.LogEvent("closed_area_exit", JsonObject().Str("note", gate->note)
                                                        .Real("x", leader->GetPositionX())
                                                        .Real("y", leader->GetPositionY())
                                                        .Real("z", leader->GetPositionZ()));
            }
            group.SetLeaderWaypoint(leader, waypoint.x, waypoint.y, waypoint.z);
            if (leader->isMoving())
                group.NoteProgress(getMSTime());
            // [b31_ubrs_door] The way out holds at a door that stays shut for good now (ShutForGoodDoorSlot: UBRS'
            // wave portcullis 175185 after Rend): the group is locked in. ubrs-n 20261025-184229-10B-43 stood 45 min
            // in the drake pen until "no boss progress"; the verdict says what happened, LOCKED_IN_FAIL_MS after.
            uint32 const lockNow = getMSTime();
            GameObject* lockDoor =
                routeState.doorHoldMs && getMSTimeDiff(routeState.doorHoldMs, lockNow) < IN_MILLISECONDS
                    ? leader->GetMap()->GetGameObject(routeState.doorHold)
                    : nullptr;
            InstanceScript const* lockScript = leader->GetInstanceScript();
            if (lockDoor && lockScript && ShutForGoodDoorSlot(lockScript, lockDoor) >= 0)
            {
                uint32& since = lockedInSince[group.Run().runSeq];
                if (!since)
                {
                    since = lockNow ? lockNow : 1;
                    LOG_INFO("playerbots", "[b31_ubrs_door] {}: the leader is locked in ({}): the way out runs through "
                                           "{} {}, shut for good",
                             group.Run().runId, gate->note, lockDoor->GetName(), lockDoor->GetEntry());
                    group.LogEvent("locked_in", JsonObject().Str("note", gate->note).Str("door", lockDoor->GetName())
                                                     .Num("doorEntry", lockDoor->GetEntry())
                                                     .Real("x", leader->GetPositionX())
                                                     .Real("y", leader->GetPositionY())
                                                     .Real("z", leader->GetPositionZ()));
                }
                // [lead135] locked in is the fact: the run ends with it (no LOCKED_IN_FAIL_MS wait)
                if (since)
                {
                    lockedInSince.erase(group.Run().runSeq);
                    group.FinishRun("failed",
                                    std::string("group locked in: ") + gate->note + " - the way out runs through " +
                                        lockDoor->GetName() + " " + std::to_string(lockDoor->GetEntry()) +
                                        ", which nothing opens any more");
                }
            }
            else
                lockedInSince.erase(group.Run().runSeq);
            return;
        }

        // [b46_patrol] PATROL_STEP_BACK: out of a patrol's way first; the rest hold then holds at the step-back spot.
        // [lead4] LEADER_ONE_PATROL_OWNER: one patrol decision (LeaderPatrolTick: wait or pull the patrol), the step
        // back only as its wait's spot; the old independent step back and patrol hold below are off
        if (LeaderOwnsPatrols(group) ? LeaderPatrolTick(group, leader) : PatrolStepBack(group, leader))
            return;

        // ROUTE_REST_HOLD: no walk while a member sits eating / drinking or is about to; the leader stands.
        {
            std::string const resting = RestHoldMembers(group, leader);
            RestHold& hold = restHolds[group.InstanceId()];
            if (!resting.empty())
            {
                if (!hold.logged)
                {
                    hold.logged = true;
                    routeState.lastEvent = "rest hold: " + resting;
                    std::string const target =
                        routeState.target.IsEmpty() ? std::string() : routeState.target.ToString();
                    group.LogEvent("rest_hold", JsonObject().Str("state", "waiting").Str("members", resting)
                                                     .Str("target", target)
                                                     .Real("x", leader->GetPositionX())
                                                     .Real("y", leader->GetPositionY())
                                                     .Real("z", leader->GetPositionZ()));
                }
                // [b48_scholo] the walk under way stops too (HaltLeaderWalk)
                if (G3D::Vector3 haltedTo; HaltLeaderWalk(group, leader, haltedTo))
                    group.LogEvent("harness_walk_halt", JsonObject().Str("hold", "rest").Str("why", resting)
                                                             .Real("x", leader->GetPositionX())
                                                             .Real("y", leader->GetPositionY())
                                                             .Real("z", leader->GetPositionZ())
                                                             .Real("toX", haltedTo.x).Real("toY", haltedTo.y)
                                                             .Real("toZ", haltedTo.z));
                group.ClearWaypoint(group.LeaderGuid());
                return;
            }
            if (hold.logged)
            {
                hold.logged = false;
                // Retarget skipped its 4 s turns during the hold (RestHoldMembers): the next target at once.
                routeState.nextRetargetMs = getMSTime();
                group.LogEvent("rest_hold", JsonObject().Str("state", hold.timedOut ? "timeout" : "done")
                                                 .Str("members", hold.members));
            }
            hold.timedOut = false;
        }

        // [b43_harn] ROUTE_PATROL_HOLD (PatrolHoldWhy): no walk while a patrol comes to the group or the tank's planner
        // pulls one; the leader stands, the planner meets it where the group is. A running patrol wait spot walk
        // ([patrol_wait_spot]) is the planner's own patrol answer and keeps its waypoint.
        if (!LeaderOwnsPatrols(group) &&  // [lead4] the leader's patrol decision ran above (LeaderPatrolTick)
            !PatrolWaitOn(group))
        {
            std::string const patrolWhy = PatrolHoldWhy(group, leader);
            PatrolHold& hold = patrolHolds[group.InstanceId()];
            if (!patrolWhy.empty())
            {
                uint32 const holdNow = getMSTime();
                if (!hold.logged)
                {
                    hold.logged = true;
                    routeState.lastEvent = "patrol hold: " + patrolWhy;
                    LOG_INFO("playerbots", "[b43_harn] patrol_hold {}: {}", leader->GetName(), patrolWhy);
                    group.LogEvent("patrol_hold", JsonObject().Str("state", "waiting").Str("why", patrolWhy)
                                                       .Real("x", leader->GetPositionX())
                                                       .Real("y", leader->GetPositionY())
                                                       .Real("z", leader->GetPositionZ()));
                }
                // The tank keeps planning the route target meanwhile; the hold is no route stall.
                if (!routeState.target.IsEmpty() && !routeState.targetIsStarter && !LeaderPicksFor(group))  // [lead5]
                    RouteTarget::Set(group.LeaderGuid(), routeState.target);
                routeState.steerSinceMs = holdNow;
                routeState.steerAnchor = leader->GetPosition();
                routeState.steerNoNearerMs = 0;
                routeState.driveSinceMs = holdNow;
                routeState.driveAnchor = leader->GetPosition();
                // [b48_scholo] the walk under way stops too (scholo-h 111724-5C-4: 12 yd on into the patrol's walk)
                if (G3D::Vector3 haltedTo; HaltLeaderWalk(group, leader, haltedTo))
                {
                    LOG_INFO("playerbots", "[b48_scholo] halt_walk {}: patrol hold stops the walk to ({:.1f},{:.1f},"
                             "{:.1f}) at ({:.1f},{:.1f},{:.1f}): {}", leader->GetName(), haltedTo.x, haltedTo.y,
                             haltedTo.z, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                             patrolWhy);
                    group.LogEvent("harness_walk_halt", JsonObject().Str("hold", "patrol").Str("why", patrolWhy)
                                                             .Real("x", leader->GetPositionX())
                                                             .Real("y", leader->GetPositionY())
                                                             .Real("z", leader->GetPositionZ())
                                                             .Real("toX", haltedTo.x).Real("toY", haltedTo.y)
                                                             .Real("toZ", haltedTo.z));
                }
                group.ClearWaypoint(group.LeaderGuid());
                return;
            }
            if (hold.logged)
            {
                hold.logged = false;
                group.LogEvent("patrol_hold", JsonObject().Str("state", hold.timedOut ? "timeout" : "done")
                                                   .Str("why", hold.why));
            }
            hold.timedOut = false;
        }

        // [b33_sh2] sh-n 20261026-003732-sh-n-5B-89, 1370-1512 s: the target Shattered Hand Champion set, the phantom
        // combat already declared and ended (the harness out of combat, phase traveling), yet the tank Rlwaraj stayed
        // flagged in combat with 0 attackers; this gate cleared its waypoint and the route target every tick without a
        // word ("pull action: no target", "test runner move -> 0.0 yd" for 140 s, then the stall skipped the target).
        // Steer runs only while the harness counts the party out of combat, so a leader flagged here with nobody on
        // it is that phantom flag: the walk goes on (the route target too, the tank's pull gets its target). A leader
        // somebody attacks still holds. Either way a held walk is logged (harness_hold).
        // [lead1332] ... and a fight of only mobs nobody can hit and no member has a sight spot for (UnhittableOnly)
        bool const unhittableOnly = Leader::Fight::UnhittableOnly(leader->GetMap(), group.State(), group.MemberGuids());
        bool const leaderFlagOnly =
            !routeState.target.IsEmpty() && leader->IsInCombat() && (!group.FightOn(leader) || unhittableOnly);
        if (!routeState.target.IsEmpty() && leader->IsInCombat())
        {
            uint32 const holdNow = getMSTime();
            std::string const why = unhittableOnly && leaderFlagOnly
                                        ? "[lead1332] only mobs nobody can hit, no sight spot: the walk goes on"
                                    : leaderFlagOnly
                                        ? "leader flagged in combat, nobody on it (phantom): the walk goes on"
                                        : "leader in combat with attackers: no walk, no pull";
            if (why != routeState.harnessHoldWhy || !routeState.harnessHoldLogMs ||
                getMSTimeDiff(routeState.harnessHoldLogMs, holdNow) > 15 * IN_MILLISECONDS)
            {
                routeState.harnessHoldWhy = why;
                routeState.harnessHoldLogMs = holdNow ? holdNow : 1;
                std::string refs;
                for (auto const& [guid, ref] : leader->GetCombatManager().GetPvECombatRefs())
                    if (Unit* other = ref ? ref->GetOther(leader) : nullptr)
                        refs += (refs.empty() ? "" : ", ") + std::string(other->GetName()) +
                                Acore::StringFormat(" {:.1f} yd", leader->GetExactDist(other));
                Unit const* holdTarget = leader->GetMap() ? leader->GetMap()->GetCreature(routeState.target) : nullptr;
                LOG_INFO("playerbots", "[b33_sh2] harness_hold {}: {} (attackers {}, refs: {})", leader->GetName(), why,
                         leader->getAttackers().size(), refs.empty() ? "none" : refs);
                group.LogEvent("harness_hold",
                         JsonObject().Str("why", why).Str("target", holdTarget ? holdTarget->GetName() : "")
                             .Real("distance", holdTarget ? leader->GetExactDist(holdTarget) : -1.0f)
                             .Num("attackers", int64(leader->getAttackers().size())).Str("combatRefs", refs)
                             .Bool("walks", leaderFlagOnly));
            }
        }

        if (routeState.target.IsEmpty() || (leader->IsInCombat() && !leaderFlagOnly))
        {
            RouteTarget::Clear(group.LeaderGuid());  // no route target to plan (or a fight on)
            // [ulda-ritual-hold] no event-spot / link / escort walk while the ritual channel runs
            if (auto const hold = ritualHoldUntilMs.find(group.InstanceId()); hold != ritualHoldUntilMs.end())
            {
                if (leader->GetCurrentSpell(CURRENT_CHANNELED_SPELL) && !leader->IsInCombat())
                {
                    group.ClearWaypoint(group.LeaderGuid());
                    return;
                }
                ritualHoldUntilMs.erase(hold);  // the channel ended: the route goes on
                routeState.nextRetargetMs = getMSTime();
            }
            bool const idle = routeState.noTargets && !leader->IsInCombat() && !group.InCombat();
            // A scripted intro keeps its boss immune (ROUTE_INTRO_WAIT, RunningIntroBoss: Svala's Arthas talk, 72 s
            // from areatrigger 5140): the group waits where it stands - within her 100 yd target pick - instead of
            // pulling elsewhere, buffing and drinking meanwhile; neither the "no targets" end nor a wing change
            // comes from that wait. At most INTRO_WAIT_MAX_MS.
            if (idle && ROUTE_INTRO_WAIT)
            {
                Creature* intro = RunningIntroBoss(leader);
                uint32 const now = getMSTime();
                auto const since = introWaitSinceMs.find(group.InstanceId());
                if (!intro)
                    introWaitSinceMs.erase(group.InstanceId());
                else if (since == introWaitSinceMs.end() || getMSTimeDiff(since->second, now) < INTRO_WAIT_MAX_MS)
                {
                    // A member started it (the UP strategy's "svala start event" walks in from 70 yd) while the
                    // leader was still on the way: the walk to the spot goes on first.
                    if (leader->GetExactDist(intro) > INTRO_WAIT_RANGE && WalkToEventSpot(group, leader))
                        return;
                    if (since == introWaitSinceMs.end())
                    {
                        introWaitSinceMs[group.InstanceId()] = now;
                        routeState.lastEvent = std::string("intro wait: ") + intro->GetName();
                        group.LogEvent("intro_wait", JsonObject().Str("state", "waiting").Str("boss", intro->GetName())
                                                          .Num("entry", intro->GetEntry())
                                                          .Real("distance", leader->GetExactDist(intro))
                                                          .Real("x", leader->GetPositionX())
                                                          .Real("y", leader->GetPositionY())
                                                          .Real("z", leader->GetPositionZ()));
                    }
                    group.ClearWaypoint(group.LeaderGuid());
                    routeState.noTargetsSinceMs = now;
                    return;
                }
            }
            if (idle && (StayWithEscort(group, leader) || WalkToEventGather(group, leader) ||
                         WalkToEventSpot(group, leader) || WalkToLink(group, leader)))
                return;
            // A starter step-out the leader walks on its own (UseStarterItem) keeps its waypoint.
            if (!routeState.waypointHoldUntilMs || int32(getMSTime() - routeState.waypointHoldUntilMs) >= 0)
                group.ClearWaypoint(group.LeaderGuid());
            // An escort still walking (or waiting for its next wave) is no reason to end the run or change wings,
            // nor the Oculus drake flow between the rings (the OC strategy mounts and flies the group).
            if (idle && (!routeState.escort.IsEmpty() || OculusDrakePhase(leader) || group.ScriptEventRunning()))
            {
                routeState.noTargetsSinceMs = getMSTime();
                return;
            }
            // [sm-wing-done] an empty wing waits WING_DONE_NO_TARGET_MS, not 30 s (one-run contents only)
            int32 doneWingNext = -1;
            bool const wingDone = ROUTE_WING_DONE_CHANGE && routeState.noTargets &&
                                  group.ContentBossEntries().empty() && routeState.entrances.size() > 1 &&
                                  routeState.entranceIdx < routeState.entrances.size() &&
                                  !WingBossesLeft(group, leader->GetMap(), leader, doneWingNext);
            // [lead135] nothing to fight out of a fight is the fact (WING_DONE_NO_TARGET_MS 3 s / 30 s were the old
            // clocks); an event that sends more (escort, script event, drakes) returned above
            (void)wingDone;
            if (routeState.noTargets && !leader->IsInCombat() && !group.InCombat())
            {
                // Entrances on the same spot (Halls of Reflection: from Icecrown and from Pit of Saron) are one.
                size_t next = routeState.entranceIdx + 1;
                while (next < routeState.entrances.size() && routeState.entranceIdx < routeState.entrances.size() &&
                       routeState.entrances[next].GetExactDist(&routeState.entrances[routeState.entranceIdx]) <
                           SAME_ENTRANCE_RANGE)
                    ++next;

                // Another wing of the same instance (Scarlet Monastery, Dire Maul, Maraudon): the group walks
                // out and in through the next portal. Bots walk by themselves: every member gets the portal goal
                // (the leader's "we go in over there") and its own "instance portal" walks out through the exit and
                // in through that entrance (TickWingWalk watches). Not while an event the group started waits for
                // its next wave.
                // A next boss behind a closed door that does not open for him: another wing does not open it
                // (Gnomeregan heroic 2026-10-25: Thermaplugg behind The Final Chamber 142207; the walk to the Train
                // Depot entrance, the same caves, paced between two exits 4 minutes without a fight).
                bool doorBlocked = false;
                for (auto const& [guid, reason] : routeState.skipReason)
                    doorBlocked |=
                        routeState.skipUntil.count(guid) && reason.rfind("next boss behind a closed door", 0) == 0;
                // Only when the next entrance's arrival is walkable from here anyway (the same caves): a wing reached
                // only from outside (Stratholme's service entrance) is the way round the door.
                bool const sameCaves =
                    doorBlocked && next < routeState.entrances.size() &&
                    RoutePath(leader, routeState.entrances[next].GetPositionX(),
                              routeState.entrances[next].GetPositionY(), routeState.entrances[next].GetPositionZ())
                        .complete;
                // [queue-door-skip-lift] not right after a starter was used (a key on a gate: strat-n
                // 20261025-115805-strat-n-5A-6, Gauntlet Gate keyed at 962 s, wing change at 999 s): what it opened
                // is the way on, the route is built again first.
                // [lead135] the fact: no retarget since the starter's use (WING_CHANGE_AFTER_STARTER_MS 60 s)
                bool const starterJustUsed = routeState.starterUsedSinceRetarget;
                if (starterJustUsed && !sameCaves && next < routeState.entrances.size())
                {
                    // waits: the "no targets" clock goes on, the change comes once the window is over
                }
                else if (!sameCaves && next < routeState.entrances.size() && !EventGatherApplies(group) &&
                         EventSpotIndex(group) < 0)
                {
                    StartWingChange(group, next, "no targets");
                }
                else
                {
                    // Event dungeons (ToC5, Violet Hold, Halls of Reflection, Culling) spawn their next wave or
                    // boss after a pause: with bosses still open the group waits before the run is called partial.
                    bool bossesOpen = false;
                    for (BossFacts const& boss : group.Bosses())
                        bossesOpen |= !boss.killed && !boss.failed;
                    // A boss behind a closed door changes only the reason, never the timing: the order hold
                    // (ORDER_HOLD_MS) keeps the later bosses out of the route while the next one is unreachable, so
                    // "no targets" for 30 s says nothing about them (wave 7 build 7: strat-n / strat-h / brs-h ended
                    // "partial: next boss behind a closed door" with most bosses alive and reachable).
                    // [hleft-mech-bridge] The Mechanar: Pathaleon comes only after the bridge pack (MechanarBridge,
                    // Mech strategy): never "no reachable targets" while he is dormant.
                    bool const pathaleonDormant =
                        leader->GetMapId() == MECHANAR_MAP_ID && MechanarBridge::PathaleonDormant(leader);
                    // [hor-hollow-waves] Halls of Reflection after a wave wipe: spirits removed from the map are not
                    // respawned by HandleWaveWipe, the waves come with 0-2 spirits and end only on their 150 s timer
                    // (hor-h 20261025-153805: waves 6-9 from 876 s, Marwyn due ~1476 s, run ended 1456 s).
                    // (PATCH_hor_gd.md 1.4 (a), applied by plan29)
                    constexpr uint32 HOR_WAVE_WAIT_MS = 11 * MINUTE * IN_MILLISECONDS;  // 4 x 150 s + call + margin
                    InstanceScript* horInstance =
                        leader->GetMapId() == HoRIds::HOR_MAP_ID ? HoRHelpers::GetInstance(leader) : nullptr;
                    bool const horWaves = horInstance && HoRHelpers::IsWaveEventActive(leader) &&
                                          horInstance->GetData(HoRIds::DATA_WAVE_NUMBER) != 0;
                    uint32 const spawnWaitMs = horWaves ? HOR_WAVE_WAIT_MS : EVENT_WAIT_FOR_SPAWNS_MS;
                    if (!pathaleonDormant && (!bossesOpen ||
                        getMSTimeDiff(routeState.noTargetsSinceMs, getMSTime()) > spawnWaitMs))
                        group.FinishRun("partial",
                                        doorBlocked ? "no reachable targets left: next boss behind a closed door"
                                                    : "no reachable targets left");
                }
            }
            return;
        }

        Map* map = leader->GetMap();
        WorldObject* object = nullptr;
        if (routeState.targetIsStarter)
        {
            object = map->GetCreature(routeState.target);
            if (!object)
                object = map->GetGameObject(routeState.target);
        }
        else
        {
            Creature* creature = map->GetCreature(routeState.target);
            if (creature && creature->IsAlive())
                object = creature;
        }

        if (!object)
        {
            RouteTarget::Clear(group.LeaderGuid());
            routeState.target.Clear();
            routeState.nextRetargetMs = getMSTime();
            group.ClearWaypoint(group.LeaderGuid());
            return;
        }

        // The tank plans the harness's route target out to 70 yd (AggressiveTargetValue RouteTarget, TTL 10 s,
        // refreshed every steer tick), not only its own 40 yd pick; a starter is no pull.
        // [lead5] LEADER_ONE_PICKER: no registry - the leader picks with its own route target (TickLeaderPlanner)
        if (!routeState.targetIsStarter && object->ToCreature() && !LeaderPicksFor(group))
            RouteTarget::Set(group.LeaderGuid(), routeState.target);
        else
            RouteTarget::Clear(group.LeaderGuid());

        uint32 const now = getMSTime();
        float const distance = leader->GetExactDist(object);
        float range = routeState.targetIsStarter ? STARTER_RANGE : routeState.approach ? BODY_PULL_RANGE : PULL_RANGE;
        // [b32_bm] a walk-in arrives beside a big model too: BODY_PULL_RANGE (exact distance) is inside the two
        // combat reaches of Aeonus (3) and a bear (1.5) - the tank stood at 6.0 yd, never "arrived", steer_stuck.
        if (!routeState.targetIsStarter && routeState.approach)
            if (Unit const* walkedTo = object->ToUnit())
                range = std::max(range, leader->GetMeleeRange(walkedTo) + BODY_PULL_MELEE_SLACK);

        // A game object starter is used from within reach, as a client clicks it: no line of sight to its centre (UBRS
        // wave 8: the leader stood on the Blackrock Altar 175706 for 8 minutes, the ray to the object's own origin
        // runs into its model, the altar was never used and the run stalled "traveling").
        bool const starterObject = routeState.targetIsStarter && object->ToGameObject();
        // A creature starter counts as the core's gossip check does (object sizes included): CoS-n wave 11 stood 4.9
        // yd from Chromie, in her talk range, never "arrived" at 4.0 and never used her.
        bool const inRange = distance <= range || (routeState.targetIsStarter && object->ToCreature() &&
                                                   leader->IsWithinDistInMap(object, STARTER_RANGE));
        bool arrived = inRange && (starterObject || leader->IsWithinLOSInMap(object));
        if (!routeState.targetIsStarter && !routeState.approach)
        {
            // Same floor as the target (a stairwell under its room is "in range" but sees nothing): keep walking the
            // path until the height matches; a body pull stands next to it anyway.
            if (arrived && std::fabs(leader->GetPositionZ() - object->GetPositionZ()) > SAME_FLOOR_DZ)
                arrived = false;
            // Never into the aggro radius of the nearest mob of the pack: the pull logic decides from its edge.
            if (!arrived)
                if (Creature* mob = object->ToCreature())
                    arrived = AtPackAggroEdge(leader, mob);
        }

        // [patrol_wait_spot] standing at the patrol wait spot: the tank's own planner pulls once the patrol is alone or
        // past; the walk back to the target waits until then (PATROL_WAIT_HOLD_MS at most).
        if (PatrolWaitOn(group) && !leader->IsInCombat())
            return;
        // The route of this tick, and where it enters the aggro reach of an idle mob around the target (ApproachStop):
        // the waypoint stops there, and standing there is the arrival.
        G3D::Vector3 goal(object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
        // [st-guard-approach] A guard of a closed guarded door with an approach point (Sunken Temple's defenders on the
        // z -67 balconies): the walk goes to the point on the guard's floor first - from the pit / the z -91 ring every
        // stop was "no sight point" or "tank does not see it" (st-h 062726: 16 skips, Jammal'an never reached).
        if (!routeState.targetIsStarter && !routeState.approach)
            if (Creature* guard = object->ToCreature())
                if (GuardApproachDef const* point = PendingGuardApproach(map, guard))
                {
                    float const toPoint = leader->GetExactDist(point->x, point->y, point->z);
                    bool const onGuardFloor =
                        std::fabs(leader->GetPositionZ() - guard->GetPositionZ()) <= SAME_FLOOR_DZ &&
                        distance <= guard->GetExactDist(point->x, point->y, point->z) + GUARD_APPROACH_SLACK;
                    std::pair<uint32, ObjectGuid> const approachKey{group.Run().runSeq, guard->GetGUID()};
                    if (toPoint > GUARD_APPROACH_ARRIVE && !onGuardFloor)
                    {
                        goal = G3D::Vector3(point->x, point->y, point->z);
                        arrived = false;
                        if (guardApproachLogged.insert(approachKey).second)
                        {
                            guardApproachSinceMs[approachKey] = now ? now : 1;
                            group.LogEvent("guard_approach",
                                     JsonObject().Str("name", guard->GetName()).Num("entry", guard->GetEntry())
                                         .Str("state", "walking").Real("toPoint", toPoint).Real("distance", distance)
                                         .Real("x", point->x).Real("y", point->y).Real("z", point->z));
                        }
                    }
                    // [guard_approach_arrived] The walk's end, once: st-n build 23 logged only "walking" (toPoint
                    // 80-140 yd) and the log could not tell a reached point from an aborted walk.
                    else if (auto const since = guardApproachSinceMs.find(approachKey);
                             since != guardApproachSinceMs.end())
                    {
                        group.LogEvent("guard_approach",
                                 JsonObject().Str("name", guard->GetName()).Num("entry", guard->GetEntry())
                                     .Str("state", "arrived").Str("by", onGuardFloor ? "guard floor" : "point")
                                     .Real("toPoint", toPoint).Real("distance", distance)
                                     .Num("sec", getMSTimeDiff(since->second, now) / IN_MILLISECONDS)
                                     .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                     .Real("z", leader->GetPositionZ()));
                        guardApproachSinceMs.erase(since);
                    }
                }
        // [selin_room_owns_steer] Magisters' Terrace: while Selin's room tactic holds the group (her Wretched idle, no
        // room fight) the harness walks the leader to none of her room's mobs: the room tactic picks the pack, the
        // cast spot out of the other packs' reach and the moment. Build 27 (20261025-150737-mgt-h-5A-14, 264-267 s):
        // the harness's approach to sight walked the tank to (212.0,-6.6), 20.3 yd from a south-pack Bruiser (real
        // aggro 21); the pack came by the pair (the core's call for assistance) and Selin 2 s later, the door shut on
        // the hunter and the shaman.
        if (leader->GetMapId() == MagistersTerrace::MAP_MAGISTERS_TERRACE)
            if (Creature const* roomMob = object->ToCreature())
            {
                uint32 const entry = roomMob->GetEntry();
                bool const selinRoomMob =
                    entry == 24688 || entry == 24689 || entry == 24690 || entry == 24723;  // Wretched x3, Selin
                PlayerbotAI* leaderAI = selinRoomMob ? GET_PLAYERBOT_AI(leader) : nullptr;
                struct RoomOwns
                {
                    uint32 sinceMs = 0;
                    uint32 seenMs = 0;
                };
                static std::unordered_map<uint32, RoomOwns> roomOwns;  // party seq (world thread)
                // ... and the entry walk of a clear room (EntryHolds): the members' "mgt out of room" walks them to
                // (221, +-9); the harness's approach stop (216.1,-4.0) held the leader outside (build 29, stall).
                bool owns = leaderAI && ((MagistersTerrace::SelinRoom::Get(leaderAI).active &&
                                          !MagistersTerrace::SelinRoom::InRoomFight(leaderAI)) ||
                                         (entry == 24723 && MagistersTerrace::SelinRoom::EntryHolds(leader)));
                if (owns)
                {
                    // [lead135] the room tactic owns its pull while it is active (SELIN_ROOM_OWNS_MAX_MS 240 s was the
                    // old cap)
                    RoomOwns& held = roomOwns[group.Run().runSeq];
                    if (!held.sinceMs)
                        held.sinceMs = now ? now : 1;
                    held.seenMs = now ? now : 1;
                    // [b35_mgt] ... and only while the group fought less than SELIN_ROOM_OWNS_IDLE_MS ago: a room
                    // tactic that holds its pull that long (20261026-024524-mgt-h-5E-15, 337-498 s: "holds the pull
                    // at the rally (Rtrogab: not at the rally)", the rogue parked by its walk guard 10 yd short) gives
                    // the pull back to the harness. The fight's start resets the idle clock (CheckIdle).
                    // [lead135] the fact that it no longer leads the pull: the room tactic owns no pull (OwnsPull),
                    // not SELIN_ROOM_OWNS_IDLE_MS 90 s idle
                    if (owns && MagistersTerrace::SelinRoom::Get(leaderAI).active &&
                        !MagistersTerrace::SelinRoom::OwnsPull(leaderAI))
                    {
                        owns = false;
                        static std::unordered_map<uint32, uint32> releaseLogMs;  // party seq (world thread)
                        uint32& logged = releaseLogMs[group.Run().runSeq];
                        if (!logged || getMSTimeDiff(logged, now) >= 30 * IN_MILLISECONDS)
                        {
                            logged = now ? now : 1;
                            group.LogEvent("boss_area_clear",
                                     JsonObject().Str("boss", "Selin Fireheart").Str("mob", roomMob->GetName())
                                         .Num("entry", entry)
                                         .Str("action", "the room tactic owns no pull: the leader pulls"));
                        }
                    }
                }
                if (owns)
                {
                    static std::unordered_map<uint32, uint32> roomOwnsLogMs;  // party seq (world thread)
                    uint32& logged = roomOwnsLogMs[group.Run().runSeq];
                    if (!logged || getMSTimeDiff(logged, now) >= 10 * IN_MILLISECONDS)
                    {
                        logged = now ? now : 1;
                        group.LogEvent("boss_area_clear",
                                 JsonObject().Str("boss", "Selin Fireheart").Str("mob", roomMob->GetName())
                                     .Num("entry", entry).Str("action", "room tactic owns the pull")
                                     .Num("idle", int64(MagistersTerrace::SelinRoom::Get(leaderAI).idle.size())));
                    }
                    routeState.steerSinceMs = now;
                    group.ClearWaypoint(group.LeaderGuid());
                    return;
                }
            }
        PathResult leg;
        bool legBuilt = false;
        bool approachStopped = false;
        G3D::Vector3 approachStop;
        // [combat_no_damage_release] the group fights and nobody has hit anything for a while: no approach hold
        if (!arrived && !routeState.targetIsStarter && !routeState.approach && !GroupDamage::NoDamageRelease(leader))
            if (Creature* mob = object->ToCreature())
            {
                SteerLeg(leader, goal, leg);
                legBuilt = true;
                Creature* blocker = nullptr;
                bool unseenStop = false;
                float const walk = leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP;
                if (!(leg.type & PATHFIND_NOPATH) &&
                    ApproachStop(leader, mob, leg.points, walk, approachStop, blocker, unseenStop, nullptr, &group) &&
                    blocker)
                {
                    G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
                    approachStopped = true;
                    G3D::Vector3 const edgeStop = approachStop;
                    // Approach to sight: a stop that hid the blocker from the tank was replaced by a point of the
                    // route that shows it (below); that point stays the stop while target and blocker are the same.
                    auto sightStop = sightStops.find(group.LeaderGuid());
                    if (sightStop != sightStops.end() && sightStop->second.target != routeState.target)
                    {
                        sightStops.erase(sightStop);
                        sightStop = sightStops.end();
                    }
                    auto sightHold = sightHolds.find(group.LeaderGuid());
                    if (sightHold != sightHolds.end() && (sightHold->second.target != routeState.target ||
                                                          sightHold->second.blocker != blocker->GetGUID()))
                    {
                        sightHolds.erase(sightHold);
                        sightHold = sightHolds.end();
                    }
                    bool sightTried = sightStop != sightStops.end() && sightStop->second.blocker == blocker->GetGUID();
                    // The held line (ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED): "[approach] <leader>: holds at the edge:
                    // planner refused <target> (<n> mobs)", or "no accepted plan for" while the planner said nothing
                    // the harness can read. Once per SIGHT_HOLD_LOG_MS per target and blocker.
                    auto const logHold = [&](PlanVerdict verdict, std::vector<Creature*> const& pack, char const* how)
                    {
                        SightHold& hold = sightHolds[group.LeaderGuid()];
                        // [queue-blocker-pull-gate] the give-way clock counts an unknown verdict only: a refusal
                        // restarts it (scholo-n w24: "refused, 97 mobs" gave way after 8 s)
                        if (hold.blocker != blocker->GetGUID() || verdict == PlanVerdict::Refused)
                            hold.unknownSinceMs = 0;
                        if (!hold.unknownSinceMs)
                            hold.unknownSinceMs = now;
                        hold.target = routeState.target;
                        hold.blocker = blocker->GetGUID();
                        hold.checkedMs = now;
                        hold.blockerAt = blocker->GetPosition();
                        hold.leaderAt = leader->GetPosition();
                        hold.verdict = uint8(verdict);
                        hold.pack.clear();
                        for (Creature* packMob : pack)
                            hold.pack.push_back(packMob->GetGUID());
                        if (hold.loggedMs && getMSTimeDiff(hold.loggedMs, now) < SIGHT_HOLD_LOG_MS)
                            return;
                        hold.loggedMs = now;
                        bool const refused = verdict == PlanVerdict::Refused;
                        LOG_INFO("playerbots", "[approach] {}: holds at the edge: {} {} ({} mobs){}", leader->GetName(),
                                 refused ? "planner refused" : "no accepted plan for", mob->GetName(), pack.size(),
                                 how);
                        group.LogEvent("approach_sight",
                                 JsonObject().Str("action", "holds at the edge").Str("name", object->GetName())
                                     .Num("entry", object->GetEntry()).Str("blocker", blocker->GetName())
                                     .Num("blockerEntry", blocker->GetEntry())
                                     .Str("verdict", refused ? "refused" : "not accepted")
                                     .Num("packMobs", uint32(pack.size()))
                                     .Real("blockerDistance", leader->GetExactDist(blocker))
                                     .Real("blockerRealAggro", RealAggroRange(blocker, leader))
                                     .Real("x", here.x).Real("y", here.y).Real("z", here.z)
                                     .Real("edgeX", edgeStop.x).Real("edgeY", edgeStop.y).Real("edgeZ", edgeStop.z));
                    };
                    // A sight point that lay in a pack's reach was taken while the planner had accepted its plan:
                    // no longer accepted (refused since, or the plan expired), the leader goes back to the edge (RFC
                    // 20:08: refused while walking to the sight point, the pack came on the walk).
                    if (sightTried && sightStop->second.intoReach && ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED)
                    {
                        std::vector<Creature*> const pack = SightKeepOutPack(group, leader, mob, blocker);
                        PlanVerdict const verdict = TankPlanVerdict(group, leader, pack);
                        if (verdict != PlanVerdict::Accepted)
                        {
                            sightStops.erase(sightStop);
                            sightStop = sightStops.end();
                            sightTried = false;
                            logHold(verdict, pack, ", back from the sight point");
                        }
                    }
                    if (sightTried)
                        approachStop = sightStop->second.point;
                    bool const atStop = (approachStop - here).length() <= APPROACH_ARRIVED;
                    // RFC 18:22-18:24 (user: "why does it stand a minute?"): the stop lay before a corner on a rising
                    // slope, the Earthborer round it out of the tank's sight; the group stood PULL_WAIT_MS, tank_view
                    // found no line of sight and the target was skipped - three times. Checked when the leader
                    // stands at the stop, not after the wait: no line of sight from the tank (its possible-targets
                    // test) - it walks on along the route to the first point that sees the blocker (the corner) and
                    // the pull comes from there (walking into its sight is the pull); no such point within
                    // APPROACH_SIGHT_PATH - the target (its own pack) is skipped at once, a neighbour that stopped
                    // the walk is marked as not seen (TankRejects) and holds no walk. Bosses keep the wait and the
                    // body pull.
                    // [hleft-boss-own-pack] a boss's encounter mob (his formation, his script's call for help) is his
                    // own pack: the boss pull brings it, as the planner says.
                    // [b33_dme] ... and the mob his wander / aggro reach brings (BossBroughtBy)
                    bool const ownPackHere = blocker == mob || blocker->GetExactDist(mob) <= PACK_RADIUS ||
                                             (routeState.targetIsBoss && BossEncounterMob(leader, mob, blocker)) ||
                                             (routeState.targetIsBoss && BossBroughtBy(leader, mob, blocker));
                    bool const bossInvolved = routeState.targetIsBoss ||
                                              routeState.bossCreatures.count(blocker->GetGUID()) ||
                                              blocker->IsDungeonBoss() || group.BossOrder(blocker) >= 0;
                    // ROUTE_SKIP_BEHIND_SKIPPED: a neighbour pack the route skipped already (the tank refused it or
                    // did not see it) stops the walk to every target behind it. Drak'Tharon heroic / Mana-Tombs
                    // 2026-10-25: the refused Scourge Brute / Ethereal Spellbinder stopped each next target, the
                    // targets flipped every ~20 s (the 45 s tank view never came) until the 4-minute stall. The target
                    // is skipped at once with the blocker named: the idle guard shows the refusal, not a walk.
                    // An event objective (a hold spot's holdAlways entry: Black Morass' rift keepers) is never
                    // skipped behind a skipped pack: its own walk and body pull decide.
                    std::vector<uint32> keeperEntries;
                    if (int32 const spotIdx = EventSpotIndex(group); spotIdx >= 0)
                        if (EventSpotDef const* spot = EventSpots(group.MapId())[size_t(spotIdx)]; spot->hold)
                            keeperEntries = spot->holdAlways;
                    bool const keeperTarget = std::find(keeperEntries.begin(), keeperEntries.end(), mob->GetEntry()) !=
                                              keeperEntries.end();
                    // [dmw-guard-behind-exempt] a door's pending guard keeps its own walk and pull, as a keeper
                    bool const guardTarget = IsPendingDoorGuard(map, mob);
                    // [lead108] b141 rfc-h 119.2 s on: the route target the leader refused (its routeSkips) skipped
                    // Trogg 56, Earthborer 4 ... "behind" it - the leader's refusal of one mob is no refusal of the
                    // walkable mobs behind it (the route walks on to the next, lead96)
                    bool const leaderSkipOnly = group.State().routeSkips.count(blocker->GetGUID()) &&
                                                Leader::Fight::TankHasWalk(leader, mob);
                    if (ROUTE_SKIP_BEHIND_SKIPPED && !ownPackHere && !bossInvolved && !keeperTarget && !guardTarget &&
                        !leaderSkipOnly && routeState.skipUntil.count(blocker->GetGUID()))
                    {
                        auto const why = routeState.skipReason.find(blocker->GetGUID());
                        std::string const blockerWhy = why != routeState.skipReason.end() ? why->second : "skipped";
                        // [route_blocker_body_pull] the skipped pack is on the way to this target: it is pulled, not
                        // the target skipped behind it - unless its skip is final (unreachable, a door, a body pull
                        // that brought no fight)
                        bool const finalSkip = blockerWhy == "not pulled" ||
                                               blockerWhy.find("unreachable") != std::string::npos ||
                                               blockerWhy.find("door") != std::string::npos ||
                                               blockerWhy.rfind("route end", 0) == 0 || blockerWhy == "path lost";
                        if (!finalSkip && TargetRouteBlocker(group, leader, blocker, object, "skipped " + blockerWhy))
                            return;
                        // [behind_root_pull] plan29 follow-up 2 (scholo-n 20261025-163739-8: "behind skipped
                        // Necrofiend (behind skipped Adept (behind skipped Adept (behind skipped Necrolyte (careful
                        // pull refused (pack)))))", PULL_ESTIMATE_AUDIT: 473 packs skipped only for standing behind a
                        // refused one): the skipped pack on the way is the target again once the group is ready - its
                        // skip and its pack's are cleared, the planner plans it afresh (the adaptive cap, the 12 s
                        // guarantee, a split or los pull) and pull_after_hold ends its hold. A blocker skipped behind
                        // another stops the walk to it the same way, so the chain is walked back to its root. At most
                        // BEHIND_ROOT_TRIES per blocker in BEHIND_ROOT_WINDOW_MS; then the skip stands as before.
                        if (!finalSkip && BEHIND_ROOT_PULL && group.GroupFullyReady(leader) && !group.NeedBuff())
                        {
                            BehindRootTry& tried = behindRootTries[InstanceKey(blocker)];
                            if (!tried.firstMs || getMSTimeDiff(tried.firstMs, now) > BEHIND_ROOT_WINDOW_MS)
                            {
                                tried.firstMs = now ? now : 1;
                                tried.count = 0;
                            }
                            if (tried.count < BEHIND_ROOT_TRIES)
                            {
                                ++tried.count;
                                std::list<Creature*> rootPack;
                                PackMobCheck rootCheck{leader, blocker};
                                Acore::CreatureListSearcher<PackMobCheck> rootSearcher(blocker, rootPack, rootCheck);
                                Cell::VisitObjects(blocker, rootSearcher, PACK_RADIUS);
                                routeState.skipUntil.erase(blocker->GetGUID());
                                routeState.skipReason.erase(blocker->GetGUID());
                                for (Creature* packMob : rootPack)
                                {
                                    routeState.skipUntil.erase(packMob->GetGUID());
                                    routeState.skipReason.erase(packMob->GetGUID());
                                }
                                group.LogEvent("behind_root_pull",
                                         JsonObject().Str("name", mob->GetName()).Num("entry", mob->GetEntry())
                                             .Str("blocker", blocker->GetName())
                                             .Num("blockerEntry", blocker->GetEntry())
                                             .Str("blockerSkip", blockerWhy).Num("try", tried.count)
                                             .Num("packMobs", uint32(rootPack.size()))
                                             .Real("blockerDistance", leader->GetExactDist(blocker)));
                                sightStops.erase(group.LeaderGuid());
                                sightHolds.erase(group.LeaderGuid());
                                routeState.neighbourTarget = blocker->GetGUID();
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
                                return;
                            }
                        }
                        LOG_INFO("playerbots", "[approach] {}: {} lies behind skipped {} ({:.1f} yd, {}): skip",
                                 leader->GetName(), mob->GetName(), blocker->GetName(), leader->GetExactDist(blocker),
                                 blockerWhy);
                        routeState.behindSkipped = blocker->GetName() + " (" + blockerWhy + ")";
                        std::string const behindWhy = "behind skipped " + blocker->GetName() + " (" + blockerWhy + ")";
                        // The whole shadow at once (scholo-n wave 11: 78 targets behind one hopeless Necrolyte pack,
                        // one per tick, 78 s without a fight): every idle non-boss mob farther than the blocker in a
                        // BEHIND_CONE_COS cone from the leader through it, within BEHIND_SHADOW_RANGE of it.
                        G3D::Vector3 const toBlocker = G3D::Vector3(blocker->GetPositionX(), blocker->GetPositionY(),
                                                                    blocker->GetPositionZ()) - here;
                        float const blockerDist = toBlocker.length();
                        if (blockerDist > 0.0f)
                        {
                            std::list<Creature*> shadow;
                            ShadowMobCheck shadowCheck{leader};
                            Acore::CreatureListSearcher<ShadowMobCheck> shadowSearcher(blocker, shadow, shadowCheck);
                            Cell::VisitObjects(blocker, shadowSearcher, BEHIND_SHADOW_RANGE);
                            for (Creature* other : shadow)
                            {
                                if (other->GetGUID() == routeState.target ||
                                    routeState.skipUntil.count(other->GetGUID()) ||
                                    routeState.bossCreatures.count(other->GetGUID()) || other->IsDungeonBoss() ||
                                    group.BossOrder(other) >= 0 || IsPendingDoorGuard(map, other) ||
                                    std::find(keeperEntries.begin(), keeperEntries.end(), other->GetEntry()) !=
                                        keeperEntries.end())
                                    continue;
                                G3D::Vector3 const toOther = G3D::Vector3(other->GetPositionX(), other->GetPositionY(),
                                                                          other->GetPositionZ()) - here;
                                float const otherDist = toOther.length();
                                if (otherDist <= blockerDist ||
                                    toOther.dot(toBlocker) < BEHIND_CONE_COS * otherDist * blockerDist)
                                    continue;
                                routeState.skipUntil[other->GetGUID()] = now + CAREFUL_REFUSED_SKIP_MS;
                                routeState.skipReason[other->GetGUID()] = behindWhy;
                            }
                        }
                        group.SkipTarget(routeState.target, CAREFUL_REFUSED_SKIP_MS, behindWhy);
                        // [b30_overpull] door spin guard (CountDoorSpin): a chain rooted in a closed door that still
                        // stands closed counts as a skip behind that door
                        constexpr char const* doorPrefix = "leader held at a closed door (";
                        if (size_t const at = behindWhy.find(doorPrefix); at != std::string::npos)
                        {
                            size_t const close = behindWhy.find(')', at);
                            size_t const space = close == std::string::npos ? close : behindWhy.rfind(' ', close);
                            uint32 const doorEntry =
                                space != std::string::npos && space > at
                                    ? uint32(std::strtoul(behindWhy.substr(space + 1, close - space - 1).c_str(),
                                                          nullptr, 10))
                                    : 0;
                            for (GameObject* door : ClosedDoors(map, leader))
                            {
                                if (!doorEntry || door->GetEntry() != doorEntry)
                                    continue;
                                bool spinLog = false;
                                uint32 spinSkips = 0;
                                uint32 spinWindowMs = 0;
                                if (CountDoorSpin(group.Run().runSeq, door->GetGUID(), now, spinLog, spinSkips,
                                                  spinWindowMs))
                                {
                                    group.ClearWaypoint(group.LeaderGuid());
                                    if (spinLog)
                                    {
                                        std::string opener;
                                        JsonObject fields = DoorSpinFields(group, leader, door, spinSkips,
                                                                           spinWindowMs, "behind a skip it rooted",
                                                                           opener);
                                        LOG_INFO("playerbots", "[door spin] {}: {} skips behind closed {} {} in {} s: "
                                                 "retarget holds {} s (opener: {})", group.Run().runId, spinSkips,
                                                 door->GetName(), door->GetEntry(), spinWindowMs / IN_MILLISECONDS,
                                                 DOOR_SPIN_HOLD_MS / IN_MILLISECONDS, opener);
                                        group.LogEvent("door_spin_hold", std::move(fields));
                                    }
                                }
                                break;
                            }
                        }
                        return;
                    }
                    // [b30_overpull] no walk to sight while the tank's planner holds this pull (a patrol / overlap
                    // wait, "held for now") or the group waits out a patrol here (patrol_wait_spot): scholo-n
                    // 20261025-184229-scholo-n-5D-8 147 s "patrol wait: Risen Guard comes to the group (eta 13 s)",
                    // 150 s "approach to sight of Risen Guard ... 16.8 yd on" past a Spectral Researcher (the
                    // pull_option spot's walk is not checked), 152 s the unplanned pull, the patrol joined it, 11
                    // attackers, the wipe. The hold stands at the edge stop; the planner's wait decides.
                    bool plannerHoldsHere = false;
                    {
                        bool holdHopeless = false;
                        uint32 holdWaitMs = 0;
                        std::string holdWhy;
                        bool const targetHeld =
                            PartyRefusal(group, routeState.target, holdHopeless, holdWaitMs,
                                                           holdWhy) &&
                            holdWhy.rfind("held for now", 0) == 0;
                        holdWhy.clear();
                        bool const blockerHeld =
                            PartyRefusal(group, blocker->GetGUID(), holdHopeless,
                                                           holdWaitMs, holdWhy) &&
                            holdWhy.rfind("held for now", 0) == 0;
                        bool const patrolWait = PatrolWaitOn(group);
                        plannerHoldsHere = !leader->IsInCombat() && (targetHeld || blockerHeld || patrolWait) &&
                                           !bossInvolved;
                        // a sight point taken before the hold is left: back to the edge stop
                        if (plannerHoldsHere && sightTried)
                        {
                            sightStops.erase(group.LeaderGuid());
                            sightTried = false;
                            approachStop = edgeStop;
                        }
                        if (plannerHoldsHere && ROUTE_APPROACH_TO_SIGHT && atStop &&
                            !leader->IsWithinLOSInMap(blocker))
                        {
                            static std::unordered_map<uint32, uint32> sightHeldLogMs;  // party seq (world thread)
                            uint32& logged = sightHeldLogMs[group.Run().runSeq];
                            if (!logged || getMSTimeDiff(logged, now) >= SIGHT_HOLD_LOG_MS)
                            {
                                logged = now ? now : 1;
                                group.LogEvent("approach_sight",
                                         JsonObject().Str("action", "held: the planner waits")
                                             .Str("name", object->GetName()).Num("entry", object->GetEntry())
                                             .Str("blocker", blocker->GetName())
                                             .Num("blockerEntry", blocker->GetEntry())
                                             .Bool("patrolWait", patrolWait).Bool("targetHeld", targetHeld)
                                             .Bool("blockerHeld", blockerHeld)
                                             .Real("x", here.x).Real("y", here.y).Real("z", here.z));
                            }
                        }
                    }
                    if (ROUTE_APPROACH_TO_SIGHT && atStop && !bossInvolved && !plannerHoldsHere &&
                        !leader->IsWithinLOSInMap(blocker))
                    {
                        // ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED: the walk to the sight point stays out of the reach of the
                        // target's and the blocker's packs unless the tank's planner accepted their plan; a point
                        // only inside it holds the leader at the edge (the stop ApproachStop chose), where the
                        // planner's hard minimum / pull back or the wait's skip decide. A hold is searched again
                        // after SIGHT_HOLD_RECHECK_MS.
                        // A fresh look-up: logHold above may have inserted into sightHolds (iterators invalidated).
                        auto const held = sightHolds.find(group.LeaderGuid());
                        std::vector<Creature*> keepOut;
                        PlanVerdict verdict = PlanVerdict::Unknown;
                        if (ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED && !sightTried)
                        {
                            keepOut = SightKeepOutPack(group, leader, mob, blocker);
                            verdict = TankPlanVerdict(group, leader, keepOut);
                        }
                        // [lead132] user: the hold is searched again when one of its facts changed, not after
                        // SIGHT_HOLD_RECHECK_MS: the blocker or the leader moved, or the planner's verdict changed
                        bool const recentHold =
                            held != sightHolds.end() && held->second.blocker == blocker->GetGUID() &&
                            blocker->GetExactDist(&held->second.blockerAt) <= STEER_PROGRESS_RANGE &&
                            leader->GetExactDist(&held->second.leaderAt) <= STEER_PROGRESS_RANGE &&
                            held->second.verdict == uint8(verdict);
                        bool gaveWay = false;    // the edge hold gives way: the tank cannot plan the pack (lead132)
                        bool intoReach = false;  // the sight point may lie in the target's (accepted) pack's reach
                        if (ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED && !sightTried && !recentHold)
                        {
                            // Sweep batch3 (SM Library 21:47): "holds at the edge: no accepted plan" for minutes
                            // while the tank had "no aggressive target" - it cannot plan a pack it does not see, so
                            // the hold waited for a verdict that never came. [lead132] that is the fact (not
                            // SIGHT_UNKNOWN_HOLD_MS 20 s / ROUTE_BLOCKER_SIGHT_HOLD_MS 8 s): an unknown verdict while
                            // the tank sees no mob of the pack gives way - walking into sight is the pull again (a
                            // refusal still holds)
                            bool const seesPack = std::any_of(keepOut.begin(), keepOut.end(),
                                                              [leader](Creature* packMob)
                                                              { return leader->IsWithinLOSInMap(packMob); });
                            if (!leader->IsInCombat() && held != sightHolds.end() &&
                                held->second.blocker == blocker->GetGUID() && !seesPack)
                                gaveWay = verdict == PlanVerdict::Unknown &&  // [queue-blocker-pull-gate]
                                          !(Leader::Leads(Leader::LEADER_NO_BODY_FALLBACK) &&
                                            group.LeaderDecides());  // [lead3] the leader never gives way
                            // Accepted (or given way): walking into sight of the pack is the pull - but of that pack
                            // only. Every other idle pack stays kept out (SightKeepOutOthers; BRS heroic 22:10:30). A
                            // give-way approaches the target's own pack alone, a neighbour blocker's pack included in
                            // the keep-out; an accepted plan covers the target's and the blocker's packs it was asked
                            // about.
                            if (verdict == PlanVerdict::Accepted || gaveWay)
                            {
                                std::vector<Creature*> const accepted = keepOut;
                                keepOut = SightKeepOutOthers(leader, mob);
                                if (!gaveWay)
                                    keepOut.erase(std::remove_if(keepOut.begin(), keepOut.end(),
                                                                 [&accepted](Creature* other)
                                                                 {
                                                                     return std::find(accepted.begin(), accepted.end(),
                                                                                      other) != accepted.end();
                                                                 }),
                                                  keepOut.end());
                                intoReach = true;
                            }
                        }
                        G3D::Vector3 sight;
                        bool heldAtEdge = recentHold;
                        bool found = !sightTried && !recentHold &&
                                     FindSightPoint(leader, blocker, mob, ownPackHere, leg.points, keepOut, sight,
                                                    heldAtEdge);
                        // [pull_option] (1) the sight pull: the route ahead showed no spot, so the arc round the
                        // pack (FindPullSightSpot: side steps that open a corner, real aggro distances). The spot
                        // stands outside every idle mob's aggro, so it needs no accepted plan: from there the tank's
                        // own pull (planner, 30 yd skill) takes the pack.
                        PullSightSearch pullSearch;
                        std::vector<Creature*> pullIdle;
                        auto const isBossMob = [&group, &routeState](Creature* unit)
                        {
                            return unit->IsDungeonBoss() || unit->isWorldBoss() ||
                                   routeState.bossCreatures.count(unit->GetGUID()) != 0 || group.BossOrder(unit) >= 0;
                        };
                        if (!found && !sightTried && !recentHold)
                        {
                            pullIdle = PullIdleMobs(leader, blocker);
                            FindPullSightSpot(leader, blocker, pullIdle, isBossMob, pullSearch);
                            group.LogEvent("sight_point_search",
                                     JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                                         .Str("blocker", blocker->GetName()).Num("blockerEntry", blocker->GetEntry())
                                         .Bool("found", pullSearch.found).Num("candidates", pullSearch.candidates)
                                         .Num("inOtherAggro", pullSearch.inOtherAggro)
                                         .Num("inOwnAggro", pullSearch.inOwnAggro).Num("noPath", pullSearch.noPath)
                                         .Num("touchesMore", pullSearch.touchesMore)
                                         .Num("touchesBoss", pullSearch.touchesBoss)
                                         .Real("margin", pullSearch.margin)
                                         .Real("minRadius", pullSearch.minRadius)
                                         .Real("maxRadius", pullSearch.maxRadius)
                                         .Real("x", here.x).Real("y", here.y).Real("z", here.z)
                                         .Real("toX", pullSearch.found ? pullSearch.best.at.x : 0.0f)
                                         .Real("toY", pullSearch.found ? pullSearch.best.at.y : 0.0f)
                                         .Real("toZ", pullSearch.found ? pullSearch.best.at.z : 0.0f)
                                         .Raw("top", PullCandidatesJson(pullSearch.top)));
                            if (pullSearch.found)
                            {
                                found = true;
                                heldAtEdge = false;
                                sight = pullSearch.best.at;
                                LOG_INFO("playerbots",
                                         "[pull_option] {}: sight pull of {} ({} mobs, boss no): arc spot "
                                         "({:.1f},{:.1f},{:.1f}) {:.1f} yd from it, {:.0f} deg off, {:.1f} yd walk",
                                         leader->GetName(), blocker->GetName(), pullSearch.best.touch.mobs.size(),
                                         sight.x, sight.y, sight.z, pullSearch.best.radius,
                                         pullSearch.best.angleOffDeg, pullSearch.best.walk);
                            }
                        }
                        std::string const view = PartyTankView(group, leader, blocker);
                        // The tank takes it as it stands (its view "accepted ..."): no skip, the normal wait at the
                        // stop decides, as for "skip at once: no LoS" below (lbrs-n build 10: the Scarshield Worg
                        // skipped "accepted", 21 targets behind it skipped with it, 0/9 in 12 minutes).
                        if (!found && gaveWay && view.rfind("accepted", 0) != 0)
                        {
                            // [unseen-blocker-pull] A small neighbour pack that holds the approach and that the tank
                            // does not see (no line of sight, so the planner never plans it) becomes the target
                            // itself: its own walk then steps into its sight - inside its own reach when nothing else
                            // stands there (the give-way keeps only the other packs out) - and that is its pull.
                            // Without this every target behind it was skipped in turn (st-n 2026-10-25 081744: Mijan,
                            // Zul'Lor and Hukku each "holds at the edge" behind the same unseen 3-mob Atal'ai Witch
                            // Doctor pack, then "no sight point outside other packs' aggro", the tank ping-ponged).
                            // [route_blocker_body_pull] a neighbour pack on the way, whatever its size, is the target
                            if (!ownPackHere && TargetRouteBlocker(group, leader, blocker, object,
                                                                   "holds the approach, no sight point (" + view +
                                                                       ")"))
                                return;
                            // ... and the route blocker that is the target is walked into
                            if (ownPackHere && IsRouteBlocker(group, routeState.target))
                            {
                                RouteBlockerBodyPull(group, leader, mob,
                                                     "no sight point outside other packs' aggro (" + view + ")");
                                return;
                            }
                            if (!ownPackHere && !routeState.skipUntil.count(blocker->GetGUID()) &&
                                routeState.neighbourTarget != blocker->GetGUID())
                            {
                                std::list<Creature*> blockerPack;
                                PackMobCheck blockerCheck{leader, blocker};
                                Acore::CreatureListSearcher<PackMobCheck> blockerSearcher(blocker, blockerPack,
                                                                                          blockerCheck);
                                Cell::VisitObjects(blocker, blockerSearcher, PACK_RADIUS);
                                if (std::find(blockerPack.begin(), blockerPack.end(), blocker) == blockerPack.end())
                                    blockerPack.push_back(blocker);
                                // [unseen_blocker_any_size] this blocker holds the approach (ApproachStop: the leg to
                                // the target enters its reach), so it stands on the way: no size cap (user rule, a
                                // pack on the only way is never skipped; st-n-92: a 5-mob Atal'ai Slave pack)
                                // [route_blocker_required_only] ... when the target behind it is a required one
                                std::string requiredWhy;
                                bool unseenHold = false;
                                std::string const unseenGate =
                                    BlockerPullGate(group, leader, blocker, object, false, unseenHold);
                                if (!unseenGate.empty() && unseenHold)
                                {
                                    group.ClearWaypoint(group.LeaderGuid());  // ready first, nothing skipped
                                    return;
                                }
                                if (unseenGate.empty() &&
                                    (blockerPack.size() <= UNSEEN_BLOCKER_PACK_MAX ||
                                     (ROUTE_BLOCKER_BODY_PULL &&
                                      RouteBlockerBehindRequired(group, map, object, requiredWhy))))
                                {
                                    if (blockerPack.size() > UNSEEN_BLOCKER_PACK_MAX)
                                        LOG_INFO("playerbots", "[unseen_blocker_any_size] {}: {} ({} mobs) holds the "
                                                 "approach to {}: the target despite its size", leader->GetName(),
                                                 blocker->GetName(), blockerPack.size(), object->GetName());
                                    group.LogEvent("approach_sight",
                                             JsonObject().Str("action", "pull the unseen blocker")
                                                 .Str("name", object->GetName()).Num("entry", object->GetEntry())
                                                 .Str("blocker", blocker->GetName())
                                                 .Num("blockerEntry", blocker->GetEntry())
                                                 .Num("packMobs", uint32(blockerPack.size())).Str("tankView", view)
                                                 .Real("blockerDistance", leader->GetExactDist(blocker)));
                                    sightStops.erase(group.LeaderGuid());
                                    sightHolds.erase(group.LeaderGuid());
                                    routeState.neighbourTarget = blocker->GetGUID();
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
                                    return;
                                }
                            }
                            // [pull_option] (2) / [boss_area_clear] No sight spot touches the pack alone. Trash round
                            // the next boss (BossAreaTrashOf) or a route blocker is never skipped: the group gets
                            // ready (RouteBlockerBodyPull: full health and mana, everyone here) and the tank takes
                            // the least dangerous option - the body pull, or a sight spot that touches more but
                            // less than the walk in (no boss first, then the fewest mobs). Never stand.
                            if (ROUTE_BOSS_AREA_CLEAR)
                            {
                                Creature* const areaBoss = RouteNextBoss(group.Run().runSeq, map);
                                char const* const area =
                                    areaBoss ? BossAreaTrashOf(leader, areaBoss, mob,
                                                               routeState.bossCreatures.count(mob->GetGUID()) != 0)
                                             : nullptr;
                                if ((area || IsRouteBlocker(group, routeState.target)) &&
                                    Leader::Leads(Leader::LEADER_NO_BODY_FALLBACK) && group.LeaderDecides())
                                {
                                    // [lead3] no sight fallback walk into more mobs, no body pull: the planner whole
                                    Leader::Dungeon::LeaderWholePack(group, leader, mob,
                                                    Acore::StringFormat("{}: no clean sight spot ({})",
                                                                        area ? area : "route blocker", view),
                                                    true, true);
                                    return;
                                }
                                if (area || IsRouteBlocker(group, routeState.target))
                                {
                                    if (pullIdle.empty())
                                        pullIdle = PullIdleMobs(leader, mob);
                                    std::vector<Creature*> intended;
                                    PullTouch const body = PullTouches(leader, mob, leg.points, pullIdle, isBossMob,
                                                                       intended);
                                    bool const sightFallback =
                                        pullSearch.hasFallback && PullTouchBetter(pullSearch.fallback.touch, body);
                                    PullTouch const& chosen = sightFallback ? pullSearch.fallback.touch : body;
                                    LOG_INFO("playerbots",
                                             "[pull_option] {}: {} ({}): no clean sight spot; body pull touches {} "
                                             "mobs (boss {}), best sight spot {} -> {}{}",
                                             leader->GetName(), mob->GetName(), area ? area : "route blocker",
                                             body.mobs.size(), body.boss ? body.bossName : std::string("no"),
                                             pullSearch.hasFallback
                                                 ? Acore::StringFormat("touches {} (boss {})",
                                                                       pullSearch.fallback.touch.mobs.size(),
                                                                       pullSearch.fallback.touch.boss
                                                                           ? pullSearch.fallback.touch.bossName
                                                                           : std::string("no"))
                                                 : std::string("none"),
                                             sightFallback ? "sight spot" : "body pull",
                                             chosen.mobs.size() > PULL_OPTION_MOB_CAP || chosen.boss
                                                 ? " (over the cap / boss: the least dangerous, group ready first)"
                                                 : "");
                                    group.LogEvent("pull_option",
                                             JsonObject().Str("name", mob->GetName()).Num("entry", mob->GetEntry())
                                                 .Str("choice", sightFallback ? "sight fallback" : "body pull")
                                                 .Num("bodyMobs", int64(body.mobs.size())).Bool("bodyBoss", body.boss)
                                                 .Num("bodyExtra", body.extra)
                                                 .Num("sightMobs", pullSearch.hasFallback
                                                                       ? int64(pullSearch.fallback.touch.mobs.size())
                                                                       : -1)
                                                 .Bool("sightBoss", pullSearch.hasFallback &&
                                                                        pullSearch.fallback.touch.boss)
                                                 .Num("intended", int64(intended.size()))
                                                 .Num("cap", PULL_OPTION_MOB_CAP)
                                                 .Bool("overCap", chosen.mobs.size() > PULL_OPTION_MOB_CAP)
                                                 .Bool("bossTouched", chosen.boss));
                                    if (area)
                                        group.LogEvent("boss_area_clear",
                                                 JsonObject().Str("boss", areaBoss->GetName())
                                                     .Num("bossEntry", areaBoss->GetEntry())
                                                     .Str("mob", mob->GetName()).Num("entry", mob->GetEntry())
                                                     .Str("why", area)
                                                     .Str("action", sightFallback ? "sight fallback" : "body pull"));
                                    if (sightFallback)
                                    {
                                        if (!group.GroupFullyReady(leader))
                                        {
                                            group.ClearWaypoint(group.LeaderGuid());
                                            return;
                                        }
                                        sightStops[group.LeaderGuid()] = {routeState.target, blocker->GetGUID(),
                                                                    pullSearch.fallback.at, true};
                                        sightHolds.erase(group.LeaderGuid());
                                        return;
                                    }
                                    RouteBlockerBodyPull(group, leader, mob,
                                                         Acore::StringFormat("{}: no sight spot outside other packs' "
                                                                             "aggro ({})",
                                                                             area ? area : "route blocker", view));
                                    return;
                                }
                            }
                            // The hold gave way and still no point shows the target without another pack's aggro: no
                            // walk in, the target is skipped as not seen (its pack marked unseen, as below).
                            LOG_INFO("playerbots",
                                     "[approach] {}: skip: no sight of {} ({:.1f} yd, {}) outside other packs' aggro "
                                     "({} kept out) after the edge hold",
                                     leader->GetName(), blocker->GetName(), leader->GetExactDist(blocker), view,
                                     keepOut.size());
                            group.LogEvent("approach_sight",
                                     JsonObject().Str("action", "skip: no sight outside other packs")
                                         .Str("name", object->GetName()).Num("entry", object->GetEntry())
                                         .Str("blocker", blocker->GetName()).Num("blockerEntry", blocker->GetEntry())
                                         .Bool("ownPack", ownPackHere).Bool("heldAtEdge", heldAtEdge)
                                         .Num("keptOut", uint32(keepOut.size())).Str("tankView", view)
                                         .Real("x", here.x).Real("y", here.y).Real("z", here.z));
                            sightStops.erase(group.LeaderGuid());
                            sightHolds.erase(group.LeaderGuid());
                            std::list<Creature*> pack;
                            PackMobCheck packCheck{leader, mob};
                            Acore::CreatureListSearcher<PackMobCheck> packSearcher(mob, pack, packCheck);
                            Cell::VisitObjects(mob, packSearcher, PACK_RADIUS);
                            if (std::find(pack.begin(), pack.end(), mob) == pack.end())
                                pack.push_back(mob);
                            for (Creature* packMob : pack)
                                if (!routeState.bossCreatures.count(packMob->GetGUID()) && !packMob->IsDungeonBoss() &&
                                    !leader->IsWithinLOSInMap(packMob))
                                    TankRejects::MarkUnseen(leader->GetGUID(), packMob->GetGUID());
                            group.SkipTarget(routeState.target, CAREFUL_REFUSED_SKIP_MS,
                                       "skip: no sight point outside other packs' aggro after the edge hold (" + view +
                                           ")");
                            return;
                        }
                        if (!found && heldAtEdge)
                        {
                            if (!recentHold)
                                logHold(verdict, keepOut, "");
                        }
                        else if (found)
                        {
                            sightStops[group.LeaderGuid()] = {routeState.target, blocker->GetGUID(), sight,
                                                     intoReach && !gaveWay};
                            sightHolds.erase(group.LeaderGuid());
                            approachStop = sight;
                            LOG_INFO("playerbots",
                                     "[approach] {}: approach to sight of {} ({:.1f} yd, {}): stop ({:.1f},{:.1f},"
                                     "{:.1f}) -> ({:.1f},{:.1f},{:.1f}) {:.1f} yd on",
                                     leader->GetName(), blocker->GetName(), leader->GetExactDist(blocker), view,
                                     here.x, here.y, here.z, sight.x, sight.y, sight.z, (sight - here).length());
                            group.LogEvent("approach_sight",
                                     JsonObject().Str("action", "approach to sight").Str("name", object->GetName())
                                         .Num("entry", object->GetEntry()).Str("blocker", blocker->GetName())
                                         .Num("blockerEntry", blocker->GetEntry()).Bool("ownPack", ownPackHere)
                                         .Real("blockerDistance", leader->GetExactDist(blocker)).Str("tankView", view)
                                         .Real("sightDistance", blocker->GetExactDist(sight.x, sight.y, sight.z))
                                         .Real("blockerRealAggro", RealAggroRange(blocker, leader))
                                         .Real("x", here.x).Real("y", here.y).Real("z", here.z)
                                         .Real("toX", sight.x).Real("toY", sight.y).Real("toZ", sight.z));
                        }
                        // The tank takes it without line of sight (its possible targets accept it: the planner's
                        // build-8 rule for route targets): no skip, the normal wait at the stop decides (wave 8
                        // ubrs-n: "skip at once: no LoS" with the tank view "accepted").
                        else if (view.rfind("accepted", 0) != 0 && ownPackHere &&
                                 IsRouteBlocker(group, routeState.target))
                        {
                            // [route_blocker_body_pull] no sight of the route blocker: walked into
                            RouteBlockerBodyPull(group, leader, mob, "no line of sight (" + view + ")");
                            return;
                        }
                        else if (view.rfind("accepted", 0) != 0 && !ownPackHere &&
                                 TargetRouteBlocker(group, leader, blocker, object, "no line of sight (" + view + ")"))
                            return;
                        else if (view.rfind("accepted", 0) != 0)
                        {
                            LOG_INFO("playerbots", "[approach] {}: skip at once: no LoS to {} ({:.1f} yd, {}){}",
                                     leader->GetName(), blocker->GetName(), leader->GetExactDist(blocker), view,
                                     sightTried ? " from the sight point" : ", no sight point on the route ahead");
                            group.LogEvent("approach_sight",
                                     JsonObject().Str("action", "skip at once: no LoS").Str("name", object->GetName())
                                         .Num("entry", object->GetEntry()).Str("blocker", blocker->GetName())
                                         .Num("blockerEntry", blocker->GetEntry()).Bool("ownPack", ownPackHere)
                                         .Real("blockerDistance", leader->GetExactDist(blocker)).Str("tankView", view)
                                         .Bool("fromSightPoint", sightTried)
                                         .Real("x", here.x).Real("y", here.y).Real("z", here.z));
                            sightStops.erase(group.LeaderGuid());
                            // The blocker's pack, what the tank does not see of it from here, holds no later walk.
                            std::list<Creature*> pack;
                            PackMobCheck packCheck{leader, blocker};
                            Acore::CreatureListSearcher<PackMobCheck> packSearcher(blocker, pack, packCheck);
                            Cell::VisitObjects(blocker, packSearcher, PACK_RADIUS);
                            if (std::find(pack.begin(), pack.end(), blocker) == pack.end())
                                pack.push_back(blocker);
                            for (Creature* packMob : pack)
                                if (!routeState.bossCreatures.count(packMob->GetGUID()) && !packMob->IsDungeonBoss() &&
                                    (packMob == blocker || !leader->IsWithinLOSInMap(packMob)))
                                    TankRejects::MarkUnseen(leader->GetGUID(), packMob->GetGUID());
                            if (ownPackHere)
                            {
                                group.SkipTarget(routeState.target, CAREFUL_REFUSED_SKIP_MS,
                                           "skip at once: no LoS (not pulled, tank does not see it: " + view + ")");
                                return;
                            }
                            // A neighbour: the next tick's stop no longer counts it, the walk goes on.
                            approachStopped = false;
                        }
                    }
                    if (approachStopped && (approachStop - here).length() <= APPROACH_ARRIVED)
                    {
                        arrived = true;
                        // A mob outside the target's pack (a neighbour on the way) ended the walk: a refusal from
                        // here skips its pack too.
                        bool const ownPack = ownPackHere;
                        ObjectGuid const blockerGuid = ownPack ? ObjectGuid::Empty : blocker->GetGUID();
                        // A stop that came late (DM wave 38: 16 yd from a static Goblin Woodcarver, inside its real
                        // aggro range, the lumber yard aggroed the next second, Sneed's Shredder walked in, wipe):
                        // the real range of the blocker and of the target (no margin) and the leader's own line of
                        // sight to them (the arrival test) against the eye-line test the stop used.
                        float const blockerAggro = blocker->GetAggroRange(leader) + blocker->m_CombatDistance;
                        float const targetAggro = mob->GetAggroRange(leader) + mob->m_CombatDistance;
                        if (routeState.approachBlocker != blockerGuid || !routeState.arrivedMs)
                            group.LogEvent("approach_stop",
                                     JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                                         .Str("blocker", blocker->GetName()).Num("blockerEntry", blocker->GetEntry())
                                         .Bool("ownPack", ownPack).Real("distance", distance)
                                         .Real("blockerDistance", leader->GetExactDist(blocker))
                                         .Real("blockerRealAggro", blockerAggro)
                                         .Real("targetRealAggro", targetAggro)
                                         .Bool("insideRealAggro", leader->GetExactDist(blocker) < blockerAggro ||
                                                                      distance < targetAggro)
                                         .Bool("blockerLos", leader->IsWithinLOSInMap(blocker))
                                         .Bool("targetLos", leader->IsWithinLOSInMap(mob))
                                         .Real("aggroReach", AggroReach(blocker, leader))
                                         .Bool("unseenStop", unseenStop)
                                         .Real("spreadReach", blockerAggro + APPROACH_FOLLOW_SPREAD)
                                         .Real("blockerDz", blocker->GetPositionZ() - here.z)
                                         .Real("x", here.x).Real("y", here.y).Real("z", here.z));
                        routeState.approachBlocker = blockerGuid;
                    }
                }
            }

        // [b47_plan] The walk to an event starter (a key door, a lever) had no idle-mob guard at all: the approach stop
        // above skips starters, and so does the walk guard below. scholo-h 20261026-093620-scholo-h-5G-4 (2359 s):
        // the walk to the Viewing Room Door 175167 (256 yd, through the Aberration / Ghoul rooms) sent the tank to
        // (192.0,99.3,104.7), 5 yd from the wandering Diseased Ghoul 48768 - pull 42 "unplanned (no planner pull)",
        // the Risen Aberrations with it, three deaths in 9 s; scholo-n 093620-5E-5 the same at ~2849 s. An idle mob
        // whose reach the next stretch of that walk enters becomes the target (TargetRouteBlocker): the tank's
        // planner pulls it like any route blocker, then the walk goes on.
        if (!arrived && routeState.targetIsStarter && !leader->IsInCombat() && !GroupDamage::NoDamageRelease(leader))
        {
            if (!legBuilt)
            {
                SteerLeg(leader, goal, leg);
                legBuilt = true;
            }
            Creature* blocker = nullptr;
            bool unseen = false;
            G3D::Vector3 stop;
            if (!(leg.type & PATHFIND_NOPATH) &&
                ApproachStop(leader, object, leg.points, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP, stop, blocker,
                             unseen, nullptr, &group) &&
                blocker && !blocker->IsInCombat() && !BehindStartersDoor(object, blocker, stop))
            {
                static std::set<std::pair<uint32, ObjectGuid>> starterBlockerLogged;  // world thread
                if (starterBlockerLogged.insert({group.Run().runSeq, blocker->GetGUID()}).second)
                    group.LogEvent("starter_walk_blocker",
                             JsonObject().Str("starter", object->GetName()).Num("starterEntry", object->GetEntry())
                                 .Str("blocker", blocker->GetName()).Num("blockerEntry", blocker->GetEntry())
                                 .Real("blockerDistance", leader->GetExactDist(blocker)).Real("x", stop.x)
                                 .Real("y", stop.y).Real("z", stop.z));
                if (TargetRouteBlocker(group, leader, blocker, object,
                                       "[b47_plan] stands on the walk to the event starter"))
                    return;
            }
        }
        // ROUTE_HARNESS_WALK_GUARD: a body pull walks into its own pack only. The approach stop above skips a body
        // pull, and its 30-yd waypoint ran through every pack on the boss's route (wave 7: scholo-h pulls 9-12 on the
        // 325-yd route to Jandice, the refused Dark Summoner pack, the Adepts behind the opened Iron Gate; mt-h / mt-n
        // on the 215-yd walk to Shaffar). Another idle pack ends the body pull at its edge.
        if (ROUTE_HARNESS_WALK_GUARD && !arrived && routeState.approach && !routeState.targetIsStarter)
            if (Creature* mob = object->ToCreature())
            {
                SteerLeg(leader, goal, leg);
                legBuilt = true;
                Creature* blocker = nullptr;
                bool held = false;
                G3D::Vector3 stop;
                bool const walkStopped =
                    !(leg.type & PATHFIND_NOPATH) &&
                    HarnessWalkStop(leader, leg.points, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP, mob, stop,
                                    blocker, held, &group);
                // [route_blocker_walk_through] The walk into a route blocker (IsRouteBlocker: the user's body pull of
                // a pack on the only way) is not ended by the next idle pack of the same crowd: it is part of that
                // pull, it joins the route blocker's pack and the walk goes on. st-h 2026-10-25 2705-2823 s: the
                // body pull of the Mummified Atal'ai (Jammal'an's room, the planner: 19 mobs come either way)
                // stopped every 16 s at the Atal'ai Deathwalker 14 yd beside it ("harness_hold_refused_pack",
                // walk "body pull"), approach reset, the body pull logged again - 2 minutes, then the 4-minute stall.
                if (walkStopped && blocker && IsRouteBlocker(group, routeState.target) && !blocker->IsDungeonBoss() &&
                    !routeState.bossCreatures.count(blocker->GetGUID()) && group.BossOrder(blocker) < 0 &&
                    blocker->GetExactDist(mob) <= ROUTE_BLOCKER_THROUGH_RANGE)
                {
                    if (std::find(routeState.routeBlockerPack.begin(), routeState.routeBlockerPack.end(),
                                  blocker->GetGUID()) == routeState.routeBlockerPack.end())
                    {
                        routeState.routeBlockerPack.push_back(blocker->GetGUID());
                        routeState.skipUntil.erase(blocker->GetGUID());
                        routeState.skipReason.erase(blocker->GetGUID());
                        group.LogEvent("route_blocker_walk_through",
                                 JsonObject().Str("target", mob->GetName()).Num("targetEntry", mob->GetEntry())
                                     .Str("joins", blocker->GetName()).Num("joinsEntry", blocker->GetEntry())
                                     .Real("apart", blocker->GetExactDist(mob))
                                     .Real("blockerDistance", leader->GetExactDist(blocker)));
                    }
                }
                else if (walkStopped)
                {
                    // [boss_on_the_way_first] another boss of the run stops the body-pull walk to this boss (dme-h
                    // 20261025-153805-dme-h-5A-22: Hydrospawn in the pool between the tank and Zevrim 20 yd above,
                    // body_pull <-> approach_stop every 2 s for 2 minutes, waves 12 / 17 the same): the boss on the
                    // way is fought first - a player clears what stands on the only way up.
                    bool const blockerIsRunBoss =
                        blocker && blocker->IsAlive() && !blocker->IsInCombat() && !blocker->IsImmuneToPC() &&
                        (blocker->IsDungeonBoss() || routeState.bossCreatures.count(blocker->GetGUID())) &&
                        blocker->GetGUID() != routeState.target &&
                        !ClassGatedBoss(group.MapId(), group.Difficulty(), blocker->GetEntry());
                    if (blockerIsRunBoss && routeState.targetIsBoss)
                    {
                        group.LogEvent("boss_on_the_way_first",
                                 JsonObject().Str("boss", mob->GetName()).Num("bossEntry", mob->GetEntry())
                                     .Str("first", blocker->GetName()).Num("firstEntry", blocker->GetEntry())
                                     .Real("firstDistance", leader->GetExactDist(blocker))
                                     .Real("bossDz", mob->GetPositionZ() - leader->GetPositionZ()));
                        routeState.target = blocker->GetGUID();
                        routeState.targetIsBoss = true;
                        routeState.targetIsStarter = false;
                        routeState.targetRouteRule = "boss on the way";
                        routeState.targetSinceMs = now;
                        routeState.approach = 0;
                        routeState.arrivedMs = 0;
                        routeState.stopSinceMs = 0;
                        routeState.approachBlocker.Clear();
                        routeState.orderHoldSinceMs.erase(blocker->GetGUID());
                        group.ClearWaypoint(group.LeaderGuid());
                        return;
                    }
                    // [b33_dme] the blocker is no boss, but its pack brings one of the run (BossBroughtBy: Hydrospawn's
                    // wander reaches the Wildspawn Hellcaller's stand, dme-n 20261026-000434-5H-40): the planner
                    // refuses it as that boss's fight, so that boss is fought first, as when he stood on the way.
                    Creature* broughtBoss = nullptr;
                    if (ROUTE_BOSS_BROUGHT_BY_PACK && blocker && !blockerIsRunBoss && routeState.targetIsBoss &&
                        !blocker->IsInCombat())
                        if (Map* walkMap = leader->GetMap())
                            for (ObjectGuid const& guid : routeState.bossCreatures)
                            {
                                Creature* boss = walkMap->GetCreature(guid);
                                if (!boss || guid == routeState.target || boss == blocker ||
                                    ClassGatedBoss(group.MapId(), group.Difficulty(), boss->GetEntry()) ||
                                    !BossBroughtBy(leader, boss, blocker))
                                    continue;
                                if (!broughtBoss || leader->GetExactDist(boss) < leader->GetExactDist(broughtBoss))
                                    broughtBoss = boss;
                            }
                    if (broughtBoss)
                    {
                        group.LogEvent("boss_on_the_way_first",
                                 JsonObject().Str("boss", mob->GetName()).Num("bossEntry", mob->GetEntry())
                                     .Str("first", broughtBoss->GetName()).Num("firstEntry", broughtBoss->GetEntry())
                                     .Real("firstDistance", leader->GetExactDist(broughtBoss))
                                     .Str("via", blocker->GetName()).Num("viaEntry", blocker->GetEntry())
                                     .Real("viaDistance", leader->GetExactDist(blocker))
                                     .Str("why", "the blocker's pack brings this boss [b33_dme]")
                                     .Real("bossDz", mob->GetPositionZ() - leader->GetPositionZ()));
                        routeState.target = broughtBoss->GetGUID();
                        routeState.targetIsBoss = true;
                        routeState.targetIsStarter = false;
                        routeState.targetRouteRule = "boss on the way";
                        routeState.targetSinceMs = now;
                        routeState.approach = 0;
                        routeState.arrivedMs = 0;
                        routeState.stopSinceMs = 0;
                        routeState.approachBlocker.Clear();
                        routeState.orderHoldSinceMs.erase(broughtBoss->GetGUID());
                        group.ClearWaypoint(group.LeaderGuid());
                        return;
                    }
                    // [b30_stalls] trash of a boss's area whose only walk runs through that boss (it joins his fight,
                    // JoinsBossFight): it is no pull before him - the planner refuses its pack ("brings a boss"), the
                    // boss_area_clear hold keeps the boss back, deadlock. brd-h 20261025-184229-brd-h-5H-9: Anvilrage
                    // Warden / Bloodhound below Gerstahn, body pull stopped at Gerstahn every 10 s, stalled 0/19
                    // (build 29 broke out only by an accidental edge-walk aggro). The trash stays skipped (a
                    // ReachabilitySkip reason, never unskipped by boss_area_clear) and the boss is the pull.
                    if (blockerIsRunBoss && !routeState.targetIsBoss && JoinsBossFight(leader, blocker, mob))
                    {
                        group.LogEvent("boss_on_the_way_first",
                                 JsonObject().Str("boss", blocker->GetName()).Num("bossEntry", blocker->GetEntry())
                                     .Str("trash", mob->GetName()).Num("trashEntry", mob->GetEntry())
                                     .Str("why", "trash reached only through its boss [b30_stalls]")
                                     .Real("bossDistance", leader->GetExactDist(blocker)));
                        std::list<Creature*> walled;
                        PackMobCheck wallCheck{leader, mob};
                        Acore::CreatureListSearcher<PackMobCheck> wallSearcher(mob, walled, wallCheck);
                        Cell::VisitObjects(mob, wallSearcher, PACK_RADIUS);
                        walled.push_back(mob);
                        for (Creature* trash : walled)
                            if (trash != blocker && !trash->IsDungeonBoss() &&
                                !routeState.bossCreatures.count(trash->GetGUID()) &&
                                JoinsBossFight(leader, blocker, trash))
                            {
                                routeState.skipUntil[trash->GetGUID()] = now + UNREACHABLE_SKIP_MS;
                                routeState.skipReason[trash->GetGUID()] =
                                    "boss area: given up (reached only through the boss)";
                            }
                        routeState.target = blocker->GetGUID();
                        routeState.targetIsBoss = true;
                        routeState.targetIsStarter = false;
                        routeState.targetRouteRule = "boss on the way";
                        routeState.targetSinceMs = now;
                        routeState.approach = 0;
                        routeState.arrivedMs = 0;
                        routeState.stopSinceMs = 0;
                        routeState.approachBlocker.Clear();
                        routeState.orderHoldSinceMs.erase(blocker->GetGUID());
                        group.ClearWaypoint(group.LeaderGuid());
                        return;
                    }
                    LogHarnessHold(group, leader, blocker, "body pull", stop.x, stop.y, stop.z);
                    routeState.approach = 0;  // back to the wait at the stop: the neighbour hold decides
                    routeState.arrivedMs = 0;
                    routeState.stopSinceMs = 0;
                    routeState.approachBlocker = blocker->GetGUID();
                    approachStopped = true;
                    approachStop = stop;
                }
            }

        // Progress of the walk to this target. Utgarde Pinnacle wave 8 (and wave 9 at the entrance): a Mindless
        // Servant on a ledge 8 yd above the hall floor - the route ends under it, the same-floor rule never lets the
        // leader "arrive", the waypoint is where it stands (or the leader's own move refuses it), and the group stood
        // there 21 minutes; servants walking in kept the stall detector quiet. A player pulls it from below when it
        // is in range and sight, and else goes elsewhere.
        // A walk that swings back and forth (ohb-h wave 8: the harness route against the bots' own event step,
        // 2144,256 <-> 2167,275 every 30 s, 4 minutes) resets the 3-yd anchor on every swing: an item starter's object
        // the leader comes no nearer to in STARTER_NO_NEARER_MS of steering is stuck as well.
        if (routeState.steerTarget != routeState.target)
        {
            routeState.steerNearest = -1.0f;
            routeState.steerNoNearerMs = 0;
            starterNearestPath.erase(group.InstanceId());
        }
        else if (routeState.steerLookPull == group.PullSeq())
            routeState.steerNoNearerMs += getMSTimeDiff(routeState.steerLastMs, now);
        if (routeState.steerNearest < 0.0f || distance + STEER_PROGRESS_RANGE < routeState.steerNearest)
        {
            routeState.steerNearest = distance;
            routeState.steerNoNearerMs = 0;
        }
        // [lead132] the swing is a fact of the walk, not STARTER_NO_NEARER_MS 90 s: the leader walked back off the
        // nearest point it came to (STEER_PROGRESS_RANGE farther again) - the route-length check below tells a walk
        // round from a swing
        bool swingStuck = routeState.targetIsStarter && routeState.steerNearest >= 0.0f &&
                          distance > routeState.steerNearest + STEER_PROGRESS_RANGE &&
                          routeState.starterIdx < routeState.starters.size() &&
                          routeState.starters[routeState.starterIdx].def->kind == StarterKind::UseItem;
        // [starter_route_progress] A walk that has to go round: progress is the remaining walk, not the straight
        // distance (LBRS: Urok's Tribute Pile is ~1100 yd of path for 95 yd straight, over the z -18.9 floor; brs-h
        // 02:56 / 06:58 and lbrs-n 05:33 were steer_stuck 75-81 yd under it, dz 67.4, on the right route). Measured
        // only when the straight metric fires; a swing (Old Hillsbrad) shortens no path and stays stuck.
        if (swingStuck)
        {
            PathResult const togo = EvaluatePath(leader, object);
            float& nearestPath = starterNearestPath[group.InstanceId()];
            if (togo.reachable && togo.length > 0.0f &&
                (nearestPath <= 0.0f || togo.length + STEER_PROGRESS_RANGE < nearestPath))
            {
                group.LogEvent("starter_route_progress",
                         JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                             .Real("straight", distance).Real("route", togo.length).Real("routeBefore", nearestPath));
                nearestPath = togo.length;
                routeState.steerNoNearerMs = 0;
                swingStuck = false;
            }
        }
        // [lead134] the measurement starts anew after a fight between the looks (the pull serial moved), not after
        // STEER_GAP_MS 2 s without a look
        bool const fightBetween = routeState.steerLookPull != group.PullSeq();
        routeState.steerLookPull = group.PullSeq();
        if (routeState.steerTarget != routeState.target || fightBetween ||
            leader->GetExactDist(&routeState.steerAnchor) > STEER_PROGRESS_RANGE)
        {
            // [w24b-steer-twin-keep] The 4 s retarget flipping between the wanderers of one pack (Botanica bot-n
            // 2026-10-25: Frayer Wildlings, 8 flips from 1538 s to the stop, the leader stood 95 s, no steer_stuck)
            // keeps the clock: a new target of the old one's entry within STEER_TWIN_RANGE of it, the leader not
            // moved, is the same stop.
            Creature const* before =
                routeState.steerTarget != routeState.target ? map->GetCreature(routeState.steerTarget) : nullptr;
            Creature const* after = object ? object->ToCreature() : nullptr;
            bool const twinFlip = before && after && before->IsAlive() && before->GetEntry() == after->GetEntry() &&
                                  before->GetExactDist(after) <= STEER_TWIN_RANGE &&
                                  !fightBetween &&
                                  leader->GetExactDist(&routeState.steerAnchor) <= STEER_PROGRESS_RANGE;
            if (twinFlip)
            {
                LOG_INFO("playerbots", "[w24b-steer-twin-keep] {}: {} -> a twin, the steer clock keeps {} s",
                         group.Run().runId, after->GetName(),
                         getMSTimeDiff(routeState.steerSinceMs, now) / IN_MILLISECONDS);
                routeState.steerTarget = routeState.target;
            }
            else
            {
                if (routeState.steerTarget != routeState.target)
                    routeState.steerReachWaived = false;
                routeState.steerTarget = routeState.target;
                routeState.steerAnchor = leader->GetPosition();
                routeState.steerSinceMs = now;
            }
        }
        routeState.steerLastMs = now;
        if (STEER_REST_NOT_STUCK && !arrived && !leader->IsInCombat() &&
            (RestState::IsDrinking(leader) || RestState::IsEating(leader)))
        {
            routeState.steerSinceMs = now;
            routeState.steerAnchor = leader->GetPosition();
            routeState.steerNoNearerMs = 0;
            // [queue-pull-hold-not-stuck] nor the drive watchdog's "own walk made no progress" (brd-n
            // 20261025-115805-brd-n-5A-2: Rlpalbx drinking at (610.4, -184.4) at 1146-1157 s, bot_cannot_move "route").
            routeState.driveSinceMs = now;
            routeState.driveAnchor = leader->GetPosition();
            if (steerRestLogged.insert({group.Run().runSeq, routeState.target}).second)
                group.LogEvent("steer_rest_not_stuck",
                         JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                             .Str("rest", RestState::IsDrinking(leader) ? "drinking" : "eating")
                             .Real("mana", leader->GetMaxPower(POWER_MANA) ? leader->GetPowerPct(POWER_MANA) : 100.0f)
                             .Real("hp", leader->GetHealthPct()).Real("distance", distance)
                             .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                             .Real("z", leader->GetPositionZ()));
        }
        // [b47_door] A member still preparing (its rebuff round open: a warlock's Create Spellstone / Soulstone casts
        // after the run-start buffing step closed at its 15 s bound; or sitting below its line) holds the tank's pull
        // and its walk (PullReadiness::GroupPreparing) - no lack of progress either. brd-h 20261026-104908-brd-h-5D-6:
        // the bear stood 27-48 s at the entrance while Rtlocaa made its stones, "own walk made no progress", the
        // Anvilrage Overseer was skipped as a route end for 300 s, every mob behind it "behind skipped", and the route
        // went to the Shadowforge Gate 170559, whose key starter waits for Roccor (afterDead): 2+ min idle. Bounded by
        // PREPARING_NOT_STUCK_MAX_MS per stand, so a member whose window never closes still ends in the old skip.
        // [lead134] while a member prepares (its fact), no lack of progress - no PREPARING_NOT_STUCK_MAX_MS bound
        if (!arrived && !leader->IsInCombat() && PullReadiness::GroupPreparing(leader))
        {
            routeState.steerSinceMs = now;
            routeState.steerAnchor = leader->GetPosition();
            routeState.steerNoNearerMs = 0;
            routeState.driveSinceMs = now;
            routeState.driveAnchor = leader->GetPosition();
        }
        // [lead2] LEADER_ONE_TANK_MOVER: the tank's open pull order owns its movement - no route stall while it is
        // open, and no clock hands the tank back (PULL_HOLD_STALL_MS / PULL_HOLD_RELEASE_MS are the A/B's old path):
        // the order's own events do (DrainOrderReports; a lost report is order_orphan, a bug event).
        static_assert(Leader::LEADER_ONE_TANK_MOVER, "[lead131] the pull-hold stall clocks are gone");
        {
            if (!arrived && !leader->IsInCombat() && WaypointHeldForPull(group, leader))
            {
                routeState.steerSinceMs = now;
                routeState.steerAnchor = leader->GetPosition();
                routeState.steerNoNearerMs = 0;
                routeState.driveSinceMs = now;
                routeState.driveAnchor = leader->GetPosition();
                Leader::PullOrder order;
                PullHoldTrack& hold = pullHolds[group.LeaderGuid()];
                if (Leader::Orders::GetOpen(group.LeaderGuid(), order) && hold.orderId != order.id)
                {
                    hold.orderId = order.id;
                    group.LogEvent("pull_hold", JsonObject().Str("name", object->GetName())
                                                     .Num("entry", object->GetEntry()).Real("distance", distance)
                                                     .Str("pull", TankPullState(leader)).Num("order", order.id)
                                                     .Str("orderState", Leader::StateName(order.state))
                                                     .Str("orderMethod", Leader::MethodName(order.method))
                                                     .Str("orderTarget", order.targetName)
                                                     .Real("x", leader->GetPositionX())
                                                     .Real("y", leader->GetPositionY())
                                                     .Real("z", leader->GetPositionZ()));
                }
            }
        }
        if (!arrived && routeState.steerReachWaived && distance <= PULL_RANGE)
            arrived = true;
        // [lead130] user: no elapsed-time rule decides (STEER_STUCK_MS 20 s without 3 yd of progress was the old
        // clock). Stuck is the fact that the walk does not move the leader: no excuse this tick (rest, a member
        // preparing, the pull's hold, 3 yd of progress - each restarts the steer anchor) and the leader, given a
        // walk point, stands with that same walk point at the same spot as at the last look (LeaderWalkStands)
        bool const walkStands = routeState.steerSinceMs != now &&
                                LeaderWalkStands(group, leader, routeState.steerStandAt, routeState.steerStandSerial);
        if (!arrived && (walkStands || swingStuck))
        {
            // A released Violet Hold prisoner is the objective, never "route end" (vh-h wave 9: Zuramat skipped at
            // 7 yd): the bots' own "vh engage" walks to him from here.
            Creature const* stuckOn = object->ToCreature();
            bool const releasedPrisoner = stuckOn && leader->GetMapId() == VIOLET_HOLD_MAP_ID &&
                                          routeState.bossCreatures.count(stuckOn->GetGUID()) &&
                                          !stuckOn->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
            // [b32_bm] a walk-in (routeState.approach) that stopped beside the target is in reach too: bm-h
            // 20261025-223301-bm-h-5H-72 skipped Aeonus at 6.0 yd as "route end" for 300 s (melee range 5.8).
            bool const walkInBeside = !routeState.targetIsStarter && routeState.approach && stuckOn &&
                                      distance <= leader->GetMeleeRange(stuckOn) + BODY_PULL_STUCK_REACH &&
                                      leader->IsWithinLOSInMap(object);
            bool const inReach = releasedPrisoner || walkInBeside ||
                                 (!routeState.targetIsStarter && !routeState.approach && distance <= PULL_RANGE &&
                                  leader->IsWithinLOSInMap(object));
            // [hleft-ak-jedoga-owns] Ahn'kahet: a Twilight Initiate 30114 is never "route end" while Jedoga lives or
            // her room owns the pull (AKJedogaRoom::OwnsPull): the AK strategy brings them, she comes only after.
            if (object->GetEntry() == AK_TWILIGHT_INITIATE_ENTRY)
            {
                PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
                bool jedogaAlive = false;
                for (ObjectGuid const& guid : routeState.bossCreatures)
                    if (Creature* boss = map->GetCreature(guid))
                        jedogaAlive |= boss->IsAlive() && boss->GetEntry() == AK_JEDOGA_ENTRY;
                if ((leaderAI && AKJedogaRoom::OwnsPull(leaderAI)) || jedogaAlive)
                {
                    routeState.steerSinceMs = now;
                    group.ClearWaypoint(group.LeaderGuid());
                    return;
                }
            }
            // [selin_entry_no_route_end] A live, idle Selin is never "route end" while her room tactic holds the
            // entry (MagistersTerrace::SelinRoom::EntryHolds: nobody pulls her until every member stands inside):
            // the steer stands still by design. mgt-n 2026-10-25 (20261025-115805-mgt-n-5A-15, 627 s): she was
            // skipped for 300 s while the members swayed at x 206-218, everything else lay behind passage gate
            // 187979, 215 s idle.
            // [lead135] while the entry walk holds (its fact), no cap (SELIN_ENTRY_HOLD_MAX_MS 180 s was the old one)
            struct SelinEntryHold
            {
                uint32 sinceMs = 0;
                uint32 seenMs = 0;
                uint32 logMs = 0;
            };
            static std::unordered_map<uint32, SelinEntryHold> selinEntryHolds;  // instance id (world thread)
            if (Creature const* boss = object->ToCreature();
                boss && boss->GetEntry() == 24723 /* Selin Fireheart */ && boss->IsAlive() && !boss->IsInCombat() &&
                leader->GetMapId() == MagistersTerrace::MAP_MAGISTERS_TERRACE &&
                MagistersTerrace::SelinRoom::EntryHolds(leader))
            {
                SelinEntryHold& hold = selinEntryHolds[group.InstanceId()];
                if (!hold.sinceMs)
                    hold.sinceMs = now ? now : 1;
                hold.seenMs = now ? now : 1;
                {
                    routeState.steerSinceMs = now;
                    group.ClearWaypoint(group.LeaderGuid());
                    if (!hold.logMs || getMSTimeDiff(hold.logMs, now) >= 10 * IN_MILLISECONDS)
                    {
                        hold.logMs = now ? now : 1;
                        LOG_INFO("playerbots", "[selin_entry_no_route_end] {}: Selin is no route end while her room "
                                 "entry holds ({:.1f} yd, {} s)", group.Run().runId, distance,
                                 getMSTimeDiff(hold.sinceMs, now) / IN_MILLISECONDS);
                    }
                    return;
                }
            }
            // [hleft-swim-back] held in the water below the target (a ledge the swimmer cannot climb): back the way
            // it swam in, before any skip.
            if (ROUTE_SWIM_BACK && !routeState.swimBack && !releasedPrisoner && leader->IsInWater() &&
                routeState.swimTrail.size() >= 2 && object->GetPositionZ() > leader->GetPositionZ() + SWIM_CLIMB_DZ)
            {
                routeState.swimBack = true;
                routeState.swimBackSinceMs = now;
                routeState.steerSinceMs = now;
                group.LogEvent("swim_back", JsonObject().Str("action", "back along the swim")
                                                 .Str("target", object->GetName())
                                                 .Real("dz", object->GetPositionZ() - leader->GetPositionZ())
                                                 .Num("trail", uint32(routeState.swimTrail.size()))
                                                 .Real("dryX", routeState.swimTrail.front().GetPositionX())
                                                 .Real("dryY", routeState.swimTrail.front().GetPositionY())
                                                 .Real("dryZ", routeState.swimTrail.front().GetPositionZ()));
                return;
            }
            group.LogEvent("steer_stuck", JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                                               .Bool("eventStarter", routeState.targetIsStarter)
                                               .Bool("bodyPull", routeState.approach != 0).Real("distance", distance)
                                               .Real("dz", object->GetPositionZ() - leader->GetPositionZ())
                                               .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                               .Real("z", leader->GetPositionZ())
                                               .Str("action", inReach ? "pull from here" : "skip"));
            if (inReach)
            {
                routeState.steerReachWaived = true;
                arrived = true;
            }
            else if (routeState.targetIsStarter)
            {
                StarterTrack& starter = routeState.starters[routeState.starterIdx];
                // One of several objects that take the item (Old Hillsbrad wave 34: the walk to the barrel at
                // 2119,42 ended 11 yd above it on the Durnholde wall, steer_stuck; the starter waited 2 minutes and
                // chose the same nearest barrel again, four times, 0 of 5 planted in 25 minutes): that object is
                // passed over, the next nearest one comes at once - a player goes to another barrel.
                // [lead132] passed over until the leader is elsewhere (STARTER_OBJECT_STUCK_SKIP_MS 10 min /
                // STARTER_STUCK_RETRY_MS 2 min were the old clocks)
                starter.stuckFrom[routeState.target] = leader->GetPosition();
                starter.nextAllowedMs = now;
                routeState.target.Clear();
                routeState.nextRetargetMs = now;
                group.ClearWaypoint(group.LeaderGuid());
                return;
            }
            else
            {
                // A boss holds the later ones as "next boss unreachable" does (order hold).
                if (routeState.targetIsBoss)
                    routeState.orderHoldSinceMs.emplace(routeState.target, now);
                // A boss in another wing (Scarlet Monastery heroic 2026-10-25: Graveyard cleared, Loksey / Doan
                // walked to the Graveyard exit 5 times, 1959 yd short, 4 minutes without a fight - the wing change
                // waited for "no targets", which the boss itself never let come): the group changes to the
                // entrance nearest to him.
                if (routeState.targetIsBoss && routeState.entrances.size() > 1 &&
                    routeState.entranceIdx < routeState.entrances.size() && !routeState.wingWalk &&
                    !EventGatherApplies(group) && EventSpotIndex(group) < 0)
                    if (int32 const wing = NearestEntrance(group, object);
                        wing >= 0 && size_t(wing) != routeState.entranceIdx &&
                        routeState.entrances[size_t(wing)].GetExactDist(
                            &routeState.entrances[routeState.entranceIdx]) >= SAME_ENTRANCE_RANGE)
                    {
                        StartWingChange(group, size_t(wing), "boss route end in another wing");
                        return;
                    }
                // Ahn'kahet wave 9/10: a ledge of Swarmers 7 yd above the leader - skipping them one at a time
                // retargets the next one 20 s later, for 4 minutes. Skip the same-entry mobs around it with it, as
                // "route end" from here (unreachableFrom: checked again once the leader stands elsewhere); never a
                // boss or an encounter creature.
                uint32 twinsSkipped = 0;
                // Never for the Twilight Initiates 30114 (Ahn'kahet): Jedoga's gate, she comes only once they are
                // dead (wotlk tactic audit, wave 21) - skipped by the dozen they kept her away ("twin_skip_exempt").
                bool const twinExempt = object->GetEntry() == 30114;
                if (twinExempt)
                    LOG_INFO("playerbots", "[route] {}: twin_skip_exempt: {} {} - only this one skipped",
                             group.Run().runId, object->GetName(), object->GetEntry());
                if (!routeState.targetIsBoss && ROUTE_TWIN_SKIP && !twinExempt)
                    if (Creature* mob = object->ToCreature())
                    {
                        std::list<Creature*> twins;
                        Acore::AllCreaturesOfEntryInRange check(mob, mob->GetEntry(), STEER_TWIN_RANGE);
                        Acore::CreatureListSearcher<Acore::AllCreaturesOfEntryInRange> searcher(mob, twins, check);
                        Cell::VisitObjects(mob, searcher, STEER_TWIN_RANGE);
                        for (Creature* twin : twins)
                            if (twin != mob && twin->IsAlive() && !twin->IsInCombat() && !twin->IsDungeonBoss() &&
                                !twin->isWorldBoss() && !routeState.bossCreatures.count(twin->GetGUID()) &&
                                group.BossOrder(twin) < 0)
                            {
                                routeState.skipUntil[twin->GetGUID()] = now + UNREACHABLE_SKIP_MS;
                                routeState.skipReason[twin->GetGUID()] = "route end (same entry)";
                                routeState.unreachableFrom[twin->GetGUID()] = leader->GetPosition();
                                if (!leader->IsWithinLOSInMap(twin))
                                    MarkUnseenRouteEnd(twin, now + UNREACHABLE_SKIP_MS);
                                ++twinsSkipped;
                            }
                    }
                if (!routeState.targetIsBoss)
                    if (Creature* mob = object->ToCreature(); mob && !leader->IsWithinLOSInMap(mob))
                    {
                        MarkUnseenRouteEnd(mob, now + UNREACHABLE_SKIP_MS);
                        // Its pack too, what of it the leader does not see (the route skips them "behind" it).
                        std::list<Creature*> pack;
                        PackMobCheck check{leader, mob};
                        Acore::CreatureListSearcher<PackMobCheck> searcher(mob, pack, check);
                        Cell::VisitObjects(mob, searcher, PACK_RADIUS);
                        for (Creature* packMob : pack)
                            if (packMob != mob && !packMob->IsDungeonBoss() &&
                                !routeState.bossCreatures.count(packMob->GetGUID()) &&
                                !leader->IsWithinLOSInMap(packMob))
                                MarkUnseenRouteEnd(packMob, now + UNREACHABLE_SKIP_MS);
                        group.LogEvent("unseen_route_end",
                                 JsonObject().Str("name", mob->GetName()).Num("entry", mob->GetEntry())
                                     .Real("distance", leader->GetExactDist(mob))
                                     .Num("twins", twinsSkipped));
                    }
                group.SkipTarget(routeState.target, UNREACHABLE_SKIP_MS,
                           twinsSkipped ? "route end (+" + std::to_string(twinsSkipped) + " same entry)"
                                        : std::string("route end"));
                return;
            }
        }

        if (arrived)
        {
            routeState.steerSinceMs = now;  // waiting for the pull is no lack of progress
            group.ClearWaypoint(group.LeaderGuid());
            if (routeState.targetIsStarter)
            {
                StarterTrack& starter = routeState.starters[routeState.starterIdx];
                // Used up meanwhile (MarkStartersDoneByGroup): nothing to click any more.
                bool const used = starter.uses < starter.def->maxUses && UseStarter(group, leader, starter, object);
                // [lead130] the target is used: no target is the retarget's fact (RetargetDue) - no 3 s ask
                routeState.target.Clear();
                // [ulda-ritual-hold] a summoning ritual casts its spell after the click only while the user still
                // channels (GameObject::Update CheckRitualList): no new walk while the leader channels it.
                // [lead130] the hold is that fact (its channel), not RITUAL_CHANNEL_HOLD_MS 6.5 s
                if (GameObject const* ritual = object->ToGameObject();
                    used && ritual && ritual->GetGoType() == GAMEOBJECT_TYPE_SUMMONING_RITUAL)
                {
                    ritualHoldUntilMs[group.InstanceId()] = now ? now : 1;  // the ritual used (since)
                    group.LogEvent("ritual_hold", JsonObject().Str("note", starter.def->note)
                                                       .Num("entry", ritual->GetEntry())
                                                       .Str("until", "the leader's channel ends"));
                }
                return;
            }

            if (!routeState.arrivedMs)
            {
                // ROUTE_STOP_KEEPS_WAIT: Stratholme / Uldaman / Drak'Tharon / Mana-Tombs 2026-10-25 - the 4 s retarget
                // flipped between the mobs of one refused pack (or the targets behind one blocker) every 4-20 s, each
                // flip reset this clock, the 45 s tank view never came and the group stood 3-4 minutes. The same
                // stop keeps the clock; a boss and a body pull keep their own.
                bool const sameStop = ROUTE_STOP_KEEPS_WAIT && routeState.stopSinceMs && !routeState.targetIsBoss &&
                                      !routeState.approach &&

                                      leader->GetExactDist(&routeState.stopPos) <= STOP_SAME_RANGE;
                routeState.arrivedMs = sameStop ? routeState.stopSinceMs : now;
                routeState.stopSinceMs = routeState.arrivedMs;
                routeState.stopPos = leader->GetPosition();
            }
            routeState.stopLastMs = now;

            uint32 waited = getMSTimeDiff(routeState.arrivedMs, now);  // [held_for_now_cap] may be set waited out
            // An event's waiting place (EventSpotDef::hold) while its event runs: what stands there attacks the event
            // (Black Morass wave 10: a rift pack of 8 was "careful pull refused" for 3 minutes next to Medivh, broke
            // his shield, the event failed and everything despawned). No deferral there, and a short wait only.
            bool atEventHold = false;
            if (ROUTE_HOLD_SPOT_FIGHT)
                if (int32 const spotIdx = EventSpotIndex(group); spotIdx >= 0)
                    if (EventSpotDef const* spot = EventSpots(group.MapId())[size_t(spotIdx)];
                        SpotHolds(group, *spot))
                        // The event's own objective (a rift keeper at its rift) is fought wherever it stands.
                        atEventHold = object->GetExactDist(spot->x, spot->y, spot->z) <=
                                          (spot->holdRadius > 0.0f ? spot->holdRadius : HOLD_TARGET_RADIUS) ||
                                      std::find(spot->holdAlways.begin(), spot->holdAlways.end(),
                                                object->GetEntry()) != spot->holdAlways.end();
            uint32 const pullWaitMs = atEventHold ? BODY_PULL_WAIT_MS : PULL_WAIT_MS;
            bool const holdForce = false;  // [lead131] no ready-hold release by time (pull_after_hold gone)
            // [lead24] LEADER_NO_FORCING_TIMERS: no ready-hold release (pull_after_hold off), no timed-out body pull
            // [lead131] user: for every group, not only a led one (PULL_AFTER_HOLD_MS / _GATE_MS, the stop's
            // PULL_WAIT_MS / BODY_PULL_WAIT_MS walk-in, the unknown-verdict GUARANTEE_UNKNOWN_HOLD_MS were the clocks)
            bool const ledNoForce = Leader::LEADER_NO_FORCING_TIMERS;
            // ROUTE_BOSS_PULL_IMMEDIATE: a boss is body-pulled BOSS_PULL_READY_MS after the group stands ready for it
            // (GroupFullyReady, its boss buffs done, the rest hold above passed), not after PULL_WAIT_MS. The tank's
            // careful planner never picks a boss (RFC 20:10, Oggleflint: "[pull hold] no aggressive target" until
            // the 45 s ran out). The long wait stays while the tank holds its pull for a known reason the harness
            // can read: the group's pre-pull crowd control (PrePullCc::IsHolding). Not in raids (encounter set-ups).
            // A stop a neighbour pack made (approachBlocker: not the boss's own pack) is no body-pull spot: the body
            // pull would walk the whole route through it (wave 7 scholo-h 01:30:28, mt-h 01:44:09, mt-n 01:44:21).
            Creature* const neighbour =
                routeState.approachBlocker.IsEmpty() ? nullptr : map->GetCreature(routeState.approachBlocker);
            bool neighbourStop = ROUTE_HARNESS_WALK_GUARD && routeState.targetIsBoss && neighbour &&
                                 neighbour->IsAlive() && !neighbour->IsInCombat() && !neighbour->IsDungeonBoss() &&
                                 !routeState.bossCreatures.count(neighbour->GetGUID()) &&
                                 group.BossOrder(neighbour) < 0;
            // [hleft-boss-own-pack] the boss's encounter mob is no neighbour (the planner pulls it as his fight).
            if (neighbourStop)
                if (Creature* bossTarget = object->ToCreature(); bossTarget && BossEncounterMob(leader, bossTarget,
                                                                                               neighbour))
                    neighbourStop = false;
            if (neighbourStop && neighbour->GetFormation())
                for (ObjectGuid const& bossGuid : routeState.bossCreatures)
                    if (Creature* boss = map->GetCreature(bossGuid);
                        boss && boss->IsAlive() && boss->GetFormation() == neighbour->GetFormation())
                        neighbourStop = false;
            bool bossPullNow = false;
            if (ROUTE_BOSS_PULL_IMMEDIATE && routeState.targetIsBoss && !routeState.approach && !group.IsRaid() &&
                !neighbourStop)
            {
                // [lead13] arc-n / arc-h b56 (Zereketh: "held for now: Rtpalbe at 70 % mana" for good): a member
                // above its own rest line does not drink, so the 80 % lines alone never let the boss pull come. In
                // leader mode the boss is pulled ready by the old gate, or - the RaidLeader's old [ready_hold_30s]
                // rule - after PULL_HOLD_READY_MAX_MS with the leader ready and every member above its own lines.
                bool levelsReady = group.GroupFullyReady(leader);
                if (!levelsReady && group.LeaderDecides() && group.State().ready)
                {
                    bool atLines = true;
                    for (LeaderGroup::MemberInfo const& member : group.Members())
                        if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                            bot && bot->IsAlive() && GET_PLAYERBOT_AI(bot) &&
                            (RestState::WantsDrink(bot) || RestState::WantsFood(bot) || RestState::IsDrinking(bot) ||
                             RestState::IsEating(bot)))
                            atLines = false;
                    if (!atLines)
                        group.State().atLinesSinceMs = 0;
                    else if (!group.State().atLinesSinceMs)
                        group.State().atLinesSinceMs = now ? now : 1;
                    // [lead131] every member at its lines with the leader ready is the fact (PULL_HOLD_READY_MAX_MS
                    // 30 s at them was the old clock)
                    levelsReady = group.State().atLinesSinceMs != 0;
                    if (levelsReady)
                        LOG_INFO("playerbots",
                                 "[lead13] boss_ready_at_lines {}: {} - every member above its own "
                                 "rest lines {} s, the leader ready: the boss pull",
                                 group.Run().runId, object->GetName(),
                                 getMSTimeDiff(group.State().atLinesSinceMs, now) / IN_MILLISECONDS);
                }
                bool const ready = levelsReady && !group.NeedBuff() &&
                                   (routeState.bossBuffed.count(routeState.target) || distance >= BOSS_BUFF_DISTANCE) &&
                                   !PrePullCc::IsHolding(leader);
                BossReady& boss = bossReady[group.LeaderGuid()];
                if (boss.target != routeState.target || !ready)
                {
                    boss.target = routeState.target;
                    boss.sinceMs = ready ? now : 0;
                }
                else if (!boss.sinceMs)
                    boss.sinceMs = now;
                // [lead131] the group ready is the fact (BOSS_PULL_READY_MS 3 s ready and the PULL_WAIT_MS window
                // were the old clocks)
                bossPullNow = ready && boss.sinceMs;
                if (bossPullNow)
                    LOG_INFO("playerbots", "[approach] {}: boss pull: immediate (group ready) - {} {:.1f} yd, "
                             "waited {} s", leader->GetName(), object->GetName(), distance, waited / IN_MILLISECONDS);
            }
            // [route_blocker_body_pull] the target is a pack on the route's way: the planner and the guarantee get
            // ROUTE_BLOCKER_PREP_MS, then the ready group walks in (no skip).
            bool const routeBlocker = IsRouteBlocker(group, routeState.target);
            // The planner's own refusal of this pack (or of the neighbour that stopped the walk), when fresh.
            bool refusalHopeless = false;
            uint32 refusalWaitMs = 0;
            std::string refusalWhy;
            // [lead43] no guarantee hold for a led party: the leader's refusal moved the route off the mob (Retarget)
            bool const plannerRefused =
                ROUTE_CAREFUL_GUARANTEE_HOLD && !group.LeaderDecides() && !routeState.approach &&
                !routeState.targetIsBoss && !atEventHold && !group.IsRaid() && group.ContentSize() <= 5 &&
                (PartyRefusal(group, routeState.target, refusalHopeless, refusalWaitMs, refusalWhy) ||
                 (!routeState.approachBlocker.IsEmpty() &&
                  PartyRefusal(group, routeState.approachBlocker, refusalHopeless, refusalWaitMs, refusalWhy)));
            // "held for now" (the planner's honest pause: the group rests / tops up, a boss-ready hold): the group
            // waits and the wait does not count - the arrival clock restarts, no guarantee skip timer runs.
            // [lead131] no cap by time (HELD_FOR_NOW_MAX_MS 75 s fully ready was the old clock): the planner's own
            // verdict ends a "held for now".
            bool heldForNowCapped = false;
            // [lead3] LEADER_NO_BODY_FALLBACK: the leader never turns its planner's hold into a walk-in; a hold no rest
            // can change is its planner_no_option report (TickLeaderPlanner, LEADER_HELD_READY_MAX_MS)
            bool const leaderPulls = Leader::Leads(Leader::LEADER_NO_BODY_FALLBACK) && group.LeaderDecides();
            // [held_for_now_cap] a route blocker keeps its clock: its walk-in comes after ROUTE_BLOCKER_PREP_MS
            // whatever the planner holds (nex-h wave 23: the Horde Ranger route blocker held "fallback plan costs
            // 0.92" from 593 s to the stall, the walk-in never came)
            if (leaderPulls)
                heldForNowCapped = false;
            // [lead132] the planner had its chance when it answered (not ROUTE_BLOCKER_PREP_MS 30 s): a route blocker
            // it holds for now is no hold
            else if (routeBlocker && plannerRefused)
                heldForNowCapped = true;
            if (!heldForNowCapped && plannerRefused && !refusalHopeless && refusalWhy.rfind("held for now", 0) == 0)
            {
                // [patrol_wait_spot] a patrol wait: the group waits out of every idle pack's reach (the patrol fight
                // happens where the group stands).
                if (!LeaderOwnsPatrols(group) &&  // [lead4] the leader's patrol decision owns the wait's spot
                    refusalWhy.find("patrol wait") != std::string::npos &&
                    (routeState.patrolWaitTarget != routeState.target || !routeState.patrolWaitMs))
                {
                    routeState.patrolWaitMs = now ? now : 1;
                    routeState.patrolWaitWhy = refusalWhy;
                    routeState.patrolWaitTarget = routeState.target;
                    G3D::Vector3 spot;
                    Creature* nearMob = nullptr;
                    if (PatrolWaitSpot(leader, spot, nearMob))
                    {
                        routeState.patrolWaitUntilMs = now ? now : 1;  // on: while PatrolWaitOn
                        group.SetLeaderWaypoint(leader, spot.x, spot.y, spot.z);
                        group.LogEvent("patrol_wait_spot",
                                 JsonObject().Str("why", refusalWhy).Str("idleMob", nearMob->GetName())
                                     .Num("idleEntry", nearMob->GetEntry())
                                     .Real("idleDistance", leader->GetExactDist(nearMob)).Real("x", spot.x)
                                     .Real("y", spot.y).Real("z", spot.z));
                    }
                    else if (nearMob)
                        group.LogEvent("patrol_wait_spot",
                                 JsonObject().Str("why", refusalWhy).Str("idleMob", nearMob->GetName())
                                     .Num("idleEntry", nearMob->GetEntry()).Str("state", "no clear spot"));
                }
                if (!routeState.guaranteeHolding || routeState.guaranteeTarget != routeState.target)
                {
                    routeState.guaranteeTarget = routeState.target;
                    routeState.guaranteeDueMs = 0;
                    routeState.guaranteeHolding = true;
                    group.LogEvent("guarantee_hold", JsonObject().Str("name", object->GetName())
                                                          .Num("entry", object->GetEntry()).Num("waitSec", 0)
                                                          .Num("waitedSec", waited / IN_MILLISECONDS)
                                                          .Str("why", refusalWhy));
                }
                if (!routeBlocker)
                {
                    routeState.arrivedMs = now;
                    routeState.stopSinceMs = now;
                }
                return;
            }
            if (plannerRefused && !refusalHopeless)
            {
                if (routeState.guaranteeTarget != routeState.target)
                {
                    routeState.guaranteeTarget = routeState.target;
                    routeState.guaranteeDueMs = 0;
                    routeState.guaranteeHolding = false;
                }
                if (!refusalWaitMs && !routeState.guaranteeDueMs)
                    routeState.guaranteeDueMs = now;
                // [lead131] user: no elapsed-time rule decides (GUARANTEE_HOLD_TOTAL_MS 150 s, GUARANTEE_EXECUTE_MS
                // 30 s, GUARANTEE_UNKNOWN_HOLD_MS 120 s were the old clocks): the group holds while the planner
                // refuses the pack and it is not hopeless - the planner's own verdict (it takes the pack, or calls
                // it hopeless) ends the hold; a route blocker is no hold (lead132)
                bool const hold = !holdForce && !routeBlocker;
                if (hold)
                {
                    if (!routeState.guaranteeHolding)
                    {
                        routeState.guaranteeHolding = true;
                        group.LogEvent("guarantee_hold", JsonObject().Str("name", object->GetName())
                                                              .Num("entry", object->GetEntry())
                                                              .Num("waitSec", refusalWaitMs / IN_MILLISECONDS)
                                                              .Num("waitedSec", waited / IN_MILLISECONDS)
                                                              .Str("why", refusalWhy));
                        LOG_INFO("playerbots", "[approach] {}: holding at {} for the planner's guarantee ({} s, {})",
                                 leader->GetName(), object->GetName(), refusalWaitMs / IN_MILLISECONDS, refusalWhy);
                    }
                    return;
                }
            }
            // [hleft-never-pulled] no body pull of an encounter creature no tank pulls (AggressiveTargetValue's
            // list): the Halls of Reflection Lich King, Hadronox before she webbed the doors.
            if (Creature const* held = object->ToCreature();
                held && (held->GetEntry() == HOR_LICH_KING_ENTRY || held->GetEntry() == HOR_LICH_KING_ESCAPE_ENTRY ||
                         (held->GetEntry() == AN_HADRONOX_ENTRY && GET_PLAYERBOT_AI(leader) &&
                          ANAdds::HadronoxHandsOff(GET_PLAYERBOT_AI(leader)))))
            {
                routeState.arrivedMs = now;
                routeState.stopSinceMs = now;
                return;
            }
            // [lead3] a boss pull the leader issued that brought no fight: "not pulled", as the old walk-in's was
            // [lead131] user: its order's facts, not BOSS_PULL_HOLD_WAIT_MS 150 s: the tank opened the boss pull's
            // order and it closed without a fight (the boss and the group out of combat)
            bool bossOrderOpen = false;
            if (leaderPulls && routeState.targetIsBoss && group.State().bossPullIssued == routeState.target)
            {
                Leader::PullOrder bossOrder;
                bossOrderOpen = Leader::Orders::GetOpen(group.LeaderGuid(), bossOrder) &&
                                bossOrder.target == routeState.target;
                if (bossOrderOpen)
                    group.State().bossPullOrderSeen = true;
            }
            Creature const* pulledBoss = object->ToCreature();
            if (leaderPulls && routeState.targetIsBoss && group.State().bossPullIssued == routeState.target &&
                group.State().bossPullIssuedMs && group.State().bossPullOrderSeen && !bossOrderOpen &&
                !leader->IsInCombat() && !group.InCombat() && pulledBoss && !pulledBoss->IsInCombat())
            {
                group.State().bossPullOrderSeen = false;
                group.State().bossPullIssued.Clear();
                group.State().bossPullIssuedMs = 0;
                group.SkipTarget(routeState.target,
                                 ROUTE_OCULUS_DRAKE_ONLY ? BOSS_NOT_PULLED_SKIP_MS : NOT_PULLED_SKIP_MS,
                                 "not pulled (the leader's boss pull brought no fight)");
                return;
            }
            // [lead6] the leader's one readiness verdict, not the body pull's own 80 % lines
            bool const pullReady = leaderPulls && Leader::Leads(Leader::LEADER_ONE_READINESS)
                                       ? group.State().ready
                                       : group.GroupFullyReady(leader);
            // [lead24] LEADER_NO_FORCING_TIMERS: a led party's stop resolves on evaluations only (a boss pull now, a
            // hopeless refusal); the stop's wait / the route blocker's prep time only log waited_long
            bool const timedOut = waited > pullWaitMs || holdForce;  // the waited_long diagnostic's line
            if (ledNoForce && timedOut && !bossPullNow && !(plannerRefused && refusalHopeless))
                Leader::Dungeon::WaitedLong(group, "stop_wait", waited,
                                            object->GetName() + (plannerRefused ? ": " + refusalWhy : ""));
            if (!routeState.approach &&
                ((!ledNoForce && timedOut) || bossPullNow || (plannerRefused && refusalHopeless)) &&
                pullReady)
            {
                // [b33_mech2] BossAreaBodyPullHold: no body pull of a boss while killable trash of his area stands
                // (BossAreaTrashOf: what joins his fight, his room, BOSS_AREA_EXTRA) - it becomes the target first
                // (unskipped, kept as boss_neighbour_target is). Honest exceptions are left: fast respawners
                // (FastRespawner), a reachability skip (ReachabilitySkip: unreachable, closed, respawn, "boss area:
                // given up"), what the leader cannot attack; and after BOSS_AREA_BODY_PULL_HOLD_MS from the first
                // hold the pull goes anyway. mech-n 20261026-000434-mech-n-5D-17: Sepethrea body-pulled (immediate,
                // 1180 s) with the listed Tempest-Forge Destroyer 138862 alive south of her room; it killed the tank.
                if (ROUTE_BOSS_AREA_CLEAR && routeState.targetIsBoss)
                    if (Creature* areaBoss = object->ToCreature(); areaBoss && areaBoss->IsAlive())
                    {
                        Creature* areaMob = nullptr;
                        char const* areaWhy = nullptr;
                        uint32 areaLeft = 0;
                        for (auto const& [spawnId, mob] : map->GetCreatureBySpawnIdStore())
                        {
                            if (!mob || !mob->IsInWorld() || !mob->IsAlive() || mob->IsInCombat() || mob == areaBoss ||
                                mob->GetExactDist(areaBoss) > BOSS_AREA_SCAN_RANGE || FastRespawner(mob) ||
                                mob->IsImmuneToPC() || mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
                                mob->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) || !leader->IsValidAttackTarget(mob) ||
                                !InThisWing(group, mob))
                                continue;
                            char const* const why = BossAreaTrashOf(
                                leader, areaBoss, mob, routeState.bossCreatures.count(mob->GetGUID()) != 0);
                            if (!why)
                                continue;
                            if (routeState.skipUntil.count(mob->GetGUID()))
                                if (auto const reason = routeState.skipReason.find(mob->GetGUID());
                                    reason != routeState.skipReason.end() && ReachabilitySkip(reason->second))
                                    continue;
                            ++areaLeft;
                            if (!areaMob || leader->GetExactDist(mob) < leader->GetExactDist(areaMob))
                            {
                                areaMob = mob;
                                areaWhy = why;
                            }
                        }
                        std::pair<uint32, ObjectGuid> const holdKey{group.Run().runSeq, areaBoss->GetGUID()};
                        if (!areaMob)
                            bossAreaBodyPullHoldMs.erase(holdKey);
                        else
                        {
                            uint32& firstHoldMs = bossAreaBodyPullHoldMs[holdKey];
                            if (!firstHoldMs)
                                firstHoldMs = now ? now : 1;
                            // [lead135] held while killable trash of his area stands (BOSS_AREA_BODY_PULL_HOLD_MS 4 min
                            // was the old clock); the trash is the target
                            uint32 const heldMs = getMSTimeDiff(firstHoldMs, now);  // for the log
                            {
                                group.LogEvent("boss_area_clear",
                                         JsonObject().Str("boss", areaBoss->GetName())
                                             .Num("bossEntry", areaBoss->GetEntry()).Str("action", "body pull held")
                                             .Str("mob", areaMob->GetName()).Num("entry", areaMob->GetEntry())
                                             .Num("spawnId", uint32(areaMob->GetSpawnId())).Str("why", areaWhy)
                                             .Num("remaining", areaLeft).Num("heldSec", heldMs / IN_MILLISECONDS)
                                             .Bool("immediate", bossPullNow).Real("x", areaMob->GetPositionX())
                                             .Real("y", areaMob->GetPositionY()).Real("z", areaMob->GetPositionZ()));
                                routeState.skipUntil.erase(areaMob->GetGUID());
                                routeState.skipReason.erase(areaMob->GetGUID());
                                routeState.neighbourTarget = areaMob->GetGUID();
                                routeState.target = areaMob->GetGUID();
                                routeState.targetIsBoss = false;
                                routeState.targetIsStarter = false;
                                routeState.targetRouteRule = areaWhy;
                                routeState.targetSinceMs = now;
                                routeState.guaranteeHolding = false;
                                routeState.arrivedMs = 0;
                                routeState.approach = 0;
                                routeState.approachBlocker.Clear();
                                routeState.stopSinceMs = 0;
                                group.ClearWaypoint(group.LeaderGuid());
                                return;
                            }
                        }
                    }
                // What the tank (the leader) sees of the target and of the mob that stopped the walk: the refusal
                // below was assumed from the wait alone (wave 13: SM Armory's Guardsman was "careful pull refused"
                // while it was not even in the tank's possible targets).
                std::string tankView;
                bool targetUnseen = false;
                if (Unit* unit = object->ToUnit())
                {
                    std::string const targetView = PartyTankView(group, leader, unit);
                    // [b36_scholo] "accepted (not in the cached possible targets)" is seen: in sight and accepted,
                    // only the tank's cached list is stale. scholo-n 20261026-034959-scholo-n-5H-4: the entrance
                    // Adepts at 22 yd were skipped as "tank does not see it" and their pack marked unseen.
                    bool const accepted = targetView.rfind("accepted", 0) == 0;
                    targetUnseen = !accepted;
                    Creature* stopper = map->GetCreature(routeState.approachBlocker);
                    std::string const blockerView =
                        stopper ? PartyTankView(group, leader, stopper) : std::string();
                    group.LogEvent("tank_view", JsonObject().Str("name", unit->GetName())
                                                     .Num("entry", unit->GetEntry())
                                                     .Real("distance", leader->GetExactDist(unit))
                                                     .Str("target", targetView)
                                                     .Bool("nonAttackable", unit->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE))
                                                     .Bool("notSelectable", unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
                                                     .Bool("immuneToPc", unit->IsImmuneToPC())
                                                     .Bool("validTarget", leader->IsValidAttackTarget(unit))
                                                     .Str("blocker", stopper ? stopper->GetName() : "")
                                                     .Real("blockerDistance",
                                                           stopper ? leader->GetExactDist(stopper) : 0.0f)
                                                     .Str("blockerView", blockerView));
                    LOG_INFO("playerbots", "[tank view] {}: {} {:.1f} yd - {}{}{} (flags{}{}{}, {})",
                             leader->GetName(),
                             unit->GetName(), leader->GetExactDist(unit), targetView, stopper ? "; blocker " : "",
                             stopper ? stopper->GetName() + " - " + blockerView : std::string(),
                             unit->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ? " non-attackable" : "",
                             unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) ? " not-selectable" : "",
                             unit->IsImmuneToPC() ? " immune-to-pc" : "",
                             leader->IsValidAttackTarget(unit) ? "valid target" : "no valid target");
                    if (!accepted && (!stopper || blockerView.rfind("accepted", 0) != 0))  // [b36_scholo]
                        tankView = targetView;
                }
                // The boss's walk stopped at a neighbour: no body pull through it. What of its pack the tank does not
                // see is marked unseen (the next walk passes it, as the trash refusal below does); a pack it sees
                // holds the group here - the planner pulls it, or the hold shows its refusal (user rule).
                // zf-h build 17: Gahz'rilla's walk stopped at a Sandfury Blood Drinker the planner never pulled, the
                // hold came every 45 s (3 times) until the stall skip. After NEIGHBOUR_HOLD_MAX holds a small
                // neighbour pack (NEIGHBOUR_PASS_PACK_MAX) no longer stops the boss's body pull; a bigger one stays.
                std::list<Creature*> neighbourPack;
                if (neighbourStop)
                {
                    PackMobCheck check{leader, neighbour};
                    Acore::CreatureListSearcher<PackMobCheck> searcher(neighbour, neighbourPack, check);
                    Cell::VisitObjects(neighbour, searcher, PACK_RADIUS);
                    if (std::find(neighbourPack.begin(), neighbourPack.end(), neighbour) == neighbourPack.end())
                        neighbourPack.push_back(neighbour);
                    // [route_blocker_boss_neighbour] a neighbour pack on the boss's way is the target now (pulled by
                    // sight, else walked into once the group is ready), not after NEIGHBOUR_HOLD_MAX holds. sv-n
                    // wave 23 (112011, 1097-1172 s): the walk to Warlord Kalithresh stopped at a Coilfang Siren the
                    // tank accepted 21.7 yd off, nothing made it the target until the stop.
                    if (TargetRouteBlocker(group, leader, neighbour, object, "stopped the walk to the boss"))
                        return;
                    if (routeState.neighbourHoldBoss != routeState.target)
                    {
                        routeState.neighbourHoldBoss = routeState.target;
                        routeState.neighbourHolds = 0;
                    }
                    if (routeState.neighbourHolds >= NEIGHBOUR_HOLD_MAX &&
                        neighbourPack.size() <= NEIGHBOUR_PASS_PACK_MAX)
                    {
                        group.LogEvent("boss_neighbour_pass", JsonObject().Str("boss", object->GetName())
                                                                   .Str("neighbour", neighbour->GetName())
                                                                   .Num("packMobs", uint32(neighbourPack.size()))
                                                                   .Num("holds", routeState.neighbourHolds));
                        neighbourStop = false;
                    }
                    else if (routeState.neighbourHolds >= NEIGHBOUR_HOLD_MAX &&
                             !routeState.skipUntil.count(neighbour->GetGUID()))
                    {
                        // A bigger neighbour pack (dme-h build 17: Zevrim held 119 s by the Wildspawn pack): it
                        // becomes the route target, the careful pull / guarantee hold decides on it, hopeless or
                        // refused it is skipped as any trash; the boss comes back after it.
                        group.LogEvent("boss_neighbour_target", JsonObject().Str("boss", object->GetName())
                                                                     .Str("neighbour", neighbour->GetName())
                                                                     .Num("entry", neighbour->GetEntry())
                                                                     .Num("packMobs", uint32(neighbourPack.size()))
                                                                     .Num("holds", routeState.neighbourHolds));
                        routeState.neighbourHolds = 0;
                        routeState.neighbourTarget = neighbour->GetGUID();
                        routeState.target = neighbour->GetGUID();
                        routeState.targetIsBoss = false;
                        routeState.targetIsStarter = false;
                        routeState.targetRouteRule.clear();
                        routeState.targetSinceMs = now;
                        routeState.guaranteeHolding = false;
                        routeState.arrivedMs = 0;
                        routeState.approach = 0;
                        routeState.approachBlocker.Clear();
                        routeState.stopSinceMs = 0;
                        return;
                    }
                    else
                        ++routeState.neighbourHolds;
                }
                if (neighbourStop)
                {
                    std::list<Creature*>& pack = neighbourPack;
                    G3D::Vector3 const tankAt(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
                    for (Creature* packMob : pack)
                        if (!packMob->IsDungeonBoss() && !routeState.bossCreatures.count(packMob->GetGUID()) &&
                            (std::fabs(packMob->GetPositionZ() - tankAt.z) > APPROACH_OTHER_FLOOR_DZ ||
                             !SeesFromPoint(leader, packMob, tankAt)))
                            TankRejects::MarkUnseen(leader->GetGUID(), packMob->GetGUID());
                    LogHarnessHold(group, leader, neighbour, "boss approach", tankAt.x, tankAt.y, tankAt.z);
                    routeState.approachBlocker.Clear();
                    routeState.stopSinceMs = 0;
                    routeState.arrivedMs = 0;
                    return;
                }
                // Careful pulls (5-man, ChooseTargetActions.cpp; heroics and level 80 too since the careful-pull pass):
                // the tank refused this pack as too big for the group, a body pull would bring all of it. The route
                // goes on elsewhere and comes back later, when its neighbours are dead or the bot finds an edge mob to
                // pull. Levelling normals always defer; heroic / level 80 at most twice per pack, the third time the
                // group body-pulls as before, so the route never loops on a pack that cannot be split. Bosses and
                // raids keep the body pull.
                bool const levelling = group.Levelling();
                // A boss's own formation (Blood Furnace: the Shadowmoon Channelers round Keli'dan) is his encounter:
                // it never splits, and the tank's boss-readiness hold kept all five "careful pull refused" for 3
                // minutes (2026-10-25, a regression of the planner's boss gate). It is body-pulled as the boss is.
                bool bossFormation = false;
                if (Creature* refused = object->ToCreature(); refused && refused->GetFormation())
                    for (ObjectGuid const& bossGuid : routeState.bossCreatures)
                        if (Creature* boss = map->GetCreature(bossGuid);
                            boss && boss->IsAlive() && boss->GetFormation() == refused->GetFormation())
                            bossFormation = true;
                // [dmw-guard-body-pull] A pending guard of a closed guarded door (Dire Maul West's pylon guards, Sunken
                // Temple's defenders) is the door's objective: refused GUARD_REFUSALS_BEFORE_BODY_PULL times, it is
                // body-pulled as a boss formation is (dmw-n 062726: guarantee holds and "careful pull refused" on the
                // ledge guards until the stop, Immol'thar 0/1). User decision 2026-10-25: approved.
                bool guardBodyPull = false;
                if (Creature* refused = object->ToCreature(); refused && IsPendingDoorGuard(map, refused))
                {
                    uint32& refusalsOfGuard = guardRefusals[{group.Run().runSeq, routeState.target}];
                    guardBodyPull = ++refusalsOfGuard > GUARD_REFUSALS_BEFORE_BODY_PULL;
                    if (guardBodyPull)
                        group.LogEvent("guard_body_pull", JsonObject().Str("name", refused->GetName())
                                                               .Num("entry", refused->GetEntry())
                                                               .Num("refusals", refusalsOfGuard));
                }
                // [route_blocker_body_pull] the neighbour pack that stopped the walk to this target is on the way: it
                // becomes the target instead of being skipped with it; the route blocker itself is walked into.
                if (Creature* onWay = map->GetCreature(routeState.approachBlocker);
                    !routeBlocker && onWay && object->ToCreature() &&
                    onWay->GetExactDist(object) > PACK_RADIUS &&
                    TargetRouteBlocker(group, leader, onWay, object, "stopped the walk to a refused target"))
                    return;
                // [pull_after_hold] the ready group's hold at a pack of the required route ends in its pull
                if (holdForce && !routeBlocker && !routeState.targetIsBoss && object->ToCreature())
                    if (std::string requiredWhy; RouteBlockerBehindRequired(group, map, object, requiredWhy))
                    {
                        RouteBlockerBodyPull(group, leader, object->ToCreature(),
                                             "pull_after_hold (" + requiredWhy + ")");
                        return;
                    }
                // [lead3] LEADER_NO_BODY_FALLBACK: no careful-refused skip of a pack the tank sees. A hopeless pack the
                // way on does not need keeps the old skip (a route decision); any other is asked of the planner whole
                // (walked in when the leader must clear it), and its answer is the leader's (TickLeaderPlanner: the
                // plan, or planner_no_option: a stop for a pack it must clear, a skip for one it need not).
                if (leaderPulls && !routeState.targetIsBoss && !atEventHold && !bossFormation && !guardBodyPull &&
                    !routeBlocker && tankView.empty() && object->ToCreature())
                {
                    std::string needWhy;
                    bool const needs = group.NeedsPack(map, object, needWhy);
                    if (needs || !(plannerRefused && refusalHopeless))
                    {
                        Leader::Dungeon::LeaderWholePack(group, leader, object->ToCreature(),
                                        Acore::StringFormat("the stop's wait ({} s{}{})", waited / IN_MILLISECONDS,
                                                            plannerRefused ? ": " + refusalWhy : std::string(),
                                                            needs ? ", " + needWhy : std::string()),
                                        needs, needs);
                        return;
                    }
                }
                if (!group.IsRaid() && group.ContentSize() <= 5 && !routeState.targetIsBoss && !atEventHold &&
                    !bossFormation && !guardBodyPull && !routeBlocker &&
                    (levelling || ++routeState.carefulRefused[routeState.target] > 0))
                {
                    // User rule (bots act on their own): the harness never walks the group into a pack the tank
                    // refused; a pack that is never split is a bot gap and shows as a stalled run.
                    if (routeState.carefulRefused[routeState.target] == 3)
                        group.LogEvent("pack_too_big",
                                       JsonObject()
                                           .Num("entry", object->GetEntry())
                                           .Num("refusals", routeState.carefulRefused[routeState.target]));
                    uint32 const refusals = routeState.carefulRefused[routeState.target];
                    // [b43_harn] A target the planner never evaluated (no fresh verdict on its pack nor on the stop's
                    // neighbour) is no "careful pull refused": scholo-h 20261026-054314-scholo-h-5A-3 skipped the lone
                    // Spectral Researcher so while every refusal was of the nearer Neophyte chain, the planner's pick.
                    std::string neverPlanned;
                    {
                        bool verdictAccepted = false;
                        uint32 verdictMobs = 0;
                        bool const anyVerdict =
                            PartyVerdict(group, routeState.target, verdictAccepted, verdictMobs) ||
                            (!routeState.approachBlocker.IsEmpty() &&
                             PartyVerdict(group, routeState.approachBlocker, verdictAccepted,
                                                     verdictMobs));
                        if (!plannerRefused && !anyVerdict)
                        {
                            // The nearer pack the planner refused instead (its pick is the nearest candidate).
                            float const targetDist = leader->GetExactDist(object);
                            std::list<Creature*> nearer;
                            IdleAggroCheck nearerCheck{leader};
                            Acore::CreatureListSearcher<IdleAggroCheck> nearerSearcher(leader, nearer, nearerCheck);
                            Cell::VisitObjects(leader, nearerSearcher, targetDist);
                            Creature* refusedNearer = nullptr;
                            for (Creature* mob : nearer)
                            {
                                bool nearHopeless = false;
                                uint32 nearWaitMs = 0;
                                std::string nearWhy;
                                if (mob->GetGUID() != routeState.target && leader->GetExactDist(mob) < targetDist &&
                                    PartyRefusal(group, mob->GetGUID(), nearHopeless,
                                                                   nearWaitMs, nearWhy) &&
                                    (!refusedNearer ||
                                     leader->GetExactDist(mob) < leader->GetExactDist(refusedNearer)))
                                    refusedNearer = mob;
                            }
                            if (refusedNearer)
                                neverPlanned = Acore::StringFormat(
                                    "planner never planned it (nearer refused pack: {} {:.1f} yd)",
                                    refusedNearer->GetName(), leader->GetExactDist(refusedNearer));
                            else
                                neverPlanned = "planner never planned it (no verdict)";
                        }
                    }
                    std::string const packSkipReason =
                        neverPlanned.empty() ? std::string("careful pull refused (pack)")
                                             : std::string("planner never planned it (pack)");
                    // The tank does not take the target: a mob of its pack or of the blocker's that the tank cannot
                    // see from where it stands (no line of sight from its eyes) or that stands on another floor is
                    // marked as not seen (TankRejects TTL) and stops no later walk (RFC wave 37: the same unseen
                    // trogg stopped the next approach after every skip). A mob the tank sees on its floor is not
                    // marked: it still stops the walk, the tank pulls it first. Bosses are never marked.
                    // A pack approach to sight held the leader at the edge of (ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED) is
                    // unseen only because the leader kept out of its reach: it is not marked, it keeps stopping the
                    // walks (marked, the next route would walk through it).
                    std::vector<ObjectGuid> heldPack;
                    if (auto const hold = sightHolds.find(group.LeaderGuid());
                        hold != sightHolds.end() && hold->second.target == routeState.target)
                        heldPack = hold->second.pack;
                    auto const isHeld = [&heldPack](Creature* mob)
                    { return std::find(heldPack.begin(), heldPack.end(), mob->GetGUID()) != heldPack.end(); };
                    G3D::Vector3 const tankAt(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
                    auto const markIfUnseen =
                        [&group, &routeState, leader, &tankAt, targetUnseen, &isHeld](Creature* mob)
                    {
                        if (!targetUnseen || routeState.bossCreatures.count(mob->GetGUID()) || mob->IsDungeonBoss() ||
                            isHeld(mob))
                            return;
                        if (std::fabs(mob->GetPositionZ() - tankAt.z) > APPROACH_OTHER_FLOOR_DZ ||
                            !SeesFromPoint(leader, mob, tankAt))
                            TankRejects::MarkUnseen(leader->GetGUID(), mob->GetGUID());
                    };
                    // The whole pack, not only the mob the route chose: Shadow Labyrinth / Mechanar wave 9 refused a
                    // pack, then chose its next member, waited 45 s again, and the stall detector fired.
                    uint32 packSkipped = 0;
                    if (Creature* refused = object->ToCreature())
                    {
                        std::list<Creature*> pack;
                        PackMobCheck check{leader, refused};
                        Acore::CreatureListSearcher<PackMobCheck> searcher(refused, pack, check);
                        Cell::VisitObjects(refused, searcher, PACK_RADIUS);
                        if (std::find(pack.begin(), pack.end(), refused) == pack.end())
                            pack.push_back(refused);
                        for (Creature* mob : pack)
                        {
                            markIfUnseen(mob);
                            if (mob != refused && !routeState.bossCreatures.count(mob->GetGUID()) &&
                                !mob->IsDungeonBoss() && group.BossOrder(mob) < 0)
                            {
                                routeState.skipUntil[mob->GetGUID()] = now + CAREFUL_REFUSED_SKIP_MS;
                                routeState.skipReason[mob->GetGUID()] = packSkipReason;
                                routeState.unreachableFrom.erase(mob->GetGUID());
                                // The pack's count, whichever member the route picks next time.
                                uint32& count = routeState.carefulRefused[mob->GetGUID()];
                                count = std::max(count, refusals);
                                ++packSkipped;
                            }
                        }
                    }
                    // The neighbour pack that stopped the approach on the way (ApproachStop) is the one in front of
                    // the tank: it is skipped with the target, or the next route stops at it again.
                    if (Creature* blocker = map->GetCreature(routeState.approachBlocker);
                        blocker && blocker->IsAlive() && !blocker->IsInCombat() &&
                        !routeState.bossCreatures.count(blocker->GetGUID()) && !blocker->IsDungeonBoss() &&
                        group.BossOrder(blocker) < 0)
                    {
                        std::list<Creature*> pack;
                        PackMobCheck check{leader, blocker};
                        Acore::CreatureListSearcher<PackMobCheck> searcher(blocker, pack, check);
                        Cell::VisitObjects(blocker, searcher, PACK_RADIUS);
                        if (std::find(pack.begin(), pack.end(), blocker) == pack.end())
                            pack.push_back(blocker);
                        for (Creature* mob : pack)
                        {
                            // A blocker the tank does not see stops no walk any more (SM Library wave 32).
                            if (!tankView.empty() && !isHeld(mob))
                                TankRejects::MarkUnseen(leader->GetGUID(), mob->GetGUID());
                            else
                                markIfUnseen(mob);
                            if (!routeState.bossCreatures.count(mob->GetGUID()) && !mob->IsDungeonBoss() &&
                                group.BossOrder(mob) < 0 && !routeState.skipUntil.count(mob->GetGUID()))
                            {
                                routeState.skipUntil[mob->GetGUID()] = now + CAREFUL_REFUSED_SKIP_MS;
                                routeState.skipReason[mob->GetGUID()] = packSkipReason;
                                routeState.unreachableFrom.erase(mob->GetGUID());
                                ++packSkipped;
                            }
                        }
                    }
                    routeState.approachBlocker.Clear();
                    routeState.stopSinceMs = 0;  // the next pack at this stop gets its own wait
                    std::string reason = tankView.empty() ? std::string("careful pull refused")
                                                          : "not pulled, tank does not see it (" + tankView + ")";
                    if (tankView.empty() && !neverPlanned.empty())
                        reason = neverPlanned;  // [b43_harn]
                    if (plannerRefused && refusalHopeless)
                    {
                        reason = "careful pull hopeless (" + refusalWhy + ")";
                        // [b32_brs] the walks go round the hopeless pack while there is a way (HopelessDetourLeg)
                        NoteHopelessPack(leader, object->ToCreature());
                    }
                    else if (plannerRefused && routeState.guaranteeHolding)
                        reason = Acore::StringFormat("careful pull refused, guarantee not taken in {} s ({})",
                                                     waited / IN_MILLISECONDS, refusalWhy);
                    routeState.guaranteeHolding = false;
                    if (packSkipped)
                        reason += " (+" + std::to_string(packSkipped) + " pack)";
                    group.SkipTarget(routeState.target, CAREFUL_REFUSED_SKIP_MS, reason);
                    return;
                }

                if (routeBlocker)
                {
                    if (Creature* blockerPack = object->ToCreature())
                        RouteBlockerBodyPull(group, leader, blockerPack,
                                             plannerRefused ? "planner refused (" + refusalWhy + ")"
                                                            : std::string("not pulled from the stop"));
                    return;
                }
                Creature* pack = object->ToCreature();
                // [lead3] LEADER_NO_BODY_FALLBACK: the leader's pull of what the harness body-pulled - a boss, his
                // formation: the leader's BossPull order; a door guard, an event pack, a trash pack the stop's wait
                // ended at: asked of its planner whole, walked in (priced by its count). The boss-on-the-way rule
                // ([b51_dme]) still makes a boss the pack brings the next pull first.
                if (leaderPulls && pack)
                {
                    for (ObjectGuid const& guid : routeState.bossCreatures)
                    {
                        Creature* boss = map->GetCreature(guid);
                        if (!routeState.targetIsBoss && !pack->IsDungeonBoss() && boss && boss->IsAlive() &&
                            !boss->IsInCombat() && boss->GetGUID() != routeState.target &&
                            BossBroughtBy(leader, boss, pack) &&
                            !ClassGatedBoss(group.MapId(), group.Difficulty(), boss->GetEntry()))
                        {
                            group.LogEvent("boss_on_the_way_first",
                                     JsonObject().Str("first", boss->GetName()).Num("firstEntry", boss->GetEntry())
                                         .Real("firstDistance", leader->GetExactDist(boss))
                                         .Str("via", pack->GetName()).Num("viaEntry", pack->GetEntry())
                                         .Str("why", "[lead3] the pack on the way brings this boss: he is pulled "
                                                     "first"));
                            routeState.target = boss->GetGUID();
                            routeState.targetIsBoss = true;
                            routeState.targetIsStarter = false;
                            routeState.targetRouteRule = "boss on the way";
                            routeState.targetSinceMs = getMSTime();
                            routeState.arrivedMs = 0;
                            routeState.stopSinceMs = 0;
                            routeState.approachBlocker.Clear();
                            routeState.orderHoldSinceMs.erase(boss->GetGUID());
                            group.ClearWaypoint(group.LeaderGuid());
                            return;
                        }
                    }
                    if (routeState.targetIsBoss || pack->IsDungeonBoss() || bossFormation)
                        Leader::Dungeon::LeaderBossPull(group, leader, pack,
                                       Acore::StringFormat("{}{} (waited {} s)", bossPullNow ? "ready" : "the wait",
                                                           bossFormation ? ", his formation" : "",
                                                           waited / IN_MILLISECONDS));
                    else
                        Leader::Dungeon::LeaderWholePack(group, leader, pack,
                                        Acore::StringFormat("{} (waited {} s)",
                                                            guardBodyPull ? "a pending door guard"
                                                            : atEventHold ? "the event's pack"
                                                            : holdForce   ? "the ready group's hold"
                                                                          : "the stop's wait",
                                                            waited / IN_MILLISECONDS),
                                        true, true);
                    return;
                }
                // [body_pull_via_planner] a trash pack: the planner first
                if (BODY_PULL_VIA_PLANNER && pack && !routeState.targetIsBoss && !pack->IsDungeonBoss())
                {
                    bool askBegan = false;
                    if (AskPlannerFirst(group.LeaderGuid(), pack, now, askBegan))
                    {
                        if (askBegan)
                            group.LogEvent("body_pull_planner_request",
                                     JsonObject().Str("name", pack->GetName()).Num("entry", pack->GetEntry())
                                         .Str("why", "the stop's wait")
                                         .Str("until", "the planner's verdict"));
                        group.ClearWaypoint(group.LeaderGuid());
                        return;
                    }
                    group.LogEvent("body_pull_planner_timeout",
                             JsonObject().Str("name", pack->GetName()).Num("entry", pack->GetEntry()));
                }
                // [b33_bodypull] no harness walk-in (a boss, his formation, a door guard, an event's pack) against
                // the planner's readiness refusal (the healer not with the tank, sitters, not topped up), nor with a
                // member sitting: the group holds where it stands, nothing skipped
                {
                    std::string sitter;
                    for (LeaderGroup::MemberInfo const& member : group.Members())
                        if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                            bot && bot->IsAlive() && bot->IsSitState())
                            sitter = bot->GetName();
                    if ((plannerRefused && RefusalIsReadiness(refusalWhy)) || !sitter.empty())
                    {
                        if (!routeState.blockerGateLogMs ||
                            getMSTimeDiff(routeState.blockerGateLogMs, now) > 10 * IN_MILLISECONDS)
                        {
                            routeState.blockerGateLogMs = now ? now : 1;
                            group.LogEvent("blocker_pull_gate",
                                     JsonObject().Str("pull", "harness body pull")
                                         .Str("blocker", pack ? pack->GetName() : std::string("?"))
                                         .Num("blockerEntry", pack ? pack->GetEntry() : 0)
                                         .Str("why", sitter.empty() ? "held: the planner waits for the group (" +
                                                                          refusalWhy + ")"
                                                                    : "the group is not ready: " + sitter + " sits")
                                         .Bool("hold", true));
                        }
                        group.ClearWaypoint(group.LeaderGuid());
                        return;
                    }
                }
                // [b49_voone] the same rule as RouteBlockerBodyPull: no walk-in to a trash pack an idle boss's own
                // aggro reach covers (BossBroughtBy) while the planner did not price that pull
                if (pack && !routeState.targetIsBoss && !pack->IsDungeonBoss() &&
                    !(plannerRefused && refusalWhy.find("(a boss pull: no fallback plan)") != std::string::npos))
                {
                    std::list<Creature*> walkPack;
                    PackMobCheck walkCheck{leader, pack};
                    Acore::CreatureListSearcher<PackMobCheck> walkSearcher(pack, walkPack, walkCheck);
                    Cell::VisitObjects(pack, walkSearcher, PACK_RADIUS);
                    if (std::find(walkPack.begin(), walkPack.end(), pack) == walkPack.end())
                        walkPack.push_back(pack);
                    for (ObjectGuid const& guid : routeState.bossCreatures)
                    {
                        Creature* boss = leader->GetMap()->GetCreature(guid);
                        if (!boss || !boss->IsAlive() || boss->IsInCombat())
                            continue;
                        auto const brings = [&](Creature* mob) { return BossBroughtBy(leader, boss, mob); };
                        if (std::none_of(walkPack.begin(), walkPack.end(), brings))
                            continue;
                        LOG_INFO("playerbots", "[b49_voone] {}: no harness body pull of {}: it brings {}, the "
                                 "planner did not price it", leader->GetName(), pack->GetName(), boss->GetName());
                        group.LogEvent("blocker_pull_gate",
                                 JsonObject().Str("pull", "harness body pull").Str("blocker", pack->GetName())
                                     .Num("blockerEntry", pack->GetEntry())
                                     .Str("why", "[b49_voone] brings boss " + boss->GetName() + " (unpriced)")
                                     .Bool("hold", false));
                        // [b51_dme] User rule 2026-10-26 (open the way, no detours): a pack that is the way on and
                        // stands in an idle boss's reach is no skip - the boss is pulled deliberately first, as his own
                        // fight (the boss logic: his buffs, ready check and the planner's boss pull, which prices him
                        // with what he brings). dme-h 20261026-130307-5E-11 stalled 0/16: "body pull refused: brings
                        // boss Hydrospawn" x3, "behind skipped Wildspawn Hellcaller" x8. The Voone intent stays: no
                        // blind walk-in to the pack; a class-gated boss keeps the old skip.
                        if (!ClassGatedBoss(group.MapId(), group.Difficulty(), boss->GetEntry()) &&
                            boss->GetGUID() != routeState.target)
                        {
                            group.LogEvent("boss_on_the_way_first",
                                     JsonObject().Str("first", boss->GetName()).Num("firstEntry", boss->GetEntry())
                                         .Real("firstDistance", leader->GetExactDist(boss)).Str("via", pack->GetName())
                                         .Num("viaEntry", pack->GetEntry())
                                         .Str("why", "[b51_dme] the pack on the way brings this boss: he is "
                                                     "pulled first"));
                            LOG_INFO("playerbots", "[b51_dme] {}: {} brings boss {}: the boss is the next pull (his "
                                     "fight), no skip",
                                     leader->GetName(), pack->GetName(), boss->GetName());
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
                            group.ClearWaypoint(group.LeaderGuid());
                            return;
                        }
                        group.ClearWaypoint(group.LeaderGuid());
                        group.SkipTarget(routeState.target, CAREFUL_REFUSED_SKIP_MS,
                                   "body pull refused: brings boss " + boss->GetName());
                        return;
                    }
                }
                // [b32_bm] A creature that is not REACT_AGGRESSIVE (Aeonus and the Black Morass adds: REACT_DEFENSIVE)
                // never aggroes on a walk-in: the walk only brings the tank beside it, the tank's own attack opens
                // (tank open idle boss / pull / reach melee). Logged as attack_pull, not body_pull.
                bool const attackPull = pack && !pack->HasReactState(REACT_AGGRESSIVE);
                routeState.approach = 1;  // the pull logic held back: walk into the pack
                routeState.arrivedMs = 0;
                group.LogEvent(attackPull ? "attack_pull" : "body_pull",
                         JsonObject().Str("name", pack ? pack->GetName() : "?")
                             .Num("entry", pack ? pack->GetEntry() : 0)
                             .Bool("boss", routeState.targetIsBoss)
                             .Num("carefulRefusals", routeState.carefulRefused.count(routeState.target)
                                                         ? routeState.carefulRefused[routeState.target]
                                                         : 0)
                             .Num("waitedSec", waited / IN_MILLISECONDS)
                             .Bool("immediate", bossPullNow)
                             .Num("react", pack ? uint32(pack->GetReactState()) : 0));
            }
            // [b32_bm] beside a non-aggressive boss the tank's own attack needs its idle-boss delay and its tries.
            // [lead131] user: the facts, not BODY_PULL_WAIT_MS 10 s / BOSS_PULL_HOLD_WAIT_MS 150 s: the walk-in stands
            // in the tank's melee reach of its target, the target out of combat, the tank attacking nothing and not
            // holding its pull - the walk-in brought no fight
            else if (routeState.approach && object->ToUnit() && leader->IsWithinMeleeRange(object->ToUnit()) &&
                     !object->ToUnit()->IsInCombat() && !leader->GetVictim() && !LeaderPullHeld(leader))
                group.SkipTarget(
                    routeState.target,
                    routeState.targetIsBoss && ROUTE_OCULUS_DRAKE_ONLY ? BOSS_NOT_PULLED_SKIP_MS : NOT_PULLED_SKIP_MS,
                    "not pulled");
            return;
        }

        // The first leg of the route (the corner path when the smooth one is over its point limit, RoutePath).
        if (!legBuilt)
            SteerLeg(leader, goal, leg);
        if (leg.type & PATHFIND_NOPATH)
        {
            group.SkipTarget(routeState.target, UNREACHABLE_SKIP_MS, "path lost");
            return;
        }
        // The walk of this tick never leads through a part closed to this run (AreaGateDef: an LBRS group and the
        // UBRS hall); from inside one the way out is no crossing.
        if (!InAnyAreaGate(steerClosedAreas, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()) &&
            RouteCrossesClosedArea(leg.points, steerClosedAreas))
        {
            group.SkipTarget(routeState.target, UNREACHABLE_SKIP_MS, "route through a closed area");
            return;
        }

        // [incomplete_route_deadend] (user 2026-10-25: "if the path is incomplete, why walk it?") A partial leg is
        // walked only when its end is progress (JudgeIncompleteEnd): not below a cliff / in water under one, not an end
        // no nearer to the goal, not an end the leader stood at already without getting nearer since. Maraudon heroic
        // 20261025-163739-mara-h-5A-17 swam to the floor end under the Earth Song waterfall (124 / 140 yd short, 79 /
        // 83 yd below Princess Theradras), low_breath 39 %, "own path refused". A complete way (the long search, from
        // the water surface too) is no partial leg; without one the target is skipped as a "route end" from here
        // (unreachableFrom: checked again once the leader stands elsewhere), as steer_stuck does after the walk.
        ForgetPassedDeadEnds(leader, goal);
        // Never a stall (Underbog heroic build 30, 20261025-184229-ub-h-5F-63: 13 refusals, 235 s idle): the third
        // refusal within DEAD_END_FALLBACK_WINDOW_MS puts the leader back on the old walk for DEAD_END_FALLBACK_MS.
        bool fallbackStarted = false;
        if (ROUTE_REFUSE_DEAD_END && leg.partial && !approachStopped && !routeState.approach &&
            !routeState.targetIsStarter && leg.points.size() >= 2 && !leader->IsFlying() && !leader->GetVehicle() &&
            !DeadEndFallback(leader, routeState.target, false, fallbackStarted))
        {
            DeadEndVerdict const verdict = JudgeIncompleteEnd(leader, leg.end, goal, leg.type, routeState.target);
            if (!verdict.why.empty() && DeadEndFallback(leader, routeState.target, true, fallbackStarted))
            {
                group.LogEvent("incomplete_route_fallback",
                         JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                             .Str("why", verdict.why).Num("refusals", DEAD_END_FALLBACK_REFUSALS)
                             .Str("until", "the target changes")
                             .Real("endLeft", verdict.left).Real("x", leader->GetPositionX())
                             .Real("y", leader->GetPositionY()).Real("z", leader->GetPositionZ()));
            }
            else if (!verdict.why.empty())
            {
                LOG_INFO("playerbots",
                         "[route] {}: incomplete_route_refused: {} {} ({}): end ({:.1f},{:.1f},{:.1f}) {:.1f} yd "
                         "short (horizontal {:.1f}, dz {:.1f}, rise {:.1f}, under water {}), {:.1f} yd away now",
                         group.Run().runId, object->GetName(), object->GetEntry(), verdict.why, leg.end.x, leg.end.y,
                         leg.end.z, verdict.left, verdict.hgap, verdict.dz, verdict.rise, verdict.underwater,
                         verdict.distance);
                group.LogEvent("incomplete_route_refused",
                         JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                             .Bool("boss", routeState.targetIsBoss).Str("why", verdict.why).Num("pathType", leg.type)
                             .Bool("straightPath", leg.straightPath)
                             .Real("distance", verdict.distance).Real("endLeft", verdict.left)
                             .Real("horizontal", verdict.hgap).Real("dz", verdict.dz).Real("rise", verdict.rise)
                             .Bool("underwater", verdict.underwater).Real("surface", verdict.surface)
                             .Num("visitSec", verdict.visitSec).Real("endX", leg.end.x).Real("endY", leg.end.y)
                             .Real("endZ", leg.end.z).Real("x", leader->GetPositionX())
                             .Real("y", leader->GetPositionY()).Real("z", leader->GetPositionZ())
                             .Real("toX", goal.x).Real("toY", goal.y).Real("toZ", goal.z));
                if (routeState.targetIsBoss)
                    routeState.orderHoldSinceMs.emplace(routeState.target, now);  // as the steer_stuck "route end" does
                group.SkipTarget(routeState.target, UNREACHABLE_SKIP_MS, "route end: incomplete route refused (" +
                                                                         verdict.why + ")");
                return;
            }
        }
        // [incomplete_route_deadend] the long search ran from the water surface: logged once per target
        if (leg.fromSurface)
        {
            static std::set<std::pair<ObjectGuid, ObjectGuid>> surfaceLogged;  // world thread only
            if (surfaceLogged.insert({group.LeaderGuid(), routeState.target}).second)
                group.LogEvent("route_surface_start",
                         JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                             .Real("distance", distance).Real("x", leader->GetPositionX())
                             .Real("y", leader->GetPositionY()).Real("z", leader->GetPositionZ())
                             .Real("endX", leg.end.x).Real("endY", leg.end.y).Real("endZ", leg.end.z));
        }

        // A waypoint between two corners of a corner path / long route has an interpolated height: on stairs and
        // slopes it floats above or sinks below the floor, and the leader's own MoveTo refuses it (SearchForBestPath
        // finds no height: "test runner move - FAILED", the wave 9 freezes at Pit of Saron's entrance and others).
        G3D::Vector3 const waypoint = FloorWaypoint(
            leader, leg,
            approachStopped ? approachStop
                            : RouteWaypoint(leg, goal, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP));

        // The navmesh ends here and the target stands on another part of it. Ahn'kahet wave 9: the entrance hall and
        // the Swarmers' level above it are two parts (tools/navmesh_probe.py 619 505.4 -963.4 30.7 525.8 -963.4 37.9:
        // gap 20.8 yd, dz 7.8), the ramp between has collision but no polygons; the leader stood at the end of the
        // partial route, its own move refused the target and the run stalled in 5 minutes. A player walks up the
        // ramp: straight, over ground the whole way (DirectLineOverGround), to the aggro edge of the target.
        G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        bool const atRouteEnd = (leg.type & PATHFIND_INCOMPLETE) && (waypoint - here).length() <= STEER_PROGRESS_RANGE;
        G3D::Vector3 next = waypoint;
        // In water the walk over the gap is a swim (ROUTE_SWIM_UP): no floor needed under the line, the swimmer
        // rises to the target's edge (Slave Pens: the route to Rokmar ends on the lake floor below him).
        bool const swimming = leader->Unit::IsInWater();
        if (atRouteEnd && (!swimming || ROUTE_SWIM_UP) && distance <= GAP_CROSS_MAX_DISTANCE)
        {
            if (routeState.gapTarget != routeState.target)
            {
                routeState.gapTarget = routeState.target;
                routeState.gapTries = 0;
            }
            if (leader->isMoving() &&
                leader->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
                return;  // on the way over already

            Creature* mob = object->ToCreature();
            float const stop = routeState.targetIsStarter || !mob
                                   ? STARTER_RANGE - 1.0f
                                   : std::min(PULL_RANGE - 2.0f, mob->GetAggroRange(leader) + 2.0f);
            if (routeState.gapTries < GAP_CROSS_TRIES && distance > stop + 1.0f)
            {
                G3D::Vector3 dest = here + (goal - here) * ((distance - stop) / distance);
                float const ground = map->GetHeight(leader->GetPhaseMask(), dest.x, dest.y, dest.z + 2.0f, true, 10.0f);
                if (ground > INVALID_HEIGHT && (!swimming || ground > dest.z))
                    dest.z = ground;
                bool const lineOk = swimming ? ground > INVALID_HEIGHT || WaterSurfaceAt(leader, dest) > INVALID_HEIGHT
                                             : ground > INVALID_HEIGHT &&
                                                   DirectLineOverGround(leader, dest.x, dest.y, dest.z);
                if (lineOk && leader->IsWithinLOS(dest.x, dest.y, dest.z + 2.0f))
                {
                    ++routeState.gapTries;
                    // The aggro edge across the gap is the leader's waypoint: its own "test runner move" walks
                    // straight where its path ends (MovementAction::MoveStraight, over ground, in sight).
                    group.SetLeaderWaypoint(leader, dest.x, dest.y, dest.z);
                    group.NoteProgress(now);
                    group.LogEvent("gap_cross", JsonObject().Str("name", object->GetName())
                                                     .Num("entry", object->GetEntry()).Num("try", routeState.gapTries)
                                                     .Bool("swim", swimming).Bool("own", true)
                                                     .Real("distance", distance).Real("x", here.x).Real("y", here.y)
                                                     .Real("z", here.z).Real("toX", dest.x).Real("toY", dest.y)
                                                     .Real("toZ", dest.z));
                    return;
                }
            }
        }
        // No straight walk over the gap: the leader's own move gets the target itself, as before the corner-path pass
        // (the core's point movement walks a partial path and goes straight on where polygons are missing - the
        // Mechanar stairs to Sepethrea in waves 6-7; wave 9 stood at their foot, 235.3,-30.2, for 4 minutes). When
        // that move is refused too, the steering watchdog above ends it.
        if (atRouteEnd)
            next = goal;
        else
        {
            // The leader's own move makes no progress on a route that goes on: logged as the bot's gap (bots walk by
            // themselves). [lead2] the dead harness drive walk is gone.
            if (LeaderOwnWalkStalled(group, leader, goal))
                group.LogCannotMove(leader, "route", "own walk made no progress for 6 s on a route that goes on",
                              waypoint.x, waypoint.y, waypoint.z);
            // [b32_brs] Bots walk by themselves: once the leader's own walk stalled (routeState.driving), its waypoint is
            // the leg's next corner, DRIVE_STEP yd at most and over ground the whole way, instead of the point
            // WAYPOINT_STEP along the leg - the near corner the runner's own MoveStraight walks when its path is
            // refused. brs-h 20261025-221216-brs-h-5F-9 (2421-2539 s): Rtwarac at (26.1,-551.5,-18.6) was sent to
            // (27.9,-530.7) / (30.8,-525.7), 21-26 yd along the leg that goes east round a raised object (navmesh
            // islands z -15..-12 at x 22-28, y -546..-549; navmesh_probe: connected via 30.9,-552.8 / 33.4,-546.9);
            // its own path was refused and the straight line ran over the object: bot_cannot_move x13, 173 s idle.
            if (routeState.driving)
                for (size_t index = 1; index < leg.points.size(); ++index)
                {
                    G3D::Vector3 const ahead = leg.points[index] - here;
                    float const length = ahead.length();
                    if (length < 1.0f)
                        continue;
                    G3D::Vector3 step = here + ahead * (std::min(length, DRIVE_STEP) / length);
                    float const ground =
                        map->GetHeight(leader->GetPhaseMask(), step.x, step.y, step.z + 3.0f, true, 10.0f);
                    if (ground > INVALID_HEIGHT && std::fabs(ground - step.z) <= 3.0f &&
                        DirectLineOverGround(leader, step.x, step.y, ground) && (step - next).length() > 2.0f)
                    {
                        step.z = ground;
                        next = step;
                        static std::unordered_map<uint32, uint32> shortStepLogMs;  // party seq -> last log (world)
                        uint32 const logNow = getMSTime();
                        uint32& loggedMs = shortStepLogMs[group.Run().runSeq];
                        if (!loggedMs || getMSTimeDiff(loggedMs, logNow) >= 10 * IN_MILLISECONDS)
                        {
                            loggedMs = logNow ? logNow : 1;
                            group.LogEvent("stall_short_step",
                                     JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                                         .Real("x", here.x).Real("y", here.y).Real("z", here.z)
                                         .Real("toX", step.x).Real("toY", step.y).Real("toZ", step.z)
                                         .Real("was", (waypoint - here).length()));
                        }
                    }
                    break;
                }
        }

        group.SetLeaderWaypoint(leader, next.x, next.y, next.z);
    }
}
