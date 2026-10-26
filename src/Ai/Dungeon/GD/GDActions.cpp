/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GDActions.h"
#include "DungeonInteract.h"
#include "InstanceScript.h"
#include "LastMovementValue.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "RaidBotDuty.h"
#include "WlkTacticLog.h"

#include <algorithm>
#include <cmath>  // [b35_gd] Poison Nova exit ring

namespace
{
constexpr float ALTAR_USE_MARGIN_YD = 1.5f;
// wider than GundrakEvent BRIDGE_ENTERED_YD (3 yd), so a bot stepping off the start never turns back to it
constexpr float BRIDGE_START_REACHED_YD = 4.0f;

// Where a swimmer leaves the central circle's pool: a surface point over the bottom (water) and the shore beyond it
// (dry ground), both in the navmesh part of the four bosses' platforms and the bridge's west landing (6061 polygons;
// tools/navmesh_probe.py on the server mmaps, bot filter).
struct GundrakPoolExit
{
    char const* name;
    float surfaceX;
    float surfaceY;
    float surfaceZ;
    float shoreX;
    float shoreY;
    float shoreZ;
};

constexpr GundrakPoolExit GUNDRAK_POOL_EXITS[] = {
    // polygon 4127 -> 4086 (19 yd): the south shore, dry on along the south corridor to Slad'ran (~33 polygons)
    { "south", 1681.6f, 653.7f, 123.8f, 1674.5f, 635.9f, 126.3f },
    // polygon 4247 -> 4269 (4 yd): under the Colossus terrace, dry on up its stairs to the Colossus (~33 polygons),
    // the bridge's west landing (~46) and Moorabi (~131)
    { "west", 1668.1f, 669.1f, 123.5f, 1668.4f, 672.5f, 126.0f },
};
constexpr size_t GUNDRAK_POOL_EXIT_COUNT = sizeof(GUNDRAK_POOL_EXITS) / sizeof(GUNDRAK_POOL_EXITS[0]);
constexpr size_t POOL_EXIT_SOUTH = 0;
constexpr size_t POOL_EXIT_WEST = 1;
constexpr float POOL_SHORE_MAX_PATH_YD = 60.0f;
constexpr uint32 POOL_RETRY_MS = 2 * IN_MILLISECONDS;
constexpr uint32 POOL_SWIM_SLACK_MS = IN_MILLISECONDS;

// [b34_gd] The deck's open stretch over the moat, between the statue island (navmesh ground to x ~1788) and the east
// landing (from x ~1797): tools/navmesh_probe.py 604 --at 1792 750 118.6 / 1792 737 118.6 finds no polygon within
// 5 yd at deck height, and water polygons (z 110.1) under both sides, joined to the mobs' spot under the landing.
constexpr float MOAT_DROP_X = 1792.0f;
constexpr float MOAT_DROP_DECK_Z = 118.6f;
// off the deck's side (half width 4 yd) over open water
constexpr float MOAT_DROP_SIDE_YD = 6.5f;
constexpr uint32 MOAT_DROP_SLACK_MS = IN_MILLISECONDS;
}  // namespace

