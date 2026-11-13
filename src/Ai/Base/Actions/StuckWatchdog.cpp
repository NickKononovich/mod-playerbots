/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "StuckWatchdog.h"
#include "BotPath.h"  // [lead109]
#include "AiObjectContext.h"
#include "Event.h"
#include "Formations.h"
#include "Group.h"
#include "LastMovementValue.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr uint32 SAMPLE_GAP_MS = 5 * IN_MILLISECONDS;   // a longer gap means the watch was paused: start over
    constexpr float STUCK_EXTRA_DISTANCE = 8.0f;            // beyond the formation distance before anything counts
    constexpr float FAR_DISTANCE = 30.0f;                   // past this, not getting closer counts as no progress
    constexpr float PROGRESS_DISTANCE = 2.0f;
    constexpr float STILL_DISTANCE = 2.0f;
    constexpr float LEADER_MOVED_DISTANCE = 10.0f;
    constexpr uint32 RESET_MOVEMENT_SECONDS = 6;
    constexpr uint32 REFOLLOW_SECONDS = 12;
    constexpr uint32 REBUILD_PATH_SECONDS = 20;
    constexpr uint32 LEADER_TRAIL_SECONDS = 45;
    constexpr uint32 GIVE_UP_SECONDS = 90;
    constexpr uint32 TRAIL_RETRY_MS = 6 * IN_MILLISECONDS;  // time to walk to one trail point before the next try
    constexpr uint8 TRAIL_MAX_TRIES = 4;
    constexpr uint32 TRAIL_MAX_PATH_CHECKS = 12;            // path queries per try, newest trail points first
    constexpr float TRAIL_POINT_SPACING = 5.0f;
    constexpr size_t TRAIL_MAX_POINTS = 40;                 // about the last 200 yards the leader walked
    constexpr float TRAIL_MIN_STEP = 4.0f;                  // a trail point closer than this is no different way
    constexpr float TRAIL_SAME_TARGET = 3.0f;
    constexpr float ANNOUNCE_SAME_SPOT = 20.0f;
    constexpr float UNSTICK_STEP_DISTANCE = 5.0f;

    StuckWatchdogValue* GetWatchdog(AiObjectContext* context)
    {
        return dynamic_cast<StuckWatchdogValue*>(context->GetUntypedValue("stuck watchdog"));
    }

    char const* StageName(StuckStage stage)
    {
        switch (stage)
        {
            case StuckStage::RESET_MOVEMENT:
                return "reset movement";
            case StuckStage::REFOLLOW:
                return "follow again";
            case StuckStage::REBUILD_PATH:
                return "rebuild path";
            case StuckStage::LEADER_TRAIL:
                return "walk the leader's trail";
            case StuckStage::GIVE_UP:
                return "give up";
            default:
                return "none";
        }
    }
}

Player* StuckWatchdogValue::GetWatchedLeader()
{
    if (!bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->InBattleground() || bot->InArena())
        return nullptr;

    if (bot->IsBeingTeleported() || bot->GetTransport() || bot->GetVehicle() ||
        bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return nullptr;

    // While the group fights around the bot, positioning belongs to the combat logic (a healer kept at range is not
    // "stuck"); following the leader then would walk the bot into the pack.
    // [stuck30] b475 dm-n five 10: this read "anyone in the group, anywhere on this map", and inside a dungeon
    // somebody is almost always fighting - so the watch stood down for the whole 41 min run while Eraelor stood
    // alone in a mine tunnel 140 yd from its five and nothing noticed. A fight that far away positions nobody: the
    // bot is out of combat, no mob of that fight can reach it, and it cannot even see the fight. The stand-down is
    // about the fight this bot is standing in, so it holds only while a fighting member is within the bot's sight
    // (the core's own visibility fact, Playerbots.SightDistance - wider than any spot the leader places a member
    // on, so a ranged member at its combat spot is still covered). The bot's own combat is checked above.
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member != bot && member->IsInCombat() && member->GetMapId() == bot->GetMapId() &&
                    member->GetInstanceId() == bot->GetInstanceId() &&
                    bot->GetDistance(member) <= sPlayerbotAIConfig.SightDistance)
                    return nullptr;

    Player* leader = botAI->GetMaster();
    if (!leader || leader == bot)
        leader = botAI->GetGroupLeader();

    if (!leader || leader == bot || !leader->IsInWorld() || leader->IsBeingTeleported() ||
        leader->GetMapId() != bot->GetMapId() || leader->GetInstanceId() != bot->GetInstanceId())
        return nullptr;

    // A leader on a taxi, a boat, a vehicle or a flying mount is followed by other code, or cannot be reached on foot.
    if (!leader->IsAlive() || leader->GetTransport() || leader->GetVehicle() ||
        leader->HasUnitState(UNIT_STATE_IN_FLIGHT) || leader->IsFlying())
        return nullptr;

    return leader;
}

