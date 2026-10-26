/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RangedPositionActions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Creature.h"
#include "Event.h"
#include "Group.h"
#include "HealAssignmentValues.h"
#include "LastMovementValue.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "AggressiveTargetValue.h"
#include "ChooseTargetActions.h"
#include "PullStrategy.h"
#include "TerrainHazard.h"
#include "Timer.h"

namespace
{
    constexpr float PACK_RADIUS = 15.0f;               // enemies this close to the tank are the pack it holds
    constexpr float ENEMY_SCAN_RADIUS = 45.0f;         // enemies further from the bot do not shape its spot
    // Closer than this to an enemy fighting someone else (inside its melee reach, where a cleave or a frontal hits):
    // move. Was 6 yd, which sent healers and casters 20 yd away from any mob near them (user, Stockade 2026-10-24:
    // «излишне трусливые — отбегают на максимальный рейндж... зачем отбегать, если можно бить с места?»).
    constexpr float MELEE_DANGER_GAP = 2.0f;
    constexpr float DANGER_STEP_OUT = 7.0f;  // [b32_pull] was 5: past MELEE_DANGER_GAP of the real melee reach
              // a step out of melee danger ends this far past the mob's reach
    constexpr float DANGER_SAFE_GAP = 5.5f;  // [b32_pull] was 4, see MeleeGapTo
              // ... and keeps this much room to every enemy
    constexpr float SAFE_GAP = 10.0f;                  // a spot keeps at least this much room to every enemy
    constexpr float SPOT_DISTANCE_FROM_PACK = 20.0f;   // the spot sits this far from the tank's target
    constexpr float SPOT_DISTANCE_FROM_TANK = 15.0f;   // ... or from the tank when it has no target yet
    constexpr float HEALER_RANGE_MARGIN = 3.0f;        // a healer moves in this far inside its heal range
    constexpr float LEASH_SPOT_MARGIN = 6.0f;          // a leashed ranged spot keeps this much inside the leash
    constexpr float CHASE_REFUGE_REACHED = 6.0f;       // a chased bot this close to the tank stays by it
    constexpr float CHASE_REFUGE_MAX = 40.0f;          // further than this the walk to the tank is no refuge
    constexpr uint32 MELEE_CHASED_BY = 2;              // [melee_back_to_tank] a melee dps with this many mobs on it
    constexpr float HEALER_SIGHT_CHECK = 40.0f;        // within this distance a lost line of sight also moves it
    constexpr float SPOT_MAX_TANK_DISTANCE = 28.0f;    // a healer's spot stays well inside heal range of the tank
    constexpr float SPOT_MAX_TARGET_DISTANCE = 28.0f;  // a ranged spot stays inside spell range of the pack
    constexpr float SPOT_MAX_HEIGHT_DIFFERENCE = 6.0f;
    constexpr float SPREAD_STEP = 0.26f;               // ~15 degrees between the spots of different ranged members
    constexpr float APPROACH_DISTANCE = 20.0f;         // no spot found: walk until this close to the tank
    constexpr float APPROACH_DISTANCE_NO_SIGHT = 8.0f;
    constexpr uint32 KEEP_MOVING_MS = 3000;
    constexpr float SAME_SPOT = 2.0f;
    // A ranged bot that just took a spot stands and casts there for a while, the way a player does: a new spot within
    // this time is taken only for an urgent reason (an enemy right on it, far outside the leash, chased).
    // Wave 2026-10-24: hunters and mages moved in 25 % of fight samples, 13 % inside a good band with a static pack.
    constexpr uint32 REPOSITION_HOLD_MS = 3000;
    constexpr uint32 UNSAFE_SPOT_WAIT_MS = 1500;  // an approach that found only unsafe spots looks again after this
    // A bot that stands well enough looks for a better view of the fight (SpotView) this often...
    constexpr uint32 VIEW_CHECK_MS = 3000;
    constexpr float VIEW_MIN_GAIN = 1.0f;     // ... takes one only if it scores this much more than where it stands
    constexpr float VIEW_MAX_WALK = 15.0f;    // ... and is at most this far away
    constexpr float VIEW_SCORE_COST = 3.0f;   // a spot's view score is worth this many yards of walk
    constexpr size_t VIEW_PACK_MOBS = 4;      // the next pack's view is rated on this many of its nearest mobs
    constexpr size_t VIEW_MAX_RATED = 6;      // the views of at most this many candidates are rated (line of sight)
    constexpr float URGENT_DANGER_GAP = 3.0f;
    constexpr float URGENT_LEASH_EXCESS = 5.0f;
    constexpr size_t MAX_ENEMIES = 16;
    // Wave 5 (Sethekk Halls, level 67): a mage stepped out of melee to a spot beside the next pack and pulled it
    // with its patrol; the 3-mob pull became 11 mobs. A spot inside the aggro range of an idle mob is not taken.
    constexpr float IDLE_AGGRO_MARGIN = 3.0f;
    constexpr size_t MAX_IDLE_ENEMIES = 24;
    // Scholomance wave 18: the healer and the hunter took their spots on the rim of the entrance bridge (the spot ray
    // is cut at the navmesh edge), 24 yd above ~40 idle undead of the pit and the hall below. Out of their aggro reach
    // (a creature does not aggro 3 yd above or below it), but in sight: the pit came up as one chain and wiped the
    // group. A player does not stand at the railing over a room it has not cleared: a spot that looks down on idle
    // mobs of a lower floor is left for one that does not.
    constexpr float LOWER_FLOOR_HEIGHT = 8.0f;  // an idle mob this far below the spot stands on another floor
    constexpr float OVERLOOK_RANGE = 45.0f;     // ... and a spot within this distance of it that sees it overlooks it
    constexpr size_t MAX_BELOW_ENEMIES = 12;
    constexpr float HEAL_FIRST_MIN_MANA_PCT = 5.0f;  // a healer this dry gains nothing by standing in melee
    constexpr uint32 HEAL_WALK_MAX_MS = 6000;        // a walk this old is no longer the one "stop to heal" ends
    // User, Stockade boss 2026-10-24: «Хил, забегая в комнату, чуть ли не заагрил других мобов... лучше оттуда, где он
    // уже пришел». A spot costs its walk plus this share of its distance to where the bot stood when the fight began
    // (the cleared side), so of two spots the one back towards the way in wins over one across the room.
    constexpr float CAME_FROM_WEIGHT = 0.6f;
    constexpr float CAME_FROM_MAX = 45.0f;  // further than this the anchor is from an earlier fight

