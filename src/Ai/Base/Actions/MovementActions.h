/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MOVEMENTACTIONS_H
#define PLAYERBOTS_MOVEMENTACTIONS_H

#include "HazardZone.h"  // [lead16]
#include "Action.h"
#include "LastMovementValue.h"
#include "ObjectGuid.h"
#include "PlayerbotAIConfig.h"
#include <G3D/Vector3.h>
#include <cmath>
#include <vector>

class Creature;
class Player;
class PlayerbotAI;
class Unit;
class WorldObject;

struct Position;

namespace Leader
{
    struct SafeZone;  // [lead9] SafeZone.h
}

#define ANGLE_45_DEG (static_cast<float>(M_PI) / 4.f)
#define ANGLE_90_DEG M_PI_2
#define ANGLE_120_DEG (2.f * static_cast<float>(M_PI) / 3.f)

// [lead109] the guard that refused this thread's last MovementAction::MoveTo ("" = none)
std::string const& LastMoveRefusal();

class MovementAction : public Action
{
public:
    MovementAction(PlayerbotAI* botAI, std::string const name);

    // [lead70] a dry run of the walk guards: MoveTo judges as always but issues no motion (true: it would walk),
    // touches no movement memory and RefuseChase drops no target - the leader asks the tank's own walk-in with it
    static thread_local bool s_dryRun;
    // [lead109] the guard that refused this thread's last MoveTo ("" = it walked), for the callers' refusal lines
    static thread_local std::string s_moveRefusal;

    // The core's proximity aggro (Creature::CanStartAttack) of an idle `mob` on `who` standing at (x, y, z), in its
    // order: the Z gate (a mob that cannot fly never wakes on a walker more than CREATURE_Z_ATTACK_RANGE plus its
    // combat distance above or below it, sizes taken off), the 3D range GetAggroRange(who) + combat distance (+
    // `margin`), then its line of sight to the walker's eyes there (when `checkLos`). nullptr when the mob wakes, else
    // why not: "z gate", "range" or "los". RFC 2026-10-24: an Earthborer on a ledge 9 yd above the path vetoed every
    // follow of the shadow priest (the tank walked under it untouched), the group oscillated for minutes.
    static char const* IdleAggroBlocker(Creature* mob, Unit* who, float x, float y, float z, float margin,
                                        bool checkLos);
    // The 3D proximity aggro range of `mob` on `who` (core: GetAggroRange + combat distance), plus `margin`.
    static float IdleAggroReach(Creature* mob, Unit* who, float margin);

    // [sight-arc] The margin over the core's real aggro distance (GetAggroRange + combat distance, 3D, no sizes) a
    // sight spot and its walk keep from an idle mob. One value for every sight search: the ranged / healer sight walk
    // (MoveToSight, UnsafeSpotReason), the idle-mob path guard and the tank's pull spot (test harness). mgt-h
    // 2026-10-25: the old 3-5 yd margins put the whole Selin room inside the side packs' "reach" (user, as GM: from
    // the doorway and the room's middle the other pack is in plain sight, nothing else near).
    static constexpr float SIGHT_IDLE_AGGRO_MARGIN = 1.5f;

    // [sight-arc] Spots round `target` on rings from `minRadius` to `maxRadius` (SIGHT_ARC_RING_STEP apart), all the
    // way round it (SIGHT_ARC_ANGLES), snapped to the ground under `walker`, on the target's floor (`floorDz`), that
    // the target sees at the walker's eye height: the side steps that open a corner, not only points of the walk
    // towards it (user: the right Wretched pack hidden behind the door pillar is seen after a few yards to the
    // left). Nearest to the walker first; `angleOffDeg` is the angle off the walker's direction from the target. At
    // most `maxLos` line-of-sight tests. Uses no AI value: safe on the world thread (test harness).
    struct SightArcCandidate
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float straight = 0.0f;  // from the walker
        float radius = 0.0f;
        float angleOffDeg = 0.0f;
    };
    static std::vector<SightArcCandidate> SightArcCandidates(Unit* walker, Unit* target, float minRadius,
                                                             float maxRadius, float floorDz, uint32 maxLos);
    // The core's Z gate alone: false when `who` standing at height `z` is out of the mob's vertical aggro reach.
    static bool PassesAggroZGate(Creature* mob, Unit* who, float z);
    // A dungeon tank's trail point where it stands now (the test harness walks its leader by MotionMaster: no MoveTo).
    static void RecordTankTrail(Player* tank);
    // [patrol_wait_trail_back] The tank's trail in this instance, oldest point first (every 2 yd, up to 256 yd);
    // empty when none was recorded.
    static std::vector<G3D::Vector3> TankTrailPoints(Player* tank);