uint8 StuckWatchdogValue::Calculate()
{
    uint32 const now = getMSTime();
    Player* leader = GetWatchedLeader();
    if (leader)
        RecordLeaderTrail(leader);

    // Doing something on purpose is not being stuck: the follow action stands down in the same cases.
    // [nav192] b475 dm-n five 10: Eraelor and Feette (both hunters) stood 140 yd from their group in Deadmines and
    // this watch never armed for either. Auto Shot is an autorepeat spell, so IsNonMeleeSpellCast(false) reads a
    // cast under way on every tick a hunter has a shot queued ([lead523] fixed the same read in the leader's move
    // order): the whole escalation was dead for every autorepeat shooter. The autorepeat shot is no cast this check
    // keeps - a walk pauses it and the core resumes it.
    bool const busy = !leader || !botAI->CanMove() || bot->HasUnitState(UNIT_STATE_LOST_CONTROL) ||
                      bot->IsRooted() || !bot->IsStandState() || bot->IsNonMeleeSpellCast(false, false, true) ||
                      botAI->HasStrategy("move from group", BOT_STATE_NON_COMBAT) ||
                      botAI->HasStrategy("master fishing", BOT_STATE_NON_COMBAT);
    if (busy)
    {
        _lastSample = 0;
        return uint8(StuckStage::NONE);
    }

    if (!_lastSample || getMSTimeDiff(_lastSample, now) > SAMPLE_GAP_MS)
    {
        _lastSample = now;
        Restart(leader, now);
        return uint8(StuckStage::NONE);
    }

    _lastSample = now;
    _lastNow = now;

    Formation* formation = context->GetValue<Formation*>("formation")->Get();
    float const followDistance = formation ? formation->GetMaxDistance() : sPlayerbotAIConfig.FollowDistance;
    float const distance = bot->GetDistance(leader);
    if (distance <= followDistance + STUCK_EXTRA_DISTANCE)
    {
        Restart(leader, now);
        return uint8(StuckStage::NONE);
    }

    // Getting closer is progress; so is keeping pace with a leader who is on the move.
    bool progress = distance + PROGRESS_DISTANCE < _bestDistance;
    if (!progress && leader->GetExactDist2d(&_leaderAnchor) >= LEADER_MOVED_DISTANCE)
    {
        progress = bot->GetExactDist2d(&_botAnchor) >= PROGRESS_DISTANCE;
        _leaderAnchor.Relocate(leader);
        _botAnchor.Relocate(bot);
    }

    if (progress)
    {
        Restart(leader, now);
        return uint8(StuckStage::NONE);
    }

    // Close to the leader, wandering around (looting, gathering) is not a lack of progress; standing still is.
    if (distance <= std::max(FAR_DISTANCE, followDistance + STUCK_EXTRA_DISTANCE))
        _noCloserSince = now;

    if (bot->GetExactDist(&_stillAnchor) >= STILL_DISTANCE)
    {
        _stillAnchor.Relocate(bot);
        _stillSince = now;
    }

    uint32 const stuckSeconds = GetStuckSeconds();
    StuckStage due = StuckStage::NONE;
    if (stuckSeconds >= GIVE_UP_SECONDS)
        due = StuckStage::GIVE_UP;
    else if (stuckSeconds >= LEADER_TRAIL_SECONDS)
        due = StuckStage::LEADER_TRAIL;
    else if (stuckSeconds >= REBUILD_PATH_SECONDS)
        due = StuckStage::REBUILD_PATH;
    else if (stuckSeconds >= REFOLLOW_SECONDS)
        due = StuckStage::REFOLLOW;
    else if (stuckSeconds >= RESET_MOVEMENT_SECONDS)
        due = StuckStage::RESET_MOVEMENT;

    return due > _stageDone ? uint8(due) : uint8(StuckStage::NONE);
}