    // Turns away from the line pack -> tank, closest first: straight behind the tank, then towards its flanks.
    constexpr std::array<float, 9> CANDIDATE_ANGLES = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.5f, -1.5f, 2.0f, -2.0f};

    // Room between a point and the enemy's hitbox edge.
    float GapTo(Unit const* enemy, float x, float y) { return enemy->GetExactDist2d(x, y) - enemy->GetCombatReach(); }
    // [b32_pull] The room outside the enemy's real melee reach (its reach + the bot's + the 4/3 yd leeway, Unit::
    // GetMeleeRange): a Cleave or a frontal hits there. GapTo alone let a bot stand inside it: dtk-h 20261025-223307-
    // dtk-h-5G-74, the Holy Paladin 7.5 yd from the tank, "not in danger" at gap > 2, died to Wretched Belcher Cleave
    // 11.6k + Ghoul Tormentor Cleave 7.5k in 15 s.
    float MeleeGapTo(Unit const* enemy, Unit const* self, float x, float y)
    {
        return GapTo(enemy, x, y) - self->GetCombatReach() - 4.0f / 3.0f;
    }

    constexpr uint32 WALK_GATE_BISECTIONS = 6;  // the edge of the Z gate along a walk, to 1/64 of its length

    // Whether `who` walking straight from -> to enters the core's proximity aggro of the idle mob (IdleAggroBlocker, no
    // line of sight check). The walk's height changes linearly, so the Z gate passes on one stretch of it: the point of
    // that stretch nearest the mob decides.
    bool WalkWakesIdle(Creature* mob, Unit* who, G3D::Vector3 const& from, G3D::Vector3 const& to, float margin)
    {
        G3D::Vector3 const walk = to - from;
        G3D::Vector3 const mobAt(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ());
        float const lengthSq = walk.squaredLength();
        float const nearest = lengthSq > 0.01f ? std::clamp((mobAt - from).dot(walk) / lengthSq, 0.0f, 1.0f) : 0.0f;
        float const level = std::fabs(walk.z) > 0.01f ? std::clamp((mobAt.z - from.z) / walk.z, 0.0f, 1.0f) : nearest;
        auto passes = [&](float t) { return MovementAction::PassesAggroZGate(mob, who, from.z + walk.z * t); };

        float t = nearest;
        if (!passes(t))
        {
            if (!passes(level))
                return false;  // the whole walk is above or below the mob's reach

            float outside = nearest;
            float inside = level;
            for (uint32 i = 0; i < WALK_GATE_BISECTIONS; ++i)
            {
                float const mid = 0.5f * (outside + inside);
                if (passes(mid))
                    inside = mid;
                else
                    outside = mid;
            }

            t = inside;
        }

        G3D::Vector3 const point = from + walk * t;
        return !MovementAction::IdleAggroBlocker(mob, who, point.x, point.y, point.z, margin, false);
    }

    // Whether a mob of a lower floor sees the point (x, y, z): a bot standing there can be seen, targeted and healed
    // into its fight, and its pet or totems can be sent down to it.
    bool OverlooksLowerFloor(std::vector<Creature*> const& below, float x, float y, float z)
    {
        for (Creature const* mob : below)
        {
            if (z - mob->GetPositionZ() < LOWER_FLOOR_HEIGHT || mob->GetExactDist(x, y, z) > OVERLOOK_RANGE)
                continue;

            if (mob->IsWithinLOS(x, y, z))
                return true;
        }

        return false;
    }

    // A healer whose heal target is hurt (below medium health or losing health fast) and within its heal range and
    // sight heals first; the walk to the tank and the step out of melee wait until it is safe again. Stockade
    // 2026-10-24 (three tank deaths, 0 heals in 15 s): a mob on the holy paladin sent it to the tank ("chased away
    // from tank", 37), next to the tank the pack's melee sent it 20 yd out again ("ranged in melee danger", 36),
    // and the mob followed: it walked 2 -> 22 -> 6 -> 21 -> 42 -> 21 -> 4 yd from the dying tank, never casting.
    bool HealComesFirst(PlayerbotAI* botAI, Player* bot)
    {
        if (!PlayerbotAI::IsHeal(bot))
            return false;

        if (bot->getPowerType() == POWER_MANA && bot->GetPowerPct(POWER_MANA) < HEAL_FIRST_MIN_MANA_PCT)
            return false;

        AiObjectContext* context = botAI->GetAiObjectContext();
        Unit* target = context->GetValue<Unit*>("party member to heal")->Get();
        if (!target || !target->IsAlive() || !target->IsInWorld() || target->GetMapId() != bot->GetMapId())
            return false;

        float const fastLoss = sPlayerbotAIConfig.HealerFastHealthLossPct;
        HealthLossMap const& losses = context->GetValue<HealthLossMap>("group health loss")->RefGet();
        bool const losingFast = fastLoss > 0.0f && RecentHealthLossPct(target, losses) >= fastLoss;
        if (target->GetHealthPct() >= static_cast<float>(sPlayerbotAIConfig.MediumHealth) && !losingFast)
            return false;

        return target == bot ||
               (bot->GetExactDist(target) <= botAI->GetRange("heal") && bot->IsWithinLOSInMap(target));
    }
}

namespace
{
    // RFC 2026-10-24 (user: «ДД встал на зубе (скальном выступе), чуть ли не свалится»): the shadow priest and the
    // shaman took the first spot of the way that saw an Earthborer round a corner, a rock tooth 9.6 yd above the
    // tank. Height alone is no reason to refuse a spot (a fight on stairs, a safe rock: «можно лазить по скалам, это
    // просто должно быть безопасно»), so a spot is tested for what makes a perch dangerous instead.
    constexpr int FOOTING_DIRECTIONS = 8;         // the ring round a spot is sampled in this many directions
    constexpr float FOOTING_NEAR = 1.5f;          // ... at this distance
    constexpr float FOOTING_FAR = 3.0f;           // ... and at this one
    constexpr float FOOTING_NEAR_DROP = 1.5f;     // ground this much lower 1.5 yd away is an edge (~45 degrees)
    constexpr float FOOTING_MAX_DROP = 2.0f;      // ... this much 3 yd away too, unless a gentle slope leads there
    constexpr float FOOTING_EYE = 1.5f;           // a wall on a side (no sight at this height) is no drop
    constexpr float FOOTING_SEARCH = 15.0f;       // no ground within this far below: a sheer drop
    constexpr float FOOTING_FLOAT = 2.0f;         // the spot itself this far above its ground does not stand on it
    constexpr float FOOTING_POLY_EXTENT = 1.0f;   // the polygon under a point is searched this far round it
    constexpr float FOOTING_POLY_HEIGHT = 2.0f;   // ... and this far above and below it
    constexpr float SPOT_IDLE_AGGRO_MARGIN = MovementAction::SIGHT_IDLE_AGGRO_MARGIN;  // was 3.0 ([sight-arc])
    constexpr float SPOT_IDLE_SCAN = 45.0f;
    constexpr float WAY_BACK_DETOUR = 2.0f;       // the walk back to the fight is at most this many times the distance
    constexpr float WAY_BACK_SLACK = 10.0f;       // ... plus this
    constexpr float WAY_BACK_END = 5.0f;          // ... and ends this close to it
    // RFC 2026-10-24 (user: «Стоят просто на краю лавы… столько места там»): the whole group stood on the lava edge.
    // A spot keeps this far from lava and slime whose surface is near its height (the shared test, TerrainHazard.h:
    // the spot itself and rings every 2 yd out to this; lava far below a bridge is the drop-off test's business).
    constexpr float HAZARD_LIQUID_RADIUS = 4.5f;

