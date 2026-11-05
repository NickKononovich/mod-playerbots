/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "RouteTargets.h"
#include "LeaderReports.h"
#include "RouteReenter.h"  // [lead341] the wing change's re-entry
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
#include "LeaderPlannerShared.h"  // [lead204] the planner's pack rule and patrol forecast
#include "LeaderPrepull.h"  // [lead823] the prepull Army's landing trash
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
#include <limits>
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
    using Leader::Dungeon::MemberFull;

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

    // StarterTrack::stuckFrom: the leader's walk to this object got stuck from about here (lead132: no clock, the
    // object is passed over until the leader is elsewhere)
    bool StarterObjectStuck(StarterTrack const& starter, ObjectGuid const& guid, Player* leader)
    {
        auto const itr = starter.stuckFrom.find(guid);
        return itr != starter.stuckFrom.end() &&
               leader->GetExactDist2d(&itr->second) <= UNREACHABLE_RECHECK_DISTANCE &&
               std::fabs(leader->GetPositionZ() - itr->second.GetPositionZ()) <= UNREACHABLE_RECHECK_HEIGHT;
    }

    uint32 OpenDoorCount(Map* map)
    {
        uint32 open = 0;
        if (map)
            for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                if (go && go->isSpawned() && go->GetGoType() == GAMEOBJECT_TYPE_DOOR &&
                    go->GetGoState() != GO_STATE_READY)
                    ++open;
        return open;
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
        // [lead823] user 2026-11-03: the trash the tank knight's prepull Army would wake where it would channel for
        // him is on his path (Leader::Prepull::ArmyLandingTrash): cleared before him
        if (mob->IsHostileTo(leader) && Leader::Prepull::ArmyLandingTrash(boss, mob))
            return "boss area: the prepull Army's ghouls land in its reach";
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

    bool PatrolOnRouteAtArrival(Player* walker, Creature* patrol, WaypointPath const& path, float reach,
                                RouteBackbone const& backbone, Creature* boss)
    {
        if (PatrolCrossesRoute(walker, path, reach, RouteBackbone(), boss))
            return true;  // its walk passes within its reach of the next boss
        std::vector<G3D::Vector3> track;
        float sampleSec = 0.0f;
        if (!patrol || !PullPlanning::PatrolForecast(patrol, track, sampleSec) || track.empty() || sampleSec <= 0.0f)
            return PatrolCrossesRoute(walker, path, reach, backbone, boss);
        float const speed = walker->GetSpeed(MOVE_RUN);
        if (speed <= 0.0f)
            return PatrolCrossesRoute(walker, path, reach, backbone, boss);
        float const step = speed * sampleSec;  // the walk per forecast sample
        auto const meets = [&](G3D::Vector3 const& at, float along) -> bool
        {
            float const index = along / step;
            for (size_t sample : {size_t(index), size_t(index) + 1})
            {
                if (sample >= track.size())
                    continue;
                G3D::Vector3 const& patrolAt = track[sample];
                if (std::fabs(patrolAt.z - at.z) <= BOSS_ROOM_TRASH_DZ &&
                    (G3D::Vector2(patrolAt.x, patrolAt.y) - G3D::Vector2(at.x, at.y)).length() < reach)
                    return true;
            }
            return false;
        };
        for (Movement::PointsArray const& points : backbone)
        {
            float along = 0.0f;  // every leg starts at the walker (a link's far leg: its arrival read early)
            for (size_t point = 1; point < points.size(); ++point)
            {
                G3D::Vector3 const& from = points[point - 1];
                G3D::Vector3 const& to = points[point];
                float const length = (to - from).length();
                for (float walked = 0.0f; walked < length; walked += step)
                {
                    if ((along + walked) / step >= float(track.size()))
                        break;
                    if (meets(from + (to - from) * (walked / length), along + walked))
                        return true;
                }
                along += length;
                if (along / step >= float(track.size()))
                    break;  // past the forecast
            }
            if (points.size() >= 2 && along / step < float(track.size()) && meets(points.back(), along))
                return true;
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

    std::map<InstanceGuid, PassageRoundMemo> passageRoundMemos;  // world thread only

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
        PassageRoundMemo& memo = passageRoundMemos[InstanceKey(target)];
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

    bool AnyMemberOnVehicle(LeaderGroup& group)
    {
        for (LeaderGroup::MemberInfo const& member : group.Members())
            if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                if (bot->IsAlive() && bot->GetVehicle())
                    return true;
        return false;
    }

    // [boss_dead_encounter_open] the script slot of a boss creature, as the boss_state event knows it (BossTrack:
    // its bound creature, else its entry); -1 when the run tracks no slot for it.
    int32 BossSlotOf(LeaderGroup& group, Creature const* boss)
    {
        if (!boss)
            return -1;
        for (BossFacts const& track : group.Bosses())
            if (track.creature == boss->GetGUID())
                return int32(track.bossId);
        for (BossFacts const& track : group.Bosses())
            if (track.entry && track.entry == boss->GetEntry())
                return int32(track.bossId);
        return -1;
    }

    // The door_spin_hold line: the door, the skips, and its opener (a key starter of the group, or none).
    JsonObject DoorSpinFields(LeaderGroup& group, Player* leader, GameObject const* door, uint32 skips,
              uint32 windowMs, char const* how, std::string& opener)
    {
        opener = "none";
        int64 retrySec = -1;
        uint32 const now = getMSTime();
        for (StarterTrack const& starter : group.Route().starters)
            if (starter.def->kind == StarterKind::UseItem && starter.def->itemId &&
                std::find(starter.def->entries.begin(), starter.def->entries.end(), door->GetEntry()) !=
                    starter.def->entries.end())
            {
                opener = std::string(starter.def->note) +
                         (leader->HasItemCount(starter.def->itemId, 1) ? "" : " (no key carried)") +
                         (starter.uses >= starter.def->maxUses ? " (used up)" : "");
                retrySec = int32(starter.nextAllowedMs - now) > 0
                               ? int64((starter.nextAllowedMs - now) / IN_MILLISECONDS)
                               : 0;
            }
        return JsonObject().Str("door", door->GetName()).Num("doorEntry", door->GetEntry()).Str("how", how)
            .Num("skips", skips).Num("windowSec", windowMs / IN_MILLISECONDS)
            .Num("holdSec", DOOR_SPIN_HOLD_MS / IN_MILLISECONDS).Str("opener", opener)
            .Num("openerRetrySec", retrySec);
    }

    // [lead5] LEADER_ONE_PICKER: EncounterOrder::IsLocked is the one encounter-order rule (the planner's and the
    // pick's: the Utgarde Keep forge order with its formations, Blackheart's room), not the harness's own copy of
    // the forge rule (UKIds::IsForgeMasterLocked) for the route's target choice.
    bool EncounterLockedFor(LeaderGroup& group, Creature* creature, InstanceScript const* script)
    {
        if (LeaderPicksFor(group))
            return EncounterOrder::IsLocked(creature);
        return UKIds::IsForgeMasterLocked(creature, script);
    }

    // [b32_strat_gate] An auto-closing key gate (KeyDoorPassDef) the whole living group is through: every member on
    // the far side of the gate plane (away from its use point). A door_hold there is a walk BACK to what stands
    // behind it - no reason to key it again (strat-h 221216-5C-1: 175352 re-armed at 1040 s for trash behind it).
    bool GroupThroughKeyGate(LeaderGroup& group, GameObject const* door)
    {
        KeyDoorPassDef const* pass = door ? KeyDoorPass(door->GetMapId(), door->GetEntry()) : nullptr;
        if (!pass)
            return false;
        float const fx = std::cos(door->GetOrientation());
        float const fy = std::sin(door->GetOrientation());
        float const bars = KeyGateBarsOffset(door, *pass);  // [b35_ss2] the bars' plane, not the spawn's
        auto const side = [&](float x, float y)
        { return (x - door->GetPositionX()) * fx + (y - door->GetPositionY()) * fy - bars >= 0.0f; };
        bool const useSide = side(pass->useX, pass->useY);
        uint32 counted = 0;
        for (LeaderGroup::MemberInfo const& member : group.Members())
            if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                bot && bot->IsAlive() && bot->GetMap() == door->GetMap())
            {
                if (side(bot->GetPositionX(), bot->GetPositionY()) == useSide)
                    return false;
                ++counted;
            }
        return counted != 0;
    }

    // Wing runs (Scarlet Monastery, Dire Maul, Blackrock Spire): a hostile closer to a boss of another wing than
    // to every boss and entrance of this one belongs to the other wing.
    bool InThisWing(LeaderGroup& group, WorldObject const* object)
    {
        if (group.Route().wingAnchors.empty() || group.Route().otherWingAnchors.empty())
            return true;
        return NearestDist(group.Route().wingAnchors, object) <= NearestDist(group.Route().otherWingAnchors, object);
    }

    // The entrance nearest (2D) to an object: the wing it stands in (Scarlet Monastery, Dire Maul).
    int32 NearestEntrance(LeaderGroup& group, WorldObject const* object)
    {
        int32 nearest = -1;
        float nearestDist = 0.0f;
        for (size_t index = 0; index < group.Route().entrances.size(); ++index)
            if (float const dist = object->GetExactDist2d(&group.Route().entrances[index]);
                nearest < 0 || dist < nearestDist)
            {
                nearest = int32(index);
                nearestDist = dist;
            }
        return nearest;
    }

    // ROUTE_REST_HOLD: the members the group waits for ("name(drinking)", ...); empty: no hold. A fight ends
    // it (the bots fight, the harness has nothing to hold), and so does an escort (the group keeps up with it).
    std::string RestHoldMembers(LeaderGroup& group, Player* leader)
    {
        if (!ROUTE_REST_HOLD || !leader->IsAlive() || !group.Route().escort.IsEmpty())
            return std::string();
        // [hos-brann-rest] Halls of Stone: between Brann's gossips 10206 (after the Tribunal of Ages) and 10012
        // (at Sjonnir's door, ~400 yd on) he walks to the door and waits there; no rest hold keeps the leader -
        // the gossip's interactor - from following him (halls_of_stone.h: boss states TRIBUNAL 2, BRANN_DOOR 5).
        // hos-n 2026-10-25 062726: rest_hold 19 s at Brann before 10206, the door gossip never came.
        constexpr uint32 HOS_MAP_ID = 599;
        constexpr uint32 HOS_BOSS_TRIBUNAL_OF_AGES = 2;
        constexpr uint32 HOS_BRANN_DOOR = 5;
        if (leader->GetMapId() == HOS_MAP_ID)
            if (InstanceScript* hos = leader->GetInstanceScript();
                hos && hos->GetBossState(HOS_BOSS_TRIBUNAL_OF_AGES) == DONE &&
                hos->GetBossState(HOS_BRANN_DOOR) != DONE)
                return std::string();
        // [b32_bm] Black Morass: no rest hold while a rift boss lives during the portals (Deja 0, Temporus 1,
        // Aeonus 2: boss state TO_BE_DECIDED 5 until his rift summons him, NOT_STARTED / IN_PROGRESS while he
        // lives). He is the next fight; Aeonus walks to Medivh by himself and drains the shield from his home spot
        // (bm-h 20261025-223301-bm-h-5H-72: the group sat drinking while he walked in, the shield fell 100 -> 0).
        constexpr uint32 BM_DATA_SHIELD_PERCENT = 13;  // the_black_morass.h
        constexpr uint32 BM_DATA_RIFT_NUMBER = 14;
        if (leader->GetMapId() == BLACK_MORASS_MAP)
            if (InstanceScript* bm = leader->GetInstanceScript();
                bm && bm->GetData(BM_DATA_RIFT_NUMBER) > 0 && bm->GetData(BM_DATA_SHIELD_PERCENT) > 0)
                for (uint32 bossId : {0u, 1u, 2u})
                    if (EncounterState const state = bm->GetBossState(bossId);
                        state == NOT_STARTED || state == IN_PROGRESS)
                        return std::string();
        uint32 const now = getMSTime();
        RestHold& hold = restHolds[group.InstanceId()];
        if (leader->IsInCombat())
        {
            hold.sinceMs = 0;
            hold.graceSinceMs = 0;
            return std::string();
        }
        // [lead15] the leader judges the rest (LEADER_ONE_REST_RULE): no cooldown either
        bool const leaderJudgesRest = group.HasContent() && !group.IsRaid() && group.ContentSize() <= 5 &&
                                      Leader::Orders::IsTracked(group.LeaderGuid());
        if (!(Leader::Leads(Leader::LEADER_ONE_REST_RULE) && leaderJudgesRest) && hold.cooldownUntilMs &&
            int32(now - hold.cooldownUntilMs) < 0)
            return std::string();
        // The grace is once per fight: a member below the rest line who does not sit down holds the group
        // REST_HOLD_SIT_GRACE_MS after it, not again and again (the regen wait logs such a member as a stall).
        if (!hold.graceSinceMs)
            hold.graceSinceMs = now;
        bool const inGrace = getMSTimeDiff(hold.graceSinceMs, now) < REST_HOLD_SIT_GRACE_MS;
        std::string who;
        for (LeaderGroup::MemberInfo const& member : group.Members())
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || !bot->IsAlive() || bot->GetMap() != leader->GetMap())
                continue;
            // A member still sitting holds the rest even when the leader walked off (rest_hold was "done" while
            // the healer drank 57 yd behind).
            bool const sitting = RestState::IsDrinking(bot) || RestState::IsEating(bot);
            if (!sitting && bot->GetDistance(leader) > RestState::GROUP_RANGE)
                continue;
            if (bot->IsInCombat())
            {
                hold.sinceMs = 0;
                hold.graceSinceMs = 0;
                return std::string();
            }
            char const* state = nullptr;
            // [regen_wait_below_line] a sitter holds only while below its own line (RestState::WantsDrink /
            // WantsFood), a member about to sit only below the hard lines (45 % health, 20 % mana)
            bool const hardLine = bot->GetHealthPct() < 45.0f ||
                                  (bot->GetMaxPower(POWER_MANA) && bot->GetPowerPct(POWER_MANA) < 20.0f);
            // [lead15] LEADER_ONE_REST_RULE: a member that sits is waited for, whatever its line
            bool const anySitter =
                Leader::Leads(Leader::LEADER_ONE_REST_RULE) && leaderJudgesRest && !MemberFull(bot);
            if (RestState::IsDrinking(bot) &&
                (anySitter || !REST_HOLD_BELOW_LINE || hardLine || RestState::WantsDrink(bot)))
                state = "drinking";
            else if (RestState::IsEating(bot) &&
                     (anySitter || !REST_HOLD_BELOW_LINE || hardLine || RestState::WantsFood(bot)))
                state = "eating";
            else if (inGrace && (!REST_HOLD_BELOW_LINE || hardLine))
            {
                bool const mana = member.usesMana &&
                                  bot->GetMaxPower(POWER_MANA);
                if (mana && RestState::WantsDrink(bot) && HasRestItem(bot, 59))
                    state = "to drink";
                else if (RestState::WantsFood(bot) && HasRestItem(bot, 11))
                    state = "to eat";
            }
            if (state)
                who += (who.empty() ? "" : ",") + member.name + "(" + state + ")";
        }
        if (who.empty())
        {
            hold.sinceMs = 0;
            return std::string();
        }
        // [lead6] LEADER_ONE_READINESS: the rest ends when the members stand (the leader's one readiness judge),
        // not after REST_HOLD_MAX_MS - the timeout walked the leader on while they still sat
        bool const leaderJudges = Leader::Leads(Leader::LEADER_ONE_READINESS) && group.HasContent() &&
                                  !group.IsRaid() && group.ContentSize() <= 5 &&
                                  Leader::Orders::IsTracked(group.LeaderGuid());
        if (!hold.sinceMs)
            hold.sinceMs = now;
        else if (!leaderJudges && getMSTimeDiff(hold.sinceMs, now) > REST_HOLD_MAX_MS)
        {
            hold.sinceMs = 0;
            hold.cooldownUntilMs = now + REST_HOLD_COOLDOWN_MS;
            hold.timedOut = true;
            return std::string();
        }
        hold.members = who;
        return who;
    }

    bool SpotBossIsNext(LeaderGroup& group, EventSpotDef const& spot, Player* leader)
    {
        int32 lowest = -1;
        int32 spotOrder = -1;
        for (BossFacts const& boss : group.Bosses())
        {
            if (boss.killed || boss.failed || boss.order < 0)
                continue;
            if (lowest < 0 || boss.order < lowest)
                lowest = boss.order;
            bool ofSpot = false;
            for (uint32 entry : spot.untilDead)
                ofSpot |= boss.entry == entry ||
                          std::find(boss.credits.begin(), boss.credits.end(), entry) != boss.credits.end();
            // Not there yet, or there but not attackable before its event (Zul'Farrak: Witch Doctor Zum'rah is
            // faction 35 until areatrigger 962 sets 37).
            bool waits = boss.creature.IsEmpty();
            if (!waits && leader && leader->GetMap())
                if (Creature* creature = leader->GetMap()->GetCreature(boss.creature))
                    waits = creature->IsAlive() && !leader->IsValidAttackTarget(creature);
            if (ofSpot && waits && (spotOrder < 0 || boss.order < spotOrder))
                spotOrder = boss.order;
        }
        return spotOrder >= 0 && spotOrder == lowest;
    }

    namespace
    {
        using PlannedStop = GroupRouteState::PlannedStop;

        // [lead204] a mob's planned spot: where it was spawned (a wanderer walks round it)
        Position PlanSpawnSpot(Creature* mob)
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            mob->GetRespawnPosition(x, y, z);
            return Position(x, y, z);
        }

        // [lead204] the leader's walk from `from` to `to` (its own filter, the long search when the smooth path
        // runs out), < 0 when there is none
        float PlanWalkLength(Player* leader, Position const& from, Position const& to)
        {
            G3D::Vector3 const start(from.GetPositionX(), from.GetPositionY(), from.GetPositionZ());
            PathResult const walk = RoutePath(leader, to.GetPositionX(), to.GetPositionY(), to.GetPositionZ(), &start);
            return walk.reachable && walk.complete ? walk.length : -1.0f;
        }

        // [lead204] the walk between two plan spots, measured once per pair while both stand where they were
        float PlanWalkBetween(LeaderGroup& group, Player* leader, ObjectGuid fromGuid, Position const& from,
                              ObjectGuid toGuid, Position const& to)
        {
            bool const swapped = toGuid < fromGuid;
            auto const key = swapped ? std::make_pair(toGuid, fromGuid) : std::make_pair(fromGuid, toGuid);
            Position const& first = swapped ? to : from;
            Position const& second = swapped ? from : to;
            GroupRouteState::PlanWalk& walk = group.Route().planWalks[key];
            if (walk.length < 0.0f || walk.from.GetExactDist(first) > Leader::LEADER_COMMIT_MOVED_YD ||
                walk.to.GetExactDist(second) > Leader::LEADER_COMMIT_MOVED_YD)
            {
                walk.from = first;
                walk.to = second;
                walk.length = PlanWalkLength(leader, first, second);
                if (walk.length < 0.0f)  // the other way round (a drop walks one way only)
                    walk.length = PlanWalkLength(leader, second, first);
            }
            return walk.length;
        }

        // [lead205] the nearest the forecast `track` comes to the straight legs through `spots`: the sample, the leg
        // (the spot it ends at) and the distance (3D)
        float TrackNearestLeg(std::vector<G3D::Vector3> const& track, std::vector<Position> const& spots,
                              size_t& sample, size_t& leg)
        {
            float nearest = -1.0f;
            for (size_t s = 0; s < track.size(); ++s)
            {
                for (size_t i = 1; i < spots.size(); ++i)
                {
                    G3D::Vector3 const a(spots[i - 1].GetPositionX(), spots[i - 1].GetPositionY(),
                                         spots[i - 1].GetPositionZ());
                    G3D::Vector3 const b(spots[i].GetPositionX(), spots[i].GetPositionY(), spots[i].GetPositionZ());
                    G3D::Vector3 const ab = b - a;
                    float const lengthSq = ab.squaredLength();
                    float const t = lengthSq > 0.0f ? std::clamp((track[s] - a).dot(ab) / lengthSq, 0.0f, 1.0f) : 0.0f;
                    float const distance = (track[s] - (a + ab * t)).length();
                    if (nearest < 0.0f || distance < nearest)
                    {
                        nearest = distance;
                        sample = s;
                        leg = i;
                    }
                }
            }
            return nearest;
        }

        std::string PlanStopName(Map* map, PlannedStop const& stop)
        {
            Creature const* mob = map->GetCreature(stop.anchor);
            return Acore::StringFormat(
                "{}{}", mob ? mob->GetName() : std::string("?"),
                stop.mobs.size() > 1 ? Acore::StringFormat(" +{}", stop.mobs.size() - 1) : std::string());
        }
    }

    // [lead204] user: the leader plans the visiting order of the on-path packs to the next boss once - the
    // shortest walk over them on the navmesh (ShortestTour, the order tools/ideal_route.py computes: b181 rfc-h
    // 1886 yd vs 2327-2803 yd walked by the greedy nearest pick, Earthborer 51 pulled from below and walked back to
    // 149-189 yd) - and the route's pick follows it. Planned again only when a fact of it changes: the next boss, a
    // pack joins the path or leaves it (refused, skipped, off the path), a pack walked off its spot, a walker's walk
    // changed; a pack that dies is the plan's step done. [lead205] a patrol is no stop of its own where its walk
    // comes to a stop's fight (its forecast at the group's arrival there): it joins that fight; else it is met where
    // its walk comes nearest to the plan's walk, and taken at once when its walk reaches the leader's walk to the
    // next stop. `mobs`: this retarget's plain on-path idle trash. The next stop's mob nearest the leader (a patrol
    // stop: the patrol), empty without a plan; `step` says which.
    ObjectGuid PlannedNext(LeaderGroup& group, Player* leader, Creature* nextBoss, std::vector<Creature*> const& mobs,
                           std::string& step)
    {
        step.clear();
        Map* map = leader->GetMap();
        GroupRouteState& route = group.Route();
        // a refusal from where the leader stood ends once it stands elsewhere
        for (auto refused = route.planRefusedFrom.begin(); refused != route.planRefusedFrom.end();)
        {
            if (leader->GetExactDist(refused->second) > Leader::LEADER_COMMIT_MOVED_YD)
                refused = route.planRefusedFrom.erase(refused);
            else
                ++refused;
        }

        std::vector<Creature*> statics;
        std::vector<Creature*> walkers;
        std::map<ObjectGuid, std::vector<G3D::Vector3>> forecasts;
        float sampleSec = 1.0f;
        for (Creature* mob : mobs)
        {
            if (route.planRefusedFrom.count(mob->GetGUID()))
                continue;
            std::vector<G3D::Vector3> track;
            if (PatrolWalkPath(mob) && PullPlanning::PatrolForecast(mob, track, sampleSec))
            {
                walkers.push_back(mob);
                forecasts[mob->GetGUID()] = std::move(track);
            }
            else
                statics.push_back(mob);
        }

        // the packs: mobs that stood together at spawn (the planner's own pack rule), joined transitively
        std::vector<size_t> parent(statics.size());
        for (size_t i = 0; i < parent.size(); ++i)
            parent[i] = i;
        auto const root = [&parent](size_t i)
        {
            while (parent[i] != i)
                i = parent[i] = parent[parent[i]];
            return i;
        };
        for (size_t i = 0; i < statics.size(); ++i)
            for (size_t j = i + 1; j < statics.size(); ++j)
                if (PullPlanning::SpawnsWith(statics[i], statics[j]))
                    parent[root(i)] = root(j);
        std::map<size_t, std::vector<Creature*>> byRoot;
        for (size_t i = 0; i < statics.size(); ++i)
            byRoot[root(i)].push_back(statics[i]);

        std::vector<PlannedStop> packs;
        for (auto const& [rootIdx, members] : byRoot)
        {
            float cx = 0.0f;
            float cy = 0.0f;
            float cz = 0.0f;
            for (Creature* mob : members)
            {
                Position const spawn = PlanSpawnSpot(mob);
                cx += spawn.GetPositionX();
                cy += spawn.GetPositionY();
                cz += spawn.GetPositionZ();
            }
            float const size = float(members.size());
            Position const centre(cx / size, cy / size, cz / size);
            PlannedStop pack;
            Creature* anchor = nullptr;
            for (Creature* mob : members)
            {
                pack.mobs.push_back(mob->GetGUID());
                if (!anchor || PlanSpawnSpot(mob).GetExactDist(centre) < PlanSpawnSpot(anchor).GetExactDist(centre))
                    anchor = mob;
            }
            std::sort(pack.mobs.begin(), pack.mobs.end());
            pack.anchor = anchor->GetGUID();
            pack.spot = PlanSpawnSpot(anchor);
            packs.push_back(std::move(pack));
        }

        // the plan's facts: what is gone is a step done, everything else that changed plans again
        std::set<ObjectGuid> current;
        for (Creature* mob : mobs)
            if (!route.planRefusedFrom.count(mob->GetGUID()))
                current.insert(mob->GetGUID());
        std::string why;
        ObjectGuid const bossGuid = nextBoss ? nextBoss->GetGUID() : ObjectGuid::Empty;
        if (route.routePlan.empty() && route.routePlanMeets.empty())
            why = current.empty() ? std::string() : "first plan";
        else if (route.routePlanBoss != bossGuid)
            why = Acore::StringFormat("the next boss is {}", nextBoss ? nextBoss->GetName() : std::string("none"));
        std::set<ObjectGuid> planned;
        // [lead1283] the stops cleared since the plan to this boss started: their spots in order (the walk behind the
        // group); a plan that ran out of stops keeps them
        bool const fresh = route.routePlanBoss != bossGuid;
        for (auto behind = route.planBehind.begin(); behind != route.planBehind.end();)
            behind = current.count(*behind) ? std::next(behind) : route.planBehind.erase(behind);
        auto const reviewStops = [&](std::vector<PlannedStop>& stops, bool cleared)
        {
            for (auto stop = stops.begin(); stop != stops.end();)
            {
                std::vector<ObjectGuid> alive;
                std::string left;
                for (ObjectGuid const& guid : stop->mobs)
                {
                    Creature* mob = map->GetCreature(guid);
                    if (!mob || !mob->IsAlive() || mob->IsInCombat())
                        continue;  // dead, or fought now: done with its fight
                    alive.push_back(guid);
                    if (!current.count(guid) && left.empty())
                        left = Acore::StringFormat(
                            "{} left the path ({})", mob->GetName(),
                            route.planRefusedFrom.count(guid) ? "refused from here" : "refused, skipped or off it");
                    else if (!stop->patrol && left.empty())
                    {
                        float wander =
                            mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE ? mob->GetWanderDistance() : 0.0f;
                        if (mob->GetExactDist(PlanSpawnSpot(mob)) > wander + Leader::LEADER_COMMIT_MOVED_YD)
                            left = Acore::StringFormat("{} walked off its spot", mob->GetName());
                    }
                    else if (stop->patrol && !forecasts.count(guid) && left.empty())
                        left = Acore::StringFormat("{} left its walk", mob->GetName());
                }
                if (why.empty() && !left.empty())
                    why = left;
                if (alive.empty())
                {
                    if (cleared && !stop->patrol)
                        route.routePlanDone.push_back(stop->spot);
                    stop = stops.erase(stop);
                    continue;
                }
                planned.insert(alive.begin(), alive.end());
                ++stop;
            }
        };
        size_t const headBefore = route.routePlan.size();
        ObjectGuid const headAnchor = route.routePlan.empty() ? ObjectGuid::Empty : route.routePlan.front().anchor;
        reviewStops(route.routePlan, true);
        reviewStops(route.routePlanMeets, false);
        if (fresh)
        {
            route.routePlanDone.clear();
            route.planBehind.clear();
        }
        // a walker that should have come to a fight that is over now (its stop done) is planned again
        for (PlannedStop const& meet : route.routePlanMeets)
            if (why.empty() && !meet.meetsAt.IsEmpty() &&
                std::none_of(route.routePlan.begin(), route.routePlan.end(),
                             [&meet](PlannedStop const& stop) { return stop.anchor == meet.meetsAt; }))
                if (Creature const* walker = map->GetCreature(meet.anchor))
                    why = Acore::StringFormat("{} did not come to its fight", walker->GetName());
        bool joinOnly = false;
        if (why.empty())
            for (Creature* mob : mobs)
                if (!planned.count(mob->GetGUID()) && !route.planRefusedFrom.count(mob->GetGUID()) &&
                    !route.planBehind.count(mob->GetGUID()))
                {
                    why = Acore::StringFormat("{} joined the path", mob->GetName());
                    joinOnly = true;
                    break;
                }

        if (!why.empty())
        {
            // [lead1283] b250 rfc-h 5B-10 230 s: "Ragefire Shaman joined the path" and the tour from where the tank
            // stood put its spot first, 101 yd back beside the corridor the group had cleared, then 130 yd forward
            // again (plan 543 yd against 369). A pack new to the plan that lies nearer the cleared walk than the walk
            // ahead, the walk to it passing a cleared stop, and out of its aggro reach of the walk ahead (it does not
            // follow the group) is behind the group: no stop, until it leaves the path or a new plan starts
            std::vector<PlannedStop> const kept = route.routePlan;
            bool const toBoss = nextBoss && nextBoss->IsAlive();
            Position const start = leader->GetPosition();
            Position const bossSpot = toBoss ? PlanSpawnSpot(nextBoss) : Position();
            auto const nearestTo = [](Position const& at, std::vector<Position> const& line)
            {
                if (line.size() == 1)
                    return at.GetExactDist(line.front());
                size_t sample = 0;
                size_t leg = 1;
                std::vector<G3D::Vector3> const point{
                    G3D::Vector3(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ())};
                return TrackNearestLeg(point, line, sample, leg);
            };
            std::vector<Position> ahead{start};
            for (PlannedStop const& stop : kept)
                if (!stop.patrol)
                    ahead.push_back(stop.spot);
            if (toBoss)
                ahead.push_back(bossSpot);
            JsonArray behindLog;
            auto const isOld = [&planned](PlannedStop const& pack)
            {
                return std::any_of(pack.mobs.begin(), pack.mobs.end(),
                                   [&planned](ObjectGuid const& guid) { return planned.count(guid) != 0; });
            };
            for (auto pack = packs.begin(); pack != packs.end();)
            {
                bool behind = std::all_of(pack->mobs.begin(), pack->mobs.end(), [&route](ObjectGuid const& guid)
                                          { return route.planBehind.count(guid) != 0; });
                if (!behind && !fresh && !isOld(*pack) && !route.routePlanDone.empty())
                {
                    Creature* anchor = map->GetCreature(pack->anchor);
                    float const back = nearestTo(pack->spot, route.routePlanDone);
                    float const forward = nearestTo(pack->spot, ahead);
                    float const far = start.GetExactDist(pack->spot);
                    bool const pastCleared =
                        std::any_of(route.routePlanDone.begin(), route.routePlanDone.end(),
                                    [&](Position const& done)
                                    { return start.GetExactDist(done) < far && done.GetExactDist(pack->spot) < far; });
                    behind = anchor && back >= 0.0f && forward >= 0.0f && back < forward && pastCleared &&
                             forward > AggroReach(anchor, leader);
                    if (behind)
                    {
                        route.planBehind.insert(pack->mobs.begin(), pack->mobs.end());
                        behindLog.Raw(JsonObject()
                                          .Str("stop", PlanStopName(map, *pack))
                                          .Real("back", back)
                                          .Real("ahead", forward)
                                          .Real("reach", AggroReach(anchor, leader))
                                          .Real("fromTank", far)
                                          .Build());
                        LOG_INFO("playerbots", "[lead1283] {}: {} is behind the group ({:.0f} yd from the cleared "
                                 "walk, {:.0f} yd from the walk ahead): no stop",
                                 group.Run().runId, PlanStopName(map, *pack), back, forward);
                    }
                }
                pack = behind ? packs.erase(pack) : std::next(pack);
            }
            // a join keeps the plan's order: the new packs go where they add the least walk
            bool const insertOnly = joinOnly && !fresh && !kept.empty();

            route.routePlan.clear();
            route.routePlanMeets.clear();
            route.routePlanBoss = bossGuid;
            route.routePlanWhy = why;
            route.routePlanLength = 0.0f;
            // the stops: the leader where it stands, the packs, the next boss last
            std::vector<PlannedStop> stops;
            JsonArray unreachable;
            std::vector<float> fromLeader;
            if (insertOnly)
            {
                // the plan's stops in their order, each with its pack as it is now (a mob that joined it included)
                std::vector<bool> used(packs.size(), false);
                for (PlannedStop const& old : kept)
                {
                    if (old.patrol)
                        continue;
                    for (size_t i = 0; i < packs.size(); ++i)
                        if (!used[i] && std::any_of(old.mobs.begin(), old.mobs.end(), [&](ObjectGuid const& guid)
                                                    {
                                                        return std::find(packs[i].mobs.begin(), packs[i].mobs.end(),
                                                                         guid) != packs[i].mobs.end();
                                                    }))
                        {
                            used[i] = true;
                            route.routePlan.push_back(packs[i]);
                            break;
                        }
                }
                auto const legWalk = [&](size_t at, PlannedStop const& stop)  // into the gap before route.routePlan[at]
                {
                    float const in = at == 0 ? PlanWalkLength(leader, start, stop.spot)
                                             : PlanWalkBetween(group, leader, route.routePlan[at - 1].anchor,
                                                               route.routePlan[at - 1].spot, stop.anchor, stop.spot);
                    bool const last = at == route.routePlan.size();
                    if (last && !toBoss)
                        return in;
                    ObjectGuid const nextGuid = last ? bossGuid : route.routePlan[at].anchor;
                    Position const& nextSpot = last ? bossSpot : route.routePlan[at].spot;
                    float const out = PlanWalkBetween(group, leader, stop.anchor, stop.spot, nextGuid, nextSpot);
                    float const was =
                        at == 0 ? PlanWalkLength(leader, start, nextSpot)
                                : PlanWalkBetween(group, leader, route.routePlan[at - 1].anchor,
                                                  route.routePlan[at - 1].spot, nextGuid, nextSpot);
                    return in < 0.0f || out < 0.0f ? -1.0f : in + out - std::max(was, 0.0f);
                };
                for (size_t i = 0; i < packs.size(); ++i)
                {
                    if (used[i])
                        continue;
                    if (PlanWalkLength(leader, start, packs[i].spot) < 0.0f)
                    {
                        unreachable.Str(PlanStopName(map, packs[i]));
                        continue;
                    }
                    float bestAdd = -1.0f;
                    size_t bestAt = route.routePlan.size();
                    for (size_t at = 0; at <= route.routePlan.size(); ++at)
                        if (float const add = legWalk(at, packs[i]); add >= 0.0f && (bestAdd < 0.0f || add < bestAdd))
                        {
                            bestAdd = add;
                            bestAt = at;
                        }
                    route.routePlan.insert(route.routePlan.begin() + bestAt, packs[i]);
                }
            }
            for (PlannedStop& pack : packs)
            {
                if (insertOnly)
                    break;
                float const walk = PlanWalkLength(leader, start, pack.spot);
                if (walk < 0.0f)
                {
                    unreachable.Str(PlanStopName(map, pack));
                    continue;
                }
                fromLeader.push_back(walk);
                stops.push_back(pack);
            }
            size_t const count = stops.size() + 1 + (toBoss ? 1 : 0);
            float const inf = std::numeric_limits<float>::infinity();
            std::vector<std::vector<float>> cost(count, std::vector<float>(count, inf));
            for (size_t i = 0; i < stops.size(); ++i)
            {
                cost[0][i + 1] = fromLeader[i];
                cost[i + 1][0] = fromLeader[i];
                for (size_t j = i + 1; j < stops.size(); ++j)
                {
                    float const walk =
                        PlanWalkBetween(group, leader, stops[i].anchor, stops[i].spot, stops[j].anchor, stops[j].spot);
                    cost[i + 1][j + 1] = cost[j + 1][i + 1] = walk < 0.0f ? inf : walk;
                }
                if (toBoss)
                {
                    float const walk =
                        PlanWalkBetween(group, leader, stops[i].anchor, stops[i].spot, bossGuid, bossSpot);
                    cost[i + 1][count - 1] = cost[count - 1][i + 1] = walk < 0.0f ? inf : walk;
                }
            }
            if (toBoss && !insertOnly)
            {
                float const walk = PlanWalkLength(leader, start, bossSpot);
                cost[0][count - 1] = cost[count - 1][0] = walk < 0.0f ? inf : walk;
            }
            float length = 0.0f;
            std::vector<Position> spots{start};
            if (insertOnly)
            {
                for (size_t i = 0; i < route.routePlan.size() && std::isfinite(length); ++i)
                {
                    float const walk = i == 0 ? PlanWalkLength(leader, start, route.routePlan[i].spot)
                                              : PlanWalkBetween(group, leader, route.routePlan[i - 1].anchor,
                                                                route.routePlan[i - 1].spot, route.routePlan[i].anchor,
                                                                route.routePlan[i].spot);
                    length = walk < 0.0f ? inf : length + walk;
                    spots.push_back(route.routePlan[i].spot);
                }
                if (toBoss && std::isfinite(length))
                {
                    float const walk = route.routePlan.empty()
                                           ? PlanWalkLength(leader, start, bossSpot)
                                           : PlanWalkBetween(group, leader, route.routePlan.back().anchor,
                                                             route.routePlan.back().spot, bossGuid, bossSpot);
                    length = walk < 0.0f ? inf : length + walk;
                }
            }
            else
            {
                std::vector<size_t> const order = ShortestTour(cost, toBoss, length);
                for (size_t node : order)
                    if (node >= 1 && node <= stops.size())
                    {
                        route.routePlan.push_back(stops[node - 1]);
                        spots.push_back(stops[node - 1].spot);
                    }
            }
            if (toBoss)
                spots.push_back(bossSpot);
            route.routePlanLength = std::isfinite(length) ? length : -1.0f;

            // [lead205] each walker: where its forecast is when the group arrives at a stop (the walk there at the
            // leader's run speed); within its aggro reach of that stop it comes to that fight, else it is met
            // where its walk comes nearest to the plan's walk (a stop of its own, cheapest insertion)
            float const speed = std::max(leader->GetSpeed(MOVE_RUN), 0.1f);
            JsonArray meets;
            for (Creature* walker : walkers)
            {
                std::vector<G3D::Vector3> const& track = forecasts[walker->GetGUID()];
                float const reach = AggroReach(walker, leader);
                PlannedStop stop;
                stop.anchor = walker->GetGUID();
                stop.mobs.push_back(walker->GetGUID());
                stop.patrol = true;
                float arrive = 0.0f;
                for (size_t i = 1; i < spots.size() && i <= route.routePlan.size() && stop.meetsAt.IsEmpty(); ++i)
                {
                    arrive += spots[i - 1].GetExactDist(spots[i]) / speed;
                    size_t const sample = size_t(arrive / sampleSec);
                    if (sample >= track.size())
                        break;
                    G3D::Vector3 const& at = track[sample];
                    if (spots[i].GetExactDist(at.x, at.y, at.z) <= reach)
                    {
                        stop.meetsAt = route.routePlan[i - 1].anchor;
                        stop.spot = Position(at.x, at.y, at.z);
                        meets.Raw(JsonObject()
                                      .Str("walker", walker->GetName())
                                      .Str("at", PlanStopName(map, route.routePlan[i - 1]))
                                      .Real("inSec", arrive)
                                      .Build());
                    }
                }
                if (!stop.meetsAt.IsEmpty())
                {
                    route.routePlanMeets.push_back(stop);
                    continue;
                }
                size_t sample = 0;
                size_t leg = 1;
                if (spots.size() < 2 || TrackNearestLeg(track, spots, sample, leg) < 0.0f)
                {
                    sample = 0;
                    leg = route.routePlan.size() + 1;
                }
                stop.spot = Position(track[sample].x, track[sample].y, track[sample].z);
                size_t const at = std::min(leg - 1, route.routePlan.size());  // before the stop that leg ends at
                route.routePlan.insert(route.routePlan.begin() + at, stop);
                spots.insert(spots.begin() + at + 1, stop.spot);
            }

            JsonArray planOrder;
            float walked = 0.0f;
            for (size_t i = 0; i < route.routePlan.size(); ++i)
            {
                PlannedStop const& stop = route.routePlan[i];
                float const leg = spots[i].GetExactDist(spots[i + 1]);
                walked += leg;
                JsonArray lows;  // [lead1284] the stop's mobs (the triage's route_back matches stops by them)
                for (ObjectGuid const& guid : stop.mobs)
                    lows.Num(int64(guid.GetCounter()));
                planOrder.Raw(JsonObject()
                              .Str("stop", PlanStopName(map, stop))
                              .Bool("walker", stop.patrol)
                              .Raw("mobs", lows.Build())
                              .Real("x", stop.spot.GetPositionX())
                              .Real("y", stop.spot.GetPositionY())
                              .Real("z", stop.spot.GetPositionZ())
                              .Build());
            }
            group.LogEvent("route_plan", JsonObject()
                                             .Str("why", why)
                                             .Str("boss", nextBoss ? nextBoss->GetName() : std::string())
                                             .Real("length", route.routePlanLength)
                                             .Real("straight", walked)
                                             .Num("stops", int64(route.routePlan.size()))
                                             .Raw("order", planOrder.Build())
                                             .Raw("meets", meets.Build())
                                             .Raw("noWalk", unreachable.Build())
                                             .Str("mode", insertOnly ? "insert" : "tour")
                                             .Raw("behind", behindLog.Build()));
            LOG_INFO("playerbots", "[lead204] {}: route plan ({}): {} stops to {}, {:.0f} yd", group.Run().runId, why,
                     route.routePlan.size(), nextBoss ? nextBoss->GetName() : std::string("no boss"),
                     route.routePlanLength);
        }
        else if (route.routePlan.size() != headBefore && !route.routePlan.empty() &&
                 route.routePlan.front().anchor != headAnchor)
            group.LogEvent("route_plan_step", JsonObject()
                                                  .Str("next", PlanStopName(map, route.routePlan.front()))
                                                  .Num("left", int64(route.routePlan.size())));

        if (route.routePlan.empty())
            return ObjectGuid::Empty;

        // [lead205] a walker whose forecast reaches the leader's walk to the next stop while the group walks it
        // comes to the group now: taken first
        PlannedStop const& head = route.routePlan.front();
        float const speed = std::max(leader->GetSpeed(MOVE_RUN), 0.1f);
        float const headSec = leader->GetExactDist(head.spot) / speed;
        for (size_t i = 1; i < route.routePlan.size(); ++i)
        {
            PlannedStop const& stop = route.routePlan[i];
            auto const forecast = forecasts.find(stop.anchor);
            Creature* walker = stop.patrol ? map->GetCreature(stop.anchor) : nullptr;
            if (!walker || forecast == forecasts.end())
                continue;
            float const reach = AggroReach(walker, leader);
            std::vector<Position> const leg{leader->GetPosition(), head.spot};
            size_t sample = 0;
            size_t legIdx = 1;
            float const nearest = TrackNearestLeg(forecast->second, leg, sample, legIdx);
            if (nearest < 0.0f || nearest > reach || float(sample) * sampleSec > headSec)
                continue;
            PlannedStop const now = stop;
            route.routePlan.erase(route.routePlan.begin() + i);
            route.routePlan.insert(route.routePlan.begin(), now);
            if (route.planPatrolNowLogged.insert(now.anchor).second)
                group.LogEvent("route_plan_walker_now", JsonObject()
                                                            .Str("walker", walker->GetName())
                                                            .Real("inSec", float(sample) * sampleSec)
                                                            .Real("gap", nearest)
                                                            .Real("reach", reach));
            break;
        }

        PlannedStop const& next = route.routePlan.front();
        Creature* pick = nullptr;
        for (ObjectGuid const& guid : next.mobs)
            if (Creature* mob = map->GetCreature(guid);
                mob && current.count(guid) && (!pick || leader->GetExactDist(mob) < leader->GetExactDist(pick)))
                pick = mob;
        if (!pick)
            return ObjectGuid::Empty;
        step = Acore::StringFormat("planned order: {} (stop 1 of {}, plan {:.0f} yd: {})", PlanStopName(map, next),
                                   route.routePlan.size(), route.routePlanLength, route.routePlanWhy);
        return pick->GetGUID();
    }

    void Retarget(LeaderGroup& group, Player* leader)
    {
        Map* map = leader->GetMap();
        if (!map || map->GetInstanceId() != group.InstanceId())
            return;

        uint32 const now = getMSTime();
        // A starter step the bots' own strategy already did (MarkStartersDoneByGroup) is not waited for.
        MarkStartersDoneByGroup(group, leader);
        // ROUTE_REST_HOLD: no new target while a member sits eating / drinking or is about to (Steer logs it).
        if (!RestHoldMembers(group, leader).empty())
            return;
        // [ulda-ritual-hold] a summoning ritual's user keeps channelling (Steer's starter arrival set the hold)
        if (auto const hold = ritualHoldUntilMs.find(group.InstanceId());
            hold != ritualHoldUntilMs.end() && int32(hold->second - now) > 0 && !leader->IsInCombat())
            return;
        // [sm-wing-done] Every boss of this wing dead (Scarlet Monastery heroic 062726: Thalnos dead at 873 s, the
        // wing change at 1089 s through "boss route end in another wing"): out and in through the entrance of the
        // lowest-order boss left, out of combat. Only one-run contents (no bossEntries) of the maps whose wings are
        // told apart by entrance (SM, Dire Maul): a wing content's entrances are portals of the same wing.
        if (ROUTE_WING_DONE_CHANGE && (map->GetId() == 189 || map->GetId() == 429) &&
            group.ContentBossEntries().empty() && group.Route().entrances.size() > 1 &&
            group.Route().entranceIdx < group.Route().entrances.size() && !group.Route().wingWalk &&
                group.Route().escort.IsEmpty() &&
            !leader->IsInCombat() && !group.InCombat() && !EventGatherApplies(group) && EventSpotIndex(group) < 0)
        {
            int32 nextWing = -1;
            if (!WingBossesLeft(group, map, leader, nextWing) && nextWing >= 0 &&
                size_t(nextWing) != group.Route().entranceIdx)
            {
                group.LogEvent("wing_done_change",
                         JsonObject().Num("from", int64(group.Route().entranceIdx)).Num("to", nextWing));
                StartWingChange(group, size_t(nextWing), "all bosses of the wing dead");
                return;
            }
        }
        bool const onOculus = leader->GetMapId() == OCULUS_MAP_ID && !leader->GetVehicleBase();
        if (OculusDrakePhase(leader) && AnyMemberOnVehicle(group))
        {
            // On the drakes: the OC strategy flies the group; the harness picks no target until they landed.
            if (!group.Route().target.IsEmpty())
                group.LogEvent("drake_phase", JsonObject().Str("state", "harness targeting paused"));
            group.Route().target.Clear();
            group.Route().noTargets = false;
            group.ClearWaypoint(group.LeaderGuid());
            return;
        }

        // [lead132] user: a skip lasts until a fact changes, not for its duration: the leader moved off where it
        // skipped (UNREACHABLE_RECHECK_DISTANCE / _HEIGHT: another part of the instance - Azjol-Nerub: Anub'arak
        // skipped from the shaft, reachable once the group dropped to the pool), the skipped object moved
        // (LEADER_COMMIT_MOVED_YD), a door of the instance opened or closed, or the object is dead / gone
        uint32 const doorsOpen = OpenDoorCount(map);
        for (auto itr = group.Route().skipUntil.begin(); itr != group.Route().skipUntil.end();)
        {
            WorldObject* skipped = ObjectAccessor::GetWorldObject(*leader, itr->first);
            auto facts = group.Route().skipFacts.find(itr->first);
            if (skipped && (facts == group.Route().skipFacts.end() || facts->second.stamp != itr->second))
                facts = group.Route()
                            .skipFacts.insert_or_assign(itr->first,
                                                        GroupRouteState::SkipFacts{leader->GetPosition(),
                                                                                   skipped->GetPosition(), doorsOpen,
                                                                                   itr->second})
                            .first;
            auto const from = group.Route().unreachableFrom.find(itr->first);
            Position const leaderAt = from != group.Route().unreachableFrom.end()
                                          ? from->second
                                          : (facts != group.Route().skipFacts.end() ? facts->second.leaderAt
                                                                                    : leader->GetPosition());
            bool const moved = leader->GetExactDist2d(&leaderAt) > UNREACHABLE_RECHECK_DISTANCE ||
                               std::fabs(leader->GetPositionZ() - leaderAt.GetPositionZ()) > UNREACHABLE_RECHECK_HEIGHT;
            Unit const* skippedUnit = skipped ? skipped->ToUnit() : nullptr;
            bool const gone = !skipped || (skippedUnit && !skippedUnit->IsAlive());
            bool const objectMoved = facts != group.Route().skipFacts.end() && skipped &&
                                     skipped->GetExactDist(&facts->second.objectAt) > Leader::LEADER_COMMIT_MOVED_YD;
            bool const doorChanged = facts != group.Route().skipFacts.end() && facts->second.doorsOpen != doorsOpen;
            if (moved || gone || objectMoved || doorChanged)
            {
                group.Route().unreachableFrom.erase(itr->first);
                group.Route().skipFacts.erase(itr->first);
                itr = group.Route().skipUntil.erase(itr);
            }
            else
                ++itr;
        }

        std::vector<Position> blocked;
        for (BossFacts const& boss : group.Bosses())
            if (boss.failed)
                blocked.push_back(boss.homePos);

        if (ROUTE_ADOPT_GROUP_ESCORT && group.Route().escort.IsEmpty() && map->ToInstanceMap())
        {
            InstanceScript* const escortScript = map->ToInstanceMap()->GetInstanceScript();
            for (StarterTrack& starter : group.Route().starters)
            {
                EventStarterDef const* def = starter.def;
                if (def->kind != StarterKind::Escort || starter.uses >= def->maxUses ||
                    (def->skipContent && strcmp(def->skipContent, group.ContentKey().c_str()) == 0) ||
                    !EntriesDead(group, def->afterDead) || !StartersUsed(group, def->afterUsed) ||
                    !ScriptBossDone(escortScript, def->afterBossDone))
                    continue;
                uint32 const escortEntry = def->escortEntry ? def->escortEntry : def->entries.front();
                // [ohb-escort-follow] once the chain is under way (a use counted: Old Hillsbrad's Thrall, stages 1-8)
                // the creature without its gossip flag is on its way or in its talk - standing still too: the route
                // stays with it, not "full clear" (ohb-h 2026-10-25 062726: 25 min of full clear between his stages).
                bool const underWay = starter.uses > 0;
                Creature* adopted = nullptr;
                for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
                    if (creature && creature->IsInWorld() && creature->IsAlive() &&
                        creature->GetEntry() == escortEntry && !creature->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) &&
                        (creature->isMoving() || creature->IsInCombat() ||
                         (underWay && !(creature->GetGUID() == group.Route().escortIdleEndGuid &&
                                        creature->GetExactDist(&group.Route().escortIdleEndPos) < 5.0f))))
                    {
                        adopted = creature;
                        break;
                    }
                if (!adopted)
                    continue;
                // The bot's own step started it: one use; a stage already counted is followed, no use more.
                if (!underWay)
                    ++starter.uses;
                starter.wipesAtUse = group.Wipes();
                group.Route().escort = adopted->GetGUID();
                group.Route().escortEntry = adopted->GetEntry();
                group.Route().escortSinceMs = now;
                group.Route().escortIdleSinceMs = now;
                group.Route().escortLastPos = adopted->GetPosition();
                group.Route().escortNote = def->note;
                group.NoteProgress(now);
                group.LogEvent("escort_start", JsonObject().Str("note", def->note)
                                                    .Str("name", adopted->GetName())
                                                    .Num("entry", adopted->GetEntry())
                                                    .Str("by", "group (a bot's own event step)")
                                                    .Real("distance", leader->GetExactDist(adopted)));
                LOG_INFO("playerbots", "[route] {}: escort started by the group: {} {:.1f} yd - staying with it",
                         group.Run().runId, adopted->GetName(), leader->GetExactDist(adopted));
                break;
            }
        }

        // boss_neighbour_target: the neighbour pack stays the target while it lives and is not skipped
        // [lead129] user: no elapsed-time rule decides (NEIGHBOUR_TARGET_MS / PATROL_HANDOVER_MS 4 min were the old
        // clocks): the hold is its facts - alive, not skipped (a hunt or handover that ends clears it)
        if (!group.Route().neighbourTarget.IsEmpty() && group.Route().target == group.Route().neighbourTarget)
        {
            Creature* held = map->GetCreature(group.Route().neighbourTarget);
            if (held && held->IsAlive() && !group.Route().skipUntil.count(group.Route().neighbourTarget))
                return;
            group.Route().neighbourTarget.Clear();
        }

        // An escort runs: only what comes near the escorted creature is a target (it attacks the escort).
        Creature* escortNpc = ActiveEscort(group, map);

        // An event's waiting place (EventSpotDef::hold: Black Morass' Medivh, Violet Hold's prison door) that
        // applies now: only what comes within HOLD_TARGET_RADIUS of it is a target, bosses always. Black Morass
        // wave 8: the swamp's wildlife kept the leader busy for 25 minutes, the group never reached Medivh (the
        // spot is walked to only with nothing to fight) and no rift ever opened.
        // Closed parts of the instance (AreaGateDef) whose event has not opened them yet; without afterDead never
        // (Blackrock Depths: the Ring of Law stands).
        std::vector<AreaGateDef const*> closedAreas;
        for (AreaGateDef const* gate : AreaGates(group.MapId()))
            if (AreaGateForContent(*gate, group.ContentKey().c_str()) &&
                (gate->afterDead.empty() || !EntriesDead(group, gate->afterDead)))
                closedAreas.push_back(gate);
        // A wing closed to this run (onlyContent: UBRS for an LBRS group) is no way either: no route through it.
        // Standing in it already (pulled in, a fight that ran there), the way out is no crossing.
        std::vector<AreaGateDef const*> const keepOut = KeepOutAreas(closedAreas);
        bool const leaderInClosedArea =
            InAnyAreaGate(keepOut, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        // ROUTE_CLOSED_DOORS: the doors closed now (Shadowfang Keep's Courtyard Door before the prisoner event).
        std::vector<GameObject*> const closedDoors = ClosedDoors(map, leader);
        // ROUTE_CLOSED_DOORS_ALL: the leader's last waypoint was cut before a closed door (door_hold). A target the
        // walk to which runs through that door, and which the door does not open for, is dropped now: the route takes
        // one on this side, or the one that opens it (DoorOpensFor: a key creature, the door itself).
        // [b30_overpull] door spin hold (set below): while it runs, the door stays closed, nobody fights and no key
        // starter of that door is due, the retarget picks nothing (the leader stands at the door hold).
        if (auto const spinHold = doorSpinHolds.find(group.Run().runSeq); spinHold != doorSpinHolds.end())
        {
            GameObject const* spinDoor = map->GetGameObject(spinHold->second.second);
            bool keyDue = false;
            for (StarterTrack const& starter : group.Route().starters)
                keyDue |= spinDoor && starter.def->kind == StarterKind::UseItem &&
                          starter.uses < starter.def->maxUses && int32(now - starter.nextAllowedMs) >= 0 &&
                          std::find(starter.def->entries.begin(), starter.def->entries.end(), spinDoor->GetEntry()) !=
                              starter.def->entries.end();
            if (int32(now - spinHold->second.first) < 0 && spinDoor && spinDoor->GetGoState() == GO_STATE_READY &&
                !keyDue && !leader->IsInCombat())
                return;
            doorSpinHolds.erase(spinHold);
        }
        if (ROUTE_CLOSED_DOORS_ALL && !group.Route().doorHold.IsEmpty())
        {
            GameObject* door = map->GetGameObject(group.Route().doorHold);
            bool const fresh = group.Route().doorHoldMs && getMSTimeDiff(group.Route().doorHoldMs, now) <
                DOOR_HOLD_FRESH_MS;
            group.Route().doorHold.Clear();
            WorldObject* held = nullptr;
            if (!group.Route().target.IsEmpty())
            {
                held = map->GetCreature(group.Route().target);
                if (!held)
                    held = map->GetGameObject(group.Route().target);
            }
            // [strat_trap_gate_hold] a rat-trap portcullis (IsTrapGate) is waited out at the hold, never a skip.
            if (fresh && door && held && IsTrapGate(door) && door->GetGoState() == GO_STATE_READY &&
                (!group.Route().trapGateLogMs || getMSTimeDiff(group.Route().trapGateLogMs, now) >= TRAP_GATE_LOG_MS))
            {
                group.Route().trapGateLogMs = now ? now : 1;
                group.LogEvent("strat_trap_gate_hold",
                         JsonObject().Str("door", door->GetName()).Num("doorEntry", door->GetEntry()));
            }
            if (fresh && door && held && door->GetGoState() == GO_STATE_READY && !DoorOpensFor(door, held) &&
                !IsTrapGate(door) &&
                RouteThroughClosedDoor(EvaluatePath(leader, held).points, std::vector<GameObject*>{door},
                                       leader->GetPhaseMask()))
            {
                group.SkipTarget(group.Route().target, CLOSED_DOOR_SKIP_MS,
                           Acore::StringFormat("leader held at a closed door ({} {})", door->GetName(),
                                               door->GetEntry()));
                // [b30_overpull] door spin guard (CountDoorSpin): the DOOR_SPIN_SKIPS-th skip behind this door in a
                // row holds the retarget DOOR_SPIN_HOLD_MS with one line instead of the next skip
                bool spinLog = false;
                uint32 spinSkips = 0;
                uint32 spinWindowMs = 0;
                if (CountDoorSpin(group.Run().runSeq, door->GetGUID(), now, spinLog, spinSkips, spinWindowMs))
                {
                    group.ClearWaypoint(group.LeaderGuid());
                    if (spinLog)
                    {
                        std::string opener;
                        JsonObject fields =
                            DoorSpinFields(group, leader, door, spinSkips, spinWindowMs, "leader held at it", opener);
                        LOG_INFO("playerbots", "[door spin] {}: {} skips behind closed {} {} in {} s: retarget holds "
                                 "{} s (opener: {})", group.Run().runId, spinSkips, door->GetName(), door->GetEntry(),
                                 spinWindowMs / IN_MILLISECONDS, DOOR_SPIN_HOLD_MS / IN_MILLISECONDS, opener);
                        group.LogEvent("door_spin_hold", std::move(fields));
                    }
                    return;
                }
            }
        }
        // [queue-door-skip-lift] the targets skipped as "leader held at a closed door (<name> <entry>)" come back as
        // soon as that door stands open (the key used on it): strat-n 20261025-115805-strat-n-5A-6 skipped the
        // Gauntlet's mobs at 946-961 s behind the Gauntlet Gate 175357, used the Key to the City on it at 962 s, saw
        // "no targets" (13 hostiles, 94 skipped) at 969 s and changed wings at 999 s - out through the closed Service
        // Entrance Gate, 84 s of bot_cannot_move "no walk to the portal" until the stop at 1083 s.
        {
            std::vector<ObjectGuid> lifted;
            for (auto const& [guid, why] : group.Route().skipReason)
            {
                constexpr char const* prefix = "leader held at a closed door (";
                // [b32_st] ...and a next boss skipped "behind a closed door (<name> <entry>)": Sunken Temple's DOOR1
                // 149432 / DOOR2 149433 stand shut only while the Shade of Hakkar event runs (smart_scripts 149432 id
                // 0-1, conditions 22 GetData(2) == / != IN_PROGRESS); Eranikus comes back once they open (Avatar dead
                // or the event reset), not CLOSED_DOOR_SKIP_MS later.
                constexpr char const* bossPrefix = "next boss behind a closed door (";
                if ((why.rfind(prefix, 0) != 0 && why.rfind(bossPrefix, 0) != 0) ||
                    !group.Route().skipUntil.count(guid))
                    continue;
                size_t const close = why.rfind(')');
                size_t const space = why.rfind(' ', close);
                if (close == std::string::npos || space == std::string::npos || space + 1 >= close)
                    continue;
                uint32 const entry =
                    uint32(std::strtoul(why.substr(space + 1, close - space - 1).c_str(), nullptr, 10));
                bool stillClosed = false;
                for (GameObject* door : closedDoors)
                    stillClosed |= door->GetEntry() == entry;
                if (entry && !stillClosed && ROUTE_CLOSED_DOORS)
                    lifted.push_back(guid);
            }
            for (ObjectGuid const& guid : lifted)
            {
                group.LogEvent("skip_lifted", JsonObject().Str("guid", guid.ToString())
                                                   .Str("was", group.Route().skipReason[guid])
                                                   .Str("why", "the door stands open"));
                group.Route().skipUntil.erase(guid);
                group.Route().skipReason.erase(guid);
                group.Route().unreachableFrom.erase(guid);
            }
        }

        EventSpotDef const* holdSpot = nullptr;
        if (int32 const spotIdx = EventSpotIndex(group); spotIdx >= 0)
            if (EventSpotDef const* spot = EventSpots(group.MapId())[size_t(spotIdx)]; SpotHolds(group, *spot))
                holdSpot = spot;
        // [brd_ring_hold] A step that holds while its event runs (EventSpotDef::eventData, the Ring of Law): idle
        // bosses outside holdRadius and every starter wait too - 2026-10-25 brd-n: an Anvilrage Guardsman 86 yd out
        // was the target 1 s after the trigger, the group left the arena and the waves failed.
        bool const eventHold = holdSpot && holdSpot->eventData >= 0;
        if (eventHold && (!group.Route().eventHoldLogMs || getMSTimeDiff(group.Route().eventHoldLogMs, now) >=
            EVENT_HOLD_LOG_MS))
        {
            group.Route().eventHoldLogMs = now ? now : 1;
            group.LogEvent("brd_ring_hold",
                     JsonObject().Str("note", holdSpot->note).Bool("retryPending", group.Route().eventSpotRetry)
                         .Num("fails", group.Route().ringFails)
                         .Real("distance", leader->GetExactDist(holdSpot->x, holdSpot->y, holdSpot->z)));
        }

        // A walk-in event spot the next boss waits behind (ROUTE_EVENT_SPOT_FIRST): its boss is the lowest open one of
        // the route and appears only through it. The group clears what lies on the way and walks in, as players do;
        // the rest of the instance comes after. Utgarde Pinnacle 10:55 / 11:43 / 11:49 runs: Svala's areatrigger 5140
        // was walked to only with nothing else to fight, the route kept choosing Mindless Servants on the entrance
        // ledges and packs the tank did not see (skipped one after another) 200 yd east of it, and the run stalled
        // "Svala Sorrowgrave - not spawned (event not started)".
        EventSpotDef const* leadSpot = nullptr;
        if (ROUTE_EVENT_SPOT_FIRST && !holdSpot && !group.IsRaid())
            if (int32 const spotIdx = EventSpotIndex(group); spotIdx >= 0)
                if (EventSpotDef const* spot = EventSpots(group.MapId())[size_t(spotIdx)];
                    !SpotHolds(group, *spot) && SpotBossIsNext(group, *spot, leader))
                    leadSpot = spot;
        float const leadSpotDistance = leadSpot ? leader->GetExactDist(leadSpot->x, leadSpot->y, leadSpot->z) : 0.0f;

        // A scripted intro keeps its boss immune (ROUTE_INTRO_WAIT, Svala's 72 s talk): the group starts no new pull
        // and waits where it stands (Steer); what attacks it is fought.
        Creature* introBoss = ROUTE_INTRO_WAIT ? RunningIntroBoss(leader) : nullptr;
        if (auto const since = introWaitSinceMs.find(group.InstanceId());
            introBoss && since != introWaitSinceMs.end() && getMSTimeDiff(since->second, now) >= INTRO_WAIT_MAX_MS)
            introBoss = nullptr;  // waited long enough: the route goes on

        // Next boss of the route: the dungeon's own encounter order (DungeonEncounter.dbc encounterIndex), the
        // nearest one where the order is unknown. A boss of a later order is not pulled before it (Gundrak: Gal'darah
        // sits next to the entrance behind his gate), and trash right next to it waits.
        InstanceScript* script = map->ToInstanceMap() ? map->ToInstanceMap()->GetInstanceScript() : nullptr;
        Creature* nextBoss = nullptr;
        int32 nextOrder = -1;
        std::vector<std::pair<int32, Creature*>> standing;
        // The order holds while an earlier boss was found unreachable less than ORDER_HOLD_MS ago (it may be behind
        // an event, a drop or a long walk): the bosses after it are no route target and no pull yet, the trash is.
        int32 gateOrder = -1;
        for (auto const& [guid, sinceMs] : group.Route().orderHoldSinceMs)
        {
            Creature* held = map->GetCreature(guid);
            int32 const order = held && held->IsAlive() ? group.BossOrder(held) : -1;
            if (order >= 0 && getMSTimeDiff(sinceMs, now) < ORDER_HOLD_MS && (gateOrder < 0 || order < gateOrder))
                gateOrder = order;
        }
        // [hleft-boss-preclear] the bosses still waiting for their pre-clear, and the trash that is required for it
        std::vector<uint32> preClearBlocked;
        std::set<uint32> preClearKeys;
        for (BossPreClearDef const* def : BossPreClears(group.MapId(), group.Difficulty()))
        {
            Creature* defBoss = nullptr;
            for (ObjectGuid const& guid : group.Route().bossCreatures)
                if (Creature* boss = map->GetCreature(guid);
                    boss && boss->IsAlive() && boss->GetEntry() == def->bossEntry)
                    defBoss = boss;
            if (!defBoss)
                continue;
            // [preclear_next_boss_only] the pre-clear is his only while he is the next boss: an earlier boss of the
            // order still standing (not skipped, not friendly) goes first. dtk-h wave 23 (112011): King Dred's
            // Gutrippers / Scytheclaws were "required" from 19 s on, with Trollgore and then Novos before him; every
            // walk stopped at Novos ("stopped by Novos the Summoner") until the stall, 1/4.
            if (int32 const defOrder = group.BossOrder(defBoss); defOrder >= 0)
            {
                Creature* earlier = nullptr;
                for (ObjectGuid const& guid : group.Route().bossCreatures)
                {
                    Creature* other = map->GetCreature(guid);
                    if (!other || other == defBoss || !other->IsAlive() || group.Route().skipUntil.count(guid) ||
                        (other->IsFriendlyTo(leader) && !other->IsInCombat()))
                        continue;
                    if (int32 const otherOrder = group.BossOrder(other); otherOrder >= 0 && otherOrder < defOrder)
                    {
                        earlier = other;
                        break;
                    }
                }
                if (earlier)
                {
                    LOG_DEBUG("playerbots", "[preclear_next_boss_only] {}: {}'s pre-clear waits, {} comes first",
                              leader->GetName(), defBoss->GetName(), earlier->GetName());
                    continue;
                }
            }
            for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
            {
                if (!creature || !creature->IsAlive() || creature->IsInCombat() || creature->IsDungeonBoss() ||
                    creature->GetEntry() == def->bossEntry || !creature->IsHostileTo(leader) ||
                    group.Route().bossCreatures.count(creature->GetGUID()) ||
                    creature->GetExactDist(def->x, def->y, def->z) > def->radius ||
                    (!def->entries.empty() && std::find(def->entries.begin(), def->entries.end(),
                                                        creature->GetEntry()) == def->entries.end()) ||
                    group.Route().skipUntil.count(creature->GetGUID()) || !creature->IsVisible() ||
                    creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE))
                    continue;
                preClearKeys.insert(creature->GetEntry());
                if (std::find(preClearBlocked.begin(), preClearBlocked.end(), def->bossEntry) == preClearBlocked.end())
                    preClearBlocked.push_back(def->bossEntry);
            }
        }
        // [b46_strat] wing order: the deferred bosses come back once no other boss stands (or after the cap)
        if (auto const wingFound = hopelessWings.find(group.Run().runSeq);
            wingFound != hopelessWings.end() && !wingFound->second.deferred.empty())
        {
            HopelessWingState& wing = wingFound->second;
            bool otherStands = false;
            for (ObjectGuid const& guid : group.Route().bossCreatures)
                if (Creature* boss = map->GetCreature(guid); boss && boss->IsAlive() && !wing.deferred.count(guid))
                    otherStands = true;
            bool const capped = getMSTimeDiff(wing.deferredSinceMs, now) >= HOPELESS_WING_DEFER_MAX_MS;
            if (!otherStands || capped)
            {
                group.LogEvent("strat_wing_order",
                         JsonObject().Str("action", capped ? "deferred bosses back: cap reached"
                                                           : "deferred bosses back: no other boss stands")
                             .Num("deferred", int64(wing.deferred.size())));
                wing.deferred.clear();
                wing.heldSinceMs = 0;
                wing.checkedMs = 0;
            }
        }
        for (ObjectGuid const& guid : group.Route().bossCreatures)
        {
            Creature* boss = map->GetCreature(guid);
            if (!boss || !boss->IsAlive())
                continue;
            if (HopelessWingDeferred(group.Run().runSeq, guid))  // [b46_strat] the other wing first
                continue;
            // Violet Hold: the prisoners come out in a random order (instance_violet_hold.cpp EVENT_SUMMON_PORTAL
            // stores urand(BOSS_MORAGG, BOSS_ZURAMAT) as PERSISTENT_DATA_FIRST_BOSS / _SECOND_BOSS), but
            // instance_encounters names Erekem 29315 "First Prisoner" (order 0) and Moragg 29316 "Second Prisoner"
            // (order 1). One still in its cell is NON_ATTACKABLE until StartBossEncounter; it is no route boss and
            // holds nothing back. Wave 34 (12:22) and 08:09: Moragg was released at wave 6, Erekem (order 0) sat in
            // his cell as the "next boss", Moragg counted as a later boss and was never a target - the tank stood at
            // the door "no aggressive target" for 20 minutes, no new portal came (the next wave waits for the boss).
            if (map->GetId() == VIOLET_HOLD_MAP_ID && boss->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) &&
                !boss->IsInCombat())
                continue;
            // A boss phased out of the group's sight (spawn phaseMask) whose appearance is no event the harness knows
            // (no EncounterRequirement row) waits for a later boss: Sunken Temple's Dreamscythe 5721 / Weaver 5720
            // spawn in phase 2, their SmartAI brings them into phase 1 only after Jammal'an (2026-10-25 st-h: the
            // unseen Dreamscythe stayed the next boss 90 yd away, Jammal'an was a later boss, the defenders detours -
            // stalled 1/8).
            char const* const bossNeeds = EncounterRequirement(map->GetId(), boss->GetEntry());
            bool const phasedUntilBoss = bossNeeds && strcmp(bossNeeds, PHASED_UNTIL_BOSS) == 0;
            if (!leader->InSamePhase(boss) && !boss->IsInCombat() && (!bossNeeds || phasedUntilBoss))
            {
                if (phasedUntilBoss && phasedBossLogged.insert({group.Run().runSeq, boss->GetGUID()}).second)
                    group.LogEvent("boss_phased_wait",
                             JsonObject().Str("boss", boss->GetName()).Num("entry", boss->GetEntry())
                                 .Str("by", "phase"));
                continue;
            }
            // [st-phased-until-boss] hidden by flag rather than phase (Shade of Eranikus, NOT_SELECTABLE until
            // Jammal'an dies): no next boss either.
            if (phasedUntilBoss && boss->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) && !boss->IsInCombat())
            {
                if (phasedBossLogged.insert({group.Run().runSeq, boss->GetGUID()}).second)
                    group.LogEvent("boss_phased_wait",
                             JsonObject().Str("boss", boss->GetName()).Num("entry", boss->GetEntry())
                                 .Str("by", "not selectable"));
                continue;
            }
            // A boss friendly to the group (Uldaman's Lost Dwarves, faction 122 Ironforge, for an all-Alliance group)
            // is never a target: no next boss either, or the route detours everything else towards him (wave 8
            // ulda-h: 16 minutes, "nextBoss Baelog ... beyond sight distance"). He stays missing in the report.
            if (boss->IsFriendlyTo(leader) && !boss->IsInCombat())
                continue;
            // [event_boss_classic] A boss who is still a vendor / quest giver out of combat (BRD Plugger Spazzring
            // 9499, npcflag 130, faction 674) turns hostile only through his event (three stolen ale mugs / boars,
            // smart_scripts 165738 / 165739 -> 9499 counter 3 -> 949900 Start attack): no next boss until then.
            // brd-h 20261025-163739 18:11:00: body_pull on him, tank_view "aggressive target: npc flags".
            if (boss->HasNpcFlag(NPCFlags(UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_QUESTGIVER)) && !boss->IsInCombat())
                continue;
            // [hleft-boss-preclear] trash this boss is never pulled next to is still alive: he waits for it.
            if (std::find(preClearBlocked.begin(), preClearBlocked.end(), boss->GetEntry()) != preClearBlocked.end())
                continue;
            // [hleft-gd-galdarah-last] Gundrak: Gal'darah's bridge 192633 rises only once Slad'ran, Moorabi and
            // the Colossus are DONE (instance_gundrak.cpp altars); before that he is no next boss (062726: targeted
            // at 1932 s with Slad'ran alive).
            if (map->GetId() == GUNDRAK_MAP_ID && boss->GetEntry() == GUNDRAK_GALDARAH_ENTRY && script &&
                (script->GetBossState(GUNDRAK_SLADRAN_SLOT) != DONE ||
                 script->GetBossState(GUNDRAK_MOORABI_SLOT) != DONE ||
                 script->GetBossState(GUNDRAK_COLOSSUS_SLOT) != DONE))
                continue;
            int32 const order = group.BossOrder(boss);
            if (group.Route().skipUntil.count(guid))
            {
                if (gateOrder >= 0 && order > gateOrder)
                    standing.emplace_back(order, boss);  // still held back as a later boss
                continue;
            }
            standing.emplace_back(order, boss);
            if (gateOrder >= 0 && order > gateOrder)
                continue;
            bool const better = !nextBoss || (order >= 0 && (nextOrder < 0 || order < nextOrder)) ||
                                (order == nextOrder && leader->GetExactDist(boss) < leader->GetExactDist(nextBoss));
            if (better)
            {
                nextBoss = boss;
                nextOrder = order;
            }
        }

        // The next boss in order must be reachable, or the route would block on it (event-spawned or closed off).
        PathResult nextRoute = nextBoss && nextOrder >= 0 ? EvaluatePath(leader, nextBoss) : PathResult();
        Creature* const nextRouteBoss = nextBoss && nextOrder >= 0 ? nextBoss : nullptr;  // whose route nextRoute is
        // ROUTE_CLOSED_DOORS_ALL: a next boss whose route runs through a closed door that does not open for him is as
        // unreachable (the order hold below; skipped CLOSED_DOOR_SKIP_MS, the door may open with an event).
        GameObject* nextDoor =
            ROUTE_CLOSED_DOORS_ALL && nextRouteBoss && nextRoute.reachable && !closedDoors.empty()
                ? RouteThroughClosedDoor(nextRoute.points, DoorsClosedFor(closedDoors, nextRouteBoss),
                                         leader->GetPhaseMask())
                : nullptr;
        // [hleft-passage-door-route] A door that opens on a boss's death (PassageDoorBoss) is his exit, not the way to
        // him: the route takes the way round it (DoorFreeRoute) and the boss in front of it stays the next boss. Only
        // with no way round (Broggok in his cell) it stays "behind a closed door" (his lever starter comes first).
        if (nextDoor && PassageDoorBoss(script, nextDoor) >= 0)
        {
            std::vector<GameObject*> const shut = DoorsClosedFor(closedDoors, nextRouteBoss);
            PathResult const round = DoorFreeRoute(
                leader, G3D::Vector3(nextBoss->GetPositionX(), nextBoss->GetPositionY(), nextBoss->GetPositionZ()),
                shut);
            if (round.reachable && !RouteThroughClosedDoor(round.points, shut, leader->GetPhaseMask()))
            {
                uint32& loggedMs = passageDetourLogMs[group.InstanceId()];
                if (!loggedMs || getMSTimeDiff(loggedMs, now) > PASSAGE_DETOUR_LOG_MS)
                {
                    loggedMs = now ? now : 1;
                    group.LogEvent("passage_door_detour",
                             JsonObject().Str("boss", nextBoss->GetName()).Num("entry", nextBoss->GetEntry())
                                 .Str("door", nextDoor->GetName()).Num("doorEntry", nextDoor->GetEntry())
                                 .Real("throughDoor", nextRoute.length).Real("round", round.length));
                }
                nextRoute = round;
                nextDoor = nullptr;
            }
        }
        // GuardedDoorDef (Dire Maul West: Immol'thar's force field, the five Crystal Generators): while a guard the
        // group can still get to is alive, the boss behind the door stays the next boss - no closed-door skip, no
        // order hold - and the guards are the objectives (guardKeys). None left unskipped: the closed-door skip.
        std::set<uint32> guardKeys;
        if (nextDoor)
        {
            for (Creature* guard : PendingDoorGuards(map, nextDoor))
                if (!group.Route().skipUntil.count(guard->GetGUID()))
                    guardKeys.insert(guard->GetEntry());
            if (!guardKeys.empty())
                nextDoor = nullptr;
        }
        // [strat_trap_gate_wait] A rat-trap portcullis (IsTrapGate) opens again 20 s after it shut: the boss behind it
        // stays the next boss and his walk holds at the gate (door_hold) until it is open.
        if (nextDoor && IsTrapGate(nextDoor))
        {
            if (!group.Route().trapGateLogMs || getMSTimeDiff(group.Route().trapGateLogMs, now) >= TRAP_GATE_LOG_MS)
            {
                group.Route().trapGateLogMs = now ? now : 1;
                group.LogEvent("strat_trap_gate_wait",
                         JsonObject().Str("boss", nextBoss->GetName()).Str("door", nextDoor->GetName())
                             .Num("doorEntry", nextDoor->GetEntry()));
            }
            nextDoor = nullptr;
        }
        // [event_boss_waits] EventSpotDef::bossComesToSpot (UBRS: Rend 10429 NON_ATTACKABLE on his balcony behind
        // 175186, which opens with his death): no closed-door / unreachable skip and no order hold while such a spot's
        // afterDead are dead - the spot leads (SpotBossIsNext: he is no valid attack target). ubrs-n 2026-10-25 06:03:
        // 12 boss_unreachable "closedDoor 175186", run_end "Rend - skipped: next boss behind a closed door".
        bool eventBossWaits = false;
        if (nextRouteBoss && (nextDoor || !nextRoute.reachable) && !leader->IsValidAttackTarget(nextRouteBoss))
            for (EventSpotDef const* spot : EventSpots(group.MapId()))
                if (spot->bossComesToSpot && EntriesDead(group, spot->afterDead) &&
                    std::find(spot->untilDead.begin(), spot->untilDead.end(), nextRouteBoss->GetEntry()) !=
                        spot->untilDead.end())
                    eventBossWaits = true;
        if (eventBossWaits)
        {
            uint32& loggedMs = eventBossWaitLogMs[group.InstanceId()];
            if (!loggedMs || getMSTimeDiff(loggedMs, now) > DOOR_STARTER_LOG_MS)
            {
                loggedMs = now ? now : 1;
                group.LogEvent("event_boss_waits",
                         JsonObject().Str("boss", nextRouteBoss->GetName()).Num("entry", nextRouteBoss->GetEntry())
                             .Str("closedDoor", nextDoor ? nextDoor->GetName() : "")
                             .Num("closedDoorEntry", nextDoor ? nextDoor->GetEntry() : 0)
                             .Bool("reachable", nextRoute.reachable));
            }
            nextDoor = nullptr;
        }
        // A link to the next boss still closed by its afterDead: those creatures open it and are the objective now,
        // whether or not they are encounters. The Mechanar normal 2026-10-24 12:22: the lift is behind both Mo'arg
        // doors, door 2 opens with Gatewatcher Gyro-Kill (instance_mechanar.cpp doorData), who has no
        // DungeonEncounter row in normal mode - trash for the route, standing 160 yd back on the z 15 level, chosen
        // late, dropped as "respawning trash"; the link was never usable and the run stalled on "boss_unreachable".
        std::set<uint32> linkKeys;
        if (nextBoss && nextOrder >= 0 && !nextRoute.complete)
            for (RouteLinkDef const* link : RouteLinks(group.MapId()))
            {
                if (EntriesDead(group, link->afterDead))
                    continue;
                G3D::Vector3 const farEnd(link->toX, link->toY, link->toZ);
                if (!RoutePath(leader, nextBoss->GetPositionX(), nextBoss->GetPositionY(), nextBoss->GetPositionZ(),
                               &farEnd)
                         .reachable)
                    continue;
                for (uint32 entry : link->afterDead)
                    if (!EntriesDead(group, std::vector<uint32>{entry}))
                        linkKeys.insert(entry);
            }
        // No complete walk, but over a link the instance has opened (Gundrak's bridge): the group crosses it
        // (WalkToLink, CrossLinks) instead of skipping the boss or walking to the nearest edge (Gal'darah's partial
        // route ends in the moat below his wing and would count as "nearer").
        // ... or a complete walk only through a closed door that does not open for him (Shattered Halls: the jump
        // into the sewer while the Grand Warlock Chamber Door 182539 is shut).
        bool const viaLink = nextBoss && nextOrder >= 0 && (!nextRoute.complete || nextDoor) &&
                             LinkTo(group, leader, nextBoss) >= 0;
        // [event_boss_classic] With no next boss to walk to (the last ones appear only through an event), a gossip /
        // escort starter the navmesh does not reach from here is walked to over a route link (RouteLinkDef), as a
        // boss is. Wailing Caverns: after Serpentis and Verdan the group stands on their ledges (a navmesh part
        // reached only by the jump over the crack), the Disciple of Naralex 3678 stands below at -135.0,125.4,-78.1;
        // wc-h 20261025-163739 2920-3221 s: "starter_partial_walk" endLeft 76.9 at the ledge edge, the target
        // flipped Disciple <-> unreachable fallback trash for 5 minutes, Mutanus "event not started".
        constexpr float STARTER_LINK_SEARCH = 250.0f;
        bool viaStarterLink = false;
        if (!viaLink && (!nextBoss || nextOrder < 0))
            for (StarterTrack const& starter : group.Route().starters)
            {
                if (!starter.def ||
                    (starter.def->kind != StarterKind::Gossip && starter.def->kind != StarterKind::Escort) ||
                    starter.uses >= starter.def->maxUses || !EntriesDead(group, starter.def->afterDead))
                    continue;
                for (uint32 entry : starter.def->entries)
                {
                    Creature* object = leader->FindNearestCreature(entry, STARTER_LINK_SEARCH, true);
                    if (!object)
                        continue;
                    PathResult const direct =
                        RoutePath(leader, object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
                    if (direct.reachable && StarterPathArrives(direct, object))
                        continue;
                    if (LinkTo(group, leader, object) >= 0)
                    {
                        viaStarterLink = true;
                        break;
                    }
                }
                if (viaStarterLink)
                    break;
            }
        // [hleft-gd-pool] Gundrak: a leader on the bottom of the central pool (a navmesh part of its own) reaches no
        // boss until the "gundrak circle route" strategy swims him out: no boss_unreachable, no boss skip from there.
        bool const inGundrakPool = map->GetId() == GUNDRAK_MAP_ID && GundrakCircle::IsOnPoolFloor(leader);
        if (nextBoss && nextOrder >= 0 && (!nextRoute.reachable || nextDoor) && !viaLink && !inGundrakPool &&
            !eventBossWaits)
        {
            // Why (Hellfire Ramparts wave 4-6: Omor, a static spawn, "unreachable" from the ramp twice): the path
            // type, whether the long search / steep slopes / corner path were needed and where the route ends, for a
            // mmaps check (tools/navmesh_probe.py <map> <from> <boss> --both).
            group.LogEvent("boss_unreachable",
                     JsonObject().Str("boss", nextBoss->GetName()).Num("entry", nextBoss->GetEntry())
                         .Num("pathType", nextRoute.type).Bool("longRoute", nextRoute.longRoute)
                         .Bool("steep", nextRoute.steep).Bool("straightPath", nextRoute.straightPath)
                         .Num("points", int64(nextRoute.points.size())).Real("pathLength", nextRoute.length)
                         .Real("straight", leader->GetExactDist(nextBoss))
                         .Real("endLeft", nextBoss->GetExactDist(nextRoute.end.x, nextRoute.end.y, nextRoute.end.z))
                         .Real("endX", nextRoute.end.x).Real("endY", nextRoute.end.y).Real("endZ", nextRoute.end.z)
                         .Real("fromX", leader->GetPositionX()).Real("fromY", leader->GetPositionY())
                         .Real("fromZ", leader->GetPositionZ())
                         .Str("closedDoor", nextDoor ? nextDoor->GetName() : "")
                         .Num("closedDoorEntry", nextDoor ? nextDoor->GetEntry() : 0));
            if (nextDoor)
                group.SkipTarget(nextBoss->GetGUID(), CLOSED_DOOR_SKIP_MS,
                           Acore::StringFormat("next boss behind a closed door ({} {})", nextDoor->GetName(),
                                               nextDoor->GetEntry()));
            else
                group.SkipTarget(nextBoss->GetGUID(), UNREACHABLE_SKIP_MS, "next boss unreachable");
            auto const held = group.Route().orderHoldSinceMs.emplace(nextBoss->GetGUID(), now);
            if (getMSTimeDiff(held.first->second, now) < ORDER_HOLD_MS)
            {
                if (held.second)
                    group.LogEvent("boss_order_hold", JsonObject().Str("boss", nextBoss->GetName())
                                                           .Num("order", nextOrder)
                                                           .Num("holdSec", ORDER_HOLD_MS / IN_MILLISECONDS));
                if (gateOrder < 0 || nextOrder < gateOrder)
                    gateOrder = nextOrder;
            }
            nextBoss = nullptr;
            nextOrder = -1;
            for (auto const& [order, boss] : standing)
                if (!group.Route().skipUntil.count(boss->GetGUID()) && !(gateOrder >= 0 && order > gateOrder) &&
                    (!nextBoss || (order >= 0 && (nextOrder < 0 || order < nextOrder))))
                {
                    nextBoss = boss;
                    nextOrder = order;
                }
        }

        // [b46_strat] wing order (HopelessWingMap): the next boss's route held by a hopeless pack for
        // HOPELESS_WING_HOLD_MS -> deferred while another boss stands; the bosses of the other wing come next (the one
        // behind a closed key door then promotes its key starter as "next boss behind a closed door").
        if (nextBoss && nextBoss == nextRouteBoss && nextOrder >= 0 && !nextDoor && nextRoute.reachable && !viaLink &&
            HopelessWingMap(map->GetId()) && !leader->IsInCombat())
        {
            HopelessWingState& wing = hopelessWings[group.Run().runSeq];
            if (wing.checkedBoss != nextBoss->GetGUID() || !wing.checkedMs ||
                getMSTimeDiff(wing.checkedMs, now) >= HOPELESS_WING_CHECK_MS)
            {
                wing.checkedBoss = nextBoss->GetGUID();
                wing.checkedMs = now ? now : 1;
                wing.checkedHeld = RouteHeldByHopelessPack(leader, nextBoss, nextRoute.points, wing.heldBy);
            }
            if (!wing.checkedHeld)
                wing.heldSinceMs = 0;
            else if (!wing.heldSinceMs)
                wing.heldSinceMs = now ? now : 1;
            else if (getMSTimeDiff(wing.heldSinceMs, now) >= HOPELESS_WING_HOLD_MS)
            {
                bool otherStands = false;
                for (ObjectGuid const& guid : group.Route().bossCreatures)
                    if (Creature* other = map->GetCreature(guid); other && other != nextBoss && other->IsAlive() &&
                                                                    !wing.deferred.count(guid))
                        otherStands = true;
                if (otherStands)
                {
                    if (wing.deferred.empty())
                        wing.deferredSinceMs = now ? now : 1;
                    wing.deferred.insert(nextBoss->GetGUID());
                    LOG_INFO("playerbots", "[b46_strat] {}: {} deferred - its route runs into the hopeless {} for {} "
                                           "s with no way round: the other wing first",
                             group.Run().runId, nextBoss->GetName(), wing.heldBy,
                             getMSTimeDiff(wing.heldSinceMs, now) / IN_MILLISECONDS);
                    group.LogEvent("strat_wing_order",
                             JsonObject().Str("action", "deferred: the other wing first")
                                 .Str("boss", nextBoss->GetName()).Num("order", nextOrder)
                                 .Str("heldBy", wing.heldBy)
                                 .Num("heldSec", int64(getMSTimeDiff(wing.heldSinceMs, now) / IN_MILLISECONDS))
                                 .Num("deferred", int64(wing.deferred.size())));
                    if (group.Route().target == nextBoss->GetGUID())
                        group.Route().target.Clear();
                    // the next one in order is judged at once on the next retarget (heldSinceMs kept)
                    wing.checkedMs = 0;
                    nextBoss = nullptr;
                    nextOrder = -1;
                    for (auto const& [order, boss] : standing)
                        if (!group.Route().skipUntil.count(boss->GetGUID()) && !wing.deferred.count(boss->GetGUID()) &&
                            !(gateOrder >= 0 && order > gateOrder) &&
                            (!nextBoss || (order >= 0 && (nextOrder < 0 || order < nextOrder))))
                        {
                            nextBoss = boss;
                            nextOrder = order;
                        }
                }
            }
        }

        // After a trash loop: straight for the next boss of the route, the trash in between only as it comes.
        bool const bossRush = nextBoss && group.Route().bossRushUntilMs &&
            int32(group.Route().bossRushUntilMs - now) > 0;

        // Bosses alive by entry, for trash that cannot die while its boss lives.
        auto bossAlive = [&group, map](uint32 entry)
        {
            for (ObjectGuid const& guid : group.Route().bossCreatures)
                if (Creature* boss = map->GetCreature(guid))
                    if (boss->GetEntry() == entry && boss->IsAlive())
                        return true;
            return false;
        };
        // [sm-chapel-wing] the wing the group stands in: by entrance on a several-entrance content, by anchors on a
        // wing content (InThisWing).
        auto const inCurrentWing = [&group](WorldObject const* object)
        {
            if (group.Route().entrances.size() > 1 && group.Route().entranceIdx < group.Route().entrances.size())
            {
                int32 const wing = NearestEntrance(group, object);
                return wing < 0 || size_t(wing) == group.Route().entranceIdx ||
                       group.Route().entrances[size_t(wing)].GetExactDist(
                           &group.Route().entrances[group.Route().entranceIdx]) <
                           SAME_ENTRANCE_RANGE;
            }
            return InThisWing(group, object);
        };

        std::vector<Creature*> laterBosses;
        int32 const routeOrder = nextOrder >= 0 ? nextOrder : gateOrder;
        if (routeOrder >= 0)
            for (auto const& [order, boss] : standing)
                if (order > routeOrder)
                    laterBosses.push_back(boss);

        std::vector<std::pair<float, Creature*>> hostiles;
        std::set<ObjectGuid> deferred;
        std::vector<Creature*> candidates = RouteCreatures(map, leader);

        // Utgarde Keep (ROUTE_FORGE_ORDER): the Forge Masters whose previous forge is not engaged yet
        // (UKIds::IsForgeMasterLocked: utgarde_keep.cpp JustEngagedWith evades while GetData(prevDataId) is 0; the
        // instance keeps a forge's bit from IN_PROGRESS on, a Reset clears it). Wave of 2026-10-24 10:44: after forge 1
        // the route took forge 3's Metalworkers, the leader stopped at master 3's aggro edge, then 1 yd from him; the
        // tank's nearest pull was master 3 ("careful plan not ready ... at 0.0 yd", "[pull check] planned 1 -> came
        // 0"), he evaded every time and the run stalled with master 2 never reached. Players kill the forges in order
        // and keep away from a forge whose turn has not come.
        std::vector<Creature*> lockedForgeMasters;
        if (ROUTE_FORGE_ORDER)
            for (Creature* creature : candidates)
                if (creature && creature->IsInWorld() && creature->IsAlive() &&
                    (EncounterLockedFor(group, creature, script) || DoorKeyLocked(creature, script)))
                    lockedForgeMasters.push_back(creature);

        // Event chain (pass 3, ROUTE_CHAIN_GUARD): once a starter of this dungeon was used and further steps are
        // pending (a starter not used up whose order conditions hold, the event spot that applies), ordinary trash
        // is a target only within EVENT_CHAIN_TRASH_RADIUS of one of them - the next step is walked to, not the far
        // end of the instance. Culling wave 10: after Chromie the route chose Risen Zombies ~970 yd away in the
        // city (score 1184) and skipped them one by one for 4 minutes; the crates and Arthas were the next steps.
        // Bosses, attackers and escorts are never held back; no anchor in the world: no guard.
        std::vector<Position> chainAnchors;
        // [event_chain_narrow] the event spot's area (holdRadius, at least EVENT_CHAIN_JOIN_RANGE); -1: no spot
        Position chainSpot;
        float chainSpotRadius = -1.0f;
        // The chain's creatures (Arthas between his stages): what they summon (Culling waves, up to ~265 yd from his
        // stand) is the chain's way, never far trash (wave 8 cos-n: wave 3 260 yd off, the group stood 2 minutes).
        std::set<ObjectGuid> chainAnchorGuids;
        if (ROUTE_CHAIN_GUARD && !group.IsRaid() && !escortNpc)
        {
            bool chainStarted = false;
            for (StarterTrack const& starter : group.Route().starters)
                chainStarted |= starter.uses > 0;
            if (chainStarted)
            {
                for (StarterTrack const& starter : group.Route().starters)
                {
                    EventStarterDef const* def = starter.def;
                    if (starter.uses >= def->maxUses ||
                        (def->skipContent && strcmp(def->skipContent, group.ContentKey().c_str()) == 0) ||
                        !EntriesDead(group, def->afterDead) || !StartersUsed(group, def->afterUsed) ||
                        !ScriptBossDone(script, def->afterBossDone) ||
                        (def->kind == StarterKind::UseItem && !leader->HasItemCount(def->itemId, 1)))
                        continue;
                    if (CreatureStarterKind(def->kind))
                    {
                        // Waiting for its gossip flag still counts (Arthas between his stages).
                        for (Creature* creature : candidates)
                            if (creature && creature->IsInWorld() && creature->IsAlive() &&
                                std::find(def->entries.begin(), def->entries.end(), creature->GetEntry()) !=
                                    def->entries.end())
                            {
                                chainAnchors.push_back(creature->GetPosition());
                                chainAnchorGuids.insert(creature->GetGUID());
                            }
                    }
                    else
                        // Only an object the starter could still use (the target pick's own filter): one a strategy
                        // used already (Ahn'kahet's east device, wave 2026-10-25: the AK strategy clicked it, the
                        // anchor held the route 150 yd round it and Jedoga's initiates were never targets) is none.
                        // [b38_sm] Nor one in another wing (sameWingOnly, the target pick's [sm-chapel-wing] filter):
                        // sm-cath-n 20261026-050341-sm-cath-n-5B-19, once the Torch was used (938 s) the only anchor
                        // left was Herod's Door in the Armory (~1900 yd away), every idle Cathedral mob was dropped
                        // as "past EVENT_CHAIN_TRASH_RADIUS", Mograine was the only candidate (routeCandidates 1) and
                        // was body-pulled with 8 chapel mobs idle - his call brought them, 3 deaths at 1135-1155 s.
                        for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                            if (go && go->isSpawned() && !starter.usedObjects.count(go->GetGUID()) &&
                                std::find(def->entries.begin(), def->entries.end(), go->GetEntry()) !=
                                    def->entries.end() &&
                                (!def->sameWingOnly || inCurrentWing(go)) &&
                                (def->kind == StarterKind::UseItem ||
                                 (!go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) &&
                                  !go->HasGameObjectFlag(GO_FLAG_IN_USE) &&
                                  !(go->GetGoType() == GAMEOBJECT_TYPE_DOOR && go->GetGoState() != GO_STATE_READY))))
                                chainAnchors.push_back(go->GetPosition());
                }
                if (int32 const spotIdx = EventSpotIndex(group); spotIdx >= 0)
                {
                    EventSpotDef const* spot = EventSpots(group.MapId())[size_t(spotIdx)];
                    chainAnchors.emplace_back(spot->x, spot->y, spot->z);
                    chainSpot.Relocate(spot->x, spot->y, spot->z);  // [event_chain_narrow] the spot's own area
                    chainSpotRadius = std::max(spot->holdRadius, EVENT_CHAIN_JOIN_RANGE);
                }
            }
        }
        // An event hold's summons past SUMMON_SCAN_RANGE (Black Morass: the rifts ~117 yd from Medivh and their
        // Chrono Lord Deja / Temporus / Aeonus - bm-n build 19: Deja spawned at rift 6 and was never a target).
        if (holdSpot)
        {
            std::list<Creature*> summoned;
            SummonedCreatureCheck check;
            Acore::CreatureListSearcher<SummonedCreatureCheck> searcher(leader, summoned, check);
            Cell::VisitObjects(leader, searcher, CHAIN_SUMMON_SCAN_RANGE);
            for (Creature* creature : summoned)
                if (std::find(candidates.begin(), candidates.end(), creature) == candidates.end())
                    candidates.push_back(creature);
        }
        // The anchors' summons past the leader's SUMMON_SCAN_RANGE (Culling wave 3 at 2348,1202: 260 yd from Arthas).
        for (ObjectGuid const& guid : chainAnchorGuids)
            if (Creature* anchor = map->GetCreature(guid))
            {
                std::list<Creature*> summoned;
                SummonedCreatureCheck check;
                Acore::CreatureListSearcher<SummonedCreatureCheck> searcher(anchor, summoned, check);
                Cell::VisitObjects(anchor, searcher, CHAIN_SUMMON_SCAN_RANGE);
                for (Creature* creature : summoned)
                    if (TempSummon const* summon = creature->ToTempSummon())
                        if (summon->GetSummonerGUID() == guid &&
                            std::find(candidates.begin(), candidates.end(), creature) == candidates.end())
                            candidates.push_back(creature);
            }

        {
            std::vector<uint32> starterEntries;
            for (StarterTrack const& starter : group.Route().starters)
                if (CreatureStarterKind(starter.def->kind) && starter.uses < starter.def->maxUses)
                    for (uint32 entry : starter.def->entries)
                    {
                        bool known = false;
                        for (Creature* creature : candidates)
                            known |= creature && creature->GetEntry() == entry;
                        if (!known)
                            starterEntries.push_back(entry);
                    }
            if (!starterEntries.empty())
            {
                std::list<Creature*> found;
                SummonedEntryCheck check{starterEntries};
                Acore::CreatureListSearcher<SummonedEntryCheck> searcher(leader, found, check);
                Cell::VisitObjects(leader, searcher, STARTER_SUMMON_SCAN_RANGE);
                candidates.insert(candidates.end(), found.begin(), found.end());
            }
        }

        // Route-only trash (ROUTE_ONLY_TRASH): the backbone - the route to the next boss (nextRoute when it is his;
        // over an open link its two legs), to a walk-in event spot and to the nearest pending starter object - and
        // the entries an objective waits for (requiredEntries: link keys, a closed area's or a pending starter's
        // afterDead). Without a backbone everything stays a target (the full clear).
        bool const routeOnly = ROUTE_ONLY_TRASH && !group.IsRaid() && !holdSpot && !escortNpc;
        RouteBackbone backbone;
        PathResult starterKeyRoute;  // [zf-event-key-leg] the walk to a pending priority step's key creature
        guardKeys.insert(preClearKeys.begin(), preClearKeys.end());  // [hleft-boss-preclear]
        // [hleft-mech-bridge] the Mechanar bridge pack is Pathaleon's starter: required while he is dormant
        // [b30_mech_pos] ...and only once Sepethrea is dead: the pack stands upstairs past her (138,106,26.5); while
        // she lives the group is still below the lift and the pack, chosen from under the bridge (137.8,91.9,12.5,
        // dz 14), was steer_stuck -> "route end" skip -> mech_bridge_far lifted the skip -> again, every second
        // (mech-n/h 20261025-184229: 174/222 skips, 220/278 s idle, the lift link to Sepethrea never walked).
        bool mechBridgeOpen = leader->GetMapId() == MECHANAR_MAP_ID && MechanarBridge::PathaleonDormant(leader);
        if (mechBridgeOpen)
            for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
                if (creature && creature->IsAlive() && creature->GetEntry() == 19221)  // Nethermancer Sepethrea
                {
                    mechBridgeOpen = false;
                    break;
                }
        if (mechBridgeOpen)
        {
            std::vector<Creature*> bridgePack;
            MechanarBridge::CollectBridgePack(leader, bridgePack);
            for (Creature* mob : bridgePack)
                guardKeys.insert(mob->GetEntry());
        }
        // [hleft-kill-summon] the trash whose death summons an open boss (Gundrak heroic: Eck's Ruins Dwellers).
        for (KillSummonDef const* def : KillSummons(group.MapId(), group.Difficulty()))
            if (script && script->GetBossState(def->bossSlot) != DONE && ScriptBossDone(script, def->afterBossDone))
            {
                bool bossThere = false;
                for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
                    bossThere |= creature && creature->IsAlive() && creature->GetEntry() == def->bossEntry;
                // a summon has no spawn id: the boss creatures the run tracks
                for (ObjectGuid const& guid : group.Route().bossCreatures)
                    if (Creature* boss = map->GetCreature(guid))
                        bossThere |= boss->IsAlive() && boss->GetEntry() == def->bossEntry;
                if (!bossThere)
                    guardKeys.insert(def->entries.begin(), def->entries.end());
            }
        // [boss_dead_encounter_open] (PATCH_selin, "Delrissa: dead, kill credit not observed") a boss lies dead but
        // his encounter slot is not DONE (Magisters' Terrace: Priestess Delrissa is credited only when every lackey
        // died, boss_priestess_delrissa.cpp HelpersKilled; mgt-h 20261025-163739-mgt-h-5A-15 killed her at the
        // entrance after a wipe, the lackeys evaded home, 25 min without a target): his living summons are required
        // targets, wherever they stand. Generic: any boss with a summoned retinue. While he lives, BossAreaTrashOf
        // still leaves his retinue to his own fight.
        if (script)
            for (ObjectGuid const& guid : group.Route().bossCreatures)
                if (Creature* boss = map->GetCreature(guid); boss && !boss->IsAlive())
                    if (int32 const slot = BossSlotOf(group, boss);
                        slot >= 0 && script->GetBossState(uint32(slot)) != DONE)
                    {
                        std::list<Creature*> summons;
                        BossSummonCheck check{boss->GetGUID()};
                        Acore::CreatureListSearcher<BossSummonCheck> searcher(boss, summons, check);
                        Cell::VisitObjects(boss, searcher, BOSS_SUMMON_SEARCH);
                        for (Creature* add : summons)
                        {
                            guardKeys.insert(add->GetEntry());
                            // past the leader's summon scan range (evaded home): a candidate all the same
                            if (std::find(candidates.begin(), candidates.end(), add) == candidates.end())
                                candidates.push_back(add);
                        }
                    }
        std::set<uint32> requiredEntries(linkKeys.begin(), linkKeys.end());
        requiredEntries.insert(guardKeys.begin(), guardKeys.end());
        // [gate-key-after-boss] which required entries come from a closed AreaGate only / from a pending starter
        std::set<uint32> gateKeys;
        std::set<uint32> starterKeys;
        // The kind of the route to the next boss (routeType in the target log) and whether a leg was left out as
        // incomplete: then the route rules stay on without "blocks route" from that leg (boss room, required,
        // patrol, joins boss), not the full clear.
        std::string routeType = "none";
        bool backboneDropped = false;
        RouteBackbone droppedLegs;  // the incomplete legs left out (route visualisation)
        if (routeOnly)
        {
            // A partial route ends at the explored polygon nearest the goal as the crow flies - in a maze a dead end
            // below or behind it (RFC 18:21: a cave 30 yd under Taragaman's ledge looked like that). Its trash blocks
            // no way the group walks: a leg counts only when it reaches its goal's polygon or ends within
            // ROUTE_BACKBONE_END_LEFT of the goal. The partial routes RoutePath still calls reachable (a navmesh
            // in parts, "ends nearer") keep their walk; only the trash choice ignores them.
            auto addLeg = [&](PathResult const& route, G3D::Vector3 const& dest, char const* what, bool bossLeg)
            {
                float const left = (dest - route.end).length();
                bool const complete = route.complete || (route.points.size() >= 2 && left < ROUTE_BACKBONE_END_LEFT);
                if (bossLeg)
                {
                    if (route.points.empty())
                        routeType = "none";
                    else if (!complete)
                        routeType = "incomplete " + std::to_string(route.type);
                    else
                        routeType = route.longRoute ? "long" : "normal";
                    if (complete && route.steep)
                        routeType += "+steep";
                }
                if (ROUTE_BACKBONE_COMPLETE_ONLY && !complete)
                {
                    backboneDropped = true;
                    if (route.points.size() >= 2)
                        droppedLegs.push_back(route.points);  // route visualisation only
                    uint64 const key = leader->GetGUID().GetRawValue() ^ (uint64(what[0]) << 56);
                    uint32& loggedMs = backboneIncompleteLogMs[key];
                    if (!loggedMs || getMSTimeDiff(loggedMs, now) > ROUTE_BACKBONE_LOG_MS)
                    {
                        loggedMs = now;
                        LOG_INFO("playerbots",
                                 "[route] {}: route backbone incomplete ({}): pathType {} longRoute {} steep {} "
                                 "points {} end ({:.1f},{:.1f},{:.1f}) {:.1f} yd from ({:.1f},{:.1f},{:.1f}) - no "
                                 "\"blocks route\" from it",
                                 leader->GetName(), what, route.type, route.longRoute, route.steep,
                                 route.points.size(), route.end.x, route.end.y, route.end.z, left, dest.x, dest.y,
                                 dest.z);
                        group.LogEvent("route_backbone",
                                 JsonObject().Str("state", "incomplete").Str("leg", what).Num("pathType", route.type)
                                     .Bool("longRoute", route.longRoute).Bool("steep", route.steep)
                                     .Num("points", int64(route.points.size())).Real("endLeft", left)
                                     .Real("endX", route.end.x).Real("endY", route.end.y).Real("endZ", route.end.z)
                                     .Real("toX", dest.x).Real("toY", dest.y).Real("toZ", dest.z)
                                     .Real("fromX", leader->GetPositionX()).Real("fromY", leader->GetPositionY())
                                     .Real("fromZ", leader->GetPositionZ()));
                    }
                    return;
                }
                if (route.points.size() >= 2)
                    backbone.push_back(route.points);
            };
            if (nextBoss)
            {
                std::vector<RouteLinkDef const*> const links = RouteLinks(group.MapId());
                if (viaLink && group.Route().linkIdx >= 0 && size_t(group.Route().linkIdx) < links.size())
                {
                    RouteLinkDef const& link = *links[size_t(group.Route().linkIdx)];
                    G3D::Vector3 const farEnd(link.toX, link.toY, link.toZ);
                    addLeg(RoutePath(leader, link.fromX, link.fromY, link.fromZ),
                           G3D::Vector3(link.fromX, link.fromY, link.fromZ), "link", false);
                    addLeg(RoutePath(leader, nextBoss->GetPositionX(), nextBoss->GetPositionY(),
                                     nextBoss->GetPositionZ(), &farEnd),
                           G3D::Vector3(nextBoss->GetPositionX(), nextBoss->GetPositionY(), nextBoss->GetPositionZ()),
                           "boss over link", true);
                    routeType = "link " + routeType;
                }
                else
                    addLeg(nextBoss == nextRouteBoss ? nextRoute : EvaluatePath(leader, nextBoss),
                           G3D::Vector3(nextBoss->GetPositionX(), nextBoss->GetPositionY(), nextBoss->GetPositionZ()),
                           "boss", true);
            }
            if (leadSpot)
                addLeg(RoutePath(leader, leadSpot->x, leadSpot->y, leadSpot->z),
                       G3D::Vector3(leadSpot->x, leadSpot->y, leadSpot->z), "event spot", false);

            WorldObject* starterObject = nullptr;
            Creature* starterKey = nullptr;  // [zf-event-key-leg]
            for (StarterTrack const& starter : group.Route().starters)
            {
                EventStarterDef const* def = starter.def;
                if (starter.uses >= def->maxUses ||
                    (def->skipContent && strcmp(def->skipContent, group.ContentKey().c_str()) == 0))
                    continue;
                for (uint32 entry : def->afterDead)
                    if (!EntriesDead(group, std::vector<uint32>{entry}))
                    {
                        requiredEntries.insert(entry);
                        starterKeys.insert(entry);
                    }
                // [zf-event-key-leg] the nearest living key creature of a pending priority step (Zul'Farrak: the
                // Sandfury Executioner 7274 for the cage) - a leg when no boss leads the route.
                if (!nextBoss && (def->priority || (def->priorityItem && leader->HasItemCount(def->priorityItem, 1))) &&
                    !EntriesDead(group, def->afterDead))
                    for (Creature* creature : candidates)
                        if (creature && creature->IsInWorld() && creature->IsAlive() &&
                            !group.Route().skipUntil.count(creature->GetGUID()) &&
                            std::find(def->afterDead.begin(), def->afterDead.end(), creature->GetEntry()) !=
                                def->afterDead.end() &&
                            (!starterKey || leader->GetExactDist(creature) < leader->GetExactDist(starterKey)))
                            starterKey = creature;
                if (!EntriesDead(group, def->afterDead) || !StartersUsed(group, def->afterUsed) ||
                        !ScriptBossDone(script, def->afterBossDone) ||
                    (def->kind == StarterKind::UseItem && !leader->HasItemCount(def->itemId, 1)))
                    continue;
                if (CreatureStarterKind(def->kind))
                {
                    for (Creature* creature : candidates)
                        if (creature && creature->IsInWorld() && creature->IsAlive() &&
                            std::find(def->entries.begin(), def->entries.end(), creature->GetEntry()) !=
                                def->entries.end() &&
                            (!starterObject || leader->GetExactDist(creature) < leader->GetExactDist(starterObject)))
                            starterObject = creature;
                }
                else
                    for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                        if (go && go->isSpawned() && !starter.usedObjects.count(go->GetGUID()) &&
                            std::find(def->entries.begin(), def->entries.end(), go->GetEntry()) != def->entries.end() &&
                            (!def->sameWingOnly || inCurrentWing(go)) &&
                            (!starterObject || leader->GetExactDist(go) < leader->GetExactDist(starterObject)))
                            starterObject = go;
            }
            if (starterObject)
                addLeg(EvaluatePath(leader, starterObject),
                       G3D::Vector3(starterObject->GetPositionX(), starterObject->GetPositionY(),
                                    starterObject->GetPositionZ()),
                       "starter", false);
            else if (starterKey)
            {
                starterKeyRoute = EvaluatePath(leader, starterKey);
                addLeg(starterKeyRoute,
                       G3D::Vector3(starterKey->GetPositionX(), starterKey->GetPositionY(), starterKey->GetPositionZ()),
                       "key of starter", false);
                if (starterKeyLegLogged.insert({group.Run().runSeq, starterKey->GetGUID()}).second)
                    group.LogEvent("route_starter_key_leg",
                             JsonObject().Str("name", starterKey->GetName()).Num("entry", starterKey->GetEntry())
                                 .Real("distance", leader->GetExactDist(starterKey)));
            }

            for (AreaGateDef const* gate : closedAreas)
                for (uint32 entry : gate->afterDead)
                    if (!EntriesDead(group, std::vector<uint32>{entry}))
                    {
                        requiredEntries.insert(entry);
                        gateKeys.insert(entry);
                    }
        }
        // [gate-key-after-boss] An entry required only by a closed area (no link, guard or starter needs it) waits
        // while the next boss in order stands attackable outside every closed area: st-n 012157 chose Hukku 8 s
        // after Atal'alarion (order 0) and stalled. Guards of a door the next boss is behind are guardKeys, not this.
        bool const nextBossStands = nextBoss && nextOrder >= 0 && leader->InSamePhase(nextBoss) &&
                                    leader->IsValidAttackTarget(nextBoss) &&
                                    !InAnyAreaGate(closedAreas, nextBoss->GetPositionX(), nextBoss->GetPositionY(),
                                                   nextBoss->GetPositionZ());
        auto const gateKeyHeld = [&](Creature const* creature)
        {
            uint32 const entry = creature->GetEntry();
            return nextBossStands && creature != nextBoss && gateKeys.count(entry) && !linkKeys.count(entry) &&
                   !guardKeys.count(entry) && !starterKeys.count(entry);
        };
        // The route rules apply with a backbone, or with an incomplete leg left out of it (no "blocks route" then).
        bool const routeRules = routeOnly && (!backbone.empty() || backboneDropped);
        {
            // What "playerbots route show" draws: this retarget's backbone (copied, read only on the world thread).
            RouteSnapshot& snapshot = routeSnapshots[group.Run().runSeq];
            snapshot.legs = backbone;
            snapshot.dropped = std::move(droppedLegs);
            snapshot.routeType = routeType;
            snapshot.routeOnly = routeOnly;
            snapshot.nextBoss = nextBoss ? nextBoss->GetGUID() : ObjectGuid::Empty;
            snapshot.nextBossName = nextBoss ? nextBoss->GetName() : std::string();
            snapshot.builtMs = now;
        }

        // [fled_survivor] A mob that fled from an earlier fight (FledMobs: the bots saw it run) and stands idle and
        // damaged on the tank's trail or within FLED_SURVIVOR_BEHIND of the leader: required, evaluated first and
        // scored first - the trail is not cleared while it stands there (user 2026-10-25: it meets the group on the
        // way back after a wipe, or brings friends). strat-h 5A-11: a Crimson Initiate fled from the Hall of Lights.
        std::set<ObjectGuid> fledSurvivors;
        if (!FledMobs::Noted(leader->GetMapId(), leader->GetInstanceId()).empty())
        {
            std::vector<G3D::Vector3> const trail = MovementAction::TankTrailPoints(leader);
            for (Creature* creature : candidates)
            {
                if (!creature || !FledMobs::IsSurvivor(creature) || !leader->IsValidAttackTarget(creature))
                    continue;

                bool nearTrail = leader->GetExactDist(creature) <= FLED_SURVIVOR_BEHIND;
                for (G3D::Vector3 const& point : trail)
                {
                    if (nearTrail)
                        break;
                    nearTrail = creature->GetExactDist(point.x, point.y, point.z) <= FLED_SURVIVOR_TRAIL;
                }
                if (!nearTrail)
                    continue;

                fledSurvivors.insert(creature->GetGUID());
                if (fledSurvivorLogged.insert({group.Run().runSeq, creature->GetGUID()}).second)
                {
                    LOG_INFO("playerbots", "[fled_survivor] {}: {} {} fled from a fight and stands idle at {:.0f} % "
                             "health {:.1f} yd away, on the group's trail: required before the group walks on",
                             leader->GetName(), creature->GetName(), creature->GetEntry(), creature->GetHealthPct(),
                             leader->GetExactDist(creature));
                    group.LogEvent("fled_survivor",
                             JsonObject().Str("mob", creature->GetName()).Num("entry", creature->GetEntry())
                                 .Real("hpPct", creature->GetHealthPct()).Real("dist", leader->GetExactDist(creature))
                                 .Real("x", creature->GetPositionX()).Real("y", creature->GetPositionY())
                                 .Real("z", creature->GetPositionZ()));
                }
            }
        }

        // Rules (1)-(3) of ROUTE_ONLY_TRASH for an idle non-boss mob (the fighting ones never come here): the rule
        // that keeps it a target, nullptr: off the route. routeWhy keeps it with BlocksRoute's findings for the
        // target log (routeRule / routeDist / reach / routeLos: why each target was chosen).
        struct RouteWhy
        {
            char const* rule = nullptr;
            float reach = 0.0f;
            RouteCheck check;
        };
        std::unordered_map<ObjectGuid, RouteWhy> routeWhy;
        auto const routeNeeds = [&](Creature* creature) -> char const*
        {
            RouteWhy& why = routeWhy[creature->GetGUID()];
            why.reach = RouteReach(creature, leader);
            bool const blocks = BlocksRoute(leader, creature, why.reach, backbone, why.check);
            why.rule = [&]() -> char const*
            {
                if (fledSurvivors.count(creature->GetGUID()))
                    return "fled survivor";
                // It joins the boss fight (JoinsBossFight: assist / aggro / formation / script).
                if (char const* joins = JoinsBossFight(leader, nextBoss, creature))
                    return joins;
                // [b33_mech2] The trash of the next boss's area (BossAreaTrashOf: his room, BOSS_AREA_EXTRA) is on
                // the route: off it, boss_area_clear never saw it (it counts the route's targets only) and the boss
                // was body-pulled with it standing. mech-n 20261026-000434-mech-n-5D-17: the listed Tempest-Forge
                // Destroyer 138862 (293.9,-14.9) was "off route" all along, Sepethrea's body pull at 1180 s, her
                // Arcane Blast threw the tank into it, it killed the tank and the DK.
                if (ROUTE_BOSS_AREA_CLEAR)
                    if (char const* area = BossAreaTrashOf(leader, nextBoss, creature,
                                                           group.Route().bossCreatures.count(creature->GetGUID()) != 0))
                        return area;
                // [b32_smpull] The boss's linked trash (Scarlet Monastery: Commander Mograine's engage sends every
                // idle mob of his chapel at his victim): a target while he stands idle with it, cleared before him -
                // off the route or not (sm-h 20261025-221216-sm-h-5E-14: the chapel's south four were "off route",
                // Mograine "blocks route" was pulled into them and never fought).
                if (LinkedChapelTrash(leader, creature))
                    return "linked trash (Mograine's call)";
                // An objective waits for it (a gate, a link, a starter): the run cannot finish without it - a gate's
                // key not before the standing next boss (gateKeyHeld): then only the ordinary rules below.
                if (requiredEntries.count(creature->GetEntry()) && !gateKeyHeld(creature))
                    return "required";
                if (blocks)
                    return "blocks route";
                // In water (swimming or under water at its spot): only what blocks the way, never "nice to have".
                if (creature->IsInWater())
                    return nullptr;
                // The event chain's trash (EVENT_CHAIN_TRASH_RADIUS, filtered above) is the chain's way -
                // [event_chain_narrow] only its own: summoned, at an anchor within reach and sight, in the spot's area.
                if (!chainAnchors.empty())
                {
                    if (!ROUTE_CHAIN_NARROW || (creature->IsSummon() && !creature->GetSpawnId()) ||
                        (chainSpotRadius >= 0.0f && creature->GetExactDist(&chainSpot) <= chainSpotRadius))
                        return "event chain";
                    for (Position const& anchor : chainAnchors)
                        if (creature->GetExactDist(&anchor) <= EVENT_CHAIN_JOIN_RANGE &&
                            SeesFromPoint(leader, creature,
                                          G3D::Vector3(anchor.GetPositionX(), anchor.GetPositionY(),
                                                       anchor.GetPositionZ())))
                            return "event chain";
                }
                // [lead1215] b240 rfc-h 5G-15 388 s: Ragefire Shaman 38 "patrol crosses route" (its walk anywhere
                // near the route) - the group detoured south for it and woke Shaman 37's pack (+4 mobs, ~28 s); the
                // fast run judged it off the path where it stood then. Only its forecast place at our arrival counts
                WaypointPath const* patrol = PatrolWalkPath(creature);
                return patrol && PatrolOnRouteAtArrival(leader, creature, *patrol, AggroReach(creature, leader),
                                                        backbone, nextBoss)
                           ? "patrol crosses route"
                           : nullptr;
            }();
            return why.rule;
        };
        // The off-route mobs, kept for the case nothing is left on the route (the boss not targetable yet: an event
        // the harness does not model may need the rest) - then the old full clear.
        std::vector<std::pair<float, Creature*>> offRouteHostiles;
        group.Route().pathVerdicts.clear();  // [lead41] this retarget's verdicts
        // [sv-bog-overlord-alone] The idle mobs pulled alone and first (PULL_ALONE_FIRST): what stands within their
        // reach waits until they are dead.
        std::vector<Creature*> pullAloneMobs;
        for (Creature* creature : candidates)
            if (creature && creature->IsInWorld() && creature->IsAlive() && !creature->IsInCombat() &&
                std::find(PULL_ALONE_FIRST.begin(), PULL_ALONE_FIRST.end(), creature->GetEntry()) !=
                    PULL_ALONE_FIRST.end() &&
                !group.Route().skipUntil.count(creature->GetGUID()) && leader->IsValidAttackTarget(creature))
                pullAloneMobs.push_back(creature);

        // [brd_seven_fast] the hold area's not-attackable -> attackable flips (eventWaveFlipMs): every retarget, and
        // with no target Retarget runs every tick, so a flipped dwarf is the target on the next harness tick.
        if (holdSpot)
        {
            auto& flips = eventWaveFlipMs[group.InstanceId()];
            float const areaRadius = holdSpot->holdRadius > 0.0f ? holdSpot->holdRadius : HOLD_TARGET_RADIUS;
            for (Creature* creature : candidates)
            {
                if (!creature || !creature->IsInWorld() || !creature->IsAlive() || creature->IsCritter() ||
                    creature->IsTotem())
                    continue;
                bool const inArea =
                    std::find(holdSpot->holdAlways.begin(), holdSpot->holdAlways.end(), creature->GetEntry()) !=
                        holdSpot->holdAlways.end() ||
                    creature->GetExactDist(holdSpot->x, holdSpot->y, holdSpot->z) <= areaRadius;
                if (!inArea)
                    continue;
                bool const attackable = !creature->IsImmuneToPC() &&
                                        !creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) &&
                                        !creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) &&
                                        creature->IsHostileTo(leader) && leader->IsValidAttackTarget(creature);
                auto const seen = flips.find(creature->GetGUID());
                if (!attackable)
                {
                    flips[creature->GetGUID()] = 0;  // (again) not attackable: an event reset re-arms the flip
                    continue;
                }
                if (seen == flips.end() || seen->second != 0)
                    continue;  // attackable since first seen (ordinary trash), or the flip is already noted
                seen->second = now ? now : 1;
                group.Route().skipUntil.erase(creature->GetGUID());
                group.Route().skipReason.erase(creature->GetGUID());
                group.LogEvent("event_wave_attackable",
                         JsonObject().Str("name", creature->GetName()).Num("entry", creature->GetEntry())
                             .Str("spot", holdSpot->note).Bool("inCombat", creature->IsInCombat())
                             .Real("distance", leader->GetExactDist(creature))
                             .Real("x", creature->GetPositionX()).Real("y", creature->GetPositionY())
                             .Real("z", creature->GetPositionZ()));
            }
        }

        uint32 lockoutPathChecks = 0;  // [b44_ring] the lockout clear's path checks in this retarget
        for (Creature* creature : candidates)
        {
            if (!creature || !creature->IsInWorld() || !creature->IsAlive() || creature->IsInEvadeMode())
                continue;
            if (group.Route().skipUntil.count(creature->GetGUID()))
                continue;
            bool const encounterCreature = creature->IsDungeonBoss() || creature->isWorldBoss() ||
                                           group.Route().bossCreatures.count(creature->GetGUID());
            TempSummon const* chainSummon = creature->IsSummon() ? creature->ToTempSummon() : nullptr;
            bool const anchorSummon = chainSummon && chainAnchorGuids.count(chainSummon->GetSummonerGUID());
            // [b44_ring] A creature the running hold event summoned (its summoner is one of the spot's untilDead: High
            // Justice Grimstone 10096 summons every Ring of Law wave and boss) is the event itself, idle or not: the
            // event FAILs when none of them has a victim for 30 s (npc_grimstone updateReset). brd-n
            // 20261026-075135-brd-n-5B-6: Rlrogaf Vanished from a Borer Beetle of wave 2 (1916 s), it evaded to its
            // summon point (608.96,-235.32, 48.6 yd from the spot), fightOnlyAttackers left it standing, 11 of 12
            // beetles died, no victim from 1933 s, FAIL at 1963 s and the 2-minute lockout. Players walk over and kill
            // it. Not fightOnlyAttackers / holdRadius / endless summons for it.
            bool const eventSummon =
                eventHold && !group.Route().eventSpotRetry && chainSummon &&
                std::find(holdSpot->untilDead.begin(), holdSpot->untilDead.end(),
                          chainSummon->GetSummonerGUID().GetEntry()) != holdSpot->untilDead.end();
            // [b44_ring] The retry lockout of a failed hold event (at_ring_of_law refuses 2 min after the FAIL,
            // Party::ringRetryAtSec): the group clears the idle trash near the spot instead of standing (user rule: no
            // fight for 2 min is a blocker). Only with BRD_LOCKOUT_CLEAR_MIN_LEFT_SEC of the lockout left, only
            // within HOLD_TARGET_RADIUS of the spot and a complete walk of at most BRD_LOCKOUT_CLEAR_PATH (checked
            // below); the closed areas (the Ring of Law stands) stay closed and the trigger still waits for the
            // lockout - nothing about the event changes.
            constexpr int64 BRD_LOCKOUT_CLEAR_MIN_LEFT_SEC = 40;
            bool const lockoutClear =
                eventHold && group.Route().eventSpotRetry && !encounterCreature && !creature->IsInCombat() &&
                int64(GameTime::GetGameTime().count()) + BRD_LOCKOUT_CLEAR_MIN_LEFT_SEC < group.Route().ringRetryAtSec;
            // [b38_sm] Mograine's linked chapel trash (LinkedChapelTrash) is the chain's way too: cleared before him.
            if (!chainAnchors.empty() && !encounterCreature && !anchorSummon && !creature->IsInCombat() &&
                NearestDist(chainAnchors, creature) > EVENT_CHAIN_TRASH_RADIUS && !LinkedChapelTrash(leader, creature))
                continue;
            // [cos-event-trash] (PATCH_queue_encounters §2) Culling heroic, Guardian of Time running: idle mobs
            // outside Arthas' event are no target (cos-h 121320: waves 20 min, the Corruptor FAILED before Epoch died).
            if (leader->GetMapId() == CoSIds::MAP_CULLING_OF_STRATHOLME && !encounterCreature &&
                CoSHelpers::IsSkippableEventTrash(leader, creature))
                continue;
            // [b35_up] Skadi's gauntlet runs (SkadiGauntletRunning): idle mobs outside his hall are no target - a walk
            // out of the hall resets the event (spell_skadi_reset_check).
            if (!encounterCreature && !creature->IsInCombat() && !InSkadiHall(creature) &&
                SkadiGauntletRunning(leader))
                continue;
            // A hold spot that fights only what attacks (EventSpotDef::fightOnlyAttackers, Azjol-Nerub's hole rim):
            // idle non-boss mobs are no target, the leader goes to the spot and the group gathers there.
            // [brd_seven_fast] not a scripted wave that just became attackable (eventWaveFlipMs): it may never attack.
            if (ROUTE_HOLD_SPOT_FIGHT && holdSpot && holdSpot->fightOnlyAttackers &&
                (!encounterCreature || eventHold) && !creature->IsInCombat() &&
                !EventWaveFresh(group.InstanceId(), creature->GetGUID()) &&
                !eventSummon && !lockoutClear)  // [b44_ring]
                continue;
            // A scripted intro runs (introBoss): nothing idle is pulled while the group waits for its boss.
            if (introBoss && !creature->IsInCombat())
                continue;
            // The Black Morass event hold (Medivh, holdRadius 100000): idle wildlife (a DB
            // spawn) is no target, and an idle event mob only within HOLD_EVENT_MOB_RANGE of the spot or of a live
            // time rift (bm-n build 19: Sable Jaguar / Darkwater Crocolisk at 138-154 yd while the shield fell).
            // holdAlways entries (keepers, rift bosses) always; what attacks is fought anyway.
            if (holdSpot && group.MapId() == BLACK_MORASS_MAP && !creature->IsInCombat() &&
                !encounterCreature &&
                std::find(holdSpot->holdAlways.begin(), holdSpot->holdAlways.end(), creature->GetEntry()) ==
                    holdSpot->holdAlways.end())
            {
                if (creature->GetSpawnId())
                    continue;
                bool closeToEvent =
                    creature->GetExactDist(holdSpot->x, holdSpot->y, holdSpot->z) <= HOLD_EVENT_MOB_RANGE;
                for (Creature* rift : candidates)
                    closeToEvent |= !closeToEvent && rift && rift->IsAlive() &&
                                    rift->GetEntry() == BLACK_MORASS_TIME_RIFT &&
                                    rift->GetExactDist(creature) <= HOLD_EVENT_MOB_RANGE;
                if (!closeToEvent)
                    continue;
            }
            // [b32_sh_ooze] ROUTE_RESPAWN_IGNORE: once per instance (its first target scan) the map's fast-respawn
            // spawns (FastSpawns, DB spawntimesecs) are respawn_ignored and their spawn points the respawn zone; a
            // patrol of them seen anywhere widens the zone.
            if (ROUTE_RESPAWN_IGNORE && !respawnZones.count(group.InstanceId()))
            {
                respawnZones[group.InstanceId()];
                std::map<uint32, std::pair<uint32, FastSpawn>> entries;  // entry -> spawn count, first spawn
                for (FastSpawn const& spawn : FastSpawns(group.MapId()))
                {
                    AddRespawnZonePoint(group.InstanceId(), spawn.pos);
                    auto& known = entries[spawn.entry];
                    if (!known.first++ || spawn.respawnSec < known.second.respawnSec)
                        known.second = spawn;
                }
                for (auto const& [entry, known] : entries)
                {
                    if (!RespawnIgnored::Mark(group.InstanceId(), entry))
                        continue;
                    CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
                    std::string const name = info ? info->Name : std::string("?");
                    std::string const why = "respawn " + std::to_string(known.second.respawnSec) + "s";
                    LOG_INFO("playerbots",
                             "[respawn_ignored] {}: {} {} ({} spawns), {}: no route target / walk stop / tank pick "
                             "while idle, fought when it attacks",
                             group.Run().runId, name, entry, known.first, why);
                    group.LogEvent("respawn_ignored",
                             JsonObject().Str("name", name).Num("entry", entry).Num("spawns", known.first)
                                 .Num("respawnSec", known.second.respawnSec).Real("x", known.second.pos.x)
                                 .Real("y", known.second.pos.y).Real("z", known.second.pos.z).Str("why", why));
                }
            }
            if (ROUTE_RESPAWN_IGNORE && RespawnIgnored::IsIgnored(creature))
                AddRespawnZonePoint(group.InstanceId(), G3D::Vector3(creature->GetPositionX(), creature->GetPositionY(),
                                                                   creature->GetPositionZ()));
            // Trash that respawns within seconds (ROUTE_FAST_RESPAWN_PASS): fought only when it attacks.
            if (!encounterCreature && !creature->IsInCombat() && FastRespawner(creature) &&
                !requiredEntries.count(creature->GetEntry()))
                continue;
            // A walk-in event spot first (leadSpot): idle trash only on the way there or right next to the leader.
            if (leadSpot && !encounterCreature && !creature->IsInCombat() &&
                creature->GetExactDist(leadSpot->x, leadSpot->y, leadSpot->z) >
                    leadSpotDistance + LEAD_SPOT_AHEAD_MARGIN &&
                leader->GetExactDist(creature) > LEAD_SPOT_NEAR_RANGE)
                continue;
            // The forge of a locked Forge Master waits with him (his own lock is checked below).
            if (!lockedForgeMasters.empty() && !encounterCreature && !creature->IsInCombat())
            {
                bool lockedForge = false;
                for (Creature* master : lockedForgeMasters)
                    lockedForge |= creature->GetExactDist(master) <= FORGE_STATION_RADIUS;
                if (lockedForge)
                    continue;
            }
            if (viaLink && creature == nextBoss)
                continue;  // reached over the link, not walked to from here
            // [b30_st_spirit] A hold spot's objective or guard is a target even when civilian: the Nightmare
            // Suppressor 8497 is flags_extra 2 (CIVILIAN, it never aggroes) and is what fails the Shade of Hakkar
            // event (st-h 20261025-184229-st-h-5B-12: egg at 3675 s, one flame lit at 3739 s, not one Suppressor
            // ever a target - this filter dropped them although [event_boss_classic] lists them - the Shade
            // despawned, no more Bloodkeepers, idle until the stall at 3988 s).
            bool const holdSpotEntry =
                holdSpot &&
                (std::find(holdSpot->holdAlways.begin(), holdSpot->holdAlways.end(), creature->GetEntry()) !=
                     holdSpot->holdAlways.end() ||
                 std::find(holdSpot->guardEntries.begin(), holdSpot->guardEntries.end(), creature->GetEntry()) !=
                     holdSpot->guardEntries.end());
            // [b32_dmw] A boss of the run is a target even when civilian: Prince Tortheldrin 11486 is flags_extra 2
            // (CIVILIAN, faction 1355 + gossip until Immol'thar's death sets West Wing data 1, then SmartAI faction
            // 14) - this filter dropped him from every candidate list, the route found "nothing on the route" and
            // full-cleared the far trash (Rotting Highborne 112 yd, Arcane Feedback 393 yd) until no_targets and
            // the stall (dmw-n 20261025-221216-5E-12: 4/5 at 2952 s, Tortheldrin "alive, not engaged" at 4775 s).
            // The hostility / attackability checks below still decide whether he can be attacked yet.
            bool const bossCreature = group.Route().bossCreatures.count(creature->GetGUID()) != 0;
            if (creature->IsCritter() || creature->IsTotem() ||
                creature->GetCreatureTemplate()->type == CREATURE_TYPE_TOTEM ||
                (creature->IsCivilian() && !holdSpotEntry && !bossCreature))
                continue;
            if (creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) || creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
                creature->IsImmuneToPC())
                continue;
            // A neutral boss that does not aggro is still a boss a group attacks (Stratholme: Hearthsinger
            // Forresten faction 7, Scholomance: Vectus / Marduk faction 15, Dire Maul: Prince Tortheldrin after
            // Immol'thar).
            // [event_boss_classic] not a vendor / quest giver out of combat (BRD Plugger Spazzring: ale-theft event)
            bool const neutralBoss =
                !creature->IsFriendlyTo(leader) &&
                (group.Route().bossCreatures.count(creature->GetGUID()) || creature->IsDungeonBoss()) &&
                !(creature->HasNpcFlag(NPCFlags(UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_QUESTGIVER)) &&
                  !creature->IsInCombat());
            if (!leader->InSamePhase(creature) || (!creature->IsHostileTo(leader) && !neutralBoss) ||
                !leader->IsValidAttackTarget(creature))
                continue;

            bool inBlockedZone = false;
            for (Position const& pos : blocked)
                if (creature->GetExactDist(&pos) < FAILED_BOSS_RADIUS)
                    inBlockedZone = true;
            if (inBlockedZone)
                continue;

            if (!InThisWing(group, creature))
                continue;

            // Bosses always (EventSpotDef::holdRadius), also a summoned one the party has not fought yet: it is no
            // party boss creature before that (Black Morass: the rift-6 Chrono Lord Deja, IsDungeonBoss through
            // instance_encounters).
            // [brd_ring_hold] an event step that holds (eventData) and EventSpotDef::bossOnlyNear (Maraudon's Rotgrip:
            // a swimmer on a 230 yd patrol): idle bosses too only within holdRadius - the group waits at the spot
            // instead of walking out to them.
            // [b44_ring] an event summon is never left outside (eventSummon); the lockout clear (lockoutClear) looks
            // as far as HOLD_TARGET_RADIUS.
            float const holdTargetRadius = holdSpot ? (holdSpot->holdRadius > 0.0f ? holdSpot->holdRadius
                                                                                    : HOLD_TARGET_RADIUS)
                                                    : 0.0f;
            if (holdSpot && !eventSummon &&
                (!encounterCreature ||
                 ((eventHold || holdSpot->bossOnlyNear) && !creature->IsInCombat())) &&
                std::find(holdSpot->holdAlways.begin(), holdSpot->holdAlways.end(), creature->GetEntry()) ==
                    holdSpot->holdAlways.end() &&
                creature->GetExactDist(holdSpot->x, holdSpot->y, holdSpot->z) >
                    (lockoutClear ? std::max(holdTargetRadius, HOLD_TARGET_RADIUS) : holdTargetRadius))
                continue;

            bool gated = false;
            for (AreaGateDef const* gate : closedAreas)
                gated |= creature->GetPositionX() >= gate->minX && creature->GetPositionX() <= gate->maxX &&
                         creature->GetPositionY() >= gate->minY && creature->GetPositionY() <= gate->maxY &&
                         creature->GetPositionZ() >= gate->minZ && creature->GetPositionZ() <= gate->maxZ;
            if (gated)
                continue;

            // [b44_ring] the lockout clear: an idle mob outside the event's own area only over a complete walk of at
            // most BRD_LOCKOUT_CLEAR_PATH from the leader (the group is back before the retry), at most
            // BRD_LOCKOUT_CLEAR_CHECKS path checks per retarget (the nearest first is not guaranteed: candidates are
            // in store order, a skipped one is simply no target this time).
            if (lockoutClear && creature->GetGUID() != group.Route().target &&
                creature->GetExactDist(holdSpot->x, holdSpot->y, holdSpot->z) > holdTargetRadius)
            {
                constexpr float BRD_LOCKOUT_CLEAR_PATH = 110.0f;
                constexpr uint32 BRD_LOCKOUT_CLEAR_CHECKS = 8;
                if (leader->GetExactDist(creature) > BRD_LOCKOUT_CLEAR_PATH ||
                    lockoutPathChecks >= BRD_LOCKOUT_CLEAR_CHECKS)
                    continue;
                ++lockoutPathChecks;
                PathResult const walk = EvaluatePath(leader, creature);
                if (!walk.reachable || !walk.complete || walk.length > BRD_LOCKOUT_CLEAR_PATH)
                    continue;
                if (lockoutClearLogged.insert({group.Run().runSeq, creature->GetGUID()}).second)
                    group.LogEvent("brd_ring_lockout_clear",
                             JsonObject().Str("name", creature->GetName()).Num("entry", creature->GetEntry())
                                 .Real("walk", walk.length)
                                 .Real("fromSpot", creature->GetExactDist(holdSpot->x, holdSpot->y, holdSpot->z))
                                 .Num("lockoutLeftSec",
                                      group.Route().ringRetryAtSec - int64(GameTime::GetGameTime().count())));
            }

            if (escortNpc && (creature == escortNpc || creature->GetExactDist(escortNpc) > ESCORT_FIGHT_RADIUS))
                continue;

            if (bossRush && creature != nextBoss)
                continue;

            if (uint32 const boss = DeathlessUntilBoss(group.MapId(), creature->GetEntry()))
                if (bossAlive(boss))
                    continue;

            // Mobs deep below the surface (Underbog Frenzy at the lake bottom, Blackfathom Myrmidons at -60..-70):
            // the group would fight at the bottom. Swimming across water and fighting at the surface stay allowed.
            // Endless summons: a summoned non-boss entry killed ENDLESS_SUMMON_KILLS times already is fought when it
            // comes, never walked to (Halls of Lightning wave 9: 116 Slags from Volkhan's forge in 25 minutes, the
            // route farmed them and never engaged Volkhan; the summon scan had made them route targets).
            // [b44_ring] not a running hold event's own summon (eventSummon): a Ring of Law retry brings 24 beetles.
            if (!creature->GetSpawnId() && !creature->IsInCombat() && !creature->IsDungeonBoss() &&
                !creature->isWorldBoss() && !group.Route().bossCreatures.count(creature->GetGUID()) && !eventSummon)
                if (auto const kills = group.Route().deadEntries.find(creature->GetEntry());
                    kills != group.Route().deadEntries.end() && kills->second >= ENDLESS_SUMMON_KILLS)
                {
                    if (group.Route().endlessSummons.insert(creature->GetEntry()).second)
                        group.LogEvent("endless_summons", JsonObject().Str("name", creature->GetName())
                                                               .Num("entry", creature->GetEntry())
                                                               .Num("kills", kills->second));
                    continue;
                }
            // Fish (swim only, creature_template_movement Ground 0) are no target either: a group does not dive after
            // them, they come when they aggro. Gundrak wave 9: the route chased the moat's Drakkari Frenzies, the
            // group sank to the moat floor (z 100, a navmesh part of its own) and froze there 4 minutes.
            if (!creature->IsDungeonBoss() && !creature->isWorldBoss() && !creature->CanWalk() && creature->CanSwim() &&
                !group.Route().bossCreatures.count(creature->GetGUID()))
                continue;
            if (!creature->IsDungeonBoss() && !creature->isWorldBoss() && creature->IsUnderWater())
            {
                LiquidData const& liquid = creature->GetLiquidData();
                if (liquid.Level != INVALID_HEIGHT && liquid.Level - creature->GetPositionZ() > DEEP_TARGET_DEPTH)
                    continue;
            }

            // Script-ordered trash: a forge master pulled before the previous one evades at once (utgarde_keep.cpp).
            if (EncounterLockedFor(group, creature, script) || DoorKeyLocked(creature, script))
                continue;

            // Flying mobs high above their floor (Oculus Azure Ring Guardians) are fought from drakes, not walked to:
            // the path ends at a ledge and the leader walks off it.
            if (!leader->GetVehicleBase() && creature->CanFly() &&
                creature->GetPositionZ() - creature->GetMapHeight(creature->GetPositionX(), creature->GetPositionY(),
                                                                  creature->GetPositionZ()) > 8.0f)
                continue;

            if (std::find(laterBosses.begin(), laterBosses.end(), creature) != laterBosses.end())
                continue;

            for (Creature* later : laterBosses)
                if (creature->GetExactDist(later) < LATER_BOSS_TRASH_RADIUS)
                    deferred.insert(creature->GetGUID());

            // [lead43] one verdict, owned by the leader: a mob the leader refused (its planner's refusal / hold, the
            // reachability verdict) is no route target either - the route never stops for a mob the leader will not
            // hand to its planner (b92 rfc-h: 195 s of stop-vs-refusal holds out of combat)
            if (group.LeaderDecides() && !encounterCreature && !creature->IsInCombat() &&
                !creature->IsDungeonBoss() && group.BossOrder(creature) < 0)
            {
                std::string refusedWhy;
                if (auto const refused = group.State().refusedPicks.find(creature->GetGUID());
                    refused != group.State().refusedPicks.end())
                    refusedWhy = refused->second;
                else if (auto const judged = group.State().unreachable.find(creature->GetGUID());
                         judged != group.State().unreachable.end() &&
                         leader->GetExactDist(judged->second.tankAt) <= Leader::LEADER_COMMIT_MOVED_YD &&
                         creature->GetExactDist(judged->second.mobAt) <= Leader::LEADER_COMMIT_MOVED_YD)
                    refusedWhy = judged->second.why;
                if (!refusedWhy.empty())
                {
                    group.Route().pathVerdicts[creature->GetGUID()] = {false, "refused by the leader: " + refusedWhy,
                        -1.0f,
                                                               0.0f};
                    continue;
                }
            }

            // Route-only trash: idle trash off the route is left standing (rule 4: a fighting mob always counts; the
            // target the group walks to stays one until it is dead or skipped).
            if (routeRules && !encounterCreature && !creature->IsInCombat() &&
                creature->GetGUID() != group.Route().target && group.BossOrder(creature) < 0 && !routeNeeds(creature))
            {
                offRouteHostiles.emplace_back(leader->GetExactDist(creature), creature);
                RouteWhy const& off = routeWhy[creature->GetGUID()];
                group.Route().pathVerdicts[creature->GetGUID()] = {false, "off the route", off.check.routeDist,
                    off.reach};
                continue;
            }

            hostiles.emplace_back(leader->GetExactDist(creature), creature);
            {
                // [lead41] on the path: the route rule that keeps it (or why the route rules do not apply to it)
                auto const kept = routeWhy.find(creature->GetGUID());
                std::string const why =
                    kept != routeWhy.end() && kept->second.rule ? std::string(kept->second.rule)
                    : creature->GetGUID() == group.Route().target       ? std::string("the route's target")
                    : creature->IsInCombat()                    ? std::string("in the fight")
                    : group.BossOrder(creature) >= 0           ? std::string("a boss of the route")
                    : encounterCreature                         ? std::string("the encounter")
                                                                : std::string("no route rules here");
                group.Route().pathVerdicts[creature->GetGUID()] = {
                    true, why, kept != routeWhy.end() ? kept->second.check.routeDist : -1.0f,
                    kept != routeWhy.end() ? kept->second.reach : 0.0f};
            }
        }
        // Nothing on the route and no boss to walk to (not targetable yet, held back): the old full clear. Not while
        // the next boss lies over a link: with nothing chosen the group walks to it (WalkToLink), as before.
        bool const routeFallback = hostiles.empty() && !viaLink && !viaStarterLink;
        if (routeFallback)
        {
            hostiles.swap(offRouteHostiles);
            // [lead41] nothing on the route and no boss to walk to: the route's own full-clear fallback is the path
            for (auto const& [distance, creature] : hostiles)
            {
                auto& verdict = group.Route().pathVerdicts[creature->GetGUID()];
                verdict.on = true;
                verdict.why = "the route's full-clear fallback (nothing on the route)";
            }
        }
        // [b34_arc] (for mech-n 20261026-014521-mech-n-5D-17, 1194-1315 s) The fallback took the nearest-scored idle
        // trash wherever it stood: a Mechanar Wrecker on the lower level (227.9,-52.1,0.1, dz -25) while the group
        // stood upstairs and Pathaleon (139.5,149.3,25.7) lay on its own floor; the walk ended at a 22 yd ledge
        // ("ledge_drop_refused", "own path refused"), then the same with a Tinkerer at z 0. In the fallback the next
        // boss, standing and walkable (a complete path), comes first; else the trash on the leader's floor with a
        // complete path; only with neither is the other level's trash kept (the old full clear).
        if (routeFallback && !hostiles.empty())
        {
            constexpr float FALLBACK_SAME_FLOOR_DZ = 8.0f;
            constexpr size_t FALLBACK_PATH_CHECKS = 8;
            auto const walkable = [&](Creature* creature)
            {
                PathResult const path = EvaluatePath(leader, creature);
                return path.reachable && path.complete;
            };
            std::string why;
            if (nextBossStands && nextBoss->IsAlive() && !nextBoss->IsInCombat() && walkable(nextBoss))
            {
                hostiles.clear();
                hostiles.emplace_back(leader->GetExactDist(nextBoss), nextBoss);
                why = "next boss walkable";
            }
            else
            {
                std::sort(hostiles.begin(), hostiles.end(),
                          [](auto const& left, auto const& right) { return left.first < right.first; });
                std::vector<std::pair<float, Creature*>> sameFloor;
                size_t checks = 0;
                for (auto const& entry : hostiles)
                {
                    Creature* creature = entry.second;
                    if (std::fabs(creature->GetPositionZ() - leader->GetPositionZ()) > FALLBACK_SAME_FLOOR_DZ &&
                        !creature->IsInCombat())
                        continue;
                    if (checks++ >= FALLBACK_PATH_CHECKS)
                        break;
                    if (creature->IsInCombat() || walkable(creature))
                        sameFloor.push_back(entry);
                }
                if (!sameFloor.empty() && sameFloor.size() < hostiles.size())
                {
                    why = Acore::StringFormat("{} of {} on the leader's floor with a complete path", sameFloor.size(),
                                              hostiles.size());
                    hostiles.swap(sameFloor);
                }
            }
            if (!why.empty())
            {
                uint32& loggedMs = fallbackFloorLogMs[group.InstanceId()];
                uint32 const logNow = getMSTime();
                if (!loggedMs || getMSTimeDiff(loggedMs, logNow) > 10 * IN_MILLISECONDS)
                {
                    loggedMs = logNow ? logNow : 1;
                    LOG_INFO("playerbots", "[b34_arc] {}: route fallback narrowed: {} ({})", group.Run().runId, why,
                             nextBoss ? nextBoss->GetName() : "no next boss");
                }
            }
        }
        // [event_spot_hold] [event_boss_classic] Right after an event starter fired, the group holds where it is: no
        // fallback / "event chain" / far trash that walks it away. Two cases, both bounded by EVENT_START_HOLD_MS
        // after the last use:
        // (a) the next boss is still no valid target nearby (immune to PC / walking in: the event runs). brd-h
        //     20261025-163739 18:08:23: the 3rd Thunderbrew keg broken, Hurley "immune to pc" walking in, the fallback
        //     took a Shadowforge Peasant at (810,-296), the whole group walked out of the bar; Hurley was attackable
        //     14 s later.
        // (b) a multi-use starter with uses left whose next use comes within the window (cooldown <= the window):
        //     the same run 18:11:19-18:11:53, three Rocknot's Ale hand-ins 5 s apart, the target flipped between
        //     Private Rocknot and a Shadowforge Peasant ("event chain", 1019 yd walk) every 4-10 s, the group ran
        //     across the bar three times.
        // What fights, a boss of the hold (a), or what stands within EVENT_START_HOLD_RADIUS of the leader is kept.
        constexpr uint32 EVENT_START_HOLD_MS = 60 * IN_MILLISECONDS;
        constexpr float EVENT_START_HOLD_BOSS_RANGE = 120.0f;
        constexpr float EVENT_START_HOLD_RADIUS = 30.0f;
        constexpr uint8 EVENT_START_HOLD_MAX_USES = 10;  // not the Oculus portal (255 uses)
        uint32 const holdNow = getMSTime();
        bool const starterRecent = group.Route().lastStarterUseMs &&
                                   getMSTimeDiff(group.Route().lastStarterUseMs, holdNow) < EVENT_START_HOLD_MS;
        bool const holdForBoss = starterRecent && nextBoss && nextBoss->IsAlive() && !nextBoss->IsInCombat() &&
                                 !leader->IsValidAttackTarget(nextBoss) &&
                                 leader->GetExactDist(nextBoss) <= EVENT_START_HOLD_BOSS_RANGE;
        EventStarterDef const* holdStarter = nullptr;
        if (starterRecent && !holdForBoss)
            for (StarterTrack const& track : group.Route().starters)
                if (track.def && track.lastUseMs && getMSTimeDiff(track.lastUseMs, holdNow) < EVENT_START_HOLD_MS &&
                    track.uses > 0 && track.uses < track.def->maxUses &&
                    track.def->maxUses <= EVENT_START_HOLD_MAX_USES && track.def->cooldownMs <= EVENT_START_HOLD_MS)
                {
                    holdStarter = track.def;
                    break;
                }
        if (holdForBoss || holdStarter)
        {
            size_t const before = hostiles.size();
            hostiles.erase(std::remove_if(hostiles.begin(), hostiles.end(),
                                          [&](auto const& entry)
                                          {
                                              Creature const* creature = entry.second;
                                              return !creature->IsInCombat() &&
                                                     entry.first > EVENT_START_HOLD_RADIUS &&
                                                     !(holdForBoss && creature == nextBoss) &&
                                                     !EventWaveFresh(group.InstanceId(),  // [brd_seven_fast]
                                                                     creature->GetGUID());
                                          }),
                           hostiles.end());
            uint32& loggedMs = eventStartHoldLogMs[group.InstanceId()];
            if (!loggedMs || getMSTimeDiff(loggedMs, holdNow) > 10 * IN_MILLISECONDS)
            {
                loggedMs = holdNow ? holdNow : 1;
                group.LogEvent("event_spot_hold",
                         JsonObject().Str("why", holdForBoss ? "next boss not attackable yet" : "starter uses left")
                             .Str("boss", holdForBoss ? nextBoss->GetName() : "")
                             .Str("starter", holdStarter ? holdStarter->note : "")
                             .Num("sinceStarterSec", getMSTimeDiff(group.Route().lastStarterUseMs,
                                 holdNow) / IN_MILLISECONDS)
                             .Num("dropped", int64(before - hostiles.size())));
            }
        }
        size_t const offRoute = offRouteHostiles.size();

        // A hold spot's objectives (EventSpotDef::holdAlways) are evaluated first: only the MAX_PATH_CANDIDATES
        // nearest get a path, and Black Morass wave 15 had more adds next to Medivh than that - the rift keepers 90-110
        // yd out were never evaluated, the rifts stayed open and the shield fell (boss_state 5 -> 0 at 705 s).
        auto const isObjective = [holdSpot, &fledSurvivors, &group](Creature const* creature)
        {
            if (fledSurvivors.count(creature->GetGUID()))
                return true;  // [fled_survivor]
            if (holdSpot && EventWaveFresh(group.InstanceId(), creature->GetGUID()))
                return true;  // [brd_seven_fast] a scripted wave that just became attackable
            return holdSpot && (AtHoldGuard(*holdSpot, creature) ||
                                std::find(holdSpot->holdAlways.begin(), holdSpot->holdAlways.end(),
                                          creature->GetEntry()) != holdSpot->holdAlways.end());
        };
        std::sort(hostiles.begin(), hostiles.end(),
                  [&deferred, &isObjective](auto const& left, auto const& right)
                  {
                      bool const leftObjective = isObjective(left.second);
                      bool const rightObjective = isObjective(right.second);
                      if (leftObjective != rightObjective)
                          return leftObjective;
                      bool const leftDeferred = deferred.count(left.second->GetGUID()) != 0;
                      bool const rightDeferred = deferred.count(right.second->GetGUID()) != 0;
                      if (leftDeferred != rightDeferred)
                          return rightDeferred;
                      return left.first < right.first;
                  });

        ObjectGuid bestGuid;
        float bestScore = 0.0f;
        bool bestIsBoss = false;
        bool bestObjective = false;  // a hold spot's holdAlways entry: never "respawning trash" or far trash
        bool nearObjective = false;
        float bestLength = 0.0f;
        // The best of the targets that are no far trash (FAR_TARGET_PATH): taken over a far best one.
        ObjectGuid nearGuid;
        float nearScore = 0.0f;
        bool nearIsBoss = false;
        // [target_commit] the current target as scored this retarget (empty: not scored - dead, skipped, unreachable)
        ObjectGuid commitGuid;
        float commitScore = 0.0f;
        float commitLength = 0.0f;
        bool commitIsBoss = false;
        bool commitObjective = false;
        [[maybe_unused]] bool const oculusDrakePhase = onOculus && OculusDrakePhase(leader);  // [b33_occ] unused now

        // Clear the boss room first (pass 3, mechanics agent): trash scores +0.25 x its distance to the next boss and
        // the boss itself 0, so the boss came before the trash of his own room (Magisters' Terrace: Selin with his
        // Wretched packs, Mechanar: Capacitus - wipes). While an idle, not deferred mob stands within
        // BOSS_ROOM_TRASH_RADIUS of the boss on his floor, the boss scores BOSS_ROOM_TRASH_PENALTY more. Trash that
        // cannot be reached is skipped as "unreachable" by the evaluation below, so this cannot hold the boss forever.
        bool bossRoomOccupied = false;
        // [boss_area_clear] (every content, raids too) trash round the next boss that a refusal skipped (no sight
        // point, the planner's refusal, a skipped pack behind it) is a target again: it is cleared before him, by
        // sight or, last, by body (Steer's [pull_option]). A reachability skip stays. At most BOSS_AREA_MAX_UNSKIPS
        // times per mob, BOSS_AREA_UNSKIP_MS apart; then it stays skipped ("boss area: given up").
        uint32 bossAreaLeft = 0;
        std::string bossAreaFirst;
        // [b33_mech2] over every candidate, not the route's hostiles: a skipped mob never gets into hostiles (the
        // candidate loop drops skipUntil first), so the unskip below never ran (0 "unskip" lines in every log).
        if (ROUTE_BOSS_AREA_CLEAR && nextBoss)
            for (Creature* creature : candidates)
            {
                if (!creature || !creature->IsInWorld() || !creature->IsAlive() || creature->IsInCombat() ||
                    creature == nextBoss || creature->GetMap() != nextBoss->GetMap() ||
                    creature->GetExactDist(nextBoss) > BOSS_AREA_SCAN_RANGE || deferred.count(creature->GetGUID()) ||
                    FastRespawner(creature) || !leader->IsValidAttackTarget(creature))
                    continue;
                char const* const area = BossAreaTrashOf(leader, nextBoss, creature,
                                                         group.Route().bossCreatures.count(creature->GetGUID()) != 0);
                if (!area)
                    continue;
                ++bossAreaLeft;
                if (bossAreaFirst.empty())
                    bossAreaFirst = creature->GetName();
                auto const skipped = group.Route().skipUntil.find(creature->GetGUID());
                if (skipped == group.Route().skipUntil.end())
                    continue;
                std::string const reason = group.Route().skipReason.count(creature->GetGUID())
                                               ? group.Route().skipReason[creature->GetGUID()]
                                               : std::string();
                if (ReachabilitySkip(reason))
                    continue;
                BossAreaUnskip& unskip = bossAreaUnskips[{group.Run().runSeq, creature->GetGUID()}];
                if (unskip.lastMs && getMSTimeDiff(unskip.lastMs, now) < BOSS_AREA_UNSKIP_MS)
                    continue;
                if (unskip.count >= BOSS_AREA_MAX_UNSKIPS)
                {
                    group.Route().skipReason[creature->GetGUID()] = "boss area: given up (" + reason + ")";
                    group.LogEvent("boss_area_clear",
                             JsonObject().Str("boss", nextBoss->GetName()).Num("bossEntry", nextBoss->GetEntry())
                                 .Str("mob", creature->GetName()).Num("entry", creature->GetEntry()).Str("why", area)
                                 .Str("action", "given up").Str("skipReason", reason)
                                 .Num("tries", unskip.count));
                    continue;
                }
                ++unskip.count;
                unskip.lastMs = now ? now : 1;
                group.Route().skipUntil.erase(skipped);
                group.Route().skipReason.erase(creature->GetGUID());
                group.LogEvent("boss_area_clear",
                         JsonObject().Str("boss", nextBoss->GetName()).Num("bossEntry", nextBoss->GetEntry())
                             .Str("mob", creature->GetName()).Num("entry", creature->GetEntry()).Str("why", area)
                             .Str("action", "unskip").Str("skipReason", reason).Num("try", unskip.count));
            }
        if (ROUTE_BOSS_ROOM_FIRST && nextBoss)
            for (auto const& [distance, creature] : hostiles)
                if (creature != nextBoss && creature->IsAlive() && !creature->IsInCombat() &&
                    !deferred.count(creature->GetGUID()) && !group.Route().skipUntil.count(creature->GetGUID()) &&
                    !creature->IsDungeonBoss() && !creature->isWorldBoss() &&
                    !group.Route().bossCreatures.count(creature->GetGUID()) &&
                    !CallsBossForHelp(creature, nextBoss) &&
                    (JoinsBossFight(leader, nextBoss, creature) ||
                     (ROUTE_BOSS_AREA_CLEAR && BossAreaTrashOf(leader, nextBoss, creature, false))))
                {
                    bossRoomOccupied = true;
                    break;
                }
        // One line per party every 30 s while the boss waits for his area.
        if (bossRoomOccupied && bossAreaLeft)
        {
            uint32& logged = bossAreaHoldLogMs[group.Run().runSeq];
            if (!logged || getMSTimeDiff(logged, now) >= 30 * IN_MILLISECONDS)
            {
                logged = now ? now : 1;
                group.LogEvent("boss_area_clear",
                         JsonObject().Str("boss", nextBoss->GetName()).Num("bossEntry", nextBoss->GetEntry())
                             .Str("action", "boss pull held").Num("remaining", bossAreaLeft)
                             .Str("first", bossAreaFirst));
            }
        }

        // The idle mobs the route skips now (RouteCrossesSkipped), with their aggro reach.
        std::vector<std::pair<Creature*, float>> skippedReaches;
        IdleAggroCheck const idleCheck{leader};
        for (auto const& [guid, until] : group.Route().skipUntil)
            if (Creature* mob = map->GetCreature(guid); mob && mob->IsInWorld() && idleCheck(mob))
                skippedReaches.emplace_back(mob, AggroReach(mob, leader));
        // A locked Forge Master is passed as a skipped pack is: a route through his reach comes after the others.
        for (Creature* master : lockedForgeMasters)
            if (idleCheck(master))
                skippedReaches.emplace_back(master, AggroReach(master, leader));
        std::set<ObjectGuid> crossesSkipped;

        // [lead204] the leader's planned order over the plain on-path idle trash (no boss, objective, key, deferred
        // or boss-calling trash, nothing of a special route: a hold spot, an escort, an event chain, a walk-in spot,
        // a pull-alone mob, a boss rush, the full-clear fallback); its next stop's mob is always scored
        bool const planApplies = group.LeaderDecides() && routeRules && !routeFallback && !viaLink && !viaStarterLink &&
                                 !holdSpot && !escortNpc && chainAnchors.empty() && !leadSpot &&
                                 pullAloneMobs.empty() && !bossRush && fledSurvivors.empty();
        std::string planStep;
        ObjectGuid planNext;
        if (planApplies)
        {
            // [lead216] b184 rfc-h 5B: the plan to Oggleflint walked the Ragefire Troggs at (-262,-6) / (-241,-8),
            // the Earthborers 23 yd from them (-272.5,8.7) / (-296.6,7.6) were off the route to that boss and the
            // plan to Taragaman came back for them from Oggleflint (431-466 s, 233 yd); the dungeon's optimum takes
            // them with the Troggs. The plan's walk goes to its stops: an idle mob off the route within its aggro
            // reach of an on-path mob, in its sight, is on that walk too (once: not the mobs beside it in turn)
            // [lead228] b187 rfc-h 5C 246-458 s: the side room under the walkway (Ragefire lows 8-18, 12 mobs) came in
            // one after the other - Shaman 12 ("skip: detour 340 yd for 79 yd") beside a plan stop, then as the plan's
            // stop the route's target, which is on the path by that alone, and its neighbours beside it in turn: 58
            // pulls, the tank's route 3352 yd. Beside means beside a mob the route itself needs (a route rule) that
            // stands at its spot - not the route's target or plan stop, not a mob that fled or walked off - and never
            // a mob the route skips
            auto const routeNeedsHere = [&](Creature* on)
            {
                auto const ruled = routeWhy.find(on->GetGUID());
                if (ruled == routeWhy.end() || !ruled->second.rule)
                    return false;
                float const wander =
                    on->GetDefaultMovementType() == RANDOM_MOTION_TYPE ? on->GetWanderDistance() : 0.0f;
                return on->GetExactDist(PlanSpawnSpot(on)) <= wander + Leader::LEADER_COMMIT_MOVED_YD;
            };
            std::vector<std::pair<float, Creature*>> beside;
            for (auto const& [distance, creature] : offRouteHostiles)
            {
                if (creature->IsInCombat() || group.BossOrder(creature) >= 0 || deferred.count(creature->GetGUID()) ||
                    group.Route().skipUntil.count(creature->GetGUID()))
                    continue;
                float const reach = AggroReach(creature, leader);
                // [lead360] b196 rfc-h 5E 295-545 s: Shaman 12 under the walkway was "beside" the boss-area Shaman
                // at (-148,0) - 25 yd in 3D through the ledge, in sight - yet its walk there is the 340-355 yd round
                // the lower loop that ROUTE_SKIP_DETOUR skips from anywhere else. As a plan stop it made the tour
                // 742 yd for 190, and the walk to it woke the whole side room (16 mobs, lows 8-43, 1100 yd, Oggleflint
                // at 686 s vs 375 s for 5A). Beside a stop means it joins that stop's fight on foot: the walk
                // between their spots is no detour by the route's own measure (DETOUR_RATIO / DETOUR_EXTRA)
                auto const joinsOnFoot = [&](Creature* on)
                {
                    Position const onSpot = PlanSpawnSpot(on);
                    Position const mobSpot = PlanSpawnSpot(creature);
                    float const straight = onSpot.GetExactDist(mobSpot);
                    float const walk =
                        PlanWalkBetween(group, leader, on->GetGUID(), onSpot, creature->GetGUID(), mobSpot);
                    return walk >= 0.0f && walk <= std::max(DETOUR_RATIO * straight, straight + DETOUR_EXTRA);
                };
                for (auto const& [onDistance, on] : hostiles)
                    if (on != nextBoss && !on->IsInCombat() && routeNeedsHere(on) &&
                        on->GetExactDist(creature) <= reach && on->IsWithinLOSInMap(creature) && joinsOnFoot(on))
                    {
                        beside.emplace_back(distance, creature);
                        group.Route().pathVerdicts[creature->GetGUID()] = {
                            true, "beside the plan's stop " + on->GetName(), on->GetExactDist(creature), reach};
                        break;
                    }
            }
            hostiles.insert(hostiles.end(), beside.begin(), beside.end());
            std::vector<Creature*> planMobs;
            for (auto const& [distance, creature] : hostiles)
                if (creature != nextBoss && !creature->IsInCombat() && !creature->IsDungeonBoss() &&
                    !creature->isWorldBoss() && !group.Route().bossCreatures.count(creature->GetGUID()) &&
                    group.BossOrder(creature) < 0 && !isObjective(creature) && !linkKeys.count(creature->GetEntry()) &&
                    !guardKeys.count(creature->GetEntry()) && !deferred.count(creature->GetGUID()) &&
                    !gateKeyHeld(creature) && !(nextBoss && CallsBossForHelp(creature, nextBoss)))
                    planMobs.push_back(creature);
            planNext = PlannedNext(group, leader, nextBoss, planMobs, planStep);
            // [lead1283] a pack behind the group is off the path: neither the route's pick nor the leader's
            std::set<ObjectGuid> const& behindGuids = group.Route().planBehind;
            if (!behindGuids.empty())
                hostiles.erase(std::remove_if(hostiles.begin(), hostiles.end(),
                                              [&](auto const& entry)
                                              {
                                                  Creature* creature = entry.second;
                                                  if (!behindGuids.count(creature->GetGUID()) || creature->IsInCombat())
                                                      return false;
                                                  group.Route().pathVerdicts[creature->GetGUID()] = {
                                                      false, "behind the group (the plan's cleared walk)", -1.0f,
                                                      0.0f};
                                                  return true;
                                              }),
                               hostiles.end());
        }
        else if (!group.Route().routePlan.empty() || !group.Route().routePlanMeets.empty())
        {
            group.Route().routePlan.clear();
            group.Route().routePlanMeets.clear();
            group.Route().routePlanBoss.Clear();
            group.LogEvent("route_plan", JsonObject().Str("why", "no plan here (a special route)").Num("stops", 0));
        }
        bool planScored = false;
        float planScore = 0.0f;
        float planLength = 0.0f;

        uint32 evaluated = 0;
        uint32 detours = 0;  // ROUTE_SKIP_DETOUR: detour paths past MAX_PATH_CANDIDATES (DETOUR_EXTRA_PATHS)
        // [guard_target_keep] the kept guard and the best other guard it was kept over
        bool guardKept = false;
        ObjectGuid guardOtherGuid;
        float guardOtherScore = 0.0f;
        for (auto const& [distance, creature] : hostiles)
        {
            // A link key (linkKeys) is looked at however far it stands, and so are the next boss and the target the
            // group walks to now. RFC wave 38: Oggleflint's only way runs round the whole lower loop (365-448 yd of
            // path for 38-57 yd straight); on that walk 50-60 mobs stood nearer in a straight line, the boss fell out
            // of the MAX_PATH_CANDIDATES nearest, was never scored, and the route turned to trash up the other branch
            // (Searing Blade, 25 minutes, 0/4 bosses). Scored, the trash on the boss's way still comes first by score.
            bool const linkKey =
                linkKeys.count(creature->GetEntry()) != 0 || guardKeys.count(creature->GetEntry()) != 0;
            bool const alwaysScored = linkKey || creature == nextBoss || creature->GetGUID() == group.Route().target ||
                                      creature->GetGUID() == planNext;  // [lead204]
            if ((evaluated >= MAX_PATH_CANDIDATES || evaluated + detours >= MAX_PATH_CANDIDATES + DETOUR_EXTRA_PATHS) &&
                !alwaysScored)
                continue;
            ++evaluated;

            PathResult const path = EvaluatePath(leader, creature);
            // A detour (ROUTE_SKIP_DETOUR): idle trash on another floor or ledge is no target - not in the boss
            // room, not on the route in the sense that matters (it reaches the group only over that walk), not worth
            // the walk. Bosses, objectives (link keys, hold spot, gates, starters), a mob that joins the boss and a
            // fighting one stay; so does trash no farther than the boss's own walk + DETOUR_BOSS_WAY_MARGIN (the way
            // is that long itself: RFC wave 38, Oggleflint over the lower loop). Not skipped (SkipTarget): the next
            // retarget measures it again from where the group stands then, and a skip would make its reach a
            // "skipped pack" every route under the ledge pays for.
            bool const detourFallback =
                group.Route().detourFallbackUntilMs && int32(group.Route().detourFallbackUntilMs - now) > 0;
            if (ROUTE_SKIP_DETOUR && !detourFallback && path.reachable && !creature->IsInCombat() &&
                !creature->IsDungeonBoss() &&
                !creature->isWorldBoss() && !group.Route().bossCreatures.count(creature->GetGUID()) &&
                group.BossOrder(creature) < 0 && !linkKey && !isObjective(creature) &&
                !requiredEntries.count(creature->GetEntry()) &&
                !JoinsBossFight(leader, nextBoss, creature))
            {
                float const straight = leader->GetExactDist(creature);
                bool const detour = path.length > std::max(DETOUR_RATIO * straight, straight + DETOUR_EXTRA);
                bool const onBossWay =
                    nextBoss && nextRoute.reachable && path.length <= nextRoute.length + DETOUR_BOSS_WAY_MARGIN;
                // [zf-event-key-leg] trash no farther than the walk to the pending step's key creature
                bool const onEventWay =
                    starterKeyRoute.reachable && path.length <= starterKeyRoute.length + DETOUR_BOSS_WAY_MARGIN;
                if (detour && !onBossWay && !onEventWay)
                {
                    --evaluated;
                    ++detours;
                    std::string const reason =
                        Acore::StringFormat("detour ({:.0f} yd for {:.0f} yd)", path.length, straight);
                    uint32& loggedMs = detourLoggedMs[{group.Run().runSeq, creature->GetGUID()}];
                    if (!loggedMs || getMSTimeDiff(loggedMs, now) >= DETOUR_LOG_MS)
                    {
                        loggedMs = now ? now : 1;
                        group.LogEvent("skip_target", JsonObject().Str("guid", creature->GetGUID().ToString())
                                                           .Str("name", creature->GetName())
                                                           .Num("entry", creature->GetEntry())
                                                           .Str("reason", "skip: " + reason)
                                                           .Num("skipSec", 0)
                                                           .Real("route", path.length).Real("straight", straight)
                                                           .Real("bossRoute", nextRoute.reachable ? nextRoute.length
                                                                                                  : -1.0f)
                                                           .Real("x", creature->GetPositionX())
                                                           .Real("y", creature->GetPositionY())
                                                           .Real("z", creature->GetPositionZ()));
                    }
                    continue;
                }
            }
            // Oculus: the rings are flown to, never "nearer" on foot - after Drakos (the drake phase) only the
            // complete walks count for a target on another ring (wave 9: the leader stood under a Centrifuge Construct
            // 74 yd up on the next ring). Before Drakos the partial walks stay targets as in wave 9 (a full clear):
            // pass 2 required complete walks everywhere, and wave 10 skipped all 27 ring targets, Drakos was "not
            // pulled" once, nothing was left and the run stalled at 11 min.
            // [b33_occ] Before Drakos too: no drake can be had before his death (OCRoute GetOculusDrakeGoal: NONE), so
            // a partial walk to another ring is never a target on foot. occ-h 20261026-000434-occ-h-5C-21: Drakos
            // skipped "route end" at 322 s (the tank's mount loop beside him, OccPortalMountMultiplier), then 9 ring
            // targets (Centrifuge Constructs, Ring-Lords, z 433 from z 360) were walked at, ~20 s each, from 323 s
            // to the stop ("steer_stuck" dz 73, "unseen_route_end", 151 s idle). Wave 10's stall came from the
            // "not pulled" Drakos (BOSS_NOT_PULLED_SKIP_MS since); the skip clears once the leader is on another
            // level (UNREACHABLE_RECHECK_HEIGHT), so the drake phase finds them again from their ring.
            bool const otherRing = std::fabs(creature->GetPositionZ() - leader->GetPositionZ()) > 20.0f;
            bool const oculusFootOnly =
                onOculus && (!ROUTE_OCULUS_DRAKE_ONLY || otherRing);
            if (!path.reachable || (oculusFootOnly && !path.complete))
            {
                group.SkipTarget(creature->GetGUID(), UNREACHABLE_SKIP_MS, "unreachable");
                continue;
            }
            // Not through a part closed to this run (AreaGateDef: an LBRS group never walks the UBRS hall).
            if (!leaderInClosedArea && RouteCrossesClosedArea(path.points, keepOut))
            {
                group.SkipTarget(creature->GetGUID(), UNREACHABLE_SKIP_MS, "route through a closed area");
                continue;
            }
            // Not through a closed door (ROUTE_CLOSED_DOORS): idle trash behind it waits until it opens.
            // ROUTE_CLOSED_DOORS_ALL: bosses and fighting mobs too (a fighting one comes to the group or the bots fight
            // it from this side), unless the door opens for it (DoorOpensFor: a DoorEventDef key creature).
            bool const doorChecked = ROUTE_CLOSED_DOORS_ALL ||
                                     (!creature->IsInCombat() && !creature->IsDungeonBoss() &&
                                      !creature->isWorldBoss() &&
                                          !group.Route().bossCreatures.count(creature->GetGUID()) &&
                                      group.BossOrder(creature) < 0);
            // [hleft-passage-door-route] A door that opens on a boss's death and that the walk goes round is no
            // blocker: the target stays, Steer walks the way round (SteerLeg).
            if (!closedDoors.empty() && doorChecked)
                if (std::vector<GameObject*> const shut =
                        ROUTE_CLOSED_DOORS_ALL ? DoorsClosedFor(closedDoors, creature) : closedDoors;
                    !shut.empty())
                    if (GameObject* door = RouteThroughClosedDoor(path.points, shut, leader->GetPhaseMask());
                        door && !GoesRoundPassageDoor(leader, script, creature, door, shut))
                {
                    // A door the instance's own escort opens (Culling of Stratholme: Arthas opens the Shkaf gate
                    // 188686 at his book shelf, culling_of_stratholme.cpp waypoint 36): what lies behind it is no
                    // target yet and no skip (cos-n wave 11: 56 "route through a closed door" lines).
                    if (std::find(ESCORT_OPENED_DOORS.begin(), ESCORT_OPENED_DOORS.end(), door->GetEntry()) !=
                        ESCORT_OPENED_DOORS.end())
                        continue;
                    // [strat_trap_gate_trash] a rat-trap portcullis opens again in 20 s: no target now, no 60 s skip.
                    if (IsTrapGate(door))
                    {
                        if (!group.Route().trapGateLogMs || getMSTimeDiff(group.Route().trapGateLogMs, now) >=
                            TRAP_GATE_LOG_MS)
                        {
                            group.Route().trapGateLogMs = now ? now : 1;
                            group.LogEvent("strat_trap_gate_trash",
                                     JsonObject().Str("door", door->GetName()).Num("doorEntry", door->GetEntry()));
                        }
                        continue;
                    }
                    // A door the group's key opens again (a used-up UseItem starter on it, the key in the leader's
                    // bags, re-arms left) while the leader stands near it: what lies behind stays a target, its walk
                    // holds at the door (door_hold) and that re-arms the key (strat-h build 14).
                    bool keyDoor = false;
                    if (leader->GetExactDist(door) <= KEY_DOOR_REACH)
                        for (StarterTrack const& keyStarter : group.Route().starters)
                            keyDoor |= keyStarter.def->kind == StarterKind::UseItem &&
                                       keyStarter.doorRearms < DOOR_REARM_MAX &&
                                       leader->HasItemCount(keyStarter.def->itemId, 1) &&
                                       std::find(keyStarter.def->entries.begin(), keyStarter.def->entries.end(),
                                                 door->GetEntry()) != keyStarter.def->entries.end();
                    if (!keyDoor)
                    {
                        group.SkipTarget(creature->GetGUID(), CLOSED_DOOR_SKIP_MS,
                                   Acore::StringFormat("route through a closed door ({} {})", door->GetName(),
                                                       door->GetEntry()));
                        continue;
                    }
                }
            // While the group heads for a link to the next boss, what only a partial walk "nears" is on the far side
            // (The Mechanar: the Centurions and Destroyers by Sepethrea above the lift, wave 2026-10-24 steer_stuck
            // "route end" from the foot of the shaft for 6 minutes): it comes after the link.
            if (viaLink && !path.complete)
                continue;

            float score = path.length;
            if (nextBoss && nextBoss != creature && !fledSurvivors.count(creature->GetGUID()))
                score += BOSS_ROUTE_WEIGHT * creature->GetExactDist(nextBoss);
            if (fledSurvivors.count(creature->GetGUID()))
                score -= FLED_SURVIVOR_BONUS;  // [fled_survivor] before anything further on
            if (deferred.count(creature->GetGUID()))
                score += DEFERRED_TRASH_PENALTY;
            if (bossRoomOccupied && creature == nextBoss)
                score += BOSS_ROOM_TRASH_PENALTY;
            // [b32_smpull] Mograine after his idle chapel (LinkedChapelTrash)
            if (creature->GetEntry() == ScarletMonastery::NPC_COMMANDER_MOGRAINE &&
                ScarletMonastery::IdleMograineWithChapel(leader) == creature)
                score += BOSS_ROOM_TRASH_PENALTY;
            // [sv-bog-overlord-alone] a mob pulled alone comes first; idle trash within its reach (its aggro reach
            // plus the assistance radius) waits for its death, so neither pull brings the other.
            if (!pullAloneMobs.empty() && !creature->IsInCombat())
            {
                if (std::find(pullAloneMobs.begin(), pullAloneMobs.end(), creature) != pullAloneMobs.end())
                    score -= PULL_ALONE_BONUS;
                else
                    for (Creature* alone : pullAloneMobs)
                        if (creature->GetExactDist(alone) <= AggroReach(alone, leader) + PULL_ALONE_ASSIST)
                        {
                            score += BOSS_ROOM_TRASH_PENALTY;
                            if (pullAloneLogged.insert({group.Run().runSeq, creature->GetGUID()}).second)
                                group.LogEvent("pull_alone_first",
                                         JsonObject().Str("name", creature->GetName())
                                             .Num("entry", creature->GetEntry()).Str("waitsFor", alone->GetName())
                                             .Num("waitsForEntry", alone->GetEntry())
                                             .Real("distance", creature->GetExactDist(alone)));
                            break;
                        }
            }
            // [gate-key-after-boss] never ahead of the standing next boss
            if (gateKeyHeld(creature))
            {
                score += BOSS_ROOM_TRASH_PENALTY;
                if (gateKeyHeldLogged.insert({group.Run().runSeq, creature->GetGUID()}).second)
                    group.LogEvent("gate_key_after_boss",
                             JsonObject().Str("name", creature->GetName()).Num("entry", creature->GetEntry())
                                 .Str("nextBoss", nextBoss->GetName()).Num("order", nextOrder));
            }
            // Trash that calls the idle boss for help is fought with him, after the rest of his room: the boss is
            // the target then, so the boss pull (buffs, prepot, boss tactics) brings it.
            if (ROUTE_BOSS_ROOM_FIRST && nextBoss && creature != nextBoss && CallsBossForHelp(creature, nextBoss))
                score += BOSS_ROOM_TRASH_PENALTY;
            if (RouteCrossesSkipped(leader, path.points, creature, skippedReaches))
            {
                score += ROUTE_SKIPPED_PACK_PENALTY;
                crossesSkipped.insert(creature->GetGUID());
            }

            // Encounter creatures without the boss flag count too (Violet Hold wave 7/8: the released prisoner
            // Xevozz, rank 1, no CREATURE_FLAG_EXTRA_DUNGEON_BOSS, was "careful pull refused" and skipped 3 min
            // while the portals broke the seal).
            // An event objective of the hold spot (EventSpotDef::holdAlways, Black Morass' rift keepers): first, as
            // a group closes the rift before it fights the adds the rift keeps sending (wave 13: the adds next to
            // Medivh always scored better, every keeper was chosen, interrupted by them and dropped as "respawning
            // trash" on its fourth choice; no rift closed in waves 10, 11 and 13).
            bool const holdObjective = holdSpot && std::find(holdSpot->holdAlways.begin(), holdSpot->holdAlways.end(),
                                                             creature->GetEntry()) != holdSpot->holdAlways.end();
            // [hleft-bm-tank-fetch] Black Morass while the event runs: the tank's own tbc-bm strategy fetches the rift
            // keepers alone and drags them to Medivh; out of combat the harness sends nobody after a keeper away from
            // the hold (the ring adds and the keepers at Medivh stay its targets, the 60 % guard rule below as before).
            if (holdObjective && group.MapId() == BLACK_MORASS_MAP && !leader->IsInCombat() &&
                group.Route().watchedRift >= 1 && group.Route().watchedShield > 0 && !creature->IsDungeonBoss() &&
                !group.Route().bossCreatures.count(creature->GetGUID()) &&
                creature->GetExactDist2d(holdSpot->x, holdSpot->y) > BM_KEEPER_FETCH_RANGE)
                continue;
            bool const objective = linkKey || holdObjective;
            if (objective)
                score -= HOLD_OBJECTIVE_PRIORITY;
            if (guardKeys.count(creature->GetEntry()) && PendingGuardApproach(map, creature))
            {
                if (creature->GetGUID() == group.Route().target)
                {
                    score -= GUARD_TARGET_KEEP;
                    guardKept = true;
                }
                else if (guardOtherGuid.IsEmpty() || score < guardOtherScore)
                {
                    guardOtherGuid = creature->GetGUID();
                    guardOtherScore = score;
                }
            }
            // Black Morass bm-h 2026-10-25: ~190 of 200 shield points fell while the whole group stood 41-140 yd away
            // at a keeper / rift boss and the adds on Medivh's ring were never chosen. Out of combat they come first.
            // bm-n build 19: with keepers first the group chased a Rift Lord 106-154 yd out while the ring adds took
            // the shield 30 -> 0 in 28 s. The shield-hitting adds come first when the shield is under
            // HOLD_GUARD_SHIELD_PCT, and a non-boss keeper farther than HOLD_KEEPER_DISTANT from the spot ranks below
            // them; otherwise the keepers first.
            bool const guardFirst = holdSpot && group.Route().watchedShield >= 0 && group.Route().watchedShield <
                HOLD_GUARD_SHIELD_PCT;
            if (holdSpot && !holdObjective && !leader->IsInCombat() && AtHoldGuard(*holdSpot, creature))
                score -= guardFirst ? HOLD_OBJECTIVE_PRIORITY + HOLD_GUARD_PRIORITY : HOLD_GUARD_PRIORITY;
            // [b32_st] An objective that is also a guard (Sunken Temple: the Nightmare Suppressor 8497, holdAlways and
            // guardEntries) out of combat at the spot comes before the other objectives (the Bloodkeepers 8438 attack
            // the group by themselves; an idle Suppressor drains the Shade's counter 2, a fight stops it).
            if (holdSpot && holdObjective && !creature->IsInCombat() && AtHoldGuard(*holdSpot, creature))
                score -= HOLD_GUARD_PRIORITY;
            if (holdObjective && !creature->IsDungeonBoss() &&
                !group.Route().bossCreatures.count(creature->GetGUID()) &&
                (guardFirst || creature->GetExactDist(holdSpot->x, holdSpot->y, holdSpot->z) > HOLD_KEEPER_DISTANT))
                score += HOLD_OBJECTIVE_PRIORITY - HOLD_GUARD_PRIORITY / 2.0f;
            bool const isBoss = creature->IsDungeonBoss() || creature->isWorldBoss() ||
                                group.Route().bossCreatures.count(creature->GetGUID()) ||
                                group.BossOrder(creature) >= 0;
            if (creature->GetGUID() == group.Route().target && !group.Route().targetIsStarter &&
                !creature->IsInCombat())
            {
                commitGuid = creature->GetGUID();  // [target_commit]
                commitScore = score;
                commitLength = path.length;
                commitIsBoss = isBoss;
                commitObjective = objective;
            }
            if (creature->GetGUID() == planNext)  // [lead204]
            {
                planScored = true;
                planScore = score;
                planLength = path.length;
            }
            if (bestGuid.IsEmpty() || score < bestScore)
            {
                bestGuid = creature->GetGUID();
                bestScore = score;
                bestIsBoss = isBoss;
                bestObjective = objective;
                bestLength = path.length;
            }
            if ((isBoss || objective || path.length <= FAR_TARGET_PATH) && (nearGuid.IsEmpty() || score < nearScore))
            {
                nearGuid = creature->GetGUID();
                nearScore = score;
                nearIsBoss = isBoss;
                nearObjective = objective;
            }
        }

        // [target_commit] The idle target walked to stays unless the challenger is much better, fights, is a boss or an
        // objective (see ROUTE_TARGET_COMMIT).
        if (ROUTE_TARGET_COMMIT && !commitGuid.IsEmpty() && !bestGuid.IsEmpty() && bestGuid != commitGuid &&
            !bestIsBoss && !bestObjective && getMSTimeDiff(group.Route().targetSinceMs, now) < TARGET_COMMIT_MAX_MS)
            if (Creature* challenger = map->GetCreature(bestGuid); challenger && !challenger->IsInCombat())
            {
                float const margin = std::max(TARGET_COMMIT_MARGIN, TARGET_COMMIT_SHARE * commitLength);
                // [b33_harn] a committed boss never outlasts his own area trash (BossAreaTrashOf: room, footprint):
                // ok-h 20261026-000434-ok-h-5C-36, Amanitar kept (92.7) over the Savage Cave Beast in her field (80.9)
                Creature* const keptBoss = commitIsBoss ? map->GetCreature(commitGuid) : nullptr;
                bool const areaChallenger =
                    ROUTE_BOSS_AREA_CLEAR && keptBoss && keptBoss->IsAlive() &&
                    BossAreaTrashOf(leader, keptBoss, challenger,
                                    group.Route().bossCreatures.count(challenger->GetGUID()) != 0);
                if (!areaChallenger && bestScore > commitScore - margin)
                {
                    if (targetCommitLogged.insert({group.Run().runSeq, commitGuid, bestGuid}).second)
                        if (Creature* kept = map->GetCreature(commitGuid))
                            group.LogEvent("target_commit",
                                     JsonObject().Str("name", kept->GetName()).Num("entry", kept->GetEntry())
                                         .Real("score", commitScore).Str("over", challenger->GetName())
                                         .Num("overEntry", challenger->GetEntry()).Real("overScore", bestScore)
                                         .Real("margin", margin)
                                         .Num("sinceSec", getMSTimeDiff(group.Route().targetSinceMs,
                                             now) / IN_MILLISECONDS));
                    bestGuid = commitGuid;
                    bestScore = commitScore;
                    bestIsBoss = commitIsBoss;
                    bestObjective = commitObjective;
                    bestLength = commitLength;
                }
            }

        // [guard_target_keep] logged once per kept guard and the guard it was kept over
        if (guardKept && bestGuid == group.Route().target && !guardOtherGuid.IsEmpty() &&
            guardOtherScore < bestScore + GUARD_TARGET_KEEP)
            if (Creature* kept = map->GetCreature(group.Route().target))
                if (Creature* other = map->GetCreature(guardOtherGuid))
                    if (guardKeepLogged.insert({group.Run().runSeq, kept->GetGUID(), other->GetGUID()}).second)
                        group.LogEvent("guard_target_keep",
                                 JsonObject().Str("name", kept->GetName()).Num("entry", kept->GetEntry())
                                     .Real("score", bestScore + GUARD_TARGET_KEEP).Str("over", other->GetName())
                                     .Num("overEntry", other->GetEntry()).Real("overScore", guardOtherScore)
                                     .Real("keep", GUARD_TARGET_KEEP));

        // Far trash (pass 3, ROUTE_CHAIN_GUARD): a non-boss best target over FAR_TARGET_PATH yd of path gives way to
        // a nearer valid one (the boss-route weight can make far trash next to the boss score best), and below to a
        // pending starter. Taken only when nothing else is there.
        bool bestFar = ROUTE_CHAIN_GUARD && !bestGuid.IsEmpty() && !bestIsBoss && !bestObjective &&
                       bestLength > FAR_TARGET_PATH;
        if (bestFar && !nearGuid.IsEmpty())
        {
            bestGuid = nearGuid;
            bestScore = nearScore;
            bestIsBoss = nearIsBoss;
            bestObjective = nearObjective;
            bestFar = false;
        }
        // Far trash gives way to the walk-in event spot the next boss waits behind (WalkToEventSpot, nothing chosen).
        if (leadSpot && bestFar)
        {
            bestGuid.Clear();
            bestFar = false;
        }

        // [lead204] the pick follows the leader's plan: its next stop over the score's best, unless the best is a
        // fight on, a boss other than the plan's end, or an objective. A next stop the route refused from here (a
        // detour, the far side of a link) is planned again without it.
        if (!planNext.IsEmpty())
        {
            Creature* const best = bestGuid.IsEmpty() ? nullptr : map->GetCreature(bestGuid);
            bool const bestYields =
                !best || (!best->IsInCombat() && !bestObjective && (!bestIsBoss || best == nextBoss));
            if (!planScored)
            {
                group.Route().planRefusedFrom[planNext] = leader->GetPosition();
                if (Creature const* refused = map->GetCreature(planNext))
                    group.LogEvent("route_plan_refused", JsonObject()
                                                             .Str("stop", refused->GetName())
                                                             .Str("why", "the route does not take it from here"));
            }
            else if (bestYields && bestGuid != planNext)
            {
                if (Creature const* planned = map->GetCreature(planNext); planned && planNext != group.Route().target)
                    group.LogEvent("route_plan_pick", JsonObject()
                                                          .Str("pick", planned->GetName())
                                                          .Real("score", planScore)
                                                          .Str("over", best ? best->GetName() : std::string("none"))
                                                          .Real("overScore", bestScore)
                                                          .Str("step", planStep));
                bestGuid = planNext;
                bestScore = planScore;
                bestIsBoss = false;
                bestObjective = false;
                bestLength = planLength;
                bestFar = false;
            }
        }

        // Event starters: a priority one comes before any trash, the others only when nothing hostile is left.
        // ROUTE_DOOR_STARTER_FIRST: while the next boss is skipped "behind a closed door", every usable starter of the
        // map counts as a priority one - the door is the event's (wave 21 hos-n: Tribunal done at 1818 s, Brann's two
        // gossips that open Sjonnir's door 191296 waited for "nothing hostile left", 1895 s and ~2030 s, the group
        // walked the halls 216 s without a fight).
        bool nextBossDoorBlocked = false;
        // [queue-door-starter-match] the doors the reasons name ("next boss behind a closed door (<name> <entry>)"):
        // a key starter of ANOTHER door is no priority for them. strat-n 20261025-163739-strat-n-5A-12: Timmy and
        // Willey "behind a closed door (King's Square Gate 175352)", the Gauntlet Gate 175357 key 215 yd away was
        // promoted as well and won every other retarget - the target flipped 175352 <-> 175357 every 4 s from 1241 s,
        // the 175352 key gather never completed (451 s idle).
        std::set<uint32> blockingDoors;
        if (ROUTE_DOOR_STARTER_FIRST)
            for (auto const& [guid, reason] : group.Route().skipReason)
            {
                if (!group.Route().skipUntil.count(guid) || reason.rfind("next boss behind a closed door", 0) != 0)
                    continue;
                nextBossDoorBlocked = true;
                size_t const close = reason.rfind(')');
                size_t const space = close == std::string::npos ? std::string::npos : reason.rfind(' ', close);
                if (space != std::string::npos && space + 1 < close)
                    if (uint32 const entry =
                            uint32(std::strtoul(reason.substr(space + 1, close - space - 1).c_str(), nullptr, 10)))
                        blockingDoors.insert(entry);
            }
        // [b35_brd2] ... and the closed doors in front of them (a key door chain). BRD heroic 2026-10-26
        // (20261026-024524-brd-h-5G-5): Lord Incendius "behind a closed door (Shadowforge Gate 170560)" promoted only
        // the Gate's key starter, whose walk ran through the closed East Garrison Door 170570 ("starter_blocked"); the
        // Garrison Door's own key starter was "a key for another door", never promoted, and lost to trash every time
        // - at 1519-1565 s the group stood 20 yd from that door with the Shadowforge Key in the tank's bags.
        auto& chainDoors = keyDoorChain[group.InstanceId()];
        if (nextBossDoorBlocked && !blockingDoors.empty())
            for (auto const& [entry, seenMs] : chainDoors)
                if (getMSTimeDiff(seenMs, now) <= KEY_DOOR_CHAIN_FRESH_MS)
                    blockingDoors.insert(entry);
        int32 starterIdx = -1;
        float starterScore = 0.0f;
        bool starterPriority = false;
        bool starterDoorPromoted = false;
        // [b32_st] Sunken Temple: the Shade of Hakkar event failed (the Shade despawned at 25 Suppressions, 8440 id
        // 9-10, or the Avatar 8443 stood 10 s out of combat, 8443 id 0-1: instance data 2 back to NOT_STARTED). The
        // Egg of Hakkar 10465 stays in the bags (no charges), the Eternal Flames relight (148418-148421 id 0,
        // conditions 22/1 GetData(2) == NOT_STARTED) and the doors 149432 / 149433 open again: a group starts over, as
        // players do. The egg and the four flame starters are armed again (at most ST_HAKKAR_REARM_MAX times).
        if (map->GetId() == 109)
        {
            constexpr uint32 ST_EGG_OF_HAKKAR = 10465;
            constexpr uint32 ST_HAKKARI_BLOOD = 10460;
            constexpr uint32 ST_TYPE_HAKKAR_EVENT = 2;  // sunken_temple.h TYPE_HAKKAR_EVENT
            constexpr uint32 ST_HAKKAR_REARM_GAP_MS = 15 * IN_MILLISECONDS;  // the egg's SendEvent sets IN_PROGRESS
            constexpr uint32 ST_HAKKAR_REARM_MAX = 4;
            static std::unordered_map<uint32, uint32> stHakkarRearms;  // instance id -> re-arms (world thread only)
            InstanceScript* const stScript = leader->GetInstanceScript();
            StarterTrack* egg = nullptr;
            for (StarterTrack& starter : group.Route().starters)
                if (starter.def->itemId == ST_EGG_OF_HAKKAR)
                    egg = &starter;
            uint32& rearms = stHakkarRearms[group.InstanceId()];
            if (stScript && egg && egg->uses >= egg->def->maxUses && egg->lastUseMs &&
                getMSTimeDiff(egg->lastUseMs, now) > ST_HAKKAR_REARM_GAP_MS && rearms < ST_HAKKAR_REARM_MAX &&
                stScript->GetData(ST_TYPE_HAKKAR_EVENT) == NOT_STARTED && !leader->IsInCombat() &&
                !leader->FindNearestCreature(8440 /* Shade of Hakkar */, 250.0f, true) &&
                !leader->FindNearestCreature(8443 /* Avatar of Hakkar */, 250.0f, true))
            {
                ++rearms;
                uint32 flames = 0;
                for (StarterTrack& starter : group.Route().starters)
                {
                    if (starter.def->itemId != ST_EGG_OF_HAKKAR && starter.def->itemId != ST_HAKKARI_BLOOD)
                        continue;
                    if (starter.def->itemId == ST_HAKKARI_BLOOD && starter.uses >= starter.def->maxUses)
                        ++flames;
                    starter.uses = 0;
                    starter.usedObjects.clear();
                    starter.failedUses.clear();
                    starter.steppedOut.clear();
                    starter.nextAllowedMs = now;
                }
                group.Route().lastEvent = "event reset: Sunken Temple Shade of Hakkar";
                group.LogEvent("starter_rearmed",
                         JsonObject().Str("note", egg->def->note)
                             .Str("why", "the Shade of Hakkar event reset (instance data 2 NOT_STARTED): egg and "
                                         "Eternal Flames armed again [b32_st]")
                             .Num("flamesLitBefore", flames)
                             .Num("bloodInBags", int64(leader->GetItemCount(ST_HAKKARI_BLOOD)))
                             .Num("rearms", rearms));
            }
            // The egg's item cooldown (item_template 10465 spellcooldown_1 300 s) is the wait before the retry
            // (UseStarterItem: nextAllowedMs): a re-armed egg waiting on it is no stall. Bounded by the cooldown.
            if (egg && rearms && egg->uses < egg->def->maxUses && leader->HasSpellCooldown(12346 /* Egg of Hakkar */))
                group.NoteProgress(now);
        }
        for (size_t index = 0; index < group.Route().starters.size(); ++index)
        {
            StarterTrack& starter = group.Route().starters[index];
            // A wipe reset the event (Halls of Reflection wave 10: Falric dead, the group wiped on Marwyn's waves,
            // Jaina offered "Can you remove the sword?" again, the used-up starter never ran again - "partial: no
            // reachable targets left"). Used up before the last wipe and its creature offers the gossip again:
            // armed again (ROUTE_STARTER_REARM).
            if (ROUTE_STARTER_REARM && starter.uses >= starter.def->maxUses && group.Wipes() > starter.wipesAtUse &&
                (starter.def->kind == StarterKind::Gossip || starter.def->kind == StarterKind::Escort))
                for (Creature* creature : candidates)
                    if (creature && creature->IsInWorld() && creature->IsAlive() &&
                        std::find(starter.def->entries.begin(), starter.def->entries.end(), creature->GetEntry()) !=
                            starter.def->entries.end() &&
                        creature->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
                    {
                        group.LogEvent("starter_rearmed", JsonObject().Str("note", starter.def->note)
                                                               .Num("entry", creature->GetEntry())
                                                               .Num("usesBefore", starter.uses)
                                                               .Num("wipes", group.Wipes()));
                        starter.uses = 0;
                        starter.wipesAtUse = group.Wipes();
                        starter.nextAllowedMs = now;
                        group.Route().eventGatherArrived = false;
                        group.Route().eventGatherLogged = false;
                        break;
                    }
            // Stratholme heroic wave 9: the Key to the City opened the Service Entrance Gate for its 3 s auto-close,
            // the tank passed, the gate shut behind it and the followers stood at it ("runs through Service Entrance
            // Gate 175368 at 2.1 yd -> stay") while the used-up starter never came again.
            if (starter.def->kind == StarterKind::UseItem && starter.uses >= starter.def->maxUses &&
                starter.doorRearms < DOOR_REARM_MAX && group.Route().lastStarterUseMs &&
                getMSTimeDiff(group.Route().lastStarterUseMs, now) > DOOR_REARM_GAP_MS &&
                leader->HasItemCount(starter.def->itemId, 1) && !leader->IsInCombat())
                for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                {
                    if (!go || !go->isSpawned() || go->GetGoType() != GAMEOBJECT_TYPE_DOOR ||
                        go->GetGoState() != GO_STATE_READY ||
                        std::find(starter.def->entries.begin(), starter.def->entries.end(), go->GetEntry()) ==
                            starter.def->entries.end())
                        continue;
                    std::vector<GameObject*> const door = {go};
                    Player* behind = nullptr;
                    for (LeaderGroup::MemberInfo const& member : group.Members())
                        if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                            bot && bot != leader && bot->IsAlive() && bot->GetMap() == map && !behind)
                        {
                            PathResult const way = RoutePath(bot, leader->GetPositionX(), leader->GetPositionY(),
                                                             leader->GetPositionZ());
                            if (way.reachable && RouteThroughClosedDoor(way.points, door, leader->GetPhaseMask()))
                                behind = bot;
                        }
                    // Or the whole group is still on the near side and the leader's own walk holds at the door
                    // (door_hold, SetLeaderWaypoint): strat-h build 14 - the key at 50 s, a fight on the entrance
                    // side, the gate shut after 3 s with all five outside, 0 starter_rearmed, 388 "route through a
                    // closed door" skips and the stall at 338 s.
                    // [b32_strat_gate] not when the whole group is through it already (GroupThroughKeyGate)
                    bool const leaderHeld = !behind && group.Route().doorHold == go->GetGUID() &&
                        group.Route().doorHoldMs &&
                                            getMSTimeDiff(group.Route().doorHoldMs, now) <= KEY_DOOR_HOLD_FRESH_MS &&
                                            !GroupThroughKeyGate(group, go);
                    if (!behind && !leaderHeld)
                        continue;
                    ++starter.doorRearms;
                    starter.uses = 0;
                    starter.usedObjects.clear();
                    starter.nextAllowedMs = now;
                    starter.rearmedBehindMs = behind ? (now ? now : 1) : 0;
                    group.LogEvent("starter_rearmed",
                             JsonObject().Str("note", starter.def->note)
                                 .Str("why", behind ? "the door closed again, " + std::string(behind->GetName()) +
                                                          " behind it"
                                                    : std::string("the door closed again, the leader's walk holds at "
                                                                  "it (the whole group on this side)"))
                                 .Num("rearms", starter.doorRearms));
                    break;
                }
            // [dme-ironbark-door] Used up, but its door is still closed STARTER_DOOR_CHECK_MS after the use and the
            // creature offers its gossip again (Ironbark's gossip select clears his npcflag, so a done select never
            // re-arms; a hello the core refused does).
            if (starter.def->opensDoor && starter.uses >= starter.def->maxUses &&
                starter.doorRearms2 < STARTER_DOOR_REARM_MAX && starter.lastUseMs &&
                getMSTimeDiff(starter.lastUseMs, now) > STARTER_DOOR_CHECK_MS)
            {
                bool doorClosed = false;
                for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                    doorClosed |= go && go->isSpawned() && go->GetEntry() == starter.def->opensDoor &&
                                  go->GetGoState() == GO_STATE_READY;
                bool offers = false;
                for (Creature* creature : candidates)
                    offers |= creature && creature->IsAlive() && creature->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) &&
                              std::find(starter.def->entries.begin(), starter.def->entries.end(),
                                        creature->GetEntry()) != starter.def->entries.end();
                if (doorClosed && offers)
                {
                    ++starter.doorRearms2;
                    starter.uses = 0;
                    starter.nextAllowedMs = now;
                    group.LogEvent("starter_door_still_closed", JsonObject().Str("note", starter.def->note)
                                                                     .Num("door", starter.def->opensDoor)
                                                                     .Num("rearms", starter.doorRearms2));
                }
            }
            if (starter.uses < starter.def->maxUses && !starter.def->doneWhen.empty())
            {
                bool past = false;
                for (uint32 entry : starter.def->doneWhen)
                    past |= group.Route().deadEntries.count(entry) != 0;
                // [ulda-altar-confirm] doneWhenAuraAbsent: a living one counts only awake (fighting, or without it)
                uint32 const sleepAura = starter.def->doneWhenAuraAbsent;
                for (Creature* creature : candidates)
                    past |= creature && creature->IsInWorld() && creature->IsAlive() &&
                            std::find(starter.def->doneWhen.begin(), starter.def->doneWhen.end(),
                                      creature->GetEntry()) != starter.def->doneWhen.end() &&
                            (!sleepAura || creature->IsInCombat() || !creature->HasAura(sleepAura));
                if (!past && sleepAura && starter.uses > starter.unconfirmedLogged &&
                    int32(now - starter.nextAllowedMs) >= 0)
                {
                    starter.unconfirmedLogged = starter.uses;
                    group.LogEvent("starter_unconfirmed", JsonObject().Str("note", starter.def->note)
                                                               .Num("uses", starter.uses)
                                                               .Num("maxUses", starter.def->maxUses));
                }
                if (past)
                {
                    group.LogEvent("starter_done_by_event",
                             JsonObject().Str("note", starter.def->note).Num("usesBefore", starter.uses));
                    starter.uses = starter.def->maxUses;
                }
            }
            if (starter.uses >= starter.def->maxUses || int32(now - starter.nextAllowedMs) < 0)
                continue;
            if (starter.def->skipContent && strcmp(starter.def->skipContent, group.ContentKey().c_str()) == 0)
                continue;
            // [occ-portal-gate] Oculus: the Nexus Portal 189985 (255 uses: the group may come back through it, its
            // cooldown 60 s) is clicked only by a leader standing on the entrance island while the bots' own
            // "oc nexus portal" step does not take it already - never a click loop from Drakos' island.
            if (map->GetId() == OCULUS_MAP_ID &&
                std::find(starter.def->entries.begin(), starter.def->entries.end(), OCIds::GO_NEXUS_PORTAL) !=
                    starter.def->entries.end())
            {
                bool ownStep = false;
                if (PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader))
                    if (Trigger* portalStep = leaderAI->GetAiObjectContext()->GetTrigger("oc nexus portal"))
                        ownStep = portalStep->IsActive();
                if (!OCRoute::IsOnEntranceIsland(leader) || ownStep)
                    continue;
            }

            // Event order (the bosses before it are dead) and the item a real group would carry.
            if (!EntriesDead(group, starter.def->afterDead) || !StartersUsed(group, starter.def->afterUsed) ||
                !ScriptBossDone(script, starter.def->afterBossDone))
                continue;
            if (starter.def->kind == StarterKind::UseItem && !leader->HasItemCount(starter.def->itemId, 1))
                continue;
            // [brd_bar_turnin] the quest's items in the bags first (the VendorBuy step before it buys them)
            if (starter.def->kind == StarterKind::QuestTurnIn && !QuestItemsReady(leader, starter.def))
                continue;

            WorldObject* object = nullptr;
            PathResult path;
            bool pathKnown = false;
            if (CreatureStarterKind(starter.def->kind))
            {
                // Not while it walks a scripted path (WalksScriptedPath, ROUTE_STARTER_NOT_WALKING): the group waits
                // until it stands (Zul'Farrak: Bly's crew on the way to the stairs or to the gate).
                for (Creature* creature : candidates)
                    if (creature && creature->IsAlive() &&
                        std::find(starter.def->entries.begin(), starter.def->entries.end(), creature->GetEntry()) !=
                            starter.def->entries.end() &&
                        creature->HasNpcFlag(StarterNpcFlag(starter.def->kind)) &&
                        !(ROUTE_STARTER_NOT_WALKING && WalksScriptedPath(creature)) &&
                        !GossipStarterHeld(leader, creature))
                    {
                        object = creature;
                        break;
                    }
            }
            else
            {
                // An item is used on its object whatever the click flags say (the spell targets it); a click needs a
                // selectable object, and a locked one the key or skill a member really has.
                // The nearest one of several (Old Hillsbrad: 15 barrels, five are used): the first of the spawn store
                // was 180 yd away through Durnholde while one stood 70 yd from the leader (wave 9: stalled 9 min).
                bool const byItem = starter.def->kind == StarterKind::UseItem;
                // [b32_strat_gate] a closed key door right by the leader is used from the leader's own side: no walk to
                // its centre is needed (the walk through it is what the path test and the stuck skip judge).
                // strat-n 20261025-221216-strat-n-5H-2: the tank 4.9 yd inside the shut Service Entrance Gate, the
                // walk to its centre stuck in the door_hold, steer_stuck, "no usable object" at 119 s - the healer
                // outside for good.
                auto const adjacentKeyDoor = [&](GameObject const* go)
                {
                    return byItem && go->GetGoType() == GAMEOBJECT_TYPE_DOOR && go->GetGoState() == GO_STATE_READY &&
                           go->IsWithinDistInMap(leader, KEY_DOOR_HELD_RANGE) &&
                           leader->IsWithinLOS(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ() + 1.5f,
                                               VMAP::ModelIgnoreFlags::M2, LINEOFSIGHT_CHECK_VMAP);
                };
                std::vector<std::pair<float, GameObject*>> objects;
                for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                    if (go && go->isSpawned() && !starter.usedObjects.count(go->GetGUID()) &&
                        std::find(starter.def->entries.begin(), starter.def->entries.end(), go->GetEntry()) !=
                            starter.def->entries.end() &&
                        !(byItem && StarterObjectStuck(starter, go->GetGUID(), leader) && !adjacentKeyDoor(go)) &&
                        // [b32_strat_gate] a re-armed key gate the whole group is through: no walk back to key it
                        !(byItem && starter.doorRearms && GroupThroughKeyGate(group, go)) &&
                        (byItem || (!go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) &&
                                    !go->HasGameObjectFlag(GO_FLAG_IN_USE) && CanOpenLock(group, go))))
                        objects.emplace_back(leader->GetExactDist(go), go);
                std::sort(objects.begin(), objects.end(),
                          [](auto const& left, auto const& right) { return left.first < right.first; });

                // Of several objects (Old Hillsbrad: 15 barrels, five are used) one whose walk ends at it
                // (StarterPathArrives) before the nearest one a partial walk only "nears": sweep 2026-10-24 21:50, the
                // walks to the nearest barrels (2213,247 / 2226,251, z 53.6 in the lodges) ended 38-57 yd off on the
                // Durnholde wall at z 64.9, the leader stood 9-11 yd above them, steer_stuck one after another -
                // first barrel at 1272 s (ohb-n), none in ohb-h. The STARTER_PATH_CANDIDATES nearest are measured.
                uint32 measured = 0;
                for (auto const& [dist, go] : objects)
                {
                    // From afar (the instance entrance) every walk is a long search that only nears: the nearest one.
                    if (measured >= STARTER_PATH_CANDIDATES || (object && dist > STARTER_PATH_SORT_RANGE))
                        break;
                    ++measured;
                    // [b32_strat_gate] an adjacent closed key door: arrived (a path of the leader's own spot).
                    if (adjacentKeyDoor(go))
                    {
                        G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(),
                                                leader->GetPositionZ());
                        object = go;
                        path = PathResult();
                        path.reachable = true;
                        path.complete = true;
                        path.points = {here, here};
                        path.end = here;
                        pathKnown = true;
                        break;
                    }
                    PathResult const candidate = EvaluatePath(leader, go);
                    if (!candidate.reachable)
                        continue;
                    bool const arrives = StarterPathArrives(candidate, go);
                    if (!object || arrives)
                    {
                        object = go;
                        path = candidate;
                        pathKnown = true;
                    }
                    if (arrives)
                        break;
                }
            }

            // [dme-starter-blocked] a priority step whose order holds but that cannot be done says why (dme-h 012157:
            // Ironbark never chosen, not one starter line in 345 s of "next boss behind a closed door").
            auto const blocked = [&](std::string const& why)
            {
                if ((starter.def->priority || nextBossDoorBlocked) && starter.blockedLogged != why)
                {
                    starter.blockedLogged = why;
                    group.LogEvent("starter_blocked", JsonObject().Str("note", starter.def->note).Str("why", why));
                }
            };
            if (!object)
            {
                std::string why = "no usable object";
                if (CreatureStarterKind(starter.def->kind))
                {
                    uint32 seen = 0, noFlag = 0, walking = 0, hidden = 0, held = 0;
                    for (Creature* creature : candidates)
                        if (creature && creature->IsAlive() &&
                            std::find(starter.def->entries.begin(), starter.def->entries.end(),
                                      creature->GetEntry()) != starter.def->entries.end())
                        {
                            ++seen;
                            noFlag += !creature->HasNpcFlag(StarterNpcFlag(starter.def->kind));
                            walking += WalksScriptedPath(creature);
                            hidden += !creature->IsVisible();
                            held += GossipStarterHeld(leader, creature);
                        }
                    why = Acore::StringFormat(
                        "creature: {} loaded, {} without the npc flag, {} walking, {} invisible, {} held", seen, noFlag,
                        walking, hidden, held);
                }
                blocked(why);
                continue;
            }
            // [sm-chapel-wing] not from another wing
            if (starter.def->sameWingOnly && !inCurrentWing(object))
            {
                blocked("in another wing");
                continue;
            }
            // [ulda-altar-clear] a ritual / channel step only out of combat with nothing attackable around the object
            if (starter.def->clearRadius > 0.0f)
            {
                Creature* nearMob = nullptr;
                for (Creature* creature : candidates)
                    if (creature && creature->IsInWorld() && creature->IsAlive() && !creature->IsCritter() &&
                        creature->IsHostileTo(leader) && leader->IsValidAttackTarget(creature) &&
                        std::find(starter.def->doneWhen.begin(), starter.def->doneWhen.end(), creature->GetEntry()) ==
                            starter.def->doneWhen.end() &&
                        creature->GetExactDist(object) <= starter.def->clearRadius &&
                        // [b30_stalls] what stands behind a closed door is not around the object (Blood Furnace:
                        // the Shadowmoon Warlock / Hellfire Imps behind Broggok's rear door 181819, 49 yd from the
                        // lever); only the creatures within the radius pay for a path
                        (closedDoors.empty() || creature->IsInCombat() ||
                         !RouteThroughClosedDoor(EvaluatePath(leader, creature).points, closedDoors,
                                                 leader->GetPhaseMask())))
                    {
                        nearMob = creature;
                        break;
                    }
                if (nearMob || leader->IsInCombat() || group.InCombat())
                {
                    if (!starter.clearLogged)
                    {
                        starter.clearLogged = true;
                        group.LogEvent("starter_wait_clear",
                                 JsonObject().Str("note", starter.def->note)
                                     .Str("mob", nearMob ? nearMob->GetName() : "")
                                     .Bool("inCombat", leader->IsInCombat() || group.InCombat())
                                     .Real("radius", starter.def->clearRadius));
                    }
                    continue;
                }
                starter.clearLogged = false;
            }

            if (!pathKnown)
                path = EvaluatePath(leader, object);
            if (!path.reachable)
            {
                blocked(Acore::StringFormat("no walk to {} ({:.0f} yd)", object->GetName(),
                                            leader->GetExactDist(object)));
                continue;
            }
            // A walk that only nears the object says so once (Old Hillsbrad 2026-10-25: every barrel walk ended on the
            // Durnholde wall 11 yd above it, 25 minutes without one starter line).
            // [event_boss_classic] over the route link first (viaStarterLink): the leader walks it with no target
            if (viaStarterLink && !StarterPathArrives(path, object))
            {
                blocked("over a route link first");
                continue;
            }
            if (!StarterPathArrives(path, object) && starter.partialLogged != object->GetGUID())
            {
                starter.partialLogged = object->GetGUID();
                group.LogEvent("starter_partial_walk",
                         JsonObject().Str("note", starter.def->note).Num("entry", object->GetEntry())
                             .Real("endLeft", object->GetExactDist(path.end.x, path.end.y, path.end.z))
                             .Real("endDz", path.end.z - object->GetPositionZ()).Bool("longRoute", path.longRoute)
                             .Real("x", path.end.x).Real("y", path.end.y).Real("z", path.end.z));
            }
            // ROUTE_CLOSED_DOORS_ALL: not through a closed door the starter does not open (the Viewing Room Door
            // itself is the object of its key: no barrier for its own use).
            // [door_hold_pass] EventStarterDef::passDoors are no barrier for this starter (Gnomeregan: Emi's Cave Ins).
            if (ROUTE_CLOSED_DOORS_ALL && !closedDoors.empty())
            {
                std::vector<GameObject*> barriers = DoorsClosedFor(closedDoors, object);
                barriers.erase(std::remove_if(barriers.begin(), barriers.end(),
                                              [&](GameObject* door)
                                              {
                                                  return std::find(starter.def->passDoors.begin(),
                                                                   starter.def->passDoors.end(),
                                                                   door->GetEntry()) != starter.def->passDoors.end();
                                              }),
                               barriers.end());
                // [b44_strat] ...unless the walk goes round that door (a passage door, PassageDoorBoss). [b50_strat]
                // A key gate is no passage door (user rule 2026-10-26: the key is there, the gate is there - it is
                // opened): a key starter whose walk runs through another key gate is blocked here and that gate's own
                // key joins the chain ([b35_brd2] key_door_chain).
                GameObject* door = RouteThroughClosedDoor(path.points, barriers, leader->GetPhaseMask());
                if (door && GoesRoundPassageDoor(leader, script, object, door, barriers))
                    door = nullptr;
                if (door)
                {
                    blocked(Acore::StringFormat("walk runs through closed {} {}", door->GetName(), door->GetEntry()));
                    // [b35_brd2] a key starter of a blocking door (or of a chained one) whose walk runs through
                    // another closed door: that door joins the chain, its own key starter is promoted
                    if (nextBossDoorBlocked && starter.def->kind == StarterKind::UseItem)
                    {
                        bool forBlocking = false;
                        for (uint32 const entry : starter.def->entries)
                            forBlocking |= blockingDoors.count(entry) != 0;
                        if (forBlocking && !blockingDoors.count(door->GetEntry()))
                        {
                            bool const fresh = !chainDoors.count(door->GetEntry()) ||
                                               getMSTimeDiff(chainDoors[door->GetEntry()], now) >
                                                   KEY_DOOR_CHAIN_FRESH_MS;
                            if (fresh)
                                group.LogEvent("key_door_chain",
                                         JsonObject().Str("note", starter.def->note)
                                             .Str("door", door->GetName()).Num("doorEntry", door->GetEntry())
                                             .Str("action", "its key starter counts for the blocking door "
                                                            "[b35_brd2]"));
                            blockingDoors.insert(door->GetEntry());
                        }
                        if (forBlocking)
                            chainDoors[door->GetEntry()] = now ? now : 1;
                    }
                    continue;
                }
            }
            if (!starter.blockedLogged.empty())
                starter.blockedLogged.clear();

            // [zf-gong-priority] EventStarterDef::priorityItem in the leader's bags
            // [queue-rearm-behind-priority] a key re-armed for a member stranded behind its door leads: strat-h
            // 20261025-115805-strat-h-5A-35 re-armed the King's Square Gate 175353 key at 1313 s ("Rtdruap behind
            // it"), never used it again - the Courtyard packs led, the guarantee held them as "not topped up" with
            // the healer and the mage behind the gate, Timmy never spawned, stalled at 1760 s.
            constexpr uint32 REARM_BEHIND_PRIORITY_MS = 3 * MINUTE * IN_MILLISECONDS;
            bool const rearmedBehind = starter.rearmedBehindMs && starter.uses < starter.def->maxUses &&
                                       getMSTimeDiff(starter.rearmedBehindMs, now) < REARM_BEHIND_PRIORITY_MS;
            // [queue-door-starter-match] a key (UseItem) for a door none of the blocking doors is: not promoted
            bool keyForOtherDoor = false;
            if (nextBossDoorBlocked && !blockingDoors.empty() && starter.def->kind == StarterKind::UseItem)
            {
                keyForOtherDoor = true;
                for (uint32 const entry : starter.def->entries)
                    keyForOtherDoor &= !blockingDoors.count(entry);
            }
            // [lead11] the door the leader's plan must open first (TickLeaderPlanner: doorStepEntry)
            bool const doorStep =
                group.State().doorStepEntry && group.State().doorStepMs &&
                getMSTimeDiff(group.State().doorStepMs, now) <= Leader::LEADER_REQUEST_TTL_MS &&
                std::find(starter.def->entries.begin(), starter.def->entries.end(), group.State().doorStepEntry) !=
                    starter.def->entries.end();
            bool const priority = starter.def->priority || (nextBossDoorBlocked && !keyForOtherDoor) ||
                                  rearmedBehind || doorStep ||
                                  (starter.def->priorityItem && leader->HasItemCount(starter.def->priorityItem, 1));
            if (starterIdx < 0 || (priority && !starterPriority) ||
                (priority == starterPriority && path.length < starterScore))
            {
                starterIdx = int32(index);
                starterScore = path.length;
                starterPriority = priority;
                starterDoorPromoted = priority && !starter.def->priority;
                group.Route().starterTargets[index] = object->GetGUID();
            }
        }
        if (starterDoorPromoted && starterIdx >= 0)
        {
            uint32& loggedMs = doorStarterLogMs[group.InstanceId()];
            if (!loggedMs || getMSTimeDiff(loggedMs, now) > DOOR_STARTER_LOG_MS)
            {
                loggedMs = now ? now : 1;
                char const* const note = group.Route().starters[size_t(starterIdx)].def->note;
                LOG_INFO("playerbots", "[route] {}: starter_door_priority: next boss behind a closed door - {} first",
                         group.Run().runId, note);
                group.LogEvent("starter_door_priority", JsonObject().Str("note", note).Real("distance", starterScore));
            }
        }

        // No starter while a scripted intro runs (Utgarde Pinnacle: Gortok's Stasis Generator is the next starter of
        // the map, the group stays for Svala); a walk-in event spot comes before the non-priority ones.
        // [brd_ring_lead] ...and before a starter that is priority only because a later boss is "behind a closed door"
        // (starterDoorPromoted): 2026-10-25 brd-h / brd-n, Grebmar dead at 1479 / 1143 s under the arena, the Ring spot
        // led (order 3) but Loregrain (order 4, behind ArenaGate03, which only the Ring's DONE opens) promoted the
        // Shadowforge Gate keys; the group walked north through both gates and then met the arena from its closed
        // exit side (door_hold ArenaGate03 until the stop, zero brd_ring_* lines in 62 min).
        bool const leadSpotWins = leadSpot && (!starterPriority || starterDoorPromoted);
        if (leadSpotWins && starterIdx >= 0 && starterDoorPromoted)
        {
            uint32& loggedMs = leadSpotFirstLogMs[group.InstanceId()];
            if (!loggedMs || getMSTimeDiff(loggedMs, now) > DOOR_STARTER_LOG_MS)
            {
                loggedMs = now ? now : 1;
                group.LogEvent("brd_ring_lead",
                         JsonObject().Str("spot", leadSpot->note)
                             .Str("deferred", group.Route().starters[size_t(starterIdx)].def->note)
                             .Real("spotDistance", leadSpotDistance));
            }
        }
        if (introBoss || leadSpotWins || eventHold)
            starterIdx = -1;
        bool useStarter = starterIdx >= 0 && (starterPriority || bestGuid.IsEmpty() || bestFar);
        // [b32_st] A hold spot's guard out of combat at the spot comes before any starter, priority ones too: the
        // Nightmare Suppressor 8497 adds 1 to the Shade of Hakkar's counter 2 every second it stands idle within 40
        // yd (smart_scripts 8497 id 5, after 4 s), 25 in all despawn the Shade (8440 id 9-10, instance data 2
        // NOT_STARTED). st-h 20261025-221216-st-h-5G-8: the Hakkari Blood in the bags made Eternal Flame 1/4 (a
        // priority starter) the target from 3668 s; the Suppressor spawned at 3671 s was chosen only at 3703 s, the
        // Shade was gone before the second Bloodkeeper (+70 s) - one flame lit, then nothing for 4 min (st-n / st-h
        // 163739 / 184229 / 221216 alike: one blood, one flame, no second Bloodkeeper).
        if (useStarter && holdSpot && !bestGuid.IsEmpty())
            if (Creature const* guard = map->GetCreature(bestGuid);
                guard && guard->IsAlive() && !guard->IsInCombat() && AtHoldGuard(*holdSpot, guard))
                useStarter = false;
        ObjectGuid chosen = useStarter ? group.Route().starterTargets[size_t(starterIdx)] : bestGuid;

        // [b30_broggok] The Blood Furnace Cell Door Lever 181982 is Broggok's pull (boss_area_clear's rule): the idle
        // trash of the cell room comes first. bf-n 20261025-184229-bf-n-5G-44: the priority lever was used at 838 s
        // with Shadowmoon Technicians still standing between the cells (452-483, 91-103); the prisoner waves dragged
        // the group into them, two deaths to Technicians before Broggok came out, he was left at 4.8 % and the run
        // ended "boss attempt loop". The build-29 clean runs had cleared that trash on the way by route order only.
        // The prisoners are not attackable until their cell opens (instance_blood_furnace ResetPrisoner): left out.
        if (useStarter && map->GetId() == 542)
        {
            std::vector<uint32> const& leverEntries = group.Route().starters[size_t(starterIdx)].def->entries;
            if (std::find(leverEntries.begin(), leverEntries.end(), 181982u) != leverEntries.end())
            {
                Creature* roomMob = nullptr;
                for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
                {
                    if (!creature || !creature->IsAlive() || creature->IsInCombat() || creature->IsSummon() ||
                        creature->IsDungeonBoss() || !creature->IsHostileTo(leader) ||
                        creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) || creature->IsImmuneToPC())
                        continue;
                    float const x = creature->GetPositionX();
                    float const y = creature->GetPositionY();
                    if (x < 400.0f || x > 510.0f || y < 45.0f || y > 130.0f ||
                        std::fabs(creature->GetPositionZ() - 9.6f) > 6.0f)
                        continue;
                    auto const skipped = group.Route().skipUntil.find(creature->GetGUID());
                    if (skipped != group.Route().skipUntil.end() && int32(skipped->second - now) > 0)
                    {
                        auto const why = group.Route().skipReason.find(creature->GetGUID());
                        if (why != group.Route().skipReason.end() && ReachabilitySkip(why->second))
                            continue;
                    }
                    if (!roomMob || leader->GetExactDist(creature) < leader->GetExactDist(roomMob))
                        roomMob = creature;
                }
                if (roomMob)
                {
                    if (roomMob->GetGUID() != group.Route().target)
                        group.LogEvent("starter_area_clear",
                                 JsonObject().Str("starter", "Cell Door Lever").Str("first", roomMob->GetName())
                                     .Num("entry", roomMob->GetEntry())
                                     .Real("distance", leader->GetExactDist(roomMob)));
                    chosen = roomMob->GetGUID();
                    useStarter = false;
                    bestIsBoss = false;
                    bestObjective = false;
                }
            }
        }

        // [b30_mech_pos] The Gong of Zul'Farrak 141832 is Gahz'rilla's pull: the idle Sandfury round the gong and
        // round his fight spot (smart_scripts 7273 id 0: he walks to 1696.9,1216.6,8.9) come first. zf-h
        // 20261025-184229-zf-h-5E-62: gong at 1110 s with ~20 Sandfury still standing 17-50 yd round that spot; the
        // fight there woke 12 of them (Witch Doctors' Flash Heal / Healing Ward, Blood Drinkers' flee at 15 %): 287 s,
        // 0 kills, wipe. Same rule as the Cell Door Lever above.
        if (useStarter && map->GetId() == 209)
        {
            std::vector<uint32> const& gongEntries = group.Route().starters[size_t(starterIdx)].def->entries;
            if (std::find(gongEntries.begin(), gongEntries.end(), 141832u) != gongEntries.end())
            {
                constexpr float POOL_CLEAR_RADIUS = 45.0f;
                constexpr float POOL_CLEAR_DZ = 6.0f;
                Position const gahzSpot(1696.9f, 1216.6f, 8.9f);
                Position const gongSpot(1650.9f, 1171.9f, 10.9f);
                Creature* poolMob = nullptr;
                for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
                {
                    if (!creature || !creature->IsAlive() || creature->IsInCombat() || creature->IsSummon() ||
                        creature->IsDungeonBoss() || creature->IsCritter() || !creature->IsHostileTo(leader) ||
                        creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) || creature->IsImmuneToPC() ||
                        !leader->IsValidAttackTarget(creature))
                        continue;
                    bool const nearSpot =
                        creature->GetExactDist2d(&gahzSpot) <= POOL_CLEAR_RADIUS &&
                        std::fabs(creature->GetPositionZ() - gahzSpot.GetPositionZ()) <= POOL_CLEAR_DZ;
                    bool const nearGong =
                        creature->GetExactDist2d(&gongSpot) <= POOL_CLEAR_RADIUS &&
                        std::fabs(creature->GetPositionZ() - gongSpot.GetPositionZ()) <= POOL_CLEAR_DZ;
                    if (!nearSpot && !nearGong)
                        continue;
                    auto const skipped = group.Route().skipUntil.find(creature->GetGUID());
                    if (skipped != group.Route().skipUntil.end() && int32(skipped->second - now) > 0)
                    {
                        auto const why = group.Route().skipReason.find(creature->GetGUID());
                        if (why != group.Route().skipReason.end() && ReachabilitySkip(why->second))
                            continue;
                    }
                    if (!poolMob || leader->GetExactDist(creature) < leader->GetExactDist(poolMob))
                        poolMob = creature;
                }
                if (poolMob)
                {
                    if (poolMob->GetGUID() != group.Route().target)
                        group.LogEvent("starter_area_clear",
                                 JsonObject().Str("starter", "Gong of Zul'Farrak").Str("first", poolMob->GetName())
                                     .Num("entry", poolMob->GetEntry())
                                     .Real("distance", leader->GetExactDist(poolMob)));
                    chosen = poolMob->GetGUID();
                    useStarter = false;
                    bestIsBoss = false;
                    bestObjective = false;
                }
            }
        }

        // ROUTE_LIFT_REFUSED_SKIP: nothing is left, but a pack the tank refused is skipped (careful pull refused);
        // with ROUTE_SKIP_BEHIND_SKIPPED everything behind it is skipped too and the group stood until the 4-minute
        // stall (Shadow Labyrinth / Sethekk / Stratholme heroic 2026-10-25; some refusals are transient: "not topped
        // up" at 100 % / 100 %). The refused mob whose skip ends first is the target again: the leader walks to its
        // edge (ApproachStop), never into it, and the tank looks again - a pull, or one more refusal logged.
        if (chosen.IsEmpty() && ROUTE_DETOUR_FALLBACK && detours &&
            (!group.Route().detourFallbackUntilMs || int32(group.Route().detourFallbackUntilMs - now) <= 0))
        {
            group.Route().detourFallbackUntilMs = now + DETOUR_FALLBACK_MS;
            group.LogEvent("detour_fallback", JsonObject().Num("detours", detours)
                                                   .Num("sec", DETOUR_FALLBACK_MS / IN_MILLISECONDS));
            LOG_INFO("playerbots", "[route] {}: every target left was a detour ({}): the long way for {} s",
                     group.Run().runId, detours, DETOUR_FALLBACK_MS / IN_MILLISECONDS);
            group.Route().nextRetargetMs = now;
            return;
        }
        if (chosen.IsEmpty() && ROUTE_LIFT_REFUSED_SKIP && !escortNpc)
        {
            Creature* lift = nullptr;
            uint32 liftUntil = 0;
            for (auto const& [guid, until] : group.Route().skipUntil)
            {
                auto const why = group.Route().skipReason.find(guid);
                // [b43_harn] the "planner never planned it" skip is lifted as the refusal it replaced
                if (why == group.Route().skipReason.end() || (why->second.rfind("careful pull refused", 0) != 0 &&
                                                      why->second.rfind("planner never planned it", 0) != 0))
                    continue;
                auto const refused = group.Route().carefulRefused.find(guid);
                if (refused != group.Route().carefulRefused.end() && refused->second >= LIFT_REFUSALS_MAX)
                    continue;
                Creature* mob = map->GetCreature(guid);
                if (!mob || !mob->IsAlive() || mob->IsInCombat() || group.Route().bossCreatures.count(guid) ||
                    mob->IsDungeonBoss() || group.BossOrder(mob) >= 0)
                    continue;
                if (!lift || int32(until - liftUntil) < 0)
                {
                    lift = mob;
                    liftUntil = until;
                }
            }
            if (lift)
            {
                group.LogEvent("skip_lifted", JsonObject().Str("name", lift->GetName())
                                                   .Num("entry", lift->GetEntry())
                                                   .Str("was", group.Route().skipReason[lift->GetGUID()])
                                                   .Num("refusals",
                                                       int64(group.Route().carefulRefused[lift->GetGUID()]))
                                                   .Str("why", "nothing else left"));
                ++group.Route().carefulRefused[lift->GetGUID()];  // each lift counts: LIFT_REFUSALS_MAX bounds the asks
                group.Route().skipUntil.erase(lift->GetGUID());
                group.Route().skipReason.erase(lift->GetGUID());
                group.Route().unreachableFrom.erase(lift->GetGUID());
                chosen = lift->GetGUID();
                useStarter = false;
                bestIsBoss = false;
                bestObjective = false;
            }
        }

        // [mech_bridge_far] plan29 follow-up 2 (mech-h / mech-n 20261025-163739: "no_targets", hostilesSeen 0, next
        // boss Pathaleon "beyond sight distance", 120 s idle, stall): the bridge pack that wakes Pathaleon stands
        // ~180 yd from Sepethrea's room, beyond the candidate scan, so nothing chose it. While he is dormant its
        // nearest alive member is the target wherever it stands (its skip lifted: it is the only way on).
        // [b30_mech_pos] not while Sepethrea lives (mechBridgeOpen): the lift link up to her comes first.
        if (chosen.IsEmpty() && mechBridgeOpen)
        {
            std::vector<Creature*> bridgePack;
            MechanarBridge::CollectBridgePack(leader, bridgePack);
            // [b30_mech_pos] lift cap: a bridge mob whose skip was lifted MECH_BRIDGE_MAX_LIFTS times while the pack
            // stayed as big is no longer lifted or chosen (mech-n/h 20261025-184229: 174 / 222 lifts of one
            // Astromage, a lift a second); any kill of the pack starts the count again.
            constexpr uint32 MECH_BRIDGE_MAX_LIFTS = 5;
            MechBridgeLifts& lifts = mechBridgeLifts[group.Run().runSeq];
            if (lifts.packAlive != bridgePack.size())
            {
                lifts.packAlive = uint32(bridgePack.size());
                lifts.count.clear();
                lifts.cappedLogged = false;
            }
            auto const capped = [&lifts](Creature* mob)
            {
                auto const found = lifts.count.find(mob->GetGUID());
                return found != lifts.count.end() && found->second >= MECH_BRIDGE_MAX_LIFTS;
            };
            Creature* nearestBridge = nullptr;
            for (Creature* mob : bridgePack)
                if (mob->IsAlive() && !mob->IsInCombat() && !capped(mob) &&
                    (!nearestBridge || leader->GetExactDist(mob) < leader->GetExactDist(nearestBridge)))
                    nearestBridge = mob;
            if (!nearestBridge && !bridgePack.empty() && !lifts.cappedLogged)
            {
                lifts.cappedLogged = true;
                group.LogEvent("mech_bridge_capped",
                         JsonObject().Num("packAlive", int64(bridgePack.size()))
                             .Num("maxLifts", MECH_BRIDGE_MAX_LIFTS)
                             .Str("why", "every bridge mob lifted that often without a kill: no more lifts"));
            }
            if (nearestBridge)
            {
                for (Creature* mob : bridgePack)
                {
                    if (capped(mob))
                        continue;
                    if (group.Route().skipUntil.erase(mob->GetGUID()) && mob == nearestBridge)
                        ++lifts.count[mob->GetGUID()];
                    group.Route().skipReason.erase(mob->GetGUID());
                }
                group.LogEvent("mech_bridge_target", JsonObject().Str("name", nearestBridge->GetName())
                                                          .Num("entry", nearestBridge->GetEntry())
                                                          .Num("packAlive", int64(bridgePack.size()))
                                                          .Real("distance", leader->GetExactDist(nearestBridge)));
                chosen = nearestBridge->GetGUID();
                useStarter = false;
                bestIsBoss = false;
                bestObjective = false;
            }
        }

        if (chosen.IsEmpty())
        {
            if (!group.Route().noTargets)
            {
                group.Route().noTargets = true;
                group.Route().noTargetsSinceMs = now;
                group.LogEvent("no_targets",
                         JsonObject().Num("hostilesSeen", int64(hostiles.size()))
                             .Num("skipped", int64(group.Route().skipUntil.size())).Num("offRoute", int64(offRoute))
                             .Str("nextBoss", nextBoss ? nextBoss->GetName() : "")
                             .Str("nextBossView",
                                  nextBoss ? AggressiveTargetValue::Explain(leader, nextBoss) : std::string()));
            }
            group.Route().target.Clear();
            group.ClearWaypoint(group.LeaderGuid());
            return;
        }

        group.Route().noTargets = false;
        if (chosen == group.Route().target)
            return;

        // The same creature chosen again after it died, again and again (it revives or respawns): skipped for a
        // while. Only a death seen in between counts (targetDied): a refused pack chosen four times is no respawn.
        for (auto const& [guid, count] : group.Route().targetCounts)
        {
            Creature* earlier = map->GetCreature(guid);
            if (!earlier || !earlier->IsAlive())
                group.Route().targetDied.insert(guid);
        }
        bool cameBack = false;
        if (!useStarter && group.Route().targetDied.count(chosen))
            if (Creature* again = map->GetCreature(chosen); again && again->IsAlive())
            {
                group.Route().targetDied.erase(chosen);
                cameBack = true;
            }
        if (!useStarter && !group.Route().targetCounts.count(chosen))
            group.Route().targetCounts[chosen] = 0;  // watched for a death from now on
        // An objective's key creature (requiredEntries: a pending starter's afterDead - Zul'Farrak's Sandfury
        // Executioner with the Executioner's Key) is never "respawning trash": the run cannot go on without it.
        Creature const* chosenCreature = useStarter ? nullptr : map->GetCreature(chosen);
        bool const chosenRequired = chosenCreature && requiredEntries.count(chosenCreature->GetEntry()) != 0;
        if (!useStarter && !bestIsBoss && !bestObjective && !chosenRequired && cameBack &&
            ++group.Route().targetCounts[chosen] > SAME_TARGET_MAX)
        {
            Creature* again = map->GetCreature(chosen);
            group.LogEvent("respawning_trash", JsonObject().Str("name", again ? again->GetName() : "?")
                                                    .Num("entry", again ? again->GetEntry() : 0)
                                                    .Num("timesChosen", group.Route().targetCounts[chosen]));
            group.Route().targetCounts[chosen] = 0;
            group.SkipTarget(chosen, RESPAWNING_SKIP_MS, "respawning trash");
            return;
        }

        group.Route().target = chosen;
        group.Route().guaranteeHolding = false;
        group.Route().targetIsStarter = useStarter;
        group.Route().starterIdx = useStarter ? size_t(starterIdx) : 0;
        group.Route().targetIsBoss = !useStarter && bestIsBoss;
        group.Route().targetSinceMs = now;
        group.Route().arrivedMs = 0;
        group.Route().approach = 0;
        group.Route().approachBlocker.Clear();
        sightStops.erase(group.LeaderGuid());  // approach to sight: per target
        sightHolds.erase(group.LeaderGuid());
        bossReady.erase(group.LeaderGuid());

        WorldObject* object = useStarter ? static_cast<WorldObject*>(map->GetCreature(chosen)) : nullptr;
        if (useStarter && !object)
            object = map->GetGameObject(chosen);
        if (!useStarter)
            object = map->GetCreature(chosen);

        group.Route().lastEvent = Acore::StringFormat("target {} {}", object ? object->GetName() : "?",
                                              useStarter ? "(event start)" : "");
        // Why the route took it (ROUTE_ONLY_TRASH): the rule, its nearest distance to the backbone (-1: none within
        // its reach in x / y), its route reach (RouteReach) and whether it sees a route point inside that reach.
        std::string routeRule = "full clear";
        RouteWhy chosenWhy;
        if (Creature* creature = object ? object->ToCreature() : nullptr; creature && !useStarter)
        {
            if (auto const why = routeWhy.find(chosen); why != routeWhy.end())
                chosenWhy = why->second;
            else if (routeRules)
            {
                chosenWhy.reach = RouteReach(creature, leader);
                BlocksRoute(leader, creature, chosenWhy.reach, backbone, chosenWhy.check);
            }
            if (!routeRules)
                routeRule = "full clear";
            else if (chosenWhy.rule)
                routeRule = chosenWhy.rule;
            else if (routeFallback)
                routeRule = "fallback (nothing on the route)";
            else if (group.Route().bossCreatures.count(chosen) || creature->IsDungeonBoss() ||
                creature->isWorldBoss() ||
                     group.BossOrder(creature) >= 0)
                routeRule = "boss";
            else if (creature->IsInCombat())
                routeRule = "fighting";
            else
                routeRule = "encounter";
        }
        else if (useStarter)
            routeRule = "event starter";
        // The hold spot's guard priority chose it (Black Morass: an add attacking Medivh, EventSpotDef guardEntries).
        if (Creature const* guardCheck = object ? object->ToCreature() : nullptr;
            guardCheck && !useStarter && holdSpot && AtHoldGuard(*holdSpot, guardCheck))
            routeRule = "hold guard (attacks the event's NPC)";
        group.Route().targetRouteRule = routeRule;  // [route_blocker_required_only]
        group.LogEvent("target", JsonObject().Str("name", object ? object->GetName() : "?")
                                      .Num("entry", object ? object->GetEntry() : 0)
                                      .Bool("boss", group.Route().targetIsBoss).Bool("eventStarter", useStarter)
                                      .Real("score", useStarter ? starterScore : bestScore)
                                      .Bool("crossesSkipped", !useStarter && crossesSkipped.count(chosen) != 0)
                                      .Num("routeCandidates", int64(hostiles.size())).Num("offRoute", int64(offRoute))
                                      .Str("routeRule", routeRule).Real("routeDist", chosenWhy.check.routeDist)
                                      .Real("reach", chosenWhy.reach).Bool("routeLos", chosenWhy.check.routeLos)
                                      .Real("routeDz", chosenWhy.check.routeDz)
                                      .Bool("routeZGated", chosenWhy.check.zGated)
                                      .Str("routeType", routeType).Bool("backboneDropped", backboneDropped)
                                      .Str("plan", chosen == planNext ? planStep : std::string())  // [lead204]
                                      .Real("x", object ? object->GetPositionX() : 0.0f)
                                      .Real("y", object ? object->GetPositionY() : 0.0f)
                                      .Real("z", object ? object->GetPositionZ() : 0.0f));
    }

    void StartWingChange(LeaderGroup& group, size_t next, char const* why)
    {
        group.Route().entranceIdx = next;
        Position const& entrance = group.Route().entrances[group.Route().entranceIdx];
        // [b30_reenter] The walk out through an exit portal and in through the next wing's entrance, or the re-entry
        // (LFG teleport out / in, a death each) when the leader has no walk inside to that entrance or one longer than
        // SHORTCUT_REENTER_MIN_GAIN (dme-h 20261025-184229-dme-h-5E-15: after Alzzin the walk out ended at the rim
        // of a one-way exit's walled pocket, "no walk to the portal" until stopped - [b30_wings] FindPortal). The
        // members in here with the leader re-enter; the dead and those outside walk in as before.
        Player* wingLeader = ObjectAccessor::FindPlayer(group.LeaderGuid());
        bool reenter = false;
        float insideWalk = -1.0f;
        if (wingLeader && wingLeader->IsAlive() &&
            wingLeader->GetMapId() == group.MapId() && !wingLeader->IsInCombat() &&
            !wingLeader->GetTransport() && !wingLeader->GetVehicle() && group.Route().linkIdx < 0 &&
                group.Route().escort.IsEmpty())
        {
            PathResult const inside =
                RoutePath(wingLeader, entrance.GetPositionX(), entrance.GetPositionY(), entrance.GetPositionZ());
            insideWalk = inside.complete ? inside.length : -1.0f;
            reenter = !inside.complete || inside.length > SHORTCUT_REENTER_MIN_GAIN;
        }
        std::vector<Player*> reentering;
        bool anyPortalGoal = false;
        for (LeaderGroup::MemberInfo const& member : group.Members())
            if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
            {
                if (reenter && bot->IsAlive() && bot->GetMap() == wingLeader->GetMap() && !bot->IsInCombat() &&
                    !bot->IsBeingTeleported() && !bot->GetTransport() && !bot->GetVehicle())
                {
                    reentering.push_back(bot);
                    continue;
                }
                anyPortalGoal = true;
                SelfTravel::PortalGoal goal;
                goal.mapId = group.MapId();
                goal.x = entrance.GetPositionX();
                goal.y = entrance.GetPositionY();
                goal.z = entrance.GetPositionZ();
                goal.exact = true;
                SelfTravel::SetPortalGoal(member.guid, goal);
                group.SetMemberHasPortalGoal(member.guid, true);
            }
        if (!reentering.empty())
            ShortcutReenter(group, reentering, next,
                            insideWalk < 0.0f ? "wing change: no walk inside, out through a portal and in again"
                                              : "wing change: the walk inside is longer",
                            insideWalk, 0.0f, "entrance " + std::to_string(next));
        if (anyPortalGoal)
        {
            group.Route().wingWalk = true;
            group.Route().wingWalkSinceMs = getMSTime();
        }
        group.Route().noTargets = false;
        group.Route().skipUntil.clear();
        group.Route().unreachableFrom.clear();
        group.Route().target.Clear();
        group.LogEvent("wing_change", JsonObject().Num("entrance", int64(group.Route().entranceIdx))
                                           .Bool("walked", anyPortalGoal)
                                           .Num("reentered", int64(reentering.size())).Str("why", why)
                                           .Real("x", entrance.GetPositionX()).Real("y", entrance.GetPositionY())
                                           .Real("z", entrance.GetPositionZ()));
    }

    // [sm-wing-done] true while a living boss of the run stands in the wing of Party::entranceIdx (its nearest
    // entrance); nextWing: the entrance of the lowest-order boss left in another wing (-1: none).
    bool WingBossesLeft(LeaderGroup& group, Map* map, Player* leader, int32& nextWing)
    {
        nextWing = -1;
        if (!map || !leader || group.Route().entranceIdx >= group.Route().entrances.size())
            return true;
        int32 nextWingOrder = -1;
        for (ObjectGuid const& guid : group.Route().bossCreatures)
        {
            Creature* boss = map->GetCreature(guid);
            if (!boss || !boss->IsAlive() || (boss->IsFriendlyTo(leader) && !boss->IsInCombat()) ||
                ClassGatedBoss(group.MapId(), group.Difficulty(), boss->GetEntry()))
                continue;
            int32 const wing = NearestEntrance(group, boss);
            if (wing < 0)
                continue;
            if (size_t(wing) == group.Route().entranceIdx ||
                group.Route().entrances[size_t(wing)].GetExactDist(
                    &group.Route().entrances[group.Route().entranceIdx]) < SAME_ENTRANCE_RANGE)
                return true;
            int32 const order = group.BossOrder(boss);
            if (nextWing < 0 || (order >= 0 && (nextWingOrder < 0 || order < nextWingOrder)))
            {
                nextWing = wing;
                nextWingOrder = order;
            }
        }
        return false;
    }
}