protected:
    // In combat a tank's walk of this action is judged by the idle-mob path guard too: a chase after a mob away from
    // the fight (Scholomance heroic: the walks to the healer's casters dragged the fight across the hall's packs).
    virtual bool GuardsTankWalk() { return false; }
    // [reach_healer_taunt_first] ... and out of combat too (a GuardsTankWalk action only): the walk to a mob on the
    // healer from an idle tank went through the next pack ("tank: reach healer attacker" started pulls with a death
    // in 44 %, PULL_ESTIMATE_AUDIT.md).
    virtual bool GuardsIdleTankWalk() { return false; }
    // A walk an encounter tactic plans into a pack's aggro on purpose (the group walking in together for a pull):
    // the idle-mob path guard lets it be ([selin_final_entry]).
    virtual bool SkipsIdleGuard() { return false; }
    // [sight-arc] The first idle mob a walk of the bot along `points` wakes, judged as the idle-mob path guard of
    // MoveTo judges it (the core's proximity aggro per segment: the Z gate, the 3D reach plus the guard margin, the
    // mob's sight; a mob the tank stands by or walked past in its sight lets the group by). `exempt` (the walk's
    // target) is not judged. Empty when nothing wakes, else "<mob> (<distance> yd of reach <reach>, <walked> yd
    // along)". Judged in every group walk, also where the guard is off (a world group): a spot is chosen, not vetoed.
    std::string IdleWakeOnWalk(Movement::PointsArray const& points, ObjectGuid exempt);
    // [lead101] the same judgement, the woken mob itself (null: nothing wakes)
    Unit* IdleWokenOnWalk(Movement::PointsArray const& points, ObjectGuid exempt);
    bool JumpTo(uint32 mapId, float x, float y, float z, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);
    bool MoveNear(uint32 mapId, float x, float y, float z, float distance = sPlayerbotAIConfig.ContactDistance,
                  MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);
    bool MoveToLOS(WorldObject* target, bool ranged = false);
    bool MoveTo(uint32 mapId, float x, float y, float z, bool idle = false, bool react = false,
                bool normal_only = false, bool exact_waypoint = false,
                MovementPriority priority = MovementPriority::MOVEMENT_NORMAL, bool lessDelay = false,
                bool backwards = false);
    bool MoveTo(WorldObject* target, float distance = 0.0f,
                MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);
    bool MoveNear(WorldObject* target, float distance = sPlayerbotAIConfig.ContactDistance,
                  MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);
    float GetFollowAngle();
    bool Follow(Unit* target, float distance = sPlayerbotAIConfig.FollowDistance);
    bool Follow(Unit* target, float distance, float angle);
    bool ChaseTo(WorldObject* obj, float distance = 0.0f);
    bool ReachCombatTo(Unit* target, float distance = 0.0f);
    // Walks the path towards `target` only as far as the first point that sees it within `range` (and a step past
    // it, clear of the corner); false when no point of the path's first stretch does or the move failed.
    bool MoveToSight(Unit* target, float range, std::string const& reason);
    // [b32_engage] A mob running to a group member that will be within `range` and in sight of the bot in ~2 s.
    static bool TargetComesIntoRange(Player* bot, Unit* target, float range);
    // Notes why the last MoveTo chose its destination (test harness "[intent] goingTo").
    void NoteMoveReason(std::string const& reason);
    float MoveDelay(float distance, bool backwards = false);
    void WaitForReach(float distance);
    void SetNextMovementDelay(float delayMillis);
    bool IsMovingAllowed(WorldObject* target);
    bool IsDuplicateMove(float x, float y, float z);
    bool IsWaitingForLastMove(MovementPriority priority);
    bool IsMovingAllowed();
    bool Flee(Unit* target);
    void ClearIdleState();
    void UpdateMovementState();
    bool MoveAway(Unit* target, float distance = sPlayerbotAIConfig.FleeDistance, bool backwards = false);
    bool MoveFromGroup(float distance);
    bool Move(float angle, float distance);
    bool MoveInside(uint32 mapId, float x, float y, float z, float distance = sPlayerbotAIConfig.FollowDistance,
                    MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);
    // A straight walk without a navmesh path, as a player walks a ramp, a bridge or a short gap the navmesh has no
    // polygons for: only in sight and, unless requireGround is false (a ghost) or the bot swims, over a floor or
    // water close below every step of the line. PlayerbotAI::GuardAirborneSpline still guards the spline.
    // exactLine: the line itself, as an escort spline of three points (a MovePoint is pathed again by
    // PointMovementGenerator, see MoveStraight), for a route link's line whose ground was checked.
    bool MoveStraight(uint32 mapId, float x, float y, float z, bool requireGround = true,
                      MovementPriority priority = MovementPriority::MOVEMENT_NORMAL, bool exactLine = false);
    // A straight flight on a flying mount (no path, the bot's own flying movement flags set).
    bool MoveFlying(uint32 mapId, float x, float y, float z,
                    MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);
    static bool StraightLineHasGround(Player* walker, float x, float y, float z);
    // [obstacle_sidestep] (ObstacleSidestep.cpp) Called by a walk to (x, y, z) every tick it wants to move. When the
    // bot has made no progress for a while and a solid game object with collision stands within 4 yd ahead on its
    // path (the navmesh has no game objects: a decoration on a road), it steps 2-4 yd to the side onto navmesh and
    // ground and walks past the object; the caller paths to its goal again afterwards. True while that walk round
    // is under way (the caller's own move waits); false otherwise.
    bool SidestepObstacle(uint32 mapId, float x, float y, float z);
    // [b33_cos2] A dungeon tank's own walk out of combat (an event's wave spot) stays within HEALER_LEASH of the
    // group's healer. Walk: to (x, y, z), cut back to the last point of the ground path still that near (unchanged
    // when the spot is near enough, or the walk closes in on the healer). Back: the tank is out of the healer's range,
    // (x, y, z) is the healer's position. Hold: in range, at the edge - no step on.
    enum class HealerLeashStep : uint8
    {
        Walk,
        Back,
        Hold
    };
    HealerLeashStep HealerLeash(float& x, float& y, float& z);
    void CreateWp(Player* wpOwner, float x, float y, float z, float o, uint32 entry, bool important = false);
    // [escape_back_trail] In a dungeon group: the nearest spot just outside the circle, back along the tank's trail
    // first, else the side toward the group; never in an idle mob's reach (no LoS), never far. `how` names it.
    bool SafeEscapeSpot(Position const& center, float radius, Position& spot, std::string& how);
    // [escape_room_kite] The room of the boss the bot fights (centre, radius): KiteRoomLookup() when set, else the
    // tank's fight spot, else the boss's home. False outside a boss fight.
    bool EscapeRoom(Position& centre, float& radius, Unit*& boss);
    // [escape_room_kite] The per-boss kite room table (Ai/Dungeon/BossKiteRooms, the data agent) registers its lookup
    // here: `MovementAction::KiteRoomLookup() = &BossKiteRooms::Find;` (true with the room's centre and radius).