    // The NavTerrain flags of the navmesh polygon under the point: -1 without a navmesh, 0 when there is none.
    int GroundPolyFlags(Map* map, float x, float y, float z)
    {
        MMapData& mmap = map->GetMapCollisionData().GetMMapData();
        dtNavMesh const* navMesh = mmap.GetNavMesh();
        dtNavMeshQuery const* query = mmap.GetNavMeshQuery();
        if (!navMesh || !query)
            return -1;

        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_GROUND_STEEP | NAV_WATER | NAV_MAGMA | NAV_SLIME));
        filter.setExcludeFlags(0);
        // Detour works in (y, z, x).
        float const point[3] = {y, z, x};
        float const extents[3] = {FOOTING_POLY_EXTENT, FOOTING_POLY_HEIGHT, FOOTING_POLY_EXTENT};
        float nearest[3];
        dtPolyRef ref = 0;
        if (dtStatusFailed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) || !ref)
            return 0;

        unsigned short flags = 0;
        if (dtStatusFailed(navMesh->getPolyFlags(ref, &flags)))
            return 0;

        return flags;
    }

    // Lava or slime at the spot or within HAZARD_LIQUID_RADIUS of it, near its height.
    bool HarmfulLiquidNear(Player* bot, float x, float y, float z)
    {
        return NearHarmfulLiquid(bot->GetMap(), bot->GetPhaseMask(), x, y, z, HAZARD_LIQUID_RADIUS);
    }

    // Ground the bot walks on without sliding: a NAV_GROUND polygon (unknown without a navmesh: taken as such).
    bool IsGentleGround(Map* map, float x, float y, float z)
    {
        int const flags = GroundPolyFlags(map, x, y, z);
        return flags < 0 || (flags & NAV_GROUND);
    }

    constexpr uint32 WATER_FOOTING_LOG_MS = 10 * IN_MILLISECONDS;

    void LogWaterFooting(Player* bot, float x, float y, float z, float ground)
    {
        static std::mutex logLock;
        static std::unordered_map<uint64, uint32> logged;
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> guard(logLock);
            uint32& at = logged[bot->GetGUID().GetRawValue()];
            if (at && getMSTimeDiff(at, now) < WATER_FOOTING_LOG_MS)
                return;
            at = now;
        }

        LOG_INFO("playerbots", "[rjoin-water] {} takes a swimming spot ({:.1f} {:.1f} {:.1f}), floor {:.1f} yd below",
                 bot->GetName(), x, y, z, ground > INVALID_HEIGHT ? z - ground : FOOTING_SEARCH);
    }

    // Why the bot cannot stand at the spot safely: on a steep polygon it slides off, off the navmesh or with no
    // ground under it it does not stand at all, and a side where the ground falls away (the rim of a ledge, the tip
    // of a rock tooth) is a fall. A side going down a gentle slope (stairs, a ramp) is no drop.
    std::string FootingRejection(Player* bot, float x, float y, float z)
    {
        Map* map = bot->GetMap();
        uint32 const phase = bot->GetPhaseMask();
        float const ground = map->GetHeight(phase, x, y, z + FOOTING_EYE, true, FOOTING_SEARCH);
        if (ground <= INVALID_HEIGHT || z - ground > FOOTING_FLOAT)
        {
            // [rjoin-water] Sunken Temple 2026-10-25 (pull 48): the group fought swimming in the flooded ring, the
            // floor deeper than FOOTING_FLOAT under the surface, and every spot there read "no ground under it": the
            // hunter's approach waited the whole fight. A bot swims in deep water (lava and slime are refused above);
            // it neither falls nor slides there.
            if (map->IsInWater(phase, x, y, z, bot->GetCollisionHeight()))
            {
                LogWaterFooting(bot, x, y, z, ground);
                return "";
            }

            return "no ground under it";
        }

        int const flags = GroundPolyFlags(map, x, y, ground);
        if (flags == 0)
            return "off the navmesh";

        if (flags > 0 && !(flags & NAV_GROUND) && (flags & NAV_GROUND_STEEP))
            return "on a steep slope";

        for (int i = 0; i < FOOTING_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / FOOTING_DIRECTIONS;
            float const dx = std::cos(angle);
            float const dy = std::sin(angle);
            float const farX = x + dx * FOOTING_FAR;
            float const farY = y + dy * FOOTING_FAR;
            if (!map->isInLineOfSight(x, y, ground + FOOTING_EYE, farX, farY, ground + FOOTING_EYE, phase,
                                      LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                continue;

            float const nearX = x + dx * FOOTING_NEAR;
            float const nearY = y + dy * FOOTING_NEAR;
            float const nearGround = map->GetHeight(phase, nearX, nearY, ground + FOOTING_EYE, true, FOOTING_SEARCH);
            float const nearDrop = nearGround > INVALID_HEIGHT ? ground - nearGround : FOOTING_SEARCH;
            float const farGround = map->GetHeight(phase, farX, farY, ground + FOOTING_EYE, true, FOOTING_SEARCH);
            float const farDrop = farGround > INVALID_HEIGHT ? ground - farGround : FOOTING_SEARCH;
            if (nearDrop <= FOOTING_NEAR_DROP && farDrop <= FOOTING_MAX_DROP)
                continue;

            bool const slope = nearDrop <= FOOTING_NEAR_DROP && farGround > INVALID_HEIGHT &&
                               IsGentleGround(map, nearX, nearY, nearGround) &&
                               IsGentleGround(map, farX, farY, farGround);
            if (!slope)
                return Acore::StringFormat("a drop-off of {:.1f} yd beside it", std::max(nearDrop, farDrop));
        }

        return "";
    }
}

std::string UnsafeSpotReason(PlayerbotAI* botAI, float x, float y, float z, WorldObject const* fight)
{
    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsInWorld())
        return "";

    // Idle mobs: inside their aggro reach (the core's Z gate and 3D range, IdleAggroBlocker; no line of sight check:
    // the prisoners behind bars) the spot pulls. A perch above the Z gate does not: the ranged spot rejects one a mob
    // below sees by OverlooksLowerFloor.
    GuidVector const possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
    for (ObjectGuid const guid : possible)
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->HasReactState(REACT_AGGRESSIVE) ||
            mob->GetMapId() != bot->GetMapId() || !bot->IsHostileTo(mob) || mob->GetExactDist2d(x, y) > SPOT_IDLE_SCAN)
            continue;

        if (!MovementAction::IdleAggroBlocker(mob, bot, x, y, z, SPOT_IDLE_AGGRO_MARGIN, false))
            return "within aggro reach of idle " + mob->GetName();
    }

    if (HarmfulLiquidNear(bot, x, y, z))
        return "next to lava or slime";

    std::string const footing = FootingRejection(bot, x, y, z);
    if (!footing.empty())
        return footing;

    // The way back down to the fight: a walkable path from the spot to it, not a long detour.
    if (fight && fight->GetMap() == bot->GetMap())
    {
        PathGenerator back(bot);
        back.CalculatePath(x, y, z, fight->GetPositionX(), fight->GetPositionY(), fight->GetPositionZ(), false);
        uint32 const type = back.GetPathType();
        G3D::Vector3 const& end = back.GetActualEndPosition();
        float const straight = fight->GetExactDist(x, y, z);
        if ((type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT)) || !(type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) ||
            fight->GetExactDist(end.x, end.y, end.z) > WAY_BACK_END ||
            back.getPathLength() > WAY_BACK_DETOUR * straight + WAY_BACK_SLACK)
            return "no walk back to the fight";
    }

    return "";
}

namespace
{
    constexpr float VIEW_SIDE_OFFSET = 2.0f;  // the watched unit is also looked at from this far to each side
    constexpr float VIEW_OPEN_RAY = 4.0f;     // a wall closer than this to a side walls the spot in on that side
    constexpr float VIEW_EYE = 1.5f;
    constexpr float VIEW_PACK_RANGE = 45.0f;  // idle mobs this close are the packs the group faces next
    constexpr float VIEW_WATCH_WEIGHT = 4.0f;
    constexpr float VIEW_OPEN_WEIGHT = 1.5f;
    constexpr float VIEW_PACK_WEIGHT = 1.0f;
}

float SpotView::Score() const
{
    float score = 0.0f;
    if (watchRays)
        score += VIEW_WATCH_WEIGHT * watch / watchRays;
    if (openRays)
        score += VIEW_OPEN_WEIGHT * open / openRays;
    // No idle mob near: nothing to miss.
    score += packMobs ? VIEW_PACK_WEIGHT * pack / packMobs : VIEW_PACK_WEIGHT;
    return score;
}

bool SpotView::Poor() const
{
    return (watchRays && watch < watchRays) || (openRays && !open) || (packMobs && 2 * pack < packMobs);
}

std::string SpotView::Describe() const
{
    return Acore::StringFormat("view tank {}/{}, pack {}/{}, open {}/{}", watch, watchRays, pack, packMobs, open,
                               openRays);
}

