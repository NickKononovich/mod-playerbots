/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "RoutePatrol.h"

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

namespace Leader::Travel
{
using namespace Leader::Fight;

    // Creature::MoveInLineOfSight aggroes within GetAggroRange + m_CombatDistance (3D, no object sizes).
    float AggroReach(Creature* mob, Player* leader)
    {
        float wander = 0.0f;
        if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
            wander = std::min(mob->GetWanderDistance(), APPROACH_WANDER_MAX);
        return std::max(mob->GetAggroRange(leader), mob->GetAttackDistance(leader)) + mob->m_CombatDistance +
               wander + APPROACH_AGGRO_MARGIN;
    }

    // The line of sight the tank's "possible targets" test (NearestUnitsValue: Player::IsWithinLOSInMap) and the
    // mob's aggro test (Creature::MoveInLineOfSight: the same line from the other end) would see with the leader
    // standing at the point: from its eyes (point + its collision height) to the mob's hit sphere. Wave 13: the old
    // check (mob->IsWithinLOS(point + 2 yd), which adds the MOB's collision height on top) cleared the crates and
    // railings a player's eye line hits - the leader stopped where the tank saw nothing (SM Armory: Scarlet
    // Guardsman 22.5 yd, possible targets empty; Arcatraz: Sentinel 24.7 yd), and the run stalled.
    bool SeesFromPoint(Player* leader, Creature* mob, G3D::Vector3 const& point)
    {
        float const eyeZ = point.z + leader->GetCollisionHeight();
        float mobX = 0.0f;
        float mobY = 0.0f;
        float mobZ = 0.0f;
        mob->GetHitSpherePointFor(Position(point.x, point.y, eyeZ), mobX, mobY, mobZ);
        return mob->GetMap()->isInLineOfSight(point.x, point.y, eyeZ, mobX, mobY, mobZ, leader->GetPhaseMask(),
                                              LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
    }

    // The core's proximity aggro of an idle mob on the leader standing at a route point, as every approach and
    // route test of the harness asks it (wave 44, RFC 5B-1 20:30:02: a Searing Blade Cultist at z -8.2 "blocks
    // route" 20.1 yd from the backbone while the group walked at z -22, 14 yd below it). Per point:
    // MovementAction::IdleAggroBlocker (Creature::CanStartAttack - the Z gate, then the 3D range), the range
    // being `reach` (the harness's own reach with its margin: RouteReach, AggroReach, ...), then the harness's
    // eye-line sight from the point (SeesFromPoint) when `checkLos`. zGated: the point lay inside the range and
    // only the Z gate kept the mob idle.
    bool AggroesAtPoint(Creature* mob, Player* leader, G3D::Vector3 const& point, float reach, bool checkLos,
                        bool* zGated)
    {
        float const margin = reach - MovementAction::IdleAggroReach(mob, leader, 0.0f);
        char const* const blocker =
            MovementAction::IdleAggroBlocker(mob, leader, point.x, point.y, point.z, margin, false);
        if (blocker)
        {
            if (zGated && strcmp(blocker, "z gate") == 0 && mob->GetExactDist(point.x, point.y, point.z) < reach)
                *zGated = true;
            return false;
        }
        return !checkLos || SeesFromPoint(leader, mob, point);
    }

    // The path a patrol walks: its own (creature_addon path, or the one its SmartAI started), else its formation
    // leader's, which it follows - the lookup of ChooseTargetActions' PatrolPath (FindPatrolRoute), uncached:
    // Retarget runs every few seconds on the world thread.
    WaypointPath const* PatrolWalkPath(Creature* creature)
    {
        for (uint8 hop = 0; creature && hop < 2; ++hop)
        {
            if (uint32 const pathId = creature->GetWaypointPath())
                if (WaypointPath const* path = sWaypointMgr->GetPath(pathId))
                    return path;
            if (SmartAI* ai = dynamic_cast<SmartAI*>(creature->AI()))
                if (uint32 const pathId = ai->GetScript()->GetPathId())
                {
                    if (WaypointPath const* path = sSmartWaypointMgr->GetPath(pathId))
                        return path;
                    if (WaypointPath const* path = sWaypointMgr->GetPath(pathId))
                        return path;
                }
            CreatureGroup const* formation = creature->GetFormation();
            Creature* formationLeader = formation ? formation->GetLeader() : nullptr;
            creature = formationLeader != creature ? formationLeader : nullptr;
        }
        return nullptr;
    }

    std::unordered_map<uint32, std::map<ObjectGuid, PatrolWaitBudget>> patrolWaitBudgets;

    std::unordered_map<uint32, ObjectGuid> patrolHandOvers;

    bool PatrolHandedOver(uint32 instanceId, ObjectGuid patrol, uint32 now)
    {
        auto const instance = patrolWaitBudgets.find(instanceId);
        if (instance == patrolWaitBudgets.end())
            return false;
        auto const budget = instance->second.find(patrol);
        return budget != instance->second.end() && budget->second.handedUntilMs;
    }

    // One tick of waiting for `patrol` (start: a hold or step-back begins). True when its budget is spent: the
    // patrol is handed over now (patrolHandOvers) or already was.
    bool ChargePatrolWait(uint32 instanceId, ObjectGuid patrol, uint32 now, bool start)
    {
        PatrolWaitBudget& budget = patrolWaitBudgets[instanceId][patrol];
        // [lead129] a patrol handed to the planner stays handed (PATROL_HANDOVER_MS 4 min was the old clock): the
        // planner pulls it, its death ends it
        if (budget.handedUntilMs)
            return true;
        // [lead130] user: no elapsed-time rule decides (PATROL_WAIT_BUDGET_MS 60 s / PATROL_WAIT_BUDGET_STARTS 4 were
        // the old clock and cap). A patrol that comes to the group or walks beside its stop is the fact that waiting
        // does not remove it (it walks the same way again): it goes to the tank's planner at once (lead41: patrols
        // are killed, never waited out)
        if (start && budget.starts < 255)
            ++budget.starts;
        budget.handedUntilMs = now ? now : 1;  // handed since
        patrolHandOvers[instanceId] = patrol;
        LOG_INFO("playerbots", "[b47_patrol] patrol_wait_budget: instance {}: patrol {} on the group's way: handed "
                 "to the tank's planner", instanceId, patrol.ToString());
        return true;
    }

    std::unordered_map<uint32, PatrolHold> patrolHolds;  // per instance (Steer, world thread)

    float PointSegmentDistance2d(G3D::Vector2 const& point, G3D::Vector2 const& from, G3D::Vector2 const& to)
    {
        G3D::Vector2 const segment = to - from;
        float const lengthSq = segment.squaredLength();
        float const along = lengthSq > 0.0f ? std::clamp((point - from).dot(segment) / lengthSq, 0.0f, 1.0f) : 0.0f;
        return (from + segment * along - point).length();
    }

    // [b48_scholo] PATROL_LANE_HOLD: the leader's walk from where it stands to `to`, sampled every
    // PATROL_LANE_SAMPLE yd, against every leg of the patrol's walk (PatrolWalkPath, closed, the legs within
    // PATROL_WAIT_TRAIL_FLOOR of the sample's height). walkGap: the walk's nearest 2D approach to it; nowGap: the
    // leader's own. True when the walk comes within `reach` of the walk, PATROL_LANE_DEEPER nearer than the leader
    // stands now, and the patrol on its walk sees that sample (its eyes to the leader's).
    bool WalkEntersPatrolLane(Player* leader, Creature* patrol, G3D::Vector3 const& to, float reach,
                              float& walkGap, float& nowGap)
    {
        walkGap = 1.0e9f;  // no leg on the walk's floor
        nowGap = 1.0e9f;
        WaypointPath const* path = PatrolWalkPath(patrol);
        if (!path || path->Nodes.size() < 2)
            return false;
        std::vector<WaypointNode> const& nodes = path->Nodes;
        G3D::Vector3 const from(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        float const length = (to - from).length();
        uint32 const steps = std::min<uint32>(64, uint32(length / PATROL_LANE_SAMPLE) + 1);
        G3D::Vector3 nearWalk = from;
        G3D::Vector3 nearLane = from;
        for (uint32 step = 0; step <= steps; ++step)
        {
            G3D::Vector3 const point = from + (to - from) * (float(step) / float(steps));
            G3D::Vector2 const flat(point.x, point.y);
            for (size_t index = 0; index < nodes.size(); ++index)
            {
                WaypointNode const& a = nodes[index];
                WaypointNode const& b = nodes[(index + 1) % nodes.size()];
                if (point.z < std::min(a.Z, b.Z) - PATROL_WAIT_TRAIL_FLOOR ||
                    point.z > std::max(a.Z, b.Z) + PATROL_WAIT_TRAIL_FLOOR)
                    continue;
                G3D::Vector2 const legFrom(a.X, a.Y);
                G3D::Vector2 const legTo(b.X, b.Y);
                float const gap = PointSegmentDistance2d(flat, legFrom, legTo);
                if (step == 0)
                    nowGap = std::min(nowGap, gap);
                if (gap >= walkGap)
                    continue;
                walkGap = gap;
                nearWalk = point;
                G3D::Vector2 const leg = legTo - legFrom;
                float const lengthSq = leg.squaredLength();
                float const along =
                    lengthSq > 0.0f ? std::clamp((flat - legFrom).dot(leg) / lengthSq, 0.0f, 1.0f) : 0.0f;
                nearLane = G3D::Vector3(a.X + (b.X - a.X) * along, a.Y + (b.Y - a.Y) * along,
                                        a.Z + (b.Z - a.Z) * along);
            }
        }
        if (walkGap > reach || walkGap + PATROL_LANE_DEEPER > nowGap)
            return false;
        return leader->GetMap()->isInLineOfSight(nearLane.x, nearLane.y, nearLane.z + patrol->GetCollisionHeight(),
                                                 nearWalk.x, nearWalk.y,
                                                 nearWalk.z + leader->GetCollisionHeight(),
                                                 leader->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                                 VMAP::ModelIgnoreFlags::Nothing);
    }

    // [b48_scholo] A hold (ROUTE_PATROL_HOLD, ROUTE_REST_HOLD) clears the leader's waypoint, which stops no walk
    // already under way: the leader's "test runner move" spline still ending at that waypoint (not a jump, a
    // link's straight walk or a gate pass) is stopped here. True: stopped; `to` is where it went.
    bool HaltLeaderWalk(LeaderGroup& group, Player* leader, G3D::Vector3& to)
    {
        // [lead2] a ROUTE-phase tool only: while the tank's pull order is open its walk is the order's
        if (Leader::LEADER_ONE_TANK_MOVER && Leader::Orders::IsOpen(leader->GetGUID()))
            return false;
        LeaderGroup::WalkPoint walk;
        if (!leader->isMoving() || leader->IsInCombat() || !group.GetWaypoint(leader->GetGUID(), walk) ||
            walk.mapId != leader->GetMapId() || walk.jump || walk.straight || walk.gatePass)
            return false;
        if (!leader->movespline || !leader->movespline->Initialized() || leader->movespline->Finalized())
            return false;
        G3D::Vector3 const end = leader->movespline->FinalDestination();
        if (std::hypot(end.x - walk.x, end.y - walk.y) > HALT_WALK_SAME_DEST)
            return false;
        to = end;
        leader->GetMotionMaster()->Clear();
        leader->StopMoving();
        return true;
    }

    std::unordered_map<uint32, PatrolStepBackState> patrolStepBacks;  // per instance (world thread)

    // An idle mob that is no patrol (no walk path of its own or of its formation leader) and not `patrol`'s.
    bool StaticPackMob(Creature* mob, Creature* patrol)
    {
        CreatureGroup const* formation = patrol->GetFormation();
        return mob != patrol && !(formation && mob->GetFormation() == formation) && !PatrolWalkPath(mob);
    }

    bool PatrolStepBackSpot(Player* leader, Creature* patrol, float reach, G3D::Vector3& spot, std::string& how,
                            std::vector<G3D::Vector3>& hops)
    {
        std::list<Creature*> idle;
        ShadowMobCheck check{leader};
        Acore::CreatureListSearcher<ShadowMobCheck> searcher(leader, idle, check);
        Cell::VisitObjects(leader, searcher, PATROL_WAIT_SCAN);
        std::vector<Creature*> packs;
        for (Creature* mob : idle)
            if (mob->HasReactState(REACT_AGGRESSIVE) && StaticPackMob(mob, patrol))
                packs.push_back(mob);
        auto const packReaches = [&](G3D::Vector3 const& point, float extra)
        {
            return std::any_of(packs.begin(), packs.end(),
                               [&](Creature* mob)
                               {
                                   return mob->GetExactDist(point.x, point.y, point.z) <=
                                          AggroReach(mob, leader) + PATROL_WAIT_ASSIST + extra;
                               });
        };
        WaypointPath const* path = PatrolWalkPath(patrol);
        auto const walkReaches = [&](G3D::Vector3 const& point)
        {
            if (patrol->GetExactDist(point.x, point.y, point.z) <= reach)
                return true;
            if (!path || path->Nodes.empty())
                return false;
            std::vector<WaypointNode> const& nodes = path->Nodes;
            G3D::Vector2 const flat(point.x, point.y);
            for (size_t index = 0; index < nodes.size(); ++index)
            {
                WaypointNode const& from = nodes[index];
                WaypointNode const& to = nodes[(index + 1) % nodes.size()];
                if (point.z < std::min(from.Z, to.Z) - PATROL_WAIT_TRAIL_FLOOR ||
                    point.z > std::max(from.Z, to.Z) + PATROL_WAIT_TRAIL_FLOOR)
                    continue;
                if (PointSegmentDistance2d(flat, G3D::Vector2(from.X, from.Y), G3D::Vector2(to.X, to.Y)) <= reach)
                    return true;
            }
            return false;
        };

        // [b48_patrol] the walk back, point by point: nullptr when clear, else the mob (or the patrol) it wakes
        Creature* walkBlocker = nullptr;
        auto const walkClear = [&](G3D::Vector3 const& point)
        {
            if (patrol->GetExactDist(point.x, point.y, point.z) <= reach)
            {
                walkBlocker = patrol;
                return false;
            }
            for (Creature* mob : packs)
                if (AggroesAtPoint(mob, leader, point,
                                   AggroReach(mob, leader) + PATROL_WAIT_ASSIST + APPROACH_FOLLOW_SPREAD, false))
                {
                    walkBlocker = mob;
                    return false;
                }
            return true;
        };

        std::vector<G3D::Vector3> const trail = MovementAction::TankTrailPoints(leader);
        G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        for (int pass = 0; pass < 2; ++pass)
        {
            float walkedBack = 0.0f;
            G3D::Vector3 previous = here;
            std::vector<G3D::Vector3> walked;
            for (auto point = trail.rbegin(); point != trail.rend(); ++point)
            {
                walkedBack += (*point - previous).length();
                previous = *point;
                if (walkedBack > PATROL_WAIT_TRAIL_MAX)
                    break;
                // [b48_patrol] another floor on the way back, or a point the followers would wake a mob from:
                // the walk goes no further
                if (std::fabs(point->z - here.z) > PATROL_WAIT_TRAIL_FLOOR)
                {
                    how = Acore::StringFormat("the way back leaves the floor {:.0f} yd back", walkedBack);
                    break;
                }
                if (!walkClear(*point))
                {
                    how = Acore::StringFormat("the way back passes {} {:.0f} yd back",
                                              walkBlocker ? walkBlocker->GetName() : std::string("?"), walkedBack);
                    break;
                }
                if (walked.empty() || (*point - walked.back()).length() >= PATROL_STEP_BACK_HOP)
                    walked.push_back(*point);
                if (walkedBack < PATROL_STEP_BACK_MIN)
                    continue;
                bool const clear = pass == 0 ? !walkReaches(*point) && !packReaches(*point, 0.0f)
                                             : !packReaches(*point, reach);
                if (!clear)
                    continue;
                spot = *point;
                hops = walked;
                if (hops.empty() || (hops.back() - spot).length() > 0.1f)
                    hops.push_back(spot);
                how = pass == 0 ? Acore::StringFormat("out of its walk, {:.0f} yd back", walkedBack)
                                : Acore::StringFormat("where it meets the group clear of the packs, {:.0f} yd "
                                                      "back", walkedBack);
                return true;
            }
        }
        return false;
    }

    bool LeaderPicksFor(Leader::LeaderGroup& group)
    {
        return Leader::Leads(Leader::LEADER_ONE_PICKER) && group.LeaderDecides();
    }

    bool LeaderVerdictCovers(Leader::LeaderGroup& group, ObjectGuid mob)
    {
        auto const& lead = group.State();
        // [lead30] no age for a led party: the verdict holds until the next look replaces it
        return lead.verdictMs &&
               (Leader::LEADER_NO_FORCING_TIMERS ||
                getMSTimeDiff(lead.verdictMs, getMSTime()) <= LEADER_VERDICT_TTL_MS) &&
               (lead.verdictTarget == mob ||
                std::find(lead.verdictPack.begin(), lead.verdictPack.end(), mob) != lead.verdictPack.end());
    }

    bool PartyVerdict(Leader::LeaderGroup& group, ObjectGuid mob, bool& accepted, uint32& mobs)
    {
        if (!LeaderPicksFor(group))
            return CarefulPlanVerdict::Get(group.LeaderGuid(), mob, accepted, mobs);
        if (!LeaderVerdictCovers(group, mob))
            return false;
        accepted = group.State().verdictAccepted;
        mobs = group.State().verdictMobs;
        return true;
    }

    // [lead4] LEADER_ONE_PATROL_OWNER: the leader owns the patrol decision (LeaderPatrolTick); the harness's
    // patrol hold is its forecast only (no wait budget, no handover, no hold clock or cooldown).
    bool LeaderOwnsPatrols(Leader::LeaderGroup& group)
    {
        return Leader::Leads(Leader::LEADER_ONE_PATROL_OWNER) && group.LeaderDecides();
    }

    // The patrol that holds the walk, with why ("planner pulls it" / "<yd> from <member>"); empty: no hold.
    std::string PatrolHoldWhy(Leader::LeaderGroup& group, Player* leader)
    {
        GroupRouteState& routeState = group.Route();
        bool const leaderOwns = LeaderOwnsPatrols(group);  // [lead4]
        if (!ROUTE_PATROL_HOLD || !leader->IsAlive() || leader->IsInCombat() || group.InCombat() ||
            !routeState.escort.IsEmpty())
            return std::string();
        uint32 const now = getMSTime();
        PatrolHold& hold = patrolHolds[group.InstanceId()];
        // [b48_scholo] the cooldown keeps the planner / "comes" holds off only; the lane hold has the wait budget
        bool const cooling = !leaderOwns && hold.cooldownUntilMs && int32(now - hold.cooldownUntilMs) < 0;
        auto const release = [&hold]()
        {
            hold.sinceMs = 0;
            hold.patrol.Clear();
            hold.lane = false;
            return std::string();
        };
        // [b48_scholo] the leader's harness walk: this tick's waypoint (Steer gives the next one after the hold)
        LeaderGroup::WalkPoint walk;
        bool const walking = PATROL_LANE_HOLD && group.GetWaypoint(group.LeaderGuid(), walk) &&
                             walk.mapId == leader->GetMapId() && !walk.jump;
        std::list<Creature*> idle;
        IdleAggroCheck check{leader};
        Acore::CreatureListSearcher<IdleAggroCheck> searcher(leader, idle, check);
        Cell::VisitObjects(leader, searcher, PATROL_HOLD_SCAN);
        std::string why;
        Creature* comesMob = nullptr;  // [b47_patrol] the patrol of a "comes to the group" hold
        Creature* holdMob = nullptr;   // [b48_scholo] the hold's patrol, whatever its kind
        bool laneHold = false;         // [b48_scholo]
        G3D::Vector3 laneTo;
        for (Creature* mob : idle)
        {
            if (routeState.bossCreatures.count(mob->GetGUID()) || mob->IsDungeonBoss() || !PatrolWalkPath(mob))
                continue;
            if (!leaderOwns && PatrolHandedOver(group.InstanceId(), mob->GetGUID(), now))
                continue;  // [b47_patrol] the tank's planner has it (PATROL_WAIT_BUDGET)
            if (!cooling)
            {
                bool accepted = false;
                uint32 mobs = 0;
                if (PartyVerdict(group, mob->GetGUID(), accepted, mobs) && accepted)  // [lead5]
                {
                    why = Acore::StringFormat("patrol {} {:.1f} yd: the tank's planner pulls it ({} mobs)",
                                              mob->GetName(), leader->GetExactDist(mob), mobs);
                    holdMob = mob;  // [b48_scholo]
                    break;
                }
                float const reach = AggroReach(mob, leader) + PATROL_HOLD_MARGIN;
                for (LeaderGroup::MemberInfo const& member : group.Members())
                {
                    Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    if (!bot || !bot->IsAlive() || bot->GetMap() != leader->GetMap())
                        continue;
                    if (float const gap = bot->GetExactDist(mob); gap <= reach)
                    {
                        why = Acore::StringFormat(
                            "patrol {} {:.1f} yd from {} (reach {:.1f}): it comes to the group", mob->GetName(),
                            gap, member.name, reach);
                        comesMob = mob;  // [b47_patrol]
                        holdMob = mob;   // [b48_scholo]
                        break;
                    }
                }
            }
            // [b48_scholo] PATROL_LANE_HOLD: the walk to this tick's waypoint (or the one this lane hold stopped,
            // for the same route target) comes into the patrol's walk
            if (why.empty() && PATROL_LANE_HOLD)
            {
                bool haveTo = walking;
                G3D::Vector3 to(walk.x, walk.y, walk.z);
                if (!haveTo && hold.lane && hold.patrol == mob->GetGUID() && hold.laneTarget == routeState.target)
                {
                    haveTo = true;
                    to = hold.laneTo;
                }
                float walkGap = 0.0f;
                float nowGap = 0.0f;
                float const laneReach = AggroReach(mob, leader);
                if (haveTo && WalkEntersPatrolLane(leader, mob, to, laneReach, walkGap, nowGap))
                {
                    why = Acore::StringFormat("patrol {} {:.1f} yd: the walk to ({:.1f},{:.1f},{:.1f}) comes "
                                              "{:.1f} yd to its walk (reach {:.1f}, {:.1f} yd now): it waits "
                                              "out of its way",
                                              mob->GetName(), leader->GetExactDist(mob), to.x, to.y, to.z,
                                              walkGap, laneReach, nowGap);
                    comesMob = mob;
                    holdMob = mob;
                    laneHold = true;
                    laneTo = to;
                }
            }
            if (!why.empty())
                break;
        }
        // [b48_scholo] PATROL_HOLD_LINGER_MS: the patrol walked out of the hold's reach (its turn): the hold lasts
        bool lingering = false;
        if (why.empty() && !cooling && hold.sinceMs && !hold.lane && !hold.patrol.IsEmpty() &&
            getMSTimeDiff(hold.lastMatchMs, now) <= PATROL_HOLD_LINGER_MS)
        {
            Creature* last = leader->GetMap()->GetCreature(hold.patrol);
            if (last && last->IsAlive() && !last->IsInCombat() &&
                leader->GetExactDist(last) <= PATROL_HOLD_SCAN + PATROL_HOLD_MARGIN &&
                (leaderOwns || !PatrolHandedOver(group.InstanceId(), last->GetGUID(), now)))
            {
                why = hold.why;
                comesMob = hold.comes ? last : nullptr;
                lingering = true;
                if (!hold.lingerLogged)
                {
                    hold.lingerLogged = true;
                    LOG_INFO("playerbots", "[b48_scholo] patrol_hold_linger {}: patrol {} walked out of the "
                             "hold's reach ({:.1f} yd from the leader): the hold lasts up to {} s more ({})",
                             leader->GetName(), last->GetName(), leader->GetExactDist(last),
                             PATROL_HOLD_LINGER_MS / IN_MILLISECONDS, hold.why);
                }
            }
        }
        if (why.empty())
            return release();
        // [b47_patrol] PATROL_WAIT_BUDGET: spent, no more hold for it (PatrolStepBack hands it over)
        // [lead4] not for the leader: its patrol decision is a wait with a forecast window or a pull, no budget
        if (!leaderOwns && comesMob && ChargePatrolWait(group.InstanceId(), comesMob->GetGUID(), now, !hold.sinceMs))
            return release();
        if (!hold.sinceMs)
            hold.sinceMs = now ? now : 1;
        else if (!leaderOwns && !laneHold && getMSTimeDiff(hold.sinceMs, now) > PATROL_HOLD_MAX_MS)
        {
            hold.cooldownUntilMs = now + PATROL_HOLD_COOLDOWN_MS;
            hold.timedOut = true;
            return release();
        }
        if (!lingering)
        {
            // [b48_scholo]
            hold.patrol = holdMob ? holdMob->GetGUID() : ObjectGuid::Empty;
            hold.lastMatchMs = now ? now : 1;
            hold.comes = comesMob != nullptr;
            hold.lingerLogged = false;
            hold.lane = laneHold;
            if (laneHold)
            {
                if (!hold.laneTo.fuzzyEq(laneTo) || hold.laneTarget != routeState.target)
                    LOG_INFO("playerbots", "[b48_scholo] patrol_lane_hold {}: {}", leader->GetName(), why);
                hold.laneTo = laneTo;
                hold.laneTarget = routeState.target;
            }
        }
        hold.why = why;
        return why;
    }

    // [b46_patrol] PATROL_STEP_BACK (PatrolStepBackSpot). True: the leader walks to the step-back spot or stands
    // there (no other walk this tick); called by Steer before the rest hold and by the regen wait (TestHarnessMgr).
    bool PatrolStepBack(Leader::LeaderGroup& group, Player* leader)
    {
        GroupRouteState& routeState = group.Route();
        Map* map = leader->GetMap();
        if (!PATROL_STEP_BACK || !map || !map->IsNonRaidDungeon() || !leader->IsAlive() ||
            !routeState.escort.IsEmpty())
            return false;

        uint32 const now = getMSTime();
        PatrolStepBackState& state = patrolStepBacks[group.InstanceId()];
        auto const holdClocks = [&]()
        {
            routeState.steerSinceMs = now;
            routeState.steerAnchor = leader->GetPosition();
            routeState.steerNoNearerMs = 0;
            routeState.driveSinceMs = now;
            routeState.driveAnchor = leader->GetPosition();
        };

        // [b47_patrol] No step back where something else owns the group's spot. mgt-n 20261026-104908-mgt-n-5H-13
        // (286-341 s, build 46): a Wretched Skulker "walks beside idle Selin Fireheart" sent the group 80 yd back to
        // (120.7,-6.1) while "mgt selin room to rally" held every member at (200,-3): move_conflict 'stuck recovery'
        // vs the rally (180 deg) for all five, 55 s lost, six "no clear spot" tries, then the wipe on Selin. Owned:
        // a boss encounter in progress, a dungeon room tactic holding the group (Selin's room, the Krik'thir watcher
        // room, Jedoga's initiates), an event started within PATROL_STEP_BACK_EVENT_MS, or a live boss within
        // PATROL_STEP_BACK_BOSS_RANGE of the leader (its room: the boss logic and the room tactics own the rally).
        {
            constexpr float PATROL_STEP_BACK_BOSS_RANGE = 60.0f;
            constexpr uint32 PATROL_STEP_BACK_EVENT_MS = 60 * IN_MILLISECONDS;  // a starter (event, key) used
            char const* owned = nullptr;
            if (group.BossEncounterInProgress())
                owned = "a boss encounter is in progress";
            if (!owned && routeState.lastStarterUseMs &&
                getMSTimeDiff(routeState.lastStarterUseMs, now) < PATROL_STEP_BACK_EVENT_MS)
                owned = "an event was started";
            if (!owned)
                if (PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader))
                {
                    if (leader->GetMapId() == MagistersTerrace::MAP_MAGISTERS_TERRACE &&
                        (MagistersTerrace::SelinRoom::HoldsGroup(leaderAI) ||
                         MagistersTerrace::SelinRoom::OwnsPull(leaderAI) ||
                         MagistersTerrace::SelinRoom::EntryHolds(leader)))
                        owned = "Selin's room tactic holds the group";
                    else if (leader->GetMapId() == ANWatcherRoom::MAP_AZJOL_NERUB &&
                             (ANWatcherRoom::OwnsPull(leaderAI) || ANWatcherRoom::HoldsRally(leaderAI)))
                        owned = "the watcher room tactic holds the group";
                    else if (leader->GetMapId() == AKJedogaRoom::MAP_AHNKAHET && AKJedogaRoom::OwnsPull(leaderAI))
                        owned = "Jedoga's room tactic holds the group";
                }
            if (!owned)
            {
                std::list<Creature*> nearBy;
                ShadowMobCheck bossCheck{leader};
                Acore::CreatureListSearcher<ShadowMobCheck> bossSearcher(leader, nearBy, bossCheck);
                Cell::VisitObjects(leader, bossSearcher, PATROL_STEP_BACK_BOSS_RANGE);
                for (Creature* mob : nearBy)
                    if (mob->IsDungeonBoss() || routeState.bossCreatures.count(mob->GetGUID()))
                    {
                        owned = "a live boss is near (its room)";
                        break;
                    }
            }
            if (owned)
            {
                if (!state.patrol.IsEmpty())
                {
                    LOG_INFO("playerbots", "[b47_patrol] patrol_step_back {}: ends, {}", leader->GetName(), owned);
                    group.LogEvent("patrol_step_back", JsonObject().Str("state", "done").Str("why", owned));
                    state.patrol.Clear();
                    state.arrived = false;
                    state.cooldownUntilMs = now + PATROL_STEP_BACK_COOLDOWN_MS;
                }
                return false;
            }
        }

        // [b47_patrol] PATROL_WAIT_BUDGET spent (ChargePatrolWait): the patrol becomes the route target, held as one
        // (boss_neighbour_target's way: Retarget keeps it while it lives, is not skipped and its time runs); the
        // tank's careful pull decides how (a patrol pull back / los pull to the cleared spot, the meet), a hopeless or
        // refused one is skipped as any trash. This tick only clears the walk; Steer approaches it on the next.
        if (auto const pending = patrolHandOvers.find(group.InstanceId()); pending != patrolHandOvers.end())
        {
            ObjectGuid const handed = pending->second;
            patrolHandOvers.erase(pending);
            Creature* patrol = map->GetCreature(handed);
            if (patrol && patrol->IsAlive() && !patrol->IsInCombat() && !leader->IsInCombat() && !group.InCombat() &&
                !routeState.skipUntil.count(handed))
            {
                if (!state.patrol.IsEmpty())
                {
                    state.patrol.Clear();
                    state.arrived = false;
                    state.cooldownUntilMs = now + PATROL_STEP_BACK_COOLDOWN_MS;
                }
                LOG_INFO("playerbots", "[b47_patrol] patrol_handover {}: patrol {} {} ({:.1f} yd) waited for long "
                         "enough: the route target, the tank's planner pulls it", leader->GetName(), patrol->GetName(),
                         patrol->GetEntry(), leader->GetExactDist(patrol));
                group.LogEvent("patrol_handover",
                         JsonObject().Str("patrol", patrol->GetName()).Num("entry", patrol->GetEntry())
                             .Real("distance", leader->GetExactDist(patrol))
                             .Str("previousTarget", routeState.target.IsEmpty() ? std::string()
                                                                                   : routeState.target.ToString()));
                routeState.neighbourTarget = handed;
                routeState.target = handed;
                routeState.targetIsBoss = false;
                routeState.targetIsStarter = false;
                routeState.targetRouteRule.clear();
                routeState.targetSinceMs = now;
                routeState.guaranteeHolding = false;
                routeState.arrivedMs = 0;
                routeState.approach = 0;
                routeState.approachBlocker.Clear();
                routeState.stopSinceMs = 0;
                if (!LeaderPicksFor(group))  // [lead5]
                    RouteTarget::Set(group.LeaderGuid(), handed);
                group.ClearWaypoint(group.LeaderGuid());
                holdClocks();
                return true;
            }
        }

        if (!state.patrol.IsEmpty())
        {
            Creature* patrol = map->GetCreature(state.patrol);
            float const gap = patrol ? patrol->GetExactDist(state.spot.x, state.spot.y, state.spot.z) : 0.0f;
            char const* end = nullptr;
            if (!patrol || !patrol->IsAlive())
                end = "patrol dead";
            else if (patrol->IsInCombat() || leader->IsInCombat() || group.InCombat())
                end = "fight";
            else if (getMSTimeDiff(state.sinceMs, now) > PATROL_STEP_BACK_MAX_MS)
                end = "timeout";
            else if (state.arrived && gap > state.reach + PATROL_STEP_BACK_GONE)
                end = "walked on";
            else if (!LeaderOwnsPatrols(group) && ChargePatrolWait(group.InstanceId(), state.patrol, now, false))
                end = "wait budget spent";  // [b47_patrol] handed over on the next tick ([lead4] not the leader's)
            // [b48_patrol] the patrol came onto the way back: no walk toward it, the group stands (the planner
            // meets it)
            else if (!state.arrived && state.hop < state.hops.size() &&
                     patrol->GetExactDist(state.hops[state.hop].x, state.hops[state.hop].y,
                                          state.hops[state.hop].z) <= state.reach)
                end = "patrol on the way back";
            if (end)
            {
                LOG_INFO("playerbots", "[b46_patrol] patrol_step_back {}: ends ({}) after {} s, patrol {:.1f} yd from "
                         "the spot", leader->GetName(), end, getMSTimeDiff(state.sinceMs, now) / IN_MILLISECONDS, gap);
                group.LogEvent("patrol_step_back",
                         JsonObject().Str("state", "done").Str("why", end)
                             .Str("patrol", patrol ? patrol->GetName() : std::string())
                             .Num("sec", getMSTimeDiff(state.sinceMs, now) / IN_MILLISECONDS).Real("gap", gap));
                state.patrol.Clear();
                state.arrived = false;
                state.cooldownUntilMs = now + PATROL_STEP_BACK_COOLDOWN_MS;
                return false;
            }
            // [b48_patrol] hop by hop along the trail (PatrolStepBackSpot), never a free walk to the spot
            while (state.hop < state.hops.size() &&
                   leader->GetExactDist(state.hops[state.hop].x, state.hops[state.hop].y, state.hops[state.hop].z) <=
                       PATROL_STEP_BACK_ARRIVED)
                ++state.hop;
            if (state.hop >= state.hops.size() ||
                leader->GetExactDist(state.spot.x, state.spot.y, state.spot.z) <= PATROL_STEP_BACK_ARRIVED)
            {
                state.arrived = true;
                group.ClearWaypoint(group.LeaderGuid());
            }
            else
                group.SetLeaderWaypoint(leader, state.hops[state.hop].x, state.hops[state.hop].y,
                                  state.hops[state.hop].z);
            holdClocks();
            return true;
        }

        if (leader->IsInCombat() || group.InCombat() ||
            (state.cooldownUntilMs && int32(now - state.cooldownUntilMs) < 0) ||
            (state.noSpotMs && getMSTimeDiff(state.noSpotMs, now) < PATROL_STEP_BACK_RETRY_MS))
            return false;
        // [b48_patrol] a patrol handed to the tank's planner is the route target now (ohb-h-5C-16: the Tracking Hound's
        // handover at 652 s, then a step back from the Sentry at 653 s walked the other way): no step back meanwhile
        if (!routeState.target.IsEmpty() && routeState.target == routeState.neighbourTarget &&
            PatrolHandedOver(group.InstanceId(), routeState.target, now))
            return false;

        // The patrol that walks in (PatrolHoldWhy's second test); one the tank's planner pulls now is its pull.
        std::list<Creature*> idle;
        IdleAggroCheck check{leader};
        Acore::CreatureListSearcher<IdleAggroCheck> searcher(leader, idle, check);
        Cell::VisitObjects(leader, searcher, PATROL_HOLD_SCAN);
        Creature* patrol = nullptr;
        std::string nearName;
        float nearGap = 0.0f;
        for (Creature* mob : idle)
        {
            if (routeState.bossCreatures.count(mob->GetGUID()) || mob->IsDungeonBoss() || !PatrolWalkPath(mob) ||
                PatrolHandedOver(group.InstanceId(), mob->GetGUID(), now))  // [b47_patrol]
                continue;
            bool accepted = false;
            uint32 mobs = 0;
            if (PartyVerdict(group, mob->GetGUID(), accepted, mobs) && accepted)
                return false;
            float const reach = AggroReach(mob, leader) + PATROL_HOLD_MARGIN;
            for (LeaderGroup::MemberInfo const& member : group.Members())
            {
                Player* bot = ObjectAccessor::FindPlayer(member.guid);
                if (!bot || !bot->IsAlive() || bot->GetMap() != map)
                    continue;
                float const gap = bot->GetExactDist(mob);
                if (gap <= reach && (!patrol || gap < nearGap))
                {
                    patrol = mob;
                    nearName = member.name;
                    nearGap = gap;
                }
            }
        }
        if (!patrol)
            return false;

        // Why step back: a member sits or wants to (RestState's own lines), or the patrol walks beside an idle pack
        // (the fight where it meets the group would call that pack in).
        std::string rester;
        for (LeaderGroup::MemberInfo const& member : group.Members())
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || !bot->IsAlive() || bot->GetMap() != map)
                continue;
            if (RestState::IsDrinking(bot) || RestState::IsEating(bot) ||
                (bot->GetMaxPower(POWER_MANA) && RestState::WantsDrink(bot)) || RestState::WantsFood(bot))
            {
                rester = member.name;
                break;
            }
        }
        Creature* beside = nullptr;
        for (Creature* mob : idle)
            if (StaticPackMob(mob, patrol) &&
                mob->GetExactDist(patrol) <= AggroReach(mob, leader) + PATROL_WAIT_ASSIST)
            {
                beside = mob;
                break;
            }
        if (rester.empty() && !beside)
            return false;