public:
    using KiteRoomLookupFn = bool (*)(Player* bot, Unit* boss, Position& centre, float& radius);
    static KiteRoomLookupFn& KiteRoomLookup();
protected:
    Position BestPositionForMeleeToFlee(Position pos, float radius);
    Position BestPositionForRangedToFlee(Position pos, float radius);
    bool FleePosition(Position pos, float radius, uint32 minInterval = 1000,
                      MovementPriority priority = MovementPriority::MOVEMENT_COMBAT);
    bool CheckLastFlee(float curAngle, std::list<FleeInfo>& infoList);

protected:
    struct CheckAngle
    {
        float angle;
        bool strict;
    };

private:
    // float SearchBestGroundZForPath(float x, float y, float z, bool generatePath, float range = 20.0f, bool
    // normal_only = false, float step = 8.0f);
    const Movement::PointsArray SearchForBestPath(float x, float y, float z, float& modified_z, int maxSearchCount = 5,
                                                  bool normal_only = false, float step = 8.0f);
    bool wasMovementRestricted = false;
    void DoMovePoint(Unit* unit, float x, float y, float z, bool generatePath, bool backwards);
    // A straight swim through open water, as a player dives to a mob on the bottom or swims up to one: the navmesh
    // has a layer on the surface and one on the bottom, and a path between them runs along either.
    bool SwimStraight(uint32 mapId, float x, float y, float z, MovementPriority priority, bool lessDelay);

    // Idle-mob path guard (MoveTo): a non-tank group member in a dungeon walks no path that enters the aggro reach
    // of an idle hostile mob (Stormwind Stockade wave 40: damage dealers and healers ran across the room at the
    // start of a fight and woke the next prisoners). What the move is for picks the spots tried before it stays.
    enum class GuardedMove : uint8
    {
        GENERIC,  // a destination of its own: a shorter stop, a melee goes next to the tank
        REACH,    // to `_guardTarget` at `_guardRange`: another spot on that ring (the group's side first), shorter
        FLEE,     // away from a mob that hits the bot: toward the tank, shorter
        AVOID,    // out of a ground effect: toward the tank, shorter, else the move as asked
        STEP,     // one of several directions the caller tries in turn (MoveAway): none
        FOLLOW    // a leg of the tank's trail: shorter
    };
    // Whether this bot's moves are guarded at all: alive, grouped, in a dungeon, on its own feet; not a tank, except
    // for a walk of an action that GuardsTankWalk() in combat.
    bool IsPathGuarded();
    // The guard on a new move to (x, y, z): true to go (to x, y, z, maybe replaced by a safe spot), false to stay.
    bool GuardIdleMobPath(float& x, float& y, float& z, bool straight);
    // [drift_fight_hold] A 5-man trash fight's chase walk ends at the edge of the tank's fight spot (FightHold in
    // ChooseTargetActions.h): the tank's walk is shortened to it (false: already there, it stays), a melee damage
    // dealer's walk to a mob far out of it is refused. True to go (to x, y, z, maybe shortened).
    bool HoldFightSpot(float& x, float& y, float& z);
    uint32 _fightHoldLogMs = 0;
    // [lead9] A harness group's fight with a safe zone (SafeZone.h, C:/WoW/LEADER_REFACTOR_PLAN.md step 9): the
    // destination, its stand and its walk stay inside the zone, else the nearest usable spot inside (zone.Clamp), else
    // the bot stays (false; logged zone_clamp). `applied`: the zone judged this move (HoldFightSpot is then not asked:
    // the zone is the radius authority); `zone` is kept for the check of a guard's other spot after it.
    bool ClampToFightZone(float& x, float& y, float& z, Leader::SafeZone& zone, bool& applied);
    // The last ask the zone judged (for GUARD_VETO_MEMO_MS) and its answer: a stay, or the clamped spot.
    float _zoneVetoX = 0.0f;
    float _zoneVetoY = 0.0f;
    float _zoneVetoZ = 0.0f;
    uint32 _zoneVetoMs = 0;
    bool _zoneClamped = false;
    float _zoneGotX = 0.0f;
    float _zoneGotY = 0.0f;
    float _zoneGotZ = 0.0f;
    // [hazard_melee_spot] A melee walk ("reach melee", "set behind") into a ground effect the bot left within 3 s: to
    // a melee spot round its target outside it (x, y, z replaced), or into a light one (true), or no walk (false).
    bool HazardMeleeSpot(float& x, float& y, float& z);
    uint32 _hazardLogMs = 0;
    // [los_restore] The target fights the group and no member has seen it for 3 s: a walk to the nearest reachable
    // spot that sees it (melee: its melee ring or a few yards round it; ranged up to 25 yd), past the fight hold,
    // still out of every idle reach. False when the bot sees it, the condition is not met, or no spot is found.
    bool LosRestoreWalk(Unit* target, float meleeDistance);
    bool _losRestore = false;
    // A dungeon walk through a closed door (TerrainHazard.h PathThroughClosedDoor: the Utgarde Keep walls of flame)
    // ends short of it: true to go (to x, y, z, maybe replaced by a stop before the door), false to stay.
    bool GuardClosedDoor(float& x, float& y, float& z);
    // [hleft-door-chase] / [hleft-chase-drop] A chase the bot may not make (through a closed door, down to a level the
    // navmesh does not join): logged "[chase refused]", and a creature target is dropped.
    void RefuseChase(Unit* target, std::string const& why);
    ObjectGuid _chaseRefusedFor;
    uint32 _chaseRefusedMs = 0;
    // Follows the tank along the positions it walked, never past it: false when there is nothing to walk.
    bool FollowTrail(Player* tank, float distance);
    // One direction of MoveAway (a combat move); a guarded one that wakes an idle mob fails, the next is tried.
    bool MoveToStep(uint32 mapId, float x, float y, float z, bool exact, bool backwards);
    GuardedMove _guardedMove = GuardedMove::GENERIC;
    ObjectGuid _guardTarget;
    float _guardRange = 0.0f;
    // The ground effect an AVOID move leaves (FleePosition): the guard looks for another way out of it.
    Position _avoidCenter;
    float _avoidRadius = 0.0f;
    // The last vetoed destination: the same ask again within a second stays without a new search.
    float _vetoX = 0.0f;
    float _vetoY = 0.0f;
    float _vetoZ = 0.0f;
    uint32 _vetoMs = 0;
    // The last destination a closed door stopped: the same ask again within a second stays without a new path.
    float _doorVetoX = 0.0f;
    float _doorVetoY = 0.0f;
    float _doorVetoZ = 0.0f;
    uint32 _doorVetoMs = 0;
};

