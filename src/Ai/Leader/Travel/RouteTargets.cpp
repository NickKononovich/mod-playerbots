/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "RouteTargets.h"
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

    bool IsTrapGate(GameObject const* door)
    {
        return door &&
               std::find(TRAP_GATE_DOORS.begin(), TRAP_GATE_DOORS.end(), door->GetEntry()) != TRAP_GATE_DOORS.end();
    }

    std::unordered_map<uint32, uint32> introWaitSinceMs;

    std::unordered_map<uint32, uint32> ritualHoldUntilMs;

    std::set<std::tuple<uint32, ObjectGuid, ObjectGuid>> guardKeepLogged;

    std::set<std::tuple<uint32, ObjectGuid, ObjectGuid>> targetCommitLogged;

    std::unordered_map<uint32, std::pair<uint32, ObjectGuid>> doorSpinHolds;

    std::unordered_map<uint32, DoorSpinCount> doorSpinCounts;  // party seq (world thread)

    // One door skip counted; true: the hold starts now (doorSpinHolds set), `log`: its line is due.
    bool CountDoorSpin(uint32 seq, ObjectGuid door, uint32 now, bool& log, uint32& skips, uint32& windowMs)
    {
        DoorSpinCount& spin = doorSpinCounts[seq];
        if (!spin.sinceMs || getMSTimeDiff(spin.sinceMs, now) > DOOR_SPIN_WINDOW_MS)
        {
            spin.sinceMs = now ? now : 1;
            spin.skips = 0;
        }
        skips = ++spin.skips;
        windowMs = getMSTimeDiff(spin.sinceMs, now);
        log = false;
        if (spin.skips < DOOR_SPIN_SKIPS)
            return false;
        doorSpinHolds[seq] = {now + DOOR_SPIN_HOLD_MS, door};
        if (!spin.loggedMs || getMSTimeDiff(spin.loggedMs, now) >= DOOR_SPIN_LOG_MS)
        {
            spin.loggedMs = now ? now : 1;
            log = true;
        }
        return true;
    }

    std::set<std::pair<uint32, ObjectGuid>> gateKeyHeldLogged;

    std::set<std::pair<uint32, ObjectGuid>> phasedBossLogged;

    std::set<std::pair<uint32, ObjectGuid>> starterKeyLegLogged;

    std::set<std::pair<uint32, ObjectGuid>> pullAloneLogged;  // [sv-bog-overlord-alone]

    std::set<std::pair<uint32, ObjectGuid>> fledSurvivorLogged;  // [fled_survivor]

    std::set<std::pair<uint32, ObjectGuid>> lockoutClearLogged;  // [b44_ring] "brd_ring_lockout_clear" lines

    std::unordered_map<uint32, RestHold> restHolds;

    // Food (spell category 11) or water (59) in the bags: TestHarnessMgr's CountRestItems (file local there).
    bool HasRestItem(Player* bot, uint32 spellCategory)
    {
        auto const matches = [spellCategory](Item const* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            return proto && proto->Class == ITEM_CLASS_CONSUMABLE &&
                   (proto->SubClass == ITEM_SUBCLASS_FOOD || proto->SubClass == ITEM_SUBCLASS_CONSUMABLE) &&
                   proto->Spells[0].SpellCategory == spellCategory;
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (matches(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
                return true;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag const* bag = bot->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (matches(bag->GetItemByPos(uint8(slot))))
                        return true;
        return false;
    }

    std::unordered_map<uint32, uint32> doorStarterLogMs;  // instance id -> last "starter_door_priority" line

    std::unordered_map<uint32, uint32> leadSpotFirstLogMs;  // instance id -> last "brd_ring_lead" line

    std::unordered_map<uint32, uint32> eventBossWaitLogMs;  // instance id -> last "event_boss_waits" line

    std::unordered_map<uint32, uint32> eventStartHoldLogMs;  // [event_spot_hold] instance id -> last line

    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> keyDoorChain;

    std::unordered_map<uint32, uint32> fallbackFloorLogMs;   // [b34_arc] instance id -> last fallback line

    std::unordered_map<uint32, std::unordered_map<ObjectGuid, uint32>> eventWaveFlipMs;

    bool EventWaveFresh(uint32 instanceId, ObjectGuid const& guid)
    {
        auto const inst = eventWaveFlipMs.find(instanceId);
        if (inst == eventWaveFlipMs.end())
            return false;
        auto const entry = inst->second.find(guid);
        return entry != inst->second.end() && entry->second != 0;
    }

    std::unordered_map<uint32, uint32> passageDetourLogMs;  // instance id -> last "passage_door_detour" line

    bool StarterPathArrives(PathResult const& path, WorldObject const* object)
    {
        return path.complete || object->GetExactDist(path.end.x, path.end.y, path.end.z) <= STARTER_PATH_ARRIVE;
    }

    // StarterTrack::stuckUntilMs: the leader's walk to this object got stuck a short while ago.
    bool StarterObjectStuck(StarterTrack const& starter, ObjectGuid const& guid, uint32 now)
    {
        auto const itr = starter.stuckUntilMs.find(guid);
        return itr != starter.stuckUntilMs.end() && int32(itr->second - now) > 0;
    }

    float NearestDist(std::vector<Position> const& anchors, WorldObject const* object)
    {
        float best = -1.0f;
        for (Position const& anchor : anchors)
        {
            float const dist = object->GetExactDist(&anchor);
            if (best < 0.0f || dist < best)
                best = dist;
        }
        return best;
    }

    std::vector<Creature*> RouteCreatures(Map* map, Player* leader)
    {
        std::vector<Creature*> result;
        result.reserve(map->GetCreatureBySpawnIdStore().size() + 16);
        for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
            if (creature)
                result.push_back(creature);

        std::list<Creature*> summoned;
        SummonedCreatureCheck check;
        Acore::CreatureListSearcher<SummonedCreatureCheck> searcher(leader, summoned, check);
        Cell::VisitObjects(leader, searcher, SUMMON_SCAN_RANGE);
        result.insert(result.end(), summoned.begin(), summoned.end());
        return result;
    }

    // [b32_smpull] `mob` is a mob of Commander Mograine's call while he stands idle with his chapel
    // (ClassicShared.h): the boss's linked trash, cleared before him.
    bool LinkedChapelTrash(Player* leader, Creature* mob)
    {
        if (!leader || !mob || leader->GetMapId() != ScarletMonastery::SCARLET_MONASTERY_MAP_ID ||
            !ScarletMonastery::IsMograineCallEntry(mob->GetEntry()) || !mob->IsAlive() || mob->IsInCombat())
            return false;
        Unit* mograine = ScarletMonastery::IdleMograineWithChapel(leader);
        return mograine && mob->GetExactDist(mograine) <= ScarletMonastery::MOGRAINE_CALL_RADIUS;
    }

    bool HopelessWingMap(uint32 mapId)
    {
        return mapId == 329;  // Stratholme
    }

    std::unordered_map<uint32, HopelessWingState> hopelessWings;  // Party::seq -> state, world thread only

    bool HopelessWingDeferred(uint32 seq, ObjectGuid const& boss)
    {
        auto const found = hopelessWings.find(seq);
        return found != hopelessWings.end() && found->second.deferred.count(boss) != 0;
    }

    // The route to `boss` enters the reach of an idle hopeless-refused mob (or the boss stands in it), and no
    // complete way round keeps out of those reaches and of the closed doors. heldBy: that mob's name.
    bool RouteHeldByHopelessPack(Player* leader, Creature* boss, Movement::PointsArray const& route,
                                 std::string& heldBy)
    {
        heldBy.clear();
        auto const listed = hopelessPacks.find(leader->GetGUID());
        if (listed == hopelessPacks.end() || listed->second.empty() || route.size() < 2)
            return false;
        Map* map = leader->GetMap();
        uint32 const now = getMSTime();
        std::vector<G3D::Vector4> discs;
        std::vector<std::string> names;
        for (HopelessMob const& known : listed->second)
        {
            Creature* mob = map ? map->GetCreature(known.guid) : nullptr;
            if (!mob || !mob->IsAlive() || mob->IsInCombat() ||
                getMSTimeDiff(known.sinceMs, now) > HOPELESS_AVOID_MS)
                continue;
            discs.emplace_back(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ(),
                               mob->GetAggroRange(leader) + HOPELESS_AVOID_MARGIN);
            names.push_back(mob->GetName());
        }
        if (discs.empty())
            return false;
        G3D::Vector3 const goal(boss->GetPositionX(), boss->GetPositionY(), boss->GetPositionZ());
        int32 hit = -1;
        for (size_t disc = 0; disc < discs.size() && hit < 0; ++disc)
            if (InDisc(goal, discs[disc]))
                hit = int32(disc);
        for (size_t index = 1; index < route.size() && hit < 0; ++index)
        {
            G3D::Vector3 const& from = route[index - 1];
            G3D::Vector3 const segment = route[index] - from;
            float const lengthSq = segment.x * segment.x + segment.y * segment.y;
            for (size_t disc = 0; disc < discs.size(); ++disc)
            {
                float t = lengthSq > 0.0f ? ((discs[disc].x - from.x) * segment.x +
                                             (discs[disc].y - from.y) * segment.y) /
                                                lengthSq
                                          : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                if (InDisc(from + segment * t, discs[disc]))
                {
                    hit = int32(disc);
                    break;
                }
            }
        }
        if (hit < 0)
            return false;
        if (!InDisc(goal, discs[size_t(hit)]))
        {
            std::vector<G3D::AABox> doorBoxes;
            for (GameObject* door : ClosedDoors(map, leader))
                if (door && door->m_model)
                {
                    G3D::AABox const& bounds = door->m_model->GetBounds();
                    G3D::Vector3 const margin(PASSAGE_DOOR_MARGIN, PASSAGE_DOOR_MARGIN, PASSAGE_DOOR_MARGIN);
                    doorBoxes.emplace_back(bounds.low() - margin, bounds.high() + margin);
                }
            G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
            for (bool const steep : {false, true})
            {
                Movement::PointsArray corners;
                bool complete = false;
                if (LongRoute(leader, here, goal, steep, corners, complete,
                              doorBoxes.empty() ? nullptr : &doorBoxes, &discs) &&
                    complete && corners.size() >= 2)
                    return false;  // a way round: not held
            }
        }
        heldBy = names[size_t(hit)];
        return true;
    }

    // Creature::MoveInLineOfSight aggroes within GetAggroRange + m_CombatDistance (3D, no object sizes).
    float RealAggroRange(Creature* mob, Player* leader)
    {
        return mob->GetAggroRange(leader) + mob->m_CombatDistance;
    }

    std::unordered_map<uint32, std::vector<G3D::Vector3>> respawnZones;  // instance id -> points (world thread)

    bool FastRespawner(Creature const* creature)
    {
        if (ROUTE_RESPAWN_IGNORE && RespawnIgnored::IsIgnored(creature))
            return true;
        return ROUTE_FAST_RESPAWN_PASS && creature && creature->GetSpawnId() && !creature->IsDungeonBoss() &&
               creature->GetRespawnDelay() >= FAST_RESPAWN_MIN_S &&
               creature->GetRespawnDelay() <= FAST_RESPAWN_MAX_S;
    }

    std::vector<FastSpawn> const& FastSpawns(uint32 mapId)
    {
        static std::unordered_map<uint32, std::vector<FastSpawn>> cache;
        if (auto const found = cache.find(mapId); found != cache.end())
            return found->second;
        std::vector<FastSpawn>& spawns = cache[mapId];
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            if (data.mapid != mapId || data.spawntimesecs < 1 ||
                data.spawntimesecs > RespawnIgnored::RESPAWN_IGNORE_MAX_S)
                continue;
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(data.id);
            if (!info || info->rank == CREATURE_ELITE_WORLDBOSS || info->npcflag ||
                (info->flags_extra & (CREATURE_FLAG_EXTRA_DUNGEON_BOSS | CREATURE_FLAG_EXTRA_CIVILIAN |
                                      CREATURE_FLAG_EXTRA_TRIGGER)))
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->faction);
            if (!faction || !faction->IsHostileToPlayers())
                continue;
            spawns.push_back({data.id, data.spawntimesecs, G3D::Vector3(data.posX, data.posY, data.posZ)});
        }
        return spawns;
    }

    void AddRespawnZonePoint(uint32 instanceId, G3D::Vector3 const& point)
    {
        std::vector<G3D::Vector3>& zone = respawnZones[instanceId];
        if (zone.size() >= RESPAWN_ZONE_MAX_POINTS)
            return;
        for (G3D::Vector3 const& known : zone)
            if ((known - point).length() < RESPAWN_ZONE_MERGE)
                return;
        zone.push_back(point);
    }

    std::unordered_map<uint32, MechBridgeLifts> mechBridgeLifts;  // world thread only

    std::unordered_map<ObjectGuid, SightStop> sightStops;  // world thread only

    std::unordered_map<ObjectGuid, SightHold> sightHolds;  // world thread only

    std::unordered_map<ObjectGuid, BossReady> bossReady;  // per leader, world thread only

    // Creature::CanAssistTo(caller, enemy, checkFaction) as the core runs it when the caller calls for help,
    // without its last test (the helper sees the enemy: the group is not there yet) - the enemy only has to be
    // hostile to it (no enemy: not tested).
    bool CanAnswerAssist(Creature* helper, Creature* caller, Player* enemy, bool checkFaction)
    {
        if (!helper->HasReactState(REACT_AGGRESSIVE) || !helper->IsAlive() || helper->IsInEvadeMode() ||
            helper->IsCivilian() || helper->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
            helper->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) || helper->IsImmuneToNPC() || helper->IsEngaged() ||
            helper->GetCharmerOrOwnerGUID() ||
            helper->GetCreatureTemplate()->HasFlagsExtra(CREATURE_FLAG_EXTRA_IGNORE_ALL_ASSISTANCE_CALLS))
            return false;
        if (checkFaction ? (helper->GetFaction() != caller->GetFaction() || !helper->RespondsToCallForHelp())
                         : !helper->IsFriendlyTo(caller))
            return false;
        return !enemy || helper->IsHostileTo(enemy);
    }

    // The pulled trash brings the idle boss (Creature::CallAssistance from the trash: AnyAssistCreatureInRangeCheck
    // - the boss can assist, within the radius of the trash, the trash sees him).
    bool CallsBossForHelp(Creature* trash, Creature* boss)
    {
        if (!boss || trash == boss || !CanAnswerAssist(boss, trash, nullptr, true))
            return false;

        float const radius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        return radius > 0.0f && trash->IsWithinDistInMap(boss, radius + ASSIST_LINK_MARGIN) &&
               trash->IsWithinLOSInMap(boss);
    }

    std::unordered_map<uint64, uint32> backboneIncompleteLogMs;  // world thread only

    std::unordered_map<uint32, RouteSnapshot> routeSnapshots;  // world thread only

    G3D::Vector3 ClosestOnSegment(G3D::Vector3 const& point, G3D::Vector3 const& from, G3D::Vector3 const& to)
    {
        G3D::Vector3 const segment = to - from;
        float const lengthSq = segment.squaredLength();
        if (lengthSq <= 0.0f)
            return from;
        return from + segment * std::clamp((point - from).dot(segment) / lengthSq, 0.0f, 1.0f);
    }

    float RouteReach(Creature* mob, Player* leader)
    {
        float wander = 0.0f;
        if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
            wander = std::min(mob->GetWanderDistance(), APPROACH_WANDER_MAX);
        return RealAggroRange(mob, leader) + wander + ROUTE_REACH_MARGIN;
    }

    // The mob aggroes on the route: a route point inside its reach (3D, as Creature::MoveInLineOfSight measures)
    // and its Z gate (AggroesAtPoint) from which it sees the leader (SeesFromPoint) - the open space between
    // them, not the distance alone. Per
    // segment its nearest point, then the points of the segment's stretch inside the reach (half and nine
    // tenths of the way to the reach's edge, both sides): a mob in a Stockade cell sees the corridor only through
    // its door, and the nearest point lies in front of the cell wall. At most ROUTE_ONLY_LOS_TRIES line-of-sight
    // tests: a cave beside the corridor behind rock stays out.
    bool BlocksRoute(Player* leader, Creature* mob, float reach, RouteBackbone const& backbone, RouteCheck& check)
    {
        G3D::Vector3 const position(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ());
        uint32 tries = 0;
        for (Movement::PointsArray const& points : backbone)
            for (size_t index = 1; index < points.size(); ++index)
            {
                G3D::Vector3 const& from = points[index - 1];
                G3D::Vector3 const& to = points[index];
                if (position.x < std::min(from.x, to.x) - reach || position.x > std::max(from.x, to.x) + reach ||
                    position.y < std::min(from.y, to.y) - reach || position.y > std::max(from.y, to.y) + reach)
                    continue;
                G3D::Vector3 const closest = ClosestOnSegment(position, from, to);
                float const distance = (closest - position).length();
                if (check.routeDist < 0.0f || distance < check.routeDist)
                {
                    check.routeDist = distance;
                    check.routeDz = position.z - closest.z;
                }
                if (distance >= reach || tries >= ROUTE_ONLY_LOS_TRIES)
                    continue;

                G3D::Vector3 const segment = to - from;
                float const length = segment.length();
                float const along = length > 0.0f ? (closest - from).dot(segment) / length : 0.0f;
                float const chord = std::sqrt(std::max(0.0f, reach * reach - distance * distance));
                float tested[5];
                uint32 testedCount = 0;
                for (float const offset : {0.0f, -0.5f, 0.5f, -0.9f, 0.9f})
                {
                    float const at = length > 0.0f ? std::clamp(along + offset * chord, 0.0f, length) : 0.0f;
                    bool repeated = false;
                    for (uint32 seen = 0; seen < testedCount; ++seen)
                        repeated |= std::fabs(tested[seen] - at) < 1.0f;
                    if (repeated)
                        continue;
                    tested[testedCount++] = at;
                    G3D::Vector3 const point = length > 0.0f ? from + segment * (at / length) : from;
                    if (AggroesAtPoint(mob, leader, point, reach, true, &check.zGated))
                    {
                        check.routeLos = true;
                        return true;
                    }
                    if (++tries >= ROUTE_ONLY_LOS_TRIES)
                        break;
                }
            }
        return false;
    }

    float SegmentsDistance2d(G3D::Vector2 const& a, G3D::Vector2 const& b, G3D::Vector2 const& c,
                             G3D::Vector2 const& d)
    {
        auto const cross = [](G3D::Vector2 const& o, G3D::Vector2 const& p, G3D::Vector2 const& q)
        { return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x); };
        float const d1 = cross(c, d, a);
        float const d2 = cross(c, d, b);
        float const d3 = cross(a, b, c);
        float const d4 = cross(a, b, d);
        if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) &&
            ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f)))
            return 0.0f;
        return std::min(std::min(PointSegmentDistance2d(a, c, d), PointSegmentDistance2d(b, c, d)),
                        std::min(PointSegmentDistance2d(c, a, b), PointSegmentDistance2d(d, a, b)));
    }

    std::map<BossRoomKey, float> bossWalkCache;  // world thread only

    std::map<std::pair<uint32, ObjectGuid>, uint32> detourLoggedMs;  // world thread only

    float BossWalk(Player* walker, Creature* boss, G3D::Vector3 const& at)
    {
        G3D::Vector3 const bossAt(boss->GetPositionX(), boss->GetPositionY(), boss->GetPositionZ());
        auto const cell = [](float value) { return int32(std::lround(value / BOSS_ROOM_CACHE_CELL)); };
        BossRoomKey const key{boss->GetMapId(), cell(bossAt.x), cell(bossAt.y), cell(bossAt.z),
                              cell(at.x),       cell(at.y),     cell(at.z)};
        if (auto const cached = bossWalkCache.find(key); cached != bossWalkCache.end())
            return cached->second;
        if (bossWalkCache.size() >= BOSS_ROOM_CACHE_MAX)
            bossWalkCache.clear();
        PathResult const walk = RoutePath(walker, bossAt.x, bossAt.y, bossAt.z, &at);
        float const left = (bossAt - walk.end).length();
        bool const arrives =
            walk.complete || (left <= GAP_REACHABLE_LEFT && std::fabs(bossAt.z - walk.end.z) <= GAP_REACHABLE_DZ);
        float const length = walk.reachable && arrives ? walk.length : -1.0f;
        bossWalkCache.emplace(key, length);
        return length;
    }

    // On the boss's floor: the walk to him is about the straight distance (BOSS_WALK_RATIO / _EXTRA).
    bool WalkableToBoss(Player* walker, Creature* boss, G3D::Vector3 const& at)
    {
        float const straight = boss->GetExactDist(at.x, at.y, at.z);
        float const walk = BossWalk(walker, boss, at);
        return walk >= 0.0f && walk <= std::max(BOSS_WALK_RATIO * straight, straight + BOSS_WALK_EXTRA);
    }

    // The boss's C++ call for help (SCRIPT_HELP_BOSSES) reaches this idle trash.
    bool ScriptCallForHelpReaches(Player* leader, Creature* boss, Creature* trash)
    {
        for (ScriptHelpDef const& def : SCRIPT_HELP_BOSSES)
            if (def.mapId == boss->GetMapId() && def.bossEntry == boss->GetEntry())
                return CanAnswerAssist(trash, boss, leader, false) && trash->IsWithinDistInMap(boss, def.radius) &&
                       trash->IsWithinLOSInMap(boss);
        return false;
    }

    std::unordered_map<uint64, float> smartHelpRanges;  // world thread only

    float SmartCallForHelpRange(Creature* boss)
    {
        if (boss->GetAIName() != "SmartAI")
            return 0.0f;
        uint64 const key = (uint64(boss->GetEntry()) << 32) | boss->GetSpawnId();
        if (auto const cached = smartHelpRanges.find(key); cached != smartHelpRanges.end())
            return cached->second;
        float range = 0.0f;
        for (int32 const source : {-int32(boss->GetSpawnId()), int32(boss->GetEntry())})
        {
            if (!source)
                continue;
            for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(source, SMART_SCRIPT_TYPE_CREATURE))
                if (holder.GetActionType() == SMART_ACTION_CALL_FOR_HELP &&
                    holder.GetTargetType() == SMART_TARGET_SELF)
                    range = std::max(range, float(holder.action.callHelp.range));
        }
        smartHelpRanges.emplace(key, range);
        return range;
    }

    // Trash that joins the next boss's fight (ROUTE_ONLY_TRASH, ROUTE_BOSS_ROOM_FIRST: his room is cleared before
    // him), by the core's mechanics: the reason, nullptr when it does not join (however near: a ledge, a floor
    // above or below him).
    // - "assist": the engaged boss calls for assistance (Creature::CallAssistance at combat start and every
    //   assistance period: AnyAssistCreatureInRangeCheck - the trash can assist him (CanAnswerAssist, same
    //   faction), within CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS of him (+ ASSIST_LINK_MARGIN for the steps of
    //   the pull) and the boss sees it);
    // - "aggro": the group fights at the boss's spot - the trash's aggro reach (AggroReach, the leader's level)
    //   covers that spot, it sees him (Creature::MoveInLineOfSight) and stands on his floor (WalkableToBoss);
    // - "formation": the boss's creature_formations group (members join their leader's fight);
    // - "script": the trash calls the idle boss when pulled (CallsBossForHelp), or the boss's SmartAI or C++
    //   (SCRIPT_HELP_BOSSES) call for help reaches it (CallOfHelpCreatureInRangeDo: friendly, within the radius,
    //   sees the boss / the group at him).
    // Not modelled: boss summons (the adds do not stand there before the pull).
    char const* JoinsBossFight(Player* leader, Creature* boss, Creature* trash)
    {
        if (!boss || trash == boss || !boss->IsAlive() || !trash->IsAlive())
            return nullptr;
        if (trash->GetFormation() && trash->GetFormation() == boss->GetFormation())
            return "joins boss: formation";
        if (CallsBossForHelp(trash, boss))
            return "joins boss: script";
        if (float const helpRange = SmartCallForHelpRange(boss);
            helpRange > 0.0f && CanAnswerAssist(trash, boss, leader, false) &&
            trash->IsWithinDistInMap(boss, helpRange) && trash->IsWithinLOSInMap(boss))
            return "joins boss: script";
        if (ScriptCallForHelpReaches(leader, boss, trash))
            return "joins boss: script";
        if (trash->GetExactDist(boss) > BOSS_ROOM_TRASH_RADIUS)
            return nullptr;  // past every assistance and aggro reach
        float const radius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        if (radius > 0.0f && CanAnswerAssist(trash, boss, leader, true) &&
            boss->IsWithinDistInMap(trash, radius + ASSIST_LINK_MARGIN) && boss->IsWithinLOSInMap(trash))
            return "joins boss: assist";
        G3D::Vector3 const at(trash->GetPositionX(), trash->GetPositionY(), trash->GetPositionZ());
        if (trash->IsHostileTo(leader) && trash->GetExactDist(boss) <= AggroReach(trash, leader) &&
            trash->IsWithinLOSInMap(boss) && WalkableToBoss(leader, boss, at))
            return "joins boss: aggro";
        return nullptr;
    }

    // Skip reasons that say the mob cannot be reached or fought now: they stay skipped.
    bool ReachabilitySkip(std::string const& reason)
    {
        return reason.find("unreachable") != std::string::npos || reason.find("path lost") != std::string::npos ||
               reason.rfind("route end", 0) == 0 || reason.find("closed") != std::string::npos ||
               reason.find("detour") != std::string::npos || reason.find("respawn") != std::string::npos ||
               reason.find("boss area: given up") != std::string::npos;
    }

    std::map<std::pair<uint32, ObjectGuid>, BossAreaUnskip> bossAreaUnskips;  // world thread only

    std::unordered_map<uint32, uint32> bossAreaHoldLogMs;                    // party seq -> last line

    bool InBossFootprint(Player* leader, Creature* boss, Creature* mob)
    {
        if (!ROUTE_BOSS_FOOTPRINT || std::fabs(mob->GetPositionZ() - boss->GetPositionZ()) > BOSS_ROOM_TRASH_DZ ||
            !mob->IsHostileTo(leader))
            return false;
        bool inside = false;
        for (BossFootprintDef const& footprint : BOSS_FOOTPRINTS)
            if (footprint.bossEntry == boss->GetEntry() &&
                mob->GetExactDist2d(footprint.x, footprint.y) <= footprint.radius)
                inside = true;
        if (!inside)
            return false;
        std::list<Creature*> pack;
        PackMobCheck packCheck{leader, mob};
        Acore::CreatureListSearcher<PackMobCheck> packSearcher(mob, pack, packCheck);
        Cell::VisitObjects(mob, packSearcher, PACK_RADIUS);
        pack.remove(boss);
        uint32 const size =
            uint32(pack.size()) + (std::find(pack.begin(), pack.end(), mob) == pack.end() ? 1u : 0u);
        return size <= BOSS_FOOTPRINT_PACK_MAX;
    }

    // Why `mob` is trash round `boss` that is cleared before him, nullptr when it is not.
    char const* BossAreaTrashOf(Player* leader, Creature* boss, Creature* mob, bool bossCreature)
    {
        if (!leader || !boss || !mob || mob == boss || !boss->IsAlive() || !mob->IsAlive() || mob->IsInCombat() ||
            bossCreature || mob->IsDungeonBoss() || mob->isWorldBoss() || mob->GetMap() != boss->GetMap() ||
            !mob->GetSpawnId() || mob->IsSummon())
            return nullptr;
        for (uint32 const entry : BOSS_AREA_ENCOUNTER_ADDS)
            if (mob->GetEntry() == entry)
                return nullptr;
        // [mech-sepethrea-south-clear] (PATCH_queue_encounters section 3) trash a boss fight is dragged into
        // although it stands past BOSS_ROOM_TRASH_RADIUS: The Mechanar, Sepethrea 19221 at (326.5,13.2) - the
        // Tempest-Forge Destroyer 138862 (293.9,-14.9, 43 yd, wanders 6 yd) and the Centurion / Astromage
        // formation 138894-138897 (272-274,-18..-29, ~64 yd) south of her room. mech-n 115805 / 132016: her
        // flame kite took the tank into them (8 / 7 attackers), the wipe reset her at 4 %.
        struct BossAreaExtraDef
        {
            uint32 bossEntry;
            ObjectGuid::LowType spawnId;
        };
        constexpr BossAreaExtraDef BOSS_AREA_EXTRA[] = {
            {19221, 138862}, {19221, 138894}, {19221, 138895}, {19221, 138896}, {19221, 138897},
        };
        for (BossAreaExtraDef const& extra : BOSS_AREA_EXTRA)
            if (boss->GetEntry() == extra.bossEntry && mob->GetSpawnId() == extra.spawnId &&
                mob->IsHostileTo(leader))
                return "boss area: listed (the fight is dragged there)";
        if (mob->GetFormation() && mob->GetFormation() == boss->GetFormation())
            return nullptr;
        if (CallsBossForHelp(mob, boss))
            return nullptr;
        if (char const* joins = JoinsBossFight(leader, boss, mob))
            return joins;
        G3D::Vector3 const at(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ());
        if (mob->GetExactDist(boss) <= BOSS_ROOM_TRASH_RADIUS &&
            std::fabs(mob->GetPositionZ() - boss->GetPositionZ()) <= BOSS_ROOM_TRASH_DZ &&
            mob->IsHostileTo(leader) && mob->IsWithinLOSInMap(boss) && WalkableToBoss(leader, boss, at))
            return "boss area: room";
        if (InBossFootprint(leader, boss, mob) && WalkableToBoss(leader, boss, at))  // [b33_harn]
            return "boss area: footprint";
        return nullptr;
    }

    // A patrol whose walk passes within `reach` (2D) of the route on its floor (the height ranges of the two
    // segments overlap within BOSS_ROOM_TRASH_DZ), or within its reach of the next boss where it sees him on his
    // floor (it aggroes on the group fighting him: JoinsBossFight "aggro").
    bool PatrolCrossesRoute(Player* walker, WaypointPath const& path, float reach, RouteBackbone const& backbone,
                            Creature* boss)
    {
        std::vector<WaypointNode> const& nodes = path.Nodes;
        for (size_t index = 0; index < nodes.size(); ++index)
        {
            WaypointNode const& from = nodes[index];
            WaypointNode const& to = nodes[(index + 1) % nodes.size()];
            if (boss && boss->GetExactDist(from.X, from.Y, from.Z) <= reach &&
                boss->IsWithinLOS(from.X, from.Y, from.Z + PATROL_EYE_HEIGHT) &&
                WalkableToBoss(walker, boss, G3D::Vector3(from.X, from.Y, from.Z)))
                return true;
            G3D::Vector2 const patrolFrom(from.X, from.Y);
            G3D::Vector2 const patrolTo(to.X, to.Y);
            float const patrolLow = std::min(from.Z, to.Z) - BOSS_ROOM_TRASH_DZ;
            float const patrolHigh = std::max(from.Z, to.Z) + BOSS_ROOM_TRASH_DZ;
            for (Movement::PointsArray const& points : backbone)
                for (size_t point = 1; point < points.size(); ++point)
                {
                    G3D::Vector3 const& routeFrom = points[point - 1];
                    G3D::Vector3 const& routeTo = points[point];
                    if (std::max(routeFrom.z, routeTo.z) < patrolLow ||
                        std::min(routeFrom.z, routeTo.z) > patrolHigh)
                        continue;
                    if (SegmentsDistance2d(patrolFrom, patrolTo, G3D::Vector2(routeFrom.x, routeFrom.y),
                                           G3D::Vector2(routeTo.x, routeTo.y)) < reach)
                        return true;
                }
        }
        return false;
    }

    // Closed parts of the instance (AreaGateDef): the gate is meant for this run (onlyContent), a point is inside.
    bool AreaGateForContent(AreaGateDef const& gate, char const* contentKey)
    {
        return !gate.onlyContent || strcmp(gate.onlyContent, contentKey) == 0;
    }

    bool InAreaGate(AreaGateDef const& gate, float x, float y, float z)
    {
        return x >= gate.minX && x <= gate.maxX && y >= gate.minY && y <= gate.maxY && z >= gate.minZ &&
               z <= gate.maxZ;
    }

    // The closed parts that are a wing of another content (onlyContent set): kept out of the route and the tank's
    // own pulls too. The other gates (an event's area: the Ring of Law stands, Stratholme's acolytes) only keep
    // their mobs from being targets - a route past them stays a route.
    std::vector<AreaGateDef const*> KeepOutAreas(std::vector<AreaGateDef const*> const& gates)
    {
        std::vector<AreaGateDef const*> result;
        for (AreaGateDef const* gate : gates)
            if (gate->onlyContent)
                result.push_back(gate);
        return result;
    }

    bool InAnyAreaGate(std::vector<AreaGateDef const*> const& gates, float x, float y, float z)
    {
        for (AreaGateDef const* gate : gates)
            if (InAreaGate(*gate, x, y, z))
                return true;
        return false;
    }

    // A route that walks through a closed part: the target is not reached that way (Blackrock Spire, LBRS waves
    // 2026-10-24: the UBRS hall lies between the Legionnaires' room and the lower city).
    bool RouteCrossesClosedArea(Movement::PointsArray const& points, std::vector<AreaGateDef const*> const& gates)
    {
        if (gates.empty() || points.empty())
            return false;
        if (InAnyAreaGate(gates, points[0].x, points[0].y, points[0].z))
            return true;
        for (size_t index = 1; index < points.size(); ++index)
        {
            G3D::Vector3 const segment = points[index] - points[index - 1];
            float const length = segment.length();
            for (float along = ROUTE_SKIPPED_SAMPLE_STEP; along < length; along += ROUTE_SKIPPED_SAMPLE_STEP)
            {
                G3D::Vector3 const point = points[index - 1] + segment * (along / length);
                if (InAnyAreaGate(gates, point.x, point.y, point.z))
                    return true;
            }
            if (InAnyAreaGate(gates, points[index].x, points[index].y, points[index].z))
                return true;
        }
        return false;
    }

    std::unordered_map<ObjectGuid, PassageRoundMemo> passageRoundMemos;  // world thread only

    bool GoesRoundPassageDoor(Player* leader, InstanceScript const* script, WorldObject* target, GameObject* door,
                              std::vector<GameObject*> const& shut)
    {
        if (!leader || !target || PassageDoorBoss(script, door) < 0)
            return false;
        // [b51_strat] User rule 2026-10-26: «Ключ есть - открыл ворота, пустил ... Не надо ходить в обходы»: a door
        // a click opens (DoorOpensByUse: a lever / an Iron Gate) or whose lock takes a key item a member of the
        // leader's group carries is opened, never walked round.
        if (DoorOpensByUse(door))
            return false;
        if (LockEntry const* lock = sLockStore.LookupEntry(door->GetGOInfo()->GetLockId()))
            for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
                if (lock->Type[i] == LOCK_KEY_ITEM && lock->Index[i])
                {
                    if (leader->HasItemCount(lock->Index[i], 1))
                        return false;
                    if (Group* group = leader->GetGroup())
                        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                            if (Player const* member = ref->GetSource();
                                member && member->HasItemCount(lock->Index[i], 1))
                                return false;
                }
        uint32 const now = getMSTime();
        PassageRoundMemo& memo = passageRoundMemos[target->GetGUID()];
        if (memo.ms && memo.door == door->GetGUID() && getMSTimeDiff(memo.ms, now) <= PASSAGE_ROUND_MEMO_MS)
            return memo.round;
        PathResult const round = DoorFreeRoute(
            leader, G3D::Vector3(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ()), shut);
        memo.door = door->GetGUID();
        memo.ms = now ? now : 1;
        memo.round = round.reachable && !RouteThroughClosedDoor(round.points, shut, leader->GetPhaseMask());
        return memo.round;
    }

    // A DoorEventDef key creature whose earlier step has not started (GetData(afterData) 0): it evades when
    // engaged, so it is no target (Utgarde Keep: Forge Master 2 before forge 1, 3 before 2).
    bool DoorKeyLocked(Creature const* creature, InstanceScript const* script)
    {
        if (!ROUTE_CLOSED_DOORS_ALL || !creature || !script)
            return false;
        for (DoorEventDef const* def : DoorEvents(creature->GetMapId()))
            if (def->afterData && IsDoorKey(*def, creature) && !script->GetData(def->afterData))
                return true;
        return false;
    }

    // GuardedDoorDef: the alive guards of the objects still guarded for this closed door (empty: none left or no
    // such event for it).
    std::vector<Creature*> PendingDoorGuards(Map* map, GameObject const* door)
    {
        std::vector<Creature*> guards;
        if (!map || !door || door->GetGoState() != GO_STATE_READY)
            return guards;
        for (GuardedDoorDef const* def : GuardedDoors(door->GetMapId()))
        {
            if (def->doorEntry != door->GetEntry())
                continue;
            // [st-guard-door-anywhere] No guarded objects: the door opens once its guards are dead (Sunken Temple's
            // forcefield 149431, Uldaman's Temple Door 124367) - guards within guardRadius of the door, 0:
            // anywhere.
            if (def->goEntries.empty())
            {
                for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
                    if (creature && creature->IsInWorld() && creature->IsAlive() &&
                        std::find(def->guardEntries.begin(), def->guardEntries.end(), creature->GetEntry()) !=
                            def->guardEntries.end() &&
                        (def->guardRadius <= 0.0f || creature->GetExactDist(door) <= def->guardRadius) &&
                        std::find(guards.begin(), guards.end(), creature) == guards.end())
                        guards.push_back(creature);
                continue;
            }
            for (auto const& [goSpawnId, go] : map->GetGameObjectBySpawnIdStore())
            {
                if (!go || !go->isSpawned() || uint32(go->GetGoState()) != def->goState ||
                    std::find(def->goEntries.begin(), def->goEntries.end(), go->GetEntry()) ==
                        def->goEntries.end())
                    continue;
                for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
                    if (creature && creature->IsInWorld() && creature->IsAlive() &&
                        std::find(def->guardEntries.begin(), def->guardEntries.end(), creature->GetEntry()) !=
                            def->guardEntries.end() &&
                        creature->GetExactDist(go) <= def->guardRadius &&
                        std::find(guards.begin(), guards.end(), creature) == guards.end())
                        guards.push_back(creature);
            }
        }
        return guards;
    }

    // [st-guard-approach] The approach point of a guard of a door still closed (GuardedDoorDef::approaches).
    GuardApproachDef const* PendingGuardApproach(Map* map, Creature const* guard)
    {
        if (!map || !guard)
            return nullptr;
        for (GuardedDoorDef const* def : GuardedDoors(map->GetId()))
            for (GuardApproachDef const& point : def->approaches)
                if (point.entry == guard->GetEntry())
                    for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                        if (go && go->isSpawned() && go->GetEntry() == def->doorEntry &&
                            go->GetGoState() == GO_STATE_READY)
                            return &point;
        return nullptr;
    }

    bool GossipStarterHeld(Player* leader, Creature* creature)
    {
        // [b36_ohb] Old Hillsbrad's Thrall: not sent on while an idle pack stands on the next stretch of his walk
        // (the same rule as the bots' own "Old Hillsbrad: Thrall" step, EscortProtect::EscortGossipHeld).
        constexpr uint32 OHB_MAP_ID = 560;
        constexpr uint32 OHB_THRALL_ENTRY = 17876;
        if (leader && creature && leader->GetMapId() == OHB_MAP_ID && creature->GetEntry() == OHB_THRALL_ENTRY)
            return EscortProtect::EscortGossipHeld(leader, creature);
        if (!leader || !creature || leader->GetMapId() != COS_MAP_ID)
            return false;
        if (creature->GetEntry() == COS_ARTHAS_ENTRY)
            return CoSHelpers::GetCorruptorDetourTarget(leader) != nullptr;
        if (creature->GetEntry() == COS_CHROMIE_MIDDLE_ENTRY)
        {
            // [w24b-cos-chromie-past] npc_cos_chromie_middle offers 9610 -> 9612 only at COS_PROGRESS_CRATES_FOUND
            // (OnGossipHello returns true otherwise: an empty menu). Past it - the CoS strategy's own "start arthas
            // intro" answered 9612 first - the step is done: no walk back (cos-n 2026-10-25 121631: the leader left
            // the walking Arthas at 201 s for Chromie 105 yd back, "event_start" with options [] at 233 s, stood
            // there until 302 s).
            if (CoSHelpers::GetProgress(leader) > CoSIds::COS_PROGRESS_CRATES_FOUND)
            {
                static uint32 pastLogMs = 0;
                uint32 const now = getMSTime();
                if (!pastLogMs || getMSTimeDiff(pastLogMs, now) >= 30 * IN_MILLISECONDS)
                {
                    pastLogMs = now ? now : 1;
                    LOG_INFO("playerbots", "[w24b-cos-chromie-past] {}: Chromie 9610 step past (progress {}), held",
                             leader->GetName(), CoSHelpers::GetProgress(leader));
                }
                return true;
            }
            return !creature->IsVisible() || !leader->InSamePhase(creature) || !leader->CanSeeOrDetect(creature);
        }
        return false;
    }

    // EventStarterDef::afterBossDone: the script's boss slot is DONE (or no slot is named).
    bool ScriptBossDone(InstanceScript const* script, int32 bossId)
    {
        return bossId < 0 || (script && script->GetBossState(uint32(bossId)) == DONE);
    }

    bool RouteCrossesSkipped(Player* walker, Movement::PointsArray const& points, Creature* target,
                             std::vector<std::pair<Creature*, float>> const& skipped)
    {
        if (skipped.empty() || points.size() < 2)
            return false;

        for (size_t index = 1; index < points.size(); ++index)
        {
            G3D::Vector3 const segment = points[index] - points[index - 1];
            float const length = segment.length();
            for (float along = 0.0f; along <= length; along += ROUTE_SKIPPED_SAMPLE_STEP)
            {
                G3D::Vector3 const point =
                    points[index - 1] + (length > 0.0f ? segment * (along / length) : segment);
                for (auto const& [mob, reach] : skipped)
                    if (mob != target && mob->GetExactDist(target) > PACK_RADIUS &&
                        AggroesAtPoint(mob, walker, point, reach, false))
                        return true;
            }
        }
        return false;
    }

    // [b35_up] Utgarde Pinnacle: Skadi's gauntlet runs (areatrigger 4991 -> boss_skadi ACTION_START_ENCOUNTER sets
    // DATA_SKADI_THE_RUTHLESS IN_PROGRESS). Skadi rides Grauf NOT_SELECTABLE and out of combat meanwhile, so his
    // dbc BossTrack reads NOT_STARTED; spell_skadi_reset_check evades him (waves despawn, the event starts over)
    // once no player in the hall keeps Gauntlet Effect 47547. up-h 20261026-024524-up-h-5D-35: waves cleared at
    // 1231 s, the fallback target was a Mindless Servant 846 yd off, shortcut_reenter walked the group out at
    // 1232 s, Skadi reset (boss_rebound 1264 s); the second gauntlet (1402 s) began with harpoons still in hand,
    // the DPS ran ahead of the tank to the launchers and the healer, 30 yd behind, died first (death burst).
    bool SkadiGauntletRunning(Player* leader)
    {
        if (!leader || leader->GetMapId() != UPIds::MAP_ID)
            return false;
        InstanceScript* instance = leader->GetInstanceScript();
        return instance && instance->GetData(UPIds::DATA_SKADI_THE_RUTHLESS) == IN_PROGRESS;
    }

    // [b35_up] Inside Skadi's hall (the gauntlet lane, UPIds::HALL_*) with a margin: the waves spawn at
    // boss_skadi.cpp SpawnLoc (477.6,-484.6), just past HALL_Y_MAX.
    bool InSkadiHall(WorldObject const* object)
    {
        constexpr float margin = 10.0f;
        return object && object->GetPositionX() >= UPIds::HALL_X_MIN - margin &&
               object->GetPositionX() <= UPIds::HALL_X_MAX + margin &&
               object->GetPositionY() >= UPIds::HALL_Y_MIN - margin &&
               object->GetPositionY() <= UPIds::HALL_Y_MAX + margin &&
               object->GetPositionZ() >= UPIds::HALL_Z_MIN && object->GetPositionZ() <= UPIds::HALL_Z_MAX;
    }

    // The boss of a scripted intro that keeps it immune while it runs (UPIds::IsSvalaIntroRunning: Svala's Arthas
    // talk after areatrigger 5140), nullptr when none runs.
    Creature* RunningIntroBoss(Player* leader)
    {
        if (leader->GetMapId() != UPIds::MAP_ID)
            return nullptr;
        InstanceScript* instance = leader->GetInstanceScript();
        Map* map = leader->GetMap();
        if (!instance || !map || instance->GetData(UPIds::DATA_SVALA_SORROWGRAVE) != IN_PROGRESS)
            return nullptr;
        for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
            if (UPIds::IsSvalaIntroRunning(creature, instance))
                return creature;
        return nullptr;
    }

    // The event spot's boss (an untilDead entry of a boss track) is the lowest open one of the route and its
    // creature is not known yet: it appears only through this event (Utgarde Pinnacle: Svala Sorrowgrave 26668,
    // order 0, comes from Svala 29281 after the intro).
    // EventSpotDef::guardEntries: this creature attacks the protected NPC of the hold spot.
    bool AtHoldGuard(EventSpotDef const& spot, Creature const* creature)
    {
        return spot.guardRadius > 0.0f &&
               std::find(spot.guardEntries.begin(), spot.guardEntries.end(), creature->GetEntry()) !=
                   spot.guardEntries.end() &&
               creature->GetExactDist(spot.x, spot.y, spot.z) <= spot.guardRadius;
    }

    // [brd_bar_buy] [brd_bar_turnin] Starters done at a creature (gossip, escort, vendor, quest giver) and the NPC
    // flag the creature shows while the step can be done.
    bool CreatureStarterKind(StarterKind kind)
    {
        return kind == StarterKind::Gossip || kind == StarterKind::Escort || kind == StarterKind::VendorBuy ||
               kind == StarterKind::QuestTurnIn;
    }

    NPCFlags StarterNpcFlag(StarterKind kind)
    {
        if (kind == StarterKind::VendorBuy)
            return UNIT_NPC_FLAG_VENDOR;
        if (kind == StarterKind::QuestTurnIn)
            return UNIT_NPC_FLAG_QUESTGIVER;
        return UNIT_NPC_FLAG_GOSSIP;
    }

    // [brd_bar_turnin] QuestTurnIn: every item the quest asks for is in the leader's bags (Rocknot's Ale 4295:
    // two Dark Iron Ale Mugs 11325).
    bool QuestItemsReady(Player* leader, EventStarterDef const* def)
    {
        Quest const* quest = def->questId ? sObjectMgr->GetQuestTemplate(def->questId) : nullptr;
        if (!quest)
            return false;
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i] &&
                !leader->HasItemCount(quest->RequiredItemId[i], std::max<uint32>(1, quest->RequiredItemCount[i])))
                return false;
        return true;
    }

    bool OculusDrakePhase(Player* leader)
    {
        if (leader->GetMapId() != OCULUS_MAP_ID)
            return false;
        InstanceScript* instance = leader->GetInstanceScript();
        return instance && instance->GetData(OCULUS_DATA_DRAKOS) == DONE;
    }
}
