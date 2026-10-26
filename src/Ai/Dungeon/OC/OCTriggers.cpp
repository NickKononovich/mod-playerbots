/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "OCTriggers.h"
#include "AiObject.h"
#include "AiObjectContext.h"
#include "DungeonInteract.h"
#include "InstanceScript.h"
#include "MoveSpline.h"  // [b35_occ]
#include "MovementActions.h"
#include "OCMultipliers.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "RaidBotDuty.h"
#include "StringFormat.h"
#include "Unit.h"
#include "Vehicle.h"  // [b38_occ]

#include <array>
#include <atomic>
#include <initializer_list>
#include <limits>  // [b38_occ]
#include <mutex>
#include <unordered_map>
#include <vector>  // [b38_occ]

namespace
{
// Centrifuge Construct packs before Varos (world DB spawns, map 578) and where the group sets down for each.
// The inner ring (z 433) carries four Ring-Lord packs, each led by one Construct, two of them on each side of the old
// "centre-south" (1103, 994) and "centre-north" (1098, 1107) points: every spot within 25 yd of those points lies in
// the reach of both neighbouring packs (Sorceresses 15 yd off), so the group set down between two packs and woke
// both (occ-h 2026-10-25 132016: 7 mobs on the drakes at 503 s, wipe; 115805 the same at 391 and 453 s). Players land
// on the empty east / west side of the ring (navmesh floor, 45+ yd from every pack, both packs of the side reachable
// on foot) and pull one pack at a time to that spot. The two outer islands (z 439.5) are a single pack each.
struct ConstructPack
{
    Position pack;     // centroid of the pack's Constructs (assigns each alive Construct to its pack)
    Position landing;  // where the group sets down to fight it
};
// [b38_occ] Both old landings stood on the ring's brink (offline navmesh, map 578 mmtiles): (1033.9,1060.0) had no
// floor 0.3 yd south-east of it (a 30 x 14 yd hole, x 1029-1058 / y 1048-1059, next floor z 104.8) and (1162.8,1049.4)
// none 2-3 yd north (z 361 below). A drake that stops 1-3 yd off the spot (DrakeDismountTrigger allows 3) put its rider
// over the void. The spots below have navmesh floor >= 6 yd round them (E) / >= 9 yd (W, SW), every Ring-Lord /
// Construct spawn >= 35 yd off, and a ground path to their packs.
// The same hole cuts the ring between the south-west pack and the west landing: the only ground path round it is
// 298.6 yd (build 38 log, "[oc pack landing] pack 2 ... NOT reachable"), so b34 set the group down by the pack itself,
// whose 25 yd search found no spot out of reach and landed 5 yd from the Construct, in the pack (occ-n
// 20261026-050341-occ-n-5C-28, 591 s). The south-west pack gets its own landing on its side of the hole.
Position const RING_EAST_LANDING = Position(1157.8f, 1041.4f, 433.8f);       // [b38_occ] was (1162.8,1049.4,434.6)
Position const RING_WEST_LANDING = Position(1039.4f, 1068.5f, 433.1f);       // [b38_occ] was (1033.9,1060.0,433.1)
Position const RING_SOUTHWEST_LANDING = Position(1042.5f, 1025.0f, 432.9f);  // [b38_occ] 46 yd from its Construct
ConstructPack const VAROS_CONSTRUCT_PACKS[] =
{
    {Position(1036.5f, 890.1f, 439.5f), Position(1036.5f, 890.1f, 439.5f)},    // outer south island, 3 constructs
    {Position(1018.0f, 1201.6f, 439.5f), Position(1018.0f, 1201.6f, 439.5f)},  // outer north island, 3 constructs
    {Position(1076.9f, 994.3f, 433.0f), RING_SOUTHWEST_LANDING},               // inner ring south-west, 1 construct
    {Position(1071.6f, 1103.8f, 433.0f), RING_WEST_LANDING},                   // inner ring north-west, 1 construct
    {Position(1129.7f, 993.2f, 433.0f), RING_EAST_LANDING},                    // inner ring south-east, 1 construct
    {Position(1124.4f, 1109.8f, 433.0f), RING_EAST_LANDING},                   // inner ring north-east, 1 construct
};
constexpr size_t VAROS_CONSTRUCT_PACK_COUNT = sizeof(VAROS_CONSTRUCT_PACKS) / sizeof(VAROS_CONSTRUCT_PACKS[0]);
Position const VAROS_LANDING = Position(1270.0f, 1070.4f, 439.5f);

// Entrance island: the Nexus Portal (1045.6, 1104.2) and the zone-in point (1055.9, 986.9) at z ~361; Drakos'
// island lies west of x 1001 at the same height.
constexpr float ENTRANCE_ISLAND_MIN_X = 1025.0f;
constexpr float ENTRANCE_ISLAND_MIN_Z = 350.0f;
constexpr float ENTRANCE_ISLAND_MAX_Z = 375.0f;

constexpr float LEG_ARRIVAL_RADIUS = 40.0f;
constexpr float LEG_ARRIVAL_HEIGHT = 15.0f;
constexpr float LANDING_SPREAD = 5.0f;
constexpr float LANDING_HEIGHT = 2.0f;
// A landing spot out of every idle mob's aggro reach (occ-h 2026-10-25, build 11: the tank set down on a construct
// island in a Ring-Lord pack's reach, and its dismount started the fight before the group had landed): rings round
// the island's point, nearest first, on its own floor; the cached choice is kept for this leg.
constexpr std::array<float, 5> LANDING_SAFE_RINGS = {5.0f, 10.0f, 15.0f, 20.0f, 25.0f};
// [b38_occ] Varos legs (Construct packs) only. The outer islands are ~90 yd discs (navmesh x 991-1082 / y 845-936
// round the south pack) with floor 28-40 yd off every mob of the pack, but the search ended at 25 yd: "[oc landing
// unsafe] no spot out of 16 idle mobs' reach within 25 yd" and the pilot hovered 17 yd over the pack and set down in it
// (occ-n 5C-28, 628-661 s: the pack woke on the hovering drake, 4 deaths). Further rings stay inside
// LEG_ARRIVAL_RADIUS (40) of the leg point, so the group on the spot is still on its leg.
constexpr std::array<float, 2> LANDING_PACK_EXTRA_RINGS = {30.0f, 35.0f};
// [b38_occ] Varos legs: the others set down LANDING_NEXT_TO_PILOT round the pilot's spot unchecked, so the spot keeps
// that much more out of every idle mob's reach; and it has floor this far round it in every direction (no landing on a
// brink) within LANDING_FLOOR_MAX_DZ of its own.
constexpr float LANDING_EDGE_CLEARANCE = 5.0f;
constexpr uint32 LANDING_EDGE_DIRECTIONS = 8;
constexpr uint32 LANDING_SAFE_DIRECTIONS = 12;
constexpr float LANDING_FLOOR_SEARCH_UP = 6.0f;
constexpr float LANDING_FLOOR_SEARCH = 12.0f;
constexpr float LANDING_FLOOR_MAX_DZ = 4.0f;
constexpr float LANDING_AGGRO_MARGIN = 3.0f;
constexpr uint32 LANDING_RETRY_MS = 3 * IN_MILLISECONDS;
// The idle mobs of the island are among the bot's possible targets only this close: farther, a "safe" spot is only
// provisional and searched again (occ-h 2026-10-25, build 12: the mage set down in a pack's reach).
constexpr float LANDING_SCAN_RANGE = 45.0f;
// The others land this far from the pilot's spot.
constexpr float LANDING_NEXT_TO_PILOT = 3.0f;
constexpr float RESURRECT_WAIT_RANGE = 60.0f;
constexpr float BOSS_PULL_RANGE = 40.0f;
constexpr float BOSS_PULL_HEIGHT = 15.0f;
constexpr float EREGOS_PULL_RANGE = 60.0f;
constexpr uint32 CONSTRUCT_CACHE_MS = 2 * IN_MILLISECONDS;
// Landing as a group: every drake this close (2D) to the pilot's spot before it sets down; the landed bots wait on
// foot this close. A member that never comes is not waited for longer than the timeout.
constexpr float LANDING_GATHER_RANGE = 25.0f;
constexpr float LANDING_GATHER_HEIGHT = 40.0f;
constexpr uint32 LANDING_GATHER_TIMEOUT_MS = 90 * IN_MILLISECONDS;
constexpr uint32 OC_LOG_INTERVAL_MS = 10 * IN_MILLISECONDS;
constexpr float LANDING_HOLD_SLACK = 4.0f;
constexpr uint32 GHOST_WAIT_TIMEOUT_MS = 4 * MINUTE * IN_MILLISECONDS;
// Below every island floor (entrance z 361, Drakos 359.5, rings 433+).
constexpr float STRANDED_BELOW_Z = 340.0f;

// Every alive member of the bot's instance is within LANDING_GATHER_RANGE of the anchor; with `onFoot` it has also
// stepped off its drake on the anchor's floor.
bool IsGroupGatheredAt(Player* bot, Position const& anchor, bool onFoot, std::string* missing)
{
    Group* group = bot->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || !member->IsInWorld() || member->GetMapId() != bot->GetMapId() ||
            member->GetInstanceId() != bot->GetInstanceId() || OCRoute::IsStrandedBelowIslands(member))
            continue;