// [b35_gd] Poison Nova (59842 heroic: 10 yd, 3.5 s cast, then a poison of ~1.6-1.9k every 1 s for 10 s) is left on a
// navmesh spot. gd-h 20261026-024524-gd-h-5H-27 (1171-1185 s, Slad'ran at 2 %): MoveAway's spots on his platform came
// back with z 0 (CheckCollisionAndGetValidCoords falls back to the grid height there: "going point to
// (1760.9,661.1,0.0) 129.4 yd", "last move to 0.0 yd (by avoid poison nova)", the stack walk's ledge_drop_refused at
// z 0.0), nobody got out, and the nova's poison killed the tank and the hunter 10 s later (the healer under it too).
// The spot is now the end of a navmesh path at the bot's own height, 14 yd from the boss.
namespace
{
constexpr float POISON_NOVA_RADIUS_YD = 10.0f;
constexpr float POISON_NOVA_EXIT_YD = POISON_NOVA_RADIUS_YD + 4.0f;
constexpr float POISON_NOVA_MIN_END_YD = POISON_NOVA_RADIUS_YD + 2.0f;
constexpr float POISON_NOVA_MAX_DZ = 3.0f;
// a path round a wall or down the stairs is no way out within the cast
constexpr float POISON_NOVA_MAX_DETOUR_YD = 6.0f;
// boss_slad_ran.cpp SpawnLoc (the stairs and the west entrance), averaged: the tank leaves away from them, so
// Slad'ran follows it back towards his platform once the cast ends.
constexpr float SLADRAN_SNAKE_SPAWN_X = 1751.7f;
constexpr float SLADRAN_SNAKE_SPAWN_Y = 633.0f;

bool FindPoisonNovaExit(Player* bot, Unit* boss, float& x, float& y, float& z)
{
    float const pi = static_cast<float>(M_PI);
    float baseAngle = bot->GetExactDist2d(boss) > 0.5f ? boss->GetAngle(bot) : bot->GetOrientation() + pi;
    if (PlayerbotAI::IsTank(bot))
        baseAngle = std::atan2(boss->GetPositionY() - SLADRAN_SNAKE_SPAWN_Y,
                               boss->GetPositionX() - SLADRAN_SNAKE_SPAWN_X);

    float const offsets[] = {0.0f,       pi / 8.0f,  -pi / 8.0f,        pi / 4.0f,        -pi / 4.0f,
                             3 * pi / 8, -3 * pi / 8, pi / 2.0f,        -pi / 2.0f,       3 * pi / 4,
                             -3 * pi / 4, pi};
    for (float offset : offsets)
    {
        float const angle = baseAngle + offset;
        float const destX = boss->GetPositionX() + std::cos(angle) * POISON_NOVA_EXIT_YD;
        float const destY = boss->GetPositionY() + std::sin(angle) * POISON_NOVA_EXIT_YD;

        PathGenerator path(bot);
        path.CalculatePath(destX, destY, bot->GetPositionZ(), false);
        uint32 const type = path.GetPathType();
        Movement::PointsArray const& points = path.GetPath();
        if (!(type & PATHFIND_NORMAL) || (type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) || points.empty())
            continue;

        G3D::Vector3 const& end = points.back();
        if (std::fabs(end.z - bot->GetPositionZ()) > POISON_NOVA_MAX_DZ ||
            boss->GetExactDist2d(end.x, end.y) < POISON_NOVA_MIN_END_YD)
            continue;

        if (path.getPathLength() > bot->GetExactDist2d(end.x, end.y) + POISON_NOVA_MAX_DETOUR_YD)
            continue;

        x = end.x;
        y = end.y;
        z = end.z;
        return true;
    }

    return false;
}
}  // namespace

bool AvoidPoisonNovaAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "slad'ran");
    if (!boss) { return false; }

    if (bot->GetExactDist2d(boss) >= POISON_NOVA_MIN_END_YD)
        return false;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!FindPoisonNovaExit(bot, boss, x, y, z))
    {
        RaidBotDuty::LogWorkaroundOnce(bot, "gd-poison-nova-exit",
                                       "Gundrak: no navmesh spot out of Slad'ran's Poison Nova found (14 yd ring)");
        return false;
    }

    if (bot->IsNonMeleeSpellCast(false))
        bot->CastStop();

    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED, true);
}

bool AttackSnakeWrapAction::Execute(Event /*event*/)
{
    Unit* snakeWrap = GundrakSladran::GetAssignedSnakeWrap(botAI);
    if (!snakeWrap) { return false; }

    if (AI_VALUE(Unit*, "current target") == snakeWrap) { return false; }

    return Attack(snakeWrap);
}

bool SladranStackOnTankAction::Execute(Event /*event*/)
{
    Player* tank = GundrakSladran::GetStackTank(botAI);
    if (!tank) { return false; }

    if (MoveTo(tank, GundrakSladran::STACK_CLOSE_TO_YD, MovementPriority::MOVEMENT_COMBAT))
        return true;

    // [b35_gd] The spot next to the tank can come back with z 0 on Slad'ran's platform (the grid height under it:
    // gd-h 20261026-024524 ledge_drop_refused "slad'ran stack on tank" to (1772.8,662.8,0.0) from z 129.2); the tank's
    // own position is ground the tank stands on.
    return MoveTo(tank->GetMapId(), tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ(), false, false,
                  false, false, MovementPriority::MOVEMENT_COMBAT);
}

bool SladranTankHoldAction::Execute(Event /*event*/)
{
    Unit* boss = GundrakSladran::GetTankHoldTarget(botAI);
    if (!boss) { return false; }

    return Attack(boss);
}

