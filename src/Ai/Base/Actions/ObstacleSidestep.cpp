/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [obstacle_sidestep] A walk round a solid game object standing on the bot's path.
//
// The navmesh is built without game objects, so a path runs straight through a decoration with a collision model
// (Hallow's End pumpkins and ghosts on the Stratholme road, cos-h 20261025-153805 / 150737): the core's collision
// (Map::CheckCollisionAndGetValidCoords, the dynamic tree) ends the spline at the model and the bot stands there,
// re-pathing into it every tick. A player steps round it. So does the bot: when its move makes no progress and such
// an object sits within SIDESTEP_LOOK_AHEAD yd ahead on its path, it steps SIDESTEP_OFFSETS yd to the side onto
// navmesh and ground, walks past the object on that line, and its own action paths to the goal again from there.
// Every leg is checked like any straight walk: line of sight (static and dynamic collision, at the knee and at half
// the bot's height), ground under every step, navmesh under both ends. Nothing is clipped through.

#include "BotMoveEvents.h"
#include "BotPath.h"  // [lead109]
#include "MovementActions.h"

#include "CellImpl.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "NearestGameObjects.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <list>
#include <mutex>
#include <unordered_map>

namespace
{
    std::mutex eventLock;
    std::unordered_map<ObjectGuid, std::deque<BotMoveEvents::Event>> events;
}

namespace BotMoveEvents
{
    void Push(ObjectGuid bot, Event event)
    {
        std::lock_guard<std::mutex> lock(eventLock);
        std::deque<Event>& queue = events[bot];
        if (queue.size() >= MAX_QUEUED_PER_BOT)
            queue.pop_front();
        queue.push_back(std::move(event));
    }

    bool Take(ObjectGuid bot, Event& event)
    {
        std::lock_guard<std::mutex> lock(eventLock);
        auto itr = events.find(bot);
        if (itr == events.end() || itr->second.empty())
            return false;

        event = std::move(itr->second.front());
        itr->second.pop_front();
        return true;
    }

    void ClearAll()
    {
        std::lock_guard<std::mutex> lock(eventLock);
        events.clear();
    }
}

namespace
{
    // A move "makes no progress": the bot stands within STUCK_RADIUS of where it stood STUCK_MS ago, not moving,
    // with its goal farther than GOAL_REACHED.
    constexpr uint32 STUCK_MS = 2500;
    constexpr float STUCK_RADIUS = 1.0f;
    constexpr float GOAL_REACHED = 3.0f;
    // The object counts when its footprint starts within this many yards ahead on the path.
    constexpr float SIDESTEP_LOOK_AHEAD = 4.0f;
    // Objects searched round the bot: the look-ahead plus the largest decoration's half size (G_Pumpkin_01 at
    // scale 4 is about 3 yd across its footprint's half diagonal).
    constexpr float OBJECT_SEARCH_RANGE = SIDESTEP_LOOK_AHEAD + 6.0f;
    // The bot's own half width on top of the object's footprint.
    constexpr float WALKER_HALF_WIDTH = 0.5f;
    // A bot this close to the footprint touches the model: the core's collision ray of every new move starts at it
    // (CheckCollisionAndGetValidCoords), whatever the walk's direction (cos-h 153805: three members stood on the
    // identical spot by a pumpkin's flank, "MoveTo starts no move").
    constexpr float TOUCH_MARGIN = 1.0f;
    // The step to the side, smallest first.
    constexpr float SIDESTEP_OFFSETS[] = {2.0f, 3.0f, 4.0f};
    // The walk past: to the far side of the footprint and this much further, within these bounds.
    constexpr float PASS_MARGIN = 2.0f;
    constexpr float PASS_MIN = 3.0f;
    constexpr float PASS_MAX = 10.0f;
    // Ground: the step's floor is within STEP_MAX_DZ of the bot's, and no step of a leg drops or climbs more than
    // LEG_MAX_DZ between two LEG_SAMPLE points.
    constexpr float STEP_MAX_DZ = 1.5f;
    constexpr float LEG_MAX_DZ = 1.0f;
    constexpr float LEG_SAMPLE = 1.0f;
    constexpr float GROUND_PROBE_UP = 2.0f;
    constexpr float GROUND_SEARCH = 5.0f;
    // Navmesh under a point: the nearest polygon within this half width, its height within NAVMESH_MAX_DZ.
    constexpr float NAVMESH_HALF_WIDTH = 0.6f;
    constexpr float NAVMESH_HALF_HEIGHT = 2.5f;
    constexpr float NAVMESH_MAX_DZ = 1.0f;
    // The knee: short decorations (a pumpkin) are under half the bot's height.
    constexpr float KNEE_HEIGHT = 0.5f;
    // Retry pacing: one try every RETRY_MS; after MAX_FAILS tries that found no way round, none for GIVE_UP_MS.
    constexpr uint32 RETRY_MS = 3 * IN_MILLISECONDS;
    constexpr uint32 MAX_FAILS = 3;
    constexpr uint32 GIVE_UP_MS = 20 * IN_MILLISECONDS;
    // A leg not walked within this time is dropped (the bot was stopped by something else).
    constexpr uint32 LEG_TIMEOUT_MS = 8 * IN_MILLISECONDS;
    constexpr float LEG_REACHED = 1.2f;
    constexpr uint32 CALL_GAP_MS = 1500;