        Unit* drake = member->GetVehicleBase();
        WorldObject const* body = drake ? static_cast<WorldObject const*>(drake) : member;
        float const distance = body->GetExactDist2d(anchor);
        float const dz = std::fabs(body->GetPositionZ() - anchor.GetPositionZ());
        bool const away = distance > LANDING_GATHER_RANGE || (onFoot && drake) ||
                          dz > (onFoot ? LEG_ARRIVAL_HEIGHT : LANDING_GATHER_HEIGHT);
        if (!away)
            continue;

        if (missing)
            *missing = Acore::StringFormat("{} {:.1f} yd dz {:.1f}{}", member->GetName(), distance, dz,
                                           drake ? " mounted" : " on foot");
        return false;
    }
    return true;
}

// An alive member of the bot's instance (the bot included) still rides a drake.
bool IsAnyMemberOnDrake(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return bot->GetVehicleBase() != nullptr;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member->IsAlive() && member->IsInWorld() && member->GetMapId() == bot->GetMapId() &&
            member->GetInstanceId() == bot->GetInstanceId() && member->GetVehicleBase())
            return true;
    }
    return false;
}

struct GatherWait
{
    Position leg;
    uint32 since = 0;
    bool timedOut = false;
};

// The gather check with its timeout, per waiting bot (`waits`) and leg.
bool GatheredOrTimedOut(std::unordered_map<uint64, GatherWait>& waits, Player* bot, Position const& legPos,
                        bool gathered, std::string const& missing, char const* stage)
{
    GatherWait& wait = waits[bot->GetGUID().GetRawValue()];
    uint32 const now = getMSTime();
    if (gathered)
    {
        if (wait.since && !wait.timedOut)
            LOG_INFO("playerbots", "[oc landing gathered] {}: {} after {} s at ({:.1f},{:.1f},{:.1f})", bot->GetName(),
                     stage, getMSTimeDiff(wait.since, now) / IN_MILLISECONDS, legPos.GetPositionX(),
                     legPos.GetPositionY(), legPos.GetPositionZ());
        wait = GatherWait();
        return true;
    }

    if (!wait.since || wait.leg.GetExactDist(&legPos) > 1.0f)
    {
        wait.leg = legPos;
        wait.since = now ? now : 1;
        wait.timedOut = false;
    }

    if (getMSTimeDiff(wait.since, now) < LANDING_GATHER_TIMEOUT_MS)
        return false;

    if (!wait.timedOut)
        LOG_INFO("playerbots", "[oc landing timeout] {}: {} without the whole group after {} s, still away: {}",
                 bot->GetName(), stage, LANDING_GATHER_TIMEOUT_MS / IN_MILLISECONDS, missing);
    wait.timedOut = true;
    return true;
}

struct ConstructCache
{
    uint32 time = 0;
    std::array<uint8, VAROS_CONSTRUCT_PACK_COUNT> alive{};
};

// Alive constructs per island, from the map's spawn store (instances keep every grid loaded). Cached per instance
// and map thread: every bot of the group asks the same question several times per tick.
std::array<uint8, VAROS_CONSTRUCT_PACK_COUNT> const& GetAliveConstructs(Player* bot)
{
    thread_local std::unordered_map<uint32, ConstructCache> cache;
    ConstructCache& entry = cache[bot->GetInstanceId()];
    uint32 const now = getMSTime();
    if (entry.time && getMSTimeDiff(entry.time, now) < CONSTRUCT_CACHE_MS)
        return entry.alive;

    entry.time = now;
    entry.alive.fill(0);
    for (auto const& [spawnId, creature] : bot->GetMap()->GetCreatureBySpawnIdStore())
    {
        if (!creature || creature->GetEntry() != NPC_CENTRIFUGE_CONSTRUCT || !creature->IsAlive())
            continue;

        Position const& home = creature->GetHomePosition();
        size_t best = 0;
        for (size_t i = 1; i < VAROS_CONSTRUCT_PACK_COUNT; ++i)
            if (home.GetExactDist2d(VAROS_CONSTRUCT_PACKS[i].pack) <
                home.GetExactDist2d(VAROS_CONSTRUCT_PACKS[best].pack))
                best = i;
        ++entry.alive[best];
    }
    return entry.alive;
}

// [b30_oculus] The shared ring landing of an inner-ring pack is only its landing when the pack stands on the same
// navmesh: occ-h 20261025-184229-26 (826-1827 s) set down on RING_WEST_LANDING for the south-west pack (one
// Construct of the 4 left, the harness target its Ring-Lord Sorceress at (1088.2,989.9)); the walk there was
// "incomplete 4" and ended at (1081,1094) north of the landing, out of the leg's arrival radius, so the pilot mounted,
// flew back to the landing and set down again, every ~30 s for 1000 s. The test is a ground path from the landing to
// the pack (the islands never move: cached per map thread and pack). False: the group sets down by the pack itself.
// [b34_occ] The b30 path test still answers "reachable, path type 1" for the south-west pack from RING_WEST_LANDING
// (build 34, both runs: 20261026-014521-occ-n-5A-26 and occ-h-5A-20), while every walk from the landing went north
// to (1081.6,1094.3), stalled ("bot_cannot_move" toward (1105,1105.5)), left the leg's 40 yd and the pilot flew the
// group back: 25 such cycles per run (40 "drake_phase", 24 "regroup_ping_pong", the harness target that Construct
// for 1100 s, 6 of the 10 Constructs dead, Varos never unshielded). A shared landing the pilot walked off on foot
// toward its own pack this often is no landing for that pack (NoteSharedLandingWalkOff): the group sets down by the
// pack. Shared by every map thread (the leg must not flip between threads); the islands never move.
constexpr uint8 PACK_LANDING_WALK_OFF_LIMIT = 2;
// A ground path this much longer than the straight line goes round the ring, not to the pack.
constexpr float PACK_LANDING_MAX_DETOUR = 2.0f;
std::array<std::atomic<bool>, VAROS_CONSTRUCT_PACK_COUNT> packLandingWalkedOff{};

