/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MovementActions.h"
#include "DetourNavMeshQuery.h"  // [move28]
#include "MapDefines.h"  // [move28] NAV_*
#include "BotPath.h"  // [lead109]
#include "AttackerCountValues.h"
#include "BotMoveEvents.h"  // [b33_sp] water_drop
#include "MoveConflict.h"
#include "PullActions.h"  // FightHold ([drift_fight_hold])
#include "CellImpl.h"           // [b43_move] the ground effects round a follow spot
#include "Corpse.h"
#include "Creature.h"
#include "DynamicObject.h"      // [b43_move]
#include "GridNotifiers.h"      // [b43_move]
#include "GridNotifiersImpl.h"  // [b43_move]
#include "Event.h"
#include "FleeManager.h"
#include "GameObject.h"
#include "Group.h"
#include "LastMovementValue.h"
#include "LeaderFightChecks.h"  // [lead1050] LeaderAggroAt
#include "LeaderOrders.h"  // [lead9] LEADER_SAFE_ZONE
#include "SoloSafety.h"        // [guild14]
#include "World.h"             // [guild14] CONFIG_CREATURE_LEASH_RADIUS
#include "DungeonTactics.h"   // [lead231] IsKnownCleaver
#include "LeaderKnowledge.h"  // [lead231] HasFrontalAttack
#include "LootObjectStack.h"
#include "HoRTriggers.h"
#include "InstanceGuid.h"  // [rot44]
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSplineInitArgs.h"
#include "MovementGenerator.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Position.h"
#include "PositionValue.h"
#include "PullStrategy.h"
#include "EstimatedLifetimeValue.h"
#include "Random.h"
#include "SafeZone.h"  // [lead9] the fight's safe zone
#include "SelfTravelActions.h"  // [lead860] IslandStepDown
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "Stances.h"
#include "TankAoeThreat.h"  // [b32_peel]
#include "TargetValue.h"
#include "TerrainHazard.h"
#include "Timer.h"
#include "Unit.h"
#include "Vehicle.h"
#include "WaypointMovementGenerator.h"
#include "G3D/Vector3.h"
#include <limits>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

MovementAction::MovementAction(PlayerbotAI* botAI, std::string const name) : Action(botAI, name)
{
    bot = botAI->GetBot();
}

namespace
{
    constexpr float OPEN_WATER_OFF_BOTTOM = 2.0f;  // this far above the bottom a point is in the water column
    constexpr float OPEN_WATER_ABOVE_LEVEL = 1.0f;  // a bot this close above the surface is still in the water
    // [hleft-chase-drop] a chase without a complete path to a target this far below is a fall off a ledge
    constexpr float CHASE_DROP_DZ = 8.0f;
    // [hleft-door-chase] one "[chase refused]" line per target this often
    constexpr uint32 CHASE_REFUSED_LOG_MS = 10 * IN_MILLISECONDS;

    // A bot in water (or right above its surface) heading for a point under water in sight, one of the two off the
    // bottom: it swims straight there. Blackfathom Deeps wave 38: after "surface for air" the tank floated 9.5 yd
    // above its Sea Witch for five minutes ("reach melee" moves to the surface above it, the navmesh path keeps to
    // the surface layer). A bot has no client to set MOVEMENTFLAG_SWIMMING, so MoveTo's isSwimming() is false here.
    bool SwimsTo(Player* bot, float x, float y, float z)
    {
        Map* map = bot->GetMap();
        if (!map || bot->IsFlying() || bot->GetVehicle() || bot->GetTransport() || bot->HasWaterWalkAura())
            return false;

        LiquidData const& liquid = bot->GetLiquidData();
        if (liquid.Status == LIQUID_MAP_NO_WATER || liquid.Level <= INVALID_HEIGHT ||
            !(liquid.Flags & (MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN)) ||
            bot->GetPositionZ() - liquid.Level > OPEN_WATER_ABOVE_LEVEL)
            return false;

        if (!map->IsUnderWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
            return false;

        float const botGround = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        float const destGround = bot->GetMapHeight(x, y, z);
        bool const botOffBottom =
            botGround <= INVALID_HEIGHT || bot->GetPositionZ() - botGround >= OPEN_WATER_OFF_BOTTOM;
        bool const destOffBottom = destGround <= INVALID_HEIGHT || z - destGround >= OPEN_WATER_OFF_BOTTOM;
        if (!botOffBottom && !destOffBottom)
            return false;  // along the bottom the navmesh path is right

        return bot->IsWithinLOS(x, y, z);
    }

    // [b33_sp] A drop into water JudgeLedgeDrop allowed (safe for the mover and the members near) that no ground path
    // carries: the mmaps join the walkway to the water below only by an off-mesh link, which the bot's own path
    // (PathGenerator) does not follow - it ends at the brink, the move is a no-op, and a straight spline ends in the
    // air. sp-h 20261026-000434-sp-h-5F-50 (843-1001 s): the route's link from (-187.0,-415.2,55.7) to the pool at
    // (-191.0,-415.3,8.4) (map 547; its twin at -349 was taken by luck in the passing runs); the leader stood at the
    // tip (-190.1,-415.7,53.5) with every member beside it, "shortcut_drop ... water, safe" 35 times, nobody moved, the
    // run stalled. Earlier passes left a member hanging at z 46-48 over the pool for minutes (123230-86). The drop is
    // made as a player makes it: from the brink, a jump into the water - and only together: every living member is
    // down there already or near enough to follow at once (the landing has no way back up), out of combat unless
    // only this mover is still up. Otherwise the walk stays as it was (the route's other way, the harness's skip).
    constexpr float WATER_DROP_BRINK_GAIN = 3.0f;  // the own walk comes no nearer (across) than this: at the brink
    constexpr float WATER_DROP_REACH = 30.0f;      // the jump's reach across
    constexpr float WATER_DROP_TOGETHER = 30.0f;   // a member this near follows the jump at once
    constexpr uint32 WATER_DROP_LOG_MS = 10 * IN_MILLISECONDS;
    constexpr float DRY_DROP_LEVEL_DZ = 2.0f;  // [b35_brd2] a dry jump spot's floor this near the landing's level

    enum class WaterDrop
    {
        Walk,   // no water drop for the jump: the walk as before
        Jump,   // jump to `jump`
        Under,  // the jump is under way
        Hold    // the group is not together: no walk off the brink
    };

    WaterDrop JudgeWaterDrop(Player* bot, LedgeDropVerdict const& ledge, float x, float y, float z,
                             G3D::Vector3& jump, std::string& why)
    {
        Map* map = bot->GetMap();
        // [b35_brd2] ... or onto navmesh floor below with no fall damage (a dry drop). brd-n 20261026-024524-brd-n-5F-6
        // (2575-3321 s): the tank on the Ring of Law stands (643.3,-293.6,-34.3), the stands' navmesh joined to the
        // z -43 walkway below by no ground path; "shortcut_drop ... dry, navmesh under the destination (no ground
        // path), height 8.7, safe" 34 times, the walk ended at the brink, the tank stood idle 188 s. A player hops
        // off the stands: the same jump from the brink, together, onto floor under LEDGE_FALL_SAFE - the tank first
        // (a member never jumps while the group's tank is still up: no split onto another level).
        bool const dry = !ledge.water && ledge.onMesh && ledge.height <= LEDGE_FALL_SAFE;
        if (!map || !ledge.drop || !ledge.allowed || !(ledge.water || dry) || !ledge.safe || ledge.groundPath ||
            bot->GetVehicle() || bot->GetTransport() || bot->IsFlying())
            return WaterDrop::Walk;
        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE ||
            bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING))
            return WaterDrop::Under;

        // At the brink: the own walk ends no nearer (across) to the destination, and not below it.
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        float const hereLeft = std::hypot(x - here.x, y - here.y);
        float endLeft = hereLeft;
        BotPath const path = FindBotPath(bot, x, y, z);  // [lead109]
        if (!(path.type & PATHFIND_NOPATH))
        {
            G3D::Vector3 const& end = path.actualEnd;
            if (end.z <= ledge.landZ + LEDGE_DROP_DZ)
                return WaterDrop::Walk;  // the walk goes down by itself
            endLeft = std::hypot(x - end.x, y - end.y);
        }
        if (hereLeft - endLeft > WATER_DROP_BRINK_GAIN)
            return WaterDrop::Walk;  // to the brink first

        // The landing, or the nearest spot round it in swimming water in sight (the cliff foot can be rock).
        G3D::Vector3 const landing(ledge.landX, ledge.landY, ledge.landZ);
        float const lift = bot->GetCollisionHeight();
        bool found = false;
        // [b35_brd2] dry: a spot just off the brink toward the landing (fanned +-60 deg), on floor of the landing's
        // level, in sight, under the fall damage height and joined to the landing by a ground path
        if (dry)
        {
            float const toward = std::atan2(ledge.landY - here.y, ledge.landX - here.x);
            for (float const reach : {3.0f, 4.5f, 6.0f, 8.0f, 10.0f, 12.0f})
            {
                for (float const turn : {0.0f, 0.5f, -0.5f, 1.0f, -1.0f})
                {
                    float const sx = here.x + reach * std::cos(toward + turn);
                    float const sy = here.y + reach * std::sin(toward + turn);
                    // the topmost floor there (from above the brink): a spot still over the stands' own floor is none
                    float const floor = map->GetHeight(bot->GetPhaseMask(), sx, sy, here.z + 2.0f, true,
                                                       LEDGE_FALL_SAFE + 4.0f);
                    if (floor <= INVALID_HEIGHT || std::fabs(floor - ledge.landZ) > DRY_DROP_LEVEL_DZ ||
                        here.z - floor <= LEDGE_DROP_DZ || here.z - floor > LEDGE_FALL_SAFE ||
                        !bot->IsWithinLOS(sx, sy, floor + lift))
                        continue;
                    BotPath const onward =  // [lead109]
                        FindBotPath(bot, sx, sy, floor, ledge.landX, ledge.landY, ledge.landZ);
                    PathType const type = onward.type;
                    if (!(type & PATHFIND_NORMAL) || (type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)))
                        continue;
                    jump = G3D::Vector3(sx, sy, floor);
                    found = true;
                    break;
                }
                if (found)
                    break;
            }
            if (!found)
            {
                why = "no floor of the landing's level in sight just off the brink";
                return WaterDrop::Walk;
            }
        }
        for (float const radius : {0.0f, 2.0f, 4.0f, 6.0f})
        {
            if (dry)
                break;
            int const steps = radius > 0.0f ? 8 : 1;
            for (int step = 0; step < steps && !found; ++step)
            {
                float const angle = float(step) * float(M_PI) / 4.0f;
                G3D::Vector3 const spot(landing.x + radius * std::cos(angle), landing.y + radius * std::sin(angle),
                                        landing.z);
                if (std::hypot(spot.x - here.x, spot.y - here.y) > WATER_DROP_REACH ||
                    !HasSwimWater(bot, spot.x, spot.y, spot.z) ||  // [lead145]
                    !bot->IsWithinLOS(spot.x, spot.y, spot.z + lift))
                    continue;
                jump = spot;
                found = true;
            }
            if (found)
                break;
        }
        if (!found)
        {
            why = "no swimming water in sight within reach of the landing";
            return WaterDrop::Walk;
        }

        // Together: every living member down there already, or near enough to follow (out of combat).
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !member->IsAlive() || member->GetMap() != map ||
                    member->GetPositionZ() <= jump.z + LEDGE_DROP_DZ)
                    continue;
                // [b35_brd2] a dry drop: the tank goes first, a member waits while the tank is still up
                bool const tankUp = dry && PlayerbotAI::IsTank(member) && !PlayerbotAI::IsTank(bot);
                if (bot->IsInCombat() || member->GetExactDist(bot) > WATER_DROP_TOGETHER || tankUp)
                {
                    why = std::string("waits for ") + member->GetName() + (bot->IsInCombat() ? " (in combat)" : "") +
                          ", " + std::to_string(int32(member->GetExactDist(bot))) + " yd, still up" +
                          (tankUp ? " (the tank first)" : "");
                    // No new stall: a hold of WATER_DROP_HOLD_MAX_MS falls back to the walk as before (the route's
                    // other way, the harness's skip); no jump without the group.
                    struct HoldClock
                    {
                        uint32 sinceMs = 0;
                        uint32 lastMs = 0;
                    };
                    thread_local std::unordered_map<uint64, HoldClock> holds;
                    constexpr uint32 WATER_DROP_HOLD_MAX_MS = 60 * IN_MILLISECONDS;
                    constexpr uint32 WATER_DROP_HOLD_GAP_MS = 10 * IN_MILLISECONDS;
                    uint32 const now = getMSTime();
                    HoldClock& hold = holds[bot->GetGUID().GetRawValue()];
                    if (!hold.lastMs || getMSTimeDiff(hold.lastMs, now) > WATER_DROP_HOLD_GAP_MS)
                        hold.sinceMs = now ? now : 1;
                    hold.lastMs = now ? now : 1;
                    if (getMSTimeDiff(hold.sinceMs, now) > WATER_DROP_HOLD_MAX_MS)
                    {
                        why += ": held 60 s, the walk as before";
                        return WaterDrop::Walk;
                    }
                    return WaterDrop::Hold;
                }
            }
        return WaterDrop::Jump;
    }
}

bool MovementAction::PassesAggroZGate(Creature* mob, Unit* who, float z)
{
    if (mob->CanFly())
        return true;

    float const dz = std::fabs(mob->GetPositionZ() - z) - (mob->GetObjectSize() + who->GetObjectSize());
    return std::max(0.0f, dz) <= CREATURE_Z_ATTACK_RANGE + mob->m_CombatDistance;
}

float MovementAction::IdleAggroReach(Creature* mob, Unit* who, float margin)
{
    return mob->GetAggroRange(who) + mob->m_CombatDistance + margin;
}

namespace
{
    // The mob's sight of `who` standing at the point: its eyes there (IsWithinLOS adds the mob's own height).
    bool SeesWalkerAt(Creature* mob, Unit* who, float x, float y, float z)
    {
        return mob->IsWithinLOS(x, y, z + who->GetCollisionHeight() - mob->GetCollisionHeight());
    }

    // [b43_move] A spot at the brink of a floor that hangs over an idle mob below. The core's proximity aggro never
    // reaches it (PassesAggroZGate: dz over CREATURE_Z_ATTACK_RANGE + combat distance), and a brink does not need it
    // to: a summon or a mob knocked / feared off the edge lands among the mobs below and their assistance wakes the
    // room; a caster at the brink sees and is seen by the room (its spells, a pet or a dps whose target stands at the
    // edge); and a woken room comes straight up. So a spot is refused when a grounded idle mob stands below it (more
    // than the Z gate, at most LOWER_FLOOR_MAX_DZ), within its aggro reach in 2D (the spot overhangs its reach), the
    // ground drops off within BRINK_PROBE of the spot (a brink, not a ceiling over a closed room), and the mob sees it.
    // scholo-h 20261026-071042-scholo-h-5E-3 (608 s): the tank's fight hold edge on the Necrolyte balcony (263,87-93,
    // z 110) over the Aberration room (z 95.8, dz 14): "[far engage] Risen Lackey ... at 257.0 101.1 95.9" and the
    // room's Aberrations / Ghoul / Corpse woke in the same tick, 3 deaths.
    constexpr float LOWER_FLOOR_MAX_DZ = 30.0f;
    constexpr float BRINK_DROP = 5.0f;         // the ground this much lower is off the brink
    constexpr std::array<float, 2> BRINK_PROBE = { 3.0f, 6.0f };
    constexpr uint32 BRINK_DIRECTIONS = 8;

    // `brink` memoises the point's drop test over the mobs judged (-1 not tested yet, 0 no, 1 yes).
    bool BrinkAboveIdleMob(Creature* mob, Unit* who, float x, float y, float z, float margin, int8& brink)
    {
        if (mob->CanFly() || MovementAction::PassesAggroZGate(mob, who, z))
            return false;

        float const dz = z - mob->GetPositionZ();
        if (dz <= 0.0f || dz > LOWER_FLOOR_MAX_DZ ||
            mob->GetExactDist2d(x, y) >= MovementAction::IdleAggroReach(mob, who, margin))
            return false;

        if (brink < 0)
        {
            brink = 0;
            for (float const probe : BRINK_PROBE)
                for (uint32 i = 0; i < BRINK_DIRECTIONS && !brink; ++i)
                {
                    float const angle = 2.0f * static_cast<float>(M_PI) * i / BRINK_DIRECTIONS;
                    float const px = x + std::cos(angle) * probe;
                    float const py = y + std::sin(angle) * probe;
                    // INVALID_HEIGHT (no floor found) counts as a drop
                    brink = who->GetMapHeight(px, py, z + 1.0f) < z - BRINK_DROP ? 1 : 0;
                }
        }

        return brink > 0 && SeesWalkerAt(mob, who, x, y, z);
    }
}

char const* MovementAction::IdleAggroBlocker(Creature* mob, Unit* who, float x, float y, float z, float margin,
                                             bool checkLos)
{
    if (!PassesAggroZGate(mob, who, z))
        return "z gate";

    if (mob->GetExactDist(x, y, z) >= IdleAggroReach(mob, who, margin))
        return "range";

    if (checkLos && !SeesWalkerAt(mob, who, x, y, z))
        return "los";

    return nullptr;
}

std::vector<MovementAction::SightArcCandidate> MovementAction::SightArcCandidates(Unit* walker, Unit* target,
                                                                                 float minRadius, float maxRadius,
                                                                                 float floorDz, uint32 maxLos)
{
    constexpr float SIGHT_ARC_RING_STEP = 3.0f;
    constexpr uint32 SIGHT_ARC_ANGLES = 24;  // 15 degrees apart
    std::vector<SightArcCandidate> result;
    if (!walker || !target || !walker->IsInWorld() || !target->IsInWorld() || walker->GetMap() != target->GetMap() ||
        maxRadius < minRadius || maxRadius <= 0.0f)
        return result;

    float const toWalker = target->GetAngle(walker);
    float const eye = walker->GetCollisionHeight();
    std::vector<SightArcCandidate> ground;
    for (float radius = maxRadius; radius >= minRadius - 0.01f; radius -= SIGHT_ARC_RING_STEP)
    {
        for (uint32 i = 0; i < SIGHT_ARC_ANGLES; ++i)
        {
            // 0, +15, -15, +30, -30 ... degrees off the walker's side
            int32 const k = (i % 2) ? static_cast<int32>((i + 1) / 2) : -static_cast<int32>(i / 2);
            float const off = static_cast<float>(k) * 2.0f * static_cast<float>(M_PI) / SIGHT_ARC_ANGLES;
            float x = target->GetPositionX() + std::cos(toWalker + off) * radius;
            float y = target->GetPositionY() + std::sin(toWalker + off) * radius;
            float z = target->GetPositionZ();
            walker->UpdateAllowedPositionZ(x, y, z);
            if (std::fabs(z - target->GetPositionZ()) > floorDz)
                continue;

            SightArcCandidate candidate;
            candidate.x = x;
            candidate.y = y;
            candidate.z = z;
            candidate.straight = walker->GetExactDist(x, y, z);
            candidate.radius = radius;
            candidate.angleOffDeg = std::fabs(static_cast<float>(k)) * 360.0f / SIGHT_ARC_ANGLES;
            ground.push_back(candidate);
        }

        if (radius - SIGHT_ARC_RING_STEP < minRadius - 0.01f && radius > minRadius + 0.01f)
            radius = minRadius + SIGHT_ARC_RING_STEP;  // the last ring lies on minRadius itself
    }

    std::sort(ground.begin(), ground.end(),
              [](SightArcCandidate const& a, SightArcCandidate const& b) { return a.straight < b.straight; });
    uint32 los = 0;
    for (SightArcCandidate const& candidate : ground)
    {
        if (los++ >= maxLos)
            break;

        if (target->IsWithinLOS(candidate.x, candidate.y, candidate.z + eye))
            result.push_back(candidate);
    }

    return result;
}

// Idle-mob path guard. The reach is the core's proximity aggro (IdleAggroBlocker, Creature::CanStartAttack): the Z
// gate per point, aggro range and combat distance in 3D plus a margin, and the mob sees the point.
namespace
{
    constexpr float GUARD_AGGRO_MARGIN = MovementAction::SIGHT_IDLE_AGGRO_MARGIN;  // was 2.0 ([sight-arc])
    constexpr float GUARD_SCAN_LENGTH = 40.0f;  // the path is judged this far; the next move looks again
    constexpr size_t GUARD_MAX_MOBS = 12;       // the nearest idle mobs judged
    constexpr uint32 GUARD_MAX_LOS = 8;         // line of sight checks one judged path may spend
    constexpr uint32 GUARD_MAX_ALT_PATHS = 4;   // other destinations searched before the bot stays
    // Out of a ground effect (AVOID): spots just outside it all round, this far past its edge, searched (nearest
    // first) before the tank's side; with none that wakes nothing the bot stays in it above this health.
    constexpr uint32 GUARD_AVOID_DIRECTIONS = 8;
    constexpr uint32 GUARD_AVOID_MAX_PATHS = 6;
    constexpr float GUARD_AVOID_EDGE = 1.5f;
    constexpr float GUARD_AVOID_STAY_HEALTH_PCT = 50.0f;
    constexpr float GUARD_MIN_SHORTER = 3.0f;   // a shorter stop that walks less than this is no move
    constexpr float GUARD_STOP_BACK = 1.0f;     // a shorter stop ends this far before the reach it would enter
    constexpr float GUARD_RING_INSIDE = 0.5f;   // a spot on the ring stands this far inside the asked range
    // [melee_behind_tank] an idle mob the tank (or its trail) stood inside the reach of: a walk stays this much
    // farther from it than the tank stood
    constexpr float TANK_EXEMPT_BEHIND = 0.5f;
    constexpr float GUARD_RING_STEP = static_cast<float>(M_PI) / 6.0f;
    constexpr float GUARD_TANK_SIDE = 3.0f;     // a spot next to the tank, toward the bot
    constexpr float GUARD_ALT_DETOUR = 2.0f;    // an alternative walks at most this times the asked path, plus
    constexpr float GUARD_ALT_DETOUR_ADD = 10.0f;
    constexpr uint32 GUARD_LOG_MS = 5 * IN_MILLISECONDS;
    constexpr uint32 GUARD_VETO_MEMO_MS = IN_MILLISECONDS;
    constexpr float GUARD_VETO_MEMO_DISTANCE = 2.0f;

    // The tank's trail: where it stood, sampled by its own moves and by every guarded move of its members (the test
    // harness walks the leader by MotionMaster, past MoveTo: RFC 2026-10-24, no trail at all). Shared between
    // map threads (one entry per tank), so behind a lock; entries are tiny and replaced when the tank moves on.
    constexpr float TRAIL_STEP = 2.0f;           // a new point once the tank is this far from the last one
    constexpr float TRAIL_JUMP = 45.0f;          // a gap this long (a teleport, nobody looked): the trail starts anew
    constexpr size_t TRAIL_MAX_POINTS = 128;     // 256 yd: a member left far behind still finds the trail
    constexpr uint32 TRAIL_STALE_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr float TRAIL_FLOOR = 5.0f;          // a trail point this far above/below the bot is another floor
    constexpr float TRAIL_BACK_SLACK = 3.0f;     // a trail leg farther than this from a walk's goal than the bot is
                                                 // leads away from it ([sight-trail-back])
    constexpr float TRAIL_JOIN = 6.0f;           // farther from the trail than this, the bot walks onto it first
    constexpr float TRAIL_LEG = 20.0f;           // one move walks at most this much of the trail
    constexpr float TRAIL_CORRIDOR = 2.5f;       // the trail points a leg skips stay this close to its line

    struct TankTrail
    {
        uint32 mapId = 0;
        uint32 instanceId = 0;
        uint32 lastMs = 0;
        std::deque<G3D::Vector3> points;
    };

    std::mutex trailLock;
    std::unordered_map<ObjectGuid, TankTrail> tankTrails;

    void RecordTrail(Player* tank)
    {
        G3D::Vector3 const here(tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(trailLock);
        TankTrail& trail = tankTrails[tank->GetGUID()];
        if (trail.points.empty() || trail.mapId != tank->GetMapId() || trail.instanceId != tank->GetInstanceId() ||
            getMSTimeDiff(trail.lastMs, now) > TRAIL_STALE_MS || (here - trail.points.back()).length() > TRAIL_JUMP)
        {
            trail.points.clear();
            trail.mapId = tank->GetMapId();
            trail.instanceId = tank->GetInstanceId();
            trail.points.push_back(here);
        }
        else if ((here - trail.points.back()).length() >= TRAIL_STEP)
        {
            trail.points.push_back(here);
            while (trail.points.size() > TRAIL_MAX_POINTS)
                trail.points.pop_front();
        }

        trail.lastMs = now;
    }

    std::vector<G3D::Vector3> GetTrail(Player* tank)
    {
        std::lock_guard<std::mutex> guard(trailLock);
        auto const found = tankTrails.find(tank->GetGUID());
        if (found == tankTrails.end() || found->second.mapId != tank->GetMapId() ||
            found->second.instanceId != tank->GetInstanceId())
            return {};

        return std::vector<G3D::Vector3>(found->second.points.begin(), found->second.points.end());
    }

    // The next leg of the trail toward the tank for a bot at `here`: onto the nearest trail point on its floor when it
    // stands off the trail (onTrail false), else as far along as one leg whose skipped points stay near its line,
    // `distance` short of the tank. Not found: no trail on its floor, or nothing left to walk.
    struct TrailLeg
    {
        bool found = false;
        bool onTrail = false;
        size_t from = 0;
        size_t to = 0;
        G3D::Vector3 goal;
    };

    TrailLeg NextTrailLeg(std::vector<G3D::Vector3> const& trail, G3D::Vector3 const& here,
                          G3D::Vector3 const& tankPos, float distance)
    {
        TrailLeg leg;
        size_t nearest = trail.size();
        float nearestDistance = 0.0f;
        for (size_t i = 0; i < trail.size(); ++i)
        {
            if (std::fabs(trail[i].z - here.z) > TRAIL_FLOOR)
                continue;

            float const distanceTo = (trail[i] - here).length();
            if (nearest == trail.size() || distanceTo < nearestDistance)
            {
                nearest = i;
                nearestDistance = distanceTo;
            }
        }

        if (nearest == trail.size())
            return leg;

        std::vector<float> toTank(trail.size());
        toTank.back() = (tankPos - trail.back()).length();
        for (size_t i = trail.size() - 1; i-- > 0;)
            toTank[i] = toTank[i + 1] + (trail[i + 1] - trail[i]).length();

        if (toTank[nearest] < distance)
            return leg;

        leg.from = nearest;
        leg.to = nearest;
        if (nearestDistance > TRAIL_JOIN)
        {
            leg.found = true;
            leg.goal = trail[nearest];
            return leg;
        }

        float walked = 0.0f;
        for (size_t j = nearest + 1; j < trail.size(); ++j)
        {
            walked += (trail[j] - trail[j - 1]).length();
            if (walked > TRAIL_LEG || toTank[j] < distance)
                break;

            G3D::Vector3 const line = trail[j] - here;
            float const lineSq = line.squaredLength();
            bool inCorridor = true;
            for (size_t k = nearest; k < j && inCorridor; ++k)
            {
                float along = 0.0f;
                if (lineSq > 0.0f)
                    along = std::clamp((trail[k] - here).dot(line) / lineSq, 0.0f, 1.0f);

                inCorridor = (trail[k] - (here + line * along)).length() <= TRAIL_CORRIDOR;
            }

            if (!inCorridor)
                break;

            leg.to = j;
        }

        if (leg.to == nearest && nearest + 1 < trail.size() && nearestDistance < TRAIL_STEP &&
            toTank[nearest + 1] >= distance)
            leg.to = nearest + 1;  // a long gap in the trail: one point at a time

        if (leg.to == nearest && nearestDistance < TRAIL_STEP)
            return leg;  // on the trail as far as the follow distance allows

        leg.found = true;
        leg.onTrail = true;
        leg.goal = trail[leg.to];
        return leg;
    }

    // Whether every point of `path` stays within `slack` of the polyline `here`, trail[from..to]: a walk on the
    // ground the tank walked.
    bool PathOnTrail(Movement::PointsArray const& path, G3D::Vector3 const& here,
                     std::vector<G3D::Vector3> const& trail, size_t from, size_t to, float slack)
    {
        std::vector<G3D::Vector3> line{ here };
        for (size_t i = from; i <= to && i < trail.size(); ++i)
            line.push_back(trail[i]);

        for (G3D::Vector3 const& point : path)
        {
            bool onLine = false;
            for (size_t i = 1; i < line.size() && !onLine; ++i)
            {
                G3D::Vector3 const segment = line[i] - line[i - 1];
                float const lengthSq = segment.squaredLength();
                float along = 0.0f;
                if (lengthSq > 0.0f)
                    along = std::clamp((point - line[i - 1]).dot(segment) / lengthSq, 0.0f, 1.0f);

                onLine = (point - (line[i - 1] + segment * along)).length() <= slack;
            }

            if (!onLine)
                return false;
        }

        return true;
    }

    // A follower the guard keeps from its tank: since when its follows are vetoed in a row (the deadlock breaker's
    // clock). Per bot across its actions, shared between map threads, so behind a lock.
    constexpr uint32 DEADLOCK_VETO_MS = 5 * IN_MILLISECONDS;   // vetoed this long, the tank out of combat: it goes
    constexpr uint32 DEADLOCK_HARD_MS = 30 * IN_MILLISECONDS;  // vetoed this long it goes, whatever the tank does
    constexpr uint32 DEADLOCK_GAP_MS = 10 * IN_MILLISECONDS;   // a longer pause between vetoes starts a new streak
    constexpr float DEADLOCK_TANK_DISTANCE = 30.0f;            // only this far from the tank: behind, not beside it
    constexpr float FOLLOW_NEAR_TANK = 8.0f;  // a destination this close to the tank follows it (a formation spot)

    struct VetoStreak
    {
        uint32 firstMs = 0;
        uint32 lastMs = 0;
        bool breaking = false;  // the breaker let it go already in this streak (logged once unthrottled)
    };

    std::mutex vetoLock;
    std::unordered_map<ObjectGuid, VetoStreak> vetoStreaks;

    // How long the bot's follows have been vetoed in a row (0: no streak going).
    uint32 FollowVetoAge(Player* bot, uint32 now, bool& breaking)
    {
        std::lock_guard<std::mutex> guard(vetoLock);
        auto const found = vetoStreaks.find(bot->GetGUID());
        if (found == vetoStreaks.end() || getMSTimeDiff(found->second.lastMs, now) > DEADLOCK_GAP_MS)
        {
            breaking = false;
            return 0;
        }

        breaking = found->second.breaking;
        return getMSTimeDiff(found->second.firstMs, now);
    }

    void NoteFollowVeto(Player* bot, uint32 now, bool breaking)
    {
        std::lock_guard<std::mutex> guard(vetoLock);
        VetoStreak& streak = vetoStreaks[bot->GetGUID()];
        if (!streak.lastMs || getMSTimeDiff(streak.lastMs, now) > DEADLOCK_GAP_MS)
        {
            streak.firstMs = now ? now : 1;
            streak.breaking = false;
        }

        streak.lastMs = now ? now : 1;
        streak.breaking = streak.breaking || breaking;
    }

    void ClearFollowVeto(Player* bot)
    {
        std::lock_guard<std::mutex> guard(vetoLock);
        vetoStreaks.erase(bot->GetGUID());
    }

    // The group's tank on the bot's map, alive: the main tank first.
    Player* FindGroupTank(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        Player* tank = nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
                member->GetMap() != bot->GetMap() || !PlayerbotAI::IsTank(member))
                continue;

            if (PlayerbotAI::IsMainTank(member))
                return member;

            if (!tank)
                tank = member;
        }

        return tank;
    }

    // A healer of the group on the bot's map, alive (the first one).
    Player* FindGroupHealer(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsInWorld() && member->IsAlive() &&
                member->GetMap() == bot->GetMap() && PlayerbotAI::IsHeal(member))
                return member;
        }

        return nullptr;
    }

    struct IdleReach
    {
        Creature* mob;
        float reach;
        float distance;  // from the bot
    };

    // [b40_open] How near the straight walk from `from` to `to` comes to `point`.
    float SegmentDistance(G3D::Vector3 const& point, G3D::Vector3 const& from, G3D::Vector3 const& to)
    {
        G3D::Vector3 const segment = to - from;
        float const lengthSq = segment.squaredLength();
        float along = 0.0f;
        if (lengthSq > 0.0f)
            along = std::clamp((point - from).dot(segment) / lengthSq, 0.0f, 1.0f);

        return (point - (from + segment * along)).length();
    }

    // The idle aggressive hostile mobs whose reach a walk of up to GUARD_SCAN_LENGTH could enter, nearest first.
    // Left out: a mob the bot stands in the reach of already (it blocks only a walk that comes nearer), one the tank
    // stands in the reach of, or walked through, in its sight (it stays idle for the group), the move's and the bot's
    // own target. A mob the line of sight budget does not cover is kept (judged as unpassed).
    // [b40_open] With the walk's end known, the GUARD_MAX_MOBS kept are the ones the walk comes deepest into the reach
    // of, not the nearest to the bot. brd-h 20261026-060218-brd-h-5F-5 pull 11 (484 s, "unplanned: Rthunac",
    // firstMobVictim Rthunac, the hunter never hit): the hunter caught up 26 yd behind the tank, past the East
    // Garrison door packs and the Torturer pack; the 12 nearest idle mobs to it were those, High Interrogator
    // Gerstahn 42 yd away was cut, and its formation spot 2.8 yd past the tank, 17.7 yd from him (reach 23.5, the
    // druid and the rogue were vetoed on the same spots seconds before), went unjudged. Gerstahn woke on the hunter
    // and killed the healer.
    std::vector<IdleReach> CollectIdleReaches(PlayerbotAI* botAI, Player* bot, GuidVector const& possible,
                                              Player* tank, std::vector<G3D::Vector3> const& trail, ObjectGuid exempt,
                                              ObjectGuid exemptToo, G3D::Vector3 const* walkEnd = nullptr,
                                              bool keepAll = false)
    {
        std::vector<IdleReach> reaches;
        uint32 losBudget = GUARD_MAX_LOS;
        for (ObjectGuid const& guid : possible)
        {
            if (guid == exempt || guid == exemptToo || Leader::PullPack::Contains(bot, guid))  // [lead29] the pull
                continue;

            Creature* mob = botAI->GetCreature(guid);
            if (!mob || !mob->IsInWorld() || !mob->IsAlive() || mob->IsInCombat() || mob->GetMap() != bot->GetMap() ||
                !bot->IsHostileTo(mob) || !mob->HasReactState(REACT_AGGRESSIVE) || mob->IsCivilian() ||
                mob->IsNeutralToAll() || IsCrowdControlled(mob))
                continue;

            float const distance = mob->GetExactDist(bot);
            float reach = MovementAction::IdleAggroReach(mob, bot, GUARD_AGGRO_MARGIN);
            if (distance > GUARD_SCAN_LENGTH + reach)
                continue;

            reach = std::min(reach, distance - 1.0f);
            if (reach <= 0.0f)
                continue;

            // Seen within its reach and still idle: the tank where it stands, or the trail point nearest to it. A
            // point the Z gate shuts proves nothing (the mob cannot wake on anyone there).
            // The exemption proves the mob stays idle only for a tank REALLY inside its aggro: inside the guard margin
            // band it proves nothing (Scholomance heroic 22:31: the tank at 22.1 yd of a 21 yd aggro "exempted" the
            // Neophyte, the hunter's follow spot was 20.3 yd from it and woke the pack).
            // [melee_behind_tank] The exemption lets a walk only as near to the mob as the tank (or its trail) stood:
            // never past it. scholo-h 20261025-153805 pull 3 (15:40:28, "unplanned: Rtshaab ... firstMobVictim
            // Rtshaab", the shaman never hit): the tank stood idle inside a Neophyte's reach at the ramp foot, the
            // exemption dropped the mob, and the shaman's follow spot 1-3 yd further down the ramp woke it. A mob that
            // stayed idle with the tank at d yd proves nothing for a point nearer than d.
            float const realReach = MovementAction::IdleAggroReach(mob, bot, 0.0f);
            if (tank && tank != bot && mob->GetExactDist(tank) < realReach &&
                MovementAction::PassesAggroZGate(mob, tank, tank->GetPositionZ()) && losBudget)
            {
                --losBudget;
                if (mob->IsWithinLOSInMap(tank))
                {
                    float const behindTank = mob->GetExactDist(tank) - TANK_EXEMPT_BEHIND;
                    if (behindTank <= 0.0f || distance <= behindTank)
                        continue;  // the bot already stands that near (or nearer): it blocks only a walk nearer

                    reaches.push_back({ mob, std::min(reach, behindTank), distance });
                    continue;
                }
            }

            G3D::Vector3 const* passed = nullptr;
            float passedDistance = std::min(reach, realReach);
            for (G3D::Vector3 const& point : trail)
            {
                float const pointDistance = mob->GetExactDist(point.x, point.y, point.z);
                if (pointDistance < passedDistance && MovementAction::PassesAggroZGate(mob, bot, point.z))
                {
                    passed = &point;
                    passedDistance = pointDistance;
                }
            }

            if (passed && losBudget)
            {
                --losBudget;
                if (SeesWalkerAt(mob, bot, passed->x, passed->y, passed->z))
                {
                    // [melee_behind_tank] as near as the trail came, no nearer
                    float const alongTrail = passedDistance - TANK_EXEMPT_BEHIND;
                    if (alongTrail <= 0.0f || distance <= alongTrail)
                        continue;

                    reaches.push_back({ mob, std::min(reach, alongTrail), distance });
                    continue;
                }
            }

            reaches.push_back({ mob, reach, distance });
        }

        // [b43_move] all of them (nearest first), for a caller that ranks them per walk (RankIdleReaches)
        if (keepAll)
        {
            std::sort(reaches.begin(), reaches.end(),
                      [](IdleReach const& left, IdleReach const& right) { return left.distance < right.distance; });
            return reaches;
        }

        // [b40_open] the deepest the straight walk comes into each reach (negative: inside it) picks the ones kept
        if (walkEnd && reaches.size() > GUARD_MAX_MOBS)
        {
            G3D::Vector3 const from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            auto const depth = [&](IdleReach const& idle)
            {
                G3D::Vector3 const mob(idle.mob->GetPositionX(), idle.mob->GetPositionY(),
                                       idle.mob->GetPositionZ());
                return SegmentDistance(mob, from, *walkEnd) - idle.reach;
            };
            std::sort(reaches.begin(), reaches.end(),
                      [&](IdleReach const& left, IdleReach const& right) { return depth(left) < depth(right); });
            reaches.resize(GUARD_MAX_MOBS);
        }

        std::sort(reaches.begin(), reaches.end(),
                  [](IdleReach const& left, IdleReach const& right) { return left.distance < right.distance; });
        if (reaches.size() > GUARD_MAX_MOBS)
            reaches.resize(GUARD_MAX_MOBS);

        return reaches;
    }

    // [b43_move] The GUARD_MAX_MOBS of `all` (CollectIdleReaches keepAll) that the walk along `walk` comes deepest
    // into the reach of, nearest first: the [b40_open] cut made for each walk judged, not once for the asked one.
    // [b40_open] follow-up (brd-h 5F-5): the guard's other destinations (a ring spot, beside the tank, out of a ground
    // effect, a trail leg) were judged against the 12 mobs ranked for the asked walk, so a pack near an alternative
    // but far from the asked line went unjudged on it.
    std::vector<IdleReach> RankIdleReaches(std::vector<IdleReach> const& all, Movement::PointsArray const& walk)
    {
        std::vector<IdleReach> reaches = all;
        if (reaches.size() > GUARD_MAX_MOBS && walk.size() >= 2)
        {
            auto const depth = [&](IdleReach const& idle)
            {
                G3D::Vector3 const mob(idle.mob->GetPositionX(), idle.mob->GetPositionY(),
                                       idle.mob->GetPositionZ());
                float nearest = std::numeric_limits<float>::max();
                for (size_t i = 1; i < walk.size(); ++i)
                    nearest = std::min(nearest, SegmentDistance(mob, walk[i - 1], walk[i]));

                return nearest - idle.reach;
            };
            std::vector<float> depths;
            depths.reserve(reaches.size());
            for (IdleReach const& idle : reaches)
                depths.push_back(depth(idle));

            std::vector<size_t> order(reaches.size());
            for (size_t i = 0; i < order.size(); ++i)
                order[i] = i;

            std::partial_sort(order.begin(), order.begin() + GUARD_MAX_MOBS, order.end(),
                              [&](size_t left, size_t right) { return depths[left] < depths[right]; });
            std::vector<IdleReach> kept;
            kept.reserve(GUARD_MAX_MOBS);
            for (size_t i = 0; i < GUARD_MAX_MOBS; ++i)
                kept.push_back(reaches[order[i]]);

            reaches.swap(kept);
        }

        std::sort(reaches.begin(), reaches.end(),
                  [](IdleReach const& left, IdleReach const& right) { return left.distance < right.distance; });
        if (reaches.size() > GUARD_MAX_MOBS)
            reaches.resize(GUARD_MAX_MOBS);

        return reaches;
    }

    struct PathWake
    {
        Creature* mob = nullptr;
        float distance = 0.0f;  // the path's nearest approach to the mob
        float reach = 0.0f;
        float walked = 0.0f;    // along the path to where it enters the reach
        float dz = 0.0f;        // the mob's height over (+) or under (-) the point where it wakes
        bool losChecked = false;  // false: the line of sight budget was spent, unseen counted as seen
    };

    // The first idle mob the walk along `points` wakes (the core's proximity aggro, IdleAggroBlocker): the first point
    // where a segment enters a reach, passes the mob's Z gate (entry, closest approach or exit; a mob on a ledge or a
    // deck above / below never wakes) and the mob sees the walker there. Out of line of sight checks counts as seen.
    PathWake FirstWake(Movement::PointsArray const& points, std::vector<IdleReach> const& reaches, Player* bot)
    {
        PathWake wake;
        uint32 losBudget = GUARD_MAX_LOS;
        float walked = 0.0f;
        // [lead1450] b266 rfc-h 5G-15 #12 206-229 s: the frost DK's walk to a melee place no nearer the idle Trogg than
        // where it stood was refused at its first yard ("would wake Ragefire Trogg (20.9 yd of reach 22.5, 0.0 yd
        // along)"). A mob that passes the walker's Z gate and sees it where it stands, yet stays idle, does not reach
        // it there (Creature::CanStartAttack): a stretch of the walk no nearer it than that is out of its reach too.
        // Per mob, the walker's own distance where it starts inside the guard's reach (-1: no such proof)
        std::vector<float> standsUnwoken(reaches.size(), -1.0f);
        if (!points.empty())
        {
            G3D::Vector3 const& from = points[0];
            for (size_t r = 0; r < reaches.size(); ++r)
            {
                Creature* const mob = reaches[r].mob;
                float const gap = (G3D::Vector3(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ()) - from)
                                      .length();
                if (gap < reaches[r].reach && losBudget && MovementAction::PassesAggroZGate(mob, bot, from.z))
                {
                    --losBudget;
                    if (SeesWalkerAt(mob, bot, from.x, from.y, from.z))
                        standsUnwoken[r] = gap;
                }
            }
        }
        for (size_t i = 1; i < points.size() && walked <= GUARD_SCAN_LENGTH; ++i)
        {
            G3D::Vector3 const& start = points[i - 1];
            G3D::Vector3 const segment = points[i] - start;
            float const lengthSq = segment.squaredLength();
            float const length = std::sqrt(lengthSq);
            float firstEntry = -1.0f;
            for (size_t r = 0; r < reaches.size(); ++r)
            {
                IdleReach const& idle = reaches[r];
                G3D::Vector3 const mob(idle.mob->GetPositionX(), idle.mob->GetPositionY(), idle.mob->GetPositionZ());
                float along = 0.0f;
                if (lengthSq > 0.0f)
                    along = std::clamp((mob - start).dot(segment) / lengthSq, 0.0f, 1.0f);

                G3D::Vector3 const closest = start + segment * along;
                float const distance = (mob - closest).length();
                if (distance >= idle.reach || (standsUnwoken[r] >= 0.0f && distance >= standsUnwoken[r]))
                    continue;  // [lead1450] no nearer than where the mob already sees the walker idle

                float entry = along;
                float leave = along;
                if (length > 0.0f)
                {
                    float const half = std::sqrt(idle.reach * idle.reach - distance * distance) / length;
                    entry = std::max(0.0f, along - half);
                    leave = std::min(1.0f, along + half);
                }

                // The first of entry, closest approach and exit the Z gate lets through (its height changes
                // monotonically along the segment, so a gated pair brackets a gated stretch).
                float wakeAt = -1.0f;
                for (float const at : { entry, along, leave })
                {
                    G3D::Vector3 const point = start + segment * at;
                    if (MovementAction::PassesAggroZGate(idle.mob, bot, point.z))
                    {
                        wakeAt = at;
                        break;
                    }
                }

                if (wakeAt < 0.0f || (firstEntry >= 0.0f && wakeAt >= firstEntry))
                    continue;

                G3D::Vector3 const wakePoint = start + segment * wakeAt;
                bool seen = true;
                bool losChecked = false;
                if (losBudget)
                {
                    --losBudget;
                    losChecked = true;
                    seen = SeesWalkerAt(idle.mob, bot, wakePoint.x, wakePoint.y, wakePoint.z) ||
                           (wakeAt != along && MovementAction::PassesAggroZGate(idle.mob, bot, closest.z) &&
                            SeesWalkerAt(idle.mob, bot, closest.x, closest.y, closest.z));
                }

                if (!seen)
                    continue;

                firstEntry = wakeAt;
                wake.mob = idle.mob;
                wake.distance = distance;
                wake.reach = idle.reach;
                wake.walked = walked + wakeAt * length;
                wake.dz = mob.z - wakePoint.z;
                wake.losChecked = losChecked;
            }

            if (wake.mob)
                return wake;

            walked += length;
        }

        return wake;
    }

    // [b33_move] The shorter stop's margin. OHB heroic 2026-10-26 (20261026-000434-ohb-h-5H-20): "escort stay close to
    // (2227.9,119.7,82.3) would wake Durnholde Rifleman ... -> alt shorter (2167.8,135.2,85.4)": the stop ended 1 yd
    // before the Rifleman's reach but at the edge of its pack's, a Sentry hit the cat and the pull wiped the group. A
    // shorter stop keeps SHORTER_IDLE_MARGIN beyond the real aggro reach of every idle hostile (Z gate at the stop, no
    // line of sight exemption); with no such stop on the walk the bot holds. A mob the bot already stands nearer to
    // blocks only a stop nearer than that, one the tank stands idle inside the reach of (in its sight) only a stop
    // nearer than the tank stood (as CollectIdleReaches).
    constexpr float SHORTER_IDLE_MARGIN = 5.0f;
    constexpr float SHORTER_BACK_STEP = 2.0f;  // the stop is moved back along the walk this far per try

    struct ShorterKeep
    {
        Creature* mob;
        float keep;  // a stop nearer to the mob than this is too near
    };

    std::vector<ShorterKeep> CollectShorterKeeps(PlayerbotAI* botAI, Player* bot, GuidVector const& possible,
                                                 Player* tank, ObjectGuid exempt, ObjectGuid exemptToo)
    {
        std::vector<ShorterKeep> keeps;
        for (ObjectGuid const& guid : possible)
        {
            if (guid == exempt || guid == exemptToo || Leader::PullPack::Contains(bot, guid))  // [lead29] the pull
                continue;

            Creature* mob = botAI->GetCreature(guid);
            if (!mob || !mob->IsInWorld() || !mob->IsAlive() || mob->IsInCombat() || mob->GetMap() != bot->GetMap() ||
                !bot->IsHostileTo(mob) || !mob->HasReactState(REACT_AGGRESSIVE) || mob->IsCivilian() ||
                mob->IsNeutralToAll() || IsCrowdControlled(mob))
                continue;

            float keep = MovementAction::IdleAggroReach(mob, bot, SHORTER_IDLE_MARGIN);
            keep = std::min(keep, mob->GetExactDist(bot));
            float const realReach = MovementAction::IdleAggroReach(mob, bot, 0.0f);
            if (tank && tank != bot && mob->GetExactDist(tank) < realReach &&
                MovementAction::PassesAggroZGate(mob, tank, tank->GetPositionZ()) && mob->IsWithinLOSInMap(tank))
                keep = std::min(keep, mob->GetExactDist(tank) - TANK_EXEMPT_BEHIND);

            if (keep > 0.0f)
                keeps.push_back({ mob, keep });
        }

        return keeps;
    }

    Creature* ShorterStopTooNear(std::vector<ShorterKeep> const& keeps, Player* bot, G3D::Vector3 const& stop)
    {
        for (ShorterKeep const& idle : keeps)
            if (idle.mob->GetExactDist(stop.x, stop.y, stop.z) < idle.keep &&
                MovementAction::PassesAggroZGate(idle.mob, bot, stop.z))
                return idle.mob;

        return nullptr;
    }

    // The point `walked` yd along `points`.
    G3D::Vector3 PointAlong(Movement::PointsArray const& points, float walked)
    {
        for (size_t i = 1; i < points.size(); ++i)
        {
            G3D::Vector3 const segment = points[i] - points[i - 1];
            float const length = segment.length();
            if (walked <= length)
                return length > 0.0f ? points[i - 1] + segment * (walked / length) : points[i - 1];

            walked -= length;
        }

        return points.empty() ? G3D::Vector3() : points.back();
    }

    // [b35_brd2] A shorter stop on the bot's or the asked floor. BRD heroic 2026-10-26 (20261026-024524-brd-h-5G-5,
    // ~1936 s): members on the arena stands' middle tier (z -38.4) followed the tank on the top tier (618.7,-125.9,
    // -34.1); the navmesh walk ran over the floor below, and "would wake Anvilrage Medic -> alt shorter
    // (620.9,-125.7,-43.2)" dropped the healer and two dps to the z -46 floor, out of the tank's sight, for the 4
    // minutes of the stall. A stop (and every walk node before it) more than SHORTER_LEVEL_DZ below the lower or above
    // the higher of the bot's and the asked z is on another level: no such stop, the bot holds.
    constexpr float SHORTER_LEVEL_DZ = 3.0f;

    bool StopLeavesLevel(Movement::PointsArray const& points, float walked, float lowZ, float highZ, float& badZ)
    {
        G3D::Vector3 const stop = PointAlong(points, walked);
        badZ = stop.z;
        if (stop.z < lowZ - SHORTER_LEVEL_DZ || stop.z > highZ + SHORTER_LEVEL_DZ)
            return true;

        for (size_t i = 1; i < points.size(); ++i)
        {
            walked -= (points[i] - points[i - 1]).length();
            if (walked <= 0.0f)
                break;

            badZ = points[i].z;
            if (points[i].z < lowZ - SHORTER_LEVEL_DZ || points[i].z > highZ + SHORTER_LEVEL_DZ)
                return true;
        }

        return false;
    }

    float PathLength(Movement::PointsArray const& points)
    {
        float length = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
            length += (points[i] - points[i - 1]).length();

        return length;
    }

    // The walk the move would take: the navmesh path, or the straight line where the move goes straight.
    Movement::PointsArray GuardPath(Player* bot, float x, float y, float z, bool straight)
    {
        G3D::Vector3 const from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (!straight)
        {
            BotPath const path = FindBotPath(bot, x, y, z);  // [lead109]
            if (!(path.type & PATHFIND_NOPATH) && path.points.size() >= 2)
                return path.points;
        }

        return Movement::PointsArray{ from, G3D::Vector3(x, y, z) };
    }

    // One veto line per bot every GUARD_LOG_MS (`force`: this one anyway), with how many were not logged since the
    // last one. The checks the wake passed: range, z gate (dz of its allowance), los (seen, or unchecked).
    void LogGuardVeto(Player* bot, char const* kind, std::string const& action, float x, float y, float z,
                      PathWake const& wake, std::string const& outcome, bool force = false)
    {
        struct Throttle
        {
            uint32 lastMs = 0;
            uint32 skipped = 0;
        };

        thread_local std::unordered_map<ObjectGuid, Throttle> throttles;
        Throttle& throttle = throttles[bot->GetGUID()];
        uint32 const now = getMSTime();
        if (!force && throttle.lastMs && getMSTimeDiff(throttle.lastMs, now) < GUARD_LOG_MS)
        {
            ++throttle.skipped;
            return;
        }

        LOG_INFO("playerbots",
                 "[{}] {}: {} to ({:.1f},{:.1f},{:.1f}) would wake {} ({:.1f} yd of reach {:.1f}: range, z gate dz "
                 "{:.1f} of {:.1f}, los {}) -> {}{}",
                 kind, bot->GetName(), action, x, y, z, wake.mob->GetName(), wake.distance, wake.reach, wake.dz,
                 CREATURE_Z_ATTACK_RANGE + wake.mob->m_CombatDistance + wake.mob->GetObjectSize() +
                     bot->GetObjectSize(),
                 wake.losChecked ? "seen" : "unchecked", outcome,
                 throttle.skipped ? " (+" + std::to_string(throttle.skipped) + " not logged)" : std::string());
        throttle.lastMs = now ? now : 1;
        throttle.skipped = 0;
    }
}

void MovementAction::RecordTankTrail(Player* tank)
{
    if (tank && tank->IsInWorld() && tank->IsAlive() && tank->GetMap() && tank->GetMap()->IsDungeon() &&
        !tank->GetTransport() && !tank->HasUnitState(UNIT_STATE_IN_FLIGHT))
        RecordTrail(tank);
}

std::vector<G3D::Vector3> MovementAction::TankTrailPoints(Player* tank)
{
    return tank ? GetTrail(tank) : std::vector<G3D::Vector3>();
}

void MovementAction::CreateWp(Player* wpOwner, float x, float y, float z, float o, uint32 entry, bool important)
{
    float dist = wpOwner->GetDistance(x, y, z);
    float delay = IN_MILLISECONDS * dist / wpOwner->GetSpeed(MOVE_RUN) + sPlayerbotAIConfig.ReactDelay;

    // if (!important)
    // delay *= 0.25;

    Creature* wpCreature = wpOwner->SummonCreature(entry, x, y, z - 1, o, TEMPSUMMON_TIMED_DESPAWN, delay);
    botAI->GetBot()->AddAura(246, wpCreature);

    if (!important)
        wpCreature->SetObjectScale(0.5f);
}

bool MovementAction::JumpTo(uint32 mapId, float x, float y, float z, MovementPriority priority)
{
    UpdateMovementState();
    if (!IsMovingAllowed())
        return false;

    if (IsDuplicateMove(x, y, z))
        return false;

    if (IsWaitingForLastMove(priority))
        return false;

    // A jump goes straight and ignores collision: none through a closed door (TerrainHazard.h).
    G3D::Vector3 const jumpFrom(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    if (PathThroughClosedDoor(bot, Movement::PointsArray{ jumpFrom, G3D::Vector3(x, y, z) }).Found())
        return false;

    float speed = bot->GetSpeed(MOVE_RUN);
    MotionMaster& mm = *bot->GetMotionMaster();
    mm.Clear();
    mm.MoveJump(x, y, z, speed, speed, 1);
    AI_VALUE(LastMovement&, "last movement").Set(mapId, x, y, z, bot->GetOrientation(), 1000, priority);
    return true;
}

bool MovementAction::MoveNear(uint32 mapId, float x, float y, float z, float distance, MovementPriority priority)
{
    float angle = GetFollowAngle();
    return MoveTo(mapId, x + cos(angle) * distance, y + sin(angle) * distance, z, false, false, false, false, priority);
}

bool MovementAction::MoveNear(WorldObject* target, float distance, MovementPriority priority)
{
    if (!target)
        return false;

    distance += target->GetCombatReach();

    float followAngle = GetFollowAngle();

    for (float angle = followAngle; angle <= followAngle + static_cast<float>(2 * M_PI);
         angle += static_cast<float>(M_PI / 4.f))
    {
        float x = target->GetPositionX() + cos(angle) * distance;
        float y = target->GetPositionY() + sin(angle) * distance;
        float z = target->GetPositionZ();
        // Clamp Z to the terrain under the offset point so we don't
        // hand PointMovementGenerator a Z that matches the target's
        // floor but not the sampled (x,y) — avoids straight-line
        // fallbacks through geometry.
        bot->UpdateAllowedPositionZ(x, y, z);

        if (!bot->IsWithinLOS(x, y, z))
            continue;

        bool moved = MoveTo(target->GetMapId(), x, y, z, false, false, false, false, priority);
        if (moved)
            return true;
    }

    return false;
}

bool MovementAction::MoveToLOS(WorldObject* target, bool ranged)
{
    if (!target)
        return false;

    float x = target->GetPositionX();
    float y = target->GetPositionY();
    float z = target->GetPositionZ();

    // Use standard PathGenerator to find a route.
    BotPath const path = FindBotPath(bot, x, y, z);  // [lead109]
    PathType type = path.type;
    if (type != PATHFIND_NORMAL && type != PATHFIND_INCOMPLETE)
        return false;

    if (!ranged)
        return MoveTo((Unit*)target, target->GetCombatReach());

    float dist = FLT_MAX;
    PositionInfo dest;

    if (!path.points.empty())
    {
        for (auto& point : path.points)
        {
            if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
                CreateWp(bot, point.x, point.y, point.z, 0.0, 2334);

            float distPoint = target->GetDistance(point.x, point.y, point.z);
            if (distPoint < dist && target->IsWithinLOS(point.x, point.y, point.z + bot->GetCollisionHeight()))
            {
                dist = distPoint;
                dest.Set(point.x, point.y, point.z, target->GetMapId());

                if (ranged)
                    break;
            }
        }
    }

    if (dest.isSet())
        return MoveTo(dest.mapId, dest.x, dest.y, dest.z);
    else
        botAI->TellError("All paths not in LOS");

    return false;
}

// [b33_cos2] cos-h 20261026-000434-cos-h-5H-22 (00:14:56-00:15:47): the tank's "cos engage event target" walk to the
// wave spot at (2139.8,1356.3) was one 200 yd spline, started with the priest 23 yd away; the priest sat down to drink
// the same tick, then fought a Risen Zombie at its spot. "tank waits for sitters" (x0.00) only keeps the action from
// running again - the walk already issued went on - and the tank pulled the wave 153-240 yd ahead of the healer: 4
// deaths at 00:15:47-00:16:03. The same walk in 221216-cos-h-5D-23, 222618-cos-n-5G-67, 002026-cos-n-5D-68. A walk of
// its own is cut at the healer's range, so no walk already issued carries the tank off; at its edge the tank holds,
// out of it the tank walks back to the healer.
MovementAction::HealerLeashStep MovementAction::HealerLeash(float& x, float& y, float& z)
{
    constexpr float HEALER_LEASH = 40.0f;          // the healer's spell range
    constexpr float HEALER_LEASH_MIN_STEP = 5.0f;  // a cut walk shorter than this is a hold
    constexpr float HEALER_LEASH_SAMPLE = 2.0f;    // the ground path is sampled every this many yd
    constexpr uint32 HEALER_LEASH_LOG_MS = 10 * IN_MILLISECONDS;

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon() || !PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot) || bot->IsInCombat() ||
        bot->GetVehicle() || bot->GetTransport())
        return HealerLeashStep::Walk;

    Player* healer = FindGroupHealer(bot);
    if (!healer)
        return HealerLeashStep::Walk;

    float const healerToDest = healer->GetExactDist(x, y, z);
    float const healerToBot = healer->GetExactDist(bot);
    // [b34_cos3] A healer that follows the tank (moving, close, neither sitting nor casting) is where the cut point
    // lies a few seconds later: the leash reaches HEALER_LEASH_FOLLOW_EXTRA further, so the walk does not stop every
    // 20-30 yd for the healer to close up. cos-h 20261026-014521-cos-h-5B-21 waves 6-10 (01:59:41 -> 02:07:22): the
    // tank walked 1247 yd at 4.7 yd/s between ~90 "cut" walks with the paladin 5-28 yd behind; the leash-free clean
    // run 20261025-221216-cos-h-5D-23 walked 1116 yd at 5.9 yd/s, and reached Salramm 1:44 sooner. A sitting healer
    // keeps the plain range (the walk back to it as before); a healer that only stopped is not walked back to.
    constexpr float HEALER_LEASH_FOLLOW_EXTRA = 15.0f;
    constexpr float HEALER_LEASH_FOLLOW_CLOSE = 25.0f;
    bool const healerFollows = healer->isMoving() && !healer->IsSitState() &&
                               !healer->HasUnitState(UNIT_STATE_CASTING) && healerToBot <= HEALER_LEASH_FOLLOW_CLOSE;
    float const leash = healerFollows ? HEALER_LEASH + HEALER_LEASH_FOLLOW_EXTRA : HEALER_LEASH;
    float const backAt = healer->IsSitState() ? HEALER_LEASH : HEALER_LEASH + HEALER_LEASH_FOLLOW_EXTRA;
    if (healerToDest <= leash || healerToDest < healerToBot)
        return HealerLeashStep::Walk;

    // the farthest point of the ground path (from its start on) still within the leash of the healer
    G3D::Vector3 cut;
    bool found = false;
    if (healerToBot < leash)
    {
        BotPath const path = FindBotPath(bot, x, y, z);  // [lead109]
        std::vector<G3D::Vector3> const& points = path.points;
        bool out = false;
        for (size_t i = 1; i < points.size() && !out; ++i)
        {
            G3D::Vector3 const& from = points[i - 1];
            G3D::Vector3 const& to = points[i];
            uint32 const steps = std::max<uint32>(1, uint32(std::ceil((to - from).length() / HEALER_LEASH_SAMPLE)));
            for (uint32 step = 1; step <= steps; ++step)
            {
                G3D::Vector3 const point = from + (to - from) * (float(step) / float(steps));
                if (healer->GetExactDist(point.x, point.y, point.z) > leash)  // [b34_cos3]
                {
                    out = true;
                    break;
                }
                cut = point;
                found = true;
            }
        }
    }

    bool const back = healerToBot > backAt;  // [b34_cos3]
    bool const hold = !found || bot->GetExactDist(cut.x, cut.y, cut.z) < HEALER_LEASH_MIN_STEP;
    char const* const state = back ? "back" : hold ? "hold" : "cut";
    thread_local std::unordered_map<uint64, uint32> leashLogMs;
    uint32 const now = getMSTime();
    uint32& at = leashLogMs[bot->GetGUID().GetRawValue()];
    if (!at || getMSTimeDiff(at, now) >= HEALER_LEASH_LOG_MS)
    {
        at = now ? now : 1;
        LOG_INFO("playerbots",
                 "[b33_cos2] [healer leash] {}: {} {} to ({:.1f},{:.1f},{:.1f}), {:.0f} yd from the healer {} "
                 "({:.0f} yd from the tank){}",
                 bot->GetName(), state, getName(), x, y, z, healerToDest, healer->GetName(), healerToBot,
                 back   ? std::string(": walks back to the healer")
                 : hold ? std::string()
                        : Acore::StringFormat(": walks to ({:.1f},{:.1f},{:.1f})", cut.x, cut.y, cut.z));
        BotMoveEvents::Event event;
        event.type = "healer_leash";
        event.Str("state", state)
            .Str("by", getName())
            .Str("healer", healer->GetName())
            .Real("healerToDest", healerToDest)
            .Real("healerToTank", healerToBot)
            .Real("leash", leash)  // [b34_cos3]
            .Real("x", bot->GetPositionX())
            .Real("y", bot->GetPositionY())
            .Real("z", bot->GetPositionZ())
            .Real("toX", x)
            .Real("toY", y)
            .Real("toZ", z);
        BotMoveEvents::Push(bot->GetGUID(), std::move(event));
    }

    if (back)
    {
        x = healer->GetPositionX();
        y = healer->GetPositionY();
        z = healer->GetPositionZ();
        return HealerLeashStep::Back;
    }
    if (hold)
        return HealerLeashStep::Hold;

    x = cut.x;
    y = cut.y;
    z = cut.z;
    return HealerLeashStep::Walk;
}

namespace
{
    // [b43_move] Out of combat, a follow whose spot lies in a persistent harmful ground effect stops just outside it.
    // scholo-h 20261026-050341-scholo-h-5B-3 (~1540 s, [b38_scholo] note): "avoid aoe" took Rtrogab / Rtpriaa out of
    // a Diseased Ghoul's On Death Cloud of Disease 17742 (persistent area aura, 5 yd, 20 s); "follow" walked them
    // straight back to their formation spot in it, and move_conflict logged the flips. The effect is read live (its
    // DynamicObject: an area spell, harmful, of a hostile caster or one gone, within MaxAoeAvoidRadius, more than
    // FOLLOW_EFFECT_MIN_LEFT_MS left), not from what the bot noted, so it holds for as long as the cloud lasts.
    constexpr float FOLLOW_EFFECT_EDGE = 2.0f;     // the spot stands this far outside the effect
    constexpr float FOLLOW_EFFECT_FLOOR = 3.0f;    // a spot on another floor than the asked one is none
    constexpr float FOLLOW_EFFECT_SCAN_MAX = 50.0f;
    constexpr int32 FOLLOW_EFFECT_MIN_LEFT_MS = 1500;
    constexpr uint32 FOLLOW_EFFECT_ANGLES = 16;
    constexpr float FOLLOW_EFFECT_NO_MOVE = 2.0f;  // the bot this near the outside spot stays where it is
    constexpr uint32 FOLLOW_EFFECT_LOG_MS = 5 * IN_MILLISECONDS;

    struct GroundEffect
    {
        float x;
        float y;
        float z;
        float radius;
        uint32 spellId;
    };

    std::vector<GroundEffect> HarmfulGroundEffects(Player* bot, float range)
    {
        std::vector<GroundEffect> effects;
        std::vector<WorldObject*> objects;
        Acore::AllWorldObjectsInRange check(bot, range);
        Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(bot, objects, check,
                                                                              GRID_MAP_TYPE_MASK_DYNAMICOBJECT);
        Cell::VisitObjects(bot, searcher, range);
        for (WorldObject* object : objects)
        {
            DynamicObject* dyn = object ? object->ToDynObject() : nullptr;
            if (!dyn || !dyn->IsInWorld() || dyn->GetByteValue(DYNAMICOBJECT_BYTES, 0) != DYNAMIC_OBJECT_AREA_SPELL)
                continue;

            int32 const left = dyn->GetDuration();
            float const radius = dyn->GetRadius();
            SpellInfo const* info = sSpellMgr->GetSpellInfo(dyn->GetSpellId());
            if ((left >= 0 && left < FOLLOW_EFFECT_MIN_LEFT_MS) || radius <= 0.0f ||
                radius > sPlayerbotAIConfig.MaxAoeAvoidRadius || !info || info->IsPositive() ||
                sPlayerbotAIConfig.AoeAvoidSpellWhitelist.find(info->Id) !=
                    sPlayerbotAIConfig.AoeAvoidSpellWhitelist.end())
                continue;

            Unit* caster = dyn->GetCaster();
            if (caster ? !caster->IsHostileTo(bot) : !dyn->GetCasterGUID().IsCreature())
                continue;

            effects.push_back({ dyn->GetPositionX(), dyn->GetPositionY(), dyn->GetPositionZ(), radius, info->Id });
        }

        return effects;
    }

    GroundEffect const* GroundEffectAt(std::vector<GroundEffect> const& effects, Player* bot, float x, float y,
                                       float z, float edge)
    {
        for (GroundEffect const& effect : effects)
            if (std::hypot(effect.x - x, effect.y - y) < effect.radius + bot->GetCombatReach() + edge &&
                std::fabs(effect.z - z) <= FOLLOW_EFFECT_FLOOR + effect.radius)
                return &effect;

        return nullptr;
    }

    // true: walk to (x, y, z), moved out of the effect when it lay in one; false: no move (the bot already stands
    // just outside, or no spot outside it on that floor).
    bool FollowSpotOutOfGroundEffect(Player* bot, std::string const& name, float& x, float& y, float& z)
    {
        float const walk = bot->GetExactDist2d(x, y);
        if (walk > FOLLOW_EFFECT_SCAN_MAX)
            return true;

        std::vector<GroundEffect> const effects =
            HarmfulGroundEffects(bot, std::min(FOLLOW_EFFECT_SCAN_MAX, walk + sPlayerbotAIConfig.MaxAoeAvoidRadius));
        GroundEffect const* covering = effects.empty() ? nullptr : GroundEffectAt(effects, bot, x, y, z, 0.0f);
        if (!covering)
            return true;

        GroundEffect const effect = *covering;
        float const ring = effect.radius + bot->GetCombatReach() + FOLLOW_EFFECT_EDGE;
        float const asked = std::atan2(y - effect.y, x - effect.x);
        float const fromBot = std::atan2(bot->GetPositionY() - effect.y, bot->GetPositionX() - effect.x);
        // the asked spot's side first (it is as near the follow target as the effect allows); a spot right on the
        // centre takes the bot's side
        float const first = std::hypot(x - effect.x, y - effect.y) > 0.5f ? asked : fromBot;
        Map* map = bot->GetMap();
        bool found = false;
        float bestX = 0.0f;
        float bestY = 0.0f;
        float bestZ = 0.0f;
        for (uint32 i = 0; i < FOLLOW_EFFECT_ANGLES && !found; ++i)
        {
            // 0, +1, -1, +2, -2 ... steps off the first side
            int32 const k = (i % 2) ? static_cast<int32>((i + 1) / 2) : -static_cast<int32>(i / 2);
            float const angle = first + static_cast<float>(k) * 2.0f * static_cast<float>(M_PI) / FOLLOW_EFFECT_ANGLES;
            float sx = effect.x + std::cos(angle) * ring;
            float sy = effect.y + std::sin(angle) * ring;
            float sz = z;
            bot->UpdateAllowedPositionZ(sx, sy, sz);
            if (std::fabs(sz - z) > FOLLOW_EFFECT_FLOOR || GroundEffectAt(effects, bot, sx, sy, sz, 0.5f) ||
                !map->isInLineOfSight(x, y, z + bot->GetCollisionHeight(), sx, sy, sz + bot->GetCollisionHeight(),
                                      bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                continue;

            found = true;
            bestX = sx;
            bestY = sy;
            bestZ = sz;
        }

        thread_local std::unordered_map<ObjectGuid, uint32> logged;
        uint32 const now = getMSTime();
        uint32& last = logged[bot->GetGUID()];
        bool const logDue = !last || getMSTimeDiff(last, now) >= FOLLOW_EFFECT_LOG_MS;
        if (logDue)
            last = now ? now : 1;

        bool const there = found && bot->GetExactDist2d(bestX, bestY) <= FOLLOW_EFFECT_NO_MOVE;
        if (logDue)
            LOG_INFO("playerbots", "[b43_move] follow_out_of_effect: {}: {} to ({:.1f},{:.1f},{:.1f}) is in ground "
                     "effect {} at ({:.1f},{:.1f}) r {:.1f}: {}", bot->GetName(), name, x, y, z, effect.spellId,
                     effect.x, effect.y, effect.radius,
                     !found  ? "no spot outside on that floor, stays"
                     : there ? "stays just outside"
                             : Acore::StringFormat("outside at ({:.1f},{:.1f},{:.1f})", bestX, bestY, bestZ));
        if (!found || there)
            return false;

        x = bestX;
        y = bestY;
        z = bestZ;
        return true;
    }
}

thread_local bool MovementAction::s_dryRun = false;
thread_local std::string MovementAction::s_moveRefusal;
thread_local std::string MovementAction::s_moveSubstitute;
thread_local ObjectGuid MovementAction::s_moveMob;

std::string const& LastMoveRefusal() { return MovementAction::s_moveRefusal; }
std::string const& LastMoveSubstitute() { return MovementAction::s_moveSubstitute; }
ObjectGuid LastMoveMob() { return MovementAction::s_moveMob; }

bool MovementAction::MoveTo(uint32 mapId, float x, float y, float z, bool /*idle*/, bool /*react*/, bool normal_only,
                            bool exact_waypoint, MovementPriority priority, bool lessDelay, bool backwards)
{
    bool const dry = s_dryRun;  // [lead70]
    s_moveRefusal.clear();
    s_moveSubstitute.clear();  // [lead372]
    s_moveMob.Clear();
    float const origX = x;  // [lead238] the destination as asked, before a guard picks another
    float const origY = y;
    float const origZ = z;
    auto const refuse = [](char const* guard)
    {
        s_moveRefusal = guard;
        return false;
    };
    // [lead161] b178 17:19:41 the worldserver died right after "leader move order to (nan,0.0,22.5)" (Rtdekab, rfc-h
    // 5D): a non-finite destination never reaches the movement generator
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
    {
        LOG_ERROR("playerbots", "[lead161] non_finite_move: {}: {} to ({},{},{}) refused", bot->GetName(), getName(), x,
                  y, z);
        return refuse("a non-finite destination");
    }
    if (!dry)
        UpdateMovementState();
    if (!IsMovingAllowed())
    {
        return refuse("IsMovingAllowed (a hold, root, cast or dead state)");
    }
    // [b35_z0] A destination z that is the map grid's fallback (0.0 on a WMO-only instance tile, from
    // UpdateAllowedPositionZ or a path end's NormalizePath when the vmap ray found no floor) far from the bot is no
    // floor: it snaps to the navmesh at (x, y) near the bot's own height (SnapGridFallbackZ, TerrainHazard.h), so the
    // duplicate check and the guards below see the floor the caller meant. Cheap: one compare within 5 yd.
    if (mapId == bot->GetMapId())
        SnapGridFallbackZ(bot, x, y, z, getName().c_str());
    if (!dry && (IsDuplicateMove(x, y, z) || IsDuplicateMove(origX, origY, origZ)))
    {
        return refuse("IsDuplicateMove (the same walk is on)");
    }
    if (!dry && IsWaitingForLastMove(priority))
    {
        return refuse("IsWaitingForLastMove (the last walk is not done)");
    }

    // [waypoint_lane] Out of a fight, a dungeon strategy's walk keeps to the harness waypoint's lane; [move_conflict]
    // every order that stands is noted for the back-and-forth detector (MoveConflict.h).
    if (mapId == bot->GetMapId())
    {
        // [b43_move] out of combat, a dungeon follow to a spot in a harmful ground effect stops just outside it (no
        // "avoid aoe" / "follow" flips); before NoteOrder, so the conflict detector sees the order that stands
        if (!bot->IsInCombat() && bot->GetGroup() && bot->GetMap() && bot->GetMap()->IsDungeon() &&
            (_guardedMove == GuardedMove::FOLLOW || getName() == "follow"))
        {
            float const askedX = x;
            float const askedY = y;
            if (!FollowSpotOutOfGroundEffect(bot, getName(), x, y, z) ||
                ((askedX != x || askedY != y) && IsDuplicateMove(x, y, z)))
                return refuse("FollowSpotOutOfGroundEffect");
        }

        if (MoveConflict::LaneApplies(bot, getName()) &&
            MoveConflict::VetoesOffWaypoint(bot, getName(), x, y, z, AI_VALUE(bool, "encounter in progress")))
            return refuse("MoveConflict::VetoesOffWaypoint (the waypoint's lane)");
        if (!dry)
            MoveConflict::NoteOrder(bot, getName(), x, y, z);
    }

    // A dungeon tank leaves its trail for the members that follow it; a member walks no path that wakes an idle mob.
    if (Map* map = bot->GetMap(); map && map->IsDungeon() && mapId == bot->GetMapId() && bot->GetGroup())
    {
        if (PlayerbotAI::IsTank(bot) && !dry)
            RecordTrail(bot);

        // The walk is judged as it is walked: an exact waypoint on the ground still goes by the navmesh (DoMovePoint
        // generatePath), only a flyer or a swimmer goes straight. RFC 2026-10-24: the formation follow (exact) was
        // judged as a straight line from the boss room down through the rock, under an Earthborer's ledge.
        // [lead9] a harness group's fight: the destination ends inside the leader's safe zone (SafeZone.h), else the
        // bot stays and takes the hit. The zone is the radius authority there in place of the fight hold below.
        Leader::SafeZone zone;
        bool zoned = false;
        float const askedX = x;
        float const askedY = y;
        if (!ClampToFightZone(x, y, z, zone, zoned))
            return refuse("ClampToFightZone (the leader's safe zone)");

        if (zoned && (x != askedX || y != askedY))
            s_moveSubstitute = "ClampToFightZone (the leader's safe zone)";  // [lead372]

        // [drift_fight_hold] a trash fight's chase stays at the tank's fight spot (before the path guard: the walk
        // it judges is the shortened one)
        float const heldX = x;
        float const heldY = y;
        if (!zoned && bot->IsInCombat() && !HoldFightSpot(x, y, z))
            return refuse("HoldFightSpot (the tank's fight spot)");
        if (x != heldX || y != heldY)
            s_moveSubstitute = "HoldFightSpot (the tank's fight spot's edge)";
        float const zonedX = x;
        float const zonedY = y;
        float const zonedZ = z;

        // [hazard_melee_spot] a melee walk into a ground effect the bot just left: a spot round the target outside
        // it, or stand in a light one, or wait at its edge (no out-in-out-in)
        if (bot->IsInCombat() && !HazardMeleeSpot(x, y, z))
            return refuse("HazardMeleeSpot (a ground effect)");
        if (x != zonedX || y != zonedY)
            s_moveSubstitute = "HazardMeleeSpot (a spot out of a ground effect)";

        if (IsPathGuarded() && !GuardIdleMobPath(x, y, z, bot->IsFlying() || bot->isSwimming()))
            return refuse("GuardIdleMobPath (the walk wakes an idle mob: see the [follow path] / [combat path] lines)");

        // [lead9] a guard above picked another destination (a melee spot out of a ground effect, a ring spot, a trail
        // leg, a spot by the tank): it stays inside the zone too, or the bot stays
        if (zoned && (x != zonedX || y != zonedY || z != zonedZ))
        {
            Leader::ZoneRole const role = Leader::FightZone::RoleOf(bot);
            Position const here = bot->GetPosition();
            Position const alt(x, y, z);
            std::string why;
            if (!zone.Contains(alt, role, &here, &why) || !zone.Standable(bot, alt, &why) ||
                !zone.PathInside(bot, alt, role, &why))
            {
                Leader::FightZone::NoteClamp(bot, getName(), alt, nullptr, "a guard's other spot: " + why);
                return refuse("the leader's safe zone (a guard's other spot)");
            }
        }
    }

    // [lead93] a led bot never walks into the instance's exit areatrigger (it would port the bot out mid-run)
    // [lead238] b189 rfc-h 5A: Rtshaab's "combat spot: behind the target - InInstanceExit" x2: the leader's spot was out
    // of the exit (its spot check tests it), a guard above (the fight hold, the safe zone, a melee spot out of a ground
    // effect) moved the walk's end into it. Such a moved end goes back to the spot as asked
    if (Map* exitMap = bot->GetMap(); exitMap && exitMap->IsDungeon() && mapId == bot->GetMapId() &&
                                      Leader::LedBot(bot) &&
                                      InInstanceExit(mapId, x, y, z, bot->GetCombatReach()))
    {
        if ((x == origX && y == origY && z == origZ) ||
            InInstanceExit(mapId, origX, origY, origZ, bot->GetCombatReach()))
            return refuse("InInstanceExit (the instance's exit)");
        x = origX;
        y = origY;
        z = origZ;
    }

    // No walk through a closed door, grouped or not, tank included (the forge walls of flame, Utgarde Keep).
    if (Map* map = bot->GetMap(); map && map->IsDungeon() && mapId == bot->GetMapId() && !bot->GetVehicle() &&
                                  !bot->GetTransport() && !bot->IsFlying() && !GuardClosedDoor(x, y, z))
        return refuse("GuardClosedDoor (see the [closed door] lines)");

    // [ledge_drop] plan29 follow-up 3: no walk off a ledge (nor the straight line PointMovementGenerator takes when
    // the navmesh has no path) to a spot below unless the drop is a justified shortcut (JudgeLedgeDrop: the landing
    // joined to the rest, a fall the group takes). dtk-n 20261025-163758-62: the tank's walk to a Death Knight 18-34
    // yd below ended on a navmesh island, the mage followed, the run stalled.
    // [b35_z0] a guard above (HoldFightSpot's edge, HazardMeleeSpot's spot) may have re-derived z the same way
    if (mapId == bot->GetMapId())
        SnapGridFallbackZ(bot, x, y, z, getName().c_str());
    if (mapId == bot->GetMapId() && !backwards && !bot->isSwimming() && bot->GetPositionZ() - z > LEDGE_DROP_DZ)
    {
        LedgeDropVerdict const ledge = JudgeLedgeDrop(bot, x, y, z, getName().c_str());
        if (!ledge.allowed)
            return refuse("JudgeLedgeDrop (see the [ledge drop] lines)");
        // [lead77] b120 rfc-h 470-560 s: the led tank dropped 6.8 yd onto an island the group could not follow and
        // walked on alone - the group moves as one: a led tank takes no ledge drop (its members refuse it too)
        // [lead81] b124 rfc-h 112-182 s: the tank stood at (-90,-40,-24) for 70 s - every walk to a mob more than
        // LEDGE_DROP_DZ below was refused although JudgeLedgeDrop found no drop on it (RFC's slope down): only a drop
        // [lead141] b165 rfc-h 60-138 s: every engage walk refused on the route's 6 yd step into the water by the
        // first cave (landing joined to the rest, a safe fall) - the group follows a drop with a way back; an
        // island (no ground path back) stays refused
        // [lead860] b224 rfc-h 5F 524.7-811 s: every engage and route walk of Rtwarac was refused here - the tank stood
        // on a 5-polygon rock top at (-122.1,43.2,-14.4), 3.5 yd over the floor (its 5108-polygon part at
        // (-118.5,39.0,-17.4), where the rogue and the hunter stood), its sides steep: no ground path led off it, so
        // every landing on the floor read "an island" (no way back up to the rock) though the island was the tank's
        // own spot. A tank on a navmesh island takes the step down to the floor beside it (SelfTravel::
        // IslandStepDown: a larger part within reach, no fall damage, in sight) - the group's floor, the members
        // there and the route's way on - and walks on from there
        if (ledge.drop && !ledge.connected && PlayerbotAI::IsTank(bot) && Leader::Orders::IsTracked(bot->GetGUID()))
        {
            G3D::Vector3 step;
            if (ledge.groundPath || !SelfTravel::IslandStepDown(bot, step))
                return refuse("a led tank's ledge drop onto an island");
            s_moveSubstitute = "the step down off the tank's navmesh island ([lead860])";
            if (dry)
                return true;
            LOG_INFO("playerbots",
                     "[lead860] island_step_down: {}: {} to ({:.1f},{:.1f},{:.1f}) from its navmesh island: the step "
                     "down to ({:.1f},{:.1f},{:.1f}) first",
                     bot->GetName(), getName(), x, y, z, step.x, step.y, step.z);
            if (!JumpTo(mapId, step.x, step.y, step.z, priority))
                return refuse("the step down off the tank's navmesh island (the jump's own guards)");
            return true;
        }

        // [b33_sp] an allowed water drop no ground path carries: from the brink, a jump into the water, together
        // (JudgeWaterDrop)
        G3D::Vector3 jump;
        std::string why;
        WaterDrop const drop = JudgeWaterDrop(bot, ledge, x, y, z, jump, why);
        if (drop == WaterDrop::Under)
            return true;
        if (drop == WaterDrop::Jump || drop == WaterDrop::Hold || !why.empty())
        {
            thread_local std::unordered_map<uint64, uint32> waterDropLogMs;
            uint32 const now = getMSTime();
            uint32& at = waterDropLogMs[bot->GetGUID().GetRawValue()];
            if (drop == WaterDrop::Jump || !at || getMSTimeDiff(at, now) >= WATER_DROP_LOG_MS)
            {
                at = now ? now : 1;
                char const* const state = drop == WaterDrop::Jump ? "jump" : drop == WaterDrop::Hold ? "hold" : "walk";
                G3D::Vector3 const into =
                    drop == WaterDrop::Jump ? jump : G3D::Vector3(ledge.landX, ledge.landY, ledge.landZ);
                LOG_INFO("playerbots",
                         "[b33_sp] [water drop] {}: {} {} to ({:.1f},{:.1f},{:.1f}): drop {:.1f} yd {} at "
                         "({:.1f},{:.1f},{:.1f}){}{}",
                         bot->GetName(), state, getName(), x, y, z, ledge.height,
                         ledge.water ? "into water" : "onto the floor below [b35_brd2]", into.x, into.y, into.z,
                         why.empty() ? "" : ": ", why);
                BotMoveEvents::Event event;
                event.type = "water_drop";
                event.Str("state", state)
                    .Str("kind", ledge.water ? "water" : "dry")  // [b35_brd2]
                    .Str("by", getName())
                    .Str("why", why)
                    .Real("height", ledge.height)
                    .Real("x", bot->GetPositionX())
                    .Real("y", bot->GetPositionY())
                    .Real("z", bot->GetPositionZ())
                    .Real("toX", x)
                    .Real("toY", y)
                    .Real("toZ", z)
                    .Real("jumpX", into.x)
                    .Real("jumpY", into.y)
                    .Real("jumpZ", into.z);
                BotMoveEvents::Push(bot->GetGUID(), std::move(event));
            }
        }
        if (drop == WaterDrop::Hold)
            return refuse("JudgeWaterDrop (hold)");
        if (drop == WaterDrop::Jump)
            return dry || JumpTo(mapId, jump.x, jump.y, jump.z, priority);
    }

    if (!backwards && SwimsTo(bot, x, y, z))
        return dry || SwimStraight(mapId, x, y, z, priority, lessDelay);

    bool generatePath = !bot->IsFlying() && !bot->isSwimming();
    // A swimmer heading for a spot out of the water (a shore, a ledge, the surface) follows the navmesh too: a
    // straight spline crosses the whole lake (Underbog: through the Underbog Frenzies) where the path keeps to land
    // (NAV_WATER costs more). PointMovementGenerator goes straight itself when there is no path. Dives (a
    // destination under water) and exact moves (surfacing for air) stay straight.
    bool swimPath = false;
    if (!bot->IsFlying() && bot->isSwimming() && !exact_waypoint && !bot->GetVehicle() && bot->GetMap() &&
        !bot->GetMap()->IsUnderWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
    {
        generatePath = true;
        swimPath = true;
    }
    bool disableMoveSplinePath =
        sPlayerbotAIConfig.DisableMoveSplinePath >= 2 ||
        (sPlayerbotAIConfig.DisableMoveSplinePath == 1 && bot->InBattleground());
    if (Vehicle* vehicle = bot->GetVehicle())
    {
        VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
        Unit* vehicleBase = vehicle->GetBase();
        generatePath = !vehicleBase || !vehicleBase->CanFly();
        if (!vehicleBase || !seat || !seat->CanControl())  // is passenger and cant move anyway
            return refuse("a vehicle seat that cannot steer");

        float distance = vehicleBase->GetExactDist(x, y, z);  // use vehicle distance, not bot
        if (distance > 0.01f)
        {
            if (dry)
                return true;
            DoMovePoint(vehicleBase, x, y, z, generatePath, backwards);
            float speed = backwards ? vehicleBase->GetSpeed(MOVE_RUN_BACK) : vehicleBase->GetSpeed(MOVE_RUN);
            float delay = IN_MILLISECONDS * (distance / speed);
            if (lessDelay)
            {
                delay -= botAI->GetReactDelay();
            }
            delay = std::max(.0f, delay);
            delay = std::min((float)sPlayerbotAIConfig.MaxWaitForMove, delay);
            AI_VALUE(LastMovement&, "last movement").Set(mapId, x, y, z, bot->GetOrientation(), delay, priority);
            AI_VALUE(LastMovement&, "last movement").lastMoveAsked = WorldPosition(mapId, origX, origY, origZ);
            return true;
        }
    }
    else if (exact_waypoint || disableMoveSplinePath || !generatePath || swimPath)
    {
        float distance = bot->GetExactDist(x, y, z);
        if (distance > 0.01f)
        {
            if (dry)
                return true;
            if (bot->IsSitState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);

            // bot->CastStop();

            DoMovePoint(bot, x, y, z, generatePath, backwards);
            float delay = IN_MILLISECONDS * MoveDelay(distance, backwards);
            if (lessDelay)
            {
                delay -= botAI->GetReactDelay();
            }
            delay = std::max(.0f, delay);
            delay = std::min((float)sPlayerbotAIConfig.MaxWaitForMove, delay);
            AI_VALUE(LastMovement&, "last movement").Set(mapId, x, y, z, bot->GetOrientation(), delay, priority);
            AI_VALUE(LastMovement&, "last movement").lastMoveAsked = WorldPosition(mapId, origX, origY, origZ);
            return true;
        }
    }
    else
    {
        float modifiedZ;
        Movement::PointsArray path =
            SearchForBestPath(x, y, z, modifiedZ, sPlayerbotAIConfig.MaxMovementSearchTime, normal_only);
        if (modifiedZ == INVALID_HEIGHT)
            return refuse(path.empty() && UsesLeaderRoute(bot) ? "no route (the leader's route search)"
                                                               : "SearchForBestPath (no floor at the destination)");
        float distance = bot->GetExactDist(x, y, modifiedZ);
        // [lead1050] b231 rfc-h 5D-12 453-783 s: a Blink put the mage on a 55-polygon rock 1.1 yd over the floor; every
        // walk to the group (Follow, combat spot) was the route's partial end - the rock's point nearest the goal,
        // where it stood - a 0 yd spline executed every tick, the group waited 330 s and the run stalled. A walk whose
        // route ends where the member stands, short of the goal, goes nowhere: the member steps off its navmesh part
        // to the one the route from reaches the goal (SelfTravel::PartStepToward), or stays and reports
        if (Leader::LedBot(bot) && UsesLeaderRoute(bot) && !path.empty() && distance > Leader::LEADER_SPOT_TOLERANCE &&
            bot->GetExactDist(path.back().x, path.back().y, path.back().z) <= Leader::LEADER_SPOT_TOLERANCE &&
            !Leader::Route(bot, Position(x, y, modifiedZ)).complete)
        {
            G3D::Vector3 landing;
            if (!SelfTravel::PartStepToward(bot, G3D::Vector3(x, y, modifiedZ), landing))
                return refuse("PartStepToward (its navmesh part does not reach the destination, no step off it)");
            if (Leader::Fight::LeaderAggroAt(bot, Position(landing.x, landing.y, landing.z)))
                return refuse("PartStepToward (the step lands in an idle mob's reach)");
            if (dry)
                return true;
            if (bot->IsSitState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);
            if (!JumpTo(mapId, landing.x, landing.y, landing.z, priority))
                return refuse("PartStepToward (the step's own guards)");
            // the walk under way is the walk to the destination (the step its first leg): no re-ask mid-step, and
            // once landed the walk there goes on (lead1044 reads a walk that ended at its end as reached)
            LastMovement& last = AI_VALUE(LastMovement&, "last movement");
            last.Set(mapId, x, y, modifiedZ, bot->GetOrientation(), last.lastdelayTime, priority);
            last.lastMoveAsked = WorldPosition(mapId, origX, origY, origZ);
            return true;
        }
        if (distance > 0.01f)
        {
            if (dry)
                return true;
            if (bot->IsSitState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);

            // bot->CastStop();

            // [lead109] the route SearchForBestPath took is the walk (no second search)
            if (backwards || !UsesLeaderRoute(bot) || !WalkBotPoints(bot, path))
                DoMovePoint(bot, x, y, modifiedZ, generatePath, backwards);
            float delay = IN_MILLISECONDS * MoveDelay(distance, backwards);
            if (lessDelay)
            {
                delay -= botAI->GetReactDelay();
            }
            delay = std::max(.0f, delay);
            delay = std::min((float)sPlayerbotAIConfig.MaxWaitForMove, delay);
            AI_VALUE(LastMovement&, "last movement")
                .Set(mapId, x, y, modifiedZ, bot->GetOrientation(), delay, priority);
            // [lead280] the duplicate check knows this walk by the end asked too: the floor search's z (modifiedZ) or
            // a guard's moved end made every repeat of the leader's same spot a new walk, issued every tick (b193 /
            // b195: 2400 s of combat-spot walks re-issued, the rotation and the Attack starved behind them)
            AI_VALUE(LastMovement&, "last movement").lastMoveAsked = WorldPosition(mapId, origX, origY, origZ);
            return true;
        }
    }

    return refuse("the walk ends where the bot stands");
    //
    // // LOG_DEBUG("playerbots", "IsMovingAllowed {}", IsMovingAllowed());
    // bot->AddUnitMovementFlag()

    // bool isVehicle = false;
    // Unit* mover = bot;
    // if (Vehicle* vehicle = bot->GetVehicle())
    // {
    //     VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
    //     LOG_DEBUG("playerbots", "!seat || !seat->CanControl() {}", !seat || !seat->CanControl());
    //     if (!seat || !seat->CanControl())
    //         return false;

    //     isVehicle = true;
    //     mover = vehicle->GetBase();
    // }

    // bool detailedMove = botAI->AllowActivity(DETAILED_MOVE_ACTIVITY);
    // if (!detailedMove)
    // {
    //     time_t now = time(nullptr);
    //     if (AI_VALUE(LastMovement&, "last movement").nextTeleport > now) // We can not teleport yet. Wait.
    //     {
    //         LOG_DEBUG("playerbots", "AI_VALUE(LastMovement&, \"last movement\").nextTeleport > now");
    //         botAI->SetNextCheckDelay(
    //             (AI_VALUE(LastMovement&, "last movement").nextTeleport - now) * IN_MILLISECONDS);
    //         return true;
    //     }
    // }

    // float minDist = sPlayerbotAIConfig.TargetPosRecalcDistance; //Minium distance a bot should move.
    // float maxDist = sPlayerbotAIConfig.ReactDistance;           //Maxium distance a bot can move in one single
    // action. float originalZ = z;                                        // save original destination height to check
    // if bot needs to fly up

    // bool generatePath = !bot->IsFlying() && !bot->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING) && !bot->IsInWater() &&
    // !bot->IsUnderWater(); if (generatePath)
    // {
    //     z += CONTACT_DISTANCE;
    //     mover->UpdateAllowedPositionZ(x, y, z);
    // }

    // if (!isVehicle && !IsMovingAllowed() && bot->isDead())
    // {
    //     bot->StopMoving();
    //     LOG_DEBUG("playerbots", "!isVehicle && !IsMovingAllowed() && bot->isDead()");
    //     return false;
    // }

    // if (!isVehicle && bot->isMoving() && !IsMovingAllowed())
    // {
    //     if (!bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
    //         bot->StopMoving();
    //     LOG_DEBUG("playerbots", "!isVehicle && bot->isMoving() && !IsMovingAllowed()");
    //     return false;
    // }

    // LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");

    // WorldPosition startPosition = WorldPosition(bot);             //Current location of the bot
    // WorldPosition endPosition = WorldPosition(mapId, x, y, z, 0); //The requested end location
    // WorldPosition movePosition;                                   //The actual end location

    // float totalDistance = startPosition.distance(endPosition);    //Total distance to where we want to go
    // float maxDistChange = totalDistance * 0.1;                    //Maximum change between previous destination
    // before needing a recalulation

    // if (totalDistance < minDist)
    // {
    //     if (lastMove.lastMoveShort.distance(endPosition) < maxDistChange)
    //         AI_VALUE(LastMovement&, "last movement").clear();

    //     mover->StopMoving();
    //     LOG_DEBUG("playerbots", "totalDistance < minDist");
    //     return false;
    // }

    // TravelPath movePath;

    // if (lastMove.lastMoveShort.distance(endPosition) < maxDistChange &&
    // startPosition.distance(lastMove.lastMoveShort) < maxDist) //The last short movement was to the same place we want
    // to move now.
    //     movePosition = endPosition;
    // else if (!lastMove.lastPath.empty() && lastMove.lastPath.getBack().distance(endPosition) < maxDistChange) //The
    // last long movement was to the same place we want to move now.
    // {
    //     movePath = lastMove.lastPath;
    // }
    // else
    // {
    //     movePosition = endPosition;

    //     if (startPosition.GetMapId() != endPosition.GetMapId() || totalDistance > maxDist)
    //     {
    //         if (!TravelNodeMap::instance().getNodes().empty() && !bot->InBattleground())
    //         {
    //             if (sPlayerbotAIConfig.TweakValue)
    //             {
    //                 if (lastMove.future.valid())
    //                 {
    //                     movePath = lastMove.future.get();
    //                 }
    //                 else
    //                 {
    //                     lastMove.future = std::async(&TravelNodeMap::getFullPath, startPosition, endPosition, bot);
    //                     LOG_DEBUG("playerbots", "lastMove.future = std::async(&TravelNodeMap::getFullPath,
    //                     startPosition, endPosition, bot);"); return true;
    //                 }
    //             }
    //             else
    //                 movePath = TravelNodeMap::instance().getFullPath(startPosition, endPosition, bot);

    //             if (movePath.empty())
    //             {
    //                 //We have no path. Beyond 450yd the standard PathGenerator will probably move the wrong way.
    //                 if (ServerFacade::instance().IsDistanceGreaterThan(totalDistance, maxDist * 3))
    //                 {
    //                     movePath.clear();
    //                     movePath.addPoint(endPosition);
    //                     AI_VALUE(LastMovement&, "last movement").setPath(movePath);

    //                     bot->StopMoving();
    //                     if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
    //                         botAI->TellMasterNoFacing("I have no path");
    //                     LOG_DEBUG("playerbots", "ServerFacade::instance().IsDistanceGreaterThan(totalDistance, maxDist * 3)");
    //                     return false;
    //                 }

    //                 movePosition = endPosition;
    //             }
    //         }
    //         else
    //         {
    //             //Use standard PathGenerator to find a route.
    //             movePosition = endPosition;
    //         }
    //     }
    // }

    // if (movePath.empty() && movePosition.distance(startPosition) > maxDist)
    // {
    //     //Use standard PathGenerator to find a route.
    //     PathGenerator path(mover);
    //     path.CalculatePath(movePosition.GetPositionX(), movePosition.GetPositionY(), movePosition.GetPositionZ(), false);
    //     PathType type = path.GetPathType();
    //     Movement::PointsArray const& points = path.GetPath();
    //     movePath.addPath(startPosition.fromPointsArray(points));
    // }

    // if (!movePath.empty())
    // {
    //     if (movePath.makeShortCut(startPosition, maxDist))
    //         if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
    //             botAI->TellMasterNoFacing("Found a shortcut.");

    //     if (movePath.empty())
    //     {
    //         AI_VALUE(LastMovement&, "last movement").setPath(movePath);

    //         if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
    //             botAI->TellMasterNoFacing("Too far from path. Rebuilding.");
    //         LOG_DEBUG("playerbots", "movePath.empty()");
    //         return true;
    //     }

    //     TravelNodePathType pathType;
    //     uint32 entry;
    //     movePosition = movePath.getNextPoint(startPosition, maxDist, pathType, entry);

    //     if (pathType == TravelNodePathType::portal) // && !botAI->isRealPlayer())
    //     {
    //         //Log bot movement
    //         if (sPlayerbotAIConfig.HasLog("bot_movement.csv"))
    //         {
    //             WorldPosition telePos;
    //             if (entry)
    //             {
    //                 if (AreaTriggerTeleport const* at = sObjectMgr->GetAreaTriggerTeleport(entry))
    //                     telePos = WorldPosition(at->target_mapId, at->target_X, at->target_Y, at->target_Z,
    //                     at->target_Orientation);
    //             }
    //             else
    //                 telePos = movePosition;

    //             std::ostringstream out;
    //             out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
    //             out << bot->GetName() << ",";
    //             if (telePos && telePos.GetExactDist(movePosition) > 0.001)
    //                 startPosition.printWKT({ startPosition, movePosition, telePos }, out, 1);
    //             else
    //                 startPosition.printWKT({ startPosition, movePosition }, out, 1);

    //             out << std::to_string(bot->getRace()) << ",";
    //             out << std::to_string(bot->getClass()) << ",";
    //             out << bot->GetLevel() << ",";
    //             out << (entry ? -1 : entry);

    //             sPlayerbotAIConfig.Log("bot_movement.csv", out.str().c_str());
    //         }

    //         if (entry)
    //         {
    //             AI_VALUE(LastMovement&, "last area trigger").lastAreaTrigger = entry;
    //         }
    //         else
    //         {
    //             LOG_DEBUG("playerbots", "!entry");
    //             return bot->TeleportTo(movePosition.GetMapId(), movePosition.GetPositionX(), movePosition.GetPositionY(),
    //             movePosition.GetPositionZ(), movePosition.GetOrientation(), 0);
    //         }
    //     }

    //     if (pathType == TravelNodePathType::transport && entry)
    //     {
    //         if (!bot->GetTransport())
    //         {
    //             for (auto& transport : movePosition.getTransports(entry))
    //                 if (movePosition.sqDistance2d(WorldPosition((WorldObject*)transport)) < 5 * 5)
    //                     transport->AddPassenger(bot, true);
    //         }
    //         WaitForReach(100.0f);
    //         LOG_DEBUG("playerbots", "pathType == TravelNodePathType::transport && entry");
    //         return true;
    //     }

    //     if (pathType == TravelNodePathType::flightPath && entry)
    //     {
    //         if (TaxiPathEntry const* tEntry = sTaxiPathStore.LookupEntry(entry))
    //         {
    //             Creature* unit = nullptr;

    //             if (!bot->m_taxi.IsTaximaskNodeKnown(tEntry->from))
    //             {
    //                 GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    //                 for (GuidVector::iterator i = npcs.begin(); i != npcs.end(); i++)
    //                 {
    //                     Creature* unit = bot->GetNPCIfCanInteractWith(*i, UNIT_NPC_FLAG_FLIGHTMASTER);
    //                     if (!unit)
    //                         continue;

    //                     bot->GetSession()->SendLearnNewTaxiNode(unit);

    //                     unit->SetFacingTo(unit->GetAngle(bot));
    //                 }
    //             }

    //             uint32 botMoney = bot->GetMoney();
    //             if (botAI->HasCheat(BotCheatMask::gold))
    //             {
    //                 bot->SetMoney(10000000);
    //             }

    //             bool goTaxi = bot->ActivateTaxiPathTo({ tEntry->from, tEntry->to }, unit, 1);

    //             if (botAI->HasCheat(BotCheatMask::gold))
    //                 bot->SetMoney(botMoney);
    //             LOG_DEBUG("playerbots", "goTaxi");
    //             return goTaxi;
    //         }
    //     }

    //     // if (pathType == TravelNodePathType::teleportSpell && entry)
    //     // {
    //     //     if (entry == 8690)
    //     //     {
    //     //         if (!bot->HasSpellCooldown(8690))
    //     //         {
    //     //             return botAI->DoSpecificAction("hearthstone", Event("move action"));
    //     //         }
    //     //         else
    //     //         {
    //     //             movePath.clear();
    //     //             AI_VALUE(LastMovement&, "last movement").setPath(movePath);
    //     //             LOG_DEBUG("playerbots", "bot->HasSpellCooldown(8690)");
    //     //             return false;
    //     //         }
    //     //     }
    //     // }

    //     //if (!isTransport && bot->GetTransport())
    //     //    bot->GetTransport()->RemovePassenger(bot);
    // }

    // AI_VALUE(LastMovement&, "last movement").setPath(movePath);

    // if (!movePosition || movePosition.GetMapId() != bot->GetMapId())
    // {
    //     movePath.clear();
    //     AI_VALUE(LastMovement&, "last movement").setPath(movePath);

    //     if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
    //         botAI->TellMasterNoFacing("No point. Rebuilding.");
    //     LOG_DEBUG("playerbots", "!movePosition || movePosition.GetMapId() != bot->GetMapId()");
    //     return false;
    // }

    // if (movePosition.distance(startPosition) > maxDist)
    // {
    //     //Use standard pathfinder to find a route.
    //     PathGenerator path(mover);
    //     path.CalculatePath(movePosition.getX(), movePosition.getY(), movePosition.getZ(), false);
    //     PathType type = path.GetPathType();
    //     Movement::PointsArray const& points = path.GetPath();
    //     movePath.addPath(startPosition.fromPointsArray(points));
    //     TravelNodePathType pathType;
    //     uint32 entry;
    //     movePosition = movePath.getNextPoint(startPosition, maxDist, pathType, entry);
    // }

    // if (movePosition == WorldPosition())
    // {
    //     movePath.clear();

    //     AI_VALUE(LastMovement&, "last movement").setPath(movePath);

    //     if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
    //         botAI->TellMasterNoFacing("No point. Rebuilding.");

    //     return false;
    // }

    // //Visual waypoints
    // if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
    // {
    //     if (!movePath.empty())
    //     {
    //         float cx = x;
    //         float cy = y;
    //         float cz = z;
    //         for (auto i : movePath.getPath())
    //         {
    //             CreateWp(bot, i.point.GetPositionX(), i.point.GetPositionY(), i.point.GetPositionZ(), 0.f, 2334);

    //             cx = i.point.GetPositionX();
    //             cy = i.point.GetPositionY();
    //             cz = i.point.GetPositionZ();
    //         }
    //     }
    //     else
    //         CreateWp(bot, movePosition.GetPositionX(), movePosition.GetPositionY(), movePosition.GetPositionZ(), 0, 2334, true);
    // }

    // //Log bot movement
    // if (sPlayerbotAIConfig.HasLog("bot_movement.csv") && lastMove.lastMoveShort.GetExactDist(movePosition) > 0.001)
    // {
    //     std::ostringstream out;
    //     out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
    //     out << bot->GetName() << ",";
    //     startPosition.printWKT({ startPosition, movePosition }, out, 1);
    //     out << std::to_string(bot->getRace()) << ",";
    //     out << std::to_string(bot->getClass()) << ",";
    //     out << bot->GetLevel();
    //     out << 0;

    //     sPlayerbotAIConfig.Log("bot_movement.csv", out.str().c_str());
    // }
    // // LOG_DEBUG("playerbots", "({}, {}) -> ({}, {})", startPosition.GetPositionX(), startPosition.GetPositionY(),
    // movePosition.GetPositionX(), movePosition.GetPositionY()); if (!react)
    //     if (totalDistance > maxDist)
    //         WaitForReach(startPosition.distance(movePosition) - 10.0f);
    //     else
    //         WaitForReach(startPosition.distance(movePosition));

    // if (!isVehicle)
    // {
    //     bot->HandleEmoteCommand(0);
    //     if (bot->IsSitState())
    //         bot->SetStandState(UNIT_STAND_STATE_STAND);

    //     bot->CastStop();

    //  /* Why do we do this?
    // if (lastMove.lastMoveShort.distance(movePosition) < minDist)
    // {
    //     bot->StopMoving();
    //     bot->GetMotionMaster()->Clear();
    // }
    // */

    // // Clean movement if not already moving the same way.
    // // if (mover->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
    // // {
    // //     mover->StopMoving();
    // //     mover->GetMotionMaster()->Clear();
    // // }
    // // else
    // // {
    // //     mover->GetMotionMaster()->GetDestination(x, y, z);
    // //     if (movePosition.distance(WorldPosition(movePosition.GetMapId(), x, y, z, 0)) > minDist)
    // //     {
    // //         mover->StopMoving();
    // //         mover->GetMotionMaster()->Clear();
    // //     }
    // // }

    // if (totalDistance > maxDist && !detailedMove && !botAI->HasPlayerNearby(&movePosition)) // Why walk if you can
    // fly?
    // {
    //     time_t now = time(nullptr);

    //     AI_VALUE(LastMovement&, "last movement").nextTeleport = now +
    //     (time_t)MoveDelay(startPosition.distance(movePosition)); LOG_DEBUG("playerbots", "totalDistance > maxDist &&
    //     !detailedMove && !botAI->HasPlayerNearby(&movePosition)"); return bot->TeleportTo(movePosition.GetMapId(),
    //     movePosition.GetPositionX(), movePosition.GetPositionY(), movePosition.GetPositionZ(), startPosition.getAngleTo(movePosition));
    // }

    // // walk if master walks and is close
    // bool masterWalking = false;
    // if (botAI->GetMaster())
    // {
    //     if (botAI->GetMaster()->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_WALKING) &&
    //     ServerFacade::instance().GetDistance2d(bot, botAI->GetMaster()) < 20.0f)
    //         masterWalking = true;
    // }

    // if (masterWalking)
    //     bot->SetWalk(true);

    // bot->SendMovementFlagUpdate();
    // // LOG_DEBUG("playerbots", "normal move? {} {} {}",
    // !bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) && !bot->HasAuraType(SPELL_AURA_FLY),
    // //     bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE), bot->getStandState());
    // if (!bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) && !bot->HasAuraType(SPELL_AURA_FLY))
    // {
    //     bot->SetWalk(masterWalking);
    //     bot->GetMotionMaster()->MovePoint(movePosition.GetMapId(), movePosition.GetPositionX(), movePosition.GetPositionY(),
    //     movePosition.GetPositionZ(), generatePath); WaitForReach(startPosition.distance(movePosition));
    //     // LOG_DEBUG("playerbots", "Movepoint to ({}, {})", movePosition.GetPositionX(), movePosition.GetPositionY());
    // }
    // else
    // {
    //     bool needFly = false;
    //     bool needLand = false;
    //     bool isFly = bot->IsFlying();

    //     if (!isFly && originalZ > bot->GetPositionZ() && (originalZ - bot->GetPositionZ()) > 5.0f)
    //         needFly = true;

    //     if (needFly && !isFly)
    //     {
    //         WorldPacket data(SMSG_SPLINE_MOVE_SET_FLYING, 9);
    //         data << bot->GetPackGUID();
    //         bot->SendMessageToSet(&data, true);

    //         if (!bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_FLYING))
    //             bot->m_movementInfo.AddMovementFlag(MOVEMENTFLAG_FLYING);

    //         if (!bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY))
    //             bot->m_movementInfo.AddMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
    //     }

    //     if (isFly)
    //     {
    //         float ground = bot->GetPositionZ();
    //         float height = bot->GetMap()->GetWaterOrGroundLevel(bot->GetPositionX(), bot->GetPositionY(),
    //         bot->GetPositionZ(), ground); if (bot->GetPositionZ() > originalZ && (bot->GetPositionZ() - originalZ
    //         < 5.0f) && (fabs(originalZ - ground) < 5.0f))
    //             needLand = true;

    //         if (needLand)
    //         {
    //             WorldPacket data(SMSG_SPLINE_MOVE_UNSET_FLYING, 9);
    //             data << bot->GetPackGUID();
    //             bot->SendMessageToSet(&data, true);

    //             if (bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_FLYING))
    //                 bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_FLYING);

    //             if (bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY))
    //                 bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
    //         }
    //     }

    //     bot->GetMotionMaster()->MovePoint(movePosition.GetMapId(), Position(movePosition.GetPositionX(), movePosition.GetPositionY(),
    //     movePosition.GetPositionZ(), 0.f)); WaitForReach(startPosition.distance(movePosition)); LOG_DEBUG("playerbots",
    //     "Movepoint to ({}, {})", movePosition.GetPositionX(), movePosition.GetPositionY());
    // }

    // AI_VALUE(LastMovement&, "last movement").setShort(movePosition);

    // if (!idle)
    //     ClearIdleState();

    // LOG_DEBUG("playerbots", "return true in the end");
    // return true;
}

bool MovementAction::MoveTo(WorldObject* target, float distance, MovementPriority priority)
{
    if (!IsMovingAllowed(target))
        return false;

    float bx = bot->GetPositionX();
    float by = bot->GetPositionY();
    float bz = bot->GetPositionZ();

    float tz = target->GetPositionZ();

    float distanceToTarget = bot->GetDistance(target);
    float angle = bot->GetAngle(target);
    float needToGo = distanceToTarget - distance;

    float maxDistance = sPlayerbotAIConfig.SpellDistance;
    if (needToGo > 0 && needToGo > maxDistance)
        needToGo = maxDistance;
    else if (needToGo < 0 && needToGo < -maxDistance)
        needToGo = -maxDistance;

    float dx = cos(angle) * needToGo + bx;
    float dy = sin(angle) * needToGo + by;
    // Start from a seed Z between bot and target, then clamp to the
    // terrain under (dx,dy). Linear interpolation alone ignores hills
    // between the two units and fed PointMovementGenerator a Z that
    // could be well above/below ground, triggering straight-line
    // fallbacks through walls.
    float dz;
    if (distanceToTarget > CONTACT_DISTANCE)
        dz = bz + (tz - bz) * (needToGo / distanceToTarget);
    else
        dz = tz;
    bot->UpdateAllowedPositionZ(dx, dy, dz);

    // To a unit at a distance: the path guard may pick another spot on that ring (a follow leg keeps its own kind).
    GuardedMove const guarded = _guardedMove;
    ObjectGuid const guardTarget = _guardTarget;
    float const guardRange = _guardRange;
    Unit* const unit = target->ToUnit();
    if (unit && guarded == GuardedMove::GENERIC)
    {
        _guardedMove = GuardedMove::REACH;
        _guardTarget = unit->GetGUID();
        _guardRange = distance + bot->GetCombatReach() + unit->GetCombatReach();
    }

    bool const moved = MoveTo(target->GetMapId(), dx, dy, dz, false, false, false, false, priority);
    _guardedMove = guarded;
    _guardTarget = guardTarget;
    _guardRange = guardRange;
    return moved;
}

bool MovementAction::ReachCombatTo(Unit* target, float distance)
{
    if (!IsMovingAllowed(target))
        return false;

    // [pullplan] a solo bot's pull from its pull spot: it pulls from there and meets the target there, not in its camp
    {
        G3D::Vector3 spot;
        switch (SoloSafety::PullStep(bot, target, spot))
        {
            case SoloSafety::PullMove::Hold:
                if (bot->isMoving())
                    bot->StopMoving();
                bot->SetFacingToObject(target);
                return true;
            case SoloSafety::PullMove::ToSpot:
                return MoveTo(bot->GetMapId(), spot.x, spot.y, spot.z, false, false, false, false,
                              MovementPriority::MOVEMENT_COMBAT, true);
            default:
                break;
        }
    }

    float tx = target->GetPositionX();
    float ty = target->GetPositionY();
    float tz = target->GetPositionZ();

    float targetOrientation = target->GetOrientation();

    float deltaAngle = Position::NormalizeOrientation(targetOrientation - target->GetAngle(bot));
    if (deltaAngle > M_PI)
        deltaAngle -= 2.0f * M_PI;  // -PI..PI
    // if target is moving forward and moving far away, predict the position
    bool behind = fabs(deltaAngle) > M_PI_2;
    if (target->HasUnitMovementFlag(MOVEMENTFLAG_FORWARD) && behind)
    {
        float predictDis = std::min(3.0f, target->GetObjectSize() * 2);
        tx += cos(target->GetOrientation()) * predictDis;
        ty += sin(target->GetOrientation()) * predictDis;
        if (!target->GetMap()->CheckCollisionAndGetValidCoords(target, target->GetPositionX(), target->GetPositionY(),
                                                               target->GetPositionZ(), tx, ty, tz))
        {
            tx = target->GetPositionX();
            ty = target->GetPositionY();
            tz = target->GetPositionZ();
        }
    }
    float combatDistance = bot->GetCombatReach() + target->GetCombatReach();
    distance += combatDistance;

    // [los_restore] in range but nobody of the group sees it (a shooter behind cover): to a spot that sees it
    if (bot->GetExactDist(tx, ty, tz) <= distance)
        return LosRestoreWalk(target, std::max(NOMINAL_MELEE_RANGE, combatDistance + 4.0f / 3.0f) - 0.5f);

    float shortenTo = distance;

    // Avoid walking too far when moving towards each other
    float disToGo = bot->GetExactDist(tx, ty, tz) - distance;
    if (disToGo >= 6.0f)
        shortenTo = disToGo / 2 + distance;

    // Under water a player swims the straight line to the target, not the navmesh path: from the surface that path
    // ends on the surface right above a mob on the bottom (Blackfathom Deeps wave 38, the tank 9.5 yd above it).
    G3D::Vector3 const targetPos(tx, ty, tz);
    G3D::Vector3 const toBot = G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) - targetPos;
    G3D::Vector3 const swimEnd = targetPos + toBot.direction() * shortenTo;
    G3D::Vector3 endPos = swimEnd;
    if (!SwimsTo(bot, swimEnd.x, swimEnd.y, swimEnd.z))
    {
        BotPath path = FindBotPath(bot, tx, ty, tz);  // [lead109]
        PathType type = path.type;
        int typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT;
        if (!(type & typeOk))
            return LosRestoreWalk(target, std::max(NOMINAL_MELEE_RANGE, combatDistance + 4.0f / 3.0f) - 0.5f);

        // [hleft-chase-drop] No chase down to a level the navmesh does not join: a path that does not reach the
        // target's polygon (INCOMPLETE / SHORTCUT) to a target far below ends in a fall off the ledge, and from the
        // floor below nothing leads back. Stratholme heroic 2026-10-25 (20261025-062726-strat-h-5A-30): the tank's
        // "reach healer attacker" after a Plagued Insect at (3601.7,-3344.1,61.2) dropped it from z 107 to z 61
        // (tools/navmesh_probe.py 329 3608 -3339 123 3602 -3344 61: NOT CONNECTED, gap 64.8 yd). The target is
        // dropped instead.
        if (Map* map = bot->GetMap(); map && map->IsDungeon() && !bot->IsFlying() && !bot->isSwimming() &&
                                      !bot->GetVehicle() && !bot->GetTransport() &&
                                      (type & (PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT)) &&
                                      ((type & PATHFIND_SHORTCUT) ? bot->GetPositionZ()
                                                                   : path.actualEnd.z) -
                                              tz >
                                          CHASE_DROP_DZ)
        {
            RefuseChase(target, "the target stands on a lower level the navmesh does not join");
            return false;
        }

        // [b32_occ] ... nor a walk to a target hovering over open air at the bot's own height: the guard above sees
        // only a target BELOW the path's end. occ-h 20261025-221216-occ-h-5G-22 (496 s): the tank (Blood DK) on
        // Ring 2's drake landing (1160.9,1038.6,433.1) walked "reach pull" for Death Grip range to an Azure Ring
        // Guardian flying 42 yd off past the rim (ground under it: the instance floor z ~107); the spline ran from
        // the rim straight down through the air (spline end (1171.1,1041.2,105.1)) and left it on the Drakos level
        // z 363, 70 yd under the group - no navmesh either way, no drake (navmesh_probe 578: NOT CONNECTED, gap
        // 328 yd); the four members re-entered as stranded (four deaths). Same Guardian, same landing, warrior
        // Charge variant: 184229-occ-h-5F-26 (the [b30_oculus] GapCloserLeavesGround guard). The walk is judged as
        // the fall it is: to the ground under the target (JudgeLedgeDrop), refused with no ground at all.
        if (Map* map = bot->GetMap(); map && map->IsDungeon() && !bot->IsFlying() && !bot->isSwimming() &&
                                      !bot->GetVehicle() && !bot->GetTransport())
        {
            float level = target->GetMapWaterOrGroundLevel(tx, ty, tz);
            // [b35_z0] the grid's fallback (0.0 on a WMO-only tile: the vmap ray from tz found no floor) is no
            // ground: a target standing on navmesh is on its floor, not over open air
            if (IsGridFallbackZ(bot, tx, ty, level))
            {
                float floor = 0.0f;
                if (NavmeshFloorZ(bot, tx, ty, tz, GAP_CLOSER_AIR_HEIGHT, floor) && floor <= tz + 1.0f)
                    level = floor;
            }
            bool const overAir = level <= INVALID_HEIGHT || tz - level > GAP_CLOSER_AIR_HEIGHT;
            if (overAir &&
                (level <= INVALID_HEIGHT ||
                 (bot->GetPositionZ() - level > LEDGE_DROP_DZ &&
                  !JudgeLedgeDrop(bot, tx, ty, level, getName().c_str()).allowed)))
            {
                RefuseChase(target, level <= INVALID_HEIGHT
                                        ? "the target hovers over open air with no ground under it"
                                        : Acore::StringFormat("the target hovers over open air, its ground {:.1f} yd "
                                                              "below: the walk to it is a fall",
                                                              bot->GetPositionZ() - level));
                return false;
            }
        }

        // [hleft-door-chase] No chase through a closed door either (the navmesh holds no game objects): Blackrock
        // Depths 2026-10-25 - the group crossed the closed ArenaGate03 161524 and the garrison door 161516 in combat
        // chases and reached Ribbly / Hurley without the Ring of Law or the Shadowforge Lock.
        if (Map* map = bot->GetMap(); map && map->IsDungeon() && !bot->IsFlying() && !bot->GetVehicle() &&
                                      !bot->GetTransport() &&
                                      ClosedDoorNear(bot, bot->GetPositionX(), bot->GetPositionY(), tx, ty,
                                                     CLOSED_DOOR_SEARCH))
            if (ClosedDoorHit const hit = PathThroughClosedDoor(bot, path.points);
                hit.Found() && !DoorOpensByUse(hit.door))  // a click door: MoveTo's guard walks up and opens it
            {
                RefuseChase(target, Acore::StringFormat("the way runs through the closed door {} {}",
                                                        hit.door->GetName(), hit.door->GetEntry()));
                return false;
            }

        // if (bot->GetExactDist(tx, ty, tz) <= shortenTo)
        //     return false;

        path.ShortenPathUntilDist(G3D::Vector3(tx, ty, tz), shortenTo);
        endPos = path.points.back();
    }

    // The path guard may pick another spot on the ring of `distance` round the target.
    GuardedMove const guarded = _guardedMove;
    ObjectGuid const guardTarget = _guardTarget;
    float const guardRange = _guardRange;
    _guardedMove = GuardedMove::REACH;
    _guardTarget = target->GetGUID();
    _guardRange = distance;
    bool const moved = MoveTo(target->GetMapId(), endPos.x, endPos.y, endPos.z, false, false, false, false,
                              MovementPriority::MOVEMENT_COMBAT, true);
    _guardedMove = guarded;
    _guardTarget = guardTarget;
    _guardRange = guardRange;
    return moved;
}

float MovementAction::GetFollowAngle()
{
    Player* master = GetMaster();
    Group* group = master ? master->GetGroup() : bot->GetGroup();
    if (!group)
        return 0.0f;

    // Prevent bots with orphaned raid groups from dividing by 0, which freezes the server.
    uint32 memberCount = group->GetMembersCount();
    if (memberCount <= 1)
        return 0.0f;

    uint32 index = 1;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        if (ref->GetSource() == master)
            continue;

        if (ref->GetSource() == bot)
            return 2 * M_PI / (memberCount - 1) * index;

        ++index;
    }

    return 0;
}

bool MovementAction::IsMovingAllowed(WorldObject* target)
{
    if (!target)
        return false;

    if (bot->GetMapId() != target->GetMapId())
        return false;

    // float distance = ServerFacade::instance().GetDistance2d(bot, target);
    // if (!bot->InBattleground() && distance > sPlayerbotAIConfig.ReactDistance)
    //     return false;

    return IsMovingAllowed();
}

bool MovementAction::IsDuplicateMove(float x, float y, float z)
{
    LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");

    // heuristic 5s
    if (lastMove.msTime + sPlayerbotAIConfig.MaxWaitForMove < getMSTime())
        return false;

    // [lead1000] b229 rfc-h 5H-16 724.7 s: "the route walk refused by IsDuplicateMove (the same walk is on)" with the
    // tank standing - the walk's end was still in LastMovement after the walk had stopped (a MoveStop: lead960
    // StopPullWalkInFight, lead792 StopOwnWalkToMob, a stun). Only a walk under way is the same walk: a stopped one is
    // walked again (the [lead280] rule below, now for the walk's own end too)
    if (!bot->isMoving())
        return false;

    // [lead280] the same end as asked while that walk is still under way
    return lastMove.lastMoveShort.GetExactDist(x, y, z) <= 0.01f ||
           lastMove.lastMoveAsked.GetExactDist(x, y, z) <= 0.01f;
}

bool MovementAction::IsWaitingForLastMove(MovementPriority priority)
{
    LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");

    if (priority > lastMove.priority)
        return false;

    // heuristic 5s
    if (lastMove.lastdelayTime + lastMove.msTime > getMSTime())
        return true;

    return false;
}

bool MovementAction::IsMovingAllowed()
{
    return botAI->CanMove();
}

bool MovementAction::Follow(Unit* target, float distance) { return Follow(target, distance, GetFollowAngle()); }

void MovementAction::UpdateMovementState()
{
    const bool isCurrentlyRestricted =  // see if the bot is currently slowed, rooted, or otherwise unable to move
        bot->HasUnitState(UNIT_STATE_LOST_CONTROL) || bot->IsRooted() || bot->isFrozen() || bot->IsPolymorphed();

    // no update movement flags while movement is current restricted.
    if (!isCurrentlyRestricted && bot->IsAlive())
    {
        // state flags
        const auto master = botAI ? botAI->GetMaster() : nullptr;
        const auto liquidState = bot->GetLiquidData().Status;
        const float gZ = bot->GetMapWaterOrGroundLevel(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        const bool onGroundZ = bot->GetPositionZ() < gZ + 1.f;
        const bool wantsSwim = liquidState == LIQUID_MAP_IN_WATER || liquidState == LIQUID_MAP_UNDER_WATER;
        const bool wantsFly = bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura();
        const bool canWaterWalk = bot->HasWaterWalkAura();
        const bool isMasterFlying = master ? master->HasUnitMovementFlag(MOVEMENTFLAG_FLYING) : true;
        const bool isMasterSwimming = master ? master->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING) : true;
        const bool isFlying = bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING);
        const bool isSwimming = bot->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
        const bool isWaterWalking = bot->HasUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
        const bool hasGravityDisabled = bot->HasUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
        bool movementFlagsUpdated = false;

        // handle water (fragile logic do not alter without testing every detail, animation and transition)
        if (liquidState != LIQUID_MAP_NO_WATER && !isFlying)
        {
            if (canWaterWalk && !isMasterSwimming && !isWaterWalking)
            {
                bot->SetSwim(false);
                bot->AddUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
                movementFlagsUpdated = true;
            }
            else if ((!canWaterWalk || isMasterSwimming) && isWaterWalking)
            {
                bot->RemoveUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
                if (wantsSwim)
                    bot->SetSwim(true);
                movementFlagsUpdated = true;
            }
            else if (!wantsSwim && isSwimming)
            {
                bot->SetSwim(false);
                movementFlagsUpdated = true;
            }
        }

        // reset when not around water while swimming or water walking
        if (liquidState == LIQUID_MAP_NO_WATER && (isSwimming || isWaterWalking))
        {
            bot->SetSwim(false);
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
            movementFlagsUpdated = true;
        }

        // handle flying
        if (wantsFly && !isFlying && isMasterFlying)
        {
            bot->AddUnitMovementFlag(MOVEMENTFLAG_CAN_FLY);
            bot->AddUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
            bot->AddUnitMovementFlag(MOVEMENTFLAG_FLYING);
            movementFlagsUpdated = true;
        }
        else if (!wantsFly && !isWaterWalking && (isFlying || hasGravityDisabled))
        {
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_CAN_FLY);
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING);
            movementFlagsUpdated = true;
        }
        else if (!isMasterFlying && isFlying && onGroundZ)
        {
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_CAN_FLY);
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING);
            movementFlagsUpdated = true;
        }

        // detect if movement/CC restrictions have been ended, refresh movement state for animations.
        if (wasMovementRestricted)
            movementFlagsUpdated = true;

        // movement flags should only be updated between state changes, if not it will break certain effects.
        if (movementFlagsUpdated)
            bot->SendMovementFlagUpdate();
    }

    // Save current state for the next check
    wasMovementRestricted = isCurrentlyRestricted;

    // Temporary speed increase in group
    // if (botAI->HasGameClientMaster())
    //     bot->SetSpeedRate(MOVE_RUN, 1.1f);
    // else
    //     bot->SetSpeedRate(MOVE_RUN, 1.0f);

    // check if target is not reachable (from Vmangos)
    // if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE && bot->CanNotReachTarget() &&
    // !bot->InBattleground())
    // {
    //     if (Unit* pTarget = ServerFacade::instance().GetChaseTarget(bot))
    //     {
    //         if (!bot->IsWithinMeleeRange(pTarget) && pTarget->IsCreature())
    //         {
    //             float angle = bot->GetAngle(pTarget);
    //             float distance = 5.0f;
    //             float x = bot->GetPositionX() + cos(angle) * distance;
    //             float y = bot->GetPositionY() + sin(angle) * distance;
    //             float z = bot->GetPositionZ();

    //             z += CONTACT_DISTANCE;
    //             bot->UpdateAllowedPositionZ(x, y, z);

    //             bot->StopMoving();
    //             bot->GetMotionMaster()->Clear();
    //             bot->NearTeleportTo(x, y, z, bot->GetOrientation());
    //             //bot->GetMotionMaster()->MovePoint(bot->GetMapId(), x, y, z, FORCED_MOVEMENT_RUN, false);
    //             return;
    //             /*if (pTarget->IsCreature() && !bot->isMoving() && bot->IsWithinDist(pTarget, 10.0f))
    //             {
    //                 // Cheating to prevent getting stuck because of bad mmaps.
    //                 bot->StopMoving();
    //                 bot->GetMotionMaster()->Clear();
    //                 bot->GetMotionMaster()->MovePoint(bot->GetMapId(), pTarget->GetPosition(), FORCED_MOVEMENT_RUN,
    //                 false); return;
    //             }*/
    //         }
    //     }
    // }

    // if ((bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE ||
    //     bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE) && bot->CanNotReachTarget()
    //     && !bot->InBattleground())
    // {
    //     if (Unit* pTarget = ServerFacade::instance().GetChaseTarget(bot))
    //     {
    //         if (pTarget != botAI->GetGroupLeader())
    //             return;

    //         if (!bot->IsWithinMeleeRange(pTarget))
    //         {
    //             if (!bot->isMoving() && bot->IsWithinDist(pTarget, 10.0f))
    //             {
    //                 // Cheating to prevent getting stuck because of bad mmaps.
    //                 float angle = bot->GetAngle(pTarget);
    //                 float distance = 5.0f;
    //                 float x = bot->GetPositionX() + cos(angle) * distance;
    //                 float y = bot->GetPositionY() + sin(angle) * distance;
    //                 float z = bot->GetPositionZ();

    //                 z += CONTACT_DISTANCE;
    //                 bot->UpdateAllowedPositionZ(x, y, z);

    //                 bot->StopMoving();
    //                 bot->GetMotionMaster()->Clear();
    //                 bot->NearTeleportTo(x, y, z, bot->GetOrientation());
    //                 //bot->GetMotionMaster()->MovePoint(bot->GetMapId(), x, y, z, FORCED_MOVEMENT_RUN, false);
    //                 return;
    //             }
    //         }
    //     }
    // }
}

bool MovementAction::Follow(Unit* target, float distance, float angle)
{
    UpdateMovementState();

    if (!target)
        return false;

    if (!bot->InBattleground() && ServerFacade::instance().IsDistanceLessOrEqualThan(ServerFacade::instance().GetDistance2d(bot, target),
                                                                           sPlayerbotAIConfig.FollowDistance))
    {
        // botAI->TellError("No need to follow");
        return false;
    }

    // [obstacle_sidestep] a follow stopped by a decoration on the way steps round it.
    if (target->GetMapId() == bot->GetMapId() &&
        SidestepObstacle(target->GetMapId(), target->GetPositionX(), target->GetPositionY(), target->GetPositionZ()))
        return true;

    // Move to target corpse if alive.
    if (!target->IsAlive() && bot->IsAlive() && target->GetGUID().IsPlayer())
    {
        Player* pTarget = (Player*)target;

        Corpse* corpse = pTarget->GetCorpse();

        if (corpse)
        {
            WorldPosition botPos(bot);
            WorldPosition cPos(corpse);

            if (botPos.fDist(cPos) > sPlayerbotAIConfig.SpellDistance)
                return MoveTo(cPos.GetMapId(), cPos.GetPositionX(), cPos.GetPositionY(), cPos.GetPositionZ());
        }
    }

    if (ServerFacade::instance().IsDistanceGreaterOrEqualThan(ServerFacade::instance().GetDistance2d(bot, target),
                                                    sPlayerbotAIConfig.SightDistance))
    {
        if (target->GetGUID().IsPlayer())
        {
            Player* pTarget = (Player*)target;

            PlayerbotAI* targetBotAI = GET_PLAYERBOT_AI(pTarget);
            if (targetBotAI)  // Try to move to where the bot is going if it is closer and in the same direction.
            {
                WorldPosition botPos(bot);
                WorldPosition tarPos(target);
                WorldPosition longMove =
                    targetBotAI->GetAiObjectContext()->GetValue<WorldPosition>("last long move")->Get();

                if (longMove)
                {
                    float lDist = botPos.fDist(longMove);
                    float tDist = botPos.fDist(tarPos);
                    float ang = botPos.getAngleBetween(tarPos, longMove);
                    if ((lDist * 1.5 < tDist && ang < static_cast<float>(M_PI) / 2) ||
                        target->HasUnitState(UNIT_STATE_IN_FLIGHT))
                    {
                        return MoveTo(longMove.GetMapId(), longMove.GetPositionX(), longMove.GetPositionY(), longMove.GetPositionZ());
                    }
                }
            }
            else
            {
                if (pTarget->HasUnitState(UNIT_STATE_IN_FLIGHT))  // Move to where the player is flying to.
                {
                    TaxiPathNodeList const& tMap =
                        static_cast<FlightPathMovementGenerator*>(pTarget->GetMotionMaster()->top())->GetPath();
                    if (!tMap.empty())
                    {
                        auto tEnd = tMap.back();
                        if (tEnd)
                            return MoveTo(tEnd->mapid, tEnd->x, tEnd->y, tEnd->z);
                    }
                }
            }
        }

        if (!target->HasUnitState(UNIT_STATE_IN_FLIGHT))
            return MoveTo(target, sPlayerbotAIConfig.FollowDistance);
    }

    if (ServerFacade::instance().IsDistanceLessOrEqualThan(ServerFacade::instance().GetDistance2d(bot, target),
                                                 sPlayerbotAIConfig.FollowDistance))
    {
        // botAI->TellError("No need to follow");
        return false;
    }

    if (target->IsFriendlyTo(bot) && bot->IsMounted() && AI_VALUE(GuidVector, "all targets").empty())
        distance += angle;

    if (!bot->InBattleground() && ServerFacade::instance().IsDistanceLessOrEqualThan(ServerFacade::instance().GetDistance2d(bot, target),
                                                                           sPlayerbotAIConfig.FollowDistance))
    {
        // botAI->TellError("No need to follow");
        return false;
    }

    // In a dungeon a member follows the tank along the ground it walked, not the shortest line past the next pack
    // (Stormwind Stockade wave 40: healers and damage dealers ran ahead into the room and woke the cells).
    if (Player* tank = target->ToPlayer(); tank && tank != bot && tank->IsAlive() && tank->GetMap() == bot->GetMap() &&
                                           !tank->GetTransport() && !tank->HasUnitState(UNIT_STATE_IN_FLIGHT) &&
                                           IsPathGuarded() && bot->GetGroup()->IsMember(tank->GetGUID()) &&
                                           PlayerbotAI::IsTank(tank))
    {
        ClearIdleState();
        return FollowTrail(tank, distance);
    }

    // MoveFollow's spline ignores collision: a follow through a closed door walks up to it instead (MoveTo's
    // GuardClosedDoor), and on once the door opens.
    if (Map* map = bot->GetMap(); map && map->IsDungeon() && target->GetMap() == map && !bot->GetVehicle() &&
                                  !bot->GetTransport() && !bot->IsFlying() &&
                                  ClosedDoorNear(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                 target->GetPositionX(), target->GetPositionY(), CLOSED_DOOR_SEARCH) &&
                                  PathThroughClosedDoor(bot, GuardPath(bot, target->GetPositionX(),
                                                                       target->GetPositionY(), target->GetPositionZ(),
                                                                       bot->isSwimming()))
                                      .Found())
    {
        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
        }

        return MoveTo(target, distance);
    }

    // [lead9] MoveFollow's spline bypasses MoveTo and so every guard on it (the idle-mob path guard, the fight's safe
    // zone): a harness group's follow in a dungeon is a walk to the target judged as every other walk is (plan 1.9,
    // [b40_open] / [b44_gerst]: a follow body-pulled Gerstahn).
    if (target->GetMap() == bot->GetMap() && !bot->GetVehicle() && !bot->GetTransport() &&
        Leader::FightZone::Applies(bot))
    {
        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
        }

        ClearIdleState();
        return MoveTo(target, distance);
    }

    bot->HandleEmoteCommand(0);

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    bot->CastStop();

    // AI_VALUE(LastMovement&, "last movement").Set(target);
    ClearIdleState();

    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
    {
        Unit* currentTarget = ServerFacade::instance().GetChaseTarget(bot);
        if (currentTarget && currentTarget->GetGUID() == target->GetGUID())
            return false;
    }

    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        bot->GetMotionMaster()->Clear();

    bot->GetMotionMaster()->MoveFollow(target, distance, angle);
    return true;
}

void MovementAction::RefuseChase(Unit* target, std::string const& why)
{
    if (!target || s_dryRun)  // [lead70] a dry run drops no target
        return;

    // [hleft-door-chase] / [hleft-chase-drop] the bot drops the target it may not chase (a player target, a group
    // member to heal or to reach, is never dropped: only the move is refused).
    uint32 const now = getMSTime();
    if (_chaseRefusedFor != target->GetGUID() || !_chaseRefusedMs ||
        getMSTimeDiff(_chaseRefusedMs, now) > CHASE_REFUSED_LOG_MS)
    {
        LOG_INFO("playerbots", "[chase refused] {}: {} {} ({:.1f} yd): {} - target dropped", bot->GetName(),
                 getName(), target->GetName(), bot->GetExactDist(target), why);
        _chaseRefusedFor = target->GetGUID();
        _chaseRefusedMs = now ? now : 1;
    }

    if (target->IsPlayer())
        return;

    if (AI_VALUE(Unit*, "current target") == target)
    {
        context->GetValue<Unit*>("current target")->Set(nullptr);
        bot->SetTarget(ObjectGuid::Empty);
        bot->AttackStop();
    }
}

bool MovementAction::ChaseTo(WorldObject* obj, float distance)
{
    if (!IsMovingAllowed())
    {
        return false;
    }

    // [hleft-door-chase] MoveChase's spline ignores collision: no chase whose path runs through a closed door.
    if (Unit* unit = obj ? obj->ToUnit() : nullptr)
        if (Map* map = bot->GetMap(); map && map->IsDungeon() && !bot->IsFlying() && !bot->GetVehicle() &&
                                      !bot->GetTransport() &&
                                      ClosedDoorNear(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                     unit->GetPositionX(), unit->GetPositionY(), CLOSED_DOOR_SEARCH))
            if (ClosedDoorHit const hit =
                    PathThroughClosedDoor(bot, GuardPath(bot, unit->GetPositionX(), unit->GetPositionY(),
                                                         unit->GetPositionZ(), bot->isSwimming()));
                hit.Found() && !DoorOpensByUse(hit.door))
            {
                RefuseChase(unit, Acore::StringFormat("the way runs through the closed door {} {}",
                                                      hit.door->GetName(), hit.door->GetEntry()));
                return false;
            }

    // [lead9] MoveChase's spline bypasses MoveTo and so every guard on it (the idle-mob path guard, the fight's safe
    // zone): a harness group's chase is a walk to the target judged as every other walk is.
    if (Unit* unit = obj ? obj->ToUnit() : nullptr;
        unit && unit->GetMap() == bot->GetMap() && !bot->GetVehicle() && Leader::FightZone::Applies(bot))
        return MoveTo(unit, distance, MovementPriority::MOVEMENT_COMBAT);

    if (Vehicle* vehicle = bot->GetVehicle())
    {
        VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
        if (!seat || !seat->CanControl())
            return false;

        // vehicle->GetMotionMaster()->Clear();
        vehicle->GetBase()->GetMotionMaster()->MoveChase((Unit*)obj, 30.0f);
        return true;
    }

    UpdateMovementState();

    if (!bot->IsStandState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    bot->CastStop();

    // bot->GetMotionMaster()->Clear();
    bot->GetMotionMaster()->MoveChase((Unit*)obj, distance);

    // TODO shouldnt this use "last movement" value?
    WaitForReach(bot->GetExactDist2d(obj) - distance);
    return true;
}

float MovementAction::MoveDelay(float distance, bool backwards)
{
    float speed;
    if (bot->isSwimming())
    {
        speed = backwards ? bot->GetSpeed(MOVE_SWIM_BACK) : bot->GetSpeed(MOVE_SWIM);
    }
    else if (bot->IsFlying())
    {
        speed = backwards ? bot->GetSpeed(MOVE_FLIGHT_BACK) : bot->GetSpeed(MOVE_FLIGHT);
    }
    else
    {
        speed = backwards ? bot->GetSpeed(MOVE_RUN_BACK) : bot->GetSpeed(MOVE_RUN);
    }
    float delay = distance / speed;
    return delay;
}

// TODO should this be removed? (or modified to use "last movement" value?)
void MovementAction::WaitForReach(float distance)
{
    float delay = IN_MILLISECONDS * MoveDelay(distance);

    if (delay > sPlayerbotAIConfig.MaxWaitForMove)
        delay = sPlayerbotAIConfig.MaxWaitForMove;

    Unit* target = *botAI->GetAiObjectContext()->GetValue<Unit*>("current target");
    Unit* player = *botAI->GetAiObjectContext()->GetValue<Unit*>("enemy player target");
    if ((player || target) && delay > sPlayerbotAIConfig.GlobalCoolDown)
        delay = sPlayerbotAIConfig.GlobalCoolDown;

    if (delay < 0)
        delay = 0;

    botAI->SetNextCheckDelay((uint32)delay);
    if (IsReaction())
        SetDuration((uint32)delay);
}

// similiar to botAI->SetNextCheckDelay() but only stops movement
void MovementAction::SetNextMovementDelay(float delayMillis)
{
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(),
             delayMillis, MovementPriority::MOVEMENT_FORCED);
}

bool MovementAction::Flee(Unit* target)
{
    Player* master = GetMaster();
    if (!target)
        target = master;

    if (!target)
        return false;

    if (!IsMovingAllowed())
    {
        botAI->TellError("I am stuck while fleeing");
        return false;
    }

    bool foundFlee = false;
    time_t lastFlee = AI_VALUE(LastMovement&, "last movement").lastFlee;
    time_t now = time(0);
    uint32 fleeDelay = urand(2, sPlayerbotAIConfig.ReturnDelay / 1000);

    if (lastFlee)
    {
        if ((now - lastFlee) <= fleeDelay)
        {
            return false;
        }
    }

    Unit* currentVictim = target->CanHaveThreatList() ? target->GetThreatMgr().GetCurrentVictim() : target->GetVictim();
    if (currentVictim && currentVictim == bot)  // bot is target - try to flee to tank or master
    {
        if (Group* group = bot->GetGroup())
        {
            Unit* fleeTarget = nullptr;
            float fleeDistance = sPlayerbotAIConfig.SightDistance;

            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* player = gref->GetSource();
                if (!player || player == bot || !player->IsAlive())
                    continue;

                if (PlayerbotAI::IsTank(player))
                {
                    float distanceToTank = ServerFacade::instance().GetDistance2d(bot, player);
                    if (distanceToTank < fleeDistance)
                    {
                        fleeTarget = player;
                        fleeDistance = distanceToTank;
                    }
                }
            }

            if (fleeTarget)
                foundFlee = MoveNear(fleeTarget);

            if ((!fleeTarget || !foundFlee) && master)
            {
                foundFlee = MoveNear(master);
            }
        }
    }
    else  // bot is not targeted, try to flee dps/healers
    {
        bool isHealer = PlayerbotAI::IsHeal(bot);
        bool needHealer = !isHealer && AI_VALUE2(uint8, "health", "self target") < 50;
        bool isRanged = PlayerbotAI::IsRanged(bot);

        Group* group = bot->GetGroup();
        if (group)
        {
            Unit* fleeTarget = nullptr;
            float fleeDistance = botAI->GetRange("shoot") * 1.5f;
            Unit* spareTarget = nullptr;
            float spareDistance = botAI->GetRange("shoot") * 2.0f;
            std::vector<Unit*> possibleTargets;

            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* player = gref->GetSource();
                if (!player || player == bot || !player->IsAlive())
                    continue;

                if ((isHealer && PlayerbotAI::IsHeal(player)) || needHealer)
                {
                    float distanceToHealer = ServerFacade::instance().GetDistance2d(bot, player);
                    float distanceToTarget = ServerFacade::instance().GetDistance2d(player, target);
                    if (distanceToHealer < fleeDistance &&
                        distanceToTarget > (botAI->GetRange("shoot") / 2 + sPlayerbotAIConfig.FollowDistance) &&
                        (needHealer || player->IsWithinLOSInMap(target)))
                    {
                        fleeTarget = player;
                        fleeDistance = distanceToHealer;
                        possibleTargets.push_back(fleeTarget);
                    }
                }
                else if (isRanged && PlayerbotAI::IsRanged(player))
                {
                    float distanceToRanged = ServerFacade::instance().GetDistance2d(bot, player);
                    float distanceToTarget = ServerFacade::instance().GetDistance2d(player, target);
                    if (distanceToRanged < fleeDistance &&
                        distanceToTarget > (botAI->GetRange("shoot") / 2 + sPlayerbotAIConfig.FollowDistance) &&
                        player->IsWithinLOSInMap(target))
                    {
                        fleeTarget = player;
                        fleeDistance = distanceToRanged;
                        possibleTargets.push_back(fleeTarget);
                    }
                }
                // remember any group member in case no one else found
                float distanceToFlee = ServerFacade::instance().GetDistance2d(bot, player);
                float distanceToTarget = ServerFacade::instance().GetDistance2d(player, target);
                if (distanceToFlee < spareDistance &&
                    distanceToTarget > (botAI->GetRange("shoot") / 2 + sPlayerbotAIConfig.FollowDistance) &&
                    player->IsWithinLOSInMap(target))
                {
                    spareTarget = player;
                    spareDistance = distanceToFlee;
                    possibleTargets.push_back(fleeTarget);
                }
            }

            if (!possibleTargets.empty())
                fleeTarget = possibleTargets[urand(0, possibleTargets.size() - 1)];

            if (!fleeTarget)
                fleeTarget = spareTarget;

            if (fleeTarget)
                foundFlee = MoveNear(fleeTarget);

            if ((!fleeTarget || !foundFlee) && master && master->IsAlive() && master->IsWithinLOSInMap(target))
            {
                float distanceToTarget = ServerFacade::instance().GetDistance2d(master, target);
                if (distanceToTarget > (botAI->GetRange("shoot") / 2 + sPlayerbotAIConfig.FollowDistance))
                    foundFlee = MoveNear(master);
            }
        }
    }

    if ((foundFlee || lastFlee) && bot->GetGroup())
    {
        if (!lastFlee)
            AI_VALUE(LastMovement&, "last movement").lastFlee = now;
        else
            AI_VALUE(LastMovement&, "last movement").lastFlee = 0;
    }

    // The move to the tank or a fellow above is kept: the FleeManager spot used to replace it in the same tick, so a
    // healer chased by a mob ran away from the tank instead of to it (wave 9 Steamvault, 60 yd from the tank).
    if (foundFlee)
        return true;

    FleeManager manager(bot, botAI->GetRange("flee"), bot->GetAngle(target) + M_PI);
    if (!manager.isUseful())
        return false;

    float rx, ry, rz;
    if (!manager.CalculateDestination(&rx, &ry, &rz))
    {
        botAI->TellError("Nowhere to flee");
        return false;
    }

    GuardedMove const guarded = _guardedMove;
    _guardedMove = GuardedMove::FLEE;
    bool result = MoveTo(target->GetMapId(), rx, ry, rz);
    _guardedMove = guarded;

    if (result)
        AI_VALUE(LastMovement&, "last movement").lastFlee = time(nullptr);

    return result;
}

void MovementAction::ClearIdleState()
{
    context->GetValue<time_t>("stay time")->Set(0);
    context->GetValue<PositionMap&>("position")->Get()["random"].Reset();
}

bool MovementAction::HoldFightSpot(float& x, float& y, float& z)
{
    // [los_restore] the walk that gives the group back its sight of the target it fights is never held
    if (_losRestore)
        return true;

    // The chase walks of a fight: to a mob (the tank's target choosers walk by "reach melee"), to the mobs on the
    // group, to the healer's attackers. The pull, the fight spot / corner walks, avoid aoe and flee are not held.
    static constexpr std::string_view TANK_CHASES[] = {
        "reach melee",        "reach spell",       "move to group member attackers", "reach healer attacker",
        "reach group member attacker", "tank assist", "aggressive target", "attack anything",
        "attack least hp target", "attack rti target", "dps assist"};
    std::string const name = getName();
    bool const tankSelf = PlayerbotAI::IsTank(bot);
    if (tankSelf)
    {
        if (std::find(std::begin(TANK_CHASES), std::end(TANK_CHASES), name) == std::end(TANK_CHASES))
            return true;
    }
    // [melee_interrupt_hold] A melee dps chases nothing out of melee reach of the tank's fight spot: no "reach spell"
    // (an interrupt or a spell range walk) and no walk to a LoS pull's old gather spot (scholo-h 20261025-153805 pull
    // 10: "dungeon priority interrupt", "move to los pull spot" and dps assist on the Spectral Tutor took the
    // Enhancement shaman 35 yd from the tank, and the casters out there killed him). Wind Shear is cast from where
    // it stands, or another member interrupts.
    else if ((name != "reach melee" && name != "reach spell" && name != "move to los pull spot") ||
             PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot))
        return true;

    Player* tank = tankSelf ? bot : FindGroupTank(bot);
    Position anchor;
    if (!tank || !FightHold::Get(tank, anchor))
        return true;

    // [fight_hold_stay_caster] A mob already on the group that stands still out there (a SmartAI caster with
    // SMARTCAST_COMBAT_MOVE: Azure Ley-Whelp 27636 holds its Arcane Bolt range) never comes to the tank, and a taunt
    // changes nothing: the chase to it is not held (occ-h 20261025-153805: 180 s idle at a whelp 38.6 yd off).
    if (Unit* chased = AI_VALUE(Unit*, "current target"))
    {
        Unit* victim = chased->GetVictim();
        bool const onGroup = victim && (victim == bot || victim == tank ||
                                        (victim->ToPlayer() && victim->ToPlayer()->IsInSameGroupWith(bot)));
        if (chased->IsAlive() && onGroup && !chased->isMoving() && chased->GetExactDist2d(x, y) <= 10.0f)
            return true;

        // [b47_idle] ... nor a chase that ReachCombatTo halved: from 25+ yd its walk ends half way ("avoid walking too
        // far when moving towards each other"), never within the 10 yd above. mgt-h 20261026-104908-mgt-h-5E-14
        // (300-480 s, "trash fight idle"): a Sunblade Warlock 24686 stood at (181.9,-5.7) for 180 s on the priest,
        // out of everyone's sight but the rogue's; the rogue's "reach melee" to (164.7,-5.5), 17 yd short of it, was
        // held 28 yd from the tank's fight spot ("waits for the mob to come") every tick, and it never came. A mob on
        // the group that has stood still STAY_CHASE_MS lets the chase toward it through (the 15 s of [b34_mgt]).
        constexpr uint32 STAY_CHASE_MS = 15 * IN_MILLISECONDS;
        constexpr float STAY_CHASE_DRIFT = 2.0f;
        if (chased->IsAlive() && onGroup && !chased->isMoving() &&
            chased->GetExactDist2d(x, y) + 1.0f < bot->GetExactDist2d(chased))
        {
            struct Stand
            {
                uint32 sinceMs;
                uint32 lastMs;
                float x;
                float y;
            };
            static std::mutex standLock;
            // [rot44] by (instance, GUID): one table for every map thread, parallel instances share creature GUIDs
            static std::map<InstanceGuid, Stand> stands;  // mob -> since when it stands there
            uint32 const nowMs = getMSTime() | 1u;
            uint32 standingMs = 0;
            {
                std::lock_guard<std::mutex> guard(standLock);
                if (stands.size() > 4096)
                    stands.clear();
                Stand& entry = stands[InstanceKey(chased)];
                if (!entry.sinceMs || getMSTimeDiff(entry.lastMs, nowMs) > 5000 ||
                    chased->GetExactDist2d(entry.x, entry.y) > STAY_CHASE_DRIFT)
                    entry = {nowMs, nowMs, chased->GetPositionX(), chased->GetPositionY()};
                entry.lastMs = nowMs;
                standingMs = getMSTimeDiff(entry.sinceMs, nowMs);
            }
            if (standingMs >= STAY_CHASE_MS)
            {
                uint32 const now = getMSTime();
                if (!_fightHoldLogMs || getMSTimeDiff(_fightHoldLogMs, now) >= 5 * IN_MILLISECONDS)
                {
                    _fightHoldLogMs = now ? now : 1;
                    LOG_INFO("playerbots", "[b47_idle] stay_chase: {}: {} to ({:.1f},{:.1f}) toward {} {}, standing "
                             "{:.0f} s on {}: not held at the fight spot", bot->GetName(), name, x, y,
                             chased->GetName(), chased->GetEntry(), standingMs / 1000.0f, victim->GetName());
                }
                return true;
            }
        }
    }

    float const dx = x - anchor.GetPositionX();
    float const dy = y - anchor.GetPositionY();
    float const out = std::sqrt(dx * dx + dy * dy);
    float const limit = tankSelf ? FightHold::RADIUS : FightHold::RADIUS + FightHold::MELEE_SLACK;
    if (out <= limit)
        return true;

    uint32 const now = getMSTime();
    bool const logDue = !_fightHoldLogMs || getMSTimeDiff(_fightHoldLogMs, now) >= 5 * IN_MILLISECONDS;
    if (logDue)
        _fightHoldLogMs = now ? now : 1;

    if (!tankSelf)
    {
        if (logDue)
            LOG_INFO("playerbots", "[fight hold] {}: {} to ({:.1f},{:.1f}) is {:.0f} yd from {}'s fight spot (over "
                     "{:.0f}): waits for the mob to come", bot->GetName(), name, x, y, out, tank->GetName(), limit);
        return false;
    }

    // to the spot's edge on the line out to the destination; a mob out there is taunted or pulled with a ranged
    // threat spell ("hand of reckoning on group member attacker", "righteous defense", ...), it comes to the tank
    float const scale = limit / out;
    float hx = anchor.GetPositionX() + dx * scale;
    float hy = anchor.GetPositionY() + dy * scale;
    float hz = anchor.GetPositionZ() + (z - anchor.GetPositionZ()) * scale;
    bot->UpdateAllowedPositionZ(hx, hy, hz);
    bool stays = bot->GetExactDist2d(hx, hy) <= sPlayerbotAIConfig.ContactDistance + 1.0f;
    // [b42_hos] Nor to an edge point that stands in an idle mob's aggro reach: the clamp moved the chase's end, so the
    // chase's own idle-pack checks (TankWalkIntoIdlePack on the mob's spot) no longer cover it. hos-h
    // 20261026-071042-hos-h-5A-21 pull 7: the patrol Dark Rune Theurgists (planned 2) cast from 13-15 yd beside an
    // idle Dark Rune pack ("[tank reach] ... idle Dark Rune Theurgist (13.4-14.5 yd from it)"); the tank's "reach
    // melee" was clamped to the fight spot's edge (1042, 667) and the fight went from 2 to 5 mobs in 3 s (292-295 s,
    // the likeliest wake), then to 8; the Prot Paladin died to Lightning Bolt / Forked Lightning in 11 s, then the
    // mage and the druid. The mob out there is taunted from the spot instead.
    // [b43_move] ... nor to one at the brink over an idle mob on the floor below that sees it (BrinkAboveIdleMob: the
    // Z gate keeps proximity aggro off, not summons, falls or casters at the edge).
    int8 brink = -1;
    if (!stays)
        for (ObjectGuid const guid : AI_VALUE(GuidVector, "possible targets no los"))
        {
            Creature* idle = botAI->GetCreature(guid);
            if (!idle || !idle->IsAlive() || idle->IsInCombat() || !idle->HasReactState(REACT_AGGRESSIVE) ||
                !bot->IsHostileTo(idle))
                continue;

            char const* const blocker = IdleAggroBlocker(idle, bot, hx, hy, hz, SIGHT_IDLE_AGGRO_MARGIN, false);
            bool const below = blocker && std::string_view(blocker) == "z gate" &&
                               BrinkAboveIdleMob(idle, bot, hx, hy, hz, SIGHT_IDLE_AGGRO_MARGIN, brink);
            if (blocker && !below)
                continue;

            stays = true;
            if (logDue && below)
                LOG_INFO("playerbots", "[b43_move] fight_hold_edge_below: {}: {} edge ({:.1f},{:.1f},{:.1f}) is the "
                         "brink over idle {} ({:.1f} yd in 2D, {:.1f} yd below, it sees the edge): stays",
                         bot->GetName(), name, hx, hy, hz, idle->GetName(), idle->GetExactDist2d(hx, hy),
                         hz - idle->GetPositionZ());
            else if (logDue)
                LOG_INFO("playerbots", "[b42_hos] fight_hold_edge_idle: {}: {} edge ({:.1f},{:.1f}) is in the aggro "
                         "reach of idle {} ({:.1f} yd): stays", bot->GetName(), name, hx, hy, idle->GetName(),
                         idle->GetExactDist(hx, hy, hz));
            break;
        }
    if (logDue)
        LOG_INFO("playerbots", "[fight hold] {}: {} to ({:.1f},{:.1f}) is {:.0f} yd from the fight spot "
                 "({:.1f},{:.1f}): {}", bot->GetName(), name, x, y, out, anchor.GetPositionX(),
                 anchor.GetPositionY(), stays ? "stays at its edge" : "walks to its edge only");
    if (stays)
        return false;

    x = hx;
    y = hy;
    z = hz;
    return true;
}

bool MovementAction::ClampToFightZone(float& x, float& y, float& z, Leader::SafeZone& zone, bool& applied)
{
    applied = false;
    if (!Leader::LEADER_SAFE_ZONE || SkipsIdleGuard() || !Leader::FightZone::Get(bot, zone))
        return true;

    std::string const name = getName();
    if (Leader::FightZone::ExemptMover(bot, name))
        return true;
    // [lead18] the clamp never overrides the leader's command for a led bot: the leader issues only safe spots, an
    // unsafe one is its bug - logged, not clamped
    if ((name == "leader move order" || name == "test runner move") && Leader::LedBot(bot))
    {
        std::string why;
        Position const here = bot->GetPosition();
        if (!zone.Contains(Position(x, y, z), Leader::FightZone::RoleOf(bot), &here, &why))
            LOG_INFO("playerbots", "[lead18] leader_spot_unsafe: {}: {} to ({:.1f},{:.1f},{:.1f}): {}",
                     bot->GetName(), name, x, y, z, why);
        return true;
    }

    applied = true;
    // the same ask again within a second: no new search (a stay stays, a clamped walk goes on to its spot)
    uint32 const now = getMSTime();
    if (_zoneVetoMs && getMSTimeDiff(_zoneVetoMs, now) < GUARD_VETO_MEMO_MS &&
        G3D::Vector3(x - _zoneVetoX, y - _zoneVetoY, z - _zoneVetoZ).length() < GUARD_VETO_MEMO_DISTANCE)
    {
        if (!_zoneClamped)
            return false;

        x = _zoneGotX;
        y = _zoneGotY;
        z = _zoneGotZ;
        return !IsDuplicateMove(x, y, z);
    }

    Leader::ZoneRole const role = Leader::FightZone::RoleOf(bot);
    Position const here = bot->GetPosition();
    Position const want(x, y, z);
    std::string why;
    if (zone.Contains(want, role, &here, &why) && zone.Standable(bot, want, &why) &&
        zone.PathInside(bot, want, role, &why))
        return true;

    // avoid aoe: no spot inside the effect it leaves
    Position const avoidCenter = _avoidCenter;
    float const avoidRadius = _guardedMove == GuardedMove::AVOID ? _avoidRadius : 0.0f;
    auto const reject = [avoidCenter, avoidRadius](Position const& p)
    { return avoidRadius > 0.0f && p.GetExactDist2d(&avoidCenter) < avoidRadius; };

    Position got;
    std::string how;
    _zoneVetoX = x;
    _zoneVetoY = y;
    _zoneVetoZ = z;
    _zoneVetoMs = now ? now : 1;
    _zoneClamped = zone.Clamp(bot, want, role, got, how, reject);
    Leader::FightZone::NoteClamp(bot, name, want, _zoneClamped ? &got : nullptr, why + " -> " + how);
    if (!_zoneClamped)
        return false;

    x = _zoneGotX = got.GetPositionX();
    y = _zoneGotY = got.GetPositionY();
    z = _zoneGotZ = got.GetPositionZ();
    return true;
}

bool MovementAction::IsPathGuarded()
{
    Map* map = bot->GetMap();
    return map && map->IsDungeon() && !bot->InBattleground() && bot->IsAlive() && bot->GetGroup() &&
           !bot->GetVehicle() && !bot->GetTransport() && !bot->IsFlying() && !bot->HasStealthAura() &&
           !bot->HasInvisibilityAura() &&
           (!PlayerbotAI::IsTank(bot) || ((bot->IsInCombat() || GuardsIdleTankWalk()) && GuardsTankWalk()) ||
            Leader::FightZone::GuardsIdleTankWalk(bot, getName())) &&  // [lead9] the harness tank between pulls
           !SkipsIdleGuard();
}

Unit* MovementAction::IdleWokenOnWalk(Movement::PointsArray const& points, ObjectGuid exempt)
{
    if (points.size() < 2 || !bot->IsInWorld())
        return nullptr;
    Player* tank = PlayerbotAI::IsTank(bot) ? bot : FindGroupTank(bot);
    std::vector<G3D::Vector3> const trail = tank ? GetTrail(tank) : std::vector<G3D::Vector3>();
    std::vector<IdleReach> const reaches =
        CollectIdleReaches(botAI, bot, AI_VALUE(GuidVector, "possible targets no los"), tank, trail, exempt,
                           bot->GetTarget(), &points.back());
    if (reaches.empty())
        return nullptr;
    return FirstWake(points, reaches, bot).mob;
}

std::string MovementAction::IdleWakeOnWalk(Movement::PointsArray const& points, ObjectGuid exempt)
{
    if (points.size() < 2 || !bot->IsInWorld())
        return "";

    Player* tank = PlayerbotAI::IsTank(bot) ? bot : FindGroupTank(bot);
    std::vector<G3D::Vector3> const trail = tank ? GetTrail(tank) : std::vector<G3D::Vector3>();
    std::vector<IdleReach> const reaches =
        CollectIdleReaches(botAI, bot, AI_VALUE(GuidVector, "possible targets no los"), tank, trail, exempt,
                           bot->GetTarget(), &points.back());  // [b40_open]
    if (reaches.empty())
        return "";

    PathWake const wake = FirstWake(points, reaches, bot);
    if (!wake.mob)
        return "";

    return Acore::StringFormat("{} ({:.1f} yd of reach {:.1f}, {:.1f} yd along)", wake.mob->GetName(), wake.distance,
                               wake.reach, wake.walked);
}

namespace
{
    constexpr float SIGHT_ARC_FOCUS_RANGE = 45.0f;  // the fight a spot this close is meant to see ([sight-arc])

    // The fight a ranged / healer spot at (x, y, z) is meant to see: the bot's own target, else the tank's, else the
    // tank (a healer's watch). Null when none is alive on the map within SIGHT_ARC_FOCUS_RANGE of the spot.
    Unit* SightArcFocus(PlayerbotAI* botAI, Player* bot, float x, float y, float z, Player* tank)
    {
        std::array<Unit*, 3> const candidates = {
            PlayerbotAI::IsHeal(bot) ? nullptr : botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get(),
            tank && !PlayerbotAI::IsHeal(bot) ? tank->GetVictim() : nullptr, tank};
        for (Unit* unit : candidates)
        {
            if (unit && unit != bot && unit->IsAlive() && unit->IsInWorld() && unit->GetMap() == bot->GetMap() &&
                unit->GetExactDist(x, y, z) <= SIGHT_ARC_FOCUS_RANGE)
                return unit;
        }

        return nullptr;
    }

    // One line per bot and log of the sight walk every SIGHT_LOG_MS: the guard runs on every replan of a walk.
    constexpr uint32 SIGHT_LOG_MS = 5 * IN_MILLISECONDS;
    bool SightLogDue(Player* bot, std::unordered_map<ObjectGuid, uint32>& logged)
    {
        uint32 const now = getMSTime();
        uint32& last = logged[bot->GetGUID()];
        if (last && getMSTimeDiff(last, now) < SIGHT_LOG_MS)
            return false;

        last = now ? now : 1;
        return true;
    }
}

bool MovementAction::GuardIdleMobPath(float& x, float& y, float& z, bool straight)
{
    uint32 const now = getMSTime();
    if (_vetoMs && getMSTimeDiff(_vetoMs, now) < GUARD_VETO_MEMO_MS &&
        G3D::Vector3(x - _vetoX, y - _vetoY, z - _vetoZ).length() < GUARD_VETO_MEMO_DISTANCE)
        return false;

    // A guarded tank (a chase in combat) is its own tank: the mobs it passed idle on its trail let it by again.
    bool const selfTank = PlayerbotAI::IsTank(bot);
    Player* tank = selfTank ? bot : FindGroupTank(bot);
    if (tank && !selfTank && !tank->GetTransport() && !tank->HasUnitState(UNIT_STATE_IN_FLIGHT))
        RecordTrail(tank);

    // A follow of the tank: a leg of its trail, a walk to it, or (out of combat) a spot next to it (the formation).
    bool const followsTank =
        tank && (_guardedMove == GuardedMove::FOLLOW ||
                 (_guardedMove == GuardedMove::REACH && _guardTarget == tank->GetGUID()) ||
                 (_guardedMove == GuardedMove::GENERIC && !bot->IsInCombat() &&
                  tank->GetExactDist(x, y, z) <= FOLLOW_NEAR_TANK));
    std::vector<G3D::Vector3> const trail = tank ? GetTrail(tank) : std::vector<G3D::Vector3>();
    ObjectGuid const moveTarget = _guardedMove == GuardedMove::REACH ? _guardTarget : ObjectGuid::Empty;
    G3D::Vector3 const walkEnd(x, y, z);  // [b40_open]
    // [b43_move] every idle reach once; the asked walk and each alternative below keep their own 12 (RankIdleReaches)
    std::vector<IdleReach> const allReaches =
        CollectIdleReaches(botAI, bot, AI_VALUE(GuidVector, "possible targets no los"), tank, trail, moveTarget,
                           bot->GetTarget(), &walkEnd, true);
    if (allReaches.empty())
    {
        if (followsTank)
            ClearFollowVeto(bot);

        return true;
    }

    float const eye = bot->GetCollisionHeight();
    Movement::PointsArray const path = GuardPath(bot, x, y, z, straight);
    // ranked along the walked path ([b40_open] took the straight line; it stays the fallback with no path)
    Movement::PointsArray const straightWalk = {
        G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()), walkEnd};
    std::vector<IdleReach> const reaches = RankIdleReaches(allReaches, path.size() >= 2 ? path : straightWalk);
    PathWake const wake = FirstWake(path, reaches, bot);
    if (!wake.mob)
    {
        if (followsTank)
            ClearFollowVeto(bot);

        return true;
    }
    s_moveMob = wake.mob->GetGUID();  // [lead372]

    char const* kind = _guardedMove == GuardedMove::FOLLOW || !bot->IsInCombat() ? "follow path" : "combat path";
    if (followsTank)
    {
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        G3D::Vector3 const tankPos(tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
        TrailLeg leg = NextTrailLeg(trail, here, tankPos, sPlayerbotAIConfig.FollowDistance);

        // [sight-trail-back] Sunken Temple 2026-10-25 (st-n-5A-92, pull 12): "[follow path] Rlmagbc: move to ranged
        // position ... would wake Deep Lurker (19.8 yd of reach 21.0) -> trail (-553.0,106.1)": the mage's walk to a
        // sight spot by the fight was swapped for a trail point 25 yd back, and the fight went on without it. A walk
        // that is no follow (a sight / ranged spot, a reach) takes the trail only when its leg does not lead away
        // from where the bot was going; else the other spots below (the arc round the fight) are tried.
        if (leg.found && _guardedMove != GuardedMove::FOLLOW)
        {
            float const askedFromHere = G3D::Vector3(x - here.x, y - here.y, z - here.z).length();
            float const askedFromLeg =
                G3D::Vector3(x - leg.goal.x, y - leg.goal.y, z - leg.goal.z).length();
            thread_local std::unordered_map<ObjectGuid, uint32> trailBackLogged;
            if (askedFromLeg > askedFromHere + TRAIL_BACK_SLACK)
            {
                if (SightLogDue(bot, trailBackLogged))
                    LOG_INFO("playerbots",
                             "[sight-trail-back] {}: {} to ({:.1f},{:.1f},{:.1f}) keeps off the trail leg "
                             "({:.1f},{:.1f},{:.1f}): {:.1f} yd from the goal against {:.1f} from here",
                             bot->GetName(), getName(), x, y, z, leg.goal.x, leg.goal.y, leg.goal.z, askedFromLeg,
                             askedFromHere);
                leg = TrailLeg();
            }
        }

        // On the tank's trail: its next leg, walked on the ground the tank walked. A mob some trail point stands in
        // the reach of let the tank by and lets the follower by too; only one the tank never came near still vetoes
        // (a patrol that walked in since).
        if (leg.found && leg.onTrail)
        {
            Movement::PointsArray const legPath = GuardPath(bot, leg.goal.x, leg.goal.y, leg.goal.z, straight);
            if (legPath.size() >= 2 && (legPath.back() - leg.goal).length() <= GUARD_MIN_SHORTER &&
                PathOnTrail(legPath, here, trail, leg.from, leg.to, TRAIL_JOIN))
            {
                std::vector<IdleReach> unpassed;
                for (IdleReach const& idle : RankIdleReaches(allReaches, legPath))  // [b43_move] the leg's own 12
                {
                    bool passed = false;
                    for (G3D::Vector3 const& point : trail)
                    {
                        if (idle.mob->GetExactDist(point.x, point.y, point.z) <
                            MovementAction::IdleAggroReach(idle.mob, bot, 0.0f))
                        {
                            passed = true;
                            break;
                        }
                    }

                    if (!passed)
                        unpassed.push_back(idle);
                }

                if (!FirstWake(legPath, unpassed, bot).mob)
                {
                    LogGuardVeto(bot, kind, getName(), x, y, z, wake,
                                 Acore::StringFormat("trail ({:.1f},{:.1f},{:.1f})", leg.goal.x, leg.goal.y,
                                                     leg.goal.z));
                    x = leg.goal.x;
                    y = leg.goal.y;
                    z = leg.goal.z;
                    ClearFollowVeto(bot);
                    return true;
                }
            }
        }

        // Deadlock breaker: a follower vetoed for DEADLOCK_VETO_MS in a row while its tank is out of combat and
        // far ahead (or for DEADLOCK_HARD_MS whatever the tank does) goes: along the trail when it has one, else as
        // asked. The guard never leaves a member behind for good (RFC 2026-10-24: the priest stood 60 yd behind
        // while the harness walked the tank back and forth to fetch it).
        // [lead9] In a harness group the breaker is off: a follow or a regroup never walks into an idle mob's reach
        // (user, step 9); the member takes another spot below or stays, and the leader regroups (MeetMember).
        bool breaking = false;
        uint32 const vetoedMs = FollowVetoAge(bot, now, breaking);
        if (bot->GetExactDist(tank) > DEADLOCK_TANK_DISTANCE && !Leader::FightZone::Applies(bot) &&
            ((vetoedMs >= DEADLOCK_VETO_MS && !tank->IsInCombat()) || vetoedMs >= DEADLOCK_HARD_MS))
        {
            std::string outcome = Acore::StringFormat("allowed: deadlock (vetoed {} s)", vetoedMs / IN_MILLISECONDS);
            if (leg.found)
                outcome += Acore::StringFormat(" -> {} ({:.1f},{:.1f},{:.1f})", leg.onTrail ? "trail" : "onto trail",
                                               leg.goal.x, leg.goal.y, leg.goal.z);

            LogGuardVeto(bot, kind, getName(), x, y, z, wake, outcome, !breaking);
            if (leg.found)
            {
                x = leg.goal.x;
                y = leg.goal.y;
                z = leg.goal.z;
            }

            NoteFollowVeto(bot, now, true);
            return true;
        }
    }

    // Another destination: one whose walk wakes nothing and is not much longer than the asked one.
    float const maxWalk = PathLength(path) * GUARD_ALT_DETOUR + GUARD_ALT_DETOUR_ADD;
    uint32 paths = GUARD_MAX_ALT_PATHS;
    G3D::Vector3 alt;
    auto const safe = [&](float altX, float altY, float altZ) -> bool
    {
        if (!paths)
            return false;

        --paths;
        Movement::PointsArray const altPath = GuardPath(bot, altX, altY, altZ, straight);
        if (altPath.size() < 2 || PathLength(altPath) > maxWalk ||
            (altPath.back() - G3D::Vector3(altX, altY, altZ)).length() > GUARD_MIN_SHORTER ||
            FirstWake(altPath, RankIdleReaches(allReaches, altPath), bot).mob)  // [b43_move] its own 12
            return false;

        alt = altPath.back();
        return true;
    };

    // Next to the tank, on the bot's side of it: where a melee waits and a fleeing bot is safe.
    auto const tankSide = [&]() -> bool
    {
        if (!tank || selfTank)
            return false;

        float const angle = tank->GetAngle(bot);
        float sideX = tank->GetPositionX() + std::cos(angle) * GUARD_TANK_SIDE;
        float sideY = tank->GetPositionY() + std::sin(angle) * GUARD_TANK_SIDE;
        float sideZ = tank->GetPositionZ();
        bot->UpdateAllowedPositionZ(sideX, sideY, sideZ);
        // [b35_brd2] by the tank means on its floor: a side spot off its ledge (the floor below) is none.
        if (std::fabs(sideZ - tank->GetPositionZ()) > SHORTER_LEVEL_DZ)
            return false;

        return bot->GetExactDist(sideX, sideY, sideZ) >= GUARD_MIN_SHORTER && safe(sideX, sideY, sideZ);
    };

    // The asked walk up to just before the reach it would enter.
    // [b33_move] ... and SHORTER_IDLE_MARGIN clear of every idle hostile's reach: the stop moves back along the walk
    // until it is; with none the bot holds (no "alt shorter" at a pack's edge).
    auto const shorter = [&]() -> bool
    {
        float stop = wake.walked - GUARD_STOP_BACK;
        if (stop < GUARD_MIN_SHORTER)
            return false;

        std::vector<ShorterKeep> const keeps =
            CollectShorterKeeps(botAI, bot, AI_VALUE(GuidVector, "possible targets no los"), tank, moveTarget,
                                bot->GetTarget());
        Creature* tooNear = nullptr;
        // [b35_brd2] ... on the bot's or the asked floor (no stop below a ledge).
        float const lowZ = std::min(bot->GetPositionZ(), z);
        float const highZ = std::max(bot->GetPositionZ(), z);
        bool offLevel = false;
        float offLevelZ = 0.0f;
        for (; stop >= GUARD_MIN_SHORTER; stop -= SHORTER_BACK_STEP)
        {
            float badZ = 0.0f;
            if (StopLeavesLevel(path, stop, lowZ, highZ, badZ))
            {
                if (!offLevel)
                    offLevelZ = badZ;

                offLevel = true;
                continue;
            }

            G3D::Vector3 const point = PointAlong(path, stop);
            Creature* const mob = ShorterStopTooNear(keeps, bot, point);
            if (!mob)
            {
                alt = point;
                return true;
            }

            tooNear = tooNear ? tooNear : mob;
        }

        thread_local std::unordered_map<ObjectGuid, uint32> marginLogged;
        if (tooNear && SightLogDue(bot, marginLogged))
            LOG_INFO("playerbots",
                     "[b33_move] {}: {} to ({:.1f},{:.1f},{:.1f}): no shorter stop keeps {:.0f} yd beyond {}'s reach "
                     "-> no alt shorter",
                     bot->GetName(), getName(), x, y, z, SHORTER_IDLE_MARGIN, tooNear->GetName());

        thread_local std::unordered_map<ObjectGuid, uint32> levelLogged;
        if (offLevel && SightLogDue(bot, levelLogged))
            LOG_INFO("playerbots",
                     "[b35_brd2] {}: {} to ({:.1f},{:.1f},{:.1f}): the walk leaves the floor (z {:.1f}, bot {:.1f}, "
                     "asked {:.1f}, band {:.0f} yd) -> no alt shorter on another level",
                     bot->GetName(), getName(), x, y, z, offLevelZ, bot->GetPositionZ(), z, SHORTER_LEVEL_DZ);
        return false;
    };

    bool const melee = PlayerbotAI::IsMelee(bot);
    std::string outcome;
    if (_guardedMove == GuardedMove::REACH)
    {
        // Another spot on the same ring round the target: the group's side (the tank's) first, then the bot's.
        Unit* target = botAI->GetUnit(_guardTarget);
        if (target && target->GetMap() == bot->GetMap())
        {
            float const radius = std::max(1.0f, _guardRange - GUARD_RING_INSIDE);
            float const fromBot = target->GetAngle(bot);
            std::vector<float> angles;
            if (tank)
                angles.push_back(target->GetAngle(tank));

            for (float const turns : { 1.0f, -1.0f, 2.0f, -2.0f })
                angles.push_back(fromBot + turns * GUARD_RING_STEP);

            for (float const angle : angles)
            {
                if (!paths)
                    break;

                float ringX = target->GetPositionX() + std::cos(angle) * radius;
                float ringY = target->GetPositionY() + std::sin(angle) * radius;
                float ringZ = target->GetPositionZ();
                bot->UpdateAllowedPositionZ(ringX, ringY, ringZ);
                if (target->IsWithinLOS(ringX, ringY, ringZ + eye) && safe(ringX, ringY, ringZ))
                {
                    outcome = "alt ring";
                    break;
                }
            }
        }

        if (outcome.empty() && melee && tankSide())
            outcome = "alt by tank";

        if (outcome.empty() && shorter())
            outcome = "alt shorter";
    }
    else if (_guardedMove == GuardedMove::AVOID && _avoidRadius > 0.0f)
    {
        // Out of the ground effect another way: a spot just past its edge whose walk wakes nothing, even if it
        // leaves the effect worse for the fight (scholo-h 2026-10-25 pull 8: "avoid aoe ... would wake Scholomance
        // Necromancer -> allow (ground effect)", the pull grew to 18 mobs); else next to the tank; a shorter stop
        // only when it ends out of the effect.
        std::vector<G3D::Vector3> spots;
        float const ring = _avoidRadius + GUARD_AVOID_EDGE;
        for (uint32 i = 0; i < GUARD_AVOID_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / GUARD_AVOID_DIRECTIONS;
            float spotX = _avoidCenter.GetPositionX() + std::cos(angle) * ring;
            float spotY = _avoidCenter.GetPositionY() + std::sin(angle) * ring;
            float spotZ = bot->GetPositionZ();
            bot->UpdateAllowedPositionZ(spotX, spotY, spotZ);
            if (std::fabs(spotZ - bot->GetPositionZ()) <= GUARD_AVOID_EDGE * 2.0f)
                spots.emplace_back(spotX, spotY, spotZ);
        }
        std::sort(spots.begin(), spots.end(),
                  [this](G3D::Vector3 const& a, G3D::Vector3 const& b)
                  { return bot->GetExactDist(a.x, a.y, a.z) < bot->GetExactDist(b.x, b.y, b.z); });
        paths = GUARD_AVOID_MAX_PATHS;
        for (G3D::Vector3 const& spot : spots)
        {
            if (!paths)
                break;
            if (bot->GetExactDist(spot.x, spot.y, spot.z) >= GUARD_MIN_SHORTER && safe(spot.x, spot.y, spot.z))
            {
                outcome = "alt out of the effect";
                break;
            }
        }

        if (outcome.empty())
        {
            paths = 1;
            if (tankSide())
                outcome = "alt by tank";
        }

        if (outcome.empty() && shorter() &&
            _avoidCenter.GetExactDist2d(alt.x, alt.y) >= _avoidRadius)
            outcome = "alt shorter";
    }
    else if (_guardedMove == GuardedMove::FLEE || _guardedMove == GuardedMove::AVOID ||
             (_guardedMove == GuardedMove::GENERIC && melee && bot->IsInCombat()))
    {
        if (tankSide())
            outcome = "alt by tank";
        else if (shorter())
            outcome = "alt shorter";
    }
    else
    {
        // [sight-arc] A ranged / healer walk of its own (a sight spot round a corner, a ranged spot) another way:
        // a spot on the arc round the fight at the asked distance, the group's side first, that sees the fight and
        // whose walk wakes nothing; only then a shorter stop.
        Unit* fight = SightArcFocus(botAI, bot, x, y, z, tank);
        if (_guardedMove == GuardedMove::GENERIC && fight && (PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot)))
        {
            float const radius = fight->GetExactDist(x, y, z);
            float const asked = fight->GetAngle(x, y);
            for (float const turns : { 1.0f, -1.0f, 2.0f, -2.0f })
            {
                if (!paths)
                    break;

                float const angle = asked + turns * GUARD_RING_STEP;
                float arcX = fight->GetPositionX() + std::cos(angle) * radius;
                float arcY = fight->GetPositionY() + std::sin(angle) * radius;
                float arcZ = fight->GetPositionZ();
                bot->UpdateAllowedPositionZ(arcX, arcY, arcZ);
                if (fight->IsWithinLOS(arcX, arcY, arcZ + eye) && safe(arcX, arcY, arcZ))
                {
                    outcome = "alt arc";
                    thread_local std::unordered_map<ObjectGuid, uint32> arcLogged;
                    if (SightLogDue(bot, arcLogged))
                        LOG_INFO("playerbots", "[sight-arc] {}: {} round {} {:.0f} deg off the asked spot ({:.1f} yd)",
                                 bot->GetName(), getName(), fight->GetName(), turns * GUARD_RING_STEP * 180.0f / M_PI,
                                 radius);
                    break;
                }
            }
        }

        if (outcome.empty() && _guardedMove != GuardedMove::STEP && shorter())
            outcome = "alt shorter";
    }

    if (followsTank)
        NoteFollowVeto(bot, now, false);  // a shorter stop or another spot is no way to the tank either

    if (!outcome.empty())
    {
        LogGuardVeto(bot, kind, getName(), x, y, z, wake,
                     Acore::StringFormat("{} ({:.1f},{:.1f},{:.1f})", outcome, alt.x, alt.y, alt.z));
        s_moveSubstitute = "GuardIdleMobPath (" + outcome + ": the walk would wake " + wake.mob->GetName() + ")";
        x = alt.x;
        y = alt.y;
        z = alt.z;
        return true;
    }

    // Out of a ground effect with no safe way: the move as asked once staying in it gets lethal (the healer
    // keeps a healthy bot up in it; a woken pack kills the group).
    if (_guardedMove == GuardedMove::AVOID && _avoidRadius > 0.0f &&
        bot->GetHealthPct() > GUARD_AVOID_STAY_HEALTH_PCT)
    {
        LogGuardVeto(bot, kind, getName(), x, y, z, wake, "stay (ground effect, health ok)");
        _vetoX = x;
        _vetoY = y;
        _vetoZ = z;
        _vetoMs = now ? now : 1;
        return false;
    }

    if (_guardedMove == GuardedMove::AVOID)
    {
        LogGuardVeto(bot, kind, getName(), x, y, z, wake, "allow (ground effect)");
        return true;
    }

    // [combat_path_pack_on_way] A tank in combat whose walk to its target finds no other way (no spot on the ring,
    // no shorter stop): the idle pack sits on the only way. MgT heroic 2026-10-25 (20261025-134911-mgt-h-5A-82):
    // "reach melee to (252.2,-13.0,1.7) would wake Wretched Bruiser ... -> stay" for minutes, Selin engaged with no
    // victim, the group in her zone-wide combat; combat_no_damage_release lifted every other hold but not this one.
    // User rule: a pack on the only way is never skipped. Once the veto held PACK_ON_WAY_ENGAGE_MS with nothing on
    // the tank, or while the group's no-damage release is on, the tank pulls that pack: its ranged pull by sight,
    // else the asked walk (the body pull). A pack with another way round keeps the veto (an "alt" above).
    if (selfTank && bot->IsInCombat() &&
        (_guardedMove == GuardedMove::REACH || _guardedMove == GuardedMove::GENERIC))
    {
        constexpr uint32 PACK_ON_WAY_ENGAGE_MS = 8 * IN_MILLISECONDS;
        constexpr uint32 PACK_ON_WAY_GAP_MS = 3 * IN_MILLISECONDS;  // a longer break starts the clock anew
        constexpr float PACK_ON_WAY_MIN_HEALTH_PCT = 50.0f;
        // Once the body pull is decided, the replans of the walk go on for this long (no veto in between).
        constexpr uint32 PACK_ON_WAY_BODY_MS = 10 * IN_MILLISECONDS;
        struct OnWay
        {
            ObjectGuid mob;
            uint32 sinceMs = 0;
            uint32 lastMs = 0;
            uint32 bodyUntilMs = 0;
        };
        thread_local std::unordered_map<ObjectGuid, OnWay> onWay;
        OnWay& held = onWay[bot->GetGUID()];
        if (held.bodyUntilMs && held.mob == wake.mob->GetGUID() && int32(held.bodyUntilMs - now) > 0)
        {
            LogGuardVeto(bot, kind, getName(), x, y, z, wake, "allowed: body pull of the pack on the way");
            return true;
        }
        held.bodyUntilMs = 0;
        if (held.mob != wake.mob->GetGUID() || !held.lastMs || getMSTimeDiff(held.lastMs, now) > PACK_ON_WAY_GAP_MS)
        {
            held.mob = wake.mob->GetGUID();
            held.sinceMs = now ? now : 1;
        }
        held.lastMs = now ? now : 1;

        bool const released = GroupDamage::NoDamageRelease(bot);
        uint32 const heldMs = getMSTimeDiff(held.sinceMs, now);
        if (bot->GetHealthPct() >= PACK_ON_WAY_MIN_HEALTH_PCT &&
            (released || (heldMs >= PACK_ON_WAY_ENGAGE_MS && bot->getAttackers().empty())))
        {
            std::string const why = released ? std::string("no-damage release")
                                             : Acore::StringFormat("vetoed {} s, nothing on the tank",
                                                                   heldMs / IN_MILLISECONDS);
            // The ranged pull by sight first (the class's pull spell, as the chain pull picks it).
            if (PullStrategy* strategy = PullStrategy::Get(botAI);
                strategy && bot->IsWithinLOSInMap(wake.mob) && !strategy->HasTarget() &&
                !strategy->IsPullPendingToStart() && !strategy->HasPullStarted())
            {
                Value<ObjectGuid>* pullTarget = context->GetValue<ObjectGuid>("pull strategy target");
                if (pullTarget)
                    pullTarget->Set(wake.mob->GetGUID());
                bool const usable = strategy->CanDoPullAction(wake.mob);
                std::string const spell = strategy->GetSpellName();
                if (pullTarget)
                    pullTarget->Set(ObjectGuid::Empty);

                ObjectGuid const selected = bot->GetTarget();
                if (usable && !spell.empty() && botAI->CanCastSpell(spell, wake.mob) &&
                    botAI->CastSpell(spell, wake.mob))
                {
                    if (!selected.IsEmpty())
                        bot->SetSelection(selected);
                    LogGuardVeto(bot, kind, getName(), x, y, z, wake,
                                 Acore::StringFormat("pull the pack on the way: {} by sight ({})", spell, why), true);
                    held = OnWay();
                    _vetoX = x;
                    _vetoY = y;
                    _vetoZ = z;
                    _vetoMs = now ? now : 1;
                    return false;  // the pack comes to the tank
                }
            }

            LogGuardVeto(bot, kind, getName(), x, y, z, wake,
                         Acore::StringFormat("pull the pack on the way: body pull, the walk as asked ({})", why), true);
            held.bodyUntilMs = (now + PACK_ON_WAY_BODY_MS) ? now + PACK_ON_WAY_BODY_MS : 1;
            return true;
        }
    }

    LogGuardVeto(bot, kind, getName(), x, y, z, wake, "stay");
    _vetoX = x;
    _vetoY = y;
    _vetoZ = z;
    _vetoMs = now ? now : 1;
    return false;
}

namespace
{
    constexpr float DOOR_STOP_BACK = 2.0f;  // a walk a closed door stops ends this far before it
    constexpr float DOOR_MIN_WALK = 1.0f;   // a stop that walks less than this is no move: the bot stays
    constexpr uint32 DOOR_LOG_MS = 5 * IN_MILLISECONDS;
    constexpr uint32 DOOR_VETO_MEMO_MS = IN_MILLISECONDS;
    constexpr float DOOR_VETO_MEMO_DISTANCE = 2.0f;

    // One line per bot every DOOR_LOG_MS, with how many were not logged since.
    void LogDoorVeto(Player* bot, std::string const& action, float x, float y, float z, ClosedDoorHit const& hit,
                     std::string const& outcome)
    {
        struct Throttle
        {
            uint32 lastMs = 0;
            uint32 skipped = 0;
        };

        thread_local std::unordered_map<ObjectGuid, Throttle> throttles;
        Throttle& throttle = throttles[bot->GetGUID()];
        uint32 const now = getMSTime();
        if (throttle.lastMs && getMSTimeDiff(throttle.lastMs, now) < DOOR_LOG_MS)
        {
            ++throttle.skipped;
            return;
        }

        LOG_INFO("playerbots", "[closed door] {}: {} to ({:.1f},{:.1f},{:.1f}) runs through {} {} at {:.1f} yd -> {}{}",
                 bot->GetName(), action, x, y, z, hit.door->GetName(), hit.door->GetEntry(), hit.walked, outcome,
                 throttle.skipped ? " (+" + std::to_string(throttle.skipped) + " not logged)" : std::string());
        throttle.lastMs = now ? now : 1;
        throttle.skipped = 0;
    }

    // Who opens a door that opens by use: a bot alone, its group's leader, or - under a leader who is no bot (a real
    // player decides himself) - the main tank. One opener per group: a second click on the open door would close it.
    bool IsDoorOpener(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group || group->IsLeader(bot->GetGUID()))
            return true;

        Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
        return !(leader && GET_PLAYERBOT_AI(leader)) && PlayerbotAI::IsMainTank(bot);
    }

    // [kara3] A door that closes again by itself (its template's auto-close time) is opened with the group at it:
    // who of the group stands on the opener's side of it and is not yet within DOOR_GATHER_RADIUS of it, or still
    // casting there, is waited for. Members on the far side are the ones the opening is for; members farther than
    // DOOR_GATHER_FAR are no part of this walk. Empty: open it.
    // b356 kara-n 20261108-234616-10A-1: the leader's own walk clicked the Gatehouse Door 183450 (3 s) with the
    // warlock casting Create Soulstone and the druid 15 yd back - both shut out again, the door cut's rescue clicks
    // brought them through (0 door gathers: the harness's DoorGatherHolds only guards the leader's waypoint).
    std::string DoorGatherWait(Player* opener, GameObject* door)
    {
        constexpr float DOOR_GATHER_RADIUS = 8.0f;
        constexpr float DOOR_GATHER_FAR = 45.0f;
        GameObjectTemplate const* info = door->GetGOInfo();
        Group* group = opener->GetGroup();
        if (!group || !info || !info->GetAutoCloseTime())
            return "";
        float const fx = std::cos(door->GetOrientation());
        float const fy = std::sin(door->GetOrientation());
        auto const side = [&](WorldObject const* who)
        {
            return (who->GetPositionX() - door->GetPositionX()) * fx +
                   (who->GetPositionY() - door->GetPositionY()) * fy;
        };
        bool const openerSide = side(opener) >= 0.0f;
        std::string wait;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == opener || !member->IsAlive() || member->GetMap() != opener->GetMap() ||
                (side(member) >= 0.0f) != openerSide || member->GetExactDist(door) > DOOR_GATHER_FAR)
                continue;
            if (member->GetExactDist(door) > DOOR_GATHER_RADIUS)
                wait += (wait.empty() ? "" : ",") + std::string(member->GetName()) +
                        Acore::StringFormat(" ({:.0f} yd)", member->GetExactDist(door));
            else if (member->IsNonMeleeSpellCast(false))
                wait += (wait.empty() ? "" : ",") + std::string(member->GetName()) + " (casting)";
        }
        return wait;
    }
}

bool MovementAction::GuardClosedDoor(float& x, float& y, float& z)
{
    uint32 const now = getMSTime();
    // The cheap gate: no closed door near the straight line, no path is built. [b34_ss] Before the veto memo: a door
    // that opened since the last "stay" lets the walk go at once - Stratholme's key gates stand open 3 s, the memo
    // (1 s) ate a third of that (strat-n 20261026-014521-5C-2: the healer 0.7 yd from King's Square Gate 175353
    // missed five openings, "own path refused" at 2102 s with the gate open).
    if (!ClosedDoorNear(bot, bot->GetPositionX(), bot->GetPositionY(), x, y, CLOSED_DOOR_SEARCH))
        return true;

    if (_doorVetoMs && getMSTimeDiff(_doorVetoMs, now) < DOOR_VETO_MEMO_MS &&
        G3D::Vector3(x - _doorVetoX, y - _doorVetoY, z - _doorVetoZ).length() < DOOR_VETO_MEMO_DISTANCE)
        return false;

    Movement::PointsArray const path = GuardPath(bot, x, y, z, bot->isSwimming());
    ClosedDoorHit const hit = PathThroughClosedDoor(bot, path);
    if (!hit.Found())
        return true;

    // A door a player opens by a click (TerrainHazard.h DoorOpensByUse: Scholomance's Iron Gates): the group's opener
    // walks up to it and uses it, out of combat; everyone else stops before it until it is open. An opened door is
    // no barrier any more, the walk is judged again for the next one.
    bool opener = DoorOpensByUse(hit.door) && IsDoorOpener(bot);
    if (opener)
        if (std::string const wait = DoorGatherWait(bot, hit.door); !wait.empty())
        {
            // [kara3] it stands at the door, the group comes to it (they follow it); no click yet
            opener = false;
            LogDoorVeto(bot, getName(), x, y, z, hit, "[kara3] door gather: waits for " + wait);
        }
    if (opener && OpenDoorByUse(bot, hit.door))
        return GuardClosedDoor(x, y, z);

    float const stop = hit.walked - (opener ? DOOR_OPEN_BACK : DOOR_STOP_BACK);
    // [b35_sh] A goal far below the bot (another level) whose path runs round through a closed door: the walk to the
    // door is no progress unless it ends nearer the goal - the bot stays. sh-n 20261026-024524-sh-n-5H-30: the healer
    // stood on the rim of the sewer jump (route link, 123.0,249.8,-15.4, a 30 yd drop past the closed Grand Warlock
    // Chamber Door 182539); her "move to ranged position" / "follow" to the tank below ran through the door, was cut
    // "shorter" to 2 yd before it, and walked her 18 yd back from the rim three times (554-572 s) before the harness
    // jump could start. She stood at the door 65-70 yd from the fight, 100 % mana, and the group died unhealed.
    constexpr float DOOR_GOAL_BELOW_DZ = 8.0f;  // the goal this far below the bot: another level
    constexpr float DOOR_GOAL_GAIN = 1.0f;      // the cut walk must end this much nearer the goal
    bool towardGoal = true;
    if (stop >= DOOR_MIN_WALK && bot->GetPositionZ() - z > DOOR_GOAL_BELOW_DZ)
        towardGoal = (PointAlong(path, stop) - G3D::Vector3(x, y, z)).length() + DOOR_GOAL_GAIN <
                     bot->GetExactDist(x, y, z);
    if (stop >= DOOR_MIN_WALK && towardGoal)
    {
        G3D::Vector3 const shorter = PointAlong(path, stop);
        LogDoorVeto(bot, getName(), x, y, z, hit,
                    Acore::StringFormat("shorter ({:.1f},{:.1f},{:.1f})", shorter.x, shorter.y, shorter.z));
        s_moveSubstitute = "GuardClosedDoor (stops before a closed door: see the [closed door] lines)";  // [lead372]
        x = shorter.x;
        y = shorter.y;
        z = shorter.z;
        return true;
    }

    LogDoorVeto(bot, getName(), x, y, z, hit,
                towardGoal ? "stay" : "stay [b35_sh] (the door is no nearer the goal below)");
    _doorVetoX = x;
    _doorVetoY = y;
    _doorVetoZ = z;
    _doorVetoMs = now ? now : 1;
    return false;
}

bool MovementAction::FollowTrail(Player* tank, float distance)
{
    // A follow movement left from before keeps chasing the tank on the shortest path: the trail replaces it.
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
    }

    RecordTrail(tank);
    std::vector<G3D::Vector3> const trail = GetTrail(tank);
    G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    G3D::Vector3 const tankPos(tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
    MovementPriority const priority =
        bot->IsInCombat() ? MovementPriority::MOVEMENT_COMBAT : MovementPriority::MOVEMENT_NORMAL;

    // The trail point nearest to the bot on its floor.
    size_t nearest = trail.size();
    float nearestDistance = 0.0f;
    for (size_t i = 0; i < trail.size(); ++i)
    {
        if (std::fabs(trail[i].z - here.z) > TRAIL_FLOOR)
            continue;

        float const distanceTo = (trail[i] - here).length();
        if (nearest == trail.size() || distanceTo < nearestDistance)
        {
            nearest = i;
            nearestDistance = distanceTo;
        }
    }

    // No trail to walk (the tank just arrived, or another floor): toward the tank, to the follow distance.
    if (nearest == trail.size() || nearest + 1 >= trail.size())
        return MoveTo(tank, distance, priority);

    // The trail left from each point to the tank.
    std::vector<float> toTank(trail.size());
    toTank.back() = (tankPos - trail.back()).length();
    for (size_t i = trail.size() - 1; i-- > 0;)
        toTank[i] = toTank[i + 1] + (trail[i + 1] - trail[i]).length();

    G3D::Vector3 goal;
    if (nearestDistance > TRAIL_JOIN)
    {
        if (toTank[nearest] < distance)
            return MoveTo(tank, distance, priority);

        goal = trail[nearest];
    }
    else
    {
        // As far along as one leg, the follow distance short of the tank, and the points it skips near its line.
        size_t reach = nearest;
        float walked = 0.0f;
        for (size_t j = nearest + 1; j < trail.size(); ++j)
        {
            walked += (trail[j] - trail[j - 1]).length();
            if (walked > TRAIL_LEG || toTank[j] < distance)
                break;

            G3D::Vector3 const line = trail[j] - here;
            float const lineSq = line.squaredLength();
            bool inCorridor = true;
            for (size_t k = nearest; k < j && inCorridor; ++k)
            {
                float along = 0.0f;
                if (lineSq > 0.0f)
                    along = std::clamp((trail[k] - here).dot(line) / lineSq, 0.0f, 1.0f);

                inCorridor = (trail[k] - (here + line * along)).length() <= TRAIL_CORRIDOR;
            }

            if (!inCorridor)
                break;

            reach = j;
        }

        if (reach != nearest)
            goal = trail[reach];
        else if (toTank[nearest] < distance)
            return false;  // as far along the trail as the follow distance allows: it stays behind the tank
        else if (nearestDistance >= TRAIL_STEP)
            goal = trail[nearest];  // onto the trail first
        else if (toTank[nearest + 1] >= distance)
            goal = trail[nearest + 1];  // a long gap in the trail: one point at a time
        else
            return false;
    }

    GuardedMove const guarded = _guardedMove;
    _guardedMove = GuardedMove::FOLLOW;
    bool const moved = MoveTo(tank->GetMapId(), goal.x, goal.y, goal.z, false, false, false, false, priority, true);
    _guardedMove = guarded;
    return moved;
}

bool MovementAction::MoveToStep(uint32 mapId, float x, float y, float z, bool exact, bool backwards)
{
    GuardedMove const guarded = _guardedMove;
    _guardedMove = GuardedMove::STEP;
    bool const moved =
        MoveTo(mapId, x, y, z, false, false, true, exact, MovementPriority::MOVEMENT_COMBAT, false, backwards);
    _guardedMove = guarded;
    return moved;
}

bool MovementAction::MoveAway(Unit* target, float distance, bool backwards)
{
    if (!target)
    {
        return false;
    }
    // [b35_mvaway] A direction's spot is the end of a navmesh path at the bot's own floor. The old spot came from
    // CheckCollisionAndGetValidCoords, which falls back to the grid height off elevated floors: gd-h
    // 20261026-024524-gd-h-5H-27 "avoid poison nova ... going point to (1760.9,661.1,0.0) 129.4 yd" from z 129.2 on
    // Slad'ran's platform (refused as a ledge drop, nobody left the nova). A path end more than 3 yd above or below
    // the bot, one that does not take it away from `target`, or one round a wall is skipped; the next direction is
    // tried. Without a navmesh (flying, swimming, a map without mmaps) the old collision spot is kept, under the
    // same 3 yd floor rule while on the ground.
    constexpr float MOVE_AWAY_MAX_DZ = 3.0f;
    constexpr float MOVE_AWAY_MAX_DETOUR_YD = 6.0f;
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const botZ = bot->GetPositionZ();
    float const fromTarget = target->GetExactDist2d(botX, botY);
    auto tryAngle = [&](float angle) -> bool
    {
        float dx = botX + cos(angle) * distance;
        float dy = botY + sin(angle) * distance;
        float dz = botZ;

        BotPath const path = FindBotPath(bot, dx, dy, botZ);  // [lead109]
        uint32 const type = path.type;
        Movement::PointsArray const& points = path.points;
        if (!(type & PATHFIND_NOT_USING_PATH))
        {
            if (!(type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) || (type & PATHFIND_NOPATH) || points.empty())
                return false;

            G3D::Vector3 const& end = points.back();
            float const travel = bot->GetExactDist2d(end.x, end.y);
            if (std::fabs(end.z - botZ) > MOVE_AWAY_MAX_DZ || travel < 0.5f ||
                target->GetExactDist2d(end.x, end.y) <= fromTarget ||
                path.length > travel + MOVE_AWAY_MAX_DETOUR_YD)
                return false;

            return MoveToStep(target->GetMapId(), end.x, end.y, end.z, true, backwards);
        }

        bool exact = true;
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, botX, botY, botZ, dx, dy, dz))
        {
            // disable prediction if position is invalid
            dx = botX + cos(angle) * distance;
            dy = botY + sin(angle) * distance;
            dz = botZ;
            exact = false;
        }
        if (!bot->IsInWater() && !bot->IsFlying() && std::fabs(dz - botZ) > MOVE_AWAY_MAX_DZ)
            return false;

        return MoveToStep(target->GetMapId(), dx, dy, dz, exact, backwards);
    };

    float init_angle = target->GetAngle(bot);
    for (float delta = 0; delta <= M_PI / 2; delta += M_PI / 8)
    {
        if (tryAngle(init_angle + delta))
            return true;

        if (delta == 0)
            continue;

        if (tryAngle(init_angle - delta))
            return true;
    }
    return false;
}

// just calculates average position of group and runs away from that position
bool MovementAction::MoveFromGroup(float distance)
{
    if (Group* group = bot->GetGroup())
    {
        uint32 mapId = bot->GetMapId();
        float closestDist = FLT_MAX;
        float x = 0.0f;
        float y = 0.0f;
        uint32 count = 0;

        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* player = gref->GetSource();
            if (!player || player == bot || !player->IsAlive() || player->GetMapId() != mapId)
                continue;
            float dist = bot->GetDistance2d(player);
            if (closestDist > dist)
                closestDist = dist;
            x += player->GetPositionX();
            y += player->GetPositionY();
            count++;
        }

        if (count && closestDist < distance)
        {
            x /= count;
            y /= count;
            // x and y are now average position of the group members
            float angle = bot->GetAngle(x, y) + M_PI;
            return Move(angle, distance - closestDist);
        }
    }
    return false;
}

bool MovementAction::Move(float angle, float distance)
{
    float x = bot->GetPositionX() + cos(angle) * distance;
    float y = bot->GetPositionY() + sin(angle) * distance;

    // TODO do we need GetMapWaterOrGroundLevel() if we're using CheckCollisionAndGetValidCoords() ?
    float z = bot->GetMapWaterOrGroundLevel(x, y, bot->GetPositionZ());
    if (z == -100000.0f || z == -200000.0f)
        z = bot->GetPositionZ();
    if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                        bot->GetPositionZ(), x, y, z, false))
        return false;

    return MoveTo(bot->GetMapId(), x, y, z);
}

bool MovementAction::MoveInside(uint32 mapId, float x, float y, float z, float distance, MovementPriority priority)
{
    if (bot->GetDistance2d(x, y) <= distance)
    {
        return false;
    }
    return MoveNear(mapId, x, y, z, distance, priority);
}

// float MovementAction::SearchBestGroundZForPath(float x, float y, float z, bool generatePath, float range, bool
// normal_only, float step)
// {
//     if (!generatePath)
//     {
//         return z;
//     }
//     float min_length = 100000.0f;
//     float current_z = INVALID_HEIGHT;
//     float modified_z;
//     float delta;
//     for (delta = 0.0f; delta <= range / 2; delta += step) {
//         modified_z = bot->GetMapWaterOrGroundLevel(x, y, z + delta);
//         PathGenerator gen(bot);
//         gen.CalculatePath(x, y, modified_z);
//         if (gen.GetPathType() == PATHFIND_NORMAL && gen.getPathLength() < min_length)
//         {
//             min_length = gen.getPathLength();
//             current_z = modified_z;
//             if (abs(current_z - z) < 0.5f)
//             {
//                 return current_z;
//             }
//         }
//     }
//     for (delta = -step; delta >= -range / 2; delta -= step) {
//         modified_z = bot->GetMapWaterOrGroundLevel(x, y, z + delta);
//         PathGenerator gen(bot);
//         gen.CalculatePath(x, y, modified_z);
//         if (gen.GetPathType() == PATHFIND_NORMAL && gen.getPathLength() < min_length)
//         {
//             min_length = gen.getPathLength();
//             current_z = modified_z;
//             if (abs(current_z - z) < 0.5f)
//                 return current_z;
//         }
//     }
//     for (delta = range / 2 + step; delta <= range; delta += 2) {
//         modified_z = bot->GetMapWaterOrGroundLevel(x, y, z + delta);
//         PathGenerator gen(bot);
//         gen.CalculatePath(x, y, modified_z);
//         if (gen.GetPathType() == PATHFIND_NORMAL && gen.getPathLength() < min_length)
//         {
//             min_length = gen.getPathLength();
//             current_z = modified_z;
//             if (abs(current_z - z) < 0.5f)
//             {
//                 return current_z;
//             }
//         }
//     }
//     if (current_z == INVALID_HEIGHT && normal_only)
//     {
//         return INVALID_HEIGHT;
//     }
//     if (current_z == INVALID_HEIGHT && !normal_only)
//     {
//         return z;
//     }
//     return current_z;
// }

namespace
{
    constexpr float WATER_DEST_BOTTOM_CLEARANCE = 2.0f;  // a destination this close to the bottom stays on it
    constexpr float WATER_DEST_MIN_DEPTH = 2.0f;         // shallower water is waded on its bottom
    constexpr float WATER_DEST_PROBE_UP = 0.5f;          // liquid probe just above the bottom, inside the water
    constexpr float WATER_DEST_SURFACE_BAND = 2.0f;      // deeper under the surface (or the head): no waypoint

    // GetMapHeight ignores liquid: a waypoint on the surface of deep water becomes a point on its bottom and the
    // bot dives instead of swimming across. Returns the height to keep for a destination at the surface of deep
    // water or ocean (its own z, at most the surface), INVALID_HEIGHT anywhere else. A destination well under the
    // surface (a unit's position) is searched as before; a bot in the water swims straight to it (SwimsTo).
    float WaterDestinationZ(Player* bot, float x, float y, float z, float groundZ)
    {
        if (groundZ <= INVALID_HEIGHT || z - groundZ < WATER_DEST_BOTTOM_CLEARANCE || !bot->GetMap())
            return INVALID_HEIGHT;

        LiquidData const liquid = bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), x, y,
                                                               groundZ + WATER_DEST_PROBE_UP,
                                                               bot->GetCollisionHeight(), {});
        if (!(liquid.Status & MAP_LIQUID_STATUS_SWIMMING) ||
            !(liquid.Flags & (MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN)) ||
            liquid.Level - groundZ < WATER_DEST_MIN_DEPTH ||
            liquid.Level - z > std::max(WATER_DEST_SURFACE_BAND, bot->GetCollisionHeight()))
            return INVALID_HEIGHT;

        return std::min(z, liquid.Level);
    }
}

const Movement::PointsArray MovementAction::SearchForBestPath(float x, float y, float z, float& modified_z,
                                                              int maxSearchCount, bool normal_only, float step)
{
    bool found = false;
    modified_z = INVALID_HEIGHT;
    float tempZ = bot->GetMapHeight(x, y, z);
    // Over deep water path to the water column, not to the bottom; on land (or if that fails) search as before.
    float const waterZ = WaterDestinationZ(bot, x, y, z, tempZ);
    if (waterZ != INVALID_HEIGHT)
    {
        BotPath const waterGen = FindBotPath(bot, x, y, waterZ);  // [lead109]
        if (waterGen.type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE))
        {
            modified_z = waterZ;
            return waterGen.points;
        }
    }
    // [lead147] b166 20261101-134022 73.7-76.8 s: the leader's Leader::Route found the walk to a spot by the tank, the
    // member's own search from the floor height there (GetMapHeight inside the rock, the tank had sunk) found none. A
    // bot on the leader's route walks to the destination's own navmesh point: the route's end, the same answer
    if (UsesLeaderRoute(bot))
    {
        BotPath const route = FindBotPath(bot, x, y, z);
        if (route.leaderRoute && (route.type & PATHFIND_NORMAL) && !route.points.empty())
        {
            modified_z = route.points.back().z;
            return route.points;
        }
    }
    BotPath const gen = FindBotPath(bot, x, y, tempZ);  // [lead109]
    Movement::PointsArray result = gen.points;
    float min_length = gen.length;
    int typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE;
    if ((gen.type & typeOk) && abs(tempZ - z) < 0.5f)
    {
        modified_z = tempZ;
        return result;
    }
    // Start searching
    if (gen.type & typeOk)
    {
        modified_z = tempZ;
        found = true;
    }
    int count = 1;
    for (float delta = step; count < maxSearchCount / 2 + 1; count++, delta += step)
    {
        tempZ = bot->GetMapHeight(x, y, z + delta);
        if (tempZ == INVALID_HEIGHT)
        {
            continue;
        }
        BotPath const gen = FindBotPath(bot, x, y, tempZ);  // [lead109]
        if ((gen.type & typeOk) && gen.length < min_length)
        {
            found = true;
            min_length = gen.length;
            result = gen.points;
            modified_z = tempZ;
        }
    }
    for (float delta = -step; count < maxSearchCount; count++, delta -= step)
    {
        tempZ = bot->GetMapHeight(x, y, z + delta);
        if (tempZ == INVALID_HEIGHT)
        {
            continue;
        }
        BotPath const gen = FindBotPath(bot, x, y, tempZ);  // [lead109]
        if ((gen.type & typeOk) && gen.length < min_length)
        {
            found = true;
            min_length = gen.length;
            result = gen.points;
            modified_z = tempZ;
        }
    }
    if (!found && normal_only)
    {
        modified_z = INVALID_HEIGHT;
        return Movement::PointsArray{};
    }
    if (!found && !normal_only)
    {
        return result;
    }
    return result;
}

namespace
{
    constexpr float STRAIGHT_LINE_STEP = 2.0f;
    constexpr float STRAIGHT_LINE_MAX_DROP = 6.0f;  // a step down a player walks without a fall
    constexpr float STRAIGHT_LINE_PROBE_UP = 2.0f;  // a line on a ramp runs just under its surface
    constexpr float STRAIGHT_LINE_SEARCH = 50.0f;

    // [w24d-exact-line-ground] An exact line (an escort spline, no navmesh, no gravity on the way) is walked only where
    // it lies on walkable ground all along: a floor at most EXACT_LINE_MAX_GAP under every step of it (no walk through
    // the air, as the 6 yd step-down allowance of a pathed straight walk would give), the line at most
    // EXACT_LINE_MAX_SINK under that floor (no walk through a slope or a wall), and the floor itself rising or falling
    // no steeper than a walkable slope between steps (no climb up a wall: an earlier gd run walked 10.8 yd up out of
    // the moat on a straight spline). No water: a swimmer swims (SwimStraight).
    constexpr float EXACT_LINE_MAX_GAP = 2.0f;
    constexpr float EXACT_LINE_MAX_SINK = 2.0f;
    constexpr float EXACT_LINE_PROBE_UP = 3.0f;
    constexpr float EXACT_LINE_SLOPE = 1.19f;      // tan(50 deg), the navmesh's walkable slope
    constexpr float EXACT_LINE_CLIMB = 0.6f;       // a stair step on top of the slope
    constexpr uint32 EXACT_LINE_LOG_MS = 10 * IN_MILLISECONDS;
    // [queue-exact-surface-line] The walk follows the floor under the line (one spline point per step at the floor's
    // height), not the chord: a floor up to EXACT_SURFACE_MAX_GAP under the chord carries it as it carries a player -
    // the slope check between steps still refuses any climb or drop steeper than a walkable slope. ub-n
    // 20261025-132016-ub-n-5A-48: the root up to Musel'ek's terrace (224.6,-194.0,3.8 -> 226.1,-163.7,24.3, 36.6 yd)
    // sags more than 2 yd under its chord - "no exact line: air under the line" for every member, the own path
    // fallback took the swamp floor 23 yd under the far end, link_off_line back to the start 36 times, 2 bosses never
    // reached.
    constexpr float EXACT_SURFACE_MAX_GAP = STRAIGHT_LINE_MAX_DROP;

    char const* ExactLineRefusal(Player* walker, float x, float y, float z, Movement::PointsArray* surface = nullptr)
    {
        Map* map = walker->GetMap();
        if (!map)
            return "no map";

        float const fromX = walker->GetPositionX();
        float const fromY = walker->GetPositionY();
        float const fromZ = walker->GetPositionZ();
        float const length = walker->GetExactDist(x, y, z);
        uint32 const steps = std::max<uint32>(1, static_cast<uint32>(length / STRAIGHT_LINE_STEP));
        float const stepHorizontal = std::hypot(x - fromX, y - fromY) / static_cast<float>(steps);
        float lastGround = map->GetHeight(walker->GetPhaseMask(), fromX, fromY, fromZ + EXACT_LINE_PROBE_UP, true,
                                          STRAIGHT_LINE_SEARCH);
        if (lastGround <= INVALID_HEIGHT || std::fabs(fromZ - lastGround) > EXACT_LINE_MAX_GAP)
            return "the bot stands on no floor";

        for (uint32 step = 1; step <= steps; ++step)
        {
            float const t = static_cast<float>(step) / static_cast<float>(steps);
            float const px = fromX + (x - fromX) * t;
            float const py = fromY + (y - fromY) * t;
            float const pz = fromZ + (z - fromZ) * t;
            float const ground =
                map->GetHeight(walker->GetPhaseMask(), px, py, pz + EXACT_LINE_PROBE_UP, true, STRAIGHT_LINE_SEARCH);
            if (ground <= INVALID_HEIGHT || pz - ground > (surface ? EXACT_SURFACE_MAX_GAP : EXACT_LINE_MAX_GAP))
                return "air under the line";
            if (ground - pz > EXACT_LINE_MAX_SINK)
                return "the line runs through the ground";
            if (std::fabs(ground - lastGround) > stepHorizontal * EXACT_LINE_SLOPE + EXACT_LINE_CLIMB)
                return "an unwalkable climb or drop on the way";
            lastGround = ground;
            if (surface)
                surface->push_back(step == steps ? G3D::Vector3(x, y, z) : G3D::Vector3(px, py, ground));
        }

        return nullptr;
    }
}

bool MovementAction::StraightLineHasGround(Player* walker, float x, float y, float z)
{
    if (!walker || !walker->GetMap())
        return false;

    Map* map = walker->GetMap();
    float const length = walker->GetExactDist(x, y, z);
    uint32 const steps = std::max<uint32>(1, static_cast<uint32>(length / STRAIGHT_LINE_STEP));
    for (uint32 step = 1; step <= steps; ++step)
    {
        float const t = static_cast<float>(step) / static_cast<float>(steps);
        float const px = walker->GetPositionX() + (x - walker->GetPositionX()) * t;
        float const py = walker->GetPositionY() + (y - walker->GetPositionY()) * t;
        float const pz = walker->GetPositionZ() + (z - walker->GetPositionZ()) * t;
        float const ground = map->GetHeight(walker->GetPhaseMask(), px, py, pz + STRAIGHT_LINE_PROBE_UP, true,
                                            STRAIGHT_LINE_SEARCH);
        if (ground > INVALID_HEIGHT && pz - ground <= STRAIGHT_LINE_MAX_DROP)
            continue;
        // Water carries a swimmer.
        LiquidData const liquid =
            map->GetLiquidData(walker->GetPhaseMask(), px, py, pz, walker->GetCollisionHeight(), {});
        if (!(liquid.Status & (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER | LIQUID_MAP_WATER_WALK)))
            return false;
    }
    return true;
}

bool MovementAction::MoveStraight(uint32 mapId, float x, float y, float z, bool requireGround,
                                  MovementPriority priority, bool exactLine)
{
    UpdateMovementState();
    if (mapId != bot->GetMapId() || !IsMovingAllowed() || IsDuplicateMove(x, y, z) ||
        IsWaitingForLastMove(priority))
        return false;

    float const distance = bot->GetExactDist(x, y, z);
    if (distance <= 0.01f)
        return false;

    if (!bot->IsWithinLOS(x, y, z))
        return false;
    if (requireGround && !bot->Unit::IsInWater() && !StraightLineHasGround(bot, x, y, z))
        return false;

    // A straight spline ignores collision: none through a closed door (TerrainHazard.h), like JumpTo. brs-h
    // 2026-10-25: MoveTo refused the leader's walk through Emberseer In, the test runner's straight walk did not.
    G3D::Vector3 const from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    if (PathThroughClosedDoor(bot, Movement::PointsArray{from, G3D::Vector3(x, y, z)}).Found())
        return false;

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    if (exactLine)
    {
        Movement::PointsArray surface;
        if (char const* refusal = ExactLineRefusal(bot, x, y, z, &surface))
        {
            thread_local std::unordered_map<uint64, uint32> refusedLogMs;
            uint32 const now = getMSTime();
            uint32& at = refusedLogMs[bot->GetGUID().GetRawValue()];
            if (!at || getMSTimeDiff(at, now) >= EXACT_LINE_LOG_MS)
            {
                at = now ? now : 1;
                LOG_INFO("playerbots", "[w24d-exact-line-ground] {}: no exact line to ({:.1f},{:.1f},{:.1f}), "
                         "{:.1f} yd: {}", bot->GetName(), x, y, z, distance, refusal);
            }
            return false;
        }

        // [w24b-exact-line] PointMovementGenerator::DoInitialize launches a generatePath=false MovePoint through
        // MoveSplineInit::MoveTo(x, y, z, true): PathGenerator runs, and its PATHFIND_INCOMPLETE path is followed to
        // the nearest polygon (gd-n 2026-10-25 115805: the bridge line to 1802,743.5,119.6 ended in the moat at
        // 1771.7,751.5,108.8; ok-n: the step onto the Volazj ramp ended 4 yd under its far end). An escort spline of
        // three points is followed as given (as SwimStraight); the sight, ground and door checks above still hold.
        G3D::Vector3 const dest(x, y, z);
        // [queue-exact-surface-line] the floor under every step (ExactLineRefusal), at least three points.
        Movement::PointsArray line = {from};
        line.insert(line.end(), surface.begin(), surface.end());
        if (line.size() < 3)
            line = {from, (from + dest) * 0.5f, dest};
        MotionMaster* mm = bot->GetMotionMaster();
        mm->Clear();
        mm->MoveSplinePath(&line);
    }
    else
        DoMovePoint(bot, x, y, z, false, false);
    float const delay = std::min(static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove),
                                 static_cast<float>(IN_MILLISECONDS) * MoveDelay(distance));
    AI_VALUE(LastMovement&, "last movement").Set(mapId, x, y, z, bot->GetOrientation(), delay, priority);
    return true;
}

bool MovementAction::MoveFlying(uint32 mapId, float x, float y, float z, MovementPriority priority)
{
    if (mapId != bot->GetMapId() || !IsMovingAllowed() || IsDuplicateMove(x, y, z) ||
        IsWaitingForLastMove(priority))
        return false;
    if (!bot->HasIncreaseMountedFlightSpeedAura() && !bot->HasFlyAura())
        return false;

    // UpdateMovementState sets these only while the master flies too, and clears them on the ground.
    if (!bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING))
    {
        bot->AddUnitMovementFlag(MOVEMENTFLAG_CAN_FLY);
        bot->AddUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
        bot->AddUnitMovementFlag(MOVEMENTFLAG_FLYING);
        bot->SendMovementFlagUpdate();
    }

    float const distance = bot->GetExactDist(x, y, z);
    if (distance <= 0.01f)
        return false;

    DoMovePoint(bot, x, y, z, false, false);
    float const delay = std::min(static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove),
                                 static_cast<float>(IN_MILLISECONDS) * MoveDelay(distance));
    AI_VALUE(LastMovement&, "last movement").Set(mapId, x, y, z, bot->GetOrientation(), delay, priority);
    return true;
}

void MovementAction::DoMovePoint(Unit* unit, float x, float y, float z, bool generatePath, bool backwards)
{
    if (!unit)
        return;

    MotionMaster* mm = unit->GetMotionMaster();
    if (!mm)
        return;

    // bot water collision correction
    if (unit->HasUnitMovementFlag(MOVEMENTFLAG_WATERWALKING) && unit->HasWaterWalkAura())
    {
        float gZ = unit->GetMapWaterOrGroundLevel(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
        unit->UpdatePosition(unit->GetPositionX(), unit->GetPositionY(), gZ, false);
    }

    // [lead109] a player bot walks the leader's route (BotPath.h); no route found: the core's own path as before
    if (unit == bot && generatePath && !backwards && WalkBotRoute(bot, x, y, z))
        return;
    // [nav24] the navmesh owner 2026-11-09: ... but only a complete core path. Its straight shortcut (no polygon at
    // the destination) and the straight tail of an INCOMPLETE path (past MAX_PATH_LENGTH 74 / 148 polygons or the
    // 1024-node query: the Dolanaar inn's 110-polygon stair, Ban'ethil's 356) climbed through towers and hills. No
    // complete path: no move (the walk's own search gives the spot up); a map with no navmesh keeps its straight walk
    if (unit == bot && generatePath && !backwards && UsesLeaderRoute(bot) && !bot->GetMap()->Instanceable())
    {
        PathGenerator core(bot);
        bool const valid = core.CalculatePath(x, y, z, false);
        PathType const type = core.GetPathType();
        bool const complete = valid && (type & PATHFIND_NORMAL) &&
                              !(type & (PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE | PATHFIND_NOPATH | PATHFIND_FARFROMPOLY));
        // [move28] an INCOMPLETE core path is walked as far as its polygons go: its last point is kept only when a
        // navmesh polygon lies under it (else it is the straight tail toward the destination, dropped). b377 Baridrol
        // stood 40 STUCK at the Dolanaar inn (9898,989): every walk there "no route, the core's path is not complete
        // (0x4)" - [nav24] refused the whole walk, not only its tail
        Movement::PointsArray points = core.GetPath();
        bool partial = false;
        if (valid && (type & PATHFIND_INCOMPLETE) && !(type & (PATHFIND_SHORTCUT | PATHFIND_NOPATH)) &&
            points.size() >= 2)
        {
            if (dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery())
            {
                dtQueryFilter filter;
                filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
                G3D::Vector3 const& last = points.back();
                float const point[3] = {last.y, last.z, last.x};
                float const extents[3] = {1.5f, 3.0f, 1.5f};
                float nearest[3] = {0.0f, 0.0f, 0.0f};
                dtPolyRef ref = 0;
                if (dtStatusFailed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) || !ref)
                    points.pop_back();
            }
            partial = points.size() >= 2;
        }
        if (!(type & PATHFIND_NOT_USING_PATH) && !complete && !partial)
        {
            LOG_DEBUG("playerbots", "[nav24] {} no move to ({:.1f},{:.1f},{:.1f}): no route, the core's path is not "
                      "complete (type {:#x})", bot->GetName(), x, y, z, uint32(type));
            return;
        }
        if ((complete || partial) && WalkBotPoints(bot, points))
            return;
    }

    mm->Clear();
    if (backwards)
    {
        mm->MovePointBackwards(
            /*id*/ 0,
            /*coords*/ x, y, z,
            /*generatePath*/ generatePath,
            /*forceDestination*/ false);
        return;
    }
    else
    {
        mm->MovePoint(
            /*id*/ 0,
            /*coords*/ x, y, z,
            /*forcedMovement*/ FORCED_MOVEMENT_NONE,
            /*speed*/ 0.f,
            /*orientation*/ 0.f,
            /*generatePath*/ generatePath,  // true => terrain path (2d mmap); false => straight spline (3d vmap)
            /*forceDestination*/ false);
    }
}

bool MovementAction::SwimStraight(uint32 mapId, float x, float y, float z, MovementPriority priority, bool lessDelay)
{
    float const distance = bot->GetExactDist(x, y, z);
    if (distance <= 0.01f)
        return false;

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    // MovePoint pathfinds even with generatePath false (PointMovementGenerator hands MoveSplineInit::MoveTo true),
    // and so does a two-point escort path; a path of three points is followed as given.
    G3D::Vector3 const start(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    G3D::Vector3 const end(x, y, z);
    Movement::PointsArray path = {start, (start + end) * 0.5f, end};
    MotionMaster* mm = bot->GetMotionMaster();
    mm->Clear();
    mm->MoveSplinePath(&path);

    float delay = IN_MILLISECONDS * MoveDelay(distance);
    if (lessDelay)
        delay -= botAI->GetReactDelay();
    delay = std::clamp(delay, 0.0f, static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove));
    AI_VALUE(LastMovement&, "last movement").Set(mapId, x, y, z, bot->GetOrientation(), delay, priority);
    return true;
}

bool FleeAction::Execute(Event /*event*/)
{
    return MoveAway(AI_VALUE(Unit*, "current target"), sPlayerbotAIConfig.FleeDistance, true);
}

bool FleeAction::isUseful()
{
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && target->IsInWorld() && !bot->IsWithinMeleeRange(target))
        return false;

    return true;
}

// [guild14]
bool SoloDisengageAction::isUseful() { return SoloSafety::Disengaging(bot); }

bool SoloDisengageAction::Execute(Event /*event*/)
{
    if (bot->GetVictim() || bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        bot->AttackStop();
    if (bot->IsNonMeleeSpellCast(false))
        bot->InterruptNonMeleeSpells(false);
    context->GetValue<Unit*>("current target")->Set(nullptr);
    bot->SetTarget(ObjectGuid::Empty);
    if (bot->GetPet())
        botAI->PetFollow();
    // the attacker it runs from: the nearest one still on it
    Unit* attacker = nullptr;
    for (Unit* unit : bot->getAttackers())
        if (unit && unit->IsAlive() && (!attacker || bot->GetExactDist(unit) < bot->GetExactDist(attacker)))
            attacker = unit;
    if (!attacker)
        return true;
    // a run already on its way (a point move of its own, not a chase)
    if (bot->isMoving() && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        return true;
    // away from where the mob leashes to (its home: Creature::CanCreatureAttack measures CONFIG_CREATURE_LEASH_RADIUS
    // from it), out past that radius; straight away first, else to either side, on a navmesh path
    Position home = attacker->GetPosition();
    if (Creature* creature = attacker->ToCreature())
        home = creature->GetHomePosition();
    float const leash = sWorld->getFloatConfig(CONFIG_CREATURE_LEASH_RADIUS);
    float const run = std::max(leash, bot->GetCombatReach()) + attacker->GetCombatReach();
    float away = home.GetAbsoluteAngle(bot);
    if (home.GetExactDist2d(bot) < bot->GetCombatReach())
        away = Position::NormalizeOrientation(attacker->GetOrientation() + float(M_PI));
    for (float turn : {0.0f, float(M_PI) / 2.0f, -float(M_PI) / 2.0f})
    {
        float const angle = Position::NormalizeOrientation(away + turn);
        float x = bot->GetPositionX() + std::cos(angle) * run;
        float y = bot->GetPositionY() + std::sin(angle) * run;
        float z = bot->GetPositionZ();
        bot->UpdateAllowedPositionZ(x, y, z);
        PathGenerator path(bot);
        path.CalculatePath(x, y, z, false);
        if (path.GetPathType() != PATHFIND_NORMAL)
            continue;
        bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true, true);
        LOG_INFO("playerbots", "[solo] {} L{} runs from {} L{}: ({:.0f},{:.0f}) -> ({:.0f},{:.0f}), {:.0f} yd from its "
                 "home after",
                 bot->GetName(), bot->GetLevel(), attacker->GetName(), attacker->GetLevel(), bot->GetPositionX(),
                 bot->GetPositionY(), x, y, home.GetExactDist2d(x, y));
        return true;
    }
    // no open way: the flee manager's spot
    Flee(attacker);
    return true;
}

bool FleeWithPetAction::Execute(Event /*event*/)
{
    if (bot->GetPet())
        botAI->PetFollow();

    return Flee(AI_VALUE(Unit*, "current target"));
}

namespace
{
// An attacker's spell the bot can still walk out of: a negative area around the caster (or its ground target) with a
// cast time that leaves CASTER_AOE_MIN_REACT_MS, or channelled. Instant point-blank spells (War Stomp, Trample, most
// Blast Waves) cannot be dodged by reacting and are healed through. Returns the danger centre and radius.
constexpr int32 CASTER_AOE_MIN_REACT_MS = 400;
constexpr float CASTER_AOE_SCAN_RANGE = 30.0f;
constexpr float CASTER_AOE_EXIT_MARGIN = 2.0f;
constexpr float CASTER_AOE_HOLD_MARGIN = 3.0f;
constexpr float CASTER_AOE_OUTRUN_SLACK = 1.0f;  // [b34_an] exit allowed past the walk before the cast lands

// One area spell of the caster: true with the danger centre and radius when the bot is inside it or within the hold
// margin just outside (so it does not walk back in while the cast still runs).
bool GetSpellAoe(Player* bot, Unit* caster, SpellInfo const* info, Spell* spell, Position& centre, float& radius)
{
    if (!info || info->IsPositive() ||
        sPlayerbotAIConfig.AoeAvoidSpellWhitelist.find(info->Id) != sPlayerbotAIConfig.AoeAvoidSpellWhitelist.end())
        return false;

    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.IsEffect())
            continue;

        SpellImplicitTargetInfo const& target = effect.TargetB.GetTarget() ? effect.TargetB : effect.TargetA;
        SpellTargetSelectionCategories const category = target.GetSelectionCategory();
        if (target.GetCheckType() != TARGET_CHECK_ENEMY && target.GetCheckType() != TARGET_CHECK_DEFAULT)
            continue;

        float const effectRadius = effect.CalcRadius(caster);
        if (effectRadius <= 0.0f || effectRadius > sPlayerbotAIConfig.MaxAoeAvoidRadius)
            continue;

        // Frontal cones (cleaves, breaths) are a positioning matter (melee behind, the tank faces the mob away),
        // not a reason to leave melee range.
        if (category != TARGET_SELECT_CATEGORY_AREA)
            continue;

        if (effect.TargetA.GetTarget() == TARGET_SRC_CASTER ||
            target.GetReferenceType() == TARGET_REFERENCE_TYPE_CASTER)
            centre = caster->GetPosition();
        else if (spell && spell->m_targets.HasDst() && spell->m_targets.GetDstPos())
            centre = spell->m_targets.GetDstPos()->GetPosition();
        else
            continue;

        if (bot->GetExactDist2d(centre) > effectRadius + bot->GetCombatReach() + CASTER_AOE_HOLD_MARGIN)
            continue;

        radius = effectRadius + bot->GetCombatReach();
        return true;
    }
    return false;
}

bool GetCasterAoe(Player* bot, Unit* caster, Position& centre, float& radius)
{
    for (CurrentSpellTypes type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
    {
        Spell* spell = caster->GetCurrentSpell(type);
        if (!spell)
            continue;

        if (type == CURRENT_GENERIC_SPELL &&
            (spell->GetCastTime() <= 0 || spell->GetCastTimeRemaining() < CASTER_AOE_MIN_REACT_MS))
            continue;

        if (!GetSpellAoe(bot, caster, spell->GetSpellInfo(), spell, centre, radius))
            continue;

        // [b34_an] A cast one the bot cannot walk out of before it lands is not dodged (healed through like an
        // instant one). an-n 20261026-014521-an-n-5G-25 (204 s): Watcher Narjil's 30 yd self-centred area cast
        // (radius 33.5 with reach + margin) sent the healer, the Ret paladin and the warrior 24-31 yd off the tank
        // (forced moves, long after the cast had landed): the healer stood 31 yd out, webbed, for 30 s and died to
        // the pack on her way back.
        if (type == CURRENT_GENERIC_SPELL)
        {
            float const exit = radius + CASTER_AOE_EXIT_MARGIN - bot->GetExactDist2d(centre);
            float const walk = bot->GetSpeed(MOVE_RUN) * spell->GetCastTimeRemaining() / 1000.0f;
            if (exit > walk + CASTER_AOE_OUTRUN_SLACK)
                continue;
        }
        return true;
    }

    return false;
}

// Tanks stay: the caster is rooted by its own cast and the tank holds its facing for the group.
Unit* FindCasterAoe(PlayerbotAI* botAI, Player* bot, GuidVector const& attackers, Position& centre, float& radius)
{
    if (PlayerbotAI::IsTank(bot))
        return nullptr;

    for (ObjectGuid const& guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !bot->IsWithinDistInMap(unit, CASTER_AOE_SCAN_RANGE))
            continue;

        if (GetCasterAoe(bot, unit, centre, radius))
            return unit;
    }
    return nullptr;
}
}  // namespace

// [lead16] the four cases' circles, without moving (the leader's hazard view)
void AvoidAoeAction::CollectZones(PlayerbotAI* botAI, std::vector<HazardZone>& out)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !bot->IsInWorld())
        return;
    AiObjectContext* context = botAI->GetAiObjectContext();
    auto const whitelisted = [](uint32 spellId)
    {
        return sPlayerbotAIConfig.AoeAvoidSpellWhitelist.find(spellId) !=
               sPlayerbotAIConfig.AoeAvoidSpellWhitelist.end();
    };
    float const maxRadius = sPlayerbotAIConfig.MaxAoeAvoidRadius;

    // #1 the area aura the bot stands in
    if (Aura* aura = AI_VALUE(Aura*, "area debuff");
        aura && !aura->IsRemoved() && !aura->IsExpired() && aura->GetType() == DYNOBJ_AURA_TYPE &&
        aura->GetSpellInfo() && !whitelisted(aura->GetSpellInfo()->Id))
        if (DynamicObject* dynOwner = aura->GetDynobjOwner(); dynOwner && dynOwner->IsInWorld())
            if (float const radius = dynOwner->GetRadius(); radius > 0.0f && radius <= maxRadius)
            {
                // [lead780] its spell, caster, time left and tick for the leader's stay-or-leave verdict
                uint32 periodMs = 0;
                for (SpellEffectInfo const& effect : aura->GetSpellInfo()->GetEffects())
                    if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE && effect.Amplitude > 0)
                        periodMs = uint32(effect.Amplitude);
                out.push_back({dynOwner->GetPosition(), radius, aura->GetSpellInfo()->SpellName[LOCALE_enUS],
                               ObjectGuid::Empty, aura->GetSpellInfo()->Id, aura->GetCasterGUID(),
                               dynOwner->GetDuration(), periodMs});
            }

    // #2 damage traps
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest trap with damage"))
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go || !go->IsInWorld() || go->GetGoType() != GAMEOBJECT_TYPE_TRAP)
            continue;
        GameObjectTemplate const* goInfo = go->GetGOInfo();
        if (!goInfo || goInfo->trap.type != 0 || !goInfo->trap.spellId || whitelisted(goInfo->trap.spellId))
            continue;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(goInfo->trap.spellId);
        if (!spellInfo || spellInfo->IsPositive())
            continue;
        float const radius = float(goInfo->trap.diameter) / 2.0f + go->GetCombatReach();
        if (radius > 0.0f && radius <= maxRadius)
            out.push_back({go->GetPosition(), radius, spellInfo->SpellName[LOCALE_enUS], ObjectGuid::Empty,
                           spellInfo->Id, ObjectGuid::Empty, -1, 0});  // [lead780] one hit per trigger
    }

    // #3 invisible trigger NPCs with a pulsing damage aura
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible triggers"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsInWorld())
            continue;
        Creature* creature = unit->ToCreature();
        if (!unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) && !(creature && creature->IsTrigger()))
            continue;
        for (AuraType type : {SPELL_AURA_PERIODIC_TRIGGER_SPELL, SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE})
            for (AuraEffect* aurEff : unit->GetAuraEffectsByType(type))
            {
                SpellInfo const* spellInfo = aurEff ? aurEff->GetSpellInfo() : nullptr;
                SpellInfo const* trigger =
                    spellInfo ? sSpellMgr->GetSpellInfo(spellInfo->Effects[aurEff->GetEffIndex()].TriggerSpell)
                              : nullptr;
                if (!trigger || whitelisted(trigger->Id))
                    continue;
                for (int j = 0; j < MAX_SPELL_EFFECTS; ++j)
                    if (trigger->Effects[j].Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                    {
                        float const radius = trigger->Effects[j].CalcRadius();
                        if (radius > 0.0f && radius <= maxRadius)
                            out.push_back({unit->GetPosition(), radius, trigger->SpellName[LOCALE_enUS],
                                           ObjectGuid::Empty, trigger->Id, unit->GetGUID(),
                                           aurEff->GetBase() ? aurEff->GetBase()->GetDuration() : -1,
                                           uint32(std::max(aurEff->GetAmplitude(), 0))});  // [lead780]
                        break;
                    }
            }
    }

    // #4 a mob's area cast with a cast bar
    Position centre;
    float radius = 0.0f;
    if (Unit* caster = FindCasterAoe(botAI, bot, AI_VALUE(GuidVector, "attackers"), centre, radius))
    {
        // [lead780] the cast's spell and its remaining time (a channel: its time left, ticking)
        uint32 spellId = 0;
        int32 leftMs = -1;
        uint32 periodMs = 0;
        if (Spell* spell = caster->GetCurrentSpell(CURRENT_GENERIC_SPELL); spell && spell->GetSpellInfo())
        {
            spellId = spell->GetSpellInfo()->Id;
            leftMs = int32(spell->GetCastTimeRemaining());
        }
        else if (Spell* channel = caster->GetCurrentSpell(CURRENT_CHANNELED_SPELL); channel && channel->GetSpellInfo())
        {
            spellId = channel->GetSpellInfo()->Id;
            leftMs = channel->GetSpellInfo()->GetDuration();
            if (Aura const* aura = caster->GetAura(spellId, caster->GetGUID()))
                leftMs = aura->GetDuration();
            for (SpellEffectInfo const& effect : channel->GetSpellInfo()->GetEffects())
                if (effect.Amplitude > 0)
                    periodMs = uint32(effect.Amplitude);
        }
        out.push_back({centre, radius + CASTER_AOE_EXIT_MARGIN, "area cast of " + caster->GetName(),
                       caster->GetGUID(), spellId, caster->GetGUID(), leftMs, periodMs});
    }
}

// Case #4: a mob casting a point-blank / ground area spell with a cast bar (Blast Wave 23039, War Stomp 38750) or
// channelling one: step just past its edge while it runs; the melee reach actions bring the bot back afterwards.
bool AvoidAoeAction::AvoidCasterAoe()
{
    Position centre;
    float radius = 0.0f;
    Unit* caster = FindCasterAoe(botAI, bot, AI_VALUE(GuidVector, "attackers"), centre, radius);
    if (!caster)
        return false;

    // Already out: hold there until the cast ends instead of stepping back in with the melee reach actions.
    if (bot->GetExactDist2d(centre) > radius)
        return true;

    // No lastMoveTimer here: isUseful() must keep holding the bot out for the rest of the cast.
    // Forced: a combat move still under way (reach, ranged position) would otherwise make the escape wait for it.
    return FleePosition(centre, radius + CASTER_AOE_EXIT_MARGIN, 500, MovementPriority::MOVEMENT_FORCED);
}

bool AvoidAoeAction::ShouldStopCastToDodge(PlayerbotAI* botAI)
{
    constexpr int32 DODGE_OWN_CAST_MIN_MS = 300;   // an own cast this close to landing is finished first
    constexpr int32 DODGE_WALK_MS = 1200;          // time to walk out once the own cast has landed

    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !bot->IsInCombat() || !sPlayerbotAIConfig.AutoAvoidAoe ||
        !botAI->HasStrategy("avoid aoe", BOT_STATE_COMBAT))
        return false;

    Spell* own = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!own || own->GetCastTimeRemaining() < DODGE_OWN_CAST_MIN_MS)
        return false;

    AiObjectContext* aiContext = botAI->GetAiObjectContext();
    if (Aura* aura = aiContext->GetValue<Aura*>("area debuff")->Get())
    {
        DynamicObject* dynOwner = aura->IsRemoved() ? nullptr : aura->GetDynobjOwner();
        SpellInfo const* info = aura->GetSpellInfo();
        if (dynOwner && dynOwner->IsInWorld() && info &&
            sPlayerbotAIConfig.AoeAvoidSpellWhitelist.find(info->Id) ==
                sPlayerbotAIConfig.AoeAvoidSpellWhitelist.end())
        {
            float const radius = dynOwner->GetRadius();
            if (radius > 0.0f && radius <= sPlayerbotAIConfig.MaxAoeAvoidRadius && bot->GetDistance(dynOwner) <= radius)
                return true;
        }
    }

    Position centre;
    float radius = 0.0f;
    Unit* caster = FindCasterAoe(botAI, bot, aiContext->GetValue<GuidVector>("attackers")->Get(), centre, radius);
    if (!caster || bot->GetExactDist2d(centre) > radius)
        return false;

    // A channelled area (Blizzard, Rain of Fire) hurts now; a cast one only matters if it lands before ours is done.
    Spell* enemy = caster->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!enemy || caster->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return true;

    return enemy->GetCastTimeRemaining() < own->GetCastTimeRemaining() + DODGE_WALK_MS;
}

namespace
{
    // [hazard_melee_spot] bot-h 20261025-171332-bot-h-5A-83 (~17:27-17:29, build 29): the Enhancement shaman went
    // out of a Sunseeker Chemist's Vial of Poison 39127 (r 5) by "avoid aoe" and straight back in by "reach melee" /
    // "set behind" to the mobs standing in it, over and over. The ground effects a bot left are noted (where, how big,
    // the damage per second its aura does): a melee walk into one picks a melee spot round its target outside it, or
    // stays in a light one (and avoid aoe leaves it be), or waits at its edge.
    constexpr uint32 HAZARD_HYSTERESIS_MS = 3 * IN_MILLISECONDS;  // a left effect keeps melee walks out this long
    constexpr uint32 HAZARD_TARGET_IN_MS = 10 * IN_MILLISECONDS;  // ... and this long while its target stands in it
    constexpr uint32 HAZARD_STAND_IN_MS = 5 * IN_MILLISECONDS;    // a "stand in, low damage" decision holds this long
    constexpr float HAZARD_SPOT_EDGE = 1.0f;                       // a melee spot stands this far outside the effect
    constexpr float HAZARD_LOW_SHARE = 0.10f;   // light: 5 s in it cost under this share of the bot's max health
    constexpr float HAZARD_LOW_SECONDS = 5.0f;
    constexpr float HAZARD_STAND_MIN_HEALTH = 50.0f;
    constexpr int HAZARD_SPOT_ANGLES = 16;
    constexpr uint32 HAZARD_LOG_MS = 3 * IN_MILLISECONDS;

    struct NotedHazard
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float radius = 0.0f;
        float dps = -1.0f;      // the aura's damage per second, -1 unknown
        uint32 ms = 0;          // last noted (the bot left it, or stood in it)
        uint32 standInMs = 0;   // "stand in, low damage" decided
    };

    std::mutex& HazardLock()
    {
        static std::mutex lock;
        return lock;
    }

    std::unordered_map<uint64, std::vector<NotedHazard>>& HazardMemory()
    {
        static std::unordered_map<uint64, std::vector<NotedHazard>> memory;
        return memory;
    }

    void NoteHazard(Player* bot, Position const& center, float radius, float dps)
    {
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(HazardLock());
        std::vector<NotedHazard>& list = HazardMemory()[bot->GetGUID().GetRawValue()];
        auto const stale = [now](NotedHazard const& h) { return getMSTimeDiff(h.ms, now) > 10 * IN_MILLISECONDS; };
        list.erase(std::remove_if(list.begin(), list.end(), stale), list.end());
        for (NotedHazard& h : list)
            if (std::hypot(h.x - center.GetPositionX(), h.y - center.GetPositionY()) <= 1.0f)
            {
                h.ms = now ? now : 1;
                h.radius = std::max(h.radius, radius);
                if (dps >= 0.0f)
                    h.dps = dps;
                return;
            }

        NotedHazard h;
        h.x = center.GetPositionX();
        h.y = center.GetPositionY();
        h.z = center.GetPositionZ();
        h.radius = radius;
        h.dps = dps;
        h.ms = now ? now : 1;
        list.push_back(h);
    }

    // The effect noted within `maxAgeMs` that (x, y) lies in, if any.
    bool NotedHazardAt(Player* bot, float x, float y, uint32 maxAgeMs, NotedHazard& found)
    {
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(HazardLock());
        auto const itr = HazardMemory().find(bot->GetGUID().GetRawValue());
        if (itr == HazardMemory().end())
            return false;

        for (NotedHazard const& h : itr->second)
            if (getMSTimeDiff(h.ms, now) <= maxAgeMs && std::hypot(h.x - x, h.y - y) < h.radius + 0.5f)
            {
                found = h;
                return true;
            }

        return false;
    }

    // [escape_room_kite] the effects the bot left within `maxAgeMs` (a boss's persistent clouds)
    std::vector<NotedHazard> NotedHazardsRecent(Player* bot, uint32 maxAgeMs)
    {
        std::vector<NotedHazard> result;
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(HazardLock());
        auto const itr = HazardMemory().find(bot->GetGUID().GetRawValue());
        if (itr != HazardMemory().end())
            for (NotedHazard const& h : itr->second)
                if (getMSTimeDiff(h.ms, now) <= maxAgeMs)
                    result.push_back(h);
        return result;
    }

    void MarkStandIn(Player* bot, NotedHazard const& hazard)
    {
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(HazardLock());
        for (NotedHazard& h : HazardMemory()[bot->GetGUID().GetRawValue()])
            if (std::hypot(h.x - hazard.x, h.y - hazard.y) <= 1.0f)
                h.standInMs = now ? now : 1;
    }

    bool StandsInDecided(Player* bot, Position const& center)
    {
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(HazardLock());
        auto const itr = HazardMemory().find(bot->GetGUID().GetRawValue());
        if (itr == HazardMemory().end())
            return false;

        for (NotedHazard const& h : itr->second)
            if (h.standInMs && getMSTimeDiff(h.standInMs, now) <= HAZARD_STAND_IN_MS &&
                std::hypot(h.x - center.GetPositionX(), h.y - center.GetPositionY()) <= 1.0f)
                return true;

        return false;
    }

    // The damage per second of a ground effect's aura (its periodic damage effects).
    float AuraDamagePerSecond(Aura const* aura)
    {
        float dps = 0.0f;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            AuraEffect const* effect = aura->GetEffect(i);
            if (!effect || effect->GetAuraType() != SPELL_AURA_PERIODIC_DAMAGE || effect->GetAmplitude() <= 0)
                continue;

            dps += float(effect->GetAmount()) * IN_MILLISECONDS / float(effect->GetAmplitude());
        }

        return dps;
    }

    bool GroupHealerUp(Player* bot)
    {
        Group* group = bot->GetGroup();
        for (GroupReference* ref = group ? group->GetFirstMember() : nullptr; ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->GetMap() == bot->GetMap() &&
                PlayerbotAI::IsHeal(member) && member->GetExactDist(bot) <= 40.0f)
                return true;
        }

        return false;
    }
}

bool AvoidAoeAction::isUseful()
{
    if (getMSTime() - moveInterval < uint32(lastMoveTimer))
        return false;

    GuidVector traps = AI_VALUE(GuidVector, "nearest trap with damage");
    GuidVector triggers = AI_VALUE(GuidVector, "possible triggers");
    if (AI_VALUE(Aura*, "area debuff") || !traps.empty() || !triggers.empty())
        return true;

    Position centre;
    float radius = 0.0f;
    return FindCasterAoe(botAI, bot, AI_VALUE(GuidVector, "attackers"), centre, radius) != nullptr;
}

bool AvoidAoeAction::Execute(Event /*event*/)
{
    // Case #1: Aura with dynamic object (e.g. rain of fire)
    if (AvoidAuraWithDynamicObj())
    {
        return true;
    }
    // Case #2: Trap game object with spell (e.g. lava bomb)
    if (AvoidGameObjectWithDamage())
    {
        return true;
    }
    // Case #3: Trigger npc (e.g. Lesser shadow fissure)
    if (AvoidUnitWithDamageAura())
    {
        return true;
    }
    // Case #4: Attacker casting an area spell around itself / a frontal cone (e.g. War Stomp)
    if (AvoidCasterAoe())
    {
        return true;
    }
    return false;
}

bool AvoidAoeAction::AvoidAuraWithDynamicObj()
{
    Aura* aura = AI_VALUE(Aura*, "area debuff");
    if (!aura || aura->IsRemoved() || aura->IsExpired())
    {
        return false;
    }
    if (!aura->GetOwner() || !aura->GetOwner()->IsInWorld())
    {
        return false;
    }
    // Crash fix: maybe change owner due to check interval
    if (aura->GetType() != DYNOBJ_AURA_TYPE)
    {
        return false;
    }
    SpellInfo const* spellInfo = aura->GetSpellInfo();
    if (!spellInfo)
    {
        return false;
    }
    if (sPlayerbotAIConfig.AoeAvoidSpellWhitelist.find(spellInfo->Id) !=
        sPlayerbotAIConfig.AoeAvoidSpellWhitelist.end())
        return false;

    DynamicObject* dynOwner = aura->GetDynobjOwner();
    if (!dynOwner || !dynOwner->IsInWorld())
    {
        return false;
    }
    float radius = dynOwner->GetRadius();
    if (!radius || radius > sPlayerbotAIConfig.MaxAoeAvoidRadius)
        return false;
    if (bot->GetDistance(dynOwner) > radius)
    {
        return false;
    }
    // [hazard_melee_spot] noted with its damage; a light one the bot decided to stand in (no melee spot outside its
    // target) is not left again while that decision holds and the bot's health does
    NoteHazard(bot, dynOwner->GetPosition(), radius, AuraDamagePerSecond(aura));
    if (StandsInDecided(bot, dynOwner->GetPosition()) && bot->GetHealthPct() >= HAZARD_STAND_MIN_HEALTH)
        return false;

    std::ostringstream name;
    name << spellInfo->SpellName[LOCALE_enUS];  // << "] (aura)";
    if (FleePosition(dynOwner->GetPosition(), radius, 1000, MovementPriority::MOVEMENT_FORCED))
    {
        if (sPlayerbotAIConfig.TellWhenAvoidAoe && lastTellTimer < time(NULL) - 10)
        {
            lastTellTimer = time(NULL);
            lastMoveTimer = getMSTime();
            std::ostringstream out;
            out << "I'm avoiding " << name.str() << " (" << spellInfo->Id << ")" << " Radius " << radius << " - [Aura]";
            bot->Say(out.str(), LANG_UNIVERSAL);
        }
        return true;
    }
    return false;
}

bool AvoidAoeAction::AvoidGameObjectWithDamage()
{
    GuidVector traps = AI_VALUE(GuidVector, "nearest trap with damage");
    if (traps.empty())
    {
        return false;
    }
    for (ObjectGuid& guid : traps)
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go || !go->IsInWorld())
        {
            continue;
        }
        if (go->GetGoType() != GAMEOBJECT_TYPE_TRAP)
        {
            continue;
        }
        GameObjectTemplate const* goInfo = go->GetGOInfo();
        if (!goInfo)
        {
            continue;
        }
        // 0 trap with no despawn after cast. 1 trap despawns after cast. 2 bomb casts on spawn.
        if (goInfo->trap.type != 0)
            continue;

        uint32 spellId = goInfo->trap.spellId;
        if (!spellId)
        {
            continue;
        }

        if (sPlayerbotAIConfig.AoeAvoidSpellWhitelist.find(spellId) !=
            sPlayerbotAIConfig.AoeAvoidSpellWhitelist.end())
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->IsPositive())
        {
            continue;
        }

        float radius = (float)goInfo->trap.diameter / 2 + go->GetCombatReach();
        if (!radius || radius > sPlayerbotAIConfig.MaxAoeAvoidRadius)
            continue;

        if (bot->GetDistance(go) > radius)
        {
            continue;
        }
        std::ostringstream name;
        name << spellInfo->SpellName[LOCALE_enUS];  // << "] (object)";
        if (FleePosition(go->GetPosition(), radius))
        {
            if (sPlayerbotAIConfig.TellWhenAvoidAoe && lastTellTimer < time(NULL) - 10)
            {
                lastTellTimer = time(NULL);
                lastMoveTimer = getMSTime();
                std::ostringstream out;
                out << "I'm avoiding " << name.str() << " (" << spellInfo->Id << ")" << " Radius " << radius
                    << " - [Trap]";
                bot->Say(out.str(), LANG_UNIVERSAL);
            }
            return true;
        }
    }
    return false;
}

namespace
{
    // How far the bot must walk along angle to stand AOE_EXIT_MARGIN outside a circle of radius around center.
    float DistanceToLeaveCircle(Player* bot, Position const& center, float radius, float angle)
    {
        constexpr float AOE_EXIT_MARGIN = 2.0f;
        float const vx = bot->GetPositionX() - center.GetPositionX();
        float const vy = bot->GetPositionY() - center.GetPositionY();
        float const along = vx * std::cos(angle) + vy * std::sin(angle);
        float const target = radius + AOE_EXIT_MARGIN;
        float const disc = along * along - (vx * vx + vy * vy) + target * target;
        if (disc <= 0.0f)
            return AOE_EXIT_MARGIN;
        return std::max(-along + std::sqrt(disc), AOE_EXIT_MARGIN);
    }
}
bool AvoidAoeAction::AvoidUnitWithDamageAura()
{
    GuidVector triggers = AI_VALUE(GuidVector, "possible triggers");
    if (triggers.empty())
    {
        return false;
    }
    for (ObjectGuid& guid : triggers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsInWorld())
        {
            continue;
        }
        // Only invisible helper NPCs that carry a ground effect; many encounter triggers have no unit flags,
        // so the creature's trigger flag counts too. Anything else is skipped, not a reason to stop looking.
        Creature* creature = unit->ToCreature();
        if (!unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) && !(creature && creature->IsTrigger()))
            continue;

        Unit::AuraEffectList const& aurasPeriodicTriggerSpell =
            unit->GetAuraEffectsByType(SPELL_AURA_PERIODIC_TRIGGER_SPELL);
        Unit::AuraEffectList const& aurasPeriodicTriggerWithValueSpell =
            unit->GetAuraEffectsByType(SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE);
        for (Unit::AuraEffectList const& list : {aurasPeriodicTriggerSpell, aurasPeriodicTriggerWithValueSpell})
        {
            for (auto i = list.begin(); i != list.end(); ++i)
            {
                AuraEffect* aurEff = *i;
                SpellInfo const* spellInfo = aurEff->GetSpellInfo();
                if (!spellInfo)
                    continue;
                SpellInfo const* triggerSpellInfo =
                    sSpellMgr->GetSpellInfo(spellInfo->Effects[aurEff->GetEffIndex()].TriggerSpell);
                if (!triggerSpellInfo)
                    continue;
                if (sPlayerbotAIConfig.AoeAvoidSpellWhitelist.find(triggerSpellInfo->Id) !=
                    sPlayerbotAIConfig.AoeAvoidSpellWhitelist.end())
                    continue;
                for (int j = 0; j < MAX_SPELL_EFFECTS; j++)
                {
                    if (triggerSpellInfo->Effects[j].Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                    {
                        float radius = triggerSpellInfo->Effects[j].CalcRadius();
                        if (bot->GetDistance(unit) > radius)
                        {
                            break;
                        }
                        if (!radius || radius > sPlayerbotAIConfig.MaxAoeAvoidRadius)
                            continue;
                        std::ostringstream name;
                        name << triggerSpellInfo->SpellName[LOCALE_enUS];  //<< "] (unit)";
                        if (FleePosition(unit->GetPosition(), radius))
                        {
                            lastMoveTimer = getMSTime();
                            if (sPlayerbotAIConfig.TellWhenAvoidAoe && lastTellTimer < time(NULL) - 10)
                            {
                                lastTellTimer = time(NULL);
                                std::ostringstream out;
                                out << "I'm avoiding " << name.str() << " (" << triggerSpellInfo->Id << ")"
                                    << " Radius " << radius << " - [Unit Trigger]";
                                bot->Say(out.str(), LANG_UNIVERSAL);
                            }
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

namespace
{
    // [escape_back_trail] scholo-h 20261025-153805 15:53:35: "avoid aoe" sent the Enhancement shaman to (271.6,154.3),
    // 75 yd off across the room past the idle left pack (the farthest exit of the candidates won), then "reach melee"
    // 37 yd back. User: "he could just have stepped back, everything behind was cleared". A way out of a ground
    // effect in a dungeon group is the nearest spot just outside it: first back along the tank's trail (ground the
    // group walked, cleared), else a side or forward spot, never inside an idle mob's aggro reach (no line of sight:
    // bars and doors pass aggro), never more than ESCAPE_MAX_WALK past the effect's edge.
    constexpr float ESCAPE_EDGE = 2.0f;           // a spot stands this far outside the circle (DistanceToLeaveCircle)
    constexpr float ESCAPE_MAX_WALK = 10.0f;      // the walk out at most this long, or the circle's exit plus slack
    constexpr float ESCAPE_EXIT_SLACK = 4.0f;
    constexpr float ESCAPE_IDLE_MARGIN = 2.0f;    // and this far outside every idle mob's reach
    constexpr float ESCAPE_MAX_DZ = 4.0f;         // on the bot's floor
    constexpr int ESCAPE_ANGLES = 16;
    constexpr int ESCAPE_WALK_SAMPLES = 4;
    constexpr float ESCAPE_FALLBACK_MAX = 15.0f;  // the old (farthest-exit) candidate only this near, in a dungeon
    constexpr float ESCAPE_TANK_STAY_HEALTH = 50.0f;  // a trash tank with a healer on it stays in an effect above this
    constexpr float ESCAPE_TANK_HEAL_RANGE = 36.0f;   // the tank's side step stays in the healer's range (40 - 4)
    constexpr float ESCAPE_TANK_GROUP_RANGE = 45.0f;  // the members that make "the group's side"
    // [escape_room_kite]
    constexpr uint32 ESCAPE_CLOUD_MEMORY_MS = 30 * IN_MILLISECONDS;  // clouds left this recently stay avoided
    constexpr float ESCAPE_ROOM_RADIUS = 30.0f;          // the fallback room round the boss's home
    constexpr float ESCAPE_ROOM_FIGHT_SPOT_SLACK = 10.0f;  // the fallback room round the fight spot: its radius + this
    constexpr float ESCAPE_KITE_MIN_STEP = 10.0f;        // a kite step out of the newest cloud
    constexpr float ESCAPE_KITE_MAX_STEP = 15.0f;
    constexpr float ESCAPE_KITE_NOT_TOWARD = static_cast<float>(M_PI / 3);  // never within 60 deg of the group

    std::vector<Creature*> EscapeIdleMobs(PlayerbotAI* botAI, Player* bot)
    {
        std::vector<Creature*> mobs;
        GuidVector const possible =
            botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
        for (ObjectGuid const& guid : possible)
        {
            Creature* mob = botAI->GetCreature(guid);
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || mob->IsInEvadeMode() ||
                !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetMap() != bot->GetMap() || !bot->IsHostileTo(mob) ||
                mob->IsCivilian() || IsCrowdControlled(mob))
                continue;

            mobs.push_back(mob);
        }

        return mobs;
    }

    // The walk from the bot to the point and the point itself keep out of every idle reach (a mob whose reach the
    // bot stands in already blocks only a point nearer to it).
    bool EscapeWakesIdle(std::vector<Creature*> const& mobs, Player* bot, float x, float y, float z)
    {
        for (Creature* mob : mobs)
        {
            float const reach = MovementAction::IdleAggroReach(mob, bot, ESCAPE_IDLE_MARGIN);
            float const botDistance = mob->GetExactDist(bot);
            for (int i = 1; i <= ESCAPE_WALK_SAMPLES; ++i)
            {
                float const along = static_cast<float>(i) / ESCAPE_WALK_SAMPLES;
                float const px = bot->GetPositionX() + (x - bot->GetPositionX()) * along;
                float const py = bot->GetPositionY() + (y - bot->GetPositionY()) * along;
                float const pz = bot->GetPositionZ() + (z - bot->GetPositionZ()) * along;
                if (!MovementAction::PassesAggroZGate(mob, bot, pz))
                    continue;

                float const distance = mob->GetExactDist(px, py, pz);
                if (distance < reach && distance < botDistance - 0.5f)
                    return true;
            }
        }

        return false;
    }

    bool EscapeSpotUsable(Player* bot, Position const& center, float radius, float x, float y, float z,
                          float maxWalk)
    {
        float const dx = x - center.GetPositionX();
        float const dy = y - center.GetPositionY();
        return std::sqrt(dx * dx + dy * dy) >= radius + ESCAPE_EDGE - 0.1f &&
               bot->GetExactDist(x, y, z) <= maxWalk && std::fabs(z - bot->GetPositionZ()) <= ESCAPE_MAX_DZ &&
               bot->IsWithinLOS(x, y, z + bot->GetCollisionHeight());
    }
}

MovementAction::KiteRoomLookupFn& MovementAction::KiteRoomLookup()
{
    static KiteRoomLookupFn lookup = nullptr;
    return lookup;
}

bool MovementAction::EscapeRoom(Position& centre, float& radius, Unit*& boss)
{
    boss = nullptr;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
    {
        Creature* creature = botAI->GetCreature(guid);
        if (creature && creature->IsAlive() && creature->IsInCombat() &&
            (creature->IsDungeonBoss() || creature->isWorldBoss()))
        {
            boss = creature;
            break;
        }
    }

    if (!boss)
        return false;

    if (KiteRoomLookupFn const lookup = KiteRoomLookup(); lookup && lookup(bot, boss, centre, radius) && radius > 0.0f)
        return true;

    // [lead9] a harness group's fight: the leader's safe zone, its disc for the bot's role (in place of the fight
    // spot's radius plus ESCAPE_ROOM_FIGHT_SPOT_SLACK below)
    if (Leader::SafeZone zone; Leader::FightZone::Get(bot, zone))
    {
        centre = zone.centre;
        radius = zone.RadiusFor(Leader::FightZone::RoleOf(bot));
        return true;
    }

    Player* tank = PlayerbotAI::IsTank(bot) ? bot : FindGroupTank(bot);
    if (tank && FightHold::Get(tank, centre))
    {
        radius = FightHold::RADIUS + ESCAPE_ROOM_FIGHT_SPOT_SLACK;
        return true;
    }

    centre = boss->ToCreature() ? boss->ToCreature()->GetHomePosition() : boss->GetPosition();
    radius = ESCAPE_ROOM_RADIUS;
    return true;
}

bool MovementAction::SafeEscapeSpot(Position const& center, float radius, Position& spot, std::string& how)
{
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon() || bot->InBattleground() || !bot->GetGroup())
        return false;

    // the shortest way out of the circle this walk may spend, plus slack
    float shortest = std::numeric_limits<float>::max();
    for (int i = 0; i < ESCAPE_ANGLES; ++i)
        shortest = std::min(shortest, DistanceToLeaveCircle(bot, center, radius,
                                                            i * 2.0f * static_cast<float>(M_PI) / ESCAPE_ANGLES));
    float const maxWalk = std::max(ESCAPE_MAX_WALK, shortest + ESCAPE_EXIT_SLACK);
    std::vector<Creature*> const mobs = EscapeIdleMobs(botAI, bot);

    // [chase_forward_escape] A chase from behind (HoR escape: the Lich King walks after the group along its trail):
    // never back along the trail or nearer to him; sideways or forward to the leader only, else stay (no fallback in
    // FleePosition either). hor-h 20261025-163739 16:56: the Enh shaman's "avoid aoe" stepped back towards him.
    Position chaseAnchor;
    Unit* chaser = nullptr;
    bool const chaseForward = HoRHelpers::GetChaseForwardAnchor(bot, chaseAnchor, chaser);
    auto const goesBack = [&](float x, float y)
    {
        if (!chaseForward)
            return false;

        float const nowToAnchor = bot->GetExactDist2d(&chaseAnchor);
        float const thenToAnchor = std::hypot(x - chaseAnchor.GetPositionX(), y - chaseAnchor.GetPositionY());
        return thenToAnchor > nowToAnchor + 1.0f ||
               (chaser && chaser->GetExactDist2d(x, y) < chaser->GetExactDist2d(bot) - 1.0f);
    };

    // [escape_room_kite] bf-h 20261025-171755-bf-h-5A-87 (build 29): Broggok's Poison Clouds (r 12) lay round the
    // doorway, the tank's side steps took it to (444.8,-13.2) in the doorway, and the next room's Shadowmoon
    // Warlocks / Laughing Skull Rogues came 11 s after the kill (user: "the cloud lay in the doorway, you must not
    // go there; kite him in a circle inside the room"). In a boss fight every escape spot stays in the boss's room
    // (MovementAction::KiteRoomLookup when set - BossKiteRooms, the data agent's table -, else the tank's fight spot,
    // else the boss's home; within its radius and in sight of its centre: never through a doorway), and out of
    // every cloud the bot left in the last ESCAPE_CLOUD_MEMORY_MS.
    Position roomCentre;
    float roomRadius = 0.0f;
    Unit* roomBoss = nullptr;
    bool const room = EscapeRoom(roomCentre, roomRadius, roomBoss);
    std::vector<NotedHazard> const clouds = NotedHazardsRecent(bot, ESCAPE_CLOUD_MEMORY_MS);
    auto const outOfRoom = [&](float x, float y, float z)
    {
        return room && (std::hypot(x - roomCentre.GetPositionX(), y - roomCentre.GetPositionY()) > roomRadius ||
                        !map->isInLineOfSight(roomCentre.GetPositionX(), roomCentre.GetPositionY(),
                                              roomCentre.GetPositionZ() + 2.0f, x, y, z + 2.0f, bot->GetPhaseMask(),
                                              LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing));
    };
    auto const inCloud = [&](float x, float y)
    {
        for (NotedHazard const& cloud : clouds)
            if (std::hypot(x - cloud.x, y - cloud.y) < cloud.radius + ESCAPE_EDGE)
                return true;
        return false;
    };
    // [lead9] a harness group's fight: every escape spot inside the leader's safe zone (none left: the bot stays and
    // takes the hit, MoveTo's clamp has the last word on the fallbacks)
    Leader::SafeZone zone;
    bool const zoned = Leader::FightZone::Get(bot, zone);
    Leader::ZoneRole const zoneRole = Leader::FightZone::RoleOf(bot);
    Position const zoneFrom = bot->GetPosition();
    auto const outOfZone = [&](float x, float y, float z)
    { return zoned && !zone.Contains(Position(x, y, z), zoneRole, &zoneFrom); };

    // [escape_tank_side] A tank with mobs on it never steps toward the group: the mobs follow it into the dps and the
    // healer (cleaves, their AoE). It steps sideways off the tank-to-group line, the least out of the effect, onto a
    // spot out of every idle reach, in the healer's range and sight, near the trail if it can. In a trash fight it
    // does not move at all above ESCAPE_TANK_STAY_HEALTH with a healer on it: the healer heals through a cloud, a
    // move with the pack costs more (user, scholo-h: "I'm avoiding cloud of disease").
    if (PlayerbotAI::IsTank(bot))
    {
        uint32 onTank = 0;
        bool boss = false;
        for (Unit* attacker : bot->getAttackers())
        {
            Creature* creature = attacker ? attacker->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive() || creature->GetVictim() != bot)
                continue;

            ++onTank;
            boss = boss || creature->IsDungeonBoss() || creature->isWorldBoss();
        }

        if (onTank)
        {
            Player* healer = nullptr;
            float cx = 0.0f;
            float cy = 0.0f;
            uint32 members = 0;
            Group* group = bot->GetGroup();
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
                    member->GetExactDist(bot) > ESCAPE_TANK_GROUP_RANGE)
                    continue;

                cx += member->GetPositionX();
                cy += member->GetPositionY();
                ++members;
                if (PlayerbotAI::IsHeal(member) && (!healer || member->GetExactDist(bot) < healer->GetExactDist(bot)))
                    healer = member;
            }

            bool const healerOn = healer && healer->GetExactDist(bot) <= ESCAPE_TANK_HEAL_RANGE &&
                                  healer->IsWithinLOSInMap(bot);
            // [hazard_melee_spot] two or more of its mobs stand in the effect: the melee cannot hit them safely, the
            // tank steps the pack out of it (a short side step, the group behind it) instead of staying
            uint32 mobsInIt = 0;
            for (Unit* attacker : bot->getAttackers())
                if (attacker && attacker->IsAlive() && attacker->GetVictim() == bot &&
                    attacker->GetExactDist2d(center.GetPositionX(), center.GetPositionY()) < radius)
                    ++mobsInIt;
            if (!boss && healerOn && bot->GetHealthPct() >= ESCAPE_TANK_STAY_HEALTH && mobsInIt < 2)
            {
                how = "stay";
                LOG_INFO("playerbots", "[avoid aoe] {}: tank escape: hazard ({:.1f},{:.1f}) r {:.1f}, damage estimate: "
                         "{:.0f} % health, healer {} on it, {} mobs on it, trash: stays, the healer heals through",
                         bot->GetName(), center.GetPositionX(), center.GetPositionY(), radius, bot->GetHealthPct(),
                         healer->GetName(), onTank);
                return false;
            }

            float const toGroup = members ? std::atan2(cy / members - bot->GetPositionY(),
                                                       cx / members - bot->GetPositionX())
                                          : bot->GetOrientation() + static_cast<float>(M_PI);
            std::vector<G3D::Vector3> const trail = TankTrailPoints(bot);
            float bestWalk = std::numeric_limits<float>::max();

            // [escape_room_kite] a boss that leaves clouds is kited round its room: a 10-15 yd step out of the newest
            // cloud along the open floor, never toward the group, out of every cloud left, in the room, the healer
            // in range and sight; the spot nearest the room's middle wins among the short ones
            if (boss && room && !chaseForward)
            {
                for (int i = 0; i < ESCAPE_ANGLES; ++i)
                {
                    float const angle = i * 2.0f * static_cast<float>(M_PI) / ESCAPE_ANGLES;
                    float const offGroup = std::fabs(
                        Position::NormalizeOrientation(angle - toGroup + static_cast<float>(M_PI)) -
                        static_cast<float>(M_PI));
                    if (members && offGroup < ESCAPE_KITE_NOT_TOWARD)
                        continue;

                    float const leave = DistanceToLeaveCircle(bot, center, radius, angle);
                    for (float const step :
                         {std::max(leave, ESCAPE_KITE_MIN_STEP), std::max(leave, ESCAPE_KITE_MAX_STEP)})
                    {
                        float x = bot->GetPositionX() + std::cos(angle) * step;
                        float y = bot->GetPositionY() + std::sin(angle) * step;
                        float z = bot->GetPositionZ();
                        if (!map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                                  bot->GetPositionZ(), x, y, z) ||
                            !EscapeSpotUsable(bot, center, radius, x, y, z, std::max(maxWalk, ESCAPE_KITE_MAX_STEP)) ||
                            EscapeWakesIdle(mobs, bot, x, y, z) || outOfRoom(x, y, z) || inCloud(x, y) ||
                            outOfZone(x, y, z) ||
                            (healer && (healer->GetExactDist(x, y, z) > ESCAPE_TANK_HEAL_RANGE ||
                                        !healer->IsWithinLOS(x, y, z + bot->GetCollisionHeight()))))
                            continue;

                        float const score = bot->GetExactDist(x, y, z) +
                                            0.5f * std::hypot(x - roomCentre.GetPositionX(),
                                                              y - roomCentre.GetPositionY());
                        if (score < bestWalk)
                        {
                            bestWalk = score;
                            spot.Relocate(x, y, z);
                        }
                    }
                }

                if (bestWalk < std::numeric_limits<float>::max())
                {
                    how = "kite (tank, boss room)";
                    LOG_INFO("playerbots", "[avoid aoe] {}: tank escape: hazard ({:.1f},{:.1f}) r {:.1f}, {} the boss, "
                             "{} clouds left, room ({:.1f},{:.1f}) r {:.0f}: kites to ({:.1f},{:.1f}), {:.1f} yd",
                             bot->GetName(), center.GetPositionX(), center.GetPositionY(), radius,
                             roomBoss ? roomBoss->GetName() : "", clouds.size(), roomCentre.GetPositionX(),
                             roomCentre.GetPositionY(), roomRadius, spot.GetPositionX(), spot.GetPositionY(),
                             bot->GetExactDist(spot));
                    return true;
                }
            }

            for (float const side : {static_cast<float>(M_PI / 2), static_cast<float>(-M_PI / 2)})
            {
                // the sideways step, a little back from the group too (never toward it)
                for (float const tilt : {0.0f, chaseForward ? 0.0f : static_cast<float>(M_PI / 8)})
                {
                    float const angle = toGroup + side + (side > 0.0f ? tilt : -tilt);
                    float const walk = DistanceToLeaveCircle(bot, center, radius, angle);
                    float x = bot->GetPositionX() + std::cos(angle) * walk;
                    float y = bot->GetPositionY() + std::sin(angle) * walk;
                    float z = bot->GetPositionZ();
                    if (!map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                              bot->GetPositionZ(), x, y, z) ||
                        !EscapeSpotUsable(bot, center, radius, x, y, z, maxWalk) ||
                        EscapeWakesIdle(mobs, bot, x, y, z) || goesBack(x, y) || outOfRoom(x, y, z) ||
                        inCloud(x, y) || outOfZone(x, y, z) ||
                        (healer && (healer->GetExactDist(x, y, z) > ESCAPE_TANK_HEAL_RANGE ||
                                    !healer->IsWithinLOS(x, y, z + bot->GetCollisionHeight()))))
                        continue;

                    // near the trail: cleared ground, 3 yd off the walk
                    float score = bot->GetExactDist(x, y, z);
                    for (G3D::Vector3 const& point : trail)
                        if (std::hypot(point.x - x, point.y - y) <= 3.0f)
                        {
                            score -= 3.0f;
                            break;
                        }

                    if (score < bestWalk)
                    {
                        bestWalk = score;
                        spot.Relocate(x, y, z);
                    }
                }
            }

            how = bestWalk < std::numeric_limits<float>::max() ? "side (tank)" : "stay";
            LOG_INFO("playerbots", "[avoid aoe] {}: tank escape: hazard ({:.1f},{:.1f}) r {:.1f}, damage estimate: "
                     "{:.0f} % health{}, {} mobs on it: {}", bot->GetName(), center.GetPositionX(),
                     center.GetPositionY(), radius, bot->GetHealthPct(), boss ? ", a boss fight" : "", onTank,
                     how == "stay" ? "no side spot, stays" : "steps sideways");
            return how != "stay";
        }
    }

    // 1. back along the tank's trail: the nearest trail point outside the circle
    Player* tank = PlayerbotAI::IsTank(bot) ? bot : FindGroupTank(bot);
    if (tank && !chaseForward)
    {
        std::vector<G3D::Vector3> const trail = TankTrailPoints(tank);
        float best = std::numeric_limits<float>::max();
        for (G3D::Vector3 const& point : trail)
        {
            float const walk = bot->GetExactDist(point.x, point.y, point.z);
            if (walk >= best || !EscapeSpotUsable(bot, center, radius, point.x, point.y, point.z, maxWalk) ||
                EscapeWakesIdle(mobs, bot, point.x, point.y, point.z) || outOfRoom(point.x, point.y, point.z) ||
                inCloud(point.x, point.y) || outOfZone(point.x, point.y, point.z))
                continue;

            best = walk;
            spot.Relocate(point.x, point.y, point.z);
        }

        if (best < std::numeric_limits<float>::max())
        {
            how = "back along trail";
            return true;
        }
    }

    // 2. just outside the circle all round, the side toward the tank (the group) first, then the nearest
    Unit* toward = tank && tank != bot ? static_cast<Unit*>(tank) : AI_VALUE(Unit*, "current target");
    float const towardAngle = chaseForward ? bot->GetAngle(&chaseAnchor)
                              : toward     ? bot->GetAngle(toward)
                                           : bot->GetAngle(&center) + static_cast<float>(M_PI);
    float bestScore = std::numeric_limits<float>::max();
    float bestOff = 0.0f;
    for (int i = 0; i < ESCAPE_ANGLES; ++i)
    {
        float const angle = i * 2.0f * static_cast<float>(M_PI) / ESCAPE_ANGLES;
        float const walk = DistanceToLeaveCircle(bot, center, radius, angle);
        float x = bot->GetPositionX() + std::cos(angle) * walk;
        float y = bot->GetPositionY() + std::sin(angle) * walk;
        float z = bot->GetPositionZ();
        if (!map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                                  x, y, z) ||
            !EscapeSpotUsable(bot, center, radius, x, y, z, maxWalk) || EscapeWakesIdle(mobs, bot, x, y, z) ||
            goesBack(x, y) || outOfRoom(x, y, z) || inCloud(x, y) || outOfZone(x, y, z))
            continue;

        // the walk, plus 1 yd for every 30 degrees off the group's side
        float const off = std::fabs(Position::NormalizeOrientation(angle - towardAngle + static_cast<float>(M_PI)) -
                                    static_cast<float>(M_PI));
        float const score = bot->GetExactDist(x, y, z) + off * 6.0f / static_cast<float>(M_PI);
        if (score < bestScore)
        {
            bestScore = score;
            bestOff = off;
            spot.Relocate(x, y, z);
        }
    }

    if (bestScore == std::numeric_limits<float>::max())
    {
        if (chaseForward)
            how = "stay";  // FleePosition: no melee / ranged fallback, those may go back
        return false;
    }

    how = bestOff <= static_cast<float>(M_PI) / 4.0f ? "forward" : "side";
    return true;
}

namespace
{
    // [los_restore] brs-h 20261025-163739-brs-h-5A-7 (~3184-3364 s, build 29): Quartermaster Zigris at 0.1 % health,
    // in combat on the tank, shot from behind cover at (-210.6,-502.1,88.3). The tank 12.2 yd off and every member
    // had no line of sight; "reach melee" returned false without a word (his spot is off the navmesh: no path), "reach
    // spell" stood in range (the range check knows no sight), the mage's sight arc found "no ground under it". 180 s,
    // "boss attempt idle". When the group's target fights the group and no member has seen it for LOS_LOST_MS, the
    // bot walks to the nearest reachable spot that sees it: past the fight hold, still out of every idle reach.
    constexpr uint32 LOS_LOST_MS = 3 * IN_MILLISECONDS;
    constexpr float LOS_RESTORE_RINGS[] = {0.0f, 5.0f, 8.0f, 12.0f, 16.0f, 20.0f, 25.0f};  // 0: the melee ring
    constexpr int LOS_RESTORE_ANGLES = 16;
    constexpr int LOS_RESTORE_MAX_PATHS = 12;  // path calculations one search may spend
    constexpr float LOS_RESTORE_MAX_DZ = 6.0f;
    constexpr float LOS_RESTORE_PATH_END = 2.0f;  // an incomplete path ending this near the spot still reaches it
    constexpr float LOS_RESTORE_MAX_RANGED = 25.0f;

    struct LosLost
    {
        uint32 sinceMs = 0;
        uint32 checkedMs = 0;
        bool anySees = true;
    };

    std::mutex& LosLostLock()
    {
        static std::mutex lock;
        return lock;
    }

    std::unordered_map<uint64, LosLost>& LosLostMap()
    {
        static std::unordered_map<uint64, LosLost> map;
        return map;
    }

    // How long no member of the bot's group has seen `target` (0: someone sees it). Checked at most every 500 ms.
    uint32 GroupLostSightMs(Player* bot, Unit* target)
    {
        uint32 const now = getMSTime();
        Group const* own = bot->GetGroup();
        uint64 const key = target->GetGUID().GetRawValue() ^ (uint64(own ? own->GetGUID().GetCounter() : 0) << 40);
        {
            std::lock_guard<std::mutex> guard(LosLostLock());
            LosLost const& entry = LosLostMap()[key];
            if (entry.checkedMs && getMSTimeDiff(entry.checkedMs, now) < 500)
                return entry.anySees || !entry.sinceMs ? 0 : getMSTimeDiff(entry.sinceMs, now);
        }

        bool anySees = false;
        Group* group = bot->GetGroup();
        for (GroupReference* ref = group ? group->GetFirstMember() : nullptr; ref && !anySees; ref = ref->next())
        {
            Player* member = ref->GetSource();
            anySees = member && member->IsAlive() && member->GetMap() == bot->GetMap() &&
                      member->GetExactDist(target) <= 45.0f && member->IsWithinLOSInMap(target);
        }

        std::lock_guard<std::mutex> guard(LosLostLock());
        LosLost& entry = LosLostMap()[key];
        entry.checkedMs = now ? now : 1;
        entry.anySees = anySees;
        if (anySees)
            entry.sinceMs = 0;
        else if (!entry.sinceMs)
            entry.sinceMs = now ? now : 1;
        return anySees ? 0 : getMSTimeDiff(entry.sinceMs, now);
    }
}

bool MovementAction::LosRestoreWalk(Unit* target, float meleeDistance)
{
    Map* map = bot->GetMap();
    if (!target || !map || !map->IsDungeon() || !bot->GetGroup() || bot->InBattleground() || !bot->IsInCombat() ||
        !target->IsAlive() || !target->IsInCombat() || !bot->IsValidAttackTarget(target) ||
        bot->IsWithinLOSInMap(target))
        return false;

    // the target fights the group: its victim is a member (or the bot)
    Unit* victim = target->GetVictim();
    Player* victimPlayer = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (!victimPlayer || (victimPlayer != bot && !victimPlayer->IsInSameGroupWith(bot)))
        return false;

    uint32 const lostMs = GroupLostSightMs(bot, target);
    if (lostMs < LOS_LOST_MS)
        return false;

    bool const ranged = !PlayerbotAI::IsTank(bot) && (PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot));
    std::vector<Creature*> const mobs = EscapeIdleMobs(botAI, bot);
    float const eye = bot->GetCollisionHeight();
    int paths = 0;
    for (float const ringSpec : LOS_RESTORE_RINGS)
    {
        float const ring = ringSpec > 0.0f ? ringSpec : meleeDistance;
        if (ranged && ring > LOS_RESTORE_MAX_RANGED)
            break;
        if (!ranged && ringSpec > 8.0f)
            break;  // tank and melee: at the target, or a few yards round it

        // candidates of this ring, nearest to the bot first
        std::vector<std::pair<float, G3D::Vector3>> ringSpots;
        for (int i = 0; i < LOS_RESTORE_ANGLES; ++i)
        {
            float const angle = i * 2.0f * static_cast<float>(M_PI) / LOS_RESTORE_ANGLES;
            float x = target->GetPositionX() + std::cos(angle) * ring;
            float y = target->GetPositionY() + std::sin(angle) * ring;
            float z = target->GetPositionZ();
            bot->UpdateAllowedPositionZ(x, y, z);
            if (std::fabs(z - target->GetPositionZ()) > LOS_RESTORE_MAX_DZ ||
                !target->IsWithinLOS(x, y, z + eye) || EscapeWakesIdle(mobs, bot, x, y, z))
                continue;

            ringSpots.push_back({bot->GetExactDist(x, y, z), G3D::Vector3(x, y, z)});
        }

        std::sort(ringSpots.begin(), ringSpots.end(),
                  [](auto const& a, auto const& b) { return a.first < b.first; });
        for (auto const& [walk, spot] : ringSpots)
        {
            if (paths++ >= LOS_RESTORE_MAX_PATHS)
                return false;

            BotPath const path = FindBotPath(bot, spot.x, spot.y, spot.z);  // [lead109]
            uint32 const type = path.type;
            if ((type & PATHFIND_NOPATH) || path.points.empty() ||
                (!(type & PATHFIND_NORMAL) && (path.actualEnd - spot).length() > LOS_RESTORE_PATH_END))
                continue;

            LOG_INFO("playerbots", "[los restore] {}: no member has seen {} for {:.1f} s: walks {:.1f} yd to "
                     "({:.1f},{:.1f},{:.1f}), {:.1f} yd from it, in its sight", bot->GetName(), target->GetName(),
                     lostMs / 1000.0f, walk, spot.x, spot.y, spot.z, target->GetExactDist(spot.x, spot.y, spot.z));
            bool const restoring = _losRestore;
            _losRestore = true;  // HoldFightSpot lets it through; the idle-mob path guard still judges it
            bool const moved = MoveTo(bot->GetMapId(), spot.x, spot.y, spot.z, false, false, false, false,
                                      MovementPriority::MOVEMENT_COMBAT, true);
            _losRestore = restoring;
            return moved;
        }
    }

    return false;
}

bool MovementAction::HazardMeleeSpot(float& x, float& y, float& z)
{
    std::string const name = getName();
    if ((name != "reach melee" && name != "set behind") || PlayerbotAI::IsTank(bot))
        return true;

    // within HAZARD_HYSTERESIS_MS of leaving it always; longer (up to HAZARD_TARGET_IN_MS) while the target still
    // stands in it (a puddle outlasts the 3 s: the walk back in would only come 3 s later)
    NotedHazard hazard;
    if (!NotedHazardAt(bot, x, y, HAZARD_TARGET_IN_MS, hazard))
        return true;

    uint32 const now = getMSTime();
    Unit* chased = AI_VALUE(Unit*, "current target");
    if (getMSTimeDiff(hazard.ms, now) > HAZARD_HYSTERESIS_MS &&
        (!chased || std::hypot(chased->GetPositionX() - hazard.x, chased->GetPositionY() - hazard.y) >= hazard.radius))
        return true;

    bool const logDue = !_hazardLogMs || getMSTimeDiff(_hazardLogMs, now) >= HAZARD_LOG_MS;
    if (logDue)
        _hazardLogMs = now ? now : 1;

    Unit* target = chased;
    if (target && target->IsAlive())
    {
        // a melee spot round the target outside the effect: the target's reach plus the bot's (the core's melee
        // range), on the target's floor, in its sight, out of idle reach, out of its front when the tank holds it
        float const meleeRange = std::max(NOMINAL_MELEE_RANGE, target->GetCombatReach() + bot->GetCombatReach() +
                                                                    4.0f / 3.0f) - 0.5f;
        Unit* victim = target->GetVictim();
        bool const avoidFront = victim && victim != bot && victim->ToPlayer() &&
                                PlayerbotAI::IsTank(victim->ToPlayer());
        std::vector<Creature*> const mobs = EscapeIdleMobs(botAI, bot);
        float best = std::numeric_limits<float>::max();
        float bx = 0.0f;
        float by = 0.0f;
        float bz = 0.0f;
        for (int i = 0; i < HAZARD_SPOT_ANGLES; ++i)
        {
            float const angle = i * 2.0f * static_cast<float>(M_PI) / HAZARD_SPOT_ANGLES;
            float sx = target->GetPositionX() + std::cos(angle) * meleeRange;
            float sy = target->GetPositionY() + std::sin(angle) * meleeRange;
            float sz = target->GetPositionZ();
            bot->UpdateAllowedPositionZ(sx, sy, sz);
            Position const candidate(sx, sy, sz);
            if (std::hypot(sx - hazard.x, sy - hazard.y) < hazard.radius + HAZARD_SPOT_EDGE ||
                std::fabs(sz - target->GetPositionZ()) > 3.0f ||
                (avoidFront && target->HasInArc(static_cast<float>(M_PI), &candidate)) ||
                !target->IsWithinLOS(sx, sy, sz + bot->GetCollisionHeight()) || EscapeWakesIdle(mobs, bot, sx, sy, sz))
                continue;

            float const walk = bot->GetExactDist(sx, sy, sz);
            if (walk < best)
            {
                best = walk;
                bx = sx;
                by = sy;
                bz = sz;
            }
        }

        if (best < std::numeric_limits<float>::max())
        {
            if (logDue)
                LOG_INFO("playerbots", "[hazard_melee_spot] {}: {} into the effect at ({:.1f},{:.1f}) r {:.1f}: spot "
                         "found ({:.1f},{:.1f}) round {}, {:.1f} yd", bot->GetName(), name, hazard.x, hazard.y,
                         hazard.radius, bx, by, target->GetName(), best);
            x = bx;
            y = by;
            z = bz;
            return true;
        }
    }

    // no spot outside: a light effect is stood in (a healer heals it), a heavy one waited out at its edge
    float const maxHealth = float(bot->GetMaxHealth());
    bool const light = hazard.dps >= 0.0f && maxHealth > 0.0f &&
                       hazard.dps * HAZARD_LOW_SECONDS < HAZARD_LOW_SHARE * maxHealth &&
                       bot->GetHealthPct() >= HAZARD_STAND_MIN_HEALTH && GroupHealerUp(bot);
    if (light)
    {
        MarkStandIn(bot, hazard);
        if (logDue)
            LOG_INFO("playerbots", "[hazard_melee_spot] {}: {} into the effect at ({:.1f},{:.1f}) r {:.1f}: stand in, "
                     "low damage ({:.0f} dps, {:.0f} % health)", bot->GetName(), name, hazard.x, hazard.y,
                     hazard.radius, hazard.dps, bot->GetHealthPct());
        return true;
    }

    if (logDue)
        LOG_INFO("playerbots", "[hazard_melee_spot] {}: {} into the effect at ({:.1f},{:.1f}) r {:.1f}: wait at edge "
                 "({} dps, no melee spot outside)", bot->GetName(), name, hazard.x, hazard.y, hazard.radius,
                 hazard.dps >= 0.0f ? std::to_string(int32(hazard.dps)) : std::string("unknown"));
    return false;
}

Position MovementAction::BestPositionForMeleeToFlee(Position pos, float radius)
{
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    std::vector<CheckAngle> possibleAngles;
    if (currentTarget)
    {
        // Normally, move to left or right is the best position
        bool isTanking = (!currentTarget->isFrozen()
            && !currentTarget->HasRootAura()) && (currentTarget->GetVictim() == bot);
        float angle = bot->GetAngle(currentTarget);
        float angleLeft = angle + (float)M_PI / 2;
        float angleRight = angle - (float)M_PI / 2;
        possibleAngles.push_back({angleLeft, false});
        possibleAngles.push_back({angleRight, false});
        possibleAngles.push_back({angle, true});
        if (isTanking)
        {
            possibleAngles.push_back({angle + (float)M_PI, false});
            possibleAngles.push_back({bot->GetAngle(&pos) - (float)M_PI, false});
        }
    }
    else
    {
        float angleTo = bot->GetAngle(&pos) - (float)M_PI;
        possibleAngles.push_back({angleTo, false});
    }
    // A damage dealer on a mob its tank holds keeps out of the mob's front (cleaves, frontal smashes and bites):
    // a spot in front is taken only when every other one is blocked.
    Unit* targetVictim = currentTarget ? currentTarget->GetVictim() : nullptr;
    bool const avoidFront = currentTarget && targetVictim && targetVictim != bot && targetVictim->ToPlayer() &&
                            PlayerbotAI::IsTank(targetVictim->ToPlayer()) && !PlayerbotAI::IsTank(bot);
    float farestDis = 0.0f;
    Position bestPos;
    float farestFrontDis = 0.0f;
    Position bestFrontPos;
    for (CheckAngle& checkAngle : possibleAngles)
    {
        float angle = checkAngle.angle;
        std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");
        if (!CheckLastFlee(angle, infoList))
        {
            continue;
        }
        bool strict = checkAngle.strict;
        float fleeDis = DistanceToLeaveCircle(bot, pos, radius, angle);
        float dx = bot->GetPositionX() + cos(angle) * fleeDis;
        float dy = bot->GetPositionY() + sin(angle) * fleeDis;
        float dz = bot->GetPositionZ();
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), dx, dy, dz))
        {
            continue;
        }
        Position fleePos{dx, dy, dz};
        if (strict && currentTarget &&
            fleePos.GetExactDist(currentTarget) - currentTarget->GetCombatReach() >
                sPlayerbotAIConfig.TooCloseDistance &&
            bot->IsWithinMeleeRange(currentTarget))
        {
            continue;
        }
        if (avoidFront && currentTarget->HasInArc(static_cast<float>(M_PI), &fleePos))
        {
            if (pos.GetExactDist(fleePos) > farestFrontDis)
            {
                farestFrontDis = pos.GetExactDist(fleePos);
                bestFrontPos = fleePos;
            }
            continue;
        }
        if (pos.GetExactDist(fleePos) > farestDis)
        {
            farestDis = pos.GetExactDist(fleePos);
            bestPos = fleePos;
        }
    }
    if (farestDis > 0.0f)
    {
        return bestPos;
    }
    if (farestFrontDis > 0.0f)
    {
        return bestFrontPos;
    }
    return Position();
}

Position MovementAction::BestPositionForRangedToFlee(Position pos, float radius)
{
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    std::vector<CheckAngle> possibleAngles;
    float angleToTarget = 0.0f;
    float angleFleeFromCenter = bot->GetAngle(&pos) - (float)M_PI;
    if (currentTarget)
    {
        // Normally, move to left or right is the best position
        angleToTarget = bot->GetAngle(currentTarget);
        float angleLeft = angleToTarget + (float)M_PI / 2;
        float angleRight = angleToTarget - (float)M_PI / 2;
        possibleAngles.push_back({angleLeft, false});
        possibleAngles.push_back({angleRight, false});
        possibleAngles.push_back({angleToTarget + (float)M_PI, true});
        possibleAngles.push_back({angleToTarget, true});
        possibleAngles.push_back({angleFleeFromCenter, true});
    }
    else
    {
        possibleAngles.push_back({angleFleeFromCenter, false});
    }
    float farestDis = 0.0f;
    Position bestPos;
    for (CheckAngle& checkAngle : possibleAngles)
    {
        float angle = checkAngle.angle;
        std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");
        if (!CheckLastFlee(angle, infoList))
        {
            continue;
        }
        bool strict = checkAngle.strict;
        float fleeDis = DistanceToLeaveCircle(bot, pos, radius, angle);
        float dx = bot->GetPositionX() + cos(angle) * fleeDis;
        float dy = bot->GetPositionY() + sin(angle) * fleeDis;
        float dz = bot->GetPositionZ();
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), dx, dy, dz))
        {
            continue;
        }
        Position fleePos{dx, dy, dz};
        if (strict && currentTarget &&
            fleePos.GetExactDist(currentTarget) - currentTarget->GetCombatReach() > sPlayerbotAIConfig.SpellDistance)
        {
            continue;
        }
        if (strict && currentTarget &&
            fleePos.GetExactDist(currentTarget) - currentTarget->GetCombatReach() <
                (sPlayerbotAIConfig.TooCloseDistance))
        {
            continue;
        }

        if (pos.GetExactDist(fleePos) > farestDis)
        {
            farestDis = pos.GetExactDist(fleePos);
            bestPos = fleePos;
        }
    }
    if (farestDis > 0.0f)
    {
        return bestPos;
    }
    return Position();
}

bool MovementAction::FleePosition(Position pos, float radius, uint32 minInterval, MovementPriority priority)
{
    std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");

    if (!infoList.empty() && infoList.back().timestamp + minInterval > getMSTime())
        return false;

    Position bestPos;
    // [escape_back_trail] in a dungeon group: the nearest safe spot just outside, back along the tank's trail first
    std::string how;
    bool const dungeonGroup = bot->GetMap() && bot->GetMap()->IsDungeon() && bot->GetGroup() && !bot->InBattleground();
    if (!SafeEscapeSpot(pos, radius, bestPos, how) && how == "stay")
        return false;  // a tank holding mobs stays (a light effect, or no side step): never toward the group
    if (!how.empty())
    {
        LOG_INFO("playerbots",
                 "[avoid aoe] {}: out of ({:.1f},{:.1f}) r {:.1f}: {} to ({:.1f},{:.1f},{:.1f}), {:.1f} yd",
                 bot->GetName(), pos.GetPositionX(), pos.GetPositionY(), radius, how, bestPos.GetPositionX(),
                 bestPos.GetPositionY(), bestPos.GetPositionZ(), bot->GetExactDist(bestPos));
    }
    else if (PlayerbotAI::IsMelee(bot))
    {
        bestPos = BestPositionForMeleeToFlee(pos, radius);
    }
    else
    {
        bestPos = BestPositionForRangedToFlee(pos, radius);
    }

    // a dungeon group's old candidate is only taken near (the safe search found nothing: the effect is big or walled)
    if (dungeonGroup && how.empty() && bestPos != Position() && bot->GetExactDist(bestPos) > ESCAPE_FALLBACK_MAX)
    {
        LOG_INFO("playerbots", "[avoid aoe] {}: out of ({:.1f},{:.1f}) r {:.1f}: no safe spot near, the far exit "
                 "({:.1f} yd) refused", bot->GetName(), pos.GetPositionX(), pos.GetPositionY(), radius,
                 bot->GetExactDist(bestPos));
        bestPos = Position();
    }

    if (bestPos != Position())
    {
        GuardedMove const guarded = _guardedMove;
        _guardedMove = GuardedMove::AVOID;
        _avoidCenter = pos;
        _avoidRadius = radius;
        bool const moved = MoveTo(bot->GetMapId(), bestPos.GetPositionX(), bestPos.GetPositionY(),
                                  bestPos.GetPositionZ(), false, false, true, false, priority);
        _guardedMove = guarded;
        _avoidRadius = 0.0f;
        if (moved)
        {
            uint32 curTS = getMSTime();
            while (!infoList.empty())
            {
                if (infoList.size() > 10 || infoList.front().timestamp + 5000 < curTS)
                {
                    infoList.pop_front();
                }
                else
                {
                    break;
                }
            }
            infoList.push_back({pos, radius, bot->GetAngle(&bestPos), curTS});
            NoteHazard(bot, pos, radius, -1.0f);  // [hazard_melee_spot] keeps the damage the aura path noted
            return true;
        }
    }
    return false;
}

bool MovementAction::CheckLastFlee(float curAngle, std::list<FleeInfo>& infoList)
{
    uint32 curTS = getMSTime();
    curAngle = Position::NormalizeOrientation(curAngle);
    while (!infoList.empty())
    {
        if (infoList.size() > 10 || infoList.front().timestamp + 5000 < curTS)
        {
            infoList.pop_front();
        }
        else
        {
            break;
        }
    }
    for (FleeInfo& info : infoList)
    {
        // more than 5 sec
        if (info.timestamp + 5000 < curTS)
        {
            continue;
        }
        float revAngle = Position::NormalizeOrientation(info.angle + M_PI);
        // angle too close
        if (fabs(revAngle - curAngle) < M_PI / 4)
        {
            return false;
        }
    }
    return true;
}

bool CombatFormationMoveAction::isUseful()
{
    if (getMSTime() - moveInterval < uint32(lastMoveTimer))
        return false;

    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
        return false;

    return true;
}

bool CombatFormationMoveAction::Execute(Event /*event*/)
{
    float dis = AI_VALUE(float, "disperse distance");
    if (dis <= 0.0f || (!bot->IsInCombat() && botAI->HasStrategy("stay", BotState::BOT_STATE_NON_COMBAT)) ||
        (bot->IsInCombat() && botAI->HasStrategy("stay", BotState::BOT_STATE_COMBAT)))
        return false;
    Player* playerToLeave = NearestGroupMember(dis);
    if (playerToLeave && bot->GetExactDist(playerToLeave) < dis)
    {
        if (FleePosition(playerToLeave->GetPosition(), dis))
        {
            lastMoveTimer = getMSTime();
        }
    }
    return false;
}

Position CombatFormationMoveAction::AverageGroupPos(float dis, bool ranged, bool self)
{
    float averageX = 0, averageY = 0, averageZ = 0;
    int cnt = 0;
    Group* group = bot->GetGroup();
    if (!group)
    {
        return Position();
    }
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member)
            continue;

        if (!self && member == bot)
            continue;

        if (ranged && !PlayerbotAI::IsRanged(member))
            continue;

        if (!member->IsAlive() || member->GetMapId() != bot->GetMapId() || member->IsCharmed() ||
            ServerFacade::instance().GetDistance2d(bot, member) > dis)
            continue;

        averageX += member->GetPositionX();
        averageY += member->GetPositionY();
        averageZ += member->GetPositionZ();
    }
    averageX /= cnt;
    averageY /= cnt;
    averageZ /= cnt;
    return Position(averageX, averageY, averageZ);
}

float CombatFormationMoveAction::AverageGroupAngle(Unit* from, bool ranged, bool self)
{
    Group* group = bot->GetGroup();
    if (!from || !group)
    {
        return 0.0f;
    }
    // float average = 0.0f;
    float sumX = 0.0f;
    float sumY = 0.0f;
    int cnt = 0;
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member)
            continue;

        if (!self && member == bot)
            continue;

        if (ranged && !PlayerbotAI::IsRanged(member))
            continue;

        if (!member->IsAlive() || member->GetMapId() != bot->GetMapId() || member->IsCharmed() ||
            ServerFacade::instance().GetDistance2d(bot, member) > sPlayerbotAIConfig.SightDistance)
            continue;

        cnt++;
        sumX += member->GetPositionX() - from->GetPositionX();
        sumY += member->GetPositionY() - from->GetPositionY();
    }
    if (cnt == 0)
        return 0.0f;

    // unnecessary division
    // sumX /= cnt;
    // sumY /= cnt;

    return atan2(sumY, sumX);
}

Position CombatFormationMoveAction::GetNearestPosition(std::vector<Position> const& positions)
{
    Position result;
    for (Position const& pos : positions)
    {
        if (bot->GetExactDist(pos) < bot->GetExactDist(result))
            result = pos;
    }
    return result;
}

Player* CombatFormationMoveAction::NearestGroupMember(float dis)
{
    float nearestDis = 10000.0f;
    Player* result = nullptr;
    Group* group = bot->GetGroup();
    if (!group)
    {
        return result;
    }
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member || !member->IsAlive() || member == bot || member->GetMapId() != bot->GetMapId() ||
            member->IsCharmed() || ServerFacade::instance().GetDistance2d(bot, member) > dis)
            continue;
        if (nearestDis > bot->GetExactDist(member))
        {
            result = member;
            nearestDis = bot->GetExactDist(member);
        }
    }
    return result;
}

bool TankFaceAction::Execute(Event /*event*/)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    if (!bot->GetGroup())
        return false;

    if (!bot->IsWithinMeleeRange(target) || target->isMoving())
        return false;

    if (!AI_VALUE2(bool, "has aggro", "current target"))
        return false;

    // [lead231] b187 rfc-h: "tank face" was the tank's biggest in-fight walk, 3271 yd in the 8 runs (410 per run, the
    // route's optimum is 1886): it circled every mob to turn it from the group. Turning matters only for an attack
    // that hits in front of the mob (a cone: its spells, the curated cleave list); the members' melee spots are behind
    // the target already. In a led group a mob without one is not turned
    if (Leader::LedBot(bot) &&
        !(target->IsCreature() && (Leader::Knowledge::HasFrontalAttack(target->GetEntry()) ||
                                   DungeonTactics::IsKnownCleaver(bot->GetMapId(), target->GetEntry()))))
        return false;

    float averageAngle = AverageGroupAngle(target, true);

    if (averageAngle == 0.0f)
        return false;

    float deltaAngle = Position::NormalizeOrientation(averageAngle - target->GetAngle(bot));
    if (deltaAngle > M_PI)
        deltaAngle -= 2.0f * M_PI;  // -PI..PI

    float tolerable = M_PI_2;

    if (fabs(deltaAngle) > tolerable)
        return false;

    float goodAngle1 = Position::NormalizeOrientation(averageAngle + M_PI * 3 / 5);
    float goodAngle2 = Position::NormalizeOrientation(averageAngle - M_PI * 3 / 5);

    // if dist < bot->GetMeleeRange(target) / 2, target will move backward
    float dist = std::max(bot->GetExactDist(target), bot->GetMeleeRange(target) / 2) - bot->GetCombatReach() -
                 target->GetCombatReach();
    std::vector<Position> availablePos;
    float x, y, z;
    target->GetNearPoint(bot, x, y, z, 0.0f, dist, goodAngle1);
    if (bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                       bot->GetPositionZ(), x, y, z))
    {
        /// @todo: movement control now is a mess, prepare to rewrite
        std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");
        Position pos(x, y, z);
        float angle = bot->GetAngle(&pos);
        if (CheckLastFlee(angle, infoList))
        {
            availablePos.push_back(Position(x, y, z));
        }
    }
    target->GetNearPoint(bot, x, y, z, 0.0f, dist, goodAngle2);
    if (bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                       bot->GetPositionZ(), x, y, z))
    {
        std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");
        Position pos(x, y, z);
        float angle = bot->GetAngle(&pos);
        if (CheckLastFlee(angle, infoList))
        {
            availablePos.push_back(Position(x, y, z));
        }
    }
    if (availablePos.empty())
        return false;
    Position nearest = GetNearestPosition(availablePos);

    // In a dungeon the tank turns the mob only to a spot its healer sees: the nearest such candidate, else it stays
    // (a step round a pillar or a door frame put the tank out of its healer's sight).
    if (Player* healer = FindGroupHealer(bot); healer && bot->GetMap()->IsDungeon())
    {
        std::sort(availablePos.begin(), availablePos.end(),
                  [this](Position const& left, Position const& right)
                  { return bot->GetExactDist(left) < bot->GetExactDist(right); });
        float const eye = bot->GetCollisionHeight();
        auto const seen = std::find_if(availablePos.begin(), availablePos.end(),
                                       [healer, eye](Position const& spot)
                                       {
                                           return healer->IsWithinLOS(spot.GetPositionX(), spot.GetPositionY(),
                                                                      spot.GetPositionZ() + eye);
                                       });
        if (seen == availablePos.end())
            return false;

        nearest = *seen;
    }

    return MoveTo(bot->GetMapId(), nearest.GetPositionX(), nearest.GetPositionY(), nearest.GetPositionZ(), false, false,
                  false, true, MovementPriority::MOVEMENT_COMBAT);
}

bool RearFlankAction::isUseful()
{
    Unit const* target = GetTarget();
    if (!target)
        return false;

    // Need to double the front angle check to account for mirrored angle.
    bool inFront = target->HasInArc(2.f * minAngle, bot);
    // Rear check does not need to double this angle as the logic is inverted
    // and we are subtracting from 2pi.
    bool inRear = !target->HasInArc((2.f * M_PI) - maxAngle, bot);

    return inFront || inRear;
}

bool RearFlankAction::Execute(Event /*event*/)
{
    Unit const* target = GetTarget();
    if (!target)
        return false;

    float angle = frand(minAngle, maxAngle);
    float baseDistance = bot->GetMeleeRange(target) * 0.5f;
    Position leftFlank = target->GetPosition();
    Position rightFlank = target->GetPosition();
    Position* destination = nullptr;
    leftFlank.RelocatePolarOffset(angle, baseDistance + distance);
    rightFlank.RelocatePolarOffset(-angle, baseDistance + distance);

    if (bot->GetExactDist2d(leftFlank) < bot->GetExactDist2d(rightFlank))
    {
        destination = &leftFlank;
    }
    else
    {
        destination = &rightFlank;
    }

    return MoveTo(bot->GetMapId(), destination->GetPositionX(), destination->GetPositionY(),
                  destination->GetPositionZ(), false, false, false, true, MovementPriority::MOVEMENT_COMBAT);
}

Unit* BossRearFlankAction::GetTarget()
{
    return AI_VALUE2(Unit*, "find target", bossName);
}

bool DisperseSetAction::Execute(Event event)
{
    std::string const text = event.getParam();
    if (text == "disable")
    {
        RESET_AI_VALUE(float, "disperse distance");
        botAI->TellMasterNoFacing("Disable disperse");
        return true;
    }
    if (text == "enable" || text == "reset")
    {
        if (PlayerbotAI::IsMelee(bot))
        {
            SET_AI_VALUE(float, "disperse distance", DEFAULT_DISPERSE_DISTANCE_MELEE);
        }
        else
        {
            SET_AI_VALUE(float, "disperse distance", DEFAULT_DISPERSE_DISTANCE_RANGED);
        }
        float dis = AI_VALUE(float, "disperse distance");
        std::ostringstream out;
        out << "Enable disperse distance " << std::setprecision(2) << dis;
        botAI->TellMasterNoFacing(out.str());
        return true;
    }
    if (text == "increase")
    {
        float dis = AI_VALUE(float, "disperse distance");
        std::ostringstream out;
        if (dis <= 0.0f)
        {
            out << "Enable disperse first";
            botAI->TellMasterNoFacing(out.str());
            return true;
        }
        dis += 1.0f;
        SET_AI_VALUE(float, "disperse distance", dis);
        out << "Increase disperse distance to " << std::setprecision(2) << dis;
        botAI->TellMasterNoFacing(out.str());
        return true;
    }
    if (text == "decrease")
    {
        float dis = AI_VALUE(float, "disperse distance");
        dis -= 1.0f;
        if (dis <= 0.0f)
        {
            dis += 1.0f;
        }
        SET_AI_VALUE(float, "disperse distance", dis);
        std::ostringstream out;
        out << "Increase disperse distance to " << std::setprecision(2) << dis;
        botAI->TellMasterNoFacing(out.str());
        return true;
    }
    if (text.starts_with("set"))
    {
        float dis = -1.0f;
        ;
        sscanf(text.c_str(), "set %f", &dis);
        std::ostringstream out;
        if (dis < 0 || dis > 100.0f)
        {
            out << "Invalid disperse distance " << std::setprecision(2) << dis;
        }
        else
        {
            SET_AI_VALUE(float, "disperse distance", dis);
            out << "Set disperse distance to " << std::setprecision(2) << dis;
        }
        botAI->TellMasterNoFacing(out.str());
        return true;
    }
    std::ostringstream out;
    out << "Usage: disperse [enable | disable | increase | decrease | set {distance}]";
    float dis = AI_VALUE(float, "disperse distance");
    if (dis > 0.0f)
    {
        out << "(Current disperse distance: " << std::setprecision(2) << dis << ")";
    }
    botAI->TellMasterNoFacing(out.str());
    return true;
}

bool RunAwayAction::Execute(Event /*event*/) { return Flee(AI_VALUE(Unit*, "group leader")); }

bool MoveToLootAction::Execute(Event /*event*/)
{
    LootObjectStack* availableLoot = AI_VALUE(LootObjectStack*, "available loot");
    if (availableLoot->IsLootPending() || bot->GetLootGUID())
    {
        lootWatchGuid.Clear();
        return false;
    }

    LootObject loot = AI_VALUE(LootObject, "loot target");
    if (!loot.IsLootPossible(bot))
    {
        lootWatchGuid.Clear();
        return false;
    }

    WorldObject* target = loot.GetWorldObject(bot);
    if (!target)
        return false;

    float const distance = bot->GetDistance(target);
    uint32 const nowMs = getMSTime();

    constexpr uint32 evalGapMs = 10000;    // longer than the slowest out-of-combat tick (1-3 s)
    constexpr uint32 noProgressMs = 5000;  // give up on a target after 5s without progress
    constexpr float reachedMargin = 2.0f;  // near-target distance jitter margin

    if (loot.guid != lootWatchGuid || nowMs - lootWatchLastEvalMs > evalGapMs)
    {
        lootWatchGuid = loot.guid;
        lootWatchDistance = distance;
        lootWatchMs = nowMs;
    }
    else if (distance < lootWatchDistance - 0.5f)
    {
        lootWatchDistance = distance;
        lootWatchMs = nowMs;
    }
    lootWatchLastEvalMs = nowMs;

    // Drop a target the bot is not getting closer to (blocked path, pulled elsewhere), so the next
    // pickup chooses a closer reachable one.
    if (distance > sPlayerbotAIConfig.ContactDistance + reachedMargin && nowMs - lootWatchMs >= noProgressMs)
    {
        availableLoot->DeferLoot(loot.guid);
        if (AI_VALUE(LootObject, "loot target").guid == loot.guid)
            context->GetValue<LootObject>("loot target")->Set(LootObject());
        lootWatchGuid.Clear();
        return false;
    }

    return MoveNear(target, sPlayerbotAIConfig.ContactDistance);
}

bool MoveOutOfEnemyContactAction::Execute(Event /*event*/)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    return MoveTo(target, sPlayerbotAIConfig.ContactDistance);
}

bool MoveOutOfEnemyContactAction::isUseful() { return AI_VALUE2(bool, "inside target", "current target"); }

bool SetFacingTargetAction::Execute(Event /*event*/)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    if (bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return false;

    // A free action: the turn is instant, so the rotation still casts in this tick (one action per tick).
    ServerFacade::instance().SetFacingTo(bot, target);
    return false;
}

bool SetFacingTargetAction::isUseful() { return !AI_VALUE2(bool, "facing", "current target"); }

bool SetFacingTargetAction::isPossible()
{
    if (bot->isFrozen() || bot->IsPolymorphed() || (bot->isDead() && !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) ||
        bot->IsBeingTeleported() || bot->HasConfuseAura() || bot->IsCharmed() || bot->HasStunAura() ||
        bot->IsInFlight() || bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
        return false;

    return true;
}

namespace
{
    // Melee spot hazards. Local copy of RangedPositionActions' HarmfulLiquidNear pattern (a shared helper,
    // Ai/Base/Util/TerrainHazard.h, did not exist yet).
    constexpr std::array<float, 3> MELEE_HAZARD_LIQUID_RINGS = { 0.0f, 1.5f, 3.0f };
    constexpr float MELEE_HAZARD_LIQUID_HEIGHT = 3.0f;  // a harmful surface above spot z - this is at the spot's level
    constexpr int MELEE_HAZARD_DIRECTIONS = 8;
    constexpr float MELEE_HAZARD_EDGE = 2.0f;  // the ground this far round the spot ...
    constexpr float MELEE_HAZARD_DROP = 3.5f;  // ... lower than this (or none at all) is a drop-off
    constexpr uint32 MELEE_HAZARD_LOG_MS = 5 * IN_MILLISECONDS;

    // Why a melee spot is unsafe: "harmful liquid" (lava / slime within 3 yd at its level), "drop-off" (the ground
    // falls away within 2 yd); nullptr when it is safe.
    char const* MeleeSpotHazard(Player* bot, float x, float y, float z)
    {
        Map* map = bot->GetMap();
        if (!map)
            return nullptr;

        uint8 const harmful = MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME;
        for (float const ring : MELEE_HAZARD_LIQUID_RINGS)
        {
            int const directions = ring > 0.0f ? MELEE_HAZARD_DIRECTIONS : 1;
            for (int i = 0; i < directions; ++i)
            {
                float const angle = 2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / MELEE_HAZARD_DIRECTIONS;
                LiquidData const liquid = map->GetLiquidData(bot->GetPhaseMask(), x + std::cos(angle) * ring,
                                                             y + std::sin(angle) * ring, z, bot->GetCollisionHeight(),
                                                             harmful);
                if (liquid.Status != LIQUID_MAP_NO_WATER && (liquid.Flags & harmful) &&
                    liquid.Level > z - MELEE_HAZARD_LIQUID_HEIGHT)
                    return "harmful liquid";
            }
        }

        for (int i = 0; i < MELEE_HAZARD_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / MELEE_HAZARD_DIRECTIONS;
            float const ground = bot->GetMapHeight(x + std::cos(angle) * MELEE_HAZARD_EDGE,
                                                   y + std::sin(angle) * MELEE_HAZARD_EDGE, z + 1.0f);
            if (ground <= INVALID_HEIGHT || z - ground > MELEE_HAZARD_DROP)
                return "drop-off";
        }

        return nullptr;
    }

    void LogMeleeSpotHazard(Player* bot, char const* reason)
    {
        thread_local std::unordered_map<ObjectGuid, uint32> lastLogs;
        uint32 const now = getMSTime();
        uint32& last = lastLogs[bot->GetGUID()];
        if (last && getMSTimeDiff(last, now) < MELEE_HAZARD_LOG_MS)
            return;

        last = now ? now : 1;
        LOG_INFO("playerbots", "[hazard] {}: behind spot rejected ({})", bot->GetName(), reason);
    }
}

bool SetBehindTargetAction::Execute(Event /*event*/)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    if (target->GetVictim() == bot)
        return false;

    if (!bot->IsWithinMeleeRange(target) || target->isMoving())
        return false;

    float deltaAngle = Position::NormalizeOrientation(target->GetOrientation() - target->GetAngle(bot));
    if (deltaAngle > M_PI)
        deltaAngle -= 2.0f * M_PI;  // -PI..PI

    float tolerable = M_PI_2;

    if (fabs(deltaAngle) > tolerable)
        return false;

    // The tank turns its mob a little all the time: a melee that just took its spot behind stays there and hits
    // unless it is now well inside the frontal cone (wave 2026-10-24: enhancement moved in 39-50 % of samples).
    constexpr uint32 BEHIND_HOLD_MS = 2500;
    constexpr float DEEP_FRONT_ANGLE = static_cast<float>(M_PI) / 3.0f;
    uint32 const now = getMSTime();
    if (_holdUntil && static_cast<int32>(_holdUntil - now) > 0 && fabs(deltaAngle) > DEEP_FRONT_ANGLE)
        return false;

    // Well behind (153 deg), not at the flank: frontal cones reach past 90 deg on big mobs.
    float goodAngle1 = Position::NormalizeOrientation(target->GetOrientation() + M_PI * 0.85f);
    float goodAngle2 = Position::NormalizeOrientation(target->GetOrientation() - M_PI * 0.85f);

    // A cone spell hits everyone inside the caster's boundary radius (~max(reach, 2 yd)) at ANY angle, so standing
    // "behind" a yard from the centre is still in the cleave (Gnomeregan Caverndeep Reaver Arcing Smash). Stand
    // outside that radius but inside melee range.
    float const meleeRange = bot->GetMeleeRange(target);
    float const minCenter = std::max(target->GetCombatReach(), 2.0f) + 1.0f;
    float const maxCenter = meleeRange - 0.5f;
    float const centerDist = minCenter > maxCenter ? maxCenter : std::clamp(meleeRange - 1.5f, minCenter, maxCenter);
    float dist = std::max(0.0f, centerDist - bot->GetCombatReach() - target->GetCombatReach());
    std::vector<Position> availablePos;
    /// @todo: movement control now is a mess, prepare to rewrite
    std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");
    // A spot at the edge of lava or a drop-off is no spot (RFC 2026-10-24: the rogue ran behind its mob onto the rim
    // over the lava, again): rejected, the next angle is tried.
    auto const consider = [&](float spotAngle) -> bool
    {
        float x, y, z;
        target->GetNearPoint(bot, x, y, z, 0.0f, dist, spotAngle);
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), x, y, z))
            return false;

        if (char const* hazard = MeleeSpotHazard(bot, x, y, z))
        {
            LogMeleeSpotHazard(bot, hazard);
            return false;
        }

        Position pos(x, y, z);
        if (!CheckLastFlee(bot->GetAngle(&pos), infoList))
            return false;

        availablePos.push_back(pos);
        return true;
    };

    bool const firstSafe = consider(goodAngle1);
    bool const secondSafe = consider(goodAngle2);
    if (!firstSafe && !secondSafe)
    {
        // The other flanks, back to side, at the same range; none safe: it stays (a front-side hit beats a fall).
        for (float const turn : { 0.75f, -0.75f, 0.5f, -0.5f })
            consider(Position::NormalizeOrientation(target->GetOrientation() + static_cast<float>(M_PI) * turn));
    }

    if (availablePos.empty())
        return false;
    Position nearest = GetNearestPosition(availablePos);
    if (!MoveTo(bot->GetMapId(), nearest.GetPositionX(), nearest.GetPositionY(), nearest.GetPositionZ(), false, false,
                false, true, MovementPriority::MOVEMENT_COMBAT))
        return false;

    _holdUntil = now + static_cast<uint32>(IN_MILLISECONDS * MoveDelay(bot->GetExactDist(&nearest))) + BEHIND_HOLD_MS;
    return true;
}

namespace
{
    constexpr float PEEL_MOVE_RANGE = 30.0f;     // taunt range: the mobs "group member attacker count" sees
    constexpr float PEEL_MOVE_AOE_REACH = 5.0f;  // this close to their centre the area threat takes them all
    constexpr float PEEL_MOVE_MAX_STEP = 15.0f;  // a step, not a run: the pack the tank holds has to keep up
    constexpr uint32 PEEL_MOVE_MIN_MOBS = 2;
    constexpr float PEEL_MOVE_IDLE_MARGIN = 3.0f;  // the fight drifts a few yards around the spot
    constexpr float PEEL_MOVE_IDLE_SCAN = 45.0f;

    // An idle aggressive mob whose aggro reaches the spot (the core's Z gate and 3D range, IdleAggroBlocker; no line
    // of sight check, the fight drifts): stepping there pulls another pack into the fight.
    bool SpotPullsIdleMob(Player* bot, GuidVector const& possible, PlayerbotAI* botAI, float x, float y, float z)
    {
        for (ObjectGuid const guid : possible)
        {
            Creature* mob = botAI->GetCreature(guid);
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->HasReactState(REACT_AGGRESSIVE) ||
                mob->GetMapId() != bot->GetMapId() || !bot->IsHostileTo(mob) || mob->IsCivilian() ||
                mob->IsNeutralToAll() || bot->GetExactDist2d(mob) > PEEL_MOVE_IDLE_SCAN)
                continue;

            if (!MovementAction::IdleAggroBlocker(mob, bot, x, y, z, PEEL_MOVE_IDLE_MARGIN, false))
                return true;
        }

        return false;
    }
}

bool MoveToGroupMemberAttackersAction::GetAttackersCenter(Position& center)
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 count = 0;

    // The healer first: towards the mobs on it alone, however few and (up to 60 yd) however far.
    if (AI_VALUE(Unit*, "far healer attacker"))
    {
        std::vector<Creature*> healerAttackers;
        FarHealerAttackerValue::GetHealerAttackers(botAI, healerAttackers);
        for (Creature* mob : healerAttackers)
        {
            // [b32_peel] towards a mob at the healer only: one casting from range is taunted or pulled in
            if (!TankPeel::MayCloseGap(bot, mob))
                continue;

            x += mob->GetPositionX();
            y += mob->GetPositionY();
            z += mob->GetPositionZ();
            ++count;
        }

        if (count)
        {
            center.Relocate(x / count, y / count, z / count);
            return true;
        }
    }

    bool const raid = bot->GetMap() && bot->GetMap()->IsRaid();
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "attackers"))
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsControlledByPlayer() || !bot->IsWithinDistInMap(mob, PEEL_MOVE_RANGE))
            continue;

        if (raid && (mob->IsDungeonBoss() || mob->isWorldBoss()))
            continue;

        Unit* victim = mob->GetVictim();
        Player* player = victim ? victim->ToPlayer() : nullptr;
        if (!player || player == bot || !player->IsAlive() || !bot->IsInSameRaidWith(player) ||
            PlayerbotAI::IsTank(player))
            continue;

        // [b32_peel] a caster hitting the member from range is not walked to: the step would leave the pack
        if (!TankPeel::MayCloseGap(bot, mob))
            continue;

        x += mob->GetPositionX();
        y += mob->GetPositionY();
        z += mob->GetPositionZ();
        ++count;
    }

    if (count < PEEL_MOVE_MIN_MOBS)
        return false;

    center.Relocate(x / count, y / count, z / count);
    return true;
}

bool MoveToGroupMemberAttackersAction::isUseful()
{
    if (!PlayerbotAI::IsTank(bot) || botAI->HasStrategy("stay", BOT_STATE_COMBAT) ||
        bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return false;

    // in a raid the main tank stays on its boss; the off-tanks pick up the adds
    if (bot->GetMap() && bot->GetMap()->IsRaid() && PlayerbotAI::IsMainTank(bot))
        return false;

    // A straight step runs into the wall in front of a healer out of sight: "reach healer attacker" paths there.
    Unit* farHealerAttacker = AI_VALUE(Unit*, "far healer attacker");
    if (farHealerAttacker && !bot->IsWithinLOSInMap(farHealerAttacker))
        return false;

    Position center;
    return GetAttackersCenter(center) && bot->GetExactDist2d(center) > PEEL_MOVE_AOE_REACH + 1.0f;
}

bool MoveToGroupMemberAttackersAction::Execute(Event /*event*/)
{
    Position center;
    if (!GetAttackersCenter(center))
        return false;

    float step = std::min(bot->GetExactDist2d(center) - PEEL_MOVE_AOE_REACH, PEEL_MOVE_MAX_STEP);
    if (step <= 1.0f)
        return false;

    // A shorter step when the full one would bring the next pack (wave 5 Sethekk: the peel walked into it).
    // [idle_pack_no_los] every idle mob in sight range, seen or not: in the Scholomance Necromancer room the log
    // repeated "possible targets: no line of sight from the bot's eyes" and the tank's peel steps took the fight
    // 30-45 yd into the next packs (wipe analysis w24-25, cause B).
    GuidVector const possible = AI_VALUE(GuidVector, "possible targets no los");
    float const angle = bot->GetAngle(&center);
    for (uint32 attempt = 0; attempt < 3; ++attempt, step *= 0.5f)
    {
        if (step <= 1.0f)
            return false;

        float x = bot->GetPositionX() + std::cos(angle) * step;
        float y = bot->GetPositionY() + std::sin(angle) * step;
        float z = bot->GetPositionZ();
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), x, y, z))
            continue;

        // [ledge_drop_false] the end height comes from the navmesh polygon; a navmesh layer under the floor model
        // put the step under the floor (bot-n 20261025-184229-56: (-10.9,286.2,-2.9), floor about -0.4; the tank
        // fell through to -87 and died). A shorter step instead.
        if (UnderWalkableFloor(bot, x, y, z))
            continue;

        if (SpotPullsIdleMob(bot, possible, botAI, x, y, z))
            continue;

        return MoveTo(bot->GetMapId(), x, y, z, false, false, false, true, MovementPriority::MOVEMENT_COMBAT);
    }

    return false;
}

bool MoveOutOfCollisionAction::Execute(Event /*event*/)
{
    float angle = M_PI * 2000 / frand(1.f, 1000.f);
    float distance = sPlayerbotAIConfig.FollowDistance;
    return MoveTo(bot->GetMapId(), bot->GetPositionX() + cos(angle) * distance,
                  bot->GetPositionY() + sin(angle) * distance, bot->GetPositionZ());
}

bool MoveOutOfCollisionAction::isUseful()
{
    // do not avoid collision on vehicle
    if (botAI->IsInVehicle())
        return false;

    return AI_VALUE2(bool, "collision", "self target") &&
           botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest friendly players")->Get().size() < 15;
}

bool MoveRandomAction::Execute(Event /*event*/)
{
    float distance = sPlayerbotAIConfig.TooCloseDistance + urand(10, 30);

    Map* map = bot->GetMap();
    for (int i = 0; i < 3; ++i)
    {
        float x = bot->GetPositionX();
        float y = bot->GetPositionY();
        float z = bot->GetPositionZ();
        float angle = (float)rand_norm() * static_cast<float>(M_PI);
        x += urand(0, distance) * cos(angle);
        y += urand(0, distance) * sin(angle);

        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), x, y, z))
            continue;
        if (map->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
            continue;

        bool moved = MoveTo(bot->GetMapId(), x, y, z, false, false, false, true);
        if (moved)
            return true;
    }

    return false;
}

bool MoveRandomAction::isUseful() { return !AI_VALUE(GuidPosition, "rpg target"); }

bool MoveInsideAction::Execute(Event /*event*/) { return MoveInside(bot->GetMapId(), x, y, bot->GetPositionZ(), distance); }

bool RotateAroundTheCenterPointAction::Execute(Event /*event*/)
{
    uint32 next_point = GetCurrWaypoint();
    if (MoveTo(bot->GetMapId(), waypoints[next_point].first, waypoints[next_point].second, bot->GetPositionZ(), false,
               false, false, false, MovementPriority::MOVEMENT_COMBAT))
    {
        call_counters += 1;
        return true;
    }
    return false;
}

bool MoveFromGroupAction::Execute(Event event)
{
    float distance = atoi(event.getParam().c_str());
    if (!distance)
        distance = 20.0f;  // flee distance from config is too small for this
    return MoveFromGroup(distance);
}

bool MoveAwayFromCreatureAction::Execute(Event /*event*/)
{
    GuidVector targets = AI_VALUE(GuidVector, "nearest npcs");

    // Find all creatures with the specified Id
    std::vector<Unit*> creatures;
    for (auto const& guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && (alive && unit->IsAlive()) && unit->GetEntry() == creatureId)
        {
            creatures.push_back(unit);
        }
    }

    if (creatures.empty())
    {
        return false;
    }

    // Search for a safe position
    const int directions = 8;
    const float increment = 3.0f;
    float bestX = bot->GetPositionX();
    float bestY = bot->GetPositionY();
    float bestZ = bot->GetPositionZ();
    float maxSafetyScore = -1.0f;

    for (int i = 0; i < directions; ++i)
    {
        float angle = (i * 2 * M_PI) / directions;
        for (float distance = increment; distance <= 30.0f; distance += increment)
        {
            float moveX = bot->GetPositionX() + distance * cos(angle);
            float moveY = bot->GetPositionY() + distance * sin(angle);
            float moveZ = bot->GetPositionZ();

            // Check creature distance constraints
            bool isSafeFromCreatures = true;
            float minCreatureDist = std::numeric_limits<float>::max();
            for (Unit* creature : creatures)
            {
                float dist = creature->GetExactDist2d(moveX, moveY);
                if (dist < range)
                {
                    isSafeFromCreatures = false;
                    break;
                }
                if (dist < minCreatureDist)
                {
                    minCreatureDist = dist;
                }
            }

            if (isSafeFromCreatures && bot->IsWithinLOS(moveX, moveY, moveZ))
            {
                // A simple safety score: the minimum distance to any creature. Higher is better.
                if (minCreatureDist > maxSafetyScore)
                {
                    maxSafetyScore = minCreatureDist;
                    bestX = moveX;
                    bestY = moveY;
                    bestZ = moveZ;
                }
            }
        }
    }

    // Move to the best position found
    if (maxSafetyScore > 0.0f)
    {
        return MoveTo(bot->GetMapId(), bestX, bestY, bestZ, false, false, false, false,
                      MovementPriority::MOVEMENT_COMBAT);
    }

    return false;
}

bool MoveAwayFromCreatureAction::isPossible() { return bot->CanFreeMove(); }

bool MoveAwayFromPlayerWithDebuffAction::Execute(Event /*event*/)
{
    Group* const group = bot->GetGroup();

    if (!group)
        return false;

    std::vector<Player*> debuffedPlayers;

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (player && player->IsAlive() && player->HasAura(spellId))
        {
            debuffedPlayers.push_back(player);
        }
    }

    if (debuffedPlayers.empty())
    {
        return false;
    }

    // Search for a safe position
    const int directions = 8;
    const float increment = 3.0f;
    float bestX = bot->GetPositionX();
    float bestY = bot->GetPositionY();
    float bestZ = bot->GetPositionZ();
    float maxSafetyScore = -1.0f;

    for (int i = 0; i < directions; ++i)
    {
        float angle = (i * 2 * M_PI) / directions;
        for (float distance = increment; distance <= (range + 5.0f); distance += increment)
        {
            float moveX = bot->GetPositionX() + distance * cos(angle);
            float moveY = bot->GetPositionY() + distance * sin(angle);
            float moveZ = bot->GetPositionZ();

            // Check creature distance constraints
            bool isSafeFromDebuffedPlayer = true;
            float minDebuffedPlayerDistance = std::numeric_limits<float>::max();
            for (Unit* debuffedPlayer : debuffedPlayers)
            {
                float dist = debuffedPlayer->GetExactDist2d(moveX, moveY);
                if (dist < range)
                {
                    isSafeFromDebuffedPlayer = false;
                    break;
                }
                if (dist < minDebuffedPlayerDistance)
                {
                    minDebuffedPlayerDistance = dist;
                }
            }

            if (isSafeFromDebuffedPlayer && bot->IsWithinLOS(moveX, moveY, moveZ))
            {
                // A simple safety score: the minimum distance to any debuffed player. Higher is better.
                if (minDebuffedPlayerDistance > maxSafetyScore)
                {
                    maxSafetyScore = minDebuffedPlayerDistance;
                    bestX = moveX;
                    bestY = moveY;
                    bestZ = moveZ;
                }
            }
        }
    }

    // Move to the best position found
    if (maxSafetyScore > 0.0f)
    {
        return MoveTo(bot->GetMapId(), bestX, bestY, bestZ, false, false, false, false,
                      MovementPriority::MOVEMENT_COMBAT, true);
    }

    return false;
}

bool MoveAwayFromPlayerWithDebuffAction::isPossible() { return bot->CanFreeMove(); }
