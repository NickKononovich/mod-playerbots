/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "RouteEvents.h"
#include "LeaderReports.h"
#include "RoutePatrol.h"
#include "RouteTargets.h"  // [guild70] DOOR_REARM_MAX, DOOR_REARM_GAP_MS

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
    using namespace Leader::Knowledge;
    using Leader::Dungeon::PULL_ABORT_HOLD_MS;

    std::unordered_map<uint32, std::pair<uint32, uint32>> starterGatherMs;  // instance -> (wait since, last log)

    // [b35_ss2] Where a key gate's bars stand along its facing, measured from its spawn point. The bars are not on
    // the spawn's plane: strat-h 20261026-024524-strat-h-5C-1, King's Square Gate 175352, the leader held at
    // +0.2 yd from the plane (3572.5,-3450.6) and the healer at +1.7 yd (3573.6,-3447.6) with the bars between
    // them (door_cut "holding" 1294-1525 s, the leader's walk "walked 2.5" into them). The plane test called
    // both "through": rearm_check "the whole group is through it" every 15 s, strat_key_pass "sent: []" after
    // the healer's own key, 260 s stalled. Measured once while the gate is shut (its collision is off when open):
    // a ray from the use point to the far point and one back, the middle of the two hits. 0 until measured.
    float KeyGateBarsOffset(GameObject const* door, KeyDoorPassDef const& pass)
    {
        static std::unordered_map<uint64, float> offsets;  // the harness's world thread only
        uint64 const key = (uint64(door->GetEntry()) << 32) | uint32(door->GetSpawnId());  // same in every instance
        if (auto const found = offsets.find(key); found != offsets.end())
            return found->second;
        if (!door->m_model || !door->m_model->isEnabled() || door->GetGoState() != GO_STATE_READY)
            return 0.0f;
        // A portcullis is a lattice: a ray can slip between two bars, so a few heights and side offsets are tried.
        static float const heights[] = {1.0f, 0.6f, 1.5f, 2.0f};
        static float const lateral[] = {0.0f, 0.3f, -0.3f};
        bool hit = false;
        G3D::Vector3 middle;
        float nearHit = 0.0f;  // [b36_ss] the two hit distances, for the log
        float farHit = 0.0f;
        for (float const height : heights)
        {
            for (float const side : lateral)
            {
                G3D::Vector3 usePoint(pass.useX, pass.useY, pass.useZ + height);
                G3D::Vector3 farPoint(pass.farX, pass.farY, pass.farZ + height);
                float const length = (farPoint - usePoint).length();
                if (length < 0.5f)
                    return 0.0f;
                G3D::Vector3 const dir = (farPoint - usePoint) / length;
                G3D::Vector3 const across(-dir.y * side, dir.x * side, 0.0f);
                usePoint += across;
                farPoint += across;
                float there = length;
                float back = length;
                if (door->m_model->intersectRay(G3D::Ray(usePoint, dir), there, true, door->GetPhaseMask(),
                                                VMAP::ModelIgnoreFlags::Nothing) &&
                    door->m_model->intersectRay(G3D::Ray(farPoint, -dir), back, true, door->GetPhaseMask(),
                                                VMAP::ModelIgnoreFlags::Nothing))
                {
                    middle = (usePoint + dir * there + farPoint - dir * back) * 0.5f;
                    nearHit = there;
                    farHit = back;
                    hit = true;
                    break;
                }
            }
            if (hit)
                break;
        }
        if (!hit)
        {
            // [b36_ss] a miss is said once per gate (0 until a ray hits; GroupThroughKeyGate etc. use the spawn
            // plane meanwhile), not taken for a measurement
            static std::unordered_set<uint64> missLogged;
            if (missLogged.insert(key).second)
                LOG_INFO("playerbots", "[b36_ss] key gate {} {}: bars probe missed (12 rays), spawn plane used",
                         door->GetName(), door->GetEntry());
            return 0.0f;
        }
        float const offset = (middle.x - door->GetPositionX()) * std::cos(door->GetOrientation()) +
                             (middle.y - door->GetPositionY()) * std::sin(door->GetOrientation());
        if (offsets.size() > 64)
            offsets.clear();
        offsets[key] = offset;
        // [b36_ss] with the hits: a symmetric lattice (Smallportcullis.m2, +-1.34 yd) gives an honest 0.00 - the
        // faces' distances from the use point / far point and the bars' thickness along the walk show it.
        float const span =
            G3D::Vector3(pass.farX - pass.useX, pass.farY - pass.useY, pass.farZ - pass.useZ).length();
        LOG_INFO("playerbots",
                 "[b35_ss2] key gate {} {}: bars {:.2f} yd from the spawn plane (hits {:.2f} from the use point, "
                 "{:.2f} from the far point, {:.2f} yd thick)",
                 door->GetName(), door->GetEntry(), offset, nearHit, farHit, span - nearHit - farHit);
        return offset;
    }

    // Where a started event waits for the group before its waves come; false: no such spot on that map.
    bool EventGatherSpot(uint32 mapId, Position& spot)
    {
        if (mapId == HoRIds::HOR_MAP_ID)
        {
            spot = HoRIds::WAVE_GATHER_SPOT;  // instance_halls_of_reflection.cpp: all within 40 yd of the altar
            return true;
        }
        return false;
    }

    // The event is at the stage where the group belongs on the spot. HoR: from the Jaina / Sylvanas gossip until
    // Marwyn is dead - the group watches the intro at the altar, as players do; the waves start only with
    // everybody within 40 yd of it (instance_halls_of_reflection.cpp Update). Waiting for IsWaveEventActive
    // (intro persistent data) left the group at the entrance in wave 6: nobody ever walked to the altar.
    bool EventGatherReady(uint32 mapId, Player* leader)
    {
        if (mapId == HoRIds::HOR_MAP_ID)
        {
            InstanceScript* instance = leader->GetInstanceScript();
            return instance && instance->GetBossState(HoRIds::DATA_MARWYN) != DONE;
        }
        return true;
    }

    // [incomplete_route_deadend] DirectLineOverGround's test on a line from any point (a route link's own start).
    bool LineOverGround(Map* map, uint32 phaseMask, G3D::Vector3 const& from, G3D::Vector3 const& to)
    {
        float const length = (to - from).length();
        uint32 const steps = std::max<uint32>(1, uint32(length / DIRECT_STEP));
        for (uint32 step = 1; step <= steps; ++step)
        {
            G3D::Vector3 const point = from + (to - from) * (float(step) / float(steps));
            float const ground = map->GetHeight(phaseMask, point.x, point.y, point.z + 2.0f, true, 50.0f);
            if (ground <= INVALID_HEIGHT || point.z - ground > DIRECT_MAX_DROP)
                return false;
        }
        return true;
    }

    bool DirectLineOverGround(Player* bot, float x, float y, float z)
    {
        Map* map = bot->GetMap();
        float const length = bot->GetExactDist(x, y, z);
        uint32 const steps = std::max<uint32>(1, uint32(length / DIRECT_STEP));
        for (uint32 step = 1; step <= steps; ++step)
        {
            float const t = float(step) / float(steps);
            float const px = bot->GetPositionX() + (x - bot->GetPositionX()) * t;
            float const py = bot->GetPositionY() + (y - bot->GetPositionY()) * t;
            float const pz = bot->GetPositionZ() + (z - bot->GetPositionZ()) * t;
            float const ground = map->GetHeight(bot->GetPhaseMask(), px, py, pz + 2.0f, true, 50.0f);
            if (ground <= INVALID_HEIGHT || pz - ground > DIRECT_MAX_DROP)
                return false;
        }
        return true;
    }

    bool RefusalIsOverCap(std::string const& why)
    {
        static char const* const OVER_CAP[] = {"does not fit", "over the cap", "split search", "no safe pull",
                                               " mobs, over ", "over the guarantee's ceiling"};
        for (char const* const part : OVER_CAP)
            if (why.find(part) != std::string::npos)
                return true;
        return false;
    }

    bool IsDoorKey(DoorEventDef const& def, Creature const* creature)
    {
        return creature && creature->GetEntry() == def.keyEntry &&
               creature->GetHomePosition().GetExactDist2d(def.keyX, def.keyY) < DOOR_KEY_MATCH;
    }

    // The walk to `target` opens this closed door: the door is the target (a key used on it: Scholomance's
    // Viewing Room Door), or the target is the key creature of its DoorEventDef (the pull opens it).
    // 2026-10-25: or the door opens by use (TerrainHazard.h DoorOpensByUse: Scholomance's Iron Gates 175611-175618
    // and Door 175610, lockId 0, flags 32) and is no DoorEventDef door (the Utgarde Keep forge walls stay shut
    // until their Forge Master): the leader walks up and clicks it (LeaderDoorStop), for any target.
    bool DoorOpensFor(GameObject const* door, WorldObject const* target)
    {
        if (!door || !target)
            return false;
        if (target == door)
            return true;
        DoorEventDef const* def = DoorEventOf(door);
        if (!def && DoorOpensByUse(door))
            return true;
        return def && IsDoorKey(*def, target->ToCreature());
    }

    // [starter_chest_open] The opening spell a client casts on a locked chest: the lock's LOCK_KEY_SKILL case the
    // caster meets (its skill; SKILL_NONE for the generic types), then the first OPEN_LOCK spell of that lock type
    // the caster knows, or one that needs no spellbook entry (TARGET_FLAG_GAMEOBJECT_ITEM: HandleCastSpellOpcode
    // skips HasActiveSpell for it). UBRS Father Flame 175245, lock 93 = LOCKTYPE_OPEN_TINKERING 12 -> 6477
    // "Opening". 0: none.
    uint32 ChestOpeningSpell(Player* caster, GameObject const* go)
    {
        uint32 const lockId = go->GetGOInfo()->GetLockId();
        LockEntry const* lock = lockId ? sLockStore.LookupEntry(lockId) : nullptr;
        if (!lock)
            return 0;
        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (lock->Type[i] != LOCK_KEY_SKILL)
                continue;
            SkillType const skill = SkillByLockType(LockType(lock->Index[i]));
            if (skill != SKILL_NONE && caster->GetSkillValue(skill) < lock->Skill[i])
                continue;
            for (uint32 spellId : PlayerbotSpellRepository::Instance().GetOpeningSpells(lock->Index[i]))
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                    info && info->HasEffect(SPELL_EFFECT_OPEN_LOCK) &&
                    (caster->HasActiveSpell(spellId) || (info->Targets & TARGET_FLAG_GAMEOBJECT_ITEM)))
                    return spellId;
        }
        return 0;
    }

    // The closed doors a route to `target` must not run through (the ones that do not open for it).
    std::vector<GameObject*> DoorsClosedFor(std::vector<GameObject*> const& doors, WorldObject const* target)
    {
        std::vector<GameObject*> result;
        result.reserve(doors.size());
        for (GameObject* door : doors)
            if (!DoorOpensFor(door, target))
                result.push_back(door);
        return result;
    }

    std::unordered_map<uint32, ArthasMovingWait> arthasMovingWaits;  // instance id -> wait, world thread

    bool ArthasStarterMoving(Creature* creature)
    {
        MotionMaster* motion = creature->GetMotionMaster();
        MovementGeneratorType const active = motion ? motion->GetMotionSlotType(MOTION_SLOT_ACTIVE)
                                                    : IDLE_MOTION_TYPE;
        bool const moving = creature->isMoving() || active == POINT_MOTION_TYPE || active == ESCORT_MOTION_TYPE ||
                            (motion && motion->GetMotionSlotType(MOTION_SLOT_CONTROLLED) == POINT_MOTION_TYPE);
        uint32 const now = getMSTime();
        ArthasMovingWait& wait = arthasMovingWaits[creature->GetInstanceId()];
        if (!moving || !creature->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        {
            wait = ArthasMovingWait();
            return false;
        }
        if (!wait.sinceMs || getMSTimeDiff(wait.seenMs, now) > ARTHAS_STARTER_SEEN_GAP_MS)
        {
            wait.sinceMs = now ? now : 1;
            LOG_INFO("playerbots",
                     "[b33_cos2] [starter] Arthas {} still moves (motion {}, spline {}) with his gossip on: no "
                     "hello for up to {} s",
                     creature->GetInstanceId(), uint32(active), creature->isMoving() ? 1 : 0,
                     ARTHAS_STARTER_MOVING_WAIT_MS / IN_MILLISECONDS);
        }
        wait.seenMs = now ? now : 1;
        return getMSTimeDiff(wait.sinceMs, now) < ARTHAS_STARTER_MOVING_WAIT_MS;
    }

    bool WalksScriptedPath(Creature* creature)
    {
        if (creature->GetEntry() == CoSIds::NPC_ARTHAS_COS)  // [b33_cos2]
            return ArthasStarterMoving(creature);
        MotionMaster* motion = creature->GetMotionMaster();
        return motion && (motion->GetMotionSlotType(MOTION_SLOT_ACTIVE) == POINT_MOTION_TYPE ||
                          motion->GetMotionSlotType(MOTION_SLOT_CONTROLLED) == POINT_MOTION_TYPE);
    }

    bool AtLinkPoint(Player* bot, float x, float y, float z, float range)
    {
        return bot->GetExactDist2d(x, y) <= range && std::fabs(bot->GetPositionZ() - z) <= LINK_ARRIVE_HEIGHT;
    }

    // The 3D distance from the bot to the segment from the link's start to its far end.
    float LinkLineDistance(Player* bot, RouteLinkDef const& link)
    {
        G3D::Vector3 const from(link.fromX, link.fromY, link.fromZ);
        G3D::Vector3 const line = G3D::Vector3(link.toX, link.toY, link.toZ) - from;
        G3D::Vector3 const at(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        float const lengthSq = line.squaredLength();
        float const t = lengthSq > 0.0f ? std::clamp((at - from).dot(line) / lengthSq, 0.0f, 1.0f) : 0.0f;
        return (at - (from + line * t)).length();
    }

    // [w24b-under-end] Under the far end on another layer (Ahn'kahet 2026-10-25: 4.2 yd under the step onto the
    // Volazj ramp, 4.4 yd from the line, inside LINK_OFF_LINE_RANGE): off the line too, back to the start and over
    // the line again.
    bool LinkOffLine(Player* bot, RouteLinkDef const& link)
    {
        if (LinkLineDistance(bot, link) > LINK_OFF_LINE_RANGE)
            return true;
        if (bot->GetExactDist2d(link.toX, link.toY) > LINK_DONE_RANGE ||
            link.toZ - bot->GetPositionZ() <= LINK_ARRIVE_HEIGHT)
            return false;
        static uint32 underEndLogMs = 0;
        uint32 const now = getMSTime();
        if (!underEndLogMs || getMSTimeDiff(underEndLogMs, now) > 10 * IN_MILLISECONDS)
        {
            underEndLogMs = now ? now : 1;
            LOG_INFO("playerbots", "[w24b-under-end] {}: {:.1f} yd under the far end of {} - off the line",
                     bot->GetName(), link.toZ - bot->GetPositionZ(), link.note);
        }
        return true;
    }

    // A link over a lift (RouteLinkDef::goEntry of GAMEOBJECT_TYPE_TRANSPORT: The Mechanar's GO 183788).
    bool LinkIsLift(RouteLinkDef const& link)
    {
        GameObjectTemplate const* info = link.goEntry ? sObjectMgr->GetGameObjectTemplate(link.goEntry) : nullptr;
        return info && info->type == GAMEOBJECT_TYPE_TRANSPORT;
    }

    std::unordered_map<ObjectGuid, uint32> pullWalkLogMs;  // tank -> last "[approach] pull running" line

    bool TankPullRunning(Player* tank)
    {
        PlayerbotAI* botAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        if (!botAI)
            return false;
        // A pull the tank just gave up before the cast: the group holds where it stands for the re-plan.
        if (CarefulPlanVerdict::RecentlyAborted(tank->GetGUID(), PULL_ABORT_HOLD_MS))
            return true;
        // Pending or started within the pull's own time limit only: HasTarget() alone stayed set for whole runs
        // (build 12: 109 "no harness waypoint" lines across all runs, vh-n Moragg not engaged for 2 min).
        if (PullStrategy const* pull = PullStrategy::Get(botAI);
            pull && (pull->IsPullPendingToStart() ||
                     (pull->HasPullStarted() &&
                      time(nullptr) - pull->GetPullStartTime() <= PullStrategy::GetMaxPullTime())))
            return true;
        LosPull::Plan plan;
        return LosPull::Get(tank, plan) && plan.tank == tank->GetGUID() && !plan.released;
    }

    std::unordered_map<ObjectGuid, PullHoldTrack> pullHolds;  // leader -> its current pull hold (world thread)

    bool PullHoldReleased(ObjectGuid leader)
    {
        auto const it = pullHolds.find(leader);
        return it != pullHolds.end() && it->second.releaseUntilMs &&
               int32(it->second.releaseUntilMs - getMSTime()) > 0;
    }

    // A jump link lands as a fall does (a client's MSG_MOVE_FALL_LAND): Player::HandleFall from the start's
    // height. MoveJump is a controlled move, PlayerbotAI::UpdateFall never sees it. No damage in water.
    void LandLinkJump(Player* bot, float fromZ)
    {
        LiquidData const& liquid = bot->GetLiquidData();
        if ((liquid.Status & MAP_LIQUID_STATUS_SWIMMING) &&
            liquid.Level - bot->GetPositionZ() >= bot->GetCollisionHeight() * 0.5f)
            return;
        MovementInfo landing = bot->m_movementInfo;
        landing.pos.Relocate(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        bot->SetFallInformation(0, fromZ);
        bot->HandleFall(landing);
        bot->SetFallInformation(0, bot->GetPositionZ());
    }

    bool LeaderWalkStands(LeaderGroup& group, Player* leader, Position& seenAt, uint32& seenSerial)
    {
        LeaderGroup::WalkPoint walk;
        uint32 const serial = group.GetWaypoint(group.LeaderGuid(), walk) ? walk.serial + 1 : 0;  // 0: no walk point
        if (leader->isMoving() || leader->IsNonMeleeSpellCast(false))
        {
            seenSerial = 0;
            seenAt = Position();
            return false;
        }
        bool const same = serial && seenSerial == serial && seenAt.GetExactDist(leader->GetPosition()) <= 0.5f &&
                          (seenAt.GetPositionX() != 0.0f || seenAt.GetPositionY() != 0.0f);
        seenSerial = serial;
        seenAt = leader->GetPosition();
        return same;
    }

    bool LeaderOwnWalkStalled(LeaderGroup& group, Player* leader, G3D::Vector3 const& goal)
    {
        GroupRouteState& routeState = group.Route();
        if (leader->IsInCombat() || leader->GetVehicle() || leader->IsInFlight() || leader->Unit::IsInWater())
            return false;

        uint32 const now = getMSTime();
        G3D::Vector3 const lastGoal(routeState.driveGoal.GetPositionX(), routeState.driveGoal.GetPositionY(),
                                    routeState.driveGoal.GetPositionZ());
        // [lead134] anew after a fight between the looks (not STEER_GAP_MS)
        bool const fightBetween = routeState.driveLookPull != group.PullSeq();
        routeState.driveLookPull = group.PullSeq();
        if (!routeState.driveLastMs || (lastGoal - goal).length() > DRIVE_SAME_GOAL || fightBetween)
        {
            routeState.driveGoal.Relocate(goal.x, goal.y, goal.z);
            routeState.driveAnchor = leader->GetPosition();
            routeState.driveSinceMs = now;
            routeState.driving = false;
        }
        routeState.driveLastMs = now;
        if (leader->GetExactDist(&routeState.driveAnchor) > STEER_PROGRESS_RANGE)
        {
            routeState.driveAnchor = leader->GetPosition();
            routeState.driveSinceMs = now;
        }

        // [lead130] user: no elapsed-time rule decides (DRIVE_AFTER_MS 6 s without progress was the old clock): the
        // leader's own walk stalled when it stands on the same walk point at the same spot as at the last look
        if (routeState.driving || routeState.driveSinceMs == now ||
            !LeaderWalkStands(group, leader, routeState.driveStandAt, routeState.driveStandSerial))
            return false;
        routeState.driving = true;
        return true;
    }

    bool PatrolWaitOn(LeaderGroup& group)
    {
        GroupRouteState& routeState = group.Route();
        if (!routeState.patrolWaitUntilMs || routeState.patrolWaitTarget != routeState.target)
            return false;
        bool hopeless = false;
        uint32 waitMs = 0;
        std::string why;
        return PartyRefusal(group, routeState.target, hopeless, waitMs, why) &&
               why.find("patrol wait") != std::string::npos;
    }

    bool PartyRefusal(LeaderGroup& group, ObjectGuid mob, bool& hopeless, uint32& waitMs, std::string& why)
    {
        if (!LeaderPicksFor(group))
            return CarefulPlanVerdict::GetRefusal(group.LeaderGuid(), mob, hopeless, waitMs, why);
        if (group.State().verdictAccepted || !LeaderVerdictCovers(group, mob))
            return false;
        hopeless = group.State().verdictHopeless;
        uint32 const age = getMSTimeDiff(group.State().verdictMs, getMSTime());
        waitMs = group.State().verdictWaitMs > age ? group.State().verdictWaitMs - age : 0;
        why = group.State().verdictWhy;
        return true;
    }

    // [b32_pos] A progress step (EventSpotDef::progressData) that was reached (eventSpotState 1, not 2 "no path")
    // and whose instance progress is still below progressMin: the group stays and its trigger re-fires.
    bool ProgressStepPending(LeaderGroup& group, EventSpotDef const& spot)
    {
        GroupRouteState& routeState = group.Route();
        if (spot.progressData < 0 || !group.HasContent())
            return false;
        std::vector<EventSpotDef const*> const spots = EventSpots(group.MapId());
        auto const it = std::find(spots.begin(), spots.end(), &spot);
        if (it == spots.end())
            return false;
        auto const state = routeState.eventSpotState.find(size_t(it - spots.begin()));
        if (state == routeState.eventSpotState.end() || state->second != 1)
            return false;
        InstanceScript* script = group.InstanceScript();
        return script && script->GetData(uint32(spot.progressData)) < spot.progressMin;
    }

    // [brd_ring_hold] A spot that holds the group now: a hold spot, or a step whose event runs
    // (EventSpotDef::eventData IN_PROGRESS) or waits for its retry after a FAIL (Party::eventSpotRetry,
    // WatchEventState).
    bool SpotHolds(LeaderGroup& group, EventSpotDef const& spot)
    {
        GroupRouteState& routeState = group.Route();
        if (spot.hold)
            return true;
        // [b32_pos] EventSpotDef::progressData: a reached step holds until the instance progress got there.
        if (spot.progressData >= 0)
            return ProgressStepPending(group, spot);
        if (spot.eventData < 0)
            return false;
        if (routeState.eventSpotRetry)
            return true;
        Player* leader = ObjectAccessor::FindPlayer(group.LeaderGuid());
        Map* map = leader ? leader->GetMap() : nullptr;
        InstanceMap* instance = map && map->GetInstanceId() == group.InstanceId() ? map->ToInstanceMap() : nullptr;
        InstanceScript* script = instance ? instance->GetInstanceScript() : nullptr;
        return script && script->GetData(uint32(spot.eventData)) == IN_PROGRESS;
    }

    // Over the link as a player walks it: a straight walk (no navmesh on a game-object bridge). Bots walk by
    // themselves: the far end is the bot's waypoint, its own "test runner move" walks straight where the navmesh
    // ends (MovementAction::MoveStraight).
    void StartLinkCross(LeaderGroup& group, Player* bot, RouteLinkDef const& link)
    {
        // A walked link (not a jump, not a lift): the line itself, straight (TestRunner::Waypoint::straight).
        group.GiveWaypoint(bot, link.toX, link.toY, link.toZ, link.jump, !link.jump && !LinkIsLift(link));
    }

    // The leader's waypoint is withheld for its own running pull (GiveWaypoint's ROUTE_NO_WALK_IN_PULL rule).
    // [lead2] LEADER_ONE_TANK_MOVER (C:/WoW/LEADER_REFACTOR_PLAN.md step 2): the tank's open pull order (Leader::
    // Orders) is the one owner of its movement, a boss target's too; the leader gets the tank back on the order's
    // events only (the tank's Aborted, the order's fight over, the tank gone, order_orphan). TankPullRunning's
    // clocks (GetMaxPullTime, the RecentlyAborted window) and the PULL_HOLD_STALL_MS / PULL_HOLD_RELEASE_MS
    // release are the A/B's old path. Violet Hold's released prisoners stay the harness's walk in both.
    bool WaypointHeldForPull(LeaderGroup& group, Player* bot)
    {
        GroupRouteState& routeState = group.Route();
        if (bot->GetGUID() != group.LeaderGuid() || bot->GetMapId() == VIOLET_HOLD_MAP_ID)
            return false;
        if (Leader::LEADER_ONE_TANK_MOVER)
        {
            Leader::PullOrder order;
            return Leader::Orders::GetOpen(group.LeaderGuid(), order) &&
                   (Leader::LEADER_ENGAGE_OWNS_MOVEMENT || order.method != Leader::PullMethod::Engage);
        }
        return ROUTE_NO_WALK_IN_PULL && !routeState.targetIsBoss && !PullHoldReleased(bot->GetGUID()) &&
               TankPullRunning(bot);
    }

    bool CanOpenLock(LeaderGroup& group, GameObject const* go)
    {
        uint32 const lockId = go->GetGOInfo()->GetLockId();
        LockEntry const* lock = lockId ? sLockStore.LookupEntry(lockId) : nullptr;
        if (!lock)
            return true;

        // A lock opens with any one of its cases (Spell::CanOpenLock): the key item, or the skill of its type.
        bool anyCase = false;
        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (lock->Type[i] == LOCK_KEY_ITEM && lock->Index[i])
            {
                anyCase = true;
                for (LeaderGroup::MemberInfo const& member : group.Members())
                    if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                        if (bot->HasItemCount(lock->Index[i], 1))
                            return true;
            }
            else if (lock->Type[i] == LOCK_KEY_SKILL)
            {
                anyCase = true;
                SkillType const skill = SkillByLockType(LockType(lock->Index[i]));
                if (skill == SKILL_NONE)
                    return true;  // generic open / close: anybody can
                for (LeaderGroup::MemberInfo const& member : group.Members())
                    if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                        if (bot->GetSkillValue(skill) >= lock->Skill[i])
                            return true;
            }
        }
        return !anyCase;
    }

    bool EntriesDead(LeaderGroup& group, std::vector<uint32> const& entries)
    {
        GroupRouteState& routeState = group.Route();
        for (uint32 entry : entries)
        {
            if (routeState.deadEntries.count(entry))
                continue;
            bool done = group.BossKilledFor(entry);
            if (!done)
                return false;
        }
        return true;
    }

    Creature* ActiveEscort(LeaderGroup& group, Map* map)
    {
        GroupRouteState& routeState = group.Route();
        if (routeState.escort.IsEmpty() || !map)
            return nullptr;

        uint32 const now = getMSTime();
        Creature* npc = map->GetCreature(routeState.escort);
        char const* end = nullptr;
        if (!npc || !npc->IsInWorld())
            end = "gone";
        else if (!npc->IsAlive())
            end = "died";
        else if (getMSTimeDiff(routeState.escortSinceMs, now) > ESCORT_MAX_MS)
            end = "timeout";
        else if (npc->IsInCombat() || npc->isMoving() || npc->GetExactDist(&routeState.escortLastPos) > 2.0f)
        {
            routeState.escortIdleSinceMs = now;
            routeState.escortLastPos = npc->GetPosition();
        }
        else if (getMSTimeDiff(routeState.escortIdleSinceMs, now) > ESCORT_IDLE_MS)
            end = "idle";

        // [event_boss_tbcwotlk] escort_stalled: in combat and not 5 yd off the same spot for a minute - who holds it
        // (an npc_escortAI walks no waypoint while in combat; ohb-h 20261025-163739 Thrall 10 min at 2207,121,89).
        if (!end && npc)
        {
            if (!routeState.escortStillSinceMs || npc->GetExactDist(&routeState.escortStillPos) > 5.0f)
            {
                routeState.escortStillSinceMs = now;
                routeState.escortStillPos = npc->GetPosition();
            }
            else if (npc->IsInCombat() &&
                     getMSTimeDiff(routeState.escortStillSinceMs, now) > MINUTE * IN_MILLISECONDS &&
                     (!routeState.escortStallLogMs ||
                      getMSTimeDiff(routeState.escortStallLogMs, now) > MINUTE * IN_MILLISECONDS))
            {
                routeState.escortStallLogMs = now;
                JsonArray attackers;
                for (Unit* attacker : npc->getAttackers())
                    if (attacker)
                        attackers.Raw(JsonObject().Str("name", attacker->GetName()).Num("entry", attacker->GetEntry())
                                          .Real("dist", npc->GetExactDist(attacker))
                                          .Real("dz", attacker->GetPositionZ() - npc->GetPositionZ())
                                          .Bool("onNpc", attacker->GetVictim() == npc)
                                          .Bool("evading", attacker->ToCreature() &&
                                                               attacker->ToCreature()->IsInEvadeMode())
                                          .Build());
                Unit* victim = npc->GetVictim();
                group.LogEvent("escort_stalled",
                         JsonObject().Str("note", routeState.escortNote).Num("entry", npc->GetEntry())
                             .Num("stillSec", getMSTimeDiff(routeState.escortStillSinceMs, now) / IN_MILLISECONDS)
                             .Real("hpPct", npc->GetHealthPct()).Str("victim", victim ? victim->GetName() : "")
                             .Real("victimDist", victim ? npc->GetExactDist(victim) : -1.0f)
                             .Num("threat", int64(npc->GetThreatMgr().GetThreatListSize()))
                             .Raw("attackers", attackers.Build())
                             .Real("x", npc->GetPositionX()).Real("y", npc->GetPositionY())
                             .Real("z", npc->GetPositionZ()));
            }
        }

        if (!end)
            return npc;

        group.LogEvent("escort_end", JsonObject().Str("note", routeState.escortNote)
                                          .Num("entry", routeState.escortEntry)
                                          .Str("reason", end)
                                          .Num("durationSec", getMSTimeDiff(routeState.escortSinceMs, now) /
                                                                  IN_MILLISECONDS));
        if (strcmp(end, "idle") == 0 && npc)
        {
            routeState.escortIdleEndGuid = npc->GetGUID();
            routeState.escortIdleEndPos = npc->GetPosition();
        }
        // [ohb-escort-death-rearm] It died: the instance brings it back at its last checkpoint with its gossip (Old
        // Hillsbrad: Thrall; ohb-h 062726 burned all 8 uses on five deaths, Epoch Hunter "event not started"). The use
        // is given back: the starter walks the group to it and talks again.
        if (strcmp(end, "died") == 0)
            for (StarterTrack& starter : routeState.starters)
            {
                EventStarterDef const* def = starter.def;
                uint32 const entry = def->escortEntry ? def->escortEntry : def->entries.front();
                if (def->kind != StarterKind::Escort || entry != routeState.escortEntry || !starter.uses ||
                    starter.escortDeathRearms >= ESCORT_DEATH_REARM_MAX)
                    continue;
                ++starter.escortDeathRearms;
                --starter.uses;
                starter.nextAllowedMs = now;
                group.LogEvent("escort_rearm_after_death", JsonObject().Str("note", def->note)
                                                                .Num("uses", starter.uses)
                                                                .Num("rearms", starter.escortDeathRearms));
                break;
            }
        routeState.escort.Clear();
        routeState.escortEntry = 0;
        routeState.escortStillSinceMs = 0;  // [event_boss_tbcwotlk]
        routeState.escortStallLogMs = 0;
        return nullptr;
    }

    bool StayWithEscort(LeaderGroup& group, Player* leader)
    {
        GroupRouteState& routeState = group.Route();
        Map* map = leader->GetMap();
        if (!map || map->GetInstanceId() != group.InstanceId() || !leader->IsAlive())
            return false;

        Creature* npc = ActiveEscort(group, map);
        if (!npc || leader->GetExactDist(npc) <= ESCORT_FOLLOW_RANGE)
            return false;

        PathResult leg;
        G3D::Vector3 const goal(npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ());
        SteerLeg(leader, goal, leg);
        if (leg.type & PATHFIND_NOPATH)
            return false;

        G3D::Vector3 const waypoint =
            FloorWaypoint(leader, leg, RouteWaypoint(leg, goal, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP));

        group.SetLeaderWaypoint(leader, waypoint.x, waypoint.y, waypoint.z);
        group.NoteProgress(getMSTime());
        routeState.lastEvent = "escort " + routeState.escortNote;
        return true;
    }

    int32 EventSpotIndex(LeaderGroup& group)
    {
        GroupRouteState& routeState = group.Route();
        if (!group.HasContent())
            return -1;

        std::vector<EventSpotDef const*> const spots = EventSpots(group.MapId());
        Player const* leader = ObjectAccessor::FindPlayer(group.LeaderGuid());
        for (size_t index = 0; index < spots.size(); ++index)
        {
            if (leader && (leader->GetPositionZ() < spots[index]->leaderMinZ ||
                           leader->GetPositionZ() > spots[index]->leaderMaxZ))
                continue;
            // No path there; or a step of a walk already reached (the next one follows).
            auto const state = routeState.eventSpotState.find(index);
            // [brd_ring_hold] a reached step stays the spot while its event runs (SpotHolds: EventSpotDef::eventData)
            if (state != routeState.eventSpotState.end() && (state->second == 2 || !SpotHolds(group, *spots[index])))
                continue;
            // EventSpotDef::afterUsed: a hold that begins with the use of its starter (Urok's pile, Kirtonos' brazier)
            if (EntriesDead(group, spots[index]->afterDead) && StartersUsed(group, spots[index]->afterUsed) &&
                (spots[index]->untilDead.empty() || !EntriesDead(group, spots[index]->untilDead)))
                return int32(index);
        }
        return -1;
    }

    bool WalkToEventSpot(LeaderGroup& group, Player* leader)
    {
        GroupRouteState& routeState = group.Route();
        Map* map = leader->GetMap();
        int32 const index = EventSpotIndex(group);
        if (!map || map->GetInstanceId() != group.InstanceId() || !leader->IsAlive() || index < 0)
            return false;

        EventSpotDef const* spot = EventSpots(group.MapId())[size_t(index)];
        Position const pos(spot->x, spot->y, spot->z);
        float const distance = leader->GetExactDist(&pos);
        // 2D with a height tolerance: an areatrigger centre (Pit of Saron tunnel) can float above the floor.
        if (leader->GetExactDist2d(spot->x, spot->y) <= EVENT_GATHER_ARRIVE &&
            std::fabs(leader->GetPositionZ() - spot->z) <= EVENT_SPOT_HEIGHT)
        {
            if (!routeState.eventSpotState.count(size_t(index)))
            {
                // A step is done here (the next one applies from the next tick); a hold spot keeps the group.
                routeState.eventSpotState[size_t(index)] = 1;
                routeState.lastEvent = std::string("event spot: ") + spot->note;
                group.LogEvent("event_spot", JsonObject().Str("state", "arrived").Str("note", spot->note)
                                                  .Real("x", spot->x).Real("y", spot->y).Real("z", spot->z));
            }
            return false;
        }

        PathResult leg;
        G3D::Vector3 const goal(spot->x, spot->y, spot->z);
        SteerLeg(leader, goal, leg);
        if (leg.type & PATHFIND_NOPATH)
        {
            routeState.eventSpotState[size_t(index)] = 2;
            group.LogEvent("event_spot", JsonObject().Str("state", "no path").Str("note", spot->note)
                                              .Real("distance", distance));
            return false;
        }

        G3D::Vector3 const waypoint =
            FloorWaypoint(leader, leg, RouteWaypoint(leg, goal, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP));

        if (routeState.lastEvent.rfind("event spot", 0) != 0)
        {
            routeState.lastEvent = std::string("event spot walk: ") + spot->note;
            group.LogEvent("event_spot", JsonObject().Str("state", "walking").Str("note", spot->note)
                                              .Real("distance", distance));
        }

        // The leader's own move refuses the way (Azjol-Nerub wave 10: the shaft ledge joins the hole's floor only
        // over steep slopes): logged as the bot's gap. [lead2] the dead harness drive walk is gone.
        if (LeaderOwnWalkStalled(group, leader, goal))
            group.LogCannotMove(leader, "event spot",
                          std::string("own walk made no progress for 6 s towards ") + spot->note, waypoint.x,
                          waypoint.y, waypoint.z);

        group.SetLeaderWaypoint(leader, waypoint.x, waypoint.y, waypoint.z);
        group.NoteProgress(getMSTime());
        return true;
    }

    bool LinkUsable(LeaderGroup& group, Map* map, RouteLinkDef const& link)
    {
        if (!EntriesDead(group, link.afterDead))
            return false;
        if (!link.goEntry)
            return true;
        // A lift is a StaticTransport, kept in the map's transports (not in the grid / spawn store); it runs on its
        // own cycle, there is no state to wait for.
        if (LinkIsLift(link))
        {
            for (Transport* transport : map->GetAllTransports())
                if (transport && transport->IsInWorld() && transport->GetEntry() == link.goEntry)
                    return true;
            return false;
        }
        for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
            if (go && go->GetEntry() == link.goEntry && go->isSpawned() && uint32(go->GetGoState()) == link.goState)
                return true;
        return false;
    }

    int32 LinkTo(LeaderGroup& group, Player* leader, WorldObject* target)
    {
        GroupRouteState& routeState = group.Route();
        Map* map = leader->GetMap();
        if (!map || !target || !group.HasContent())
            return -1;

        std::vector<RouteLinkDef const*> const links = RouteLinks(group.MapId());
        // [b30_wings] The current link is judged first and kept while it still holds: in index order the first link
        // that passed won each call, and wc-h 20261025-184229-wc-h-5A-17 (idle 208 s) flipped "chosen" between the
        // jump onto Serpentis' / Verdan's ledges and the drop back down, both "for Disciple of Naralex", every 2-10 s.
        constexpr uint32 LINK_REVERSE_MS = 3 * MINUTE * IN_MILLISECONDS;
        uint32 const nowMs = getMSTime();
        std::vector<size_t> order;
        if (routeState.linkIdx >= 0 && size_t(routeState.linkIdx) < links.size())
            order.push_back(size_t(routeState.linkIdx));
        for (size_t index = 0; index < links.size(); ++index)
            if (int32(index) != routeState.linkIdx)
                order.push_back(index);
        for (size_t const index : order)
        {
            RouteLinkDef const& link = *links[index];
            if (!LinkUsable(group, map, link))
                continue;
            if (auto const fails = routeState.linkFails.find(int32(index));
                fails != routeState.linkFails.end() && fails->second >= LINK_MAX_FAILS)
                continue;
            // The leader already over it (the link of an earlier boss), or no walk to its start, or nothing walkable
            // from its far end to the target.
            if (leader->GetExactDist(link.toX, link.toY, link.toZ) <= LINK_DONE_RANGE)
                continue;
            // [b30_wings] Directional: a far end the leader walks to without the link is on its own side already -
            // the link leads nowhere new, crossed it leads back (wc-h: on the ledges, the crack jump's far end is the
            // ledge itself; its start counts as "reachable" only as a partial route ending at the crack). A straight
            // line (swimming, no navmesh) says nothing about the side, nor does a walk through a closed door the
            // link is the way round (the mmaps hold no doors).
            if (PathResult const toFar = RoutePath(leader, link.toX, link.toY, link.toZ);
                toFar.complete && !(toFar.type & (PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH)))
            {
                std::vector<GameObject*> const doors =
                    ROUTE_CLOSED_DOORS_ALL ? DoorsClosedFor(ClosedDoors(map, leader), target)
                                           : std::vector<GameObject*>();
                if (doors.empty() || !RouteThroughClosedDoor(toFar.points, doors, leader->GetPhaseMask()))
                    continue;
            }
            // [b30_wings] No immediate reverse: for the target the leader just crossed a link for, no other link
            // whose far end lies farther from that target than the crossed one's.
            if (routeState.lastLinkIdx >= 0 && routeState.lastLinkIdx != int32(index) &&
                size_t(routeState.lastLinkIdx) < links.size() && routeState.lastLinkFor == target->GetGUID() &&
                getMSTimeDiff(routeState.lastLinkMs, nowMs) <= LINK_REVERSE_MS)
            {
                RouteLinkDef const& last = *links[size_t(routeState.lastLinkIdx)];
                if (target->GetExactDist(link.toX, link.toY, link.toZ) >
                    target->GetExactDist(last.toX, last.toY, last.toZ))
                    continue;
            }
            PathResult const toStart = RoutePath(leader, link.fromX, link.fromY, link.fromZ);
            if (!toStart.reachable)
                continue;
            // [b34_mech3] The walk to the start must get there: a partial route that only "ends nearer" (RoutePath)
            // is no way onto a link, it may end on an island beside the far end's own part. mech-n
            // 20261026-014521-mech-n-5D-17 (1318 s): a short step left the leader on a 5-polygon island at the upper
            // level's edge (232.6,-37.4,25.4; its main part 6.9 yd away), no complete walk to the top landing from
            // there, so the lift was "not crossed yet" and its bottom start 24 yd below "reachable" (the partial route
            // ended on the island, 91 yd short). The walk to it climbed onto a pipe and stood 230 s at its end.
            if (!toStart.complete)
            {
                G3D::Vector3 const linkStart(link.fromX, link.fromY, link.fromZ);
                if ((toStart.end - linkStart).length() > GAP_REACHABLE_LEFT ||
                    std::fabs(toStart.end.z - linkStart.z) > GAP_REACHABLE_DZ)
                    continue;
            }
            G3D::Vector3 const farEnd(link.toX, link.toY, link.toZ);
            PathResult const overLink =
                RoutePath(leader, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), &farEnd);
            if (!overLink.reachable)
                continue;
            // Neither leg through a closed door that does not open for the target (a link is the way round one).
            if (ROUTE_CLOSED_DOORS_ALL)
                if (std::vector<GameObject*> const doors = DoorsClosedFor(ClosedDoors(map, leader), target);
                    !doors.empty() && (RouteThroughClosedDoor(toStart.points, doors, leader->GetPhaseMask()) ||
                                       RouteThroughClosedDoor(overLink.points, doors, leader->GetPhaseMask())))
                    continue;

            if (routeState.linkIdx != int32(index))
            {
                routeState.linkIdx = int32(index);
                routeState.linkSinceMs = getMSTime();
                routeState.linkLeaderCrossed = false;
                routeState.linkWalkLogged = false;
                routeState.linkCrossed.clear();
                group.LogEvent("route_link", JsonObject().Str("state", "chosen").Str("note", link.note)
                                                  .Str("for", target->GetName())
                                                  .Real("distance", leader->GetExactDist(link.fromX, link.fromY,
                                                                                         link.fromZ)));
            }
            routeState.linkFor = target->GetGUID();  // [b30_wings]
            return int32(index);
        }
        return -1;
    }

    void EndLink(LeaderGroup& group, char const* reason)
    {
        GroupRouteState& routeState = group.Route();
        if (routeState.linkIdx < 0)
            return;
        std::vector<RouteLinkDef const*> const links = RouteLinks(group.MapId());
        char const* note = size_t(routeState.linkIdx) < links.size() ? links[size_t(routeState.linkIdx)]->note : "?";
        uint32 const fails = std::strcmp(reason, "done") == 0 ? 0 : ++routeState.linkFails[routeState.linkIdx];
        group.LogEvent("route_link", JsonObject().Str("state", reason).Str("note", note)
                                          .Num("crossed", int64(routeState.linkCrossed.size()))
                                          .Num("fails", fails).Bool("givenUp", fails >= LINK_MAX_FAILS)
                                          .Num("durationSec", getMSTimeDiff(routeState.linkSinceMs, getMSTime()) /
                                                                  IN_MILLISECONDS));
        routeState.linkIdx = -1;
        routeState.linkLeaderCrossed = false;
        routeState.linkCrossed.clear();
        // The members' waypoints over the link (bots walk by themselves) end with it.
        group.ClearMemberIntent();
        // The other side is in reach now: what was unreachable from here is checked again.
        for (auto const& [guid, from] : routeState.unreachableFrom)
            routeState.skipUntil.erase(guid);
        routeState.unreachableFrom.clear();
        routeState.nextRetargetMs = getMSTime();
    }

    bool WalkToLink(LeaderGroup& group, Player* leader)
    {
        GroupRouteState& routeState = group.Route();
        Map* map = leader->GetMap();
        if (routeState.linkIdx < 0 || !map ||
            map->GetInstanceId() != group.InstanceId() || !leader->IsAlive())
            return false;

        std::vector<RouteLinkDef const*> const links = RouteLinks(group.MapId());
        if (size_t(routeState.linkIdx) >= links.size())
        {
            routeState.linkIdx = -1;
            return false;
        }
        RouteLinkDef const& link = *links[size_t(routeState.linkIdx)];
        uint32 const now = getMSTime();
        // Over it on its own feet: the far end stays the leader's waypoint until it stands there.
        if (routeState.linkLeaderCrossed)
        {
            if (!routeState.linkCrossLogMs || getMSTimeDiff(routeState.linkCrossLogMs, now) >= LINK_CROSS_LOG_MS)
            {
                routeState.linkCrossLogMs = now ? now : 1;
                LeaderGroup::WalkPoint current;
                bool const hasWaypoint = group.GetWaypoint(group.LeaderGuid(), current);
                group.LogEvent("link_cross",
                         JsonObject().Str("note", link.note).Num("sec", getMSTimeDiff(routeState.linkSinceMs, now) /
                                                                            IN_MILLISECONDS)
                             .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                             .Real("z", leader->GetPositionZ())
                             .Real("toEnd", leader->GetExactDist(link.toX, link.toY, link.toZ))
                             .Real("fromStart", leader->GetExactDist(link.fromX, link.fromY, link.fromZ))
                             .Bool("moving", leader->isMoving()).Num("motion",
                                                                     uint32(leader->GetMotionMaster()
                                                                                ->GetCurrentMovementGeneratorType()))
                             .Bool("waypoint", hasWaypoint).Bool("inCombat", leader->IsInCombat()));
            }
            if (AtLinkPoint(leader, link.toX, link.toY, link.toZ, LINK_DONE_RANGE))
                return false;
            // A jump keeps the jump waypoint StartLinkCross gave (a walk to the far end goes round through the closed
            // door); given again while the leader still stands at the start without it.
            if (link.jump)
            {
                bool const airborne =
                    leader->GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE ||
                    leader->HasUnitMovementFlag(MOVEMENTFLAG_FALLING);
                LeaderGroup::WalkPoint current;
                if (!airborne && AtLinkPoint(leader, link.fromX, link.fromY, link.fromZ, LINK_ARRIVE_RANGE) &&
                    (!group.GetWaypoint(group.LeaderGuid(), current) || !current.jump))
                    StartLinkCross(group, leader, link);
                if (airborne)
                    group.NoteProgress(now);
                return true;
            }
            // LINK_OFF_LINE_BACK: a leader off the line (its own path took another level: the Underbog root, wave 21,
            // the floor 23 yd under the far end) walks back to the start and crosses again from there.
            if (!LinkIsLift(link) && LinkOffLine(leader, link))
            {
                // [queue-link-off-line-cap] the same walk off the line again and again is no crossing: ub-n
                // 20261025-132016-ub-n-5A-48 left the root's line 36 times in 224 s (1359-1583 s, own path to the
                // swamp floor under the far end), gd-h 20261025-115805-gd-h-5A-45 47 times (the moat); the link was
                // chosen again after its 4-minute timeout. LINK_OFF_LINE_MAX_RETURNS returns end it as a failure
                // (LINK_MAX_FAILS gives it up).
                constexpr uint32 LINK_OFF_LINE_MAX_RETURNS = 4;
                if (!routeState.linkOffLineNow)
                {
                    routeState.linkOffLineNow = true;
                    if (++routeState.linkOffLineReturns > LINK_OFF_LINE_MAX_RETURNS)
                    {
                        LOG_INFO("playerbots", "[queue-link-off-line-cap] {}: {} off the line of {} {} times - ended",
                                 group.Run().runId, leader->GetName(), link.note, routeState.linkOffLineReturns);
                        group.ClearWaypoint(group.LeaderGuid());
                        EndLink(group, "off the line");
                        return false;
                    }
                }
                if (!routeState.linkOffLineLogMs ||
                    getMSTimeDiff(routeState.linkOffLineLogMs, now) >= LINK_CROSS_LOG_MS)
                {
                    routeState.linkOffLineLogMs = now ? now : 1;
                    LOG_INFO("playerbots",
                             "[route] {}: link_off_line: {} {:.1f} yd off the line of {} - back to its start",
                             group.Run().runId,
                             leader->GetName(), LinkLineDistance(leader, link), link.note);
                    group.LogEvent("link_off_line",
                             JsonObject().Str("note", link.note).Str("name", leader->GetName())
                                 .Num("returns", routeState.linkOffLineReturns)
                                 .Real("offLine", LinkLineDistance(leader, link))
                                 .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                 .Real("z", leader->GetPositionZ()));
                }
                // Back on the line near its start, the next tick gives the far end again (straight).
                group.SetLeaderWaypoint(leader, link.fromX, link.fromY, link.fromZ);
                if (leader->isMoving())
                    group.NoteProgress(now);
                return true;
            }
            routeState.linkOffLineNow = false;
            group.SetLeaderWaypoint(leader, link.toX, link.toY, link.toZ, !LinkIsLift(link));
            if (leader->isMoving())
                group.NoteProgress(now);
            return true;
        }
        if (!LinkUsable(group, map, link))
        {
            EndLink(group, "no longer usable");
            return false;
        }
        float const toStart = leader->GetExactDist(link.fromX, link.fromY, link.fromZ);
        if (!routeState.linkProgressMs || int32(routeState.linkProgressMs - routeState.linkSinceMs) < 0 ||
            leader->IsInCombat() ||
            toStart < routeState.linkBestDist - LINK_PROGRESS_STEP)
        {
            routeState.linkProgressMs = now ? now : 1;
            routeState.linkBestDist = toStart;
        }
        if (getMSTimeDiff(routeState.linkProgressMs, now) > LINK_MAX_MS ||
            getMSTimeDiff(routeState.linkSinceMs, now) > LINK_WALK_MAX_MS)
        {
            EndLink(group, "timeout");
            return false;
        }

        if (AtLinkPoint(leader, link.fromX, link.fromY, link.fromZ, LINK_ARRIVE_RANGE))
        {
            group.ClearWaypoint(group.LeaderGuid());
            // Over it together: every living member near the leader, nobody fighting.
            for (LeaderGroup::MemberInfo const& member : group.Members())
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                    if (bot->IsAlive() && (bot->GetMap() != map || bot->GetExactDist(leader) > LINK_GATHER_RANGE ||
                                           bot->IsInCombat()))
                        return true;

            // The link must carry the walk: ground (the bridge's collision) under every 2 yd of the line. A lift has
            // none on the line: every member gets the far end as its goal, the dungeon strategy rides it there.
            // [incomplete_route_deadend] The probe from where the leader stands is no verdict on the link: AtLinkPoint
            // takes 3 yd round the start, and the line from there can leave a narrow ramp. Underbog heroic build 30
            // (20261025-184229-ub-h-5F-63, 19:07:07): the leader arrived at ~222,-196 (the root ramp is x 224-228),
            // "no ground on the link" twice in 1 s, the link was given up - build 29 crossed the same link from
            // 224.6,-194.0. The line from the link's own start decides; a leader off it steps onto the start first
            // (LINK_EXACT_START, LINK_EXACT_MAX_MS at most), then crosses from there.
            if (!LinkIsLift(link) && !link.jump && !DirectLineOverGround(leader, link.toX, link.toY, link.toZ))
            {
                constexpr float LINK_EXACT_START = 1.0f;
                constexpr uint32 LINK_EXACT_MAX_MS = 10 * IN_MILLISECONDS;
                static std::unordered_map<ObjectGuid, uint32> exactStartSinceMs;  // per leader, world thread only
                G3D::Vector3 const linkStart(link.fromX, link.fromY, link.fromZ);
                G3D::Vector3 const linkEnd(link.toX, link.toY, link.toZ);
                if (!LineOverGround(map, leader->GetPhaseMask(), linkStart, linkEnd))
                {
                    exactStartSinceMs.erase(group.LeaderGuid());
                    group.LogEvent("route_link_probe",
                             JsonObject().Str("note", link.note).Str("verdict", "no ground from the link start")
                                 .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                 .Real("z", leader->GetPositionZ()));
                    EndLink(group, "no ground on the link");
                    return false;
                }
                uint32& since = exactStartSinceMs[group.LeaderGuid()];
                if (!since)
                {
                    since = now ? now : 1;
                    group.LogEvent("route_link_probe",
                             JsonObject().Str("note", link.note)
                                 .Str("verdict", "ground from the link start, not from here: to the start")
                                 .Real("offStart", leader->GetExactDist2d(link.fromX, link.fromY))
                                 .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                 .Real("z", leader->GetPositionZ()));
                }
                if (leader->GetExactDist2d(link.fromX, link.fromY) > LINK_EXACT_START &&
                    getMSTimeDiff(since, now) <= LINK_EXACT_MAX_MS)
                {
                    group.SetLeaderWaypoint(leader, link.fromX, link.fromY, link.fromZ);
                    if (leader->isMoving())
                        group.NoteProgress(now);
                    return true;
                }
                exactStartSinceMs.erase(group.LeaderGuid());  // on the start (or as near as it gets): the line carries
            }

            StartLinkCross(group, leader, link);
            routeState.linkLeaderCrossed = true;
            // [b30_wings] remembered against the immediate reverse (LinkTo)
            routeState.lastLinkIdx = routeState.linkIdx;
            routeState.lastLinkFor = routeState.linkFor;
            routeState.lastLinkMs = now;
            routeState.linkOffLineReturns = 0;
            routeState.linkOffLineNow = false;
            // The crossing's own clock (LINK_MAX_MS): the walk here may have taken minutes of it.
            routeState.linkSinceMs = now;
            group.NoteProgress(now);
            routeState.lastEvent = std::string("route link: ") + link.note;
            group.LogEvent("route_link", JsonObject().Str("state", "leader crossing").Str("note", link.note));
            return true;
        }

        PathResult leg;
        G3D::Vector3 const goal(link.fromX, link.fromY, link.fromZ);
        SteerLeg(leader, goal, leg);
        if (leg.type & PATHFIND_NOPATH)
        {
            EndLink(group, "no path to the link");
            return false;
        }

        if (!routeState.linkWalkLogged)
        {
            routeState.linkWalkLogged = true;
            routeState.lastEvent = std::string("route link walk: ") + link.note;
            group.LogEvent("route_link", JsonObject().Str("state", "walking").Str("note", link.note)
                                              .Real("distance", leader->GetExactDist(link.fromX, link.fromY,
                                                                                     link.fromZ)));
        }

        G3D::Vector3 const waypoint =
            FloorWaypoint(leader, leg, RouteWaypoint(leg, goal, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP));
        group.SetLeaderWaypoint(leader, waypoint.x, waypoint.y, waypoint.z);
        group.NoteProgress(now);
        return true;
    }

    void CrossLinks(LeaderGroup& group, std::vector<Player*> const& players, Player* leader)
    {
        GroupRouteState& routeState = group.Route();
        if (routeState.linkIdx < 0 || !routeState.linkLeaderCrossed || !leader)
            return;

        std::vector<RouteLinkDef const*> const links = RouteLinks(group.MapId());
        if (size_t(routeState.linkIdx) >= links.size())
        {
            routeState.linkIdx = -1;
            return;
        }
        RouteLinkDef const& link = *links[size_t(routeState.linkIdx)];
        uint32 const now = getMSTime();
        // A lift cycles every 15 s: a crossing still not done after LIFT_MAX_MS is no ride (LINK_MAX_FAILS gives up).
        if (getMSTimeDiff(routeState.linkSinceMs, now) > (LinkIsLift(link) ? LIFT_MAX_MS : LINK_MAX_MS))
        {
            EndLink(group, "timeout");
            return;
        }

        bool everybody = true;
        for (Player* bot : players)
        {
            if (!bot->IsAlive() || bot->GetMap() != leader->GetMap() || routeState.linkCrossed.count(bot->GetGUID()))
                continue;
            ObjectGuid memberGuid;
            for (LeaderGroup::MemberInfo const& candidate : group.Members())
                if (candidate.guid == bot->GetGUID())
                    memberGuid = candidate.guid;
            bool const member = !memberGuid.IsEmpty();
            bool const airborne =
                link.jump && (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE ||
                              bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
            if (airborne)
            {
                everybody = false;
                group.NoteProgress(now);
                continue;  // in the air: no new cross, no landing yet
            }
            if (AtLinkPoint(bot, link.toX, link.toY, link.toZ, LINK_DONE_RANGE))
            {
                if (link.jump)
                    LandLinkJump(bot, link.fromZ);
                routeState.linkCrossed.insert(bot->GetGUID());
                if (member && bot != leader)
                    group.ClearMemberWaypoint(memberGuid);
                continue;
            }
            everybody = false;
            if (bot == leader || bot->IsInCombat())
                continue;

            if (AtLinkPoint(bot, link.fromX, link.fromY, link.fromZ, LINK_ARRIVE_RANGE))
            {
                StartLinkCross(group, bot, link);
                if (member)
                    group.SetMemberHasWaypoint(memberGuid, true);
                group.NoteProgress(now);
                continue;
            }
            // Bots walk by themselves: a member already given the far end walks on over the link.
            if (member && group.MemberHasWaypoint(memberGuid))
            {
                LeaderGroup::WalkPoint current;
                if (group.GetWaypoint(bot->GetGUID(), current) &&
                    std::fabs(current.x - link.toX) < 0.1f && std::fabs(current.y - link.toY) < 0.1f)
                {
                    // LINK_OFF_LINE_BACK: off the line on another level - back to the start below, cross again.
                    if (!LinkIsLift(link) && !link.jump && LinkOffLine(bot, link))
                    {
                        LOG_INFO("playerbots",
                                 "[route] {}: link_off_line: member {} {:.1f} yd off the line of {} - back to its "
                                 "start",
                                 group.Run().runId, bot->GetName(), LinkLineDistance(bot, link), link.note);
                        if (member)
                            group.ClearMemberWaypoint(memberGuid);
                    }
                    else
                    {
                        if (bot->isMoving())
                            group.NoteProgress(now);
                        continue;
                    }
                }
            }

            // To the link's start on the navmesh (its own follow would go straight for the leader, over the gap).
            PathResult leg;
            G3D::Vector3 const goal(link.fromX, link.fromY, link.fromZ);
            SteerLeg(bot, goal, leg);
            if (leg.type & PATHFIND_NOPATH)
                continue;
            G3D::Vector3 const waypoint =
                FloorWaypoint(bot, leg, RouteWaypoint(leg, goal, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP));
            // As a leader points at the bridge: the route point is the member's waypoint, its own walk.
            if (member)
                group.SetMemberWaypoint(memberGuid, bot, waypoint.x, waypoint.y, waypoint.z);
            if (bot->isMoving())
                group.NoteProgress(now);
        }

        if (everybody)
            EndLink(group, "done");
    }

    // [b51_strat] User 2026-10-26: «Ключ есть - открыл ворота, пустил»; before the door is opened the WHOLE group
    // stands right next to the opener («совсем рядом», ~3-4 yd), out of combat, and passes in the same opening. Every
    // door that closes again by itself: GameObjectTemplate::GetAutoCloseTime() on a DOOR / BUTTON (world DB, 5-man
    // maps: Stratholme 175368 / 175352 / 175353 / 175369, Dire Maul 177211-177215 Doors, BRD 170555-170557 / 170572 /
    // 161461, UBRS 175185, Deadmines / SFK / Gnomeregan / SM levers and buttons, ...) or a KeyDoorPassDef gate. No cap
    // (user correction 2026-10-26: "never open early"): a member who cannot come is a stall for the stall recovery; the
    // wait is logged every DOOR_GATHER_LOG_MS. A member whose walk to the opener runs through this closed door is on
    // its far side (it is the one the door is opened for) and the door-cut member of this door too: not waited for.
    // Dead members do not count.
    bool DoorGatherHolds(LeaderGroup& group, Player* opener, GameObject* door)
    {
        GroupRouteState& routeState = group.Route();
        constexpr float DOOR_GATHER_RANGE = 4.0f;   // 2D, from the opener
        constexpr float DOOR_GATHER_DZ = 4.0f;
        constexpr float DOOR_GATHER_SPREAD = 1.5f;  // members are walked to points this far round the opener
        constexpr uint32 DOOR_GATHER_SEND_MS = 3 * IN_MILLISECONDS;
        constexpr uint32 DOOR_GATHER_LOG_MS = 10 * IN_MILLISECONDS;
        if (!opener || !door || !opener->IsInWorld())
            return false;
        GameObjectTemplate const* info = door->GetGOInfo();
        bool const autoCloses =
            ((door->GetGoType() == GAMEOBJECT_TYPE_DOOR || door->GetGoType() == GAMEOBJECT_TYPE_BUTTON) && info &&
             info->GetAutoCloseTime() > 0) ||
            (door->GetGoType() == GAMEOBJECT_TYPE_DOOR && KeyDoorPass(door->GetMapId(), door->GetEntry()));
        routeState.doorGatherHeld = false;
        if (!autoCloses)
            return false;
        Map* map = opener->GetMap();
        uint32 const now = getMSTime();
        // per instance and door (the harness's world thread only): (waiting since, last log), the members walked
        static std::unordered_map<uint64, std::pair<uint32, uint32>> gatherMs;
        static std::unordered_map<uint64, std::set<ObjectGuid>> walked;
        static std::unordered_map<uint64, uint32> sentMs;
        uint64 const key = (uint64(group.InstanceId()) << 32) | uint32(door->GetSpawnId());
        std::string waitFor;
        if (opener->IsInCombat())
            waitFor = std::string(opener->GetName()) + " (opener in combat)";
        bool const send = !sentMs.count(key) || getMSTimeDiff(sentMs[key], now) >= DOOR_GATHER_SEND_MS;
        uint32 slot = 0;
        for (LeaderGroup::MemberInfo const& member : group.Members())
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || bot == opener || !bot->IsAlive() || bot->GetMap() != map)
                continue;
            if (member.guid == routeState.doorCutMember && routeState.doorCutDoor == door->GetGUID())
                continue;  // stranded behind this door: it is opened for it
            // [lead1584] a held opening (lead1570) keeps the group back out of its reach: a member with its spot back
            // at this door is gathered there, not within reach of the opener (b281 strat-n: this wait for 4 yd and
            // the spot back held each other, 81 gathers, no key used)
            if (routeState.keyPassBackDoor == door->GetGUID() && routeState.keyPassBack.count(member.guid))
                continue;
            bool const close = bot->GetExactDist2d(opener) <= DOOR_GATHER_RANGE &&
                               std::fabs(bot->GetPositionZ() - opener->GetPositionZ()) <= DOOR_GATHER_DZ;
            if (close && !bot->IsInCombat())
                continue;
            if (!close && door->GetGoType() == GAMEOBJECT_TYPE_DOOR && door->GetGoState() == GO_STATE_READY)
            {
                PathResult const way =
                    RoutePath(bot, opener->GetPositionX(), opener->GetPositionY(), opener->GetPositionZ());
                if (way.reachable &&
                    RouteThroughClosedDoor(way.points, std::vector<GameObject*>{door}, opener->GetPhaseMask()))
                    continue;  // on the far side: cannot come, the opening is for it
            }
            if (!close && !bot->IsInCombat() && send)
            {
                float const angle = opener->GetOrientation() + float(M_PI) + (float(slot++ % 5) - 2.0f) * 0.9f;
                float const x = opener->GetPositionX() + std::cos(angle) * DOOR_GATHER_SPREAD;
                float const y = opener->GetPositionY() + std::sin(angle) * DOOR_GATHER_SPREAD;
                float const ground =
                    map->GetHeight(bot->GetPhaseMask(), x, y, opener->GetPositionZ() + 3.0f, true, 10.0f);
                group.SetMemberWaypoint(member.guid, bot, x, y,
                                        ground > INVALID_HEIGHT ? ground : opener->GetPositionZ());
                walked[key].insert(member.guid);
            }
            waitFor += (waitFor.empty() ? "" : ",") + member.name +
                       Acore::StringFormat(" ({}{:.1f} yd)", bot->IsInCombat() ? "combat, " : "",
                                           bot->GetExactDist2d(opener));
        }
        if (send)
            sentMs[key] = now ? now : 1;
        auto& [sinceMs, logMs] = gatherMs[key];
        if (waitFor.empty())
        {
            if (sinceMs)
            {
                group.LogEvent("door_gather", JsonObject().Str("state", "together").Str("door", door->GetName())
                                                   .Num("doorEntry", door->GetEntry()).Str("opener", opener->GetName())
                                                   .Num("waitSec", getMSTimeDiff(sinceMs, now) / IN_MILLISECONDS));
                LOG_INFO("playerbots", "[b51_strat] door_gather: {} opens {} {} with the group within {:.0f} yd ({} s)",
                         opener->GetName(), door->GetName(), door->GetEntry(), DOOR_GATHER_RANGE,
                         getMSTimeDiff(sinceMs, now) / IN_MILLISECONDS);
            }
            // the gather walks are over: the members follow again (a key pass sends them through itself)
            for (ObjectGuid const& guid : walked[key])
                for (LeaderGroup::MemberInfo const& member : group.Members())
                    if (member.guid == guid && group.MemberHasWaypoint(member.guid))
                        group.ClearMemberWaypoint(member.guid);
            walked.erase(key);
            gatherMs.erase(key);
            sentMs.erase(key);
            return false;
        }
        if (!sinceMs)
            sinceMs = now ? now : 1;
        if (!logMs || getMSTimeDiff(logMs, now) >= DOOR_GATHER_LOG_MS)
        {
            logMs = now ? now : 1;
            uint32 const waitSec = getMSTimeDiff(sinceMs, now) / IN_MILLISECONDS;
            group.LogEvent("door_gather", JsonObject().Str("state", "waiting").Str("door", door->GetName())
                                               .Num("doorEntry", door->GetEntry()).Str("opener", opener->GetName())
                                               .Str("waitFor", waitFor).Num("waitSec", waitSec));
            LOG_INFO("playerbots", "[b51_strat] door_gather waiting for {} ({} s): {} holds {} {} until everyone "
                     "stands within {:.0f} yd", waitFor, waitSec, opener->GetName(), door->GetName(), door->GetEntry(),
                     DOOR_GATHER_RANGE);
        }
        routeState.doorGatherHeld = true;
        return true;
    }

    bool UseStarter(LeaderGroup& group, Player* leader, StarterTrack& starter, WorldObject* object)
    {
        GroupRouteState& routeState = group.Route();
        uint32 const now = getMSTime();
        // [starter_wait_group] EventStarterDef::gatherRadius: a gate closes on the use (Kirtonos' 175570): everyone in
        // first, at most STARTER_GATHER_MAX_MS.
        if (starter.def->gatherRadius > 0.0f)
        {
            std::string away;
            for (LeaderGroup::MemberInfo const& member : group.Members())
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    bot && bot->IsAlive() && bot->GetMap() == object->GetMap() &&
                    bot->GetExactDist(object) > starter.def->gatherRadius)
                    away += (away.empty() ? "" : ",") + member.name;
            auto& [sinceMs, logMs] = starterGatherMs[group.InstanceId()];
            if (away.empty())
                sinceMs = 0;
            else
            {
                if (!sinceMs)
                    sinceMs = now ? now : 1;
                // [b51_strat] never used early (user 2026-10-26); the cap is only logged
                bool const overCap = getMSTimeDiff(sinceMs, now) >= STARTER_GATHER_MAX_MS;
                bool const gaveUp = false;
                if (!logMs || getMSTimeDiff(logMs, now) >= STARTER_GATHER_LOG_MS || gaveUp)
                {
                    logMs = now ? now : 1;
                    group.LogEvent("starter_wait_group",
                             JsonObject().Str("note", starter.def->note).Str("away", away)
                                 .Real("radius", starter.def->gatherRadius)
                                 .Num("waitSec", getMSTimeDiff(sinceMs, now) / IN_MILLISECONDS).Bool("gaveUp", gaveUp)
                                 .Bool("overCap", overCap));
                }
                if (!gaveUp)
                {
                    starter.nextAllowedMs = now + STARTER_GATHER_RETRY_MS;
                    return false;
                }
                sinceMs = 0;
            }
        }
        ++starter.uses;
        starter.wipesAtUse = group.Wipes();
        starter.nextAllowedMs = now + starter.def->cooldownMs;
        // [b51_strat] a door_gather hold (DoorGatherHolds) is no progress: the stall recovery sees a member who
        // cannot come
        uint32 const progressBefore = group.LastProgressMs();
        group.NoteProgress(now);
        routeState.doorGatherHeld = false;

        JsonArray options;
        if (starter.def->kind == StarterKind::UseItem)
        {
            GameObject* go = object->ToGameObject();
            if (!go || !UseStarterItem(group, leader, starter, go))
            {
                // Not used (still cooling down, stepping out of the object, or the core refused the cast): no use
                // counted (pass 3); UseStarterItem set when to try again.
                if (ROUTE_STARTER_ITEM_CHECK)
                    --starter.uses;
                if (routeState.doorGatherHeld)
                    group.NoteProgress(progressBefore);  // [b51_strat]
                return false;
            }
            starter.usedObjects.insert(go->GetGUID());
            if (starter.def->expectDoor)
                starter.expectDoorCheckMs = now + EXPECT_DOOR_MS;  // [expect_door] CheckExpectedDoors
            // [strat_key_pass] an auto-closing key gate with a KeyDoorPassDef - the group goes through at once
            // (TickKeyDoorPass).
            if (go->GetGoType() == GAMEOBJECT_TYPE_DOOR && KeyDoorPass(go->GetMapId(), go->GetEntry()))
            {
                routeState.keyPassDoor = go->GetGUID();
                routeState.keyPassSinceMs = now ? now : 1;
                routeState.keyPassSent = false;
                routeState.keyPassOpened = false;    // [b32_strat_gate]
                routeState.keyPassCaster.Clear();    // the leader keyed it
            }
        }
        else if (starter.def->kind == StarterKind::VendorBuy || starter.def->kind == StarterKind::QuestTurnIn)
        {
            Creature* creature = object->ToCreature();
            if (!creature || !UseStarterAtNpc(group, leader, starter, creature))
            {
                --starter.uses;
                return false;
            }
        }
        else if (starter.def->kind == StarterKind::Gossip || starter.def->kind == StarterKind::Escort)
        {
            Creature* creature = object->ToCreature();
            if (!creature)
                return false;
            // It set off on a scripted walk while the leader came (WalksScriptedPath): no click, no use counted.
            if (ROUTE_STARTER_NOT_WALKING && WalksScriptedPath(creature))
            {
                --starter.uses;
                starter.nextAllowedMs = now + STARTER_WALKING_RETRY_MS;
                return false;
            }

            // A client's gossip needs no stop or turn.
            WorldPacket hello(CMSG_GOSSIP_HELLO);
            hello << creature->GetGUID();
            leader->GetSession()->HandleGossipHelloOpcode(hello);

            for (uint32 page = 0; page < 4; ++page)
            {
                GossipMenu& menu = leader->PlayerTalkClass->GetGossipMenu();
                if (menu.Empty())
                    break;

                uint32 chosen = UINT32_MAX;
                for (uint32 preference : starter.def->optionPrefs)
                    if (GossipMenuItem const* item = menu.GetItem(preference))
                        if (!item->IsCoded)
                        {
                            chosen = preference;
                            break;
                        }

                if (chosen == UINT32_MAX && !starter.def->strictOptions)
                    for (auto const& [index, item] : menu.GetMenuItems())
                        if (!item.IsCoded)
                        {
                            chosen = index;
                            break;
                        }

                if (chosen == UINT32_MAX)
                {
                    // EventStarterDef::strictOptions: the step is past (the bots' own strategy did it), nothing else
                    // on the menu is picked. Logged once with what was offered.
                    if (starter.def->strictOptions && page == 0)
                    {
                        JsonArray offered;
                        for (auto const& [index, offer] : menu.GetMenuItems())
                            offered.Raw(JsonObject().Num("option", index).Str("text", offer.Message).Build());
                        group.LogEvent("starter_step_past", JsonObject().Str("note", starter.def->note)
                                                                 .Num("menu", menu.GetMenuId())
                                                                 .Raw("offered", offered.Build()));
                        leader->PlayerTalkClass->SendCloseGossip();
                    }
                    break;
                }

                GossipMenuItem const* item = menu.GetItem(chosen);
                options.Raw(JsonObject().Num("menu", menu.GetMenuId()).Num("option", chosen)
                                .Str("text", item ? item->Message : "").Build());

                WorldPacket select(CMSG_GOSSIP_SELECT_OPTION);
                select << creature->GetGUID() << uint32(menu.GetMenuId()) << uint32(chosen);
                leader->GetSession()->HandleGossipSelectOptionOpcode(select);
            }

            // The escort begins: the group stays with the escorted creature from now on.
            if (starter.def->kind == StarterKind::Escort)
            {
                Creature* escorted = creature;
                if (starter.def->escortEntry && starter.def->escortEntry != creature->GetEntry())
                    escorted = creature->FindNearestCreature(starter.def->escortEntry, 100.0f, true);
                if (escorted)
                {
                    routeState.escort = escorted->GetGUID();
                    routeState.escortEntry = escorted->GetEntry();
                    routeState.escortSinceMs = now;
                    routeState.escortIdleSinceMs = now;
                    routeState.escortLastPos = escorted->GetPosition();
                    routeState.escortNote = starter.def->note;
                    group.LogEvent("escort_start", JsonObject().Str("note", starter.def->note)
                                                        .Str("name", escorted->GetName())
                                                        .Num("entry", escorted->GetEntry()));
                }
            }
        }
        else
        {
            GameObject* go = object->ToGameObject();
            if (!go)
                return false;

            std::vector<Player*> users = {leader};
            if (starter.def->allMembers)
            {
                users.clear();
                for (LeaderGroup::MemberInfo const& member : group.Members())
                    if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                        if (bot->IsAlive() && bot->GetMap() == leader->GetMap())
                            users.push_back(bot);
            }

            // [starter_chest_open] A locked chest: GameObject::Use has no case for GAMEOBJECT_TYPE_CHEST (a click does
            // nothing). The client casts the lock's opening spell on it; EffectOpenLock -> Player::SendLoot ->
            // SetLootState(GO_ACTIVATED) -> GameObjectAI::OnStateChanged (go_father_flame: DATA_SOLAKAR_FLAMEWREATH
            // IN_PROGRESS). 2026-10-25 ubrs-n: five "event_start use 175245", Solakar "event not started" every time.
            uint32 const openSpell = go->GetGoType() == GAMEOBJECT_TYPE_CHEST ? ChestOpeningSpell(leader, go) : 0;
            if (openSpell)
            {
                // [event_boss_classic] [starter_chest_backoff] Not while the leader casts or sits in a global cooldown:
                // HandleCastSpellOpcode then only queues the cast (Player::SpellQueue), the check below saw no cast
                // and called it refused (brd-h 20261025-163739 18:20:05 / 18:20:16: Dark Iron Ale Mug 165738, lock
                // 93, spell 6477 at 2.5 yd, LoS true - the prot paladin tank is never off the GCD for long).
                SpellInfo const* openInfo = sSpellMgr->GetSpellInfo(openSpell);
                if (leader->IsNonMeleeSpellCast(false) || !leader->SpellQueue.empty() ||
                    (openInfo && leader->GetGlobalCooldownMgr().HasGlobalCooldown(openInfo)))
                {
                    --starter.uses;
                    starter.nextAllowedMs = now + 1500;
                    return false;
                }
                WorldPacket cast(CMSG_CAST_SPELL);
                cast << uint8(0) << openSpell << uint8(0) << uint32(TARGET_FLAG_GAMEOBJECT) << go->GetPackGUID();
                leader->GetSession()->HandleCastSpellOpcode(cast);
                bool const opened = leader->IsNonMeleeSpellCast(false) || go->getLootState() == GO_ACTIVATED;
                group.LogEvent(opened ? "starter_chest_open" : "starter_chest_refused",
                         JsonObject().Str("note", starter.def->note).Num("entry", go->GetEntry())
                             .Num("lock", go->GetGOInfo()->GetLockId()).Num("spell", openSpell)
                             .Real("distance", leader->GetExactDist(go)).Bool("los", leader->IsWithinLOSInMap(go)));
                // [b32_dm_door] a chest the leader has no line of sight to from where it stands (the Deadmines
                // Defias Gunpowder crate 17155: los false at every distance, all 11 refusals in today's dm runs at
                // 0.9-4.0 yd, all 5 opens at 0.5-0.6 yd - standing on it) is stepped onto first, in a straight walk to
                // its centre (a test-runner waypoint counts as reached 2.5 yd off), and not counted as a refusal.
                constexpr float CHEST_STEP_ON_RANGE = 0.7f;
                if (!opened && !leader->IsWithinLOSInMap(go) && leader->GetExactDist2d(go) > CHEST_STEP_ON_RANGE &&
                    leader->GetExactDist2d(go) <= 6.0f && starter.steppedOut.insert(go->GetGUID()).second &&
                    DirectLineOverGround(leader, go->GetPositionX(), go->GetPositionY(), go->GetPositionZ()))
                {
                    --starter.uses;
                    group.SetLeaderWaypoint(leader, go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(),
                                      true);
                    routeState.waypointHoldUntilMs = now + 3 * IN_MILLISECONDS;
                    starter.nextAllowedMs = now + 2 * IN_MILLISECONDS;
                    group.LogEvent("starter_chest_step_on",
                             JsonObject().Str("note", starter.def->note).Num("entry", go->GetEntry())
                                 .Real("distance", leader->GetExactDist(go)));
                    return false;
                }
                if (!opened)
                {
                    --starter.uses;  // refused (range 5 yd, line of sight): tried again, as the item path does
                    // [event_boss_classic] [starter_chest_backoff] A refused chest is no loop: each refusal waits
                    // longer (30 s per refusal of the starter, at most 120 s) so the route goes on meanwhile; an
                    // object refused STARTER_ITEM_MAX_FAILS times counts as used (the next one is tried), and after
                    // CHEST_REFUSALS_GIVE_UP refusals in all the starter is given up (logged). brd-h 20261025-163739
                    // 18:19-18:20: Mug refused -> Doom'rel -> Mug ... every 5 s, the tank running between them.
                    constexpr uint32 CHEST_REFUSALS_GIVE_UP = 6;
                    constexpr uint32 CHEST_BACKOFF_STEP_MS = 30 * IN_MILLISECONDS;
                    constexpr uint32 CHEST_BACKOFF_MAX_MS = 120 * IN_MILLISECONDS;
                    uint32 const objectFails = ++starter.failedUses[go->GetGUID()];
                    uint32 totalFails = 0;
                    for (auto const& [guid, fails] : starter.failedUses)
                        totalFails += fails;
                    if (objectFails >= STARTER_ITEM_MAX_FAILS)
                        starter.usedObjects.insert(go->GetGUID());
                    starter.steppedOut.erase(go->GetGUID());  // [b32_dm_door] a step onto it before the next try
                    starter.nextAllowedMs =
                        now + std::min(CHEST_BACKOFF_MAX_MS, CHEST_BACKOFF_STEP_MS * totalFails);
                    // [b32_dm_door] every object of the starter refused STARTER_ITEM_MAX_FAILS times is a give-up as
                    // well: dm-h 20261025-221216-dm-h-5D-59, the only crate refused 3 times (1283-1453 s), counted
                    // used, "no usable object" at 1547 s - and the starter itself never used, so the Defias Cannon
                    // (afterUsed 17155; its Defias Gunpowder carried from the start) never came: Smite / Cookie /
                    // Greenskin "behind a closed door (Iron Clad Door 16397)", stalled at 1926 s.
                    bool allRefused = true;
                    for (auto const& [spawnId, other] : leader->GetMap()->GetGameObjectBySpawnIdStore())
                        if (other && other->isSpawned() &&
                            std::find(starter.def->entries.begin(), starter.def->entries.end(),
                                      other->GetEntry()) != starter.def->entries.end() &&
                            !starter.usedObjects.count(other->GetGUID()))
                            allRefused = false;
                    if (totalFails >= CHEST_REFUSALS_GIVE_UP || allRefused)
                    {
                        starter.uses = starter.def->maxUses;
                        group.LogEvent("starter_given_up",
                                 JsonObject().Str("note", starter.def->note).Num("entry", go->GetEntry())
                                     .Num("refusals", totalFails).Str("why", "chest open refused"));
                    }
                    if (routeState.target == go->GetGUID())
                        routeState.target.Clear();
                    return false;
                }
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(openSpell))
                    routeState.castHoldUntilMs = now + info->CalcCastTime(leader) + 1000;  // 1 s: a walk would break it
                group.ClearWaypoint(group.LeaderGuid());
            }
            else if (!leader->IsInCombat() && routeState.doorCutDoor != go->GetGUID() &&
                     DoorGatherHolds(group, users.empty() ? leader : users.front(), go))
            {
                // [b51_strat] door_gather: a lever / button / door that closes again, used with the group next to it
                --starter.uses;
                group.NoteProgress(progressBefore);
                starter.nextAllowedMs = now + STARTER_GATHER_RETRY_MS;
                return false;
            }
            else
                for (Player* user : users)
                {
                    WorldPacket use(CMSG_GAMEOBJ_USE);
                    use << go->GetGUID();
                    user->GetSession()->HandleGameObjectUseOpcode(use);
                }
            if (starter.def->distinct)
                starter.usedObjects.insert(go->GetGUID());
        }

        routeState.lastEvent = std::string("event start: ") + starter.def->note;
        char const* kind = "use";
        if (starter.def->kind == StarterKind::Gossip)
            kind = "gossip";
        else if (starter.def->kind == StarterKind::Escort)
            kind = "escort";
        else if (starter.def->kind == StarterKind::UseItem)
            kind = "item";
        else if (starter.def->kind == StarterKind::VendorBuy)
            kind = "vendor";
        else if (starter.def->kind == StarterKind::QuestTurnIn)
            kind = "quest";
        routeState.lastStarterUseMs = getMSTime();
        routeState.starterUsedSinceRetarget = true;  // [lead135] the route is built again first
        starter.lastUseMs = routeState.lastStarterUseMs ? routeState.lastStarterUseMs : 1;
        group.LogEvent("event_start", JsonObject().Str("note", starter.def->note).Str("kind", kind)
                                           .Num("entry", object->GetEntry()).Num("uses", starter.uses)
                                           .Raw("options", options.Build()));
        return true;
    }

    bool UseStarterItem(LeaderGroup& group, Player* leader, StarterTrack& starter, GameObject* go)
    {
        GroupRouteState& routeState = group.Route();
        uint32 const now = getMSTime();
        Item* item = leader->GetItemByEntry(starter.def->itemId);
        uint32 spellId = 0;
        if (item)
            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS && !spellId; ++i)
                if (item->GetTemplate()->Spells[i].SpellId > 0 &&
                    item->GetTemplate()->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                    spellId = uint32(item->GetTemplate()->Spells[i].SpellId);

        // The leader casts this item's spell already (its own "dungeon event step", Old Hillsbrad wave 13: the
        // harness logged the bombs "missing" during the bot's own 5 s cast and walked it out of it): wait for it.
        Spell const* casting = leader->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (spellId && casting && casting->GetSpellInfo() && casting->GetSpellInfo()->Id == spellId)
        {
            routeState.castHoldUntilMs = now + 1500;
            starter.nextAllowedMs = now + 1500;
            return false;
        }

        InventoryResult const usable = item ? leader->CanUseItem(item) : EQUIP_ERR_ITEM_NOT_FOUND;
        if (!item || usable != EQUIP_ERR_OK || leader->IsNonMeleeSpellCast(false))
        {
            Spell const* other = leader->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (!other)
                other = leader->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            group.LogEvent("starter_item_missing", JsonObject().Str("note", starter.def->note)
                                                        .Num("item", starter.def->itemId)
                                                        .Bool("inBags", item != nullptr)
                                                        .Num("canUse", uint32(usable))
                                                        .Num("castingSpell", other && other->GetSpellInfo()
                                                                                 ? other->GetSpellInfo()->Id
                                                                                 : 0)
                                                        .Bool("inCombat", leader->IsInCombat()));
            if (item && usable == EQUIP_ERR_OK)
                starter.nextAllowedMs = now + STARTER_ITEM_RETRY_MS;  // busy casting: later
            return false;
        }

        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info)
            return false;
        ItemTemplate const* proto = item->GetTemplate();

        bool expectCooldown = info->RecoveryTime > 0 || info->CategoryRecoveryTime > 0;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            if (uint32(proto->Spells[i].SpellId) == spellId)
                expectCooldown |= proto->Spells[i].SpellCooldown > 0 || proto->Spells[i].SpellCategoryCooldown > 0;
        if (ROUTE_STARTER_ITEM_CHECK)
        {
            // Still cooling down (a dungeon strategy used the same item: Culling's "cos dispel crate"): later.
            if (leader->HasSpellCooldown(spellId))
            {
                starter.nextAllowedMs = now + leader->GetSpellCooldownDelay(spellId) + 500;
                return false;
            }

            // Standing inside the object's model (the CoS strategy walks to a crate's centre; Culling wave 1-10 the
            // leader stood on Perelli's crate, 1.1 yd from its helper, for 4 minutes): one step out first, then the
            // use from STARTER_ITEM_STEP_OUT yd, as a player stands next to a crate.
            float const near2d = leader->GetExactDist2d(go);
            // Once per object: the leader that did not get out of the model (Old Hillsbrad wave 13: five step outs at
            // 0.0 yd from a barrel, then nothing) uses it from where it stands.
            if (near2d < STARTER_ITEM_MIN_RANGE && starter.steppedOut.insert(go->GetGUID()).second)
            {
                float const angle = near2d > 0.3f ? go->GetAngle(leader) : leader->GetOrientation() + float(M_PI);
                G3D::Vector3 dest(go->GetPositionX() + std::cos(angle) * STARTER_ITEM_STEP_OUT,
                                  go->GetPositionY() + std::sin(angle) * STARTER_ITEM_STEP_OUT, leader->GetPositionZ());
                float const ground = leader->GetMap()->GetHeight(leader->GetPhaseMask(), dest.x, dest.y,
                                                                 dest.z + 3.0f, true, 10.0f);
                if (ground > INVALID_HEIGHT && DirectLineOverGround(leader, dest.x, dest.y, ground))
                {
                    dest.z = ground;
                    uint32 const walkMs = uint32(STARTER_ITEM_STEP_OUT /
                                                 std::max(1.0f, leader->GetSpeed(MOVE_RUN)) * IN_MILLISECONDS);
                    // The step out is the leader's waypoint (its own "test runner move"), kept for the walk.
                    group.SetLeaderWaypoint(leader, dest.x, dest.y, dest.z);
                    routeState.waypointHoldUntilMs = now + walkMs + 1500;
                    starter.nextAllowedMs = now + walkMs + 500;
                    group.LogEvent("starter_step_out", JsonObject().Str("note", starter.def->note)
                                                            .Num("entry", go->GetEntry()).Real("distance", near2d)
                                                            .Real("toX", dest.x).Real("toY", dest.y)
                                                            .Real("toZ", dest.z));
                    return false;
                }
            }

            // A client casting from a ground mount dismounts first; the server refuses a mounted cast
            // (SPELL_FAILED_NOT_MOUNTED). A dungeon action that answers OK every tick keeps the bot's own "check
            // mount state" from ever running.
            if (leader->IsMounted())
                leader->RemoveAurasByType(SPELL_AURA_MOUNTED);
        }

        // [lbrs_urok_event] A cast-time item spell broken by moving is refused while the caster moves (Spell::prepare:
        // SPELL_FAILED_MOVING); a player stops first. lbrs-n 20261025-163739-lbrs-n-5A-25: Omokk's Head (16447, 2 s,
        // InterruptFlags 31) was cast three times mid-walk (positions "moving": true, 2.2 / 3.7 yd from the pile),
        // three "starter_item_failed", the pile counted used ("no usable object"), Urok "event not started". The walk
        // ends here and the use is tried again once the leader stands - no fail counted. expectDoor has its own wait.
        if (ROUTE_STARTER_ITEM_CHECK && !starter.def->expectDoor && info->CalcCastTime(leader) > 0 &&
            (info->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT) && leader->isMoving())
        {
            group.ClearWaypoint(group.LeaderGuid());
            leader->StopMoving();
            routeState.castHoldUntilMs = now + 1500;
            starter.nextAllowedMs = now + 1000;
            group.LogEvent("starter_item_wait_still", JsonObject().Str("note", starter.def->note)
                                                           .Num("entry", go->GetEntry()).Num("spell", spellId)
                                                           .Real("distance", leader->GetExactDist(go)));
            return false;
        }

        // [expect_door_wait] EventStarterDef::expectDoor: a cast that breaks or is refused leaves the door shut and the
        // starter used up (2026-10-25 dm-h: event_start at 1121 s, retarget 1 s later, Iron Clad Door shut to the end).
        // Cast only out of combat (nobody of the group fighting), standing still, within EXPECT_DOOR_CAST_RANGE and
        // in line of sight; otherwise step in toward the object and try again shortly - no use counted.
        if (ROUTE_STARTER_ITEM_CHECK && starter.def->expectDoor)
        {
            char const* waitFor = nullptr;
            bool groupFighting = leader->IsInCombat();
            for (LeaderGroup::MemberInfo const& member : group.Members())
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    bot && bot->GetMap() == leader->GetMap() && bot->IsAlive() && bot->IsInCombat())
                    groupFighting = true;
            float const centre = leader->GetExactDist(go);
            // [w24b-cannon-los] Sight as the server checks it for an object target: Spell::CheckCast has no line-of-
            // sight test for a GO target, and IsWithinLOSInMap(go) aims at the object's origin - the Defias Cannon's
            // (z 7.2) lies under the deck it stands on (dm-h 2026-10-25 115805: 21 "no line of sight" waits from 1183 s
            // to 1417 s at 2.2-3.9 yd, never cast). Walls still count: the static map only, toward the origin raised by
            // the leader's collision height (IsWithinLOS adds it).
            bool const floorLos = leader->IsWithinLOSInMap(go);
            bool const los = floorLos || leader->IsWithinLOS(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(),
                                                             VMAP::ModelIgnoreFlags::M2, LINEOFSIGHT_CHECK_VMAP);
            if (!floorLos && los && !groupFighting && centre <= EXPECT_DOOR_CAST_RANGE && !leader->isMoving())
            {
                static uint32 cannonLosLogMs = 0;
                if (!cannonLosLogMs || getMSTimeDiff(cannonLosLogMs, now) >= EXPECT_DOOR_LOG_MS)
                {
                    cannonLosLogMs = now ? now : 1;
                    LOG_INFO("playerbots", "[w24b-cannon-los] {}: {}: cast with the object's origin hidden ({:.1f} yd, "
                             "static map clear)", group.Run().runId, starter.def->note, centre);
                }
            }
            if (groupFighting)
                waitFor = "group in combat";
            else if (centre > EXPECT_DOOR_CAST_RANGE || !los)
                waitFor = !los ? "no line of sight" : "too far";
            else if (leader->isMoving())
                waitFor = "moving";
            if (waitFor)
            {
                if (!groupFighting && (centre > EXPECT_DOOR_CAST_RANGE || !los))
                {
                    float const toLeader = go->GetAngle(leader);
                    G3D::Vector3 dest(go->GetPositionX() + std::cos(toLeader) * EXPECT_DOOR_STEP,
                                      go->GetPositionY() + std::sin(toLeader) * EXPECT_DOOR_STEP,
                                      leader->GetPositionZ());
                    float const ground = leader->GetMap()->GetHeight(leader->GetPhaseMask(), dest.x, dest.y,
                                                                     dest.z + 3.0f, true, 10.0f);
                    if (ground > INVALID_HEIGHT)
                    {
                        dest.z = ground;
                        group.SetLeaderWaypoint(leader, dest.x, dest.y, dest.z);
                        routeState.waypointHoldUntilMs = now + STARTER_ITEM_RETRY_MS;
                    }
                }
                starter.nextAllowedMs = now + EXPECT_DOOR_WAIT_MS;
                if (!starter.expectDoorWaitLogMs ||
                    getMSTimeDiff(starter.expectDoorWaitLogMs, now) >= EXPECT_DOOR_LOG_MS)
                {
                    starter.expectDoorWaitLogMs = now ? now : 1;
                    group.LogEvent("expect_door_wait",
                             JsonObject().Str("note", starter.def->note).Num("entry", go->GetEntry())
                                 .Str("waitFor", waitFor).Real("distance", centre).Bool("los", los));
                }
                return false;
            }
        }

        // [strat_key_gather] KeyDoorPassDef: the first use of an auto-closing key gate is made from its use point with
        // the whole group there, out of combat (strat-n/-h 2026-10-25: keyed from 9 yd without sight, then the gate
        // shut 3 s after it opened on members 10-30 yd back - six re-keys in two minutes). Re-keys (doorRearms > 0) are
        // made from where the leader stands (ReopenKeyDoorHeld / ReopenKeyDoorInCombat): members may be on both sides.
        // [queue-key-gate-autoclose] re-keys too: the King's Square gates close 3 s after they open
        // (gameobject_template 175352 / 175353 data2 autoCloseTime 3000), a re-key "from where the leader stands"
        // opened 175353 for members 10-40 yd away who never made it through (strat-h 20261025-163739-strat-h-5A-11,
        // 3098-3130 s: rearms 4, the healer "follow ... runs through King's Square Gate 175353 -> stay" right after
        // each 3 s opening). Members already behind the gate are not gathered at the use point (they cannot come):
        // they wait at the far point.
        // A re-key in a fight (ReopenKeyDoorInCombat) keeps its use from where the leader stands.
        KeyDoorPassDef const* pass = go->GetGoType() == GAMEOBJECT_TYPE_DOOR && go->GetGoState() == GO_STATE_READY &&
                                             !(starter.doorRearms && leader->IsInCombat())
                                         ? KeyDoorPass(go->GetMapId(), go->GetEntry())
                                         : nullptr;
        if (pass)
        {
            std::string waitFor;
            // [b32_strat_gate] the use point on the LEADER's side of the gate. strat-n 20261025-221216-strat-n-5H-2:
            // the gate shut on the healer, the in-combat re-key armed at 92 s with the tank 4.9 yd inside it, and this
            // block sent the tank to the entrance-side use point - through the closed gate: door_hold at its own spot
            // every 5 s ("leader to the use point", 7.1 yd), no cast, steer_stuck at 118 s, the gate passed over
            // ("no usable object") and the group walked on without the healer. A leader on the far side keys from the
            // mirror of the use point through the gate's centre (3 yd in front of it on its own side).
            float const gateFx0 = std::cos(go->GetOrientation());
            float const gateFy0 = std::sin(go->GetOrientation());
            float const gateBars0 = KeyGateBarsOffset(go, *pass);  // [b35_ss2] the bars' plane
            auto const gateSide0 = [&](float x, float y)
            { return (x - go->GetPositionX()) * gateFx0 + (y - go->GetPositionY()) * gateFy0 - gateBars0; };
            bool const leaderFarSide = (gateSide0(leader->GetPositionX(), leader->GetPositionY()) >= 0.0f) !=
                                       (gateSide0(pass->useX, pass->useY) >= 0.0f);
            float const useX = leaderFarSide ? 2.0f * go->GetPositionX() - pass->useX : pass->useX;
            float const useY = leaderFarSide ? 2.0f * go->GetPositionY() - pass->useY : pass->useY;
            float const useZ = pass->useZ;
            // [b30_overpull] at the use point, or within the runner's reach of it (a waypoint there never moves the
            // leader) with the gate's centre in the Opening's range and sight (raised as the cannon test above)
            // [b48_key] the range is the core's own (Spell::CheckRange -> GameObject::IsAtInteractDistance: the gate's
            // model box grown by the Opening's 5 yd), the 4.5 yd to the centre is only a part of it. strat-n
            // 20261026-111724-strat-n-5A-3: the re-key of 175368 (rearms 2) stopped the leader
            // (starter_item_wait_still) 2.0 yd behind the use point, 4.96 yd from the centre: in the core's range (the
            // box's 0.35 yd half depth:
            // 5.35 yd), out of the 4.5 - and the use point's waypoint, within the runner's 2.5 yd, never moved it:
            // 1277-1634 s idle to the stop. Same at 060218-strat-h-5H-1 (23-290 s, 4.9 yd) and the King's Square gates.
            float const toUse = leader->GetExactDist2d(useX, useY);
            bool const atUse =
                toUse <= KEY_PASS_LEADER_RANGE ||
                (toUse <= KEY_PASS_RUNNER_REACH &&
                 (leader->GetExactDist(go) <= KEY_PASS_CAST_CENTRE || go->IsAtInteractDistance(leader, info)) &&
                 leader->IsWithinLOS(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(),
                                     VMAP::ModelIgnoreFlags::M2, LINEOFSIGHT_CHECK_VMAP));
            // [b30_overpull] the gather's clock (per instance): past KEY_PASS_GATHER_MAX_MS the members are not waited
            // (since, last seen) per instance: a gap over 10 s starts a new gather (world thread)
            static std::unordered_map<uint32, std::pair<uint32, uint32>> keyGatherMs;
            auto& [gatherSince, gatherSeen] = keyGatherMs[group.InstanceId()];
            if (!gatherSince || getMSTimeDiff(gatherSeen, now) > 10 * IN_MILLISECONDS)
                gatherSince = now ? now : 1;
            gatherSeen = now ? now : 1;
            // [b51_strat] no cap any more (user 2026-10-26: "if someone has not come up, the door is NOT opened"); a
            // member who cannot come is the stall recovery's
            bool const gatherGaveUp = false && getMSTimeDiff(gatherSince, now) >= KEY_PASS_GATHER_MAX_MS;
            if (leader->IsInCombat())
                waitFor = "leader in combat";
            else if (!atUse)
            {
                waitFor = toUse <= KEY_PASS_RUNNER_REACH
                              ? "leader to the use point (within the runner's reach, gate out of range or sight)"
                              : "leader to the use point";
                // [b48_key] within the runner's reach the use point's waypoint counts as reached and never moves the
                // leader: the waypoint is the use point stepped toward the gate's centre (KEY_PASS_RUNNER_REACH -
                // KEY_PASS_LEADER_RANGE = 1 yd, still on the leader's side), a walk the runner makes.
                float wayX = useX;
                float wayY = useY;
                if (toUse <= KEY_PASS_RUNNER_REACH)
                {
                    float const toGateX = go->GetPositionX() - useX;
                    float const toGateY = go->GetPositionY() - useY;
                    float const toGate = std::max(0.1f, std::hypot(toGateX, toGateY));
                    float const step = std::min(KEY_PASS_RUNNER_REACH - KEY_PASS_LEADER_RANGE, toGate * 0.5f);
                    wayX += toGateX / toGate * step;
                    wayY += toGateY / toGate * step;
                }
                float const ground =
                    leader->GetMap()->GetHeight(leader->GetPhaseMask(), wayX, wayY, useZ + 3.0f, true, 10.0f);
                // [b32_strat_gate] the use point of the leader's side
                group.SetLeaderWaypoint(leader, wayX, wayY, ground > INVALID_HEIGHT ? ground : useZ);
                routeState.waypointHoldUntilMs = now + STARTER_ITEM_RETRY_MS;
            }
            else if (leaderFarSide)
            {
                // [b32_strat_gate] a re-key from the far side: the members on the leader's side are with it; the ones
                // cut off on the other side wait at the entrance-side use point (TickKeyDoorPass sends them through the
                // moment the gate opens). Nobody is walked through the closed gate.
                for (LeaderGroup::MemberInfo const& member : group.Members())
                {
                    Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    if (!bot || bot == leader || !bot->IsAlive() || bot->GetMap() != leader->GetMap() ||
                        (gateSide0(bot->GetPositionX(), bot->GetPositionY()) >= 0.0f) ==
                            (gateSide0(leader->GetPositionX(), leader->GetPositionY()) >= 0.0f) ||
                        bot->GetExactDist2d(pass->useX, pass->useY) <= KEY_PASS_GATHER_RANGE)
                        continue;
                    if (!bot->IsInCombat())
                    {
                        group.SetMemberWaypoint(member.guid, bot, pass->useX, pass->useY, pass->useZ);
                        group.MarkGateWaypoint(bot);  // [b33_strat_gate]
                    }
                    waitFor += (waitFor.empty() ? "" : ",") + member.name + " (cut off, to the gate)";
                }
                if (!waitFor.empty() && gatherGaveUp)
                    waitFor.clear();
            }
            else
            {
                // The side away from the gate: round the use point, within +-80 degrees of the gate -> use direction.
                float const away = std::atan2(pass->useY - go->GetPositionY(), pass->useX - go->GetPositionX());
                // [queue-key-gate-autoclose] the gate plane (its facing), as TickKeyDoorPass: a member on the far side
                // waits at the far point, it is not called back through the closed gate.
                float const gateFx = std::cos(go->GetOrientation());
                float const gateFy = std::sin(go->GetOrientation());
                float const gateBars = KeyGateBarsOffset(go, *pass);  // [b35_ss2]
                auto const gateSide = [&](float x, float y)
                { return (x - go->GetPositionX()) * gateFx + (y - go->GetPositionY()) * gateFy - gateBars; };
                bool const useSign = gateSide(pass->useX, pass->useY) >= 0.0f;
                constexpr float KEY_PASS_FAR_WAIT_RANGE = 12.0f;
                uint32 slot = 0;
                for (LeaderGroup::MemberInfo const& member : group.Members())
                {
                    Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    if (!bot || bot == leader || !bot->IsAlive() || bot->GetMap() != leader->GetMap())
                        continue;
                    if (bot->IsInCombat())
                    {
                        waitFor += (waitFor.empty() ? "" : ",") + member.name + " (combat)";
                        continue;
                    }
                    if ((gateSide(bot->GetPositionX(), bot->GetPositionY()) >= 0.0f) != useSign)
                    {
                        // [lead1581] the far side's wait is the safe spot of the key-door rule when there is one
                        bool const safe = routeState.keyPassSafeDoor == go->GetGUID();
                        Position const farWait = safe ? routeState.keyPassSafe
                                                      : Position(pass->farX, pass->farY, pass->farZ);
                        if (bot->GetExactDist2d(farWait) <= (safe ? KEY_PASS_GATHER_RANGE : KEY_PASS_FAR_WAIT_RANGE))
                            continue;
                        group.SetMemberWaypoint(member.guid, bot, farWait.GetPositionX(), farWait.GetPositionY(),
                                                farWait.GetPositionZ());
                        waitFor += (waitFor.empty() ? "" : ",") + member.name + " (far side)";
                        continue;
                    }
                    // [lead1570] a member sent back out of the opening's reach waits at its spot
                    if (routeState.keyPassBackDoor == go->GetGUID())
                        if (auto const back = routeState.keyPassBack.find(member.guid);
                            back != routeState.keyPassBack.end())
                        {
                            if (bot->GetExactDist2d(back->second) > KEY_PASS_GATHER_RANGE)
                            {
                                group.SetMemberWaypoint(member.guid, bot, back->second.GetPositionX(),
                                                        back->second.GetPositionY(), back->second.GetPositionZ());
                                waitFor += (waitFor.empty() ? "" : ",") + member.name + " (to its spot back)";
                            }
                            continue;
                        }
                    if (bot->GetExactDist2d(pass->useX, pass->useY) <= KEY_PASS_GATHER_RANGE)
                        continue;
                    float const angle = away + (float(slot++ % 5) - 2.0f) * 0.7f;
                    float const x = pass->useX + std::cos(angle) * KEY_PASS_SPREAD;
                    float const y = pass->useY + std::sin(angle) * KEY_PASS_SPREAD;
                    float const ground =
                        bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, pass->useZ + 3.0f, true, 10.0f);
                    group.SetMemberWaypoint(member.guid, bot, x, y, ground > INVALID_HEIGHT ? ground : pass->useZ);
                    waitFor += (waitFor.empty() ? "" : ",") + member.name;
                }
                // [b30_overpull] the members' gather is bounded: the key is used, a member left out is re-keyed for
                if (!waitFor.empty() && gatherGaveUp)
                {
                    group.LogEvent("strat_key_gather",
                             JsonObject().Str("note", pass->note).Str("waitFor", waitFor)
                                 .Str("action", "gave up waiting, keyed").Real("leaderToUse", toUse)
                                 .Num("waitSec", getMSTimeDiff(gatherSince, now) / IN_MILLISECONDS));
                    waitFor.clear();
                }
                // [b38_strat] The opening is a pull of what stands behind the gate: the closed gate hides the far side
                // (no sight, no aggro), the open one shows the group to every idle mob whose reach covers the use or
                // the far point. Not keyed while such a pack (3+ mobs with their formations, or one the tank's planner
                // refused over its cap) stands there and nobody of the group is on the far side: it is waited for to
                // walk off, at most KEY_PASS_FAR_PACK_MAX_MS. strat-n 20261026-050341-strat-n-5H-2: 175353 keyed at
                // 1437 s with the Mangled Cadaver chain 12.5 yd past it (the planner: "brings 16 mobs, over the hard
                // cap; waits for Mangled Cadaver (wanderer) to walk off"), the opening woke it on the mage (unplanned
                // pull 19, 1443 s), the tank's pass walked it through, the gate shut on the healer (no sight through
                // it), the tank died unhealed at 1469 s, then two more. The first 175353 key of 014521-5C-2,
                // 034959-5A-1 and 034959-5A-2 was an unplanned pull 6 s later too.
                if (waitFor.empty())
                {
                    constexpr float KEY_PASS_FAR_PACK_SCAN = 45.0f;
                    constexpr uint32 KEY_PASS_FAR_PACK_MIN = 3;
                    // [b39_strat] 60 s (was 120): the Service Entrance pack is static (strat-h/-n 054314: held from
                    // 22 s / ~70 s to the cap, never walked off); the opening fight is held on this side
                    // (OpeningFight) when it wakes anyway
                    constexpr uint32 KEY_PASS_FAR_PACK_MAX_MS = 60 * IN_MILLISECONDS;
                    bool anyFarSide = false;
                    for (LeaderGroup::MemberInfo const& member : group.Members())
                        if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                            bot && bot->IsAlive() && bot->GetMap() == leader->GetMap() &&
                            (gateSide(bot->GetPositionX(), bot->GetPositionY()) >= 0.0f) != useSign)
                            anyFarSide = true;
                    std::list<Creature*> idle;
                    if (!anyFarSide)
                    {
                        IdleAggroCheck check{leader};
                        Acore::CreatureListSearcher<IdleAggroCheck> searcher(go, idle, check);
                        Cell::VisitObjects(go, searcher, KEY_PASS_FAR_PACK_SCAN);
                    }
                    G3D::Vector3 const usePoint(pass->useX, pass->useY, pass->useZ);
                    G3D::Vector3 const farPoint(pass->farX, pass->farY, pass->farZ);
                    std::set<ObjectGuid> woken;
                    Creature* first = nullptr;
                    std::string refusedWhy;
                    for (Creature* mob : idle)
                    {
                        if ((gateSide(mob->GetPositionX(), mob->GetPositionY()) >= 0.0f) == useSign)
                            continue;  // the near side: the group stands in its sight already
                        float const reach = AggroReach(mob, leader) + KEY_PASS_SPREAD;
                        if (!AggroesAtPoint(mob, leader, usePoint, reach, false) &&
                            !AggroesAtPoint(mob, leader, farPoint, reach, false))
                            continue;
                        if (!first)
                            first = mob;
                        woken.insert(mob->GetGUID());
                        if (CreatureGroup const* formation = mob->GetFormation())
                            for (auto const& [member, info] : formation->GetMembers())
                                if (member && member->IsAlive() && !member->IsInCombat())
                                    woken.insert(member->GetGUID());
                        bool hopeless = false;
                        uint32 waitMs = 0;
                        std::string why;
                        if (refusedWhy.empty() &&
                            PartyRefusal(group, mob->GetGUID(), hopeless, waitMs, why) &&  // [lead5]
                            (hopeless || RefusalIsOverCap(why) || why.find("over the hard cap") != std::string::npos))
                            refusedWhy = why;
                    }
                    static std::unordered_map<uint64, uint32> farPackSince;  // the harness's world thread only
                    uint64 const farKey = (uint64(group.InstanceId()) << 32) | go->GetEntry();
                    bool farPack = first && (woken.size() >= KEY_PASS_FAR_PACK_MIN || !refusedWhy.empty());
                    // [lead1581] the user's key-door rule, the leader's computation: a spot behind the door out of
                    // every far-side mob's aggro reach (the careful pull's test, AggroesAtPoint, by the reach + the
                    // pricing's spread + the runner's stop), whose walk from the use point through the gate stays out
                    // of it too, with the opening at the use point waking nobody - the whole group passes to it
                    // together (the plain pass, its far point that spot) and pulls from there. None: the opening is
                    // held and the group waits for the pack on this side (lead1570).
                    if (routeState.keyPassSafeDoor == go->GetGUID() && (!farPack || !group.LeaderDecides()))
                        routeState.keyPassSafeDoor.Clear();
                    if (farPack && Leader::Leads(Leader::LEADER_DOOR_INPUTS) && group.LeaderDecides())
                    {
                        Map* map = leader->GetMap();
                        // every idle mob whose reach (the core's MAX_AGGRO_RADIUS at most) may cover a spot searched
                        std::list<Creature*> nearMobs;
                        IdleAggroCheck nearCheck{leader};
                        Acore::CreatureListSearcher<IdleAggroCheck> nearSearcher(go, nearMobs, nearCheck);
                        Cell::VisitObjects(go, nearSearcher, KEY_PASS_FAR_PACK_SCAN + MAX_AGGRO_RADIUS);
                        auto const outOfAll = [&](G3D::Vector3 const& point, float extra)
                        {
                            for (Creature* mob : nearMobs)
                                if (mob->IsAlive() && !mob->IsInCombat() &&
                                    (gateSide(mob->GetPositionX(), mob->GetPositionY()) >= 0.0f) != useSign &&
                                    AggroesAtPoint(mob, leader, point,
                                                   AggroReach(mob, leader) + KEY_PASS_SPREAD + extra, false))
                                    return false;
                            return true;
                        };
                        G3D::Vector3 const gatePoint(go->GetPositionX(), go->GetPositionY(), useZ);
                        auto const walkOut = [&](G3D::Vector3 const& from, G3D::Vector3 const& to)
                        {
                            float const len = (to - from).length();
                            for (float d = 0.0f; d <= len; d += KEY_PASS_SPREAD)
                                if (!outOfAll(from + (to - from) * (len > 0.0f ? d / len : 0.0f), 0.0f))
                                    return false;
                            return outOfAll(to, 0.0f);
                        };
                        bool safeFound = false;
                        G3D::Vector3 safe;
                        if (outOfAll(usePoint, 0.0f) && walkOut(usePoint, gatePoint))
                        {
                            float const ahead =
                                std::atan2(pass->farY - go->GetPositionY(), pass->farX - go->GetPositionX());
                            for (float out = KEY_PASS_SPREAD; !safeFound && out <= KEY_PASS_FAR_PACK_SCAN;
                                 out += KEY_PASS_SPREAD)
                                for (int slot = 0; !safeFound && slot < 5; ++slot)
                                {
                                    float const lateral =
                                        float(slot % 2 ? slot / 2 + 1 : -(slot / 2)) * KEY_PASS_SPREAD;
                                    float const x =
                                        go->GetPositionX() + std::cos(ahead) * out - std::sin(ahead) * lateral;
                                    float const y =
                                        go->GetPositionY() + std::sin(ahead) * out + std::cos(ahead) * lateral;
                                    if ((gateSide(x, y) >= 0.0f) == useSign)
                                        continue;
                                    float const z =
                                        map->GetHeight(leader->GetPhaseMask(), x, y, useZ + 3.0f, true, 10.0f);
                                    G3D::Vector3 const spot(x, y, z);
                                    if (z <= INVALID_HEIGHT || !outOfAll(spot, KEY_PASS_RUNNER_REACH) ||
                                        !walkOut(gatePoint, spot) ||
                                        !map->isInLineOfSight(gatePoint.x, gatePoint.y,
                                                              gatePoint.z + leader->GetCollisionHeight(), x, y,
                                                              z + leader->GetCollisionHeight(), leader->GetPhaseMask(),
                                                              LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                                        continue;
                                    safe = spot;
                                    safeFound = true;
                                }
                        }
                        bool const known = routeState.keyPassSafeDoor == go->GetGUID();
                        static std::set<std::pair<uint32, uint32>> ruleLogged;  // party seq, door entry
                        if (ruleLogged.insert({group.Run().runSeq, go->GetEntry()}).second ||
                            known != safeFound)
                            group.LogEvent("key_door_rule",
                                     JsonObject().Str("note", pass->note).Str("door", go->GetName())
                                         .Num("doorEntry", go->GetEntry()).Num("farPackMobs", uint32(woken.size()))
                                         .Str("case", safeFound ? "safe spot behind: the group passes together"
                                                                : "no safe spot: held, the pack comes to the use side")
                                         .Real("x", safe.x).Real("y", safe.y).Real("z", safe.z)
                                         .Str("tag", "[lead1581]"));
                        if (safeFound)
                        {
                            routeState.keyPassSafeDoor = go->GetGUID();
                            routeState.keyPassSafe = Position(safe.x, safe.y, safe.z);
                            farPack = false;  // the plain pass, to the safe spot
                        }
                        else if (known)
                            routeState.keyPassSafeDoor.Clear();
                    }
                    routeState.keyPassFarPack = farPack;  // [lead1569] TickKeyDoorPass holds the pass for it
                    if (!farPack && routeState.keyPassBackDoor == go->GetGUID())
                        routeState.keyPassBack.clear();  // [lead1570] nothing wakes: the plain gather
                    // [lead10] LEADER_DOOR_INPUTS: the opening is the leader's pull of the far side, priced with it
                    // (no KEY_PASS_FAR_PACK_MAX_MS give-up): within the planner's whole-pack cap (WholePackCap) and
                    // not refused over its cap, the key is used now and the fight is held on the use side
                    // (OpeningFight); over it, the pass waits while a mover of it may walk off, else no option.
                    bool const leaderPrices = farPack && Leader::Leads(Leader::LEADER_DOOR_INPUTS) &&
                                              group.LeaderDecides();
                    if (leaderPrices)
                    {
                        farPackSince.erase(farKey);
                        uint32 const cap = PullPlanner::WholePackCap(leader);
                        // [lead1572] the opening is the only pull here (the gate is the route's way on, nothing on
                        // this side measures the tank): a planner refusal does not hold it when the opening's own
                        // pack - the woken mobs and the refused verdict's pack - is carried by the survival terms
                        // alone (the tank kit's hold count is the prior lead1566 lets facts replace) and the tank and
                        // the healers carry it (TankCarries). b278 strat-n 5A-1 / 5B-2 / 5C-3 / 5F-6 / 5G-7: "far
                        // side: 7 mobs ... refused by the planner" 58-86 times, the gate never keyed; b277: these
                        // 10-mob packs, over by hold 1.43-2.50 alone, were won 6 of 7 times without a death
                        std::string carriedWhy;
                        bool carried = false;
                        // [lead1580] the opening's pack is what the opening wakes: the woken mobs and the core's chain
                        // from them fighting at the use point (PullPlanner::JudgeOpening), not the refused verdict's
                        // pack - b279 strat-n all 8 runs: that verdict was "Ravaged Cadaver as it comes: 27 mobs", a
                        // wanderer 69-81 yd past the gate chained round its own spawn (the nearest pick's look), its
                        // 27 merged in put the 7-mob opening over the cap 18, strat_key_gather 14-46 times, 0 pulls
                        std::vector<ObjectGuid> opening;
                        bool openingFits = false;
                        if (cap)
                        {
                            float openingOverload = 0.0f;
                            openingFits = PullPlanner::JudgeOpening(
                                leader, std::vector<ObjectGuid>(woken.begin(), woken.end()),
                                Position(usePoint.x, usePoint.y, usePoint.z), opening, openingOverload, carriedWhy,
                                carried);
                            carried = carried && opening.size() <= cap;
                        }
                        bool const fits = cap && !opening.empty() && opening.size() <= cap && (openingFits || carried);
                        bool mover = false;
                        for (ObjectGuid const& guid : woken)
                            if (Creature* mob = leader->GetMap()->GetCreature(guid);
                                mob && (PatrolWalkPath(mob) || (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE &&
                                                                mob->GetWanderDistance() > 0.0f)))
                                mover = true;
                        static std::set<std::pair<uint32, uint32>> pricedLogged;  // party seq, door entry
                        if (fits)
                        {
                            if (pricedLogged.insert({group.Run().runSeq, go->GetEntry()}).second)
                            {
                                group.LogEvent("door_far_side_priced",
                                         JsonObject().Str("note", pass->note).Str("door", go->GetName())
                                             .Num("doorEntry", go->GetEntry()).Num("farPackMobs", uint32(woken.size()))
                                             .Num("cap", cap).Str("first", first->GetName())
                                             .Num("openingMobs", uint32(opening.size()))
                                             .Str("action", refusedWhy.empty()
                                                                ? "keyed: the opening's pull fits [lead1580]"
                                                                : "keyed: the opening's pull is carried [lead1580]")
                                             .Str("refused", refusedWhy).Str("carried", carriedWhy));
                                if (!refusedWhy.empty())
                                    LOG_INFO("playerbots",
                                             "[lead1572] {}: {} {} keyed over the planner's refusal ({}): the "
                                             "opening's pull is carried: {}", leader->GetName(), go->GetName(),
                                             go->GetEntry(), refusedWhy, carriedWhy);
                            }
                            // [lead1570] the opening is the tank's pull: the tank keys it from the use point, the
                            // only member in the woken mobs' reach; every other member waits behind it, out of that
                            // reach, in sight of the use point (the pack comes to the tank through the gate and meets
                            // nobody else on its way). b278 strat-n 5D-4 t=39 / 5E-5 t=50 / 5H-8 t=31: the group
                            // gathered within 2 yd round the use point, the opening woke the pack on the warlock /
                            // the warlock / the healer ("unplanned"), the tank went through to it, the gate shut
                            Map* map = leader->GetMap();
                            auto const inReach = [&](Player* who, G3D::Vector3 const& point, float extra)
                            {
                                for (ObjectGuid const& guid : woken)
                                    if (Creature* mob = map->GetCreature(guid);
                                        mob && mob->IsAlive() && !mob->IsInCombat() &&
                                        AggroesAtPoint(mob, who, point, AggroReach(mob, who) + KEY_PASS_SPREAD + extra,
                                                       false))
                                        return true;
                                return false;
                            };
                            if (routeState.keyPassBackDoor != go->GetGUID())
                            {
                                routeState.keyPassBack.clear();
                                routeState.keyPassBackDoor = go->GetGUID();
                            }
                            float const leaderEye = useZ + leader->GetCollisionHeight();
                            uint32 backSlot = 0;
                            for (LeaderGroup::MemberInfo const& member : group.Members())
                            {
                                Player* bot = ObjectAccessor::FindPlayer(member.guid);
                                if (!bot || bot == leader || !bot->IsAlive() || bot->GetMap() != map ||
                                    bot->IsInCombat() ||
                                    (gateSide(bot->GetPositionX(), bot->GetPositionY()) >= 0.0f) != useSign)
                                    continue;
                                float const lateral = (float(backSlot++ % 5) - 2.0f) * KEY_PASS_SPREAD;
                                if (!inReach(bot, G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(),
                                                               bot->GetPositionZ()), 0.0f))
                                {
                                    // out of the opening's reach where it stands: that is its spot back
                                    // [lead1583] b280 strat-n: a spot kept from an earlier tick held the gather for
                                    // members already 20 yd back that never walked to it (73 gathers, no key used)
                                    routeState.keyPassBack[member.guid] = bot->GetPosition();
                                    continue;
                                }
                                auto const known = routeState.keyPassBack.find(member.guid);
                                bool placed = known != routeState.keyPassBack.end() &&
                                              bot->GetExactDist2d(known->second) > KEY_PASS_RUNNER_REACH;
                                // the first spot back on the use point's side (out of the reach by the runner's stop
                                // margin as well: a walk ends within it), in sight of the use point
                                for (float back = KEY_PASS_SPREAD; !placed && back <= KEY_PASS_FAR_PACK_SCAN;
                                     back += KEY_PASS_SPREAD)
                                {
                                    float const x = pass->useX + std::cos(away) * back - std::sin(away) * lateral;
                                    float const y = pass->useY + std::sin(away) * back + std::cos(away) * lateral;
                                    float const z = map->GetHeight(bot->GetPhaseMask(), x, y, useZ + 3.0f, true, 10.0f);
                                    if (z <= INVALID_HEIGHT ||
                                        inReach(bot, G3D::Vector3(x, y, z), KEY_PASS_RUNNER_REACH) ||
                                        !map->isInLineOfSight(x, y, z + bot->GetCollisionHeight(), useX, useY,
                                                              leaderEye, bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                                              VMAP::ModelIgnoreFlags::Nothing))
                                        continue;
                                    routeState.keyPassBack[member.guid] = Position(x, y, z);
                                    placed = true;
                                }
                                auto const spot = routeState.keyPassBack.find(member.guid);
                                if (spot == routeState.keyPassBack.end())
                                    continue;  // no spot out of its reach in sight: it stays where it is
                                group.SetMemberWaypoint(member.guid, bot, spot->second.GetPositionX(),
                                                        spot->second.GetPositionY(), spot->second.GetPositionZ());
                                waitFor += (waitFor.empty() ? "" : ",") + member.name +
                                           " (back out of the opening's reach) [lead1570]";
                            }
                        }
                        else
                        {
                            // [lead1580] the opening's own pull (woken + chain at the use point) is not carried
                            waitFor = Acore::StringFormat(
                                "far side: {} mobs in reach of the opening, its pull {} mobs with the chain at the use "
                                "point, cap {} ({} {:.1f} yd past the use point): not carried: {} [lead1580]{}",
                                woken.size(), opening.size(), cap, first->GetName(),
                                first->GetExactDist(usePoint.x, usePoint.y, usePoint.z), carriedWhy,
                                refusedWhy.empty() ? "" : "; the planner's own verdict: " + refusedWhy);
                            if (!mover)
                                Leader::Dungeon::LeaderNoOption(group, leader, first, opening,
                                               "the key pass opens into a " + std::to_string(opening.size()) +
                                                   "-mob pull, not carried (" + carriedWhy + ") [lead1580]",
                                               true, JsonObject().Str("kind", "door").Str("door", go->GetName()));
                        }
                    }
                    else if (!farPack)
                        farPackSince.erase(farKey);
                    else
                    {
                        auto [since, fresh] = farPackSince.emplace(farKey, now ? now : 1);
                        if (getMSTimeDiff(since->second, now) < KEY_PASS_FAR_PACK_MAX_MS)
                            waitFor = Acore::StringFormat(
                                "far side: {} mobs in reach of the opening ({} {:.1f} yd past the use point){}",
                                woken.size(), first->GetName(), first->GetExactDist(usePoint.x, usePoint.y,
                                                                                    usePoint.z),
                                refusedWhy.empty() ? "" : ", refused by the planner: " + refusedWhy);
                        else if (fresh || getMSTimeDiff(since->second, now) <
                                              KEY_PASS_FAR_PACK_MAX_MS + KEY_PASS_RETRY_MS * 2)
                            group.LogEvent("strat_key_gather",
                                     JsonObject().Str("note", pass->note)
                                         .Str("action", "far side still held, keyed (b38_strat)")
                                         .Num("farPackMobs", uint32(woken.size())).Str("first", first->GetName()));
                    }
                }
            }
            if (!waitFor.empty())
            {
                // [b30_overpull] retried at KEY_PASS_RETRY_MS: UseStarter set the starter's 60 s cooldown, and the
                // retarget spun through the trash behind the gate meanwhile (strat-h 20261025-184229-5: gather at
                // 26 s, the key at 87 s, 40 "leader held at a closed door" skips in between)
                starter.nextAllowedMs = now + KEY_PASS_RETRY_MS;
                // [queue-door-starter-match] the gather keeps its target: the leader stands at the use point while the
                // members come (strat-n 163739: the 1.5 s retry let the retarget take another starter, the leader left
                // the use point and the members' gather waypoints fought their follow - move_conflict every 20 s).
                // Only a leader in combat lets the target go.
                if (!leader->IsInCombat() && atUse)
                    group.ClearWaypoint(group.LeaderGuid());
                if (!routeState.keyPassLogMs || getMSTimeDiff(routeState.keyPassLogMs, now) >= KEY_PASS_LOG_MS)
                {
                    routeState.keyPassLogMs = now ? now : 1;
                    group.LogEvent("strat_key_gather",
                             JsonObject().Str("note", pass->note).Str("waitFor", waitFor)
                                 .Real("leaderToUse", leader->GetExactDist2d(useX, useY))
                                 .Bool("farSide", leaderFarSide));  // [b32_strat_gate]
                }
                return false;
            }
        }

        // [b51_strat] door_gather: an auto-closing door is keyed with the whole group next to the leader. Not for a
        // re-open for a member stranded behind it (the rest stay where they are) nor an in-combat re-key.
        if (go->GetGoType() == GAMEOBJECT_TYPE_DOOR && go->GetGoState() == GO_STATE_READY && !leader->IsInCombat() &&
            routeState.doorCutDoor != go->GetGUID() && DoorGatherHolds(group, leader, go))
        {
            starter.nextAllowedMs = now + KEY_PASS_RETRY_MS;
            if (!leader->IsInCombat())
                group.ClearWaypoint(group.LeaderGuid());
            return false;
        }

        // Bots walk by themselves: the leader stopped on arriving, nothing stops or turns it here.

        // [b30_stalls] A player leaves a form the item spell refuses (Spell::CheckCast -> CheckShapeshift:
        // SPELL_FAILED_NOT_SHAPESHIFT). ulda-h 20261025-184229-ulda-h-5D-61: the bear tank used the Staff of
        // Prehistoria (9577, Attributes 0x10100 = SPELL_ATTR0_NOT_SHAPESHIFTED) on the Keystone 34 times, all
        // "starter_item_failed" while standing at 1.4-3.9 yd, Ironaya "event not started" to the end.
        if (ShapeshiftForm const form = leader->GetShapeshiftForm();
            form != FORM_NONE && info->CheckShapeshift(form) != SPELL_CAST_OK)
            leader->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);

        // As the client sends it: the object as the target when the spell takes one, else no explicit target (a
        // spell focus or an area spell finds the object itself). The core still runs every cast check.
        WorldPacket packet(CMSG_USE_ITEM);
        packet << item->GetBagSlot() << item->GetSlot() << uint8(1) << spellId << item->GetGUID() << uint32(0)
               << uint8(0);
        if (info->GetExplicitTargetMask() & TARGET_FLAG_GAMEOBJECT_MASK)
            packet << uint32(TARGET_FLAG_GAMEOBJECT) << go->GetPackGUID();
        else
            packet << uint32(TARGET_FLAG_NONE);
        leader->GetSession()->HandleUseItemOpcode(packet);

        // A cast with a cast time (Planting Incendiary Bomb: 5 s, interrupted by moving): no retarget and no new
        // waypoint until it is done, or the next walk breaks it and the object counts as used anyway.
        if (leader->IsNonMeleeSpellCast(false))
        {
            uint32 const castMs = info->CalcCastTime(leader);
            routeState.castHoldUntilMs = now + castMs + 1000;
            group.ClearWaypoint(group.LeaderGuid());
        }

        // An instant item spell is cast at once and starts its cooldown (Spell::prepare); none: CheckCast refused it
        // (line of sight, range, target). Not a use: retried, at most STARTER_ITEM_MAX_FAILS times per object.
        // A cast-time spell that is not casting now was refused too (a key's 5 s Opening 3366 has no cooldown).
        bool const castRefused = info->CalcCastTime(leader) > 0 && !leader->IsNonMeleeSpellCast(false) &&
                                 ((go->GetGoState() == GO_STATE_READY && go->GetGoType() == GAMEOBJECT_TYPE_DOOR) ||
                                  info->RequiresSpellFocus || starter.def->expectDoor);
        if (ROUTE_STARTER_ITEM_CHECK && (expectCooldown || castRefused) && !leader->HasSpellCooldown(spellId) &&
            !leader->IsNonMeleeSpellCast(false))
        {
            uint32 const fails = ++starter.failedUses[go->GetGUID()];
            bool const door = go->GetGoType() == GAMEOBJECT_TYPE_DOOR;
            bool const los = leader->IsWithinLOSInMap(go);
            // Which side of the door the leader stands on (the GO's facing: front > 0, back < 0).
            float const facing = go->GetOrientation();
            float const side = (leader->GetPositionX() - go->GetPositionX()) * std::cos(facing) +
                               (leader->GetPositionY() - go->GetPositionY()) * std::sin(facing);
            // A key door is never counted as used by failed casts (strat-h build 19: four "los false" fails at 9.7 /
            // 3.2 yd marked the Service Entrance Gate used, the route stalled at it): retried, and without line of
            // sight the leader steps to KEY_DOOR_CAST_RANGE yd from its centre, toward where it stands.
            // [expect_door] an expectDoor starter's limit is CheckExpectedDoors' EXPECT_DOOR_MAX_RETRIES instead.
            if (fails >= STARTER_ITEM_MAX_FAILS && !door && !starter.def->expectDoor)
                starter.usedObjects.insert(go->GetGUID());
            starter.nextAllowedMs = now + STARTER_ITEM_RETRY_MS;
            group.LogEvent("starter_item_failed", JsonObject().Str("note", starter.def->note)
                                                       .Num("entry", go->GetEntry()).Num("spell", spellId)
                                                       .Num("fails", fails)
                                                       .Real("distance", leader->GetExactDist(go))
                                                       .Bool("mounted", leader->IsMounted()).Bool("los", los)
                                                       .Str("side", side >= 0.0f ? "front" : "back")
                                                       .Real("sideDist", side));
            if (door && !los)
            {
                // Toward the leader from the door's centre (its own side whatever the model's facing), at
                // KEY_DOOR_CAST_RANGE: the use point a player stands on.
                // [lead1563] ... along the door's own facing (its plane's normal) on the leader's side, as the
                // KeyDoorPassDef use points are built (175368's 3 yd use point keys): a step toward the leader
                // from off the normal lands in the door model's frame. b277 strat-n 20261106-105814-5G-15
                // 1497-2007 s: Gauntlet Gate 175357, the leader 8.3 yd off (6.8 on the normal), the step 3 yd toward
                // it ran "through" the closed gate ("route walk: stays (closed door Gauntlet Gate 175357 at 5.9
                // yd)"), door_hold held it where it stood, 102 failed keys (los false) to the stop
                float const toLeader = side >= 0.0f ? facing : facing + float(M_PI);
                G3D::Vector3 dest(go->GetPositionX() + std::cos(toLeader) * KEY_DOOR_CAST_RANGE,
                                  go->GetPositionY() + std::sin(toLeader) * KEY_DOOR_CAST_RANGE,
                                  leader->GetPositionZ());
                float const ground = leader->GetMap()->GetHeight(leader->GetPhaseMask(), dest.x, dest.y,
                                                                 dest.z + 3.0f, true, 10.0f);
                if (ground > INVALID_HEIGHT)
                {
                    dest.z = ground;
                    group.SetLeaderWaypoint(leader, dest.x, dest.y, dest.z);
                    routeState.waypointHoldUntilMs = now + STARTER_ITEM_RETRY_MS;
                    group.LogEvent("starter_door_step", JsonObject().Str("door", go->GetName())
                                                             .Str("side", side >= 0.0f ? "front" : "back")
                                                             .Real("toX", dest.x).Real("toY", dest.y)
                                                             .Real("toZ", dest.z));
                }
            }
            return false;
        }
        return true;
    }

    // [brd_bar_buy] [brd_bar_turnin] A vendor or quest step at its creature, as the client does it: CMSG_BUY_ITEM's
    // Player::BuyItemFromVendorSlot (money, reputation, the vendor's stock and reach are checked by the core), or
    // CMSG_QUESTGIVER_ACCEPT_QUEST + CMSG_QUESTGIVER_CHOOSE_REWARD (the creature AI's sQuestReward runs from the reward
    // handler: Blackrock Depths' npc_rocknot counts the ale, TYPE_BAR SPECIAL at three). A step counts only if it
    // changed the bags (bought items in, quest items out).
    bool UseStarterAtNpc(LeaderGroup& group, Player* leader, StarterTrack& starter, Creature* creature)
    {
        GroupRouteState& routeState = group.Route();
        uint32 const now = getMSTime();
        EventStarterDef const* def = starter.def;
        if (leader->IsInCombat() || leader->GetExactDist(creature) > INTERACTION_DISTANCE)
        {
            starter.nextAllowedMs = now + STARTER_ITEM_RETRY_MS;
            return false;
        }
        InstanceScript const* script = leader->GetInstanceScript();
        if (def->kind == StarterKind::VendorBuy)
        {
            VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(creature->GetEntry());
            int32 slot = -1;
            for (uint32 index = 0; items && index < items->GetItemCount(); ++index)
                if (VendorItem const* offer = items->GetItem(index); offer && offer->item == def->itemId)
                {
                    slot = int32(index);
                    break;
                }
            uint32 const before = leader->GetItemCount(def->itemId);
            uint32 const moneyBefore = leader->GetMoney();
            if (slot >= 0)
                leader->BuyItemFromVendorSlot(creature->GetGUID(), uint32(slot), def->itemId,
                                              uint8(std::max<uint32>(1, def->buyCount)), NULL_BAG, NULL_SLOT);
            uint32 const after = leader->GetItemCount(def->itemId);
            group.LogEvent("brd_bar_buy", JsonObject().Str("note", def->note).Num("item", def->itemId)
                                               .Num("asked", def->buyCount).Num("bought", after - before)
                                               .Num("vendorSlot", slot).Num("moneyBefore", moneyBefore)
                                               .Num("money", leader->GetMoney()));
            if (after <= before)
            {
                // [event_boss_classic] A buy that bought nothing (no money, no stock, no vendor slot) backs off 30 s
                // per failure and is given up after VENDOR_BUY_GIVE_UP failures, logged: brd-n 20261025-163739
                // brd-n-5A-21 bought 0 with moneyBefore 0, and the target ping-ponged Plugger <-> the Shadowforge
                // Gate (833 yd) until idle at 228 s.
                constexpr uint32 VENDOR_BUY_GIVE_UP = 3;
                uint32 const fails = ++starter.failedUses[creature->GetGUID()];
                starter.nextAllowedMs = now + std::max<uint32>(def->cooldownMs, fails * 30 * IN_MILLISECONDS);
                group.LogEvent("starter_vendor_failed",
                         JsonObject().Str("note", def->note).Num("fails", fails).Num("money", leader->GetMoney())
                             .Num("vendorSlot", slot).Bool("givenUp", fails >= VENDOR_BUY_GIVE_UP));
                if (fails >= VENDOR_BUY_GIVE_UP)
                    starter.uses = def->maxUses + 1u;  // the caller takes the failed use back: used up
                if (routeState.target == creature->GetGUID())
                    routeState.target.Clear();
                return false;
            }
            return true;
        }
        Quest const* quest = sObjectMgr->GetQuestTemplate(def->questId);
        if (!quest)
            return false;
        uint32 const itemBefore = def->itemId ? leader->GetItemCount(def->itemId) : 0;
        if (leader->GetQuestStatus(def->questId) == QUEST_STATUS_NONE)
        {
            WorldPacket accept(CMSG_QUESTGIVER_ACCEPT_QUEST);
            accept << creature->GetGUID() << def->questId << uint32(0);
            leader->GetSession()->HandleQuestgiverAcceptQuestOpcode(accept);
        }
        if (leader->GetQuestStatus(def->questId) == QUEST_STATUS_COMPLETE)
        {
            WorldPacket reward(CMSG_QUESTGIVER_CHOOSE_REWARD);
            reward << creature->GetGUID() << def->questId << uint32(0);
            leader->GetSession()->HandleQuestgiverChooseRewardOpcode(reward);
        }
        leader->PlayerTalkClass->SendCloseGossip();
        uint32 const itemAfter = def->itemId ? leader->GetItemCount(def->itemId) : 0;
        bool handedIn = leader->GetQuestStatus(def->questId) != QUEST_STATUS_COMPLETE;
        if (def->itemId)
            handedIn = itemAfter < itemBefore;
        group.LogEvent("brd_bar_turnin", JsonObject().Str("note", def->note).Num("quest", def->questId)
                                              .Bool("handedIn", handedIn).Num("itemsBefore", itemBefore)
                                              .Num("itemsAfter", itemAfter)
                                              .Num("questStatus", uint32(leader->GetQuestStatus(def->questId)))
                                              .Num("barData", script ? script->GetData(3 /* TYPE_BAR */) : 0));
        if (!handedIn)
        {
            starter.nextAllowedMs = now + def->cooldownMs;
            return false;
        }
        return true;
    }

    bool StartersUsed(LeaderGroup& group, std::vector<uint32> const& entries)
    {
        GroupRouteState& routeState = group.Route();
        for (uint32 entry : entries)
            for (StarterTrack const& other : routeState.starters)
                // [b32_strat_gate] a re-armed key door (doorRearms: used once, armed again because it shut) stays
                // "used" for the starters after it. strat-h 20261025-221216-strat-h-5C-1: King's Square Gate 175352
                // keyed at 1017 s, the whole group through, re-armed at 1040 s (uses 0) by a leader's walk back to
                // trash behind it - 175353 (afterUsed 175352) never came again, Timmy / Willey "next boss behind a
                // closed door (King's Square Gate 175353)" and 366 s idle until the stop at 1404 s.
                if (std::find(other.def->entries.begin(), other.def->entries.end(), entry) !=
                        other.def->entries.end() &&
                    other.uses < other.def->maxUses && !other.doorRearms)
                    return false;
        return true;
    }

    void MarkStartersDoneByGroup(LeaderGroup& group, Player* leader)
    {
        GroupRouteState& routeState = group.Route();
        if (!leader)
            return;

        for (StarterTrack& starter : routeState.starters)
        {
            if (starter.uses >= starter.def->maxUses ||
                (starter.def->kind != StarterKind::Gossip && starter.def->kind != StarterKind::Escort))
                continue;

            for (StarterTrack const& next : routeState.starters)
            {
                EventStarterDef const* def = next.def;
                if (def->kind != StarterKind::UseItem || !def->itemId ||
                    routeState.eventItemsGiven.count(def->itemId) ||
                    !leader->HasItemCount(def->itemId, 1))
                    continue;
                bool namesThis = false;
                for (uint32 entry : def->afterUsed)
                    namesThis |= std::find(starter.def->entries.begin(), starter.def->entries.end(), entry) !=
                                 starter.def->entries.end();
                if (!namesThis)
                    continue;

                group.LogEvent("starter_done_by_group", JsonObject().Str("note", starter.def->note)
                                                             .Num("usesBefore", starter.uses)
                                                             .Num("evidenceItem", def->itemId)
                                                             .Str("next", def->note));
                starter.uses = starter.def->maxUses;
                starter.wipesAtUse = group.Wipes();
                group.NoteProgress(getMSTime());
                break;
            }
        }
    }

    bool WalkToEventGather(LeaderGroup& group, Player* leader)
    {
        GroupRouteState& routeState = group.Route();
        Position spot;
        Map* map = leader->GetMap();
        if (!map || map->GetInstanceId() != group.InstanceId() || !leader->IsAlive() || !EventGatherApplies(group) ||
            !EventGatherSpot(group.MapId(), spot) || !EventGatherReady(group.MapId(), leader))
            return false;

        float const distance = leader->GetExactDist(&spot);
        if (distance <= EVENT_GATHER_ARRIVE)
        {
            if (!routeState.eventGatherArrived)
            {
                routeState.eventGatherArrived = true;
                routeState.eventGatherArrivedMs = getMSTime();
                group.LogEvent("event_gather", JsonObject().Str("state", "arrived").Real("x", spot.GetPositionX())
                                                    .Real("y", spot.GetPositionY()).Real("z", spot.GetPositionZ()));
            }
            // The event starts only with EVERY player near the spot (Halls of Reflection: all alive within 40 yd 2D
            // of the altar, instance Update; after a wipe too). Wave 10 after the wipe: the leader stood at the spot,
            // four members 45-51 yd from the altar, the waves never resumed - "partial" 5 minutes later. A member
            // left behind walks in (its own path, the AI held for the walk), as a player answers "come here".
            uint32 const now = getMSTime();
            if (ROUTE_GATHER_MEMBERS && getMSTimeDiff(routeState.gatherCallMs, now) > GATHER_CALL_MS)
                for (LeaderGroup::MemberInfo const& member : group.Members())
                {
                    Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    if (!bot || bot == leader || !bot->IsAlive() || bot->IsInCombat() ||
                        bot->GetMap() != leader->GetMap())
                        continue;
                    if (bot->GetExactDist2d(&spot) <= EVENT_GATHER_MEMBER_RANGE)
                    {
                        if (group.MemberHasWaypoint(member.guid))
                            group.ClearMemberWaypoint(member.guid);
                        continue;
                    }
                    routeState.gatherCallMs = now;
                    float const angle = spot.GetAngle(bot);
                    float const x = spot.GetPositionX() + std::cos(angle) * DRY_SPOT_SPREAD;
                    float const y = spot.GetPositionY() + std::sin(angle) * DRY_SPOT_SPREAD;
                    // As a leader says "come here": the spot is the member's waypoint, its own "test runner move"
                    // walks there.
                    group.SetMemberWaypoint(member.guid, bot, x, y, spot.GetPositionZ());
                    group.LogEvent("event_gather", JsonObject().Str("state", "member called").Str("name", member.name)
                                                        .Real("distance", bot->GetExactDist2d(&spot))
                                                        .Bool("own", true));
                }
            return false;
        }

        PathResult leg;
        G3D::Vector3 const goal(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ());
        SteerLeg(leader, goal, leg);
        if (leg.type & PATHFIND_NOPATH)
        {
            if (!routeState.eventGatherLogged)
            {
                routeState.eventGatherLogged = true;
                group.LogEvent("event_gather", JsonObject().Str("state", "no path").Real("distance", distance));
            }
            return false;
        }

        G3D::Vector3 const waypoint =
            FloorWaypoint(leader, leg, RouteWaypoint(leg, goal, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP));

        if (!routeState.eventGatherLogged)
        {
            routeState.eventGatherLogged = true;
            routeState.lastEvent = "event gather";
            group.LogEvent("event_gather", JsonObject().Str("state", "walking").Real("distance", distance)
                                                .Real("x", spot.GetPositionX()).Real("y", spot.GetPositionY())
                                                .Real("z", spot.GetPositionZ()));
        }

        group.SetLeaderWaypoint(leader, waypoint.x, waypoint.y, waypoint.z);
        group.NoteProgress(getMSTime());
        return true;
    }

    bool EventGatherApplies(LeaderGroup& group)
    {
        GroupRouteState& routeState = group.Route();
        Position spot;
        if (!group.HasContent() || !EventGatherSpot(group.MapId(), spot))
            return false;

        bool started = false;
        for (StarterTrack const& starter : routeState.starters)
            started |= starter.uses > 0;
        // Started by someone else: the HoR dungeon strategy talks to Jaina / Sylvanas itself ("hor start intro"). Wave
        // 9: it did so while the harness walked there, the leader lost its gossip flag (halls_of_reflection.cpp:193),
        // the starter was never used, the group never went to the altar and stalled at the corner in 4 minutes.
        if (!started && group.MapId() == HoRIds::HOR_MAP_ID)
            if (Player* leader = ObjectAccessor::FindPlayer(group.LeaderGuid()))
                if (Map* map = leader->GetMap(); map && map->GetId() == HoRIds::HOR_MAP_ID)
                {
                    bool leaderSeen = false;
                    bool gossipOffered = false;
                    for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
                        if (creature && creature->IsInWorld() &&
                            (creature->GetEntry() == HoRIds::NPC_JAINA_PART1 ||
                             creature->GetEntry() == HoRIds::NPC_SYLVANAS_PART1))
                        {
                            leaderSeen = true;
                            gossipOffered |= creature->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP);
                        }
                    InstanceScript* instance = leader->GetInstanceScript();
                    started = (leaderSeen && !gossipOffered) ||
                              (instance && instance->GetPersistentData(HoRIds::PERSISTENT_DATA_INTRO));
                }
        bool const bossesOpen = group.BossesOpen();
        // [hor-gather-waves] (PATCH_hor_gd.md 1.4 (b), applied by plan29) no gather point while the HoR waves run:
        // the harness and "hor move to wave position" pulled the bots back and forth every 5 s (hor-h 1077-1456 s);
        // after a wipe GetWaveHold returns the gather spot by itself.
        if (group.MapId() == HoRIds::HOR_MAP_ID)
            if (Player* leader = ObjectAccessor::FindPlayer(group.LeaderGuid()))
                if (InstanceScript* instance = leader->GetMapId() == HoRIds::HOR_MAP_ID
                                                   ? HoRHelpers::GetInstance(leader)
                                                   : nullptr;
                    instance && instance->GetData(HoRIds::DATA_WAVE_NUMBER) != 0)
                    return false;
        return started && bossesOpen;
    }

    void ReopenKeyDoorInCombat(LeaderGroup& group, Player* leader)
    {
        GroupRouteState& route = group.Route();
        Map* map = leader->GetMap();
        if (!map || map->GetInstanceId() != group.InstanceId() || !leader->IsAlive() ||
            leader->IsNonMeleeSpellCast(false))
            return;
        uint32 const now = getMSTime();
        for (StarterTrack& starter : route.starters)
        {
            if (starter.def->kind != StarterKind::UseItem || starter.uses < starter.def->maxUses ||
                starter.doorRearms >= DOOR_REARM_MAX || starter.combatRearms >= DOOR_REARM_COMBAT_MAX ||
                !route.lastStarterUseMs || getMSTimeDiff(route.lastStarterUseMs, now) <= DOOR_REARM_GAP_MS ||
                (starter.combatRearmMs && getMSTimeDiff(starter.combatRearmMs, now) < DOOR_REARM_COMBAT_GAP_MS) ||
                !leader->getAttackers().empty() || !leader->HasItemCount(starter.def->itemId, 1))
                continue;
            for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
            {
                if (!go || !go->isSpawned() || go->GetGoType() != GAMEOBJECT_TYPE_DOOR ||
                    go->GetGoState() != GO_STATE_READY || !go->IsWithinDistInMap(leader, INTERACTION_DISTANCE) ||
                    std::find(starter.def->entries.begin(), starter.def->entries.end(), go->GetEntry()) ==
                        starter.def->entries.end())
                    continue;
                std::vector<GameObject*> const door = {go};
                Player* behind = nullptr;
                for (ObjectGuid const& memberGuid : group.MemberGuids())
                    if (Player* bot = ObjectAccessor::FindPlayer(memberGuid);
                        bot && bot != leader && bot->IsAlive() && bot->GetMap() == map && !behind)
                    {
                        PathResult const way = RoutePath(bot, leader->GetPositionX(), leader->GetPositionY(),
                                                         leader->GetPositionZ());
                        if (way.reachable && RouteThroughClosedDoor(way.points, door, leader->GetPhaseMask()))
                            behind = bot;
                    }
                if (!behind)
                    continue;
                ++starter.doorRearms;
                ++starter.combatRearms;
                starter.combatRearmMs = now ? now : 1;
                starter.uses = 0;
                starter.usedObjects.clear();
                group.LogEvent("starter_rearmed", JsonObject().Str("note", starter.def->note)
                                                      .Str("why", "the door closed again in a fight, " +
                                                                      std::string(behind->GetName()) + " behind it")
                                                      .Num("rearms", starter.doorRearms).Bool("inCombat", true));
                UseStarter(group, leader, starter, go);
                route.lastStarterUseMs = now;  // DOOR_REARM_GAP_MS throttles a refused or interrupted cast too
                return;
            }
        }
    }

    void CheckExpectedDoors(LeaderGroup& group, Player* leader)
    {
        GroupRouteState& route = group.Route();
        Map* map = leader ? leader->GetMap() : nullptr;
        if (!map || map->GetInstanceId() != group.InstanceId())
            return;
        uint32 const now = getMSTime();
        for (StarterTrack& starter : route.starters)
        {
            if (!starter.def->expectDoor || !starter.expectDoorCheckMs || int32(now - starter.expectDoorCheckMs) < 0)
                continue;
            starter.expectDoorCheckMs = 0;
            GameObject* door = nullptr;
            for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                if (go && go->isSpawned() && go->GetEntry() == starter.def->expectDoor)
                {
                    door = go;
                    break;
                }
            if (door && door->GetGoState() != GO_STATE_READY)
            {
                group.NoteProgress(now);
                group.LogEvent("expect_door_ok", JsonObject().Str("note", starter.def->note)
                                                     .Str("door", door->GetName()).Num("doorEntry", door->GetEntry())
                                                     .Num("goState", uint32(door->GetGoState()))
                                                     .Num("retries", starter.expectDoorFails));
                continue;
            }
            ++starter.expectDoorFails;
            bool const item = !starter.def->itemId || leader->HasItemCount(starter.def->itemId, 1);
            bool const rearm = starter.expectDoorFails <= EXPECT_DOOR_MAX_RETRIES && item;
            if (rearm)
            {
                starter.uses = 0;
                starter.usedObjects.clear();
                starter.failedUses.clear();
                starter.steppedOut.clear();
                starter.nextAllowedMs = now + STARTER_ITEM_RETRY_MS;
            }
            group.LogEvent(rearm ? "expect_door_retry" : "expect_door_gave_up",
                           JsonObject().Str("note", starter.def->note).Num("doorEntry", starter.def->expectDoor)
                               .Bool("doorFound", door != nullptr).Num("retries", starter.expectDoorFails)
                               .Bool("itemInBags", item)
                               .Real("leaderDistance", door ? leader->GetExactDist(door) : -1.0f)
                               .Bool("leaderInCombat", leader->IsInCombat()));
        }
    }
}