bool SladranTankParkAction::Execute(Event /*event*/)
{
    Unit* boss = GundrakSladran::GetTankParkBoss(botAI);
    Creature* creature = boss ? boss->ToCreature() : nullptr;
    if (!creature)
        return false;

    Position const& home = creature->GetHomePosition();
    return MoveTo(bot->GetMapId(), home.GetPositionX(), home.GetPositionY(), home.GetPositionZ(), false, false, false,
                  false, MovementPriority::MOVEMENT_COMBAT, true);
}

bool AvoidWhirlingSlashAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "gal'darah");
    if (!boss) { return false; }

    float distance = bot->GetExactDist2d(boss->GetPosition());
    float radius = 5.0f;
    float distanceExtra = 2.0f;

    if (distance < radius + distanceExtra)
    {
        if (PlayerbotAI::IsTank(bot))
        {
            // The boss chases tank during this, leads to jittery stutter-stepping
            // by the tank if we don't pre-move additional range. 2*radius seems ok
            return MoveAway(boss, (2.0f * radius) + distanceExtra - distance);
        }
        // else
        return MoveAway(boss, radius + distanceExtra - distance);
    }

    return false;
}

bool GundrakUseAltarAction::Execute(Event /*event*/)
{
    GameObject* altar = GundrakEvent::GetAltarToUse(botAI);
    if (!altar)
        return false;

    if (!altar->IsWithinDistInMap(bot, altar->GetInteractionDistance() - ALTAR_USE_MARGIN_YD))
        return MoveTo(altar->GetMapId(), altar->GetPositionX(), altar->GetPositionY(), altar->GetPositionZ(), false,
                      false, false, false, MovementPriority::MOVEMENT_NORMAL);

    if (!DungeonInteract::UseGameObject(bot, altar))
        return false;

    LOG_INFO("playerbots", "{} used {} (Gundrak)", bot->GetName(), altar->GetName());
    return true;
}

bool EngageLivingMojoAction::Execute(Event /*event*/)
{
    Creature* colossus = GundrakEvent::GetDormantColossus(botAI);
    if (!colossus)
        return false;

    Creature* mojo = GundrakEvent::FindLivingMojo(bot, colossus);
    if (!mojo)
    {
        RaidBotDuty::LogWorkaroundOnce(bot, "gd-colossus-mojo",
                                       "Gundrak: Drakkari Colossus is dormant without a Living Mojo next to it, "
                                       "needs honest tactic");
        return false;
    }

    return Attack(mojo);
}

bool GundrakCrossBridgeAction::Execute(Event /*event*/)
{
    if (!GundrakEvent::IsOnBridge(bot))
    {
        Position const* start = GundrakEvent::GetBridgeStart();
        if (bot->GetExactDist2d(start->GetPositionX(), start->GetPositionY()) > BRIDGE_START_REACHED_YD)
            return MoveTo(bot->GetMapId(), start->GetPositionX(), start->GetPositionY(), start->GetPositionZ(), false,
                          false, false, false, MovementPriority::MOVEMENT_NORMAL);

        // the leader waits at the start until the group stands with it
        if (DungeonInteract::IsInteractor(botAI) &&
            !GundrakEvent::IsReadyToStart(bot, bot, GundrakEvent::GATHER_YD))
            return false;
    }

    Position const* next = GundrakEvent::GetNextBridgeWaypoint(bot);
    if (!next)
        return false;

    RaidBotDuty::LogWorkaroundOnce(bot, "gd-bridge-path",
                                   "Gundrak: needs path fix - the bridge to Gal'darah (GO 192633/193188) is not in "
                                   "the mmaps, walking its deck in straight segments after the altars opened it");
    return WalkStraightTo(*next);
}

bool GundrakCrossBridgeAction::WalkStraightTo(Position const& point)
{
    float const x = point.GetPositionX();
    float const y = point.GetPositionY();
    float const z = point.GetPositionZ();
    if (bot->isMoving() && IsDuplicateMove(x, y, z))
        return true;

    if (!IsMovingAllowed())
        return false;

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    // no navmesh under the deck: a straight spline at run speed, the way a client walks over it.
    // [w24b-gd-deck-line] Not MovePoint: PointMovementGenerator::DoInitialize paths even generatePath=false
    // (MoveSplineInit::MoveTo(x, y, z, true)) and the PATHFIND_INCOMPLETE path ends in the moat (gd-n 2026-10-25
    // 115805: the deck point 1766.5,743.45,118.4 became a spline to 1767.1,750.6,108.8). An escort spline of three
    // points is followed as given.
    G3D::Vector3 const start(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    G3D::Vector3 const deck(x, y, z);
    Movement::PointsArray line = {start, (start + deck) * 0.5f, deck};
    MotionMaster* motionMaster = bot->GetMotionMaster();
    motionMaster->Clear();
    motionMaster->MoveSplinePath(&line);
    if (WlkTacticLog::Allow(bot, "gd bridge deck line"))
        LOG_INFO("playerbots", "[w24b-gd-deck-line] {} walks the bridge deck straight to ({:.1f}, {:.1f}, {:.1f}), "
                 "{:.1f} yd", bot->GetName(), x, y, z, bot->GetExactDist(x, y, z));

    float const delay = IN_MILLISECONDS * MoveDelay(bot->GetExactDist(x, y, z));
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), x, y, z, bot->GetOrientation(), delay, MovementPriority::MOVEMENT_FORCED);
    return true;
}