bool PackReachableFromLanding(Player* pilot, size_t pack)
{
    ConstructPack const& def = VAROS_CONSTRUCT_PACKS[pack];
    if (def.landing.GetExactDist(&def.pack) < 1.0f)
        return true;

    if (packLandingWalkedOff[pack].load(std::memory_order_relaxed))  // [b34_occ]
        return false;

    thread_local std::array<int8, VAROS_CONSTRUCT_PACK_COUNT> reachable = [] {
        std::array<int8, VAROS_CONSTRUCT_PACK_COUNT> unknown{};
        unknown.fill(-1);
        return unknown;
    }();
    if (reachable[pack] < 0)
    {
        PathGenerator path(pilot);
        path.CalculatePath(def.landing.GetPositionX(), def.landing.GetPositionY(), def.landing.GetPositionZ(),
                           def.pack.GetPositionX(), def.pack.GetPositionY(), def.pack.GetPositionZ(), false);
        PathType const type = path.GetPathType();
        // [b34_occ] also no detour round the ring, and the length is logged (build 34 had only "path type 1")
        float const straight = def.landing.GetExactDist(&def.pack);
        float const length = path.getPathLength();
        bool const ground =
            (type & PATHFIND_NORMAL) &&
            !(type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE | PATHFIND_NOT_USING_PATH |
                      PATHFIND_SHORT | PATHFIND_FARFROMPOLY)) &&
            length <= PACK_LANDING_MAX_DETOUR * straight;
        reachable[pack] = ground ? 1 : 0;
        LOG_INFO("playerbots", "[oc pack landing] pack {} at ({:.1f},{:.1f},{:.1f}): {} on foot from its landing "
                 "({:.1f},{:.1f},{:.1f}), path type {}, length {:.1f} yd (straight {:.1f})", pack,
                 def.pack.GetPositionX(), def.pack.GetPositionY(), def.pack.GetPositionZ(),
                 ground ? "reachable" : "NOT reachable (sets down by the pack)", def.landing.GetPositionX(),
                 def.landing.GetPositionY(), def.landing.GetPositionZ(), uint32(type), length, straight);
    }
    return reachable[pack] > 0;
}

bool IsOutOfCombatGroupReady(Player* pilot)
{
    Group* group = pilot->GetGroup();
    if (!group)
        return !pilot->IsInCombat();

    bool healerAlive = false;
    bool corpseNearby = false;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->GetMapId() != pilot->GetMapId() || member->GetInstanceId() != pilot->GetInstanceId())
            continue;

        if (member->IsAlive())
        {
            if (member->IsInCombat())
                return false;

            healerAlive = healerAlive || PlayerbotAI::IsHeal(member);
        }
        else if (member->IsWithinDistInMap(pilot, RESURRECT_WAIT_RANGE))
            corpseNearby = true;
    }

    // A dead member next to us can still be resurrected before the group flies off.
    return !(corpseNearby && healerAlive);
}
}  // namespace

OculusDrakeGoal GetOculusDrakeGoal(Player* bot)
{
    if (bot->GetMapId() != OCULUS_MAP_ID)
        return OculusDrakeGoal::NONE;

    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetData(OCULUS_DATA_DRAKOS) != DONE)
        return OculusDrakeGoal::NONE;

    if (instance->GetData(OCULUS_DATA_VAROS) != DONE)
        return OculusDrakeGoal::VAROS;

    if (instance->GetData(OCULUS_DATA_UROM) != DONE)
        return OculusDrakeGoal::UROM;

    if (instance->GetData(OCULUS_DATA_EREGOS) != DONE)
        return OculusDrakeGoal::EREGOS;

    return OculusDrakeGoal::NONE;
}

namespace OCRoute
{
Player* GetPilot(PlayerbotAI* botAI) { return DungeonInteract::GetInteractor(botAI); }

bool IsOnEntranceIsland(WorldObject const* object)
{
    return object->GetMapId() == OCULUS_MAP_ID && object->GetPositionX() > ENTRANCE_ISLAND_MIN_X &&
           object->GetPositionZ() > ENTRANCE_ISLAND_MIN_Z && object->GetPositionZ() < ENTRANCE_ISLAND_MAX_Z;
}

Creature* GetBoss(Player* bot, uint32 dataId)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance)
        return nullptr;

    ObjectGuid const guid = instance->GetGuidData(dataId);
    return guid ? bot->GetMap()->GetCreature(guid) : nullptr;
}

LegTarget GetLegTarget(Player* pilot)
{
    LegTarget leg;
    switch (GetOculusDrakeGoal(pilot))
    {
        case OculusDrakeGoal::VAROS:
        {
            InstanceScript* instance = pilot->GetInstanceScript();
            if (instance && instance->GetData(OCULUS_DATA_CC_COUNT) >= OCULUS_CENTRIFUGE_CONSTRUCTS)
            {
                leg.pos = VAROS_LANDING;
                leg.land = true;
                leg.valid = true;
                break;
            }

            // Drake spells may not target the constructs (conditions 17/13) and hitting a grounded enemy with
            // one kills the rider (npc_oculus_drake::SpellHitTarget): land on their island and fight on foot.
            auto const& alive = GetAliveConstructs(pilot);
            int32 best = -1;
            for (size_t i = 0; i < VAROS_CONSTRUCT_PACK_COUNT; ++i)
                if (alive[i] && (best < 0 || pilot->GetExactDist2d(VAROS_CONSTRUCT_PACKS[i].pack) <
                                                 pilot->GetExactDist2d(VAROS_CONSTRUCT_PACKS[best].pack)))
                    best = static_cast<int32>(i);

            if (best < 0)
                RaidBotDuty::LogWorkaroundOnce(pilot, "oc-varos-constructs",
                                               "Oculus: fewer than 10 constructs counted but none alive, hovering");

            leg.pos = best >= 0 ? VAROS_CONSTRUCT_PACKS[best].landing : OCULUS_CONSTRUCT_RING_CENTER;
            // [b30_oculus] a ring landing the pack cannot be walked to from: set down by the pack instead
            // (GetLandingSpot still picks a spot out of the idle mobs' reach round it)
            if (best >= 0 && !PackReachableFromLanding(pilot, static_cast<size_t>(best)))
                leg.pos = VAROS_CONSTRUCT_PACKS[best].pack;
            leg.land = best >= 0;
            leg.valid = true;
            if (best >= 0)
            {
                // Fighting the pack where it stood (the pull went that way) still counts as being on this leg.
                leg.pack = VAROS_CONSTRUCT_PACKS[best].pack;
                leg.hasPack = true;
                leg.packIndex = static_cast<int8>(best);  // [b34_occ]
            }
            break;
        }
        case OculusDrakeGoal::UROM:
        {
            // Urom teleports to the next platform after each Summon Menagerie, then to the inner ring.
            Position uromPos(uromCoords[0][0], uromCoords[0][1], uromCoords[0][2]);
            if (Creature* urom = GetBoss(pilot, OCULUS_DATA_UROM))
                uromPos = urom->GetPosition();

            uint8 best = 0;
            for (uint8 i = 1; i < 4; ++i)
                if (uromPos.GetExactDist(uromCoords[i][0], uromCoords[i][1], uromCoords[i][2]) <
                    uromPos.GetExactDist(uromCoords[best][0], uromCoords[best][1], uromCoords[best][2]))
                    best = i;

            leg.pos.Relocate(uromCoords[best][0], uromCoords[best][1], uromCoords[best][2]);
            leg.land = true;
            leg.valid = true;
            break;
        }
        case OculusDrakeGoal::EREGOS:
        {
            Creature* eregos = GetBoss(pilot, OCULUS_DATA_EREGOS);
            leg.pos = eregos && eregos->IsAlive() ? eregos->GetPosition() : OCULUS_EREGOS_POSITION;
            leg.valid = true;
            break;
        }
        default:
            break;
    }
    return leg;
}

bool IsAtLegTarget(WorldObject const* object, LegTarget const& leg)
{
    if (!leg.valid || !leg.land)
        return false;

    auto const within = [object](Position const& point)
    {
        return object->GetExactDist2d(point) < LEG_ARRIVAL_RADIUS &&
               std::fabs(object->GetPositionZ() - point.GetPositionZ()) < LEG_ARRIVAL_HEIGHT;
    };
    return within(leg.pos) || (leg.hasPack && within(leg.pack));
}

