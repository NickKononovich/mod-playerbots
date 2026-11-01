/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "RouteEvents.h"
#include "LeaderReports.h"

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
}