void StuckWatchdogValue::Restart(Player* leader, uint32 now)
{
    _noCloserSince = now;
    _stillSince = now;
    _lastNow = now;
    _bestDistance = bot->GetDistance(leader);
    _botAnchor.Relocate(bot);
    _stillAnchor.Relocate(bot);
    _leaderAnchor.Relocate(leader);
    _stageDone = StuckStage::NONE;
    _trailTries = 0;
}

void StuckWatchdogValue::Restart()
{
    _lastSample = 0;
    _stageDone = StuckStage::NONE;
    _trailTries = 0;
    value = uint8(StuckStage::NONE);
}

void StuckWatchdogValue::RecordLeaderTrail(Player* leader)
{
    // GetWatchedLeader() only hands out a leader on foot on our map, so every point is a walkable ground spot.
    if (leader->GetMapId() != _trailMapId || leader->GetInstanceId() != _trailInstanceId)
    {
        _leaderTrail.clear();
        _trailMapId = leader->GetMapId();
        _trailInstanceId = leader->GetInstanceId();
    }

    if (!_leaderTrail.empty() && leader->GetExactDist2d(&_leaderTrail.back()) < TRAIL_POINT_SPACING)
        return;

    _leaderTrail.emplace_back(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
    if (_leaderTrail.size() > TRAIL_MAX_POINTS)
        _leaderTrail.pop_front();
}

void StuckWatchdogValue::SetTrailTarget(Position const& target)
{
    _lastTrailTarget.Relocate(target);
    ++_trailTries;
}

bool StuckWatchdogValue::ShouldAnnounceStuck()
{
    if (_announced && _announcedMapId == bot->GetMapId() && bot->GetExactDist2d(&_announcedAt) < ANNOUNCE_SAME_SPOT)
        return false;

    _announced = true;
    _announcedMapId = bot->GetMapId();
    _announcedAt.Relocate(bot);
    return true;
}

void StuckWatchdogValue::MarkDone(StuckStage stage)
{
    _stageDone = std::max(_stageDone, stage);
    value = uint8(StuckStage::NONE);
}

uint32 StuckWatchdogValue::GetStuckSeconds() const
{
    uint32 const noCloser = getMSTimeDiff(_noCloserSince, _lastNow);
    uint32 const still = getMSTimeDiff(_stillSince, _lastNow);
    return std::max(noCloser, still) / IN_MILLISECONDS;
}

uint32 StuckWatchdogValue::GetStillSeconds() const { return getMSTimeDiff(_stillSince, _lastNow) / IN_MILLISECONDS; }

bool StuckWatchdogTrigger::IsActive()
{
    StuckWatchdogValue* watchdog = GetWatchdog(context);
    return watchdog && watchdog->Get() != uint8(StuckStage::NONE);
}

bool StuckRecoveryAction::isUseful()
{
    StuckWatchdogValue* watchdog = GetWatchdog(context);
    if (!watchdog)
        return false;

    StuckStage const stage = StuckStage(watchdog->LazyGet());
    if (stage == StuckStage::NONE)
        return false;

    if (stage != StuckStage::LEADER_TRAIL)
        return true;

    // Each trail point gets the time to be walked to before the next one is picked.
    return !_lastTrailTry || getMSTimeDiff(_lastTrailTry, getMSTime()) >= TRAIL_RETRY_MS;
}

bool StuckRecoveryAction::Execute(Event /*event*/)
{
    StuckWatchdogValue* watchdog = GetWatchdog(context);
    if (!watchdog)
        return false;

    StuckStage const stage = StuckStage(watchdog->LazyGet());
    Player* leader = watchdog->GetWatchedLeader();
    if (stage == StuckStage::NONE || !leader)
        return false;

    switch (stage)
    {
        case StuckStage::RESET_MOVEMENT:
            LogStep(StageName(stage), leader);
            ResetMovement();
            break;
        case StuckStage::REFOLLOW:
            LogStep(StageName(stage), leader);
            Refollow(leader);
            break;
        case StuckStage::REBUILD_PATH:
            LogStep(StageName(stage), leader);
            RebuildPath(leader);
            break;
        case StuckStage::LEADER_TRAIL:
            _lastTrailTry = getMSTime();
            if (watchdog->GetTrailTries() < TRAIL_MAX_TRIES && WalkLeaderTrail(watchdog))
            {
                LogStep(StageName(stage), leader);
                // Stays due (paced by isUseful) until the tries run out or the bot makes progress.
                if (watchdog->GetTrailTries() >= TRAIL_MAX_TRIES)
                    watchdog->MarkDone(stage);
                return true;
            }
            // No other way along the leader's trail: nothing is left to try.
            GiveUp(watchdog, leader);
            watchdog->MarkDone(StuckStage::GIVE_UP);
            return true;
        case StuckStage::GIVE_UP:
            GiveUp(watchdog, leader);
            break;
        default:
            return false;
    }

    // Each step runs once per stuck episode, whether or not it moved the bot; the next one is due later.
    watchdog->MarkDone(stage);
    return true;
}

bool StuckRecoveryAction::ResetMovement()
{
    bot->StopMoving();
    bot->GetMotionMaster()->Clear();
    AI_VALUE(LastMovement&, "last movement").clear();
    return true;
}

bool StuckRecoveryAction::Refollow(Player* leader)
{
    ResetMovement();
    // [queue-refollow-needs-path] No follow without a ground path to the leader: the core follow then goes in a
    // straight spline, through the air off a ledge. dtk-n 20261025-163758-dtk-n-5A-62: the tank fell to a 3-polygon
    // island at (-292,-676,55) (tools/navmesh_probe.py: NOT CONNECTED to the keep), "follow again" walked Rlmagaj
    // from (-294,-655,89) straight towards it - a fall death at 1318 s, the group waited for her, stall at 1531 s.
    // The next stages (rebuild path, the leader's trail) take over.
    if (bot->GetMap() && bot->GetMap()->IsDungeon() && !bot->IsFlying() && !bot->isSwimming())
    {
        BotPath const path =  // [lead109]
            FindBotPath(bot, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        if (!(path.type & PATHFIND_NORMAL))
            return false;
    }
    return Follow(leader);
}

bool StuckRecoveryAction::RebuildPath(Player* leader)
{
    ResetMovement();

    BotPath const path =  // [lead109]
        FindBotPath(bot, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
    PathType const type = path.type;
    Movement::PointsArray const& points = path.points;
    if ((type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) && points.size() > 1)
    {
        // An incomplete path still ends at the reachable point nearest to the leader.
        G3D::Vector3 const& end = points.back();
        if (MoveTo(bot->GetMapId(), end.x, end.y, end.z))
            return true;
    }

    // No usable path from this spot: step aside so the next attempt starts from a fresh polygon.
    float const side = urand(0, 1) ? static_cast<float>(M_PI_2) : -static_cast<float>(M_PI_2);
    return Move(bot->GetAngle(leader) + side, UNSTICK_STEP_DISTANCE);
}

bool StuckRecoveryAction::WalkLeaderTrail(StuckWatchdogValue* watchdog)
{
    std::deque<Position> const& trail = watchdog->GetLeaderTrail();
    Position const* lastTarget = watchdog->GetLastTrailTarget();

    // Newest first: the reachable point the leader passed most recently is the furthest along their way. The
    // leader's own spot fails the way the follow path did, so the search falls back to where they walked before.
    uint32 checks = 0;
    for (auto it = trail.rbegin(); it != trail.rend() && checks < TRAIL_MAX_PATH_CHECKS; ++it)
    {
        Position const& point = *it;
        if (bot->GetExactDist2d(&point) < TRAIL_MIN_STEP)
            continue;

        // The point of the last try was either reached (then it is behind us) or it is the one that did not work.
        if (lastTarget && lastTarget->GetExactDist2d(&point) < TRAIL_SAME_TARGET)
            continue;

        ++checks;
        BotPath const path =  // [lead109]
            FindBotPath(bot, point.GetPositionX(), point.GetPositionY(), point.GetPositionZ());
        if (path.type != PATHFIND_NORMAL)
            continue;

        ResetMovement();
        if (!MoveTo(bot->GetMapId(), point.GetPositionX(), point.GetPositionY(), point.GetPositionZ(), false, false,
                    true))
            continue;

        watchdog->SetTrailTarget(point);
        return true;
    }

    return false;
}

void StuckRecoveryAction::GiveUp(StuckWatchdogValue* watchdog, Player* leader)
{
    ResetMovement();

    // For whoever fixes the navmesh or the follow path: the exact spot, once per stuck episode.
    LOG_INFO("playerbots",
             "Bot {} <{}> stuck: needs path fix (map {} zone {} area {} at {:.1f} {:.1f} {:.1f}; leader {} at "
             "{:.1f} {:.1f} {:.1f}, {:.1f} yd; no progress {}s, trail tries {})",
             bot->GetGUID().ToString(), bot->GetName(), bot->GetMapId(), bot->GetZoneId(), bot->GetAreaId(),
             bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), leader->GetName(), leader->GetPositionX(),
             leader->GetPositionY(), leader->GetPositionZ(), bot->GetDistance(leader), watchdog->GetStuckSeconds(),
             uint32(watchdog->GetTrailTries()));

    if (!watchdog->ShouldAnnounceStuck())
        return;

    std::string const text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "stuck_needs_path_fix", "I am stuck and cannot find a way to you (map %map, %position). Come back for me.",
        {{"%map", std::to_string(bot->GetMapId())},
         {"%position", Acore::StringFormat("{:.1f} {:.1f} {:.1f}", bot->GetPositionX(), bot->GetPositionY(),
                                           bot->GetPositionZ())}});

    Group* group = bot->GetGroup();
    if (group && group->isRaidGroup())
        botAI->SayToRaid(text);
    else if (group)
        botAI->SayToParty(text);
    else
        botAI->TellMaster(text);
}

void StuckRecoveryAction::LogStep(char const* step, Player* leader)
{
    StuckWatchdogValue* watchdog = GetWatchdog(context);
    uint32 const stuckSeconds = watchdog ? watchdog->GetStuckSeconds() : 0;
    uint32 const stillSeconds = watchdog ? watchdog->GetStillSeconds() : 0;
    LOG_INFO("playerbots",
             "Bot {} <{}> stuck watchdog: {} (no progress {}s, still {}s, {:.1f} yd from {}, map {} at {:.1f} {:.1f} "
             "{:.1f})",
             bot->GetGUID().ToString(), bot->GetName(), step, stuckSeconds, stillSeconds, bot->GetDistance(leader),
             leader->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
}