bool IsNearLegTarget2d(WorldObject const* object, LegTarget const& leg)
{
    return leg.valid && leg.land && object->GetExactDist2d(leg.pos) < LEG_ARRIVAL_RADIUS &&
           std::fabs(object->GetPositionZ() - leg.pos.GetPositionZ()) < LANDING_GATHER_HEIGHT;
}

bool IsGroupGatheredForLanding(Player* pilot, LegTarget const& leg, std::string* missing)
{
    PlayerbotAI* pilotAI = GET_PLAYERBOT_AI(pilot);
    if (!pilotAI || !leg.valid || !leg.land)
        return true;

    thread_local std::unordered_map<uint64, GatherWait> waits;
    std::string away;
    bool const gathered = IsGroupGatheredAt(pilot, GetLandingSpot(pilotAI, leg), false, &away);
    if (missing)
        *missing = away;
    return GatheredOrTimedOut(waits, pilot, leg.pos, gathered, away, "sets down");
}

bool IsGroupLandedAtLeg(PlayerbotAI* botAI, LegTarget const& leg, std::string* missing)
{
    Player* pilot = GetPilot(botAI);
    PlayerbotAI* pilotAI = pilot ? GET_PLAYERBOT_AI(pilot) : nullptr;
    if (!pilotAI || !leg.valid || !leg.land)
        return true;

    Position const spot = GetLandingSpot(pilotAI, leg);
    Position const floor(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ() - LANDING_HEIGHT);
    thread_local std::unordered_map<uint64, GatherWait> waits;
    std::string away;
    bool const gathered = IsGroupGatheredAt(botAI->GetBot(), floor, true, &away);
    if (missing)
        *missing = away;
    return GatheredOrTimedOut(waits, botAI->GetBot(), leg.pos, gathered, away, "stops waiting on foot");
}

bool IsMemberOnEntranceIsland(Player* bot, std::string* who)
{
    Group* group = bot->GetGroup();
    if (!group)
        return IsOnEntranceIsland(bot);

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || !member->IsInWorld() || member->GetMapId() != bot->GetMapId() ||
            member->GetInstanceId() != bot->GetInstanceId() || !IsOnEntranceIsland(member))
            continue;

        if (who)
            *who = member->GetName();
        return true;
    }
    return false;
}

bool IsMemberRunningBack(Player* bot, std::string* who)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || member->IsAlive() || !member->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            continue;

        if (who)
            *who = member->GetName();
        return true;
    }
    return false;
}

// [b35_occ] The bot's own spline ends at the Nexus Portal (its walk there is under way).
bool IsWalkingToNexusPortal(Player* bot)
{
    constexpr float PORTAL_WALK_END_RANGE = 6.0f;
    if (!bot->movespline || bot->movespline->Finalized())
        return false;

    G3D::Vector3 const end = bot->movespline->FinalDestination();
    G3D::Vector3 const portal(OCIds::NEXUS_PORTAL_X, OCIds::NEXUS_PORTAL_Y, end.z);
    return (end - portal).length() < PORTAL_WALK_END_RANGE;
}

bool IsStrandedBelowIslands(Player const* member)
{
    return member && member->GetMapId() == OCULUS_MAP_ID && member->IsAlive() && !member->GetVehicleBase() &&
           !member->IsInFlight() && member->GetPositionZ() < STRANDED_BELOW_Z;
}

bool ShouldLog(Player* bot, char const* key, uint32 intervalMs)
{
    // Bots of different maps run on different threads.
    thread_local std::unordered_map<std::string, uint32> lastLogged;
    std::string const fullKey = bot->GetGUID().ToString() + key;
    uint32 const now = getMSTime();
    auto const it = lastLogged.find(fullKey);
    if (it != lastLogged.end() && getMSTimeDiff(it->second, now) < intervalMs)
        return false;

    lastLogged[fullKey] = now;
    return true;
}