        std::string why;
        if (!rester.empty())
            why = rester + " rests";
        else
            why = "it walks beside idle " + beside->GetName();
        float const reach = AggroReach(patrol, leader) + PATROL_STEP_BACK_SPREAD;
        G3D::Vector3 spot;
        std::string how;
        std::vector<G3D::Vector3> hops;
        if (!PatrolStepBackSpot(leader, patrol, reach, spot, how, hops))
        {
            state.noSpotMs = now ? now : 1;
            LOG_INFO("playerbots", "[b46_patrol] patrol_step_back {}: patrol {} {:.1f} yd from {} walks in ({}), no "
                     "clear spot on the way in within {:.0f} yd: stays ([b48_patrol] {})", leader->GetName(),
                     patrol->GetName(), nearGap, nearName, why, PATROL_WAIT_TRAIL_MAX, how);
            group.LogEvent("patrol_step_back",
                     JsonObject().Str("state", "no clear spot").Str("patrol", patrol->GetName())
                         .Num("entry", patrol->GetEntry()).Str("why", why).Str("how", how).Real("gap", nearGap));
            return false;
        }
        // [b47_patrol] a new wait for the same patrol counts against its budget; spent: handed over next tick
        // ([b48_patrol] only a step back that walks: a search that found no spot is no wait)
        if (!LeaderOwnsPatrols(group) && ChargePatrolWait(group.InstanceId(), patrol->GetGUID(), now, true))  // [lead4]
            return false;