class FleeAction : public MovementAction
{
public:
    FleeAction(PlayerbotAI* botAI) : MovementAction(botAI, "flee")
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;
};

class FleeWithPetAction : public MovementAction
{
public:
    FleeWithPetAction(PlayerbotAI* botAI) : MovementAction(botAI, "flee with pet") {}

    bool Execute(Event event) override;
};

class AvoidAoeAction : public MovementAction
{
public:
    AvoidAoeAction(PlayerbotAI* botAI, int moveInterval = 1000)
        : MovementAction(botAI, "avoid aoe"), moveInterval(moveInterval)
    {
    }

    bool isUseful() override;
    bool Execute(Event event) override;

    // The bot is casting while standing in a ground effect, or inside an enemy area cast that lands before its own
    // cast ends: it stops casting so this action can walk it out (the AI does not tick during a cast).
    static bool ShouldStopCastToDodge(PlayerbotAI* botAI);

    // [lead16] The dungeon leader's hazard view of this bot (LeaderOrders.h LEADER_NO_OWN_AOE_DODGE): the circles the
    // four cases of Execute would flee, without moving (the same filters: whitelist, max radius, negative spells).
    // World thread (the leader reads the bot's AI values after the map updates).
    static void CollectZones(PlayerbotAI* botAI, std::vector<HazardZone>& out);

protected:
    bool AvoidAuraWithDynamicObj();
    bool AvoidGameObjectWithDamage();
    bool AvoidUnitWithDamageAura();
    bool AvoidCasterAoe();
    time_t lastTellTimer = 0;
    int lastMoveTimer = 0;
    int moveInterval;
};