SpotView RateSpotView(WorldObject const* watch, float x, float y, float z, std::vector<Creature*> const& pack)
{
    SpotView view;
    if (!watch || !watch->IsInWorld())
        return view;

    Map* map = watch->GetMap();
    uint32 const phase = watch->GetPhaseMask();
    // The sides: square to the line spot -> watched unit.
    float const toWatch = std::atan2(watch->GetPositionY() - y, watch->GetPositionX() - x);
    float const sideX = std::cos(toWatch + static_cast<float>(M_PI) / 2.0f);
    float const sideY = std::sin(toWatch + static_cast<float>(M_PI) / 2.0f);

    view.watchRays = 3;
    for (float const side : {0.0f, VIEW_SIDE_OFFSET, -VIEW_SIDE_OFFSET})
        if (watch->IsWithinLOS(x + sideX * side, y + sideY * side, z))
            ++view.watch;

    view.openRays = 2;
    for (float const side : {VIEW_OPEN_RAY, -VIEW_OPEN_RAY})
        if (map->isInLineOfSight(x, y, z + VIEW_EYE, x + sideX * side, y + sideY * side, z + VIEW_EYE, phase,
                                 LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
            ++view.open;

    for (Creature const* mob : pack)
    {
        ++view.packMobs;
        if (mob->IsWithinLOS(x, y, z))
            ++view.pack;
    }

    return view;
}

std::vector<Creature*> IdleMobsNear(PlayerbotAI* botAI, float x, float y, float z, size_t maxMobs)
{
    std::vector<Creature*> mobs;
    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsInWorld() || !maxMobs)
        return mobs;

    GuidVector const possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
    for (ObjectGuid const guid : possible)
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || mob->GetMapId() != bot->GetMapId() ||
            !bot->IsHostileTo(mob) || mob->GetExactDist(x, y, z) > VIEW_PACK_RANGE)
            continue;

        mobs.push_back(mob);
    }

    std::sort(mobs.begin(), mobs.end(),
              [x, y, z](Creature* a, Creature* b) { return a->GetExactDist(x, y, z) < b->GetExactDist(x, y, z); });
    if (mobs.size() > maxMobs)
        mobs.resize(maxMobs);

    return mobs;
}

Player* RangedPositionValue::FindTank() const
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    ObjectGuid const mainTankGuid = PlayerbotAI::GetMainTankGuid(group);
    Player* fallback = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
            member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;

        if (member->GetGUID() == mainTankGuid)
            return member;

        if (!fallback && PlayerbotAI::IsTank(member))
            fallback = member;
    }

    return fallback;
}