bool GundrakMoatDropAction::Execute(Event /*event*/)
{
    // the drop is under way: nothing else moves the bot meanwhile
    if (GundrakEvent::IsDroppingIntoMoat(bot))
        return true;

    Creature* mob = GundrakEvent::FindMoatAttacker(botAI);
    if (!mob || !IsMovingAllowed())
        return false;

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    // a player jumps off the deck's side into the water; the deck and the landing have no navmesh link to the moat,
    // so the escort spline (three points or more, followed as given) walks the deck, steps off and comes down
    float const deckY = GundrakEvent::GetBridgeStart()->GetPositionY();
    float const side = mob->GetPositionY() >= deckY ? 1.0f : -1.0f;
    float const waterY = deckY + side * MOAT_DROP_SIDE_YD;
    G3D::Vector3 const start(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    G3D::Vector3 const deck(MOAT_DROP_X, deckY, MOAT_DROP_DECK_Z);
    G3D::Vector3 const edge(MOAT_DROP_X, waterY, MOAT_DROP_DECK_Z);
    G3D::Vector3 const water(MOAT_DROP_X, waterY, GundrakEvent::MOAT_SURFACE_Z);
    Movement::PointsArray points = {start, deck, edge, water};

    MotionMaster* motionMaster = bot->GetMotionMaster();
    motionMaster->Clear();
    motionMaster->MoveSplinePath(&points);

    float const length = (deck - start).length() + (edge - deck).length() + (water - edge).length();
    uint32 const durationMs = static_cast<uint32>(IN_MILLISECONDS * MoveDelay(length)) + MOAT_DROP_SLACK_MS;
    GundrakEvent::NoteMoatDrop(bot, durationMs);
    float const delay =
        std::min(static_cast<float>(durationMs), static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove));
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), water.x, water.y, water.z, bot->GetOrientation(), delay,
             MovementPriority::MOVEMENT_FORCED);

    LOG_INFO("playerbots", "[b34_gd] moat drop: {} at ({:.1f}, {:.1f}, {:.1f}) drops into the moat at ({:.1f}, "
             "{:.1f}, {:.1f}) after {} {} ({:.1f}, {:.1f}, {:.1f}), {:.1f} yd, in combat with it and no attacker up "
             "here", bot->GetName(), start.x, start.y, start.z, water.x, water.y, water.z, mob->GetName(),
             mob->GetEntry(), mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ(), bot->GetExactDist(mob));
    return true;
}

bool GundrakMoatExitAction::Execute(Event /*event*/)
{
    Position const* start = GundrakEvent::GetBridgeStart();
    if (WlkTacticLog::Allow(bot, "gd moat exit"))
        LOG_INFO("playerbots", "[b34_gd] moat exit: {} at ({:.1f}, {:.1f}, {:.1f}) walks back to the bridge start "
                 "({:.1f}, {:.1f}, {:.1f})", bot->GetName(), bot->GetPositionX(), bot->GetPositionY(),
                 bot->GetPositionZ(), start->GetPositionX(), start->GetPositionY(), start->GetPositionZ());

    return MoveTo(bot->GetMapId(), start->GetPositionX(), start->GetPositionY(), start->GetPositionZ(), false, false,
                  false, false, MovementPriority::MOVEMENT_NORMAL);
}

bool GundrakCircleRouteAction::IsLeavingPool() const
{
    return _swimStartMs && !bot->movespline->Finalized() &&
           getMSTimeDiff(_swimStartMs, getMSTime()) < _swimDurationMs + POOL_SWIM_SLACK_MS;
}