class CombatFormationMoveAction : public MovementAction
{
public:
    CombatFormationMoveAction(PlayerbotAI* botAI, std::string name = "combat formation move", int moveInterval = 1000)
        : MovementAction(botAI, name), moveInterval(moveInterval)
    {
    }

    bool isUseful() override;
    bool Execute(Event event) override;

protected:
    Position AverageGroupPos(float dis = sPlayerbotAIConfig.SightDistance, bool ranged = false, bool self = false);
    Player* NearestGroupMember(float dis = sPlayerbotAIConfig.SightDistance);
    float AverageGroupAngle(Unit* from, bool ranged = false, bool self = false);
    Position GetNearestPosition(std::vector<Position> const& positions);
    int lastMoveTimer = 0;
    int moveInterval;
};

class TankFaceAction : public CombatFormationMoveAction
{
public:
    TankFaceAction(PlayerbotAI* botAI) : CombatFormationMoveAction(botAI, "tank face") {}

    bool Execute(Event event) override;
};

class RearFlankAction : public MovementAction
{
    // 90 degree minimum angle prevents any frontal cleaves/breaths and avoids parry-hasting the current target.
    // 120 degree maximum angle leaves a 120 degree symmetrical cone at the tail end which is usually enough to avoid
    // tail swipes. Some dragons or mobs may have different danger zone angles, override if needed.
public:
    RearFlankAction(PlayerbotAI* botAI, float distance = 0.0f, float minAngle = ANGLE_90_DEG,
                    float maxAngle = ANGLE_120_DEG, std::string const& name = "rear flank")
        : MovementAction(botAI, name)
    {
        this->distance = distance;
        this->minAngle = minAngle;
        this->maxAngle = maxAngle;
    }