    struct SidestepState
    {
        float anchorX = 0.0f;
        float anchorY = 0.0f;
        float anchorZ = 0.0f;
        uint32 anchorMs = 0;
        uint32 lastCallMs = 0;  // a walk that was not asked for a while starts its stuck clock again
        uint32 lastTryMs = 0;
        uint32 fails = 0;
        uint32 giveUpMs = 0;  // when it last gave up (0: never)
        // The walk round, in two legs: to the side (step), then past the object (pass).
        bool walking = false;
        bool passIssued = false;
        uint32 legMs = 0;
        G3D::Vector3 step;
        G3D::Vector3 pass;
    };

    std::mutex stateLock;
    std::unordered_map<ObjectGuid, SidestepState> states;

    SidestepState LoadState(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(stateLock);
        return states[guid];
    }

    void StoreState(ObjectGuid guid, SidestepState const& state)
    {
        std::lock_guard<std::mutex> lock(stateLock);
        states[guid] = state;
    }

    float Dist2d(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

    float GroundAt(Player* bot, float x, float y, float z)
    {
        return bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, z + GROUND_PROBE_UP, true, GROUND_SEARCH);
    }

    bool OnNavmesh(Player* bot, G3D::Vector3 const& point)
    {
        dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
        if (!query)
            return false;

        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_GROUND_STEEP));
        filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME | NAV_WATER));
        // Detour works in (y, z, x).
        float const center[3] = {point.y, point.z, point.x};
        float const extents[3] = {NAVMESH_HALF_WIDTH, NAVMESH_HALF_HEIGHT, NAVMESH_HALF_WIDTH};
        float nearest[3];
        dtPolyRef ref = 0;
        if (dtStatusFailed(query->findNearestPoly(center, extents, &filter, &ref, nearest)) || !ref)
            return false;

        return Dist2d(nearest[2], nearest[0], point.x, point.y) <= NAVMESH_HALF_WIDTH &&
               std::fabs(nearest[1] - point.z) <= NAVMESH_MAX_DZ;
    }

    // Free of static and dynamic collision at the knee and at half the walker's height.
    bool LegClear(Player* bot, G3D::Vector3 const& from, G3D::Vector3 const& to)
    {
        Map* map = bot->GetMap();
        float const half = bot->GetCollisionHeight() * 0.5f;
        for (float height : {KNEE_HEIGHT, half})
        {
            if (!map->isInLineOfSight(from.x, from.y, from.z + height, to.x, to.y, to.z + height, bot->GetPhaseMask(),
                                      LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                return false;
        }

        return true;
    }

    // Ground under every LEG_SAMPLE yd of the leg, no step up or down over LEG_MAX_DZ. The end's floor in `endZ`.
    bool LegHasGround(Player* bot, G3D::Vector3 const& from, float toX, float toY, float& endZ)
    {
        float const length = Dist2d(from.x, from.y, toX, toY);
        uint32 const steps = std::max<uint32>(1, static_cast<uint32>(std::ceil(length / LEG_SAMPLE)));
        float previous = from.z;
        for (uint32 i = 1; i <= steps; ++i)
        {
            float const t = static_cast<float>(i) / static_cast<float>(steps);
            float const px = from.x + (toX - from.x) * t;
            float const py = from.y + (toY - from.y) * t;
            float const ground = GroundAt(bot, px, py, previous);
            if (ground <= INVALID_HEIGHT || std::fabs(ground - previous) > LEG_MAX_DZ)
                return false;
            previous = ground;
        }

        endZ = previous;
        return true;
    }

    struct Blocker
    {
        GameObject* go = nullptr;
        float along = 0.0f;    // of the footprint's centre, on the walk direction
        float lateral = 0.0f;  // of the footprint's centre, left of the walk direction positive
        float reach = 0.0f;    // the footprint's half diagonal
    };

    bool IsSolidObstacle(GameObject* go, Player* bot)
    {
        if (!go || !go->isSpawned() || !go->m_model || !go->m_model->isEnabled() || go->m_model->IsMapObject())
            return false;
        if (!(go->GetPhaseMask() & bot->GetPhaseMask()) || !bot->GetMap()->ContainsGameObjectModel(*go->m_model))
            return false;

        // Doors are the closed-door guard's (GuardClosedDoor); transports and buildings are walked on, not round.
        switch (go->GetGoType())
        {
            case GAMEOBJECT_TYPE_DOOR:
            case GAMEOBJECT_TYPE_TRANSPORT:
            case GAMEOBJECT_TYPE_MO_TRANSPORT:
            case GAMEOBJECT_TYPE_DESTRUCTIBLE_BUILDING:
                return false;
            default:
                return true;
        }
    }

    // The nearest solid object whose footprint lies on the walk within SIDESTEP_LOOK_AHEAD yd, confirmed by the
    // dynamic tree: its model stops a ray along the walk at the knee or at half the bot's height.
    bool FindBlocker(Player* bot, float dirX, float dirY, Blocker& found)
    {
        std::list<GameObject*> objects;
        AnyGameObjectInObjectRangeCheck check(bot, OBJECT_SEARCH_RANGE);
        Acore::GameObjectListSearcher<AnyGameObjectInObjectRangeCheck> searcher(bot, objects, check);
        Cell::VisitObjects(bot, searcher, OBJECT_SEARCH_RANGE);

        float const bx = bot->GetPositionX();
        float const by = bot->GetPositionY();
        float const bz = bot->GetPositionZ();
        float const top = bz + bot->GetCollisionHeight();
        bool any = false;
        for (GameObject* go : objects)
        {
            if (!IsSolidObstacle(go, bot))
                continue;

            G3D::AABox const& bounds = go->m_model->GetBounds();
            G3D::Vector3 const lo = bounds.low();
            G3D::Vector3 const hi = bounds.high();
            if (hi.z < bz + KNEE_HEIGHT * 0.5f || lo.z > top)
                continue;  // under the feet or overhead

            float const cx = (lo.x + hi.x) * 0.5f;
            float const cy = (lo.y + hi.y) * 0.5f;
            float const halfSide = std::max(hi.x - lo.x, hi.y - lo.y) * 0.5f;
            float const reach = Dist2d(lo.x, lo.y, hi.x, hi.y) * 0.5f;
            float const along = (cx - bx) * dirX + (cy - by) * dirY;
            float const lateral = (cy - by) * dirX - (cx - bx) * dirY;
            bool const touching = Dist2d(cx, cy, bx, by) <= halfSide + WALKER_HALF_WIDTH + TOUCH_MARGIN;
            bool const ahead = along + halfSide >= 0.0f && along - halfSide <= SIDESTEP_LOOK_AHEAD &&
                               std::fabs(lateral) <= halfSide + WALKER_HALF_WIDTH;
            if (!touching && !ahead)
                continue;

            if (any && along >= found.along)
                continue;

            float const rayLength = std::max(along + halfSide, 1.0f);
            bool hit = touching;
            for (float height : {KNEE_HEIGHT, bot->GetCollisionHeight() * 0.5f})
            {
                G3D::Vector3 const from(bx, by, bz + height);
                G3D::Vector3 const to(bx + dirX * rayLength, by + dirY * rayLength, bz + height);
                G3D::Vector3 hitPos;
                if (bot->GetMap()->GetDynamicMapTree().GetObjectHitPos(bot->GetPhaseMask(), from, to, hitPos, 0.0f))
                {
                    hit = true;
                    break;
                }
            }
            if (!hit)
                continue;  // its mesh leaves the walk free (an arch, a frame)

            found.go = go;
            found.along = along;
            found.lateral = lateral;
            found.reach = reach;
            any = true;
        }

        return any;
    }

    // The walk's direction where the bot stands: towards the first corner of its own path at least a yard away (the
    // corner the spline was heading for when the model stopped it), else straight at the goal.
    bool WalkDirection(Player* bot, float gx, float gy, float gz, float& dirX, float& dirY)
    {
        float tx = gx;
        float ty = gy;
        BotPath const path = FindBotPath(bot, gx, gy, gz);  // [lead109]
        if (path.valid)
        {
            for (G3D::Vector3 const& point : path.points)
            {
                if (Dist2d(point.x, point.y, bot->GetPositionX(), bot->GetPositionY()) >= 1.0f)
                {
                    tx = point.x;
                    ty = point.y;
                    break;
                }
            }
        }

        float const dx = tx - bot->GetPositionX();
        float const dy = ty - bot->GetPositionY();
        float const length = std::hypot(dx, dy);
        if (length < 0.1f)
            return false;

        dirX = dx / length;
        dirY = dy / length;
        return true;
    }
}

bool MovementAction::SidestepObstacle(uint32 mapId, float x, float y, float z)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || mapId != bot->GetMapId() || bot->IsBeingTeleported() ||
        bot->IsFlying() || bot->isSwimming() || bot->GetVehicle() || bot->GetTransport() || !bot->GetMap())
        return false;

    uint32 const now = getMSTime();
    ObjectGuid const guid = bot->GetGUID();
    SidestepState state = LoadState(guid);
    bool const resumed = !state.lastCallMs || getMSTimeDiff(state.lastCallMs, now) > CALL_GAP_MS;
    state.lastCallMs = now ? now : 1;
    G3D::Vector3 const position(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    MovementPriority const priority =
        bot->IsInCombat() ? MovementPriority::MOVEMENT_FORCED : MovementPriority::MOVEMENT_COMBAT;

    // A walk round under way: the step to the side, then the walk past the object. The caller's own move waits.
    if (state.walking)
    {
        if (getMSTimeDiff(state.legMs, now) > LEG_TIMEOUT_MS)
            state.walking = false;
        else if (bot->isMoving())
        {
            StoreState(guid, state);
            return true;
        }
        else if (!state.passIssued && Dist2d(position.x, position.y, state.step.x, state.step.y) <= LEG_REACHED)
        {
            if (MoveStraight(mapId, state.pass.x, state.pass.y, state.pass.z, true, priority, true))
            {
                state.passIssued = true;
                state.legMs = now;
                StoreState(guid, state);
                return true;
            }
            state.walking = false;
        }
        else
        {
            // Past the object (or stopped on the way: the caller's path is tried again from here).
            state.walking = false;
        }

        state.anchorX = position.x;
        state.anchorY = position.y;
        state.anchorZ = position.z;
        state.anchorMs = now;
        StoreState(guid, state);
        return false;
    }

    // Progress: the bot left the spot it stood on. Not "is moving": a spline re-issued into the model every tick
    // starts and ends on the spot.
    if (!state.anchorMs || resumed || bot->GetExactDist(state.anchorX, state.anchorY, state.anchorZ) > STUCK_RADIUS)
    {
        if (state.anchorMs && !resumed)
            state.fails = 0;
        state.anchorX = position.x;
        state.anchorY = position.y;
        state.anchorZ = position.z;
        state.anchorMs = now;
        StoreState(guid, state);
        return false;
    }

    if (bot->GetExactDist(x, y, z) <= GOAL_REACHED || getMSTimeDiff(state.anchorMs, now) < STUCK_MS ||
        (state.giveUpMs && getMSTimeDiff(state.giveUpMs, now) < GIVE_UP_MS) ||
        (state.lastTryMs && getMSTimeDiff(state.lastTryMs, now) < RETRY_MS))
    {
        StoreState(guid, state);
        return false;
    }

    state.lastTryMs = now;

    float dirX = 0.0f;
    float dirY = 0.0f;
    Blocker blocker;
    if (!WalkDirection(bot, x, y, z, dirX, dirY) || !FindBlocker(bot, dirX, dirY, blocker))
    {
        StoreState(guid, state);
        return false;  // stuck on something else: the caller's own fallbacks
    }

    // Round the side the object leaves wider first (its centre left of the walk: step right), then the other.
    float const preferred = blocker.lateral > 0.0f ? -1.0f : 1.0f;
    float const passLength = std::clamp(blocker.along + blocker.reach + PASS_MARGIN, PASS_MIN, PASS_MAX);
    for (float side : {preferred, -preferred})
    {
        // Left of the walk direction is (-dirY, dirX).
        float const sideX = -dirY * side;
        float const sideY = dirX * side;
        for (float offset : SIDESTEP_OFFSETS)
        {
            // The pass line must run clear of the footprint.
            if (std::fabs(side * offset - blocker.lateral) <= WALKER_HALF_WIDTH)
                continue;

            float stepZ = 0.0f;
            float const stepX = position.x + sideX * offset;
            float const stepY = position.y + sideY * offset;
            if (!LegHasGround(bot, position, stepX, stepY, stepZ) || std::fabs(stepZ - position.z) > STEP_MAX_DZ)
                continue;

            G3D::Vector3 const step(stepX, stepY, stepZ);
            if (!OnNavmesh(bot, step) || !LegClear(bot, position, step))
                continue;

            float passZ = 0.0f;
            float const passX = stepX + dirX * passLength;
            float const passY = stepY + dirY * passLength;
            if (!LegHasGround(bot, step, passX, passY, passZ))
                continue;

            G3D::Vector3 const pass(passX, passY, passZ);
            if (!OnNavmesh(bot, pass) || !LegClear(bot, step, pass))
                continue;

            if (!MoveStraight(mapId, step.x, step.y, step.z, true, priority, true))
                continue;

            state.walking = true;
            state.passIssued = false;
            state.legMs = now;
            state.step = step;
            state.pass = pass;
            state.fails = 0;
            StoreState(guid, state);

            GameObject* go = blocker.go;
            LOG_INFO("playerbots",
                     "[obstacle_sidestep] {} ({}): GO {} '{}' (type {}) at ({:.1f},{:.1f},{:.1f}) {:.1f} yd ahead "
                     "blocks the walk to ({:.1f},{:.1f},{:.1f}); steps {:.0f} yd {} to ({:.1f},{:.1f},{:.1f}), past "
                     "to ({:.1f},{:.1f},{:.1f})",
                     bot->GetName(), getName(), go->GetEntry(), go->GetName(), static_cast<uint32>(go->GetGoType()),
                     go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(), blocker.along, x, y, z, offset,
                     side > 0.0f ? "left" : "right", step.x, step.y, step.z, pass.x, pass.y, pass.z);
            BotMoveEvents::Push(guid, BotMoveEvents::Event{"obstacle_sidestep", {}, {}}
                                          .Str("name", bot->GetName())
                                          .Str("action", getName())
                                          .Num("goEntry", go->GetEntry())
                                          .Str("goName", go->GetName())
                                          .Num("goGuid", go->GetSpawnId())
                                          .Num("goType", static_cast<uint32>(go->GetGoType()))
                                          .Real("goX", go->GetPositionX())
                                          .Real("goY", go->GetPositionY())
                                          .Real("goZ", go->GetPositionZ())
                                          .Real("ahead", blocker.along)
                                          .Real("offset", offset)
                                          .Str("side", side > 0.0f ? "left" : "right")
                                          .Real("x", position.x).Real("y", position.y).Real("z", position.z)
                                          .Real("stepX", step.x).Real("stepY", step.y).Real("stepZ", step.z)
                                          .Real("passX", pass.x).Real("passY", pass.y).Real("passZ", pass.z)
                                          .Real("toX", x).Real("toY", y).Real("toZ", z));
            return true;
        }
    }

    ++state.fails;
    if (state.fails >= MAX_FAILS)
    {
        state.fails = 0;
        state.giveUpMs = now ? now : 1;
        GameObject* go = blocker.go;
        LOG_INFO("playerbots",
                 "[obstacle_sidestep] {} ({}): no free side round {} '{}' at ({:.1f},{:.1f},{:.1f}) after {} tries "
                 "(navmesh, ground or collision on every {}-{} yd step): left to the caller's fallbacks",
                 bot->GetName(), getName(), go->GetEntry(), go->GetName(), go->GetPositionX(), go->GetPositionY(),
                 go->GetPositionZ(), MAX_FAILS, SIDESTEP_OFFSETS[0], SIDESTEP_OFFSETS[2]);
        BotMoveEvents::Push(guid, BotMoveEvents::Event{"obstacle_sidestep_failed", {}, {}}
                                      .Str("name", bot->GetName())
                                      .Str("action", getName())
                                      .Num("goEntry", go->GetEntry())
                                      .Str("goName", go->GetName())
                                      .Real("goX", go->GetPositionX())
                                      .Real("goY", go->GetPositionY())
                                      .Real("goZ", go->GetPositionZ())
                                      .Real("x", position.x).Real("y", position.y).Real("z", position.z)
                                      .Real("toX", x).Real("toY", y).Real("toZ", z));
    }

    StoreState(guid, state);
    return false;
}