Position GetLandingSpot(PlayerbotAI* botAI, LegTarget const& leg)
{
    Player* bot = botAI->GetBot();
    int32 const slot = std::max(0, botAI->GetGroupSlotIndex(bot));
    float const slotAngle = slot * 2.0f * static_cast<float>(M_PI) / 5.0f;

    // The others set down next to the pilot's own spot, the one checked clear of every idle pack, not on slot
    // points of their own (occ-h build 17: the mage's own spot lay in a Ring-Lord pack's reach).
    Player* pilot = GetPilot(botAI);
    PlayerbotAI* pilotAI = pilot && pilot != bot ? GET_PLAYERBOT_AI(pilot) : nullptr;
    if (pilotAI && pilot->GetMap() == bot->GetMap())
    {
        Position const base = GetLandingSpot(pilotAI, leg);
        float const x = base.GetPositionX() + LANDING_NEXT_TO_PILOT * std::cos(slotAngle);
        float const y = base.GetPositionY() + LANDING_NEXT_TO_PILOT * std::sin(slotAngle);
        float const floor = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y,
                                                     base.GetPositionZ() + LANDING_FLOOR_SEARCH_UP, true,
                                                     LANDING_FLOOR_SEARCH);
        if (floor > INVALID_HEIGHT && std::fabs(floor + LANDING_HEIGHT - base.GetPositionZ()) <= LANDING_FLOOR_MAX_DZ)
            return Position(x, y, floor + LANDING_HEIGHT);
        return base;
    }
    Position const plain(leg.pos.GetPositionX() + LANDING_SPREAD * std::cos(slotAngle),
                         leg.pos.GetPositionY() + LANDING_SPREAD * std::sin(slotAngle),
                         leg.pos.GetPositionZ() + LANDING_HEIGHT);

    struct Choice
    {
        Position leg;
        Position spot;
        uint32 ms = 0;
        bool safe = false;
    };
    // Bots of different maps run on different threads.
    thread_local std::unordered_map<uint64, Choice> choices;
    Choice& choice = choices[bot->GetGUID().GetRawValue()];
    std::vector<Creature*> idle;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
    {
        Creature* mob = botAI->GetCreature(guid);
        if (mob && mob->IsAlive() && !mob->IsInCombat() && mob->HasReactState(REACT_AGGRESSIVE) &&
            bot->IsHostileTo(mob))
            idle.push_back(mob);
    }

    // IdleAggroBlocker is nullptr when the mob WAKES (else the reason it does not): the test was inverted, so a spot
    // was "safe" only if every idle mob would wake, and the pilot always fell back to the plain leg point.
    // [b38_occ] Varos legs: the wider margin (the others land LANDING_NEXT_TO_PILOT off this spot), floor all round
    // and a ground walk to the pack (see LANDING_PACK_EXTRA_RINGS / LANDING_EDGE_CLEARANCE).
    bool const packLeg = leg.hasPack;
    float const aggroMargin = LANDING_AGGRO_MARGIN + (packLeg ? LANDING_NEXT_TO_PILOT : 0.0f);
    auto const safeAt = [&](float x, float y, float z)
    {
        for (Creature* mob : idle)
            if (!MovementAction::IdleAggroBlocker(mob, bot, x, y, z, aggroMargin, false))
                return false;
        return true;
    };
    // [b38_occ] Floor within LANDING_FLOOR_MAX_DZ of `floor` LANDING_EDGE_CLEARANCE round (x, y) in every direction.
    auto const solidRound = [&](float x, float y, float floor)
    {
        for (uint32 i = 0; i < LANDING_EDGE_DIRECTIONS; ++i)
        {
            float const angle = i * 2.0f * static_cast<float>(M_PI) / LANDING_EDGE_DIRECTIONS;
            float const ex = x + LANDING_EDGE_CLEARANCE * std::cos(angle);
            float const ey = y + LANDING_EDGE_CLEARANCE * std::sin(angle);
            float const ez = bot->GetMap()->GetHeight(bot->GetPhaseMask(), ex, ey, floor + LANDING_FLOOR_SEARCH_UP,
                                                      true, LANDING_FLOOR_SEARCH);
            if (ez <= INVALID_HEIGHT || std::fabs(ez - floor) > LANDING_FLOOR_MAX_DZ)
                return false;
        }
        return true;
    };
    // [b38_occ] A ground walk from the spot to the pack, not round the ring (the south-west hole). A path the
    // generator does not build (NOT_USING_PATH: no navmesh answer) does not reject the spot.
    auto const walksToPack = [&](float x, float y, float z)
    {
        PathGenerator path(bot);
        path.CalculatePath(x, y, z, leg.pack.GetPositionX(), leg.pack.GetPositionY(), leg.pack.GetPositionZ(), false);
        PathType const type = path.GetPathType();
        if (type & PATHFIND_NOT_USING_PATH)
            return true;

        float const straight = leg.pack.GetExactDist(x, y, z);
        return (type & PATHFIND_NORMAL) &&
               !(type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE | PATHFIND_SHORT |
                         PATHFIND_FARFROMPOLY)) &&
               path.getPathLength() <= PACK_LANDING_MAX_DETOUR * straight + LANDING_EDGE_CLEARANCE;
    };
    auto const fitsPackLeg = [&](float x, float y, float floor)
    { return !packLeg || (solidRound(x, y, floor) && walksToPack(x, y, floor)); };

    // A cached "safe" spot is checked again against the idle mobs on every call (WIPE_ANALYSIS_w24_25: the cached
    // spot outlived the pack layout and the group dismounted next to a pack).
    uint32 const now = getMSTime();
    if (choice.ms && choice.leg.GetExactDist(&leg.pos) < 1.0f &&
        (choice.safe ? safeAt(choice.spot.GetPositionX(), choice.spot.GetPositionY(),
                              choice.spot.GetPositionZ() - LANDING_HEIGHT)
                     : getMSTimeDiff(choice.ms, now) < LANDING_RETRY_MS))
        return choice.spot;

    choice.leg = leg.pos;
    choice.ms = now ? now : 1;
    choice.safe = false;
    choice.spot = plain;

    // [b38_occ] Varos legs: a ring landing (a fixed spot checked offline, see RING_EAST_LANDING) is tried first as it
    // is; a leg set down by the pack itself searches on past 25 yd (LANDING_PACK_EXTRA_RINGS).
    std::vector<float> rings;
    if (packLeg && leg.pos.GetExactDist(&leg.pack) >= 1.0f)
        rings.push_back(0.0f);
    rings.insert(rings.end(), LANDING_SAFE_RINGS.begin(), LANDING_SAFE_RINGS.end());
    if (packLeg)
        rings.insert(rings.end(), LANDING_PACK_EXTRA_RINGS.begin(), LANDING_PACK_EXTRA_RINGS.end());

    for (float const ring : rings)
    {
        for (uint32 i = 0; i < (ring > 0.0f ? LANDING_SAFE_DIRECTIONS : 1u); ++i)
        {
            // From the bot's own slot direction outwards both ways, so the group still spreads.
            float const step = 2.0f * static_cast<float>(M_PI) / LANDING_SAFE_DIRECTIONS;
            float const turn = (i % 2 ? -1.0f : 1.0f) * static_cast<float>((i + 1) / 2) * step;
            float const angle = slotAngle + turn;
            float const x = leg.pos.GetPositionX() + ring * std::cos(angle);
            float const y = leg.pos.GetPositionY() + ring * std::sin(angle);
            float const floor = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y,
                                                         leg.pos.GetPositionZ() + LANDING_FLOOR_SEARCH_UP, true,
                                                         LANDING_FLOOR_SEARCH);
            if (floor <= INVALID_HEIGHT || std::fabs(floor - leg.pos.GetPositionZ()) > LANDING_FLOOR_MAX_DZ ||
                !safeAt(x, y, floor) || !fitsPackLeg(x, y, floor))
                continue;

            choice.spot = Position(x, y, floor + LANDING_HEIGHT);
            choice.safe = bot->GetExactDist(&leg.pos) <= LANDING_SCAN_RANGE;
            if (choice.safe && ShouldLog(bot, "landing-spot", OC_LOG_INTERVAL_MS))
                LOG_INFO("playerbots",
                         "[oc landing spot] {}: sets down at ({:.1f},{:.1f},{:.1f}), {:.1f} yd off the leg point, out "
                         "of {} idle mobs' reach{}",
                         bot->GetName(), x, y, floor, leg.pos.GetExactDist2d(x, y), idle.size(),
                         packLeg ? " [b38_occ] (+3 yd margin, floor 5 yd round, walk to the pack)" : "");
            return choice.spot;
        }
    }

    // The leg points of the Varos ring are themselves out of every pack's reach; only the small outer islands have
    // no such spot (one pack on the whole island): the group then sets down together on it.
    if (bot->GetExactDist(&leg.pos) <= LANDING_SCAN_RANGE && ShouldLog(bot, "landing-unsafe", OC_LOG_INTERVAL_MS))
        LOG_INFO("playerbots", "[oc landing unsafe] {}: no spot out of {} idle mobs' reach within {:.0f} yd of "
                 "({:.1f},{:.1f},{:.1f}), sets down at ({:.1f},{:.1f},{:.1f})", bot->GetName(), idle.size(),
                 rings.back(), leg.pos.GetPositionX(), leg.pos.GetPositionY(), leg.pos.GetPositionZ(),
                 plain.GetPositionX(), plain.GetPositionY(), plain.GetPositionZ());
    return choice.spot;
}

bool IsGroupReadyToFly(Player* pilot) { return IsOutOfCombatGroupReady(pilot); }

Creature* GetPullableBoss(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance)
        return nullptr;

    // Drakos, Varos and Urom never aggro on sight (empty MoveInLineOfSight): somebody has to hit them.
    for (uint32 dataId : {OCULUS_DATA_DRAKOS, OCULUS_DATA_VAROS, OCULUS_DATA_UROM})
    {
        if (instance->GetData(dataId) == DONE)
            continue;

        Creature* boss = GetBoss(bot, dataId);
        if (!boss || !boss->IsAlive() || boss->IsInCombat() || boss->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE))
            continue;

        if (bot->IsWithinDistInMap(boss, BOSS_PULL_RANGE) &&
            std::fabs(bot->GetPositionZ() - boss->GetPositionZ()) < BOSS_PULL_HEIGHT)
            return boss;
    }
    return nullptr;
}

Creature* GetPullableEregos(Player* bot, Unit const* from)
{
    if (GetOculusDrakeGoal(bot) != OculusDrakeGoal::EREGOS)
        return nullptr;

    Creature* eregos = GetBoss(bot, OCULUS_DATA_EREGOS);
    if (!eregos || !eregos->IsAlive() || eregos->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
        !from->IsWithinDistInMap(eregos, EREGOS_PULL_RANGE))
        return nullptr;

    // [b38_occ] Nobody can call a drake once Eregos is engaged (OculusShouldMountDrake), so whoever is still on foot
    // then is out of the fight for good: occ-h 20261026-050341-occ-h-5H-17 opened on him at 1510 s with the hunter
    // and the mage still on Urom's platform (z 508): the hunter died there to Arcane Volley at 1536 s and the mage
    // never joined; three drakes fought Eregos and his whelps alone. Open only with every alive member on a drake,
    // or after EREGOS_MOUNT_WAIT_MS (a member who cannot mount must not stall the run).
    constexpr uint32 EREGOS_MOUNT_WAIT_MS = 90 * IN_MILLISECONDS;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!eregos->IsInCombat() && botAI && !RaidBotDuty::GetHumanMaster(botAI))
    {
        std::string onFoot;
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref && onFoot.empty(); ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member->IsAlive() && member->IsInWorld() && member->GetMapId() == bot->GetMapId() &&
                    member->GetInstanceId() == bot->GetInstanceId() && !member->GetVehicleBase())
                    onFoot = member->GetName();
            }

        thread_local std::unordered_map<uint32, uint32> waitSince;
        uint32& since = waitSince[bot->GetInstanceId()];
        if (onFoot.empty())
            since = 0;
        else
        {
            uint32 const now = getMSTime();
            if (!since)
                since = now ? now : 1;
            if (getMSTimeDiff(since, now) < EREGOS_MOUNT_WAIT_MS)
            {
                if (ShouldLog(bot, "eregos-mount-wait", OC_LOG_INTERVAL_MS))
                    LOG_INFO("playerbots", "[b38_occ] [oc eregos hold] {}: no opener on Eregos, {} is not on a drake "
                             "({} s)", bot->GetName(), onFoot, getMSTimeDiff(since, now) / IN_MILLISECONDS);
                return nullptr;
            }
        }
    }

    return eregos;
}