    std::string const GetTargetName() override { return "current target"; };
    bool Execute(Event event) override;
    bool isUseful() override;

protected:
    float distance, minAngle, maxAngle;
};

class BossRearFlankAction : public RearFlankAction
{
public:
    BossRearFlankAction(PlayerbotAI* botAI, std::string bossName, float distance = 0.0f, float minAngle = ANGLE_90_DEG,
                        float maxAngle = ANGLE_120_DEG)
        : RearFlankAction(botAI, distance, minAngle, maxAngle, bossName + " rear flank"),
          bossName(std::move(bossName)) {}

    Unit* GetTarget() override;

protected:
    const std::string bossName;
};

class DisperseSetAction : public Action
{
public:
    DisperseSetAction(PlayerbotAI* botAI, std::string const name = "disperse set") : Action(botAI, name) {}

    bool Execute(Event event) override;
    float DEFAULT_DISPERSE_DISTANCE_RANGED = 5.0f;
    float DEFAULT_DISPERSE_DISTANCE_MELEE = 2.0f;
};

class RunAwayAction : public MovementAction
{
public:
    RunAwayAction(PlayerbotAI* botAI) : MovementAction(botAI, "runaway") {}

    bool Execute(Event event) override;
};

class MoveToLootAction : public MovementAction
{
public:
    MoveToLootAction(PlayerbotAI* botAI) : MovementAction(botAI, "move to loot") {}

    bool Execute(Event event) override;

private:
    // Progress watchdog: stop chasing a loot target we are not actually getting closer to.
    ObjectGuid lootWatchGuid;
    float lootWatchDistance = -1.0f;
    uint32 lootWatchMs = 0;
    uint32 lootWatchLastEvalMs = 0;
};

class MoveOutOfEnemyContactAction : public MovementAction
{
public:
    MoveOutOfEnemyContactAction(PlayerbotAI* botAI) : MovementAction(botAI, "move out of enemy contact") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class SetFacingTargetAction : public Action
{
public:
    SetFacingTargetAction(PlayerbotAI* botAI) : Action(botAI, "set facing") {}

    // it turns to the current target (the default "self target" named the bot itself in its intent and gates)
    std::string const GetTargetName() override { return "current target"; }
    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
};

class SetBehindTargetAction : public CombatFormationMoveAction
{
public:
    SetBehindTargetAction(PlayerbotAI* botAI) : CombatFormationMoveAction(botAI, "set behind") {}

