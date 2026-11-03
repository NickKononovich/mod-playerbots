/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [lead9] The fight's safe zone (SafeZone.h, C:/WoW/LEADER_REFACTOR_PLAN.md step 9).

#include "SafeZone.h"
#include "BotPath.h"  // [lead109]

#include "BotMoveEvents.h"
#include "Creature.h"
#include "Group.h"
#include "LeaderOrders.h"
#include "Log.h"
#include "Map.h"
#include "MovementActions.h"
#include "MoveSplineInitArgs.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PullActions.h"  // FightHold
#include "StringFormat.h"
#include "TargetValue.h"  // IsCrowdControlled
#include "TerrainHazard.h"
#include "Timer.h"
#include "World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace
{
    constexpr std::array<float, 2> BRINK_PROBE = {3.0f, 6.0f};
    constexpr uint32 BRINK_DIRECTIONS = 8;
    constexpr uint32 BUILD_MAX_LOS = 12;          // line of sight checks one build spends on the hole exemptions
    constexpr float LEDGE_SEGMENT_DROP = 3.5f;    // a walk segment that falls this much faster than 1:1 is a ledge
    constexpr float RING_STEPS[] = {3.0f, 6.0f};  // Clamp: the spots round the asked one
    constexpr uint32 RING_DIRECTIONS = 8;
    constexpr float RING_MAX_DZ = 4.0f;
    constexpr uint32 BACK_OFF_SAMPLES = 4;        // Clamp: a stop along the walk that is no stand steps back this often
    constexpr uint32 CHARGE_LOG_MS = 5 * IN_MILLISECONDS;

    std::mutex zoneLock;
    std::unordered_map<uint64, Leader::FightPlan> zones;  // per tracked tank

    float Dist2d(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

    bool IdleHostile(Creature* mob, Player* tank)
    {
        return mob && mob->IsInWorld() && mob->IsAlive() && !mob->IsInCombat() && !mob->IsInEvadeMode() &&
               mob->GetMap() == tank->GetMap() && tank->IsHostileTo(mob) && mob->HasReactState(REACT_AGGRESSIVE) &&
               !mob->IsCivilian() && !mob->IsNeutralToAll() && !IsCrowdControlled(mob);
    }

    // The walker's eyes at the point, from the mob's (as the idle-mob path guard's SeesWalkerAt).
    bool SeesAt(Creature* mob, Unit* walker, float x, float y, float z)
    {
        return mob->IsWithinLOS(x, y, z + walker->GetCollisionHeight() - mob->GetCollisionHeight());
    }

    // The walk the walker takes to `to`: its navmesh path, the straight line for a short hop or with no path.
    Movement::PointsArray WalkPoints(Unit* walker, Position const& to, bool& searched)
    {
        G3D::Vector3 const from(walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ());
        G3D::Vector3 const end(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ());
        searched = false;
        if ((end - from).length() > Leader::SAFE_ZONE_SHORT_HOP && !walker->IsFlying())
        {
            searched = true;
            BotPath const path = FindBotPath(walker, end.x, end.y, end.z);  // [lead109]
            if (!(path.type & PATHFIND_NOPATH) && path.points.size() >= 2)
                return path.points;
        }

        return Movement::PointsArray{from, end};
    }

    // How far along `points` the walk stays inside (samples every SAFE_ZONE_SAMPLE yd, no closed door, no ledge):
    // `lastGood` the last inside sample, `walkedGood` its distance along; true when the whole walk is inside.
    bool InsideUntil(Leader::SafeZone const& zone, Unit* walker, Movement::PointsArray const& points,
                     Leader::ZoneRole role, std::vector<Position>* samplesOut, float& walkedGood, std::string& why)
    {
        Position const from = walker->GetPosition();
        walkedGood = 0.0f;
        if (points.size() < 2)
            return true;

        float doorAt = -1.0f;
        G3D::Vector3 const& end = points.back();
        if (ClosedDoorNear(walker, from.GetPositionX(), from.GetPositionY(), end.x, end.y, CLOSED_DOOR_SEARCH))
            if (ClosedDoorHit const hit = PathThroughClosedDoor(walker, points);
                hit.Found() && !DoorOpensByUse(hit.door))
                doorAt = hit.walked;

        float walked = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
        {
            G3D::Vector3 const& a = points[i - 1];
            G3D::Vector3 const& b = points[i];
            G3D::Vector3 const segment = b - a;
            float const length = segment.length();
            float const flat = std::hypot(segment.x, segment.y);
            if (a.z - b.z > LEDGE_SEGMENT_DROP && a.z - b.z > flat)
            {
                why = Acore::StringFormat("a drop of {:.1f} yd {:.0f} yd along", a.z - b.z, walked);
                return false;
            }

            uint32 const steps = std::max(1u, static_cast<uint32>(std::ceil(length / Leader::SAFE_ZONE_SAMPLE)));
            for (uint32 step = 1; step <= steps; ++step)
            {
                float const along = static_cast<float>(step) / steps;
                float const at = walked + length * along;
                if (doorAt >= 0.0f && at >= doorAt - Leader::SAFE_ZONE_SAMPLE)
                {
                    why = Acore::StringFormat("a closed door {:.0f} yd along", doorAt);
                    return false;
                }

                G3D::Vector3 const p = a + segment * along;
                Position const sample(p.x, p.y, p.z);
                bool const last = i + 1 == points.size() && step == steps;
                std::string sampleWhy;
                if (!zone.Contains(sample, role, &from, &sampleWhy, last ? 0.5f : Leader::SAFE_ZONE_PATH_SLACK))
                {
                    why = sampleWhy + Acore::StringFormat(" {:.0f} yd along", at);
                    return false;
                }

                walkedGood = at;
                if (samplesOut)
                    samplesOut->push_back(sample);
            }

            walked += length;
        }

        return true;
    }

    Leader::SafeZone BuildZone(Player* bot, Player* tank, Position const& anchor, uint32 orderId)
    {
        Leader::SafeZone zone;
        zone.valid = true;
        zone.tank = tank->GetGUID();
        zone.orderId = orderId;
        zone.centre = anchor;
        zone.builtMs = getMSTime() | 1u;

        // the tank's own view of the room (the group's lead), else the asking member's
        PlayerbotAI* ai = GET_PLAYERBOT_AI(tank);
        if (!ai)
            ai = GET_PLAYERBOT_AI(bot);
        if (!ai)
            return zone;

        float const assist = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        uint32 losBudget = BUILD_MAX_LOS;
        for (ObjectGuid const& guid : ai->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
        {
            Creature* mob = ObjectAccessor::GetCreature(*tank, guid);
            if (!IdleHostile(mob, tank) || mob->GetExactDist2d(&anchor) > Leader::SAFE_ZONE_HOLE_SCAN)
                continue;

            float r = MovementAction::IdleAggroReach(mob, tank, Leader::SAFE_ZONE_HOLE_MARGIN);
            if (assist > 0.0f)
                r = std::max(r, assist + Leader::SAFE_ZONE_ASSIST_SLACK);

            // Seen within its reach and still idle: the tank where it stands, the fight spot. As near as they stand,
            // no nearer (the path guard's [melee_behind_tank] exemption).
            float const realReach = MovementAction::IdleAggroReach(mob, tank, 0.0f);
            float const tankDistance = mob->GetExactDist(tank);
            if (tankDistance < realReach && MovementAction::PassesAggroZGate(mob, tank, tank->GetPositionZ()) &&
                losBudget)
            {
                --losBudget;
                if (mob->IsWithinLOSInMap(tank))
                    r = std::min(r, tankDistance - Leader::SAFE_ZONE_EXEMPT_BEHIND);
            }
            float const anchorDistance = mob->GetExactDist(&anchor);
            if (anchorDistance < r && anchorDistance < realReach &&
                MovementAction::PassesAggroZGate(mob, tank, anchor.GetPositionZ()) && losBudget)
            {
                --losBudget;
                if (SeesAt(mob, tank, anchor.GetPositionX(), anchor.GetPositionY(), anchor.GetPositionZ()))
                    r = std::min(r, anchorDistance - Leader::SAFE_ZONE_EXEMPT_BEHIND);
            }
            if (r <= Leader::SAFE_ZONE_EXEMPT_BEHIND)
                continue;
            // [lead29] a mob of the committed pull (pull together) is no hole: it is coming
            if (Leader::PullPack::Contains(tank, guid))
                continue;

            Leader::ZoneHole hole;
            hole.mob = guid;
            hole.name = mob->GetName();
            hole.entry = mob->GetEntry();
            hole.x = mob->GetPositionX();
            hole.y = mob->GetPositionY();
            hole.z = mob->GetPositionZ();
            hole.r = r;
            hole.zGate = CREATURE_Z_ATTACK_RANGE + mob->m_CombatDistance + mob->GetObjectSize() + tank->GetObjectSize();
            hole.flyer = mob->CanFly();
            zone.holes.push_back(hole);
        }

        // [b47_idle] / [fight_hold_stay_caster] a mob fighting the group that stands still out there (a caster
        // holding its range) never comes to the spot: the way to it is part of the fight
        for (ObjectGuid const& guid : ai->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
        {
            Unit* unit = ObjectAccessor::GetUnit(*tank, guid);
            Creature* mob = unit ? unit->ToCreature() : nullptr;
            if (!mob || !mob->IsAlive() || !mob->IsInCombat() || mob->isMoving() || mob->IsDungeonBoss() ||
                mob->isWorldBoss() || mob->GetMap() != tank->GetMap() ||
                mob->GetExactDist2d(&anchor) > Leader::SAFE_ZONE_LANE_MAX)
                continue;

            Unit* victim = mob->GetVictim();
            Player* victimPlayer = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (!victimPlayer || (victimPlayer != tank && !victimPlayer->IsInSameGroupWith(tank)))
                continue;

            Leader::ZoneLane lane;
            lane.mob = guid;
            lane.x = mob->GetPositionX();
            lane.y = mob->GetPositionY();
            lane.z = mob->GetPositionZ();
            lane.halfWidth = mob->GetCombatReach() + Leader::SAFE_ZONE_LANE_WIDTH;
            zone.lanes.push_back(lane);
        }

        return zone;
    }

    bool ThrottleDue(std::unordered_map<uint64, std::pair<uint32, uint32>>& slots, ObjectGuid guid, uint32 everyMs,
                     uint32& skipped)
    {
        uint32 const now = getMSTime();
        std::pair<uint32, uint32>& slot = slots[guid.GetRawValue()];
        if (slot.first && getMSTimeDiff(slot.first, now) < everyMs)
        {
            ++slot.second;
            return false;
        }

        skipped = slot.second;
        slot = {now ? now : 1, 0};
        return true;
    }
}

namespace Leader
{
    char const* ZoneRoleName(ZoneRole role)
    {
        switch (role)
        {
            case ZoneRole::Tank:
                return "tank";
            case ZoneRole::Melee:
                return "melee";
            case ZoneRole::Ranged:
                return "ranged";
            case ZoneRole::Healer:
                return "healer";
        }
        return "?";
    }

    float SafeZone::RadiusFor(ZoneRole role) const
    {
        switch (role)
        {
            case ZoneRole::Tank:
                return tankRadius;
            case ZoneRole::Melee:
                return meleeRadius;
            case ZoneRole::Ranged:
                return rangedRadius;
            case ZoneRole::Healer:
                return healerRadius;
        }
        return tankRadius;
    }

    bool SafeZone::Contains(Position const& p, ZoneRole role, Position const* from, std::string* why,
                            float fromSlack) const
    {
        if (!valid)
            return true;

        float const cx = centre.GetPositionX();
        float const cy = centre.GetPositionY();
        float const cz = centre.GetPositionZ();
        float const d = Dist2d(p.GetPositionX(), p.GetPositionY(), cx, cy);

        bool inLane = false;
        float laneZ = cz;
        for (ZoneLane const& lane : lanes)
        {
            float const vx = lane.x - cx;
            float const vy = lane.y - cy;
            float const lengthSq = vx * vx + vy * vy;
            float t = 0.0f;
            if (lengthSq > 0.0f)
                t = std::clamp(((p.GetPositionX() - cx) * vx + (p.GetPositionY() - cy) * vy) / lengthSq, 0.0f, 1.0f);
            if (Dist2d(p.GetPositionX(), p.GetPositionY(), cx + vx * t, cy + vy * t) <= lane.halfWidth)
            {
                inLane = true;
                laneZ = cz + (lane.z - cz) * t;
                break;
            }
        }

        float const fromD = from ? Dist2d(from->GetPositionX(), from->GetPositionY(), cx, cy) : 0.0f;
        float const radius = RadiusFor(role);
        // [lead530] b212 rfc-h 5A 949-1256 s: a Voidwalker Minion on the healer (it had no path to her, the core
        // evaded every hit on it) stood 27 yd from the fight spot; the tank's "reach melee" to it was clamped at the
        // 15 yd tank disc ~170 times (to the disc's edge and back), it never reached it: 300 s of fight, 21 aggro
        // losses. The tank disc stood for "the fight is not dragged into an idle pack" - the holes below are that
        // fact; the tank, which must reach every mob of the fight, has no disc
        if (role != ZoneRole::Tank && d > radius && !inLane && !(from && d <= fromD + fromSlack))
        {
            if (why)
                *why = Acore::StringFormat("{:.0f} yd from the fight spot (the {} disc is {:.0f})", d,
                                           ZoneRoleName(role), radius);
            return false;
        }

        float const dz = std::fabs(p.GetPositionZ() - cz);
        bool const floorOk = dz <= floorDz + SAFE_ZONE_FLOOR_SLOPE * d ||
                             (inLane && std::fabs(p.GetPositionZ() - laneZ) <= floorDz) ||
                             (from && dz <= std::fabs(from->GetPositionZ() - cz) + 1.0f);
        if (!floorOk)
        {
            if (why)
                *why = Acore::StringFormat("another level ({:+.1f} yd off the fight spot's floor)",
                                           p.GetPositionZ() - cz);
            return false;
        }

        for (ZoneHole const& hole : holes)
        {
            if (!hole.flyer && std::fabs(hole.z - p.GetPositionZ()) > hole.zGate)
                continue;  // out of its vertical reach (a brink over it is Standable's)

            float const dx = p.GetPositionX() - hole.x;
            float const dy = p.GetPositionY() - hole.y;
            float const dzh = p.GetPositionZ() - hole.z;
            float const distance = std::sqrt(dx * dx + dy * dy + dzh * dzh);
            float r = hole.r;
            if (from)
            {
                float const fx = from->GetPositionX() - hole.x;
                float const fy = from->GetPositionY() - hole.y;
                float const fz = from->GetPositionZ() - hole.z;
                float const fromDistance = std::sqrt(fx * fx + fy * fy + fz * fz);
                if (fromDistance < r && (hole.flyer || std::fabs(fz) <= hole.zGate))
                    r = std::min(r, fromDistance - 0.25f);  // it stands there: no nearer
            }

            if (distance < r)
            {
                if (why)
                    *why = Acore::StringFormat("in the reach of idle {} {} ({:.1f} yd of {:.1f})", hole.name,
                                               hole.entry, distance, hole.r);
                return false;
            }
        }

        return true;
    }

    bool SafeZone::Standable(Unit* walker, Position const& p, std::string* why) const
    {
        if (!valid || !walker || !walker->IsInWorld())
            return true;

        // [b43_move] the brink of a floor over an idle mob below that sees it (a knocked / feared bot, a summon or a
        // caster at the edge wakes the room below)
        int8 brink = -1;
        for (ZoneHole const& hole : holes)
        {
            float const above = p.GetPositionZ() - hole.z;
            if (hole.flyer || above <= hole.zGate || above > SAFE_ZONE_BRINK_MAX_DZ ||
                Dist2d(p.GetPositionX(), p.GetPositionY(), hole.x, hole.y) >= hole.r)
                continue;

            if (brink < 0)
            {
                brink = 0;
                for (float const probe : BRINK_PROBE)
                    for (uint32 i = 0; i < BRINK_DIRECTIONS && !brink; ++i)
                    {
                        float const angle = 2.0f * static_cast<float>(M_PI) * i / BRINK_DIRECTIONS;
                        float const height = walker->GetMapHeight(p.GetPositionX() + std::cos(angle) * probe,
                                                                  p.GetPositionY() + std::sin(angle) * probe,
                                                                  p.GetPositionZ() + 1.0f);
                        brink = height < p.GetPositionZ() - SAFE_ZONE_BRINK_DROP ? 1 : 0;
                    }
            }
            if (!brink)
                break;

            Creature* mob = ObjectAccessor::GetCreature(*walker, hole.mob);
            if (mob && SeesAt(mob, walker, p.GetPositionX(), p.GetPositionY(), p.GetPositionZ()))
            {
                if (why)
                    *why = Acore::StringFormat("the brink over idle {} {} ({:.1f} yd below)", hole.name, hole.entry,
                                               above);
                return false;
            }
        }

        // no stand in a doorway (the door shuts on half the group, the next room behind it)
        if (DoorNear(walker, p.GetPositionX(), p.GetPositionY(), p.GetPositionZ(), SAFE_ZONE_DOOR_KEEP) &&
            !DoorNear(walker, walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ(),
                      SAFE_ZONE_DOOR_KEEP))
        {
            if (why)
                *why = "a doorway";
            return false;
        }

        return true;
    }

    bool SafeZone::PathInside(Unit* walker, Position const& to, ZoneRole role, std::string* why) const
    {
        if (!valid || !walker)
            return true;

        bool searched = false;
        Movement::PointsArray const points = WalkPoints(walker, to, searched);
        float walkedGood = 0.0f;
        std::string reason;
        bool const inside = InsideUntil(*this, walker, points, role, nullptr, walkedGood, reason);
        if (!inside && why)
            *why = "the walk: " + reason;
        return inside;
    }

    bool SafeZone::Clamp(Unit* walker, Position const& want, ZoneRole role, Position& out, std::string& how,
                         std::function<bool(Position const&)> const& reject) const
    {
        if (!valid || !walker)
        {
            out = want;
            how = "no zone";
            return true;
        }

        Position const from = walker->GetPosition();
        uint32 paths = SAFE_ZONE_PATH_BUDGET;
        std::string lastWhy;
        auto const usable = [&](Position const& p) -> bool
        {
            if (reject && reject(p))
            {
                lastWhy = "inside what it leaves";
                return false;
            }
            if (walker->GetExactDist(&p) < SAFE_ZONE_MIN_STEP)
            {
                lastWhy = "no step left";
                return false;
            }
            return Contains(p, role, &from, &lastWhy) && Standable(walker, p, &lastWhy);
        };

        // 1. as far along the walk as the zone allows (the chase's edge, the shorter stop)
        {
            bool searched = false;
            Movement::PointsArray const points = WalkPoints(walker, want, searched);
            if (searched && paths)
                --paths;
            std::vector<Position> samples;
            float walkedGood = 0.0f;
            std::string reason;
            InsideUntil(*this, walker, points, role, &samples, walkedGood, reason);
            for (uint32 back = 0; back <= BACK_OFF_SAMPLES && !samples.empty(); ++back)
            {
                Position const& stop = samples.back();
                if (usable(stop))
                {
                    out = stop;
                    how = reason.empty() ? std::string("along the walk, short of the spot")
                                         : Acore::StringFormat("along the walk ({:.0f} yd, then {})", walkedGood,
                                                               reason);
                    return true;
                }
                samples.pop_back();
            }
            if (!reason.empty())
                lastWhy = reason;
        }

        // 2. the role disc's edge toward the asked spot
        float const cx = centre.GetPositionX();
        float const cy = centre.GetPositionY();
        float const d = Dist2d(want.GetPositionX(), want.GetPositionY(), cx, cy);
        float const radius = RadiusFor(role) - 0.5f;
        if (role != ZoneRole::Tank && d > radius && d > 0.0f && paths)  // [lead530] no tank disc
        {
            float const scale = radius / d;
            float ex = cx + (want.GetPositionX() - cx) * scale;
            float ey = cy + (want.GetPositionY() - cy) * scale;
            float ez = centre.GetPositionZ() + (want.GetPositionZ() - centre.GetPositionZ()) * scale;
            walker->UpdateAllowedPositionZ(ex, ey, ez);
            Position const edge(ex, ey, ez);
            if (usable(edge))
            {
                --paths;
                if (PathInside(walker, edge, role, &lastWhy))
                {
                    out = edge;
                    how = "the disc's edge";
                    return true;
                }
            }
        }

        // 3. a spot round the asked one, nearest first
        std::vector<Position> ring;
        for (float const step : RING_STEPS)
            for (uint32 i = 0; i < RING_DIRECTIONS; ++i)
            {
                float const angle = 2.0f * static_cast<float>(M_PI) * i / RING_DIRECTIONS;
                float rx = want.GetPositionX() + std::cos(angle) * step;
                float ry = want.GetPositionY() + std::sin(angle) * step;
                float rz = want.GetPositionZ();
                walker->UpdateAllowedPositionZ(rx, ry, rz);
                if (std::fabs(rz - want.GetPositionZ()) > RING_MAX_DZ)
                    continue;
                Position const spot(rx, ry, rz);
                if (usable(spot))
                    ring.push_back(spot);
            }
        std::sort(ring.begin(), ring.end(),
                  [&want](Position const& a, Position const& b)
                  { return a.GetExactDist(&want) < b.GetExactDist(&want); });
        for (Position const& spot : ring)
        {
            if (!paths)
                break;
            --paths;
            if (PathInside(walker, spot, role, &lastWhy))
            {
                out = spot;
                how = Acore::StringFormat("round the asked spot ({:.0f} yd off)", spot.GetExactDist(&want));
                return true;
            }
        }

        how = lastWhy.empty() ? std::string("no spot inside") : "no spot inside (" + lastWhy + ")";
        return false;
    }

    namespace FightZone
    {
        bool Applies(Player* bot, Player** tankOut)
        {
            if (!LEADER_SAFE_ZONE || !bot || !bot->IsInWorld() || !bot->IsAlive() || bot->InBattleground())
                return false;

            Group* group = bot->GetGroup();
            Map* map = bot->GetMap();
            if (!group || group->isRaidGroup() || !map || !map->IsNonRaidDungeon())
                return false;

            ObjectGuid const leaderGuid = group->GetLeaderGUID();
            if (leaderGuid.IsEmpty() || !Orders::IsTracked(leaderGuid))
                return false;

            Player* tank = leaderGuid == bot->GetGUID() ? bot : ObjectAccessor::GetPlayer(*bot, leaderGuid);
            if (!tank || !tank->IsInWorld() || !tank->IsAlive() || tank->GetMap() != map)
                return false;

            if (tankOut)
                *tankOut = tank;
            return true;
        }

        ZoneRole RoleOf(Player* bot)
        {
            if (PlayerbotAI::IsTank(bot))
                return ZoneRole::Tank;
            if (PlayerbotAI::IsHeal(bot))
                return ZoneRole::Healer;
            if (PlayerbotAI::IsRanged(bot))
                return ZoneRole::Ranged;
            return ZoneRole::Melee;
        }

        bool GetPlan(Player* bot, FightPlan& plan)
        {
            Player* tank = nullptr;
            if (!Applies(bot, &tank) || !tank->IsInCombat())
                return false;

            // the leader's fight plan when the open order carries one, else FightHold's anchor (the default zone)
            PullOrder order;
            bool const open = Orders::GetOpen(tank->GetGUID(), order);
            bool const ordered = open && order.fight.zone.valid;
            Position anchor;
            if (ordered)
                anchor = order.fight.zone.centre;
            else if (!FightHold::Get(tank, anchor))
                return false;

            uint32 const now = getMSTime();
            uint64 const key = tank->GetGUID().GetRawValue();
            {
                std::lock_guard<std::mutex> guard(zoneLock);
                auto const found = zones.find(key);
                if (found != zones.end() && found->second.zone.valid &&
                    getMSTimeDiff(found->second.zone.builtMs, now) < SAFE_ZONE_REBUILD_MS &&
                    found->second.zone.centre.GetExactDist(&anchor) <= SAFE_ZONE_MOVED &&
                    found->second.zone.orderId == (open ? order.id : 0u))
                {
                    plan = found->second;
                    return true;
                }
            }

            FightPlan built;
            if (ordered)
                built = order.fight;
            built.fightSpot = anchor;
            built.zone = BuildZone(bot, tank, anchor, open ? order.id : 0);
            if (ordered)
            {
                built.zone.tankRadius = order.fight.zone.tankRadius;
                built.zone.meleeRadius = order.fight.zone.meleeRadius;
                built.zone.rangedRadius = order.fight.zone.rangedRadius;
                built.zone.healerRadius = order.fight.zone.healerRadius;
            }

            {
                std::lock_guard<std::mutex> guard(zoneLock);
                if (zones.size() > 256)
                    zones.clear();
                zones[key] = built;
            }
            plan = std::move(built);
            return true;
        }

        bool Get(Player* bot, SafeZone& zone)
        {
            FightPlan plan;
            if (!GetPlan(bot, plan))
                return false;
            zone = std::move(plan.zone);
            return zone.valid;
        }

        bool ExemptMover(Player* bot, std::string const& action)
        {
            if (action == "test runner move")
                return true;  // the leader's own waypoint walk

            // the tank's pull and the walks that move the fight spot itself (the zone moves with it)
            static constexpr std::string_view TANK_EXEMPT[] = {"reach pull", "move to los pull spot",
                                                                "move fight spot back", "return to pull position"};
            return PlayerbotAI::IsTank(bot) &&
                   std::find(std::begin(TANK_EXEMPT), std::end(TANK_EXEMPT), action) != std::end(TANK_EXEMPT);
        }

        bool GuardsIdleTankWalk(Player* bot, std::string const& action)
        {
            if (!LEADER_SAFE_ZONE_IDLE_TANK || !bot || bot->IsInCombat())
                return false;

            Player* tank = nullptr;
            if (!Applies(bot, &tank) || tank != bot || Orders::IsOpen(bot->GetGUID()))
                return false;  // an open order owns the tank's walks (its pull)

            // the leader's waypoint, the planner's commit walk (the order opens right after it), the pull itself
            static constexpr std::string_view EXEMPT[] = {"test runner move",     "aggressive target",
                                                          "reach pull",           "move to los pull spot",
                                                          "move fight spot back", "return to pull position",
                                                          "pre pull cc"};
            // [lead13] ... and a dungeon room tactic's own pull walk ("mgt selin room pull": MgT's Selin room owns
            // the pull, SelinRoom::OwnsPull): mgt-n / mgt-h b56 stood 113 s+ at "no walk / not arrived" while this
            // guard refused the walk to the tactic's pull spot ("would wake Wretched Bruiser (17.7 yd of reach
            // 20.5) -> stay", every tick); the tactic checks its spot against the other packs' reach itself
            if (std::find(std::begin(EXEMPT), std::end(EXEMPT), action) != std::end(EXEMPT) ||
                action.find("escort") != std::string::npos ||
                (action.size() > 5 && action.compare(action.size() - 5, 5, " pull") == 0))
                return false;

            return true;
        }

        bool GapCloserLeavesZone(Unit* caster, Unit* target, char const* what)
        {
            Player* bot = caster ? caster->ToPlayer() : nullptr;
            if (!bot || !target || !PlayerbotAI::IsTank(bot) || target->GetMap() != bot->GetMap() ||
                !Applies(bot))
                return false;

            Position const from = bot->GetPosition();
            Position const land = target->GetPosition();
            std::string why;
            if (bot->IsInCombat())
            {
                SafeZone zone;
                if (!Get(bot, zone))
                    return false;

                if (!target->IsInCombat())
                    why = "a new idle target in a fight";
                else if (!zone.Contains(land, ZoneRole::Tank, &from, &why))
                    why = "it lands out of the zone: " + why;
                else
                {
                    // the line it flies along stays inside too
                    float const length = bot->GetExactDist(&land);
                    uint32 const steps = std::max(1u, static_cast<uint32>(std::ceil(length / SAFE_ZONE_SAMPLE)));
                    for (uint32 step = 1; step < steps && why.empty(); ++step)
                    {
                        float const along = static_cast<float>(step) / steps;
                        Position const p(from.GetPositionX() + (land.GetPositionX() - from.GetPositionX()) * along,
                                         from.GetPositionY() + (land.GetPositionY() - from.GetPositionY()) * along,
                                         from.GetPositionZ() + (land.GetPositionZ() - from.GetPositionZ()) * along);
                        std::string sampleWhy;
                        if (!zone.Contains(p, ZoneRole::Tank, &from, &sampleWhy))
                            why = "its line leaves the zone: " + sampleWhy;
                    }
                }
            }
            else
            {
                if (Orders::IsOpen(bot->GetGUID()))
                    return false;  // the order's pull

                PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
                if (!ai)
                    return false;
                for (ObjectGuid const& guid :
                     ai->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
                {
                    if (guid == target->GetGUID())
                        continue;
                    Creature* mob = ObjectAccessor::GetCreature(*bot, guid);
                    if (!IdleHostile(mob, bot) || mob->GetExactDist(&land) > 60.0f)
                        continue;
                    if (!MovementAction::IdleAggroBlocker(mob, bot, land.GetPositionX(), land.GetPositionY(),
                                                          land.GetPositionZ(), SAFE_ZONE_HOLE_MARGIN, true))
                    {
                        why = Acore::StringFormat("no order open, it lands in the reach of idle {} {}",
                                                  mob->GetName(), mob->GetEntry());
                        break;
                    }
                }
            }

            if (why.empty())
                return false;

            thread_local std::unordered_map<uint64, std::pair<uint32, uint32>> logged;
            uint32 skipped = 0;
            if (ThrottleDue(logged, bot->GetGUID(), CHARGE_LOG_MS, skipped))
                LOG_INFO("playerbots", "[lead9] zone_charge: {}: no {} onto {} {}: {}{}", bot->GetName(),
                         what ? what : "gap closer", target->GetName(), target->GetEntry(), why,
                         skipped ? " (+" + std::to_string(skipped) + " not logged)" : std::string());
            return true;
        }

        void NoteClamp(Player* bot, std::string const& mover, Position const& want, Position const* got,
                       std::string const& why)
        {
            if (!bot)
                return;

            thread_local std::unordered_map<uint64, std::pair<uint32, uint32>> logged;
            uint32 skipped = 0;
            if (!ThrottleDue(logged, bot->GetGUID(), SAFE_ZONE_LOG_MS, skipped))
                return;

            std::string const result =
                got ? Acore::StringFormat("({:.1f},{:.1f},{:.1f})", got->GetPositionX(), got->GetPositionY(),
                                          got->GetPositionZ())
                    : std::string("stay");
            LOG_INFO("playerbots", "[lead9] zone_clamp: {} ({}): {} to ({:.1f},{:.1f},{:.1f}) -> {}: {}{}",
                     bot->GetName(), ZoneRoleName(RoleOf(bot)), mover, want.GetPositionX(), want.GetPositionY(),
                     want.GetPositionZ(), result, why,
                     skipped ? " (+" + std::to_string(skipped) + " not logged)" : std::string());

            BotMoveEvents::Event event;
            event.type = "zone_clamp";
            event.Str("mover", mover)
                .Str("role", ZoneRoleName(RoleOf(bot)))
                .Str("got", got ? "clamped" : "stay")
                .Str("why", why)
                .Real("x", bot->GetPositionX())
                .Real("y", bot->GetPositionY())
                .Real("z", bot->GetPositionZ())
                .Real("toX", want.GetPositionX())
                .Real("toY", want.GetPositionY())
                .Real("toZ", want.GetPositionZ())
                .Num("skipped", skipped);
            if (got)
                event.Real("gotX", got->GetPositionX()).Real("gotY", got->GetPositionY()).Real("gotZ",
                                                                                             got->GetPositionZ());
            BotMoveEvents::Push(bot->GetGUID(), std::move(event));
        }
    }
}