Creature* GetWhelpOnGroup(Player* bot, Unit const* from, float range)
{
    if (!from || from->GetMapId() != OCULUS_MAP_ID)
        return nullptr;

    std::list<Creature*> whelps;
    from->GetCreatureListWithEntryInGrid(whelps, NPC_GREATER_LEY_WHELP, range);
    Creature* best = nullptr;
    for (Creature* whelp : whelps)
    {
        if (!whelp || !whelp->IsAlive() || !whelp->IsInCombat() || !whelp->CanFly())
            continue;

        Unit* victim = whelp->GetVictim();
        Unit* rider = victim && victim->GetVehicleKit() ? victim->GetVehicleKit()->GetPassenger(0) : nullptr;
        Player* player = rider ? rider->ToPlayer() : (victim ? victim->ToPlayer() : nullptr);
        if (!player || (player != bot && !bot->IsInSameGroupWith(player)))
            continue;

        if (!best || from->GetExactDist(whelp) < from->GetExactDist(best))
            best = whelp;
    }
    return best;
}

bool GetLowDrakeRefuge(Player* bot, Unit* drake, Position& refuge)
{
    constexpr float LOW_DRAKE_HEALTH_PCT = 25.0f;
    constexpr float REFUGE_ABOVE_FLOOR = 3.0f;
    constexpr float REFUGE_MAX_DROP = 60.0f;      // the Eregos floor (~602) under drakes at 605-640; not the bottom
    constexpr float REFUGE_RECHECK_MS = 3 * IN_MILLISECONDS;
    constexpr std::array<float, 6> RADII = {0.0f, 10.0f, 20.0f, 30.0f, 45.0f, 60.0f};
    constexpr uint32 DIRECTIONS = 16;
    if (!drake || !drake->IsAlive() || drake->GetMapId() != OCULUS_MAP_ID ||
        drake->HealthAbovePct(LOW_DRAKE_HEALTH_PCT))
        return false;

    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetData(OCULUS_DATA_EREGOS) != IN_PROGRESS)
        return false;

    struct Refuge
    {
        Position spot;
        uint32 ms = 0;
        bool found = false;
    };
    thread_local std::unordered_map<uint64, Refuge> refuges;
    Refuge& cached = refuges[drake->GetGUID().GetRawValue()];
    uint32 const now = getMSTime();
    if (cached.ms && getMSTimeDiff(cached.ms, now) < REFUGE_RECHECK_MS)
    {
        refuge = cached.spot;
        return cached.found;
    }

    cached.ms = now ? now : 1;
    cached.found = false;
    float bestScore = std::numeric_limits<float>::max();
    for (float radius : RADII)
        for (uint32 i = 0; i < (radius > 0.0f ? DIRECTIONS : 1u); ++i)
        {
            float const angle = i * 2.0f * static_cast<float>(M_PI) / DIRECTIONS;
            float const x = drake->GetPositionX() + radius * std::cos(angle);
            float const y = drake->GetPositionY() + radius * std::sin(angle);
            float const floor = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, drake->GetPositionZ() + 2.0f,
                                                         true, REFUGE_MAX_DROP);
            if (floor <= INVALID_HEIGHT || drake->GetPositionZ() - floor > REFUGE_MAX_DROP)
                continue;

            // the way there counts like the drop: a short flight to a low floor beats a long one
            float const score = radius + 0.5f * std::max(0.0f, drake->GetPositionZ() - floor);
            if (score >= bestScore)
                continue;

            bestScore = score;
            cached.spot.Relocate(x, y, floor + REFUGE_ABOVE_FLOOR);
            cached.found = true;
        }

    if (cached.found && ShouldLog(bot, "low-drake-refuge", OC_LOG_INTERVAL_MS))
        LOG_INFO("playerbots", "[b38_occ] [oc low drake] {}: drake at {:.0f} % hp, flies low over the floor at "
                 "({:.1f},{:.1f},{:.1f}) (from {:.1f} yd up)", bot->GetName(), drake->GetHealthPct(),
                 cached.spot.GetPositionX(), cached.spot.GetPositionY(), cached.spot.GetPositionZ(),
                 drake->GetPositionZ() - cached.spot.GetPositionZ());
    refuge = cached.spot;
    return cached.found;
}

// [b36_occ]
std::list<Creature*> GetPlanarAnomalies(WorldObject const* from, float range)
{
    std::list<Creature*> anomalies;
    if (!from || from->GetMapId() != OCULUS_MAP_ID)
        return anomalies;

    from->GetCreatureListWithEntryInGrid(anomalies, OCIds::NPC_PLANAR_ANOMALY, range);
    anomalies.remove_if([](Creature* anomaly) { return !anomaly || !anomaly->IsAlive(); });
    return anomalies;
}

bool IsInEnergizeCoresCone(Creature const* varos, WorldObject const* object)
{
    float const angle = varos->GetAbsoluteAngle(object);
    for (uint8 i = 0; i < 4; ++i)
    {
        float const zap = Position::NormalizeOrientation(OCIds::VAROS_FIRST_ZAP_ANGLE + i * static_cast<float>(M_PI_2));
        float diff = Position::NormalizeOrientation(angle - zap);
        if (diff > static_cast<float>(M_PI))
            diff -= 2.0f * static_cast<float>(M_PI);

        if (std::fabs(diff) < OCIds::ENERGIZE_CORES_HALF_CONE)
            return true;
    }
    return false;
}

float GetEnergizeCoresSafeAngle(Creature const* varos, WorldObject const* object)
{
    // The four 36 deg gaps between the zap cones are centred 45 deg off each zap direction.
    float const angle = varos->GetAbsoluteAngle(object);
    float best = 0.0f;
    float bestDiff = 2.0f * static_cast<float>(M_PI);
    for (uint8 i = 0; i < 4; ++i)
    {
        float const zap = OCIds::VAROS_FIRST_ZAP_ANGLE + i * static_cast<float>(M_PI_2);
        float const safe = Position::NormalizeOrientation(zap + static_cast<float>(M_PI_4));
        float diff = Position::NormalizeOrientation(angle - safe);
        if (diff > static_cast<float>(M_PI))
            diff = 2.0f * static_cast<float>(M_PI) - diff;

        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = safe;
        }
    }
    return best;
}
}  // namespace OCRoute

