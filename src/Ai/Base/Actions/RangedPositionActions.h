/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RANGEDPOSITIONACTIONS_H
#define PLAYERBOTS_RANGEDPOSITIONACTIONS_H

#include "MovementActions.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "Trigger.h"
#include "Value.h"

#include <string>
#include <vector>

class Creature;
class PlayerbotAI;
class Unit;

// Where a healer or a ranged bot of a party should stand while the tank fights, the way players do it: behind
// the tank, at range from the pack (out of cleaves, frontal attacks and melee splash), with the tank in sight
// and in heal range. Five-man and open-world groups only; raids keep their encounter positioning.
struct RangedPositionInfo
{
    bool fighting = false;  // the group's tank is in combat with a pack
    bool needed = false;    // the bot stands where it should not and has somewhere better to go
    bool danger = false;    // an enemy that fights someone else is in melee reach of the bot
    bool approach = false;  // no good spot found: walk towards the tank instead
    bool chased = false;    // a mob melees the bot away from the tank: walk to the tank ("move to tank")
    bool improve = false;   // the bot sees the fight badly (a corner, a corridor): a spot nearby sees it better
    float dangerGap = 0.0f;     // room to the closest enemy fighting someone else (danger below MELEE_DANGER_GAP)
    float tankDistance = 0.0f;  // the bot's distance to the tank
    float leash = 0.0f;         // the leash that applied (0: none)
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    ObjectGuid tank;
    std::string reason;  // why this spot ("[intent] goingTo")
};

class RangedPositionValue : public CalculatedValue<RangedPositionInfo>
{
public:
    static constexpr uint32 CHECK_INTERVAL_MS = 500;

    RangedPositionValue(PlayerbotAI* botAI)
        : CalculatedValue<RangedPositionInfo>(botAI, "group ranged position", CHECK_INTERVAL_MS)
    {
    }

    RangedPositionInfo Calculate() override;

private:
    Player* FindTank() const;

    // Where the bot stood when the tank's fight began: the side the group came from, cleared and walked. A spot
    // towards it is preferred over one across the room, next to corridors nobody has looked into.
    Position _cameFrom;
    bool _hasCameFrom = false;
    uint32 _viewCheckMs = 0;  // the last look for a better view (VIEW_CHECK_MS)
};

// An enemy that is not fighting the bot stands within melee reach of a healer or ranged bot.
class RangedInMeleeDangerTrigger : public Trigger
{
public:
    RangedInMeleeDangerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ranged in melee danger") {}

    bool IsActive() override;
};

// The tank fights out of the healer's comfortable heal range (or out of its sight), or a ranged member stands
// further than RangedTankLeash from it.
class HealerTooFarFromTankTrigger : public Trigger
{
public:
    HealerTooFarFromTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "healer too far from tank") {}

    bool IsActive() override;
};

// A mob (not a boss) melees the healer or ranged bot while the tank fights or stands within CHASE_REFUGE_MAX yards
// but more than CHASE_REFUGE_REACHED away: the bot walks to the tank instead of standing alone (wave 10: dps and
// healers died to one mob each, 25-50 yd from the tank).
class ChasedAwayFromTankTrigger : public Trigger
{
public:
    ChasedAwayFromTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "chased away from tank") {}

    bool IsActive() override;
};

// The last walk "move to tank" or "move to ranged position" started: when, and where to (LastMovement's target).
struct HealWalk
{
    uint32 startMs = 0;
    float x = 0.0f;
    float y = 0.0f;
};

class HealWalkValue : public ManualSetValue<HealWalk>
{
public:
    HealWalkValue(PlayerbotAI* botAI) : ManualSetValue<HealWalk>(botAI, HealWalk(), "heal walk") {}
};

// When the approach of "move to ranged position" (the bot lost the fight from sight or range) began to fail: the
// getMSTime() of the first failed walk in a row, 0 while it walks or is not approaching. RangedPositionFollowMultiplier
// hands the bot to "follow" once that lasts APPROACH_STALL_RELEASE_MS ([rjoin-follow]).
class RangedApproachStallValue : public ManualSetValue<uint32>
{
public:
    static constexpr uint32 APPROACH_STALL_RELEASE_MS = 2500;

    RangedApproachStallValue(PlayerbotAI* botAI) : ManualSetValue<uint32>(botAI, 0, "ranged approach stall") {}
};

// Notes the walk one of the two actions above has just started (for "healer stops to heal").
void NoteHealWalk(PlayerbotAI* botAI);

// A healer is still on a walk of "move to tank" or "move to ranged position" while its heal target needs a heal now
// (below medium health or losing health fast, in heal range and sight): it stops so the cast-time heal can start.
class HealerStopsToHealTrigger : public Trigger
{
public:
    HealerStopsToHealTrigger(PlayerbotAI* botAI) : Trigger(botAI, "healer stops to heal") {}

    bool IsActive() override;
};

class StopToHealAction : public MovementAction
{
public:
    StopToHealAction(PlayerbotAI* botAI) : MovementAction(botAI, "stop to heal") {}