RangedPositionInfo RangedPositionValue::Calculate()
{
    RangedPositionInfo info;

    if (!bot->IsInWorld() || !bot->IsAlive() || bot->InBattleground() || bot->InArena() || bot->GetVehicle() ||
        bot->GetTransport() || bot->IsFlying())
        return info;

    Group* group = bot->GetGroup();
    Map* map = bot->GetMap();
    // Raids keep their encounter positioning; this is for parties.
    if (!group || !map || map->IsRaid() || group->isRaidGroup())
        return info;

    if (PlayerbotAI::IsTank(bot))
        return info;

    bool const healer = PlayerbotAI::IsHeal(bot);
    // A melee dps only takes the walk to the tank below (the "behind" strategy holds "chased away from tank"):
    // Stockade 2026-10-24, a rogue 23 yd from the tank stood 25 s under three mobs out of the tank's sight and died.
    bool const melee = !healer && !PlayerbotAI::IsRanged(bot);

    if (botAI->HasStrategy("stay", botAI->GetState()))
        return info;

    Player* tank = FindTank();
    if (!tank)
        return info;

    if (!tank->IsInCombat())
        _hasCameFrom = false;
    else if (!_hasCameFrom || bot->GetExactDist(_cameFrom) > CAME_FROM_MAX)
    {
        _cameFrom = bot->GetPosition();
        _hasCameFrom = true;
    }

    // A mob meleeing the bot follows it: next to the tank its area threat and taunts take the mob. Out of combat too:
    // the tank may not have been hit yet (wave 9 Steamvault).
    if (sPlayerbotAIConfig.RangedFleeToTank)
    {
        bool chased = false;
        // [melee_back_to_tank] scholo-h 20261025-153805 pull 10 (15:50:47-15:51:18): the Enhancement shaman stood 35 yd
        // from the tank, four Adepts, two Necromancers and the Spectral Tutor switched to him (Frostbolts, Frost Shock,
        // the Tutor's melee) and he died in 2 s: casters on him never counted as "chased". Any member (melee, ranged,
        // healer) with two or more mobs on it runs into the tank's AoE, at any range of theirs (user: "when mobs hang
        // on the healer, the healer runs to the tank, the tank AoEs"); "move to tank" outranks "flee".
        uint32 onBot = 0;
        for (Unit* attacker : bot->getAttackers())
        {
            Creature* creature = attacker ? attacker->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive() || creature->IsDungeonBoss() || creature->isWorldBoss() ||
                creature->GetVictim() != bot)
                continue;

            ++onBot;
            if (creature->IsWithinMeleeRange(bot) || onBot >= MELEE_CHASED_BY)
            {
                chased = true;
                break;
            }
        }

        float const tankDistance = bot->GetExactDist(tank);
        if (chased && tankDistance <= CHASE_REFUGE_MAX)
        {
            info.tank = tank->GetGUID();
            info.fighting = true;  // no follow either: the bot stays by the tank until the mob is taken
            info.chased = tankDistance > CHASE_REFUGE_REACHED;
            return info;
        }
    }

    if (melee || !tank->IsInCombat())
        return info;

    info.tank = tank->GetGUID();

    // The pack the tank holds, and everything else fighting the group close to the bot.
    std::vector<Unit*> enemies;
    Unit* focus = nullptr;
    float focusDistance = PACK_RADIUS;
    float dangerGap = SAFE_GAP * 10.0f;
    GuidVector const attackers = AI_VALUE(GuidVector, "attackers");
    for (ObjectGuid const guid : attackers)
    {
        Unit* enemy = botAI->GetUnit(guid);
        if (!enemy || !enemy->IsAlive() || enemy->GetMapId() != bot->GetMapId())
            continue;

        float const toTank = enemy->GetExactDist2d(tank);
        if (toTank > PACK_RADIUS && bot->GetExactDist2d(enemy) > ENEMY_SCAN_RADIUS)
            continue;

        if (enemies.size() < MAX_ENEMIES)
            enemies.push_back(enemy);

        if (toTank <= focusDistance)
        {
            focusDistance = toTank;
            focus = enemy;
        }

        // An enemy hitting the bot follows it anywhere; only splash from fights of others is stepped out of.
        if (enemy->GetVictim() != bot)
            dangerGap = std::min(dangerGap, MeleeGapTo(enemy, bot, bot->GetPositionX(), bot->GetPositionY()));  // [b32_pull]
    }

    float const tankDistance = bot->GetExactDist(tank);
    info.dangerGap = dangerGap;
    info.tankDistance = tankDistance;
    // Ranged members keep within the leash of the tank too: with follow off during the fight nothing brought them
    // back, and wave 10 heroic trash killed them 25-50 yd from the tank and the healer. Healers keep the same leash
    // when it is on (wave 11: they stood 30-33 yd away, inside the old 33-yard rule).
    // A healer moves in when the tank is out of its heal range or sight, not earlier (user, 2026-10-24: «"healer too
    // far from tank" must apply to healers only and only when out of heal range/LoS»). A dps keeps the leash only
    // while the tank's target is out of its spell range or sight: one that casts at it from where it stands stays.
    Unit* tankVictim = tank->GetVictim();
    float const rangedLeash = sPlayerbotAIConfig.RangedTankLeash;
    float const leash = healer ? std::max(botAI->GetRange("heal") - HEALER_RANGE_MARGIN, 10.0f) : rangedLeash;
    bool tooFar = false;
    if (healer)
        tooFar = tankDistance > leash || (tankDistance <= HEALER_SIGHT_CHECK && !bot->IsWithinLOSInMap(tank));
    else if (leash > 0.0f && tankDistance > leash)
        tooFar = !tankVictim || !tankVictim->IsAlive() || tankVictim->GetMap() != bot->GetMap() ||
                 bot->GetExactDist(tankVictim) > botAI->GetRange("spell") || !bot->IsWithinLOSInMap(tankVictim);
    info.leash = leash;

    if (enemies.empty())
    {
        // The fight is out of the bot's sight or range ("attackers" needs both): it walks to the fighting tank
        // instead of standing where it lost it (wave 11: ranged idle 40-54 yd away with no target). A dps inside
        // the leash too, when the tank's target is out of its sight or spell range (user, 2026-10-24: «второй стоял
        // за углом»): it comes round the corner only as far as it sees the mob (MoveToRangedPositionAction).
        bool const lost = tooFar || (!healer && tankVictim && tankVictim->IsAlive() &&
                                     tankVictim->GetMap() == bot->GetMap() &&
                                     (bot->GetExactDist(tankVictim) > botAI->GetRange("spell") ||
                                      !bot->IsWithinLOSInMap(tankVictim)));
        if (lost && tankVictim && tankVictim->IsAlive())
        {
            info.reason = healer ? "ranged pos: the tank out of heal range or sight"
                                 : "ranged pos: the tank's target out of sight or range";
            info.fighting = true;
            info.needed = true;
            info.approach = true;
        }
        // Between two kills (or before the next target is picked) the bot stands where it is instead of following
        // the tank into the pack: no-target walks were 40 % of the in-band moves of ranged dps (wave 2026-10-24).
        else if (leash > 0.0f && !lost && tankVictim && tankVictim->IsAlive())
            info.fighting = true;

        return info;
    }

    info.fighting = true;

    if (tankVictim && tankVictim->IsAlive() && tankVictim->GetExactDist2d(tank) <= PACK_RADIUS)
        focus = tankVictim;

    info.danger = dangerGap < MELEE_DANGER_GAP;

    // Idle mobs of a lower floor near the bot, nearest first (see LOWER_FLOOR_HEIGHT).
    std::vector<Creature*> below;
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->HasReactState(REACT_AGGRESSIVE) ||
            mob->GetMapId() != bot->GetMapId() || !bot->IsHostileTo(mob) ||
            bot->GetPositionZ() - mob->GetPositionZ() < LOWER_FLOOR_HEIGHT || bot->GetExactDist(mob) > OVERLOOK_RANGE)
            continue;

        below.push_back(mob);
    }

    std::sort(below.begin(), below.end(),
              [this](Creature* a, Creature* b) { return bot->GetExactDist(a) < bot->GetExactDist(b); });
    if (below.size() > MAX_BELOW_ENEMIES)
        below.resize(MAX_BELOW_ENEMIES);

    bool const overlooks =
        !below.empty() && OverlooksLowerFloor(below, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

    // RFC 2026-10-24 (user: «можно же немножко отбежать левее, чтобы видеть танка с большего угла, и видеть весь этот
    // пак... логика должна это понимать»): the ranged and the healer stood bunched in the corridor behind the tank at
    // its mouth, seeing it round the edge and nothing of the pack in the lava room, while a step aside into the open
    // saw both. A bot that stands well enough otherwise looks for a better view now and then (VIEW_CHECK_MS) and goes
    // only to a spot that sees clearly better (VIEW_MIN_GAIN) a short walk away; every hard rule still applies.
    bool improve = false;
    if (!info.danger && !tooFar && !overlooks)
    {
        // [rjoin-firsthit] The first hit before a better view: a dps that has not opened yet (not in combat itself)
        // opens from where it stands; the view is looked at once it fights. The engage audit of 2026-10-25 (161 runs)
        // had casters 2.7 s (median) from the tank's first hit to their own first action.
        if (!bot->IsInCombat())
            return info;

        uint32 const now = getMSTime();
        if (bot->isMoving() || (_viewCheckMs && getMSTimeDiff(_viewCheckMs, now) < VIEW_CHECK_MS))
            return info;

        _viewCheckMs = now;
        improve = true;
    }

    // Mobs not fighting yet near the bot: the spot must not be inside their aggro range.
    // From the list that ignores the bot's sight, nearest first: Stockade 2026-10-24 (chain pull 15), the prisoners
    // behind their cell bars are out of the bot's line of sight, "possible targets" left them out, and the healer and
    // the shaman took a spot right next to an Inmate. The aggro test below is geometric (Z gate, range + margin).
    std::vector<Creature*> idle;
    GuidVector const possible = AI_VALUE(GuidVector, "possible targets no los");
    for (ObjectGuid const guid : possible)
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->HasReactState(REACT_AGGRESSIVE) ||
            mob->GetMapId() != bot->GetMapId() || !bot->IsHostileTo(mob) ||
            bot->GetExactDist2d(mob) > ENEMY_SCAN_RADIUS)
            continue;

        idle.push_back(mob);
    }

    std::sort(idle.begin(), idle.end(),
              [this](Creature* a, Creature* b) { return bot->GetExactDist2d(a) < bot->GetExactDist2d(b); });
    if (idle.size() > MAX_IDLE_ENEMIES)
        idle.resize(MAX_IDLE_ENEMIES);

    std::vector<Creature*> const pack(idle.begin(), idle.begin() + std::min(idle.size(), VIEW_PACK_MOBS));
    SpotView here;
    if (improve)
    {
        here = RateSpotView(tank, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), pack);
        if (!here.Poor())
            return info;
    }

    // Members of the same kind spread over neighbouring spots instead of all taking the same one.
    uint32 rangedIndex = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member == bot)
            break;

        if (member && !PlayerbotAI::IsTank(member) && PlayerbotAI::IsRanged(member))
            ++rangedIndex;
    }

    float const side = (rangedIndex % 2) ? 1.0f : -1.0f;
    float const spread = side * static_cast<float>((rangedIndex + 1) / 2) * SPREAD_STEP;

    WorldObject const* center = focus ? static_cast<WorldObject const*>(focus) : tank;
    float const baseAngle = focus ? focus->GetAbsoluteAngle(tank) : tank->GetAbsoluteAngle(bot);
    // Only out of melee danger (in range and sight otherwise): the smallest step out, not the full ranged distance.
    bool const stepOut = info.danger && !tooFar && !overlooks;
    float const safeGap = stepOut ? DANGER_SAFE_GAP : SAFE_GAP;
    float radius = focus ? SPOT_DISTANCE_FROM_PACK + focus->GetCombatReach() : SPOT_DISTANCE_FROM_TANK;
    if (stepOut && focus)
        radius = std::min(radius, std::max(bot->GetExactDist2d(focus), focus->GetCombatReach() + DANGER_STEP_OUT));
    WorldObject const* sightAnchor = healer ? static_cast<WorldObject const*>(tank) : center;

    bool found = false;
    float bestCost = 0.0f;
    bool foundFallback = false;
    float fallbackGap = 0.0f;
    float fallbackX = 0.0f;
    float fallbackY = 0.0f;
    float fallbackZ = 0.0f;
    bool foundOverlook = false;
    float overlookCost = 0.0f;
    float overlookX = 0.0f;
    float overlookY = 0.0f;
    float overlookZ = 0.0f;
    SpotView bestView;
    SpotView fallbackView;
    SpotView overlookView;
    size_t rated = 0;
    // A candidate about to be taken must be safe to stand on (UnsafeSpotReason; RFC 2026-10-24, the rock tooth).
    // Checked only then: the footing samples and the walk back cost more than the geometric tests above them.
    std::string rejected;
    auto safe = [&](float x, float y, float z)
    {
        std::string const why = UnsafeSpotReason(botAI, x, y, z, tank);
        if (why.empty())
            return true;

        if (rejected.empty())
            rejected = Acore::StringFormat("rejected a spot {:+.1f} yd above the tank: {}", z - tank->GetPositionZ(),
                                           why);
        return false;
    };

    for (float const offset : CANDIDATE_ANGLES)
    {
        float const angle = Position::NormalizeOrientation(baseAngle + spread + offset);
        float x = center->GetPositionX() + std::cos(angle) * radius;
        float y = center->GetPositionY() + std::sin(angle) * radius;
        float z = center->GetPositionZ();

        // Walls and ledges cut the spot short along the ray from the tank, which is also the way to see it.
        if (!map->CheckCollisionAndGetValidCoords(bot, tank->GetPositionX(), tank->GetPositionY(),
                                                  tank->GetPositionZ(), x, y, z, false))
            continue;

        if (std::fabs(z - tank->GetPositionZ()) > SPOT_MAX_HEIGHT_DIFFERENCE)
            continue;

        if (healer ? tank->GetExactDist(x, y, z) > SPOT_MAX_TANK_DISTANCE
                   : center->GetExactDist(x, y, z) > SPOT_MAX_TARGET_DISTANCE)
            continue;

        if (!healer && leash > 0.0f && tank->GetExactDist(x, y, z) > leash - LEASH_SPOT_MARGIN)
            continue;

        float gap = SAFE_GAP * 10.0f;
        float meleeGap = SAFE_GAP * 10.0f;  // [b32_pull]
        for (Unit const* enemy : enemies)
        {
            gap = std::min(gap, GapTo(enemy, x, y));
            meleeGap = std::min(meleeGap, MeleeGapTo(enemy, bot, x, y));
        }

        if (meleeGap <= MELEE_DANGER_GAP)  // [b32_pull] was gap
            continue;

        // Neither the spot nor the straight walk to it passes inside the aggro reach of an idle mob (the core's Z gate
        // and 3D range, IdleAggroBlocker; no line of sight check: the prisoners behind bars).
        G3D::Vector3 const botAt(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        G3D::Vector3 const spotAt(x, y, z);
        bool pullsIdle = false;
        for (Creature* mob : idle)
        {
            if (!MovementAction::IdleAggroBlocker(mob, bot, x, y, z, IDLE_AGGRO_MARGIN, false) ||
                WalkWakesIdle(mob, bot, botAt, spotAt, IDLE_AGGRO_MARGIN))
            {
                pullsIdle = true;
                break;
            }
        }

        if (pullsIdle)
            continue;

        if (!sightAnchor->IsWithinLOS(x, y, z))
            continue;

        // A better view only: a short walk, out of the cleaves with room to spare, and clearly better than here.
        if (improve && (bot->GetExactDist2d(x, y) > VIEW_MAX_WALK || gap < safeGap))
            continue;

        // How well the spot sees the tank, the next pack and the room round it: a preference worth some walk.
        SpotView view;
        if (rated < VIEW_MAX_RATED)
        {
            ++rated;
            view = RateSpotView(tank, x, y, z, pack);
        }
        else if (improve)
            continue;

        if (improve && view.Score() < here.Score() + VIEW_MIN_GAIN)
            continue;

        // A spot over a lower floor of idle mobs is the last resort of a bot in melee danger or out of the leash,
        // and no spot at all for a bot that only leaves such a rim.
        float const cost = bot->GetExactDist2d(x, y) +
                           (_hasCameFrom ? CAME_FROM_WEIGHT * _cameFrom.GetExactDist2d(x, y) : 0.0f) -
                           VIEW_SCORE_COST * view.Score();
        if (!below.empty() && OverlooksLowerFloor(below, x, y, z))
        {
            if ((info.danger || tooFar) && (!foundOverlook || cost < overlookCost) && safe(x, y, z))
            {
                foundOverlook = true;
                overlookCost = cost;
                overlookX = x;
                overlookY = y;
                overlookZ = z;
                overlookView = view;
            }
            continue;
        }

        if (gap < safeGap)
        {
            if ((!foundFallback || gap > fallbackGap) && safe(x, y, z))
            {
                foundFallback = true;
                fallbackGap = gap;
                fallbackX = x;
                fallbackY = y;
                fallbackZ = z;
                fallbackView = view;
            }
            continue;
        }

        if ((!found || cost < bestCost) && safe(x, y, z))
        {
            found = true;
            bestCost = cost;
            info.x = x;
            info.y = y;
            info.z = z;
            bestView = view;
        }
    }

    if (!found && foundFallback)
    {
        found = true;
        info.x = fallbackX;
        info.y = fallbackY;
        info.z = fallbackZ;
        bestView = fallbackView;
    }

    if (!found && foundOverlook)
    {
        found = true;
        info.x = overlookX;
        info.y = overlookY;
        info.z = overlookZ;
        bestView = overlookView;
    }

    if (found)
    {
        info.needed = true;
        info.improve = improve;
        bool const backwards =
            _hasCameFrom && _cameFrom.GetExactDist2d(info.x, info.y) < _cameFrom.GetExactDist2d(bot);
        info.reason = std::string("ranged pos: ") +
                      (tooFar ? (healer ? "tank out of heal range or sight" : "target out of spell range or sight")
                              : (info.danger ? "step out of melee"
                                             : (improve ? "better view (here " + here.Describe() + ")"
                                                        : std::string("off the overlook")))) +
                      (backwards ? ", back towards the way in" : ", across") + ", " + std::to_string(idle.size()) +
                      " idle mobs near, none within reach" +
                      (bestView.watchRays ? ", " + bestView.Describe() : std::string());
        return info;
    }

    // No usable spot (tight corridor, other floor): a healer at least walks towards the tank it has to heal.
    if (tooFar)
    {
        info.needed = true;
        info.approach = true;
        info.reason = "ranged pos: no spot, approach the tank";
        if (!rejected.empty())
            info.reason += " (" + rejected + ")";
    }

    return info;
}