namespace
{
// [b34_occ] The pilot stood on foot at a shared ring landing for this leg's pack and is now off the leg on the same
// floor without the pack dead (it would call its drake to fly back): the walk from that landing does not reach the
// pack. Counted per pilot and pack; at PACK_LANDING_WALK_OFF_LIMIT the landing is dropped for that pack and the group
// sets down by the pack (PackReachableFromLanding). Called with the pilot on foot, out of combat.
void NoteSharedLandingWalkOff(Player* pilot, OCRoute::LegTarget const& leg, bool atLeg)
{
    constexpr float WALK_OFF_RANGE = 120.0f;
    struct WalkOffState
    {
        int8 atPack = -1;
        std::array<uint8, VAROS_CONSTRUCT_PACK_COUNT> walkOffs{};
    };
    static std::mutex lock;
    static std::unordered_map<uint64, WalkOffState> states;

    bool const sharedLanding = leg.valid && leg.land && leg.hasPack && leg.packIndex >= 0 &&
                               static_cast<size_t>(leg.packIndex) < VAROS_CONSTRUCT_PACK_COUNT &&
                               leg.pos.GetExactDist(&leg.pack) >= 1.0f;
    bool const onLegFloor = pilot->GetExactDist2d(leg.pos) < WALK_OFF_RANGE &&
                            std::fabs(pilot->GetPositionZ() - leg.pos.GetPositionZ()) < LEG_ARRIVAL_HEIGHT;

    std::lock_guard<std::mutex> guard(lock);
    WalkOffState& state = states[pilot->GetGUID().GetRawValue()];
    if (!sharedLanding || !onLegFloor)
    {
        state.atPack = -1;
        return;
    }

    if (atLeg)
    {
        state.atPack = leg.packIndex;
        return;
    }

    if (state.atPack != leg.packIndex)
    {
        state.atPack = -1;
        return;
    }

    state.atPack = -1;
    size_t const pack = static_cast<size_t>(leg.packIndex);
    uint8 const count = ++state.walkOffs[pack];
    LOG_INFO("playerbots", "[oc landing walk-off] {}: on foot at ({:.1f},{:.1f},{:.1f}), {:.1f} yd off the landing "
             "({:.1f},{:.1f},{:.1f}) of pack {} at ({:.1f},{:.1f},{:.1f}) with it alive, {} of {}", pilot->GetName(),
             pilot->GetPositionX(), pilot->GetPositionY(), pilot->GetPositionZ(), pilot->GetExactDist2d(leg.pos),
             leg.pos.GetPositionX(), leg.pos.GetPositionY(), leg.pos.GetPositionZ(), pack, leg.pack.GetPositionX(),
             leg.pack.GetPositionY(), leg.pack.GetPositionZ(), uint32(count), uint32(PACK_LANDING_WALK_OFF_LIMIT));
    if (count >= PACK_LANDING_WALK_OFF_LIMIT && !packLandingWalkedOff[pack].exchange(true))
        LOG_INFO("playerbots", "[oc landing walk-off] pack {}: its shared landing is dropped, the group sets down by "
                 "the pack at ({:.1f},{:.1f},{:.1f})", pack, leg.pack.GetPositionX(), leg.pack.GetPositionY(),
                 leg.pack.GetPositionZ());
}
}  // namespace

// A human master picks the moment by mounting first (upstream behaviour). Otherwise the pilot mounts when the next
// island needs a flight and the group is out of combat; the others mount once the pilot rides, or to reach a pilot
// standing on another island.
bool OculusShouldMountDrake(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != OCULUS_MAP_ID || bot->GetVehicleBase() || !bot->IsAlive())
        return false;

    // Never call a drake into a running Eregos fight (npc_oculus_drake::IsSummonedBy despawns him on retail).
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetData(OCULUS_DATA_EREGOS) == IN_PROGRESS)
        return false;

    if (Player* human = RaidBotDuty::GetHumanMaster(botAI))
        return human->GetVehicleBase() != nullptr;

    if (bot->IsInCombat() || GetOculusDrakeGoal(bot) == OculusDrakeGoal::NONE || OCRoute::IsOnEntranceIsland(bot))
        return false;

    Player* pilot = OCRoute::GetPilot(botAI);
    if (!pilot)
        return false;

    OCRoute::LegTarget const leg = OCRoute::GetLegTarget(pilot);
    if (!leg.valid)
        return false;

    if (pilot == bot)
    {
        bool const atLeg = OCRoute::IsAtLegTarget(bot, leg);
        NoteSharedLandingWalkOff(bot, leg, atLeg);  // [b34_occ]
        if (atLeg || !OCRoute::IsGroupReadyToFly(bot))
            return false;

        // After a wipe the group comes back through the Nexus Portal one by one: the pilot calls its drake once
        // nobody is left on the entrance island, so the flight starts with the whole group on Drakos' island.
        std::string who;
        if (OCRoute::IsMemberOnEntranceIsland(bot, &who))
        {
            if (OCRoute::ShouldLog(bot, "mount-wait-entrance", OC_LOG_INTERVAL_MS))
                LOG_INFO("playerbots", "[oc mount wait entrance] {}: no drake yet, {} is still on the entrance island",
                         bot->GetName(), who);
            return false;
        }

        // A member who released runs back as a ghost and revives at the zone-in: wait for him (bounded), as a group
        // would, instead of leaving him to cross the rings alone on his drake.
        thread_local std::unordered_map<uint64, uint32> ghostWaitSince;
        uint32& since = ghostWaitSince[bot->GetGUID().GetRawValue()];
        if (!OCRoute::IsMemberRunningBack(bot, &who))
        {
            since = 0;
            return true;
        }

        uint32 const now = getMSTime();
        if (!since)
            since = now ? now : 1;
        if (getMSTimeDiff(since, now) >= GHOST_WAIT_TIMEOUT_MS)
            return true;

        if (OCRoute::ShouldLog(bot, "mount-wait-ghost", OC_LOG_INTERVAL_MS))
            LOG_INFO("playerbots", "[oc mount wait ghost] {}: no drake yet, {} is running back as a ghost ({} s)",
                     bot->GetName(), who, getMSTimeDiff(since, now) / IN_MILLISECONDS);
        return false;
    }

    if (pilot->GetVehicleBase())
        return true;

    return OCRoute::IsAtLegTarget(pilot, leg) && !OCRoute::IsAtLegTarget(bot, leg);
}

bool DrakeMountTrigger::IsActive() { return OculusShouldMountDrake(botAI); }