    bool Execute(Event event) override;
};

class MoveToRangedPositionAction : public MovementAction
{
public:
    MoveToRangedPositionAction(PlayerbotAI* botAI) : MovementAction(botAI, "move to ranged position") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // The fight is out of sight or range: walk towards it (info.approach).
    bool Approach(RangedPositionInfo const& info);
    // Walk to the spot the value picked.
    bool TakeSpot(RangedPositionInfo const& info);

    uint32 _lastMoveTime = 0;
    uint32 _holdUntil = 0;  // the spot taken last is kept until then (arrival plus REPOSITION_HOLD_MS)
    float _lastX = 0.0f;
    float _lastY = 0.0f;
    float _lastZ = 0.0f;
    uint32 _waitUntil = 0;  // only unsafe spots were on the way: the approach waits until then before looking again
};

// A dungeon tank holding mobs has lost its healer's line of sight (or heal range) for TANK_SIGHT_GRACE_MS: the
// healer walks to sight at once (the reach / ranged position actions); when that has not brought it back, the tank
// steps back towards the healer, its mobs following, to the first spot of the way that the healer sees. User,
// Stockade 2026-10-24: «надо, чтобы танк выбегал в прямую видимость хила... иначе хил не достаёт».
class TankLostHealerSightTrigger : public Trigger
{
public:
    static constexpr uint32 SIGHT_CHECK_MS = 500;
    static constexpr uint32 TANK_SIGHT_GRACE_MS = 2000;

    TankLostHealerSightTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tank lost healer sight") {}

    bool IsActive() override;

private:
    uint32 _checkMs = 0;
    uint32 _lostSinceMs = 0;
};

class TankStepToHealerSightAction : public MovementAction
{
public:
    TankStepToHealerSightAction(PlayerbotAI* botAI) : MovementAction(botAI, "tank step to healer sight") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// [prepull_sight_spot] Before the tank's first hit, a ranged / healer member that will not see the fight walks to a
// spot that does (MoveToSight, arc-based [sight-arc]). What it watches comes from the tank's published plan: a pull
// back (PullCastSpot with its kill spot) is fought where the tank stands while it is still at the kill spot; an
// accepted engage (CarefulPlanVerdict on the tank's target, no cast spot) is fought at the mob. False when there is
// nothing to watch; `range` is the member's casting (heal) range.
bool PrePullSightWatch(PlayerbotAI* botAI, Player* bot, Unit*& watch, float& range, std::string& why);

class PrePullSightSpotTrigger : public Trigger
{
public:
    static constexpr int32 CHECK_INTERVAL_SEC = 2;

    PrePullSightSpotTrigger(PlayerbotAI* botAI) : Trigger(botAI, "prepull sight spot", CHECK_INTERVAL_SEC) {}

    bool IsActive() override;
};

class MoveToPrePullSightSpotAction : public MovementAction
{
public:
    MoveToPrePullSightSpotAction(PlayerbotAI* botAI) : MovementAction(botAI, "move to prepull sight spot") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// The group's first healer on the bot's map, alive (nullptr without one).
Player* FindGroupHealer(Player* bot);

// Why a ranged or healer spot is not safe to stand on, empty when it is: inside an idle mob's aggro reach, on a
// steep polygon, off the navmesh, beside a drop-off (a ledge rim, a rock tooth), or with no walk back to `fight`.
// Height itself is no reason (RFC 2026-10-24, user: «можно лазить по скалам, это просто должно быть безопасно»).
std::string UnsafeSpotReason(PlayerbotAI* botAI, float x, float y, float z, WorldObject const* fight);

// How well a spot sees the fight (RFC 2026-10-24, user: «можно же немножко отбежать левее, чтобы видеть танка с
// большего угла, и видеть весь этот пак»): the watched unit (the tank) from the spot and from two points 2 yd to its
// sides (a spot on the edge of a corner sees it from one of them only), the idle mobs the group faces next, and
// open room to both sides (a spot in a corridor has walls on both). A preference, never a reason to take a spot.
struct SpotView
{
    int watch = 0;
    int watchRays = 0;
    int pack = 0;
    int packMobs = 0;
    int open = 0;
    int openRays = 0;

    float Score() const;
    // Sees the watched unit only round an edge, is walled in on both sides, or misses most of the next pack.
    bool Poor() const;
    std::string Describe() const;
};

SpotView RateSpotView(WorldObject const* watch, float x, float y, float z, std::vector<Creature*> const& pack);

// Idle hostile mobs within VIEW_PACK_RANGE of the point, nearest first, at most `maxMobs` (the next packs).
std::vector<Creature*> IdleMobsNear(PlayerbotAI* botAI, float x, float y, float z, size_t maxMobs);

// The first spot MoveToSight refused as unsafe on its last call on this thread ("" when none): its caller waits
// instead of falling back to a plain walk that would end on the same perch.
std::string const& LastSightRejection();

#endif