    bool Execute(Event event) override;

private:
    uint32 _holdUntil = 0;  // a spot behind taken a moment ago is kept until then unless the bot is deep in front
};

// Tank: walk a short way towards the mobs hitting the healer and the damage dealers, so its area threat
// (Consecration, Thunder Clap, Swipe, Death and Decay) reaches them; towards those on the healer alone while there
// are any ("far healer attacker"). What it already holds follows it; the usual reach-melee and tank-face movement
// bring everything back to place afterwards.
class MoveToGroupMemberAttackersAction : public MovementAction
{
public:
    MoveToGroupMemberAttackersAction(PlayerbotAI* botAI) : MovementAction(botAI, "move to group member attackers") {}

    bool isUseful() override;
    bool Execute(Event event) override;

protected:
    bool GuardsTankWalk() override { return true; }

private:
    bool GetAttackersCenter(Position& center);
};

class MoveOutOfCollisionAction : public MovementAction
{
public:
    MoveOutOfCollisionAction(PlayerbotAI* botAI) : MovementAction(botAI, "move out of collision") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class MoveRandomAction : public MovementAction
{
public:
    MoveRandomAction(PlayerbotAI* botAI) : MovementAction(botAI, "move random") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class MoveInsideAction : public MovementAction
{
public:
    MoveInsideAction(PlayerbotAI* botAI, float x, float y, float distance = 5.0f) : MovementAction(botAI, "move inside")
    {
        this->x = x;
        this->y = y;
        this->distance = distance;
    }
    virtual bool Execute(Event event);

protected:
    float x, y, distance;
};

class RotateAroundTheCenterPointAction : public MovementAction
{
public:
    RotateAroundTheCenterPointAction(PlayerbotAI* botAI, std::string name, float center_x, float center_y,
                                     float radius = 40.0f, uint32 intervals = 16, bool clockwise = true,
                                     float start_angle = 0)
        : MovementAction(botAI, name)
    {
        this->center_x = center_x;
        this->center_y = center_y;
        this->radius = radius;
        this->intervals = intervals;
        this->clockwise = clockwise;
        this->call_counters = 0;
        for (uint32 i = 0; i < intervals; i++)
        {
            float angle = start_angle + 2 * M_PI * i / intervals;
            waypoints.push_back(std::make_pair(center_x + cos(angle) * radius, center_y + sin(angle) * radius));
        }
    }
    virtual bool Execute(Event event);

protected:
    virtual uint32 GetCurrWaypoint() { return 0; }
    uint32 FindNearestWaypoint();
    float center_x, center_y, radius;
    uint32 intervals, call_counters;
    bool clockwise;
    std::vector<std::pair<float, float>> waypoints;
};

class MoveFromGroupAction : public MovementAction
{
public:
    MoveFromGroupAction(PlayerbotAI* botAI, std::string const name = "move from group") : MovementAction(botAI, name) {}

    bool Execute(Event event) override;
};

class MoveAwayFromCreatureAction : public MovementAction
{
public:
    MoveAwayFromCreatureAction(PlayerbotAI* botAI, std::string name, uint32 creatureId, float range, bool alive = true)
        : MovementAction(botAI, name), creatureId(creatureId), range(range), alive(alive)
    {
    }

    bool Execute(Event event) override;
    bool isPossible() override;

private:
    uint32 creatureId;
    float range;
    bool alive;
};

class MoveAwayFromPlayerWithDebuffAction : public MovementAction
{
public:
    MoveAwayFromPlayerWithDebuffAction(PlayerbotAI* botAI, std::string name, uint32 spellId, float range)
        : MovementAction(botAI, name), spellId(spellId), range(range)
    {
    }

    bool Execute(Event event) override;
    bool isPossible() override;

private:
    uint32 spellId;
    float range;
};

#endif