bool DrakeDismountTrigger::IsActive()
{
    if (bot->GetMapId() != OCULUS_MAP_ID || !bot->GetVehicleBase())
        return false;

    if (Player* human = RaidBotDuty::GetHumanMaster(botAI))
        return !human->GetVehicleBase();

    // Bot-led group: only step off once the drake has set down on the target island, never in mid-air.
    Player* pilot = OCRoute::GetPilot(botAI);
    if (!pilot)
        return false;

    OCRoute::LegTarget const leg = OCRoute::GetLegTarget(pilot);
    if (!leg.valid || !leg.land || (pilot != bot && !OCRoute::IsAtLegTarget(pilot, leg)))
        return false;

    // Nobody steps off before the pilot is down on its checked spot.
    if (pilot != bot && pilot->GetVehicleBase())
        return false;

    constexpr float LANDED_DISTANCE = 3.0f;
    // [b38_occ] was 4 (a rider stepped off up to 6 yd over the spot's floor)
    constexpr float LANDED_HEIGHT = 2.5f;
    Unit* drake = bot->GetVehicleBase();
    Position const spot = OCRoute::GetLandingSpot(botAI, leg);
    // [b38_occ] Bounded: a drake parked off a spot of a non-Varos leg (no floor-round check there) must not keep its
    // rider up over a floor of the wrong height for good.
    constexpr uint32 DISMOUNT_FLOOR_HOLD_MS = 10 * IN_MILLISECONDS;
    thread_local std::unordered_map<uint64, uint32> floorHoldSince;
    uint32& holdSince = floorHoldSince[bot->GetGUID().GetRawValue()];
    if (drake->GetExactDist2d(spot) >= LANDED_DISTANCE ||
        std::fabs(drake->GetPositionZ() - spot.GetPositionZ()) >= LANDED_HEIGHT)
    {
        holdSince = 0;
        return false;
    }

    // [b38_occ] ExitVehicle puts the rider down where the drake is, not on the spot: step off only with the spot's own
    // floor under the drake, at most LANDED_ABOVE_FLOOR below it (not over the brink of a ring or island, where the
    // next floor is the instance bottom, nor over a ledge of another level).
    constexpr float LANDED_ABOVE_FLOOR = LANDING_HEIGHT + 2.0f;
    constexpr float LANDED_FLOOR_DZ = 1.5f;
    float const spotFloor = spot.GetPositionZ() - LANDING_HEIGHT;
    float const under = bot->GetMap()->GetHeight(bot->GetPhaseMask(), drake->GetPositionX(), drake->GetPositionY(),
                                                 drake->GetPositionZ() + 1.0f, true, LANDING_FLOOR_SEARCH);
    if (under <= INVALID_HEIGHT || std::fabs(under - spotFloor) > LANDED_FLOOR_DZ ||
        drake->GetPositionZ() - under > LANDED_ABOVE_FLOOR)
    {
        uint32 const now = getMSTime();
        if (!holdSince)
            holdSince = now ? now : 1;
        bool const expired = under > INVALID_HEIGHT && getMSTimeDiff(holdSince, now) >= DISMOUNT_FLOOR_HOLD_MS;
        if (OCRoute::ShouldLog(bot, "dismount-no-floor", OC_LOG_INTERVAL_MS))
            LOG_INFO("playerbots", "[b38_occ] [oc dismount hold] {}: drake at ({:.1f},{:.1f},{:.1f}) {:.1f} yd off its "
                     "spot, floor under it {:.1f} (spot floor {:.1f}): {}", bot->GetName(), drake->GetPositionX(),
                     drake->GetPositionY(), drake->GetPositionZ(), drake->GetExactDist2d(spot), under, spotFloor,
                     expired ? "steps off anyway after 10 s" : "stays mounted");
        if (!expired)
            return false;
    }
    else
        holdSince = 0;

    // The pilot steps off only with every drake of the group over the island.
    return pilot != bot || OCRoute::IsGroupGatheredForLanding(bot, leg);
}

bool GroupFlyingTrigger::IsActive()
{
    if (bot->GetMapId() != OCULUS_MAP_ID || !bot->GetVehicleBase())
        return false;

    if (Player* human = RaidBotDuty::GetHumanMaster(botAI))
        return human->GetVehicleBase() != nullptr;

    return true;
}

bool DrakeCombatTrigger::IsActive()
{
    if (bot->GetMapId() != OCULUS_MAP_ID || !bot->GetVehicleBase())
        return false;

    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    return !targets.empty() || OCRoute::GetPullableEregos(bot, bot->GetVehicleBase());
}

// [b36_occ] Planar Anomalies only exist during Eregos' Planar Shift (boss immune, 18 s): the dodge owns the drake.
bool OcPlanarAnomalyTrigger::IsActive()
{
    Unit* drake = bot->GetVehicleBase();
    if (bot->GetMapId() != OCULUS_MAP_ID || !drake || !bot->IsAlive())
        return false;

    return !OCRoute::GetPlanarAnomalies(drake, OCIds::PLANAR_ANOMALY_SCAN_RANGE).empty();
}

// Energize Cores: on foot near Varos while he is engaged and standing in one of the four fixed zap cones.
bool VarosCloudstriderTrigger::IsActive()
{
    if (bot->GetMapId() != OCULUS_MAP_ID || bot->GetVehicleBase())
        return false;

    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetData(OCULUS_DATA_VAROS) != IN_PROGRESS)
        return false;

    constexpr float ENERGIZE_CORES_RANGE = 100.0f;
    constexpr float VAROS_BOUNDARY = 3.0f;
    Creature* varos = OCRoute::GetBoss(bot, OCULUS_DATA_VAROS);
    if (!varos || !varos->IsAlive() || !bot->IsWithinDistInMap(varos, ENERGIZE_CORES_RANGE))
        return false;

    return bot->GetExactDist2d(varos) < VAROS_BOUNDARY || OCRoute::IsInEnergizeCoresCone(varos, bot);
}

bool UromArcaneExplosionTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "mage-lord urom");
    if (!boss) { return false; }

    return bool(boss->FindCurrentSpellBySpellId(SPELL_EMPOWERED_ARCANE_EXPLOSION));
}

bool UromTimeBombTrigger::IsActive()
{
    return bot->HasAura(SPELL_TIME_BOMB);
}

bool OcNexusPortalTrigger::IsActive()
{
    if (bot->GetMapId() != OCULUS_MAP_ID || !bot->IsAlive() || bot->IsInCombat() || bot->GetVehicleBase() ||
        !OCRoute::IsOnEntranceIsland(bot))
        return false;

    // Each player clicks the portal for himself; the pilot goes first once the group stands with him, everybody
    // else follows a leader who already left the entrance island.
    Player* leader = OCRoute::GetPilot(botAI);
    if (!leader)
        leader = RaidBotDuty::GetHumanMaster(botAI);
    if (!leader)
        return false;

    // [b35_occ] ... or already on its way there: the walk is one spline to the portal, and only an active trigger
    // lets the action stop it for the laggers (OcUseNexusPortalAction).
    if (leader == bot)
        return DungeonInteract::IsGroupGathered(bot, bot, LEG_ARRIVAL_RADIUS) || OCRoute::IsWalkingToNexusPortal(bot);

    return leader->IsInWorld() && leader->GetMapId() == bot->GetMapId() &&
           leader->GetInstanceId() == bot->GetInstanceId() && !OCRoute::IsOnEntranceIsland(leader);
}

bool OcPullBossTrigger::IsActive()
{
    if (bot->GetMapId() != OCULUS_MAP_ID || !bot->IsAlive() || bot->IsInCombat() || bot->GetVehicleBase())
        return false;

    Creature* boss = OCRoute::GetPullableBoss(bot);
    if (!boss)
        return false;

    // The tank pulls; without a living tank the interactor does.
    bool puller = PlayerbotAI::IsTank(bot);
    if (!puller && DungeonInteract::IsInteractor(botAI))
    {
        puller = true;
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref && puller; ref = ref->next())
                if (Player* member = ref->GetSource(); DungeonInteract::IsActiveMember(bot, member) &&
                                                       member != bot && PlayerbotAI::IsTank(member))
                    puller = false;
    }

    return puller && DungeonInteract::IsGroupGathered(bot, boss, BOSS_PULL_RANGE + 5.0f);
}

bool OculusShouldHoldLanding(PlayerbotAI* botAI, Position* spot)
{
    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != OCULUS_MAP_ID || bot->GetVehicleBase() || !bot->IsAlive() || bot->IsInCombat() ||
        RaidBotDuty::GetHumanMaster(botAI))
        return false;

    Player* pilot = OCRoute::GetPilot(botAI);
    if (!pilot)
        return false;

    OCRoute::LegTarget const leg = OCRoute::GetLegTarget(pilot);
    if (!OCRoute::IsAtLegTarget(bot, leg))
        return false;

    // Only the landing itself is waited out: once nobody rides a drake any more the group is down, and the tank
    // walking out to pull a pack (45+ yd off the Varos ring landings) is no "group not gathered" to be held back for.
    if (!IsAnyMemberOnDrake(bot) || OCRoute::IsGroupLandedAtLeg(botAI, leg))
        return false;

    if (spot)
    {
        Position const air = OCRoute::GetLandingSpot(botAI, leg);
        spot->Relocate(air.GetPositionX(), air.GetPositionY(), air.GetPositionZ() - LANDING_HEIGHT);
    }
    return true;
}

bool OcLandingHoldTrigger::IsActive()
{
    Position spot;
    return OculusShouldHoldLanding(botAI, &spot) && bot->GetExactDist2d(spot) > LANDING_HOLD_SLACK;
}
