/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SAFEZONE_H
#define PLAYERBOTS_SAFEZONE_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <functional>
#include <string>
#include <vector>

class Player;
class Unit;

// [lead9] C:/WoW/LEADER_REFACTOR_PLAN.md step 9 (sections 1.9, 2.8): the fight's safe zone for the reflex movers.
//
// User intent: the leader defines a safe zone for each fight. It stays out of every idle mob's aggro + assist reach,
// has no ledges and no doorways, and may carry per-role spots (the healer sees the tank and the fight spot, ranged
// see the fight spot). The bots keep their reflexes (AoE dodge, ranged repositioning, line of sight fixes, flee,
// follow, chase), but every destination must be inside the zone; with no spot inside that works the bot stays and
// takes the hit.
//
// The zone of a fight is a disc round the fight spot (FightHold's anchor, or the open order's FightPlan when the
// leader published one), one radius per role, plus a lane to every mob fighting the group that stands still (a caster
// holding its range never comes: [b47_idle], [fight_hold_stay_caster]), minus a hole per idle mob (its proximity
// aggro reach on the tank plus the guard margin, or the assistance radius an engaged mob calls it from, whichever is
// larger), minus the brink over an idle mob below ([b43_move] fight_hold_edge_below) and the floor of another level,
// minus doorways and closed doors. A hole that the tank or the fight spot already stands in (in the mob's sight, the
// mob still idle) shrinks to that distance, as the idle-mob path guard's exemption does; a walker that stands in a
// hole may walk away from the mob, never nearer.
//
// Only groups the harness leads are affected (Leader::Orders::IsTracked on the group leader, which is the tank):
// out of harness runs nothing changes. Switches: Leader::LEADER_SAFE_ZONE (LeaderOrders.h).
namespace Leader
{
    enum class ZoneRole : uint8
    {
        Tank,
        Melee,
        Ranged,
        Healer
    };

    char const* ZoneRoleName(ZoneRole role);

    // The zone's radius per role round the fight spot. Tank and melee are FightHold::RADIUS (15) and RADIUS +
    // MELEE_SLACK (23), the radius authority they replace; ranged and healers were never held: their discs keep the
    // spell / heal range of a pack held at the spot. [lead530] Contains / Clamp apply no tank disc (the holes are what
    // it stood for: the tank reaches every mob of the fight); the radius is the escape room's for a tank only.
    constexpr float SAFE_ZONE_TANK_RADIUS = 15.0f;
    constexpr float SAFE_ZONE_MELEE_RADIUS = 23.0f;
    constexpr float SAFE_ZONE_RANGED_RADIUS = 30.0f;
    constexpr float SAFE_ZONE_HEALER_RADIUS = 34.0f;
    // A hole: the idle mob's aggro reach on the tank plus this margin (MovementAction::SIGHT_IDLE_AGGRO_MARGIN) ...
    constexpr float SAFE_ZONE_HOLE_MARGIN = 1.5f;
    // ... or the creature assistance radius (CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS: an engaged mob calls the idle
    // ones round it every assistance period) plus this much, the room an engaged mob stands from its victim.
    constexpr float SAFE_ZONE_ASSIST_SLACK = 3.0f;
    // A hole the tank / the fight spot stands in shrinks to this much nearer to the mob than they stand.
    constexpr float SAFE_ZONE_EXEMPT_BEHIND = 0.5f;
    // Idle mobs this far (2d) from the fight spot are looked at.
    constexpr float SAFE_ZONE_HOLE_SCAN = 75.0f;
    // A lane to a still mob fighting the group: at most this far from the spot, this much wider than its reach.
    constexpr float SAFE_ZONE_LANE_MAX = 45.0f;
    constexpr float SAFE_ZONE_LANE_WIDTH = 4.0f;
    // One floor: a point within FLOOR_DZ + FLOOR_SLOPE x its 2d distance from the spot's height (a ramp is walked, a
    // floor right below or above is another level).
    constexpr float SAFE_ZONE_FLOOR_DZ = 8.0f;
    constexpr float SAFE_ZONE_FLOOR_SLOPE = 0.5f;
    // The walk is judged every this many yards; a walk shorter than SHORT_HOP that stays inside is judged straight.
    constexpr float SAFE_ZONE_SAMPLE = 2.0f;
    constexpr float SAFE_ZONE_SHORT_HOP = 6.0f;
    // A walk's samples between its start and its end may stand this much farther out of the disc than the walker
    // (never its end, never in a hole): the way back in from outside may bend round a pillar.
    constexpr float SAFE_ZONE_PATH_SLACK = 3.0f;
    // A clamped destination that walks less than this is no move: the bot stays.
    constexpr float SAFE_ZONE_MIN_STEP = 1.5f;
    // No destination this close (2d) to a door (open or closed): no fight in a doorway.
    constexpr float SAFE_ZONE_DOOR_KEEP = 3.0f;
    // A brink: the ground this much lower within BRINK_PROBE of the point, over an idle mob at most this far below.
    constexpr float SAFE_ZONE_BRINK_DROP = 5.0f;
    constexpr float SAFE_ZONE_BRINK_MAX_DZ = 30.0f;
    // The zone is rebuilt this often (or when the fight spot moves): holes come and go as mobs die and wake.
    constexpr uint32 SAFE_ZONE_REBUILD_MS = 1000;
    constexpr float SAFE_ZONE_MOVED = 1.0f;
    // At most this many path searches per Clamp.
    constexpr uint32 SAFE_ZONE_PATH_BUDGET = 4;
    // One zone_clamp line / event per bot this often.
    constexpr uint32 SAFE_ZONE_LOG_MS = 2000;