bool GundrakCircleRouteAction::isUseful()
{
    if (IsLeavingPool())
        return true;

    return !_refusedMs || getMSTimeDiff(_refusedMs, getMSTime()) >= POOL_RETRY_MS;
}

bool GundrakCircleRouteAction::Execute(Event /*event*/)
{
    // the swim out is under way: keep it, nothing else moves the bot meanwhile
    if (IsLeavingPool())
        return true;

    if (!IsMovingAllowed())
        return false;

    uint32 const now = getMSTime();
    InstanceScript* instance = bot->GetInstanceScript();
    bool const sladranFirst = instance && instance->GetBossState(GundrakIds::DATA_SLAD_RAN) != DONE;
    size_t const first = sladranFirst ? POOL_EXIT_SOUTH : POOL_EXIT_WEST;

    // Where the bottom touches the shore a complete walk exists: not stranded, the usual movement goes on.
    GundrakPoolExit const& preferred = GUNDRAK_POOL_EXITS[first];
    PathGenerator direct(bot);
    direct.CalculatePath(preferred.shoreX, preferred.shoreY, preferred.shoreZ);
    if (direct.GetPathType() == PATHFIND_NORMAL)
    {
        _refusedMs = now;
        return false;
    }

    for (size_t index = 0; index < GUNDRAK_POOL_EXIT_COUNT; ++index)
    {
        GundrakPoolExit const& poolExit = GUNDRAK_POOL_EXITS[(first + index) % GUNDRAK_POOL_EXIT_COUNT];

        // leg 1: the straight swim up, in sight and through water all the way
        if (!bot->IsWithinLOS(poolExit.surfaceX, poolExit.surfaceY, poolExit.surfaceZ) ||
            !StraightLineHasGround(bot, poolExit.surfaceX, poolExit.surfaceY, poolExit.surfaceZ))
            continue;

        // leg 2: a complete navmesh walk from the surface point to the shore
        PathGenerator shore(bot);
        shore.CalculatePath(poolExit.surfaceX, poolExit.surfaceY, poolExit.surfaceZ, poolExit.shoreX, poolExit.shoreY,
                            poolExit.shoreZ, false);
        Movement::PointsArray const& walk = shore.GetPath();
        if (shore.GetPathType() != PATHFIND_NORMAL || walk.size() < 2 ||
            shore.getPathLength() > POOL_SHORE_MAX_PATH_YD)
            continue;

        // three points or more: an escort spline is followed as given, not pathed again from the bottom
        G3D::Vector3 const start(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        G3D::Vector3 const surface(poolExit.surfaceX, poolExit.surfaceY, poolExit.surfaceZ);
        Movement::PointsArray points = { start, (start + surface) * 0.5f, surface };
        points.insert(points.end(), walk.begin() + 1, walk.end());

        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);

        MotionMaster* motionMaster = bot->GetMotionMaster();
        motionMaster->Clear();
        motionMaster->MoveSplinePath(&points);

        float const length = (surface - start).length() + shore.getPathLength();
        _swimStartMs = now;
        _swimDurationMs = static_cast<uint32>(IN_MILLISECONDS * MoveDelay(length));
        float const delay =
            std::min(static_cast<float>(_swimDurationMs), static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove));
        AI_VALUE(LastMovement&, "last movement")
            .Set(bot->GetMapId(), poolExit.shoreX, poolExit.shoreY, poolExit.shoreZ, bot->GetOrientation(), delay,
                 MovementPriority::MOVEMENT_FORCED);

        if (WlkTacticLog::Allow(bot, "gd circle route"))
            LOG_INFO("playerbots", "[gd circle route] {} leaves the pool bottom at ({:.1f}, {:.1f}, {:.1f}): swims "
                     "up to the {} exit ({:.1f}, {:.1f}, {:.1f}), then {:.1f} yd to the shore ({:.1f}, {:.1f}, {:.1f})",
                     bot->GetName(), start.x, start.y, start.z, poolExit.name, poolExit.surfaceX, poolExit.surfaceY,
                     poolExit.surfaceZ, shore.getPathLength(), poolExit.shoreX, poolExit.shoreY, poolExit.shoreZ);
        return true;
    }

    _refusedMs = now;
    if (WlkTacticLog::Allow(bot, "gd circle route refused"))
        LOG_INFO("playerbots", "[gd circle route refused] {} on the pool bottom at ({:.1f}, {:.1f}, {:.1f}): no pool "
                 "exit in sight with a complete walk to its shore",
                 bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    return false;
}