        state.patrol = patrol->GetGUID();
        state.spot = spot;
        state.reach = reach;
        state.sinceMs = now ? now : 1;
        state.noSpotMs = 0;
        state.arrived = false;
        state.how = how;
        state.hops = hops;  // [b48_patrol]
        state.hop = 0;
        LOG_INFO("playerbots", "[b46_patrol] patrol_step_back {}: patrol {} {} {:.1f} yd from {} walks in ({}): the "
                 "group steps back {:.1f} yd along its way in to ({:.1f},{:.1f},{:.1f}), {} (reach {:.1f})",
                 leader->GetName(), patrol->GetName(), patrol->GetEntry(), nearGap, nearName, why,
                 leader->GetExactDist(spot.x, spot.y, spot.z), spot.x, spot.y, spot.z, how, reach);
        group.LogEvent("patrol_step_back",
                 JsonObject().Str("state", "start").Str("patrol", patrol->GetName()).Num("entry", patrol->GetEntry())
                     .Real("gap", nearGap).Str("member", nearName).Str("why", why).Str("how", how).Real("reach", reach)
                     .Real("x", spot.x).Real("y", spot.y).Real("z", spot.z)
                     .Real("fromX", leader->GetPositionX()).Real("fromY", leader->GetPositionY())
                     .Real("fromZ", leader->GetPositionZ()).Num("hops", uint32(hops.size())));
        group.SetLeaderWaypoint(leader, hops.front().x, hops.front().y, hops.front().z);  // [b48_patrol] first hop
        holdClocks();
        return true;
    }

    // [lead4] One patrol owner. The harness's forecast (PatrolHoldWhy: a patrol comes to the group, the walk to the
    // next waypoint enters its lane, the planner pulls it) feeds one decision:
    // - the leader's planner pulls it (an accepted verdict on it): the group stands, the order comes;
    // - the walk enters its lane, or its forecast walk never leaves the group's spot (PullPlanner::PatrolLeavesReach,
    //   the planner's own forecast): pull it - an intercept request (LeaderInputs::intercept: planned as the target,
    //   no patrol wait for it);
    // - else wait (a declared Wait, ended by the forecast, no clock), standing, or back along the way in when a member
    //   rests or the patrol walks beside an idle pack (PatrolStepBack's spot).
    // No PATROL_WAIT_BUDGET, no handover, no PATROL_HOLD_MAX_MS / COOLDOWN, and the planner holds for a patrol only
    // while its forecast shows a window (mayHoldFor, ChooseTargetActions.cpp).
    bool LeaderPatrolTick(Leader::LeaderGroup& group, Player* leader)
    {
        GroupRouteState& routeState = group.Route();
        auto& lead = group.State();
        // [lead18] LEADER_COMMIT_PULL: a committed pull owns its patrol timing (its window, runCommit); no wait by
        // forecast meanwhile
        if (Leader::LEADER_COMMIT_PULL && lead.commit)
            return false;
        uint32 const now = getMSTime() ? getMSTime() : 1;
        Map* map = leader->GetMap();
        // [lead4c] build 54 hos-h: 33 "start" lines for two patrols - the forecast flickers (the scan edge, the
        // linger) and the two patrols took turns; a wait of the same patrol resumed within LEADER_PATROL_WAIT_GAP_MS
        // is the same wait (no new line), and its "done" is logged once it really ended
        constexpr uint32 LEADER_PATROL_WAIT_GAP_MS = 10 * IN_MILLISECONDS;
        auto const endWait = [&](char const* why)
        {
            if (!lead.waitPatrol.IsEmpty())
            {
                lead.lastWaitPatrol = lead.waitPatrol;
                lead.lastWaitEndMs = now;
                LOG_DEBUG("playerbots", "[lead4c] leader_patrol_wait {}: ends ({}) after {} s", group.Run().runId, why,
                          getMSTimeDiff(lead.waitSinceMs, now) / IN_MILLISECONDS);
            }
            lead.waitPatrol.Clear();
            lead.waitSinceMs = 0;
            lead.waitWhy.clear();
            PatrolStepBackState& step = patrolStepBacks[group.InstanceId()];
            step.patrol.Clear();
            step.arrived = false;
        };
        if (!map || !leader->IsAlive() || leader->IsInCombat() || group.InCombat() || !routeState.escort.IsEmpty())
        {
            endWait("fight or escort");
            return false;
        }
        std::string const why = PatrolHoldWhy(group, leader);
        PatrolHold const& hold = patrolHolds[group.InstanceId()];
        Creature* patrol = why.empty() || hold.patrol.IsEmpty() ? nullptr : map->GetCreature(hold.patrol);
        if (!patrol || !patrol->IsAlive() || patrol->IsInCombat())
        {
            endWait(why.empty() ? "no patrol holds the group" : "the patrol is gone or fights");
            return false;
        }
        routeState.steerSinceMs = now;
        routeState.steerAnchor = leader->GetPosition();
        routeState.steerNoNearerMs = 0;
        routeState.driveSinceMs = now;
        routeState.driveAnchor = leader->GetPosition();
        float const reach = AggroReach(patrol, leader) + PATROL_HOLD_MARGIN;
        bool accepted = false;
        uint32 mobs = 0;
        bool const plannerPulls = PartyVerdict(group, patrol->GetGUID(), accepted, mobs) && accepted;
        // [lead33] rfc-h 20261031-151355: the patrol wait held the group on Ragefire Trogg while Trogg WAS the
        // planner's pull: the leader's plan target / its plan's mobs are no patrol to wait for - the plan pulls them
        if (Leader::LEADER_NO_FORCING_TIMERS &&
            (plannerPulls || patrol->GetGUID() == lead.planTarget ||
             std::find(lead.planMobs.begin(), lead.planMobs.end(), patrol->GetGUID()) != lead.planMobs.end()))
        {
            endWait("the leader's plan pulls it");
            return false;
        }
        bool known = false;
        bool const window = PullPlanner::PatrolLeavesReach(patrol, leader->GetPosition(), reach, &known);
        // [lead4b] the pull is chosen only once the patrol is near: within its reach (+ PATROL_HOLD_MARGIN) of a
        // member. A lane hold is a static test of its whole walk (build 53: ac-h / dme-n / uk-h intercepted patrols
        // 45-55 yd off in the first minute, every one "refused for now" at that range); far off it is a wait, and the
        // planner meets it first when it comes ("comes to the group: pulls it first").
        bool patrolNear = false;
        for (LeaderGroup::MemberInfo const& member : group.Members())
            if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                bot && bot->IsAlive() && bot->GetMap() == map && bot->GetExactDist(patrol) <= reach)
                patrolNear = true;
        // ... or, for a lane the route must cross, once it is within the tank's dungeon pull range (a lane hold no
        // approach of the patrol ends otherwise)
        constexpr float LEADER_LANE_PULL_RANGE = 40.0f;  // AggressiveTargetValue's DUNGEON_TANK_PULL_RANGE
        bool const laneInRange = hold.lane && leader->GetExactDist(patrol) <= LEADER_LANE_PULL_RANGE;
        // [lead4c] the lane wait is bounded: a lane the route must cross whose patrol never comes near would hold
        // the group for good (build 54 hos-h, Dark Rune Warrior / Theurgist 36-40 yd). LEADER_LANE_WAIT_MAX_MS of
        // waiting for it (gaps under LEADER_PATROL_WAIT_GAP_MS kept) ends in the pull: the leader's planner plans it
        // where it is, no blind walk.
        constexpr uint32 LEADER_LANE_WAIT_MAX_MS = 30 * IN_MILLISECONDS;
        // [lead14] ... and so is every patrol wait, not only a lane's: b58 mech-h (Mechanar Tinkerer pacing 35-36 yd
        // "beside idle Sunseeker Astromage", the step back "no clear spot"), sfk-h (Tormented Officer at "38.0 yd
        // (reach 38.0): it comes"), mgt-h (Wretched Skulker) each held the leader 4 minutes ("no walk / not
        // arrived") on a wait whose patrol never came nor left. LEADER_LANE_WAIT_MAX_MS of waiting for one patrol
        // ends in its pull, wherever it stands.
        bool laneWaitedOut = false;
        if (!plannerPulls)
        {
            if (lead.laneWaitPatrol != patrol->GetGUID() || !lead.laneWaitSeenMs ||
                getMSTimeDiff(lead.laneWaitSeenMs, now) > LEADER_PATROL_WAIT_GAP_MS)
            {
                lead.laneWaitPatrol = patrol->GetGUID();
                lead.laneWaitSinceMs = now;
            }
            lead.laneWaitSeenMs = now;
            laneWaitedOut = getMSTimeDiff(lead.laneWaitSinceMs, now) >= LEADER_LANE_WAIT_MAX_MS;
            if (laneWaitedOut && Leader::LEADER_NO_FORCING_TIMERS)
            {
                Leader::Dungeon::WaitedLong(group, "patrol_lane_wait", getMSTimeDiff(lead.laneWaitSinceMs, now),
                                            patrol->GetName());
                laneWaitedOut = false;  // [lead24] no pull because time passed
            }
        }
        // [lead12] a patrol the planner cannot pull (interceptBanned): no intercept; its lane is crossed while it is
        // farther than twice its reach from the leader (the walk timed against its loop), else the group waits
        bool banned = false;
        if (auto const ban = lead.interceptBanned.find(patrol->GetGUID()); ban != lead.interceptBanned.end())
        {
            if (int32(ban->second - now) > 0)
                banned = true;
            else
                lead.interceptBanned.erase(ban);
        }
        // [lead14] a banned patrol (its pull failed or never came) holds nothing but the timed lane crossing: the
        // group walks on, and meets it as a fight if it comes (the old path after its wait budget and handover)
        if (banned && (!hold.lane || (!patrolNear && leader->GetExactDist(patrol) > 2.0f * reach)))
        {
            endWait(hold.lane ? "its lane is crossed while it is far (no pull of it fits)"
                              : "no pull of it came: the group walks on");
            return false;
        }
        // [lead14] an intercept that brought no order for LEADER_INTERCEPT_MAX_MS (the planner holds it, cannot reach
        // it, or plans another mob): the patrol is banned, the group walks on
        constexpr uint32 LEADER_INTERCEPT_MAX_MS = 20 * IN_MILLISECONDS;
        if (Leader::LEADER_NO_FORCING_TIMERS && lead.intercept == patrol->GetGUID() && lead.interceptSinceMs &&
            getMSTimeDiff(lead.interceptSinceMs, now) >= LEADER_INTERCEPT_MAX_MS &&
            !Leader::Orders::IsOpen(group.LeaderGuid()))
            Leader::Dungeon::WaitedLong(group, "patrol_intercept", getMSTimeDiff(lead.interceptSinceMs, now),
                                        patrol->GetName());
        else if (lead.intercept == patrol->GetGUID() && lead.interceptSinceMs &&
                 getMSTimeDiff(lead.interceptSinceMs, now) >= LEADER_INTERCEPT_MAX_MS &&
                 !Leader::Orders::IsOpen(group.LeaderGuid()))
        {
            constexpr uint32 LEADER_INTERCEPT_BAN_MS = 5 * MINUTE * IN_MILLISECONDS;
            lead.interceptBanned[patrol->GetGUID()] = now + LEADER_INTERCEPT_BAN_MS;
            LOG_INFO("playerbots", "[lead14] leader_patrol_pull {}: patrol {} {}: no order in {} s, banned; the "
                     "group walks on ({})", group.Run().runId, patrol->GetName(), patrol->GetEntry(),
                     getMSTimeDiff(lead.interceptSinceMs, now) / IN_MILLISECONDS, why);
            group.LogEvent("leader_patrol_pull",
                     JsonObject().Str("patrol", patrol->GetName()).Num("entry", patrol->GetEntry())
                         .Str("state", "no order: banned, the group walks on").Str("why", why)
                         .Real("distance", leader->GetExactDist(patrol)));
            lead.intercept.Clear();
            lead.interceptMs = 0;
            lead.interceptSinceMs = 0;
            endWait("its pull brought no order");
            return false;
        }
        bool const pull = Leader::LEADER_PATROL_IS_TARGET
                              ? !plannerPulls  // [lead34] «Если патруль, надо убивать патруль»: no wait, its pull
                              : !banned && !plannerPulls && (patrolNear || laneInRange || laneWaitedOut) &&
                                    (hold.lane || !window || laneWaitedOut);
        if (Leader::LEADER_PATROL_IS_TARGET && pull && lead.patrolTarget != patrol->GetGUID())
        {
            lead.patrolTarget = patrol->GetGUID();
            group.LogEvent("patrol_target",
                     JsonObject().Str("patrol", patrol->GetName()).Str("choice", "own pull")
                         .Str("reason", "it holds the route: " + why).Real("distance", leader->GetExactDist(patrol)));
        }
        auto const halt = [&](char const* kind)
        {
            if (G3D::Vector3 haltedTo; HaltLeaderWalk(group, leader, haltedTo))
                group.LogEvent("harness_walk_halt", JsonObject().Str("hold", kind).Str("why", why)
                                                         .Real("x", leader->GetPositionX())
                                                         .Real("y", leader->GetPositionY())
                                                         .Real("z", leader->GetPositionZ())
                                                         .Real("toX", haltedTo.x).Real("toY", haltedTo.y)
                                                         .Real("toZ", haltedTo.z));
            group.ClearWaypoint(group.LeaderGuid());
        };
        if (pull)
        {
            if (lead.intercept != patrol->GetGUID() || !lead.interceptMs ||
                getMSTimeDiff(lead.interceptMs, now) > Leader::LEADER_REQUEST_TTL_MS)
            {
                LOG_INFO("playerbots", "[lead4] leader_patrol_pull {}: patrol {} {} ({:.1f} yd): {} - pulled ({})",
                         group.Run().runId, patrol->GetName(), patrol->GetEntry(), leader->GetExactDist(patrol),
                         hold.lane ? "the route crosses its walk" : "its walk never leaves the group's spot", why);
                group.LogEvent("leader_patrol_pull",
                         JsonObject().Str("patrol", patrol->GetName()).Num("entry", patrol->GetEntry())
                             .Str("why", why).Bool("lane", hold.lane).Bool("window", window).Bool("forecast", known)
                             .Real("distance", leader->GetExactDist(patrol)));
            }
            if (lead.intercept != patrol->GetGUID() || !lead.interceptMs ||
                getMSTimeDiff(lead.interceptMs, now) > Leader::LEADER_REQUEST_TTL_MS)
                lead.interceptSinceMs = now;
            lead.intercept = patrol->GetGUID();
            lead.interceptMs = now;
            lead.interceptWhy = why;
            endWait("the patrol is pulled");
            halt("patrol pull");
            return true;
        }
        if (Leader::LEADER_PATROL_IS_TARGET)  // [lead34] no patrol wait (the planner pulls it already)
        {
            endWait("the leader's plan pulls it");
            return false;
        }
        if (lead.waitPatrol != patrol->GetGUID())
        {
            bool const resumed = lead.lastWaitPatrol == patrol->GetGUID() && lead.lastWaitEndMs &&
                                 getMSTimeDiff(lead.lastWaitEndMs, now) <= LEADER_PATROL_WAIT_GAP_MS;
            lead.waitPatrol = patrol->GetGUID();
            lead.waitSinceMs = resumed && lead.waitSinceMs ? lead.waitSinceMs : now;
            lead.waitWhy = why;
            routeState.lastEvent = "patrol wait: " + why;
            if (!resumed)
                group.LogEvent("leader_patrol_wait",
                     JsonObject().Str("state", "start").Str("patrol", patrol->GetName())
                         .Num("entry", patrol->GetEntry()).Str("why", why).Bool("plannerPulls", plannerPulls)
                         .Bool("window", window).Bool("forecast", known)
                         .Real("distance", leader->GetExactDist(patrol)));
        }
        // the wait's spot: back along the way in while a member rests or the patrol walks beside an idle pack
        if (!plannerPulls && PatrolStepBack(group, leader))
            return true;
        halt("patrol wait");
        return true;
    }
}