bool RangedInMeleeDangerTrigger::IsActive()
{
    RangedPositionInfo const info = AI_VALUE(RangedPositionInfo, "group ranged position");
    return info.needed && info.danger && !HealComesFirst(botAI, bot);
}

bool HealerTooFarFromTankTrigger::IsActive()
{
    RangedPositionInfo const info = AI_VALUE(RangedPositionInfo, "group ranged position");
    // Healers; a dps only walks to a fight it has lost entirely (no enemy in its sight or range, `approach`), its
    // range and sight to the target are the reach actions' job.
    if (!PlayerbotAI::IsHeal(bot) && !info.approach && !info.improve)
        return false;

    return info.needed && !info.danger && !HealComesFirst(botAI, bot);
}

bool ChasedAwayFromTankTrigger::IsActive()
{
    RangedPositionInfo const info = AI_VALUE(RangedPositionInfo, "group ranged position");
    return info.chased && !HealComesFirst(botAI, bot);
}

bool PrePullSightWatch(PlayerbotAI* botAI, Player* bot, Unit*& watch, float& range, std::string& why)
{
    constexpr float KILL_SPOT_NEAR = 5.0f;        // the tank still stands at its pull back's kill spot
    constexpr float ENGAGE_WATCH_RANGE = 30.0f;  // an engage is watched from no farther than this

    watch = nullptr;
    if (!botAI || !bot || bot->IsInCombat() || !bot->GetGroup() || !bot->GetMap()->IsDungeon() ||
        PlayerbotAI::IsTank(bot) || !(PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot)))
        return false;

    Unit* tankUnit = botAI->GetAiObjectContext()->GetValue<Unit*>("main tank")->Get();
    Player* tank = tankUnit ? tankUnit->ToPlayer() : nullptr;
    if (!tank || tank == bot || !tank->IsInWorld() || !tank->IsAlive() || tank->IsInCombat() ||
        tank->GetMap() != bot->GetMap())
        return false;

    // a line-of-sight pull has its own group spot (LosPull), held by the whole group
    if (LosPull::IsHolding(bot))
        return false;

    range = PlayerbotAI::IsHeal(bot) ? botAI->GetRange("heal") : botAI->GetRange("spell");

    // A pull back: the mob comes to the kill spot, where the tank stands before it walks out to cast.
    ObjectGuid castFor;
    Position cast;
    Position kill;
    bool hasKill = false;
    if (PullCastSpot::GetLatest(tank, castFor, cast, kill, hasKill))
    {
        Unit* mob = botAI->GetUnit(castFor);
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || !hasKill || tank->GetExactDist(kill) > KILL_SPOT_NEAR)
            return false;

        watch = tank;
        why = "prepull sight: the kill spot of the pull back on " + mob->GetName();
        return true;
    }

    // An accepted engage (a body pull): the fight is at the mob.
    ObjectGuid const target = tank->GetTarget();
    Unit* mob = target.IsEmpty() ? nullptr : botAI->GetUnit(target);
    bool accepted = false;
    uint32 mobs = 0;
    if (!mob || !mob->IsAlive() || mob->IsInCombat() || mob->IsFriendlyTo(bot) ||
        !CarefulPlanVerdict::Get(tank->GetGUID(), target, accepted, mobs) || !accepted)
        return false;

    watch = mob;
    range = std::min(range, ENGAGE_WATCH_RANGE);
    why = "prepull sight: the engage on " + mob->GetName();
    return true;
}