    struct ZoneHole
    {
        ObjectGuid mob;
        std::string name;
        uint32 entry = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float r = 0.0f;      // 3d
        float zGate = 0.0f;  // a point more than this above / below the mob is out of its proximity aggro
        bool flyer = false;  // no Z gate
    };

    struct ZoneLane
    {
        ObjectGuid mob;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float halfWidth = 0.0f;
    };

    struct SafeZone
    {
        bool valid = false;
        ObjectGuid tank;
        uint32 orderId = 0;  // the tank's open order when the zone was built (0: none)
        Position centre;     // the fight spot
        float tankRadius = SAFE_ZONE_TANK_RADIUS;
        float meleeRadius = SAFE_ZONE_MELEE_RADIUS;
        float rangedRadius = SAFE_ZONE_RANGED_RADIUS;
        float healerRadius = SAFE_ZONE_HEALER_RADIUS;
        float floorDz = SAFE_ZONE_FLOOR_DZ;
        std::vector<ZoneHole> holes;
        std::vector<ZoneLane> lanes;
        uint32 builtMs = 0;

        float RadiusFor(ZoneRole role) const;
        // Cheap geometry, no map access: in the role's disc or a lane, on the spot's floor, out of every hole. `from`
        // (the walker's position) relaxes it for a walker outside: no farther from the spot or the floor than it
        // stands (plus `fromSlack`: a walk's own samples get SAFE_ZONE_PATH_SLACK, round a pillar on the way back
        // in), no nearer to a hole's mob than it stands. `why` names the first rule broken.
        bool Contains(Position const& p, ZoneRole role, Position const* from = nullptr, std::string* why = nullptr,
                      float fromSlack = 0.5f) const;
        // The point as a stand: no brink over an idle mob below that sees it, no doorway (unless the walker stands
        // in that doorway already).
        bool Standable(Unit* walker, Position const& p, std::string* why = nullptr) const;
        // The walker's walk to `to` (its navmesh path, the straight line without one) stays inside, sampled every
        // SAFE_ZONE_SAMPLE yd, and runs through no closed door.
        bool PathInside(Unit* walker, Position const& to, ZoneRole role, std::string* why = nullptr) const;
        // The nearest usable destination inside the zone for a walker that wants `want`: as far along its walk to
        // `want` as the zone allows, else the role disc's edge toward it, else a spot round `want`. `reject` drops
        // candidates (avoid aoe: a spot inside the effect). False: no spot, the walker stays. `how` names the pick.
        bool Clamp(Unit* walker, Position const& want, ZoneRole role, Position& out, std::string& how,
                   std::function<bool(Position const&)> const& reject = {}) const;
    };

    // Optional per-role spot of a fight plan (the leader's choice; preferred when it is still inside the zone).
    struct RoleSpot
    {
        ZoneRole role = ZoneRole::Ranged;
        Position spot;
        Position fallback;
    };

    // The fight plan of a pull order (PullOrder::fight in LeaderOrders.h, plan section 2.2).
    struct FightPlan
    {
        Position fightSpot;  // where the pack is killed
        SafeZone zone;       // zone.valid false: no plan published, the bots build the default zone round FightHold
        std::vector<RoleSpot> roleSpots;
        Position gather;
    };

    namespace FightZone
    {
        // The bot belongs to a group the harness leads (the leader tracked, alive, on the bot's map, a 5-man
        // dungeon) and LEADER_SAFE_ZONE is on. `tank` (optional): that leader.
        bool Applies(Player* bot, Player** tank = nullptr);
        ZoneRole RoleOf(Player* bot);
        // The zone of the fight the bot's group fights now (the leader in combat with a fight spot): built round
        // the open order's FightPlan when the leader published one, else round FightHold's anchor; cached per tank
        // for SAFE_ZONE_REBUILD_MS. False: no zone (no harness group, no fight, a boss fight).
        bool Get(Player* bot, SafeZone& zone);
        bool GetPlan(Player* bot, FightPlan& plan);
        // Movers that are no reflex: the tank's own pull and fight spot walks, the leader's waypoint walk.
        bool ExemptMover(Player* bot, std::string const& action);
        // Between pulls: a walk of the harness tank of its own (no order open, out of combat, not the leader's
        // waypoint, not the planner's commit or the pull) is judged by the idle-mob path guard like a member's.
        bool GuardsIdleTankWalk(Player* bot, std::string const& action);
        // A tank gap closer (Charge, Intercept, Feral Charge) onto `target` that leaves the zone in a fight (a
        // target out of it, a new idle target) or, out of a fight with no order open, lands in another idle mob's
        // reach: true (no charge). Logged.
        bool GapCloserLeavesZone(Unit* caster, Unit* target, char const* what);
        // One "[lead9] zone_clamp" line and one zone_clamp move event (test harness JSONL) per bot every
        // SAFE_ZONE_LOG_MS: `got` nullptr is a stay.
        void NoteClamp(Player* bot, std::string const& mover, Position const& want, Position const* got,
                       std::string const& why);
    }
}

#endif