bool PrePullSightSpotTrigger::IsActive()
{
    Unit* watch = nullptr;
    float range = 0.0f;
    std::string why;
    if (!PrePullSightWatch(botAI, bot, watch, range, why))
        return false;

    return bot->GetExactDist(watch) > range || !bot->IsWithinLOSInMap(watch);
}

bool MoveToPrePullSightSpotAction::isUseful()
{
    return !bot->IsInCombat() && !bot->IsSitState() && !PlayerbotAI::IsTank(bot);
}

bool MoveToPrePullSightSpotAction::Execute(Event /*event*/)
{
    Unit* watch = nullptr;
    float range = 0.0f;
    std::string why;
    if (!PrePullSightWatch(botAI, bot, watch, range, why))
        return false;

    if (bot->GetExactDist(watch) <= range && bot->IsWithinLOSInMap(watch))
        return false;

    if (!MoveToSight(watch, range, why))
        return false;

    // One line per bot and pull target: the walk is replanned while the plan holds.
    constexpr uint32 PREPULL_SIGHT_LOG_MS = 10 * IN_MILLISECONDS;
    thread_local std::unordered_map<ObjectGuid, std::pair<ObjectGuid, uint32>> logged;
    std::pair<ObjectGuid, uint32>& last = logged[bot->GetGUID()];
    uint32 const now = getMSTime();
    if (last.first != watch->GetGUID() || !last.second || getMSTimeDiff(last.second, now) >= PREPULL_SIGHT_LOG_MS)
    {
        last = {watch->GetGUID(), now ? now : 1};
        LOG_INFO("playerbots", "[prepull_sight_spot] {}: {} ({:.1f} yd, range {:.0f})", bot->GetName(), why,
                 bot->GetExactDist(watch), range);
    }

    return true;
}

Player* FindGroupHealer(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && member->IsInWorld() && member->IsAlive() && member->GetMap() == bot->GetMap() &&
            PlayerbotAI::IsHeal(member))
            return member;
    }

    return nullptr;
}

namespace
{
    constexpr float TANK_SIGHT_RANGE_MARGIN = 5.0f;  // the tank stays this far inside its healer's heal range

    // The tank holds a non-boss mob in melee (a boss stays where its encounter wants it).
    bool HoldsTrash(Player* bot)
    {
        bool holds = false;
        for (Unit* attacker : bot->getAttackers())
        {
            Creature* creature = attacker ? attacker->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive())
                continue;

            if (creature->IsDungeonBoss() || creature->isWorldBoss())
                return false;

            holds = holds || creature->GetVictim() == bot;
        }

        return holds;
    }
}

bool TankLostHealerSightTrigger::IsActive()
{
    if (!PlayerbotAI::IsTank(bot) || !bot->IsInCombat() || !botAI->IsInNonRaidDungeon())
    {
        _lostSinceMs = 0;
        return false;
    }

    uint32 const now = getMSTime();
    if (_checkMs && getMSTimeDiff(_checkMs, now) < SIGHT_CHECK_MS)
        return _lostSinceMs && getMSTimeDiff(_lostSinceMs, now) >= TANK_SIGHT_GRACE_MS;

    _checkMs = now;
    Player* healer = FindGroupHealer(bot);
    bool const lost = healer && HoldsTrash(bot) &&
                      (bot->GetExactDist(healer) > botAI->GetRange("heal") || !bot->IsWithinLOSInMap(healer));
    if (!lost)
        _lostSinceMs = 0;
    else if (!_lostSinceMs)
        _lostSinceMs = now;

    return _lostSinceMs && getMSTimeDiff(_lostSinceMs, now) >= TANK_SIGHT_GRACE_MS;
}

bool TankStepToHealerSightAction::isUseful()
{
    return botAI->CanMove() && !bot->IsRooted() && !bot->HasUnitState(UNIT_STATE_LOST_CONTROL) &&
           !bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
}

bool TankStepToHealerSightAction::Execute(Event /*event*/)
{
    Player* healer = FindGroupHealer(bot);
    if (!healer)
        return false;

    float const range = std::max(botAI->GetRange("heal") - TANK_SIGHT_RANGE_MARGIN, 10.0f);
    float const distance = bot->GetExactDist(healer);
    if (!MoveToSight(healer, range, "tank los: back into the healer's sight"))
        return false;

    LastMovement const& last = AI_VALUE(LastMovement&, "last movement");
    static std::mutex logLock;
    static std::unordered_map<uint64, uint32> logged;
    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> guard(logLock);
        uint32& at = logged[bot->GetGUID().GetRawValue()];
        if (at && getMSTimeDiff(at, now) < 5 * IN_MILLISECONDS)
            return true;
        at = now;
    }

    LOG_INFO("playerbots", "[tank los] {} lost healer {} LoS ({:.1f} yd) -> steps back to ({:.1f} {:.1f} {:.1f})",
             bot->GetName(), healer->GetName(), distance, last.lastMoveToX, last.lastMoveToY, last.lastMoveToZ);
    return true;
}

void NoteHealWalk(PlayerbotAI* botAI)
{
    if (!PlayerbotAI::IsHeal(botAI->GetBot()))
        return;

    AiObjectContext* context = botAI->GetAiObjectContext();
    LastMovement const& last = context->GetValue<LastMovement&>("last movement")->Get();
    HealWalk walk;
    walk.startMs = getMSTime();
    walk.x = last.lastMoveToX;
    walk.y = last.lastMoveToY;
    context->GetValue<HealWalk>("heal walk")->Set(walk);
}

bool HealerStopsToHealTrigger::IsActive()
{
    if (!bot->isMoving())
        return false;

    HealWalk const walk = AI_VALUE(HealWalk, "heal walk");
    if (!walk.startMs || getMSTimeDiff(walk.startMs, getMSTime()) > HEAL_WALK_MAX_MS)
        return false;

    // Only that walk: any later move (an AoE dodge, a reach to the heal target) has replaced LastMovement's target.
    LastMovement const& last = AI_VALUE(LastMovement&, "last movement");
    if (std::fabs(last.lastMoveToX - walk.x) + std::fabs(last.lastMoveToY - walk.y) > SAME_SPOT)
        return false;

    if (bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE)
        return false;

    return HealComesFirst(botAI, bot);
}

bool StopToHealAction::Execute(Event /*event*/)
{
    AI_VALUE(LastMovement&, "last movement").clear();
    context->GetValue<HealWalk>("heal walk")->Reset();
    bot->GetMotionMaster()->Clear(false);
    bot->StopMoving();
    return true;
}

bool MoveToRangedPositionAction::isUseful()
{
    RangedPositionInfo const info = AI_VALUE(RangedPositionInfo, "group ranged position");
    if (!info.needed)
        return false;

    if (!botAI->CanMove() || bot->IsRooted() || bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
        return false;

    // Tranquility, Hymn of Hope and other channels are worth more than the step.
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return false;

    // Out of melee danger a heal in progress is finished before walking.
    if (!info.danger && bot->IsNonMeleeSpellCast(false, true, true))
        return false;

    // Eating or drinking a healer gets up only when the tank it has to heal is already hurt.
    if (!bot->IsStandState())
    {
        Unit* tank = botAI->GetUnit(info.tank);
        if (!PlayerbotAI::IsHeal(bot) || !tank ||
            tank->GetHealthPct() >= static_cast<float>(sPlayerbotAIConfig.AlmostFullHealth))
            return false;
    }

    return true;
}

bool MoveToRangedPositionAction::Execute(Event /*event*/)
{
    RangedPositionInfo const info = AI_VALUE(RangedPositionInfo, "group ranged position");
    if (!info.needed)
        return false;

    if (!bot->IsStandState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    if (info.approach)
    {
        // A walk that fails in a row hands the bot to "follow" after a while (RangedPositionFollowMultiplier).
        bool const moved = Approach(info);
        uint32 const stall = AI_VALUE(uint32, "ranged approach stall");
        if (moved)
            context->GetValue<uint32>("ranged approach stall")->Set(0);
        else if (!stall)
            context->GetValue<uint32>("ranged approach stall")->Set(std::max<uint32>(getMSTime(), 1));
        return moved;
    }

    context->GetValue<uint32>("ranged approach stall")->Set(0);
    return TakeSpot(info);
}

bool MoveToRangedPositionAction::Approach(RangedPositionInfo const& info)
{
    Unit* tank = botAI->GetUnit(info.tank);
    if (!tank)
        return false;

    if (_waitUntil && static_cast<int32>(_waitUntil - getMSTime()) > 0)
        return false;

    // Out of sight: round the corner only as far as the tank (a dps: the tank's target) comes in view, not into
    // the pack.
    bool const heal = PlayerbotAI::IsHeal(bot);
    Unit* tankVictim = tank->GetVictim();
    Unit* sightOf = !heal && tankVictim && tankVictim->IsAlive() && tankVictim->GetMap() == bot->GetMap()
                        ? tankVictim
                        : tank;
    bool const sight = bot->IsWithinLOSInMap(sightOf);
    // [b32_engage] a hunter sees to its weapon's range (Auto Shot 35 yd), not the casters' 28
    float const sightRange =
        heal ? SPOT_MAX_TANK_DISTANCE : std::max(SPOT_MAX_TARGET_DISTANCE, botAI->GetHunterWeaponRange());
    // [b32_engage] A dps that sees the tank's target in range, or whose target runs into its range and sight within
    // ~2 s (the pack on its way to the tank), stays and shoots: no new walk every SIGHT_REPLAN_MS after a moving
    // mob, no walk to the tank (engage_delay.md "G ranged repositioning", the hunters' stop-start crawl).
    if (!heal && sightOf != tank &&
        ((sight && bot->GetExactDist(sightOf) <= sightRange) || TargetComesIntoRange(bot, sightOf, sightRange)))
        return false;

    std::string rejected;
    if (!sight || bot->GetExactDist(sightOf) > sightRange)
    {
        if (MoveToSight(sightOf, sightRange,
                        heal ? "sight: tank round the corner" : "sight: the tank's target round the corner"))
        {
            NoteHealWalk(botAI);
            return true;
        }

        rejected = LastSightRejection();
    }

    // The walk towards the tank ends at the first safe spot of the way that sees it within `distance`, not
    // wherever the plain reach stops (RFC 2026-10-24: the shaman's reach ended on the rock tooth 22 yd from it).
    float const distance = sight ? APPROACH_DISTANCE : APPROACH_DISTANCE_NO_SIGHT;
    if (rejected.empty() && bot->GetExactDist(tank) > distance)
    {
        if (MoveToSight(tank, distance, info.reason))
        {
            NoteHealWalk(botAI);
            return true;
        }

        rejected = LastSightRejection();
    }

    // Only unsafe spots on the way: stay and wait, the tank brings the mob (or steps back into the healer's
    // sight, TankStepToHealerSightAction) instead of the bot climbing onto the perch.
    if (!rejected.empty())
    {
        _waitUntil = getMSTime() + UNSAFE_SPOT_WAIT_MS;
        if (!bot->isMoving())
            NoteMoveReason("sight: waiting, " + rejected);
        return false;
    }

    // The plain reach ends `distance` short of the tank on the bot's side: not inside an idle pack's reach
    // either (scholo-h 2026-10-25 pull 8: a hunter and a mage walked 40 yd "the tank's target out of sight or
    // range" into the next packs, 18 mobs). Wait instead; the tank brings the fight.
    if (bot->GetExactDist(tank) > distance)
    {
        float const angle = tank->GetAngle(bot);
        float endX = tank->GetPositionX() + std::cos(angle) * distance;
        float endY = tank->GetPositionY() + std::sin(angle) * distance;
        float endZ = tank->GetPositionZ();
        bot->UpdateAllowedPositionZ(endX, endY, endZ);
        std::string const unsafe = UnsafeSpotReason(botAI, endX, endY, endZ, tank);
        if (!unsafe.empty())
        {
            _waitUntil = getMSTime() + UNSAFE_SPOT_WAIT_MS;
            if (!bot->isMoving())
                NoteMoveReason("approach: waiting, " + unsafe);
            return false;
        }
    }

    if (!ReachCombatTo(tank, distance))
        return false;

    NoteMoveReason(info.reason);
    NoteHealWalk(botAI);
    return true;
}

bool MoveToRangedPositionAction::TakeSpot(RangedPositionInfo const& info)
{
    uint32 const now = getMSTime();
    bool const sameSpot =
        std::fabs(_lastX - info.x) + std::fabs(_lastY - info.y) + std::fabs(_lastZ - info.z) < SAME_SPOT;
    // Still walking to the spot picked a moment ago: keep the tick, a cast-time spell would stop the walk.
    if (_lastMoveTime && getMSTimeDiff(_lastMoveTime, now) < KEEP_MOVING_MS && bot->isMoving() && sameSpot)
        return true;

    // A dps keeps the spot it took a moment ago unless the reason to leave it is urgent (see REPOSITION_HOLD_MS);
    // a healer follows its tank's sight and range at once.
    bool const urgent = PlayerbotAI::IsHeal(bot) || info.dangerGap < URGENT_DANGER_GAP ||
                        (info.leash > 0.0f && info.tankDistance > info.leash + URGENT_LEASH_EXCESS);
    if (_holdUntil && static_cast<int32>(_holdUntil - now) > 0 && !sameSpot && !urgent)
        return false;

    float const travel = bot->GetExactDist(info.x, info.y, info.z);

    if (!MoveTo(bot->GetMapId(), info.x, info.y, info.z, false, false, false, false,
                MovementPriority::MOVEMENT_COMBAT, true))
        return false;

    NoteMoveReason(info.reason);
    NoteHealWalk(botAI);
    _lastMoveTime = now;
    _holdUntil = now + static_cast<uint32>(IN_MILLISECONDS * MoveDelay(travel)) + REPOSITION_HOLD_MS;
    _lastX = info.x;
    _lastY = info.y;
    _lastZ = info.z;
    return true;
}
