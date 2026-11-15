/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STUCKWATCHDOG_H
#define PLAYERBOTS_STUCKWATCHDOG_H

#include "MovementActions.h"
#include "Position.h"
#include "Trigger.h"
#include "Value.h"

#include <deque>

class Player;
class PlayerbotAI;

// Escalation steps of a bot that follows someone and makes no progress towards them.
enum class StuckStage : uint8
{
    NONE = 0,
    RESET_MOVEMENT = 1,
    REFOLLOW = 2,
    REBUILD_PATH = 3,
    LEADER_TRAIL = 4,  // walk a different way: the points the leader actually walked through
    GIVE_UP = 5        // stop, log the spot for a navmesh/path fix, tell the group once
};

// Samples the follow progress once a second and answers which escalation step is due (NONE when nothing is).
// "Progress" is getting closer to the one we follow, or keeping up with them while they move. Time the bot
// spends doing something on purpose (fighting, eating, casting, CC, taxi, transports) restarts the watch.
class StuckWatchdogValue : public CalculatedValue<uint8>
{
public:
    StuckWatchdogValue(PlayerbotAI* botAI) : CalculatedValue<uint8>(botAI, "stuck watchdog", 1 * IN_MILLISECONDS) {}

    // The one this bot follows, or nullptr when the watch does not apply right now.
    Player* GetWatchedLeader();
    void MarkDone(StuckStage stage);
    void Restart();
    uint32 GetStuckSeconds() const;
    uint32 GetStillSeconds() const;
    // [stuck260] what the criterion measured: the gap this episode armed at, and the yards of it actually closed
    float GetArmGap() const { return _armGap; }
    float GetClosedYd() const { return _armGap - _gapToBeat; }

    // Where the leader walked recently on this map, oldest first, a point every few yards.
    std::deque<Position> const& GetLeaderTrail() const { return _leaderTrail; }
    // The leader-trail walk of this stuck episode: how many tries, and the point of the last one.
    uint8 GetTrailTries() const { return _trailTries; }
    Position const* GetLastTrailTarget() const { return _trailTries ? &_lastTrailTarget : nullptr; }
    void SetTrailTarget(Position const& target);
    // True once per stuck spot: a bot stuck at the same place again does not repeat its message.
    bool ShouldAnnounceStuck();

protected:
    uint8 Calculate() override;

private:
    void Restart(Player* leader, uint32 now);
    void RecordLeaderTrail(Player* leader);

    uint32 _lastSample = 0;
    uint32 _noCloserSince = 0;
    uint32 _stillSince = 0;
    uint32 _lastNow = 0;
    // [stuck260] the gap THIS episode has to beat, and whether the episode has started at all. The watch arms on
    // the first sample the bot is behind the group and takes the reference there: a gap measured while the bot was
    // with its leader is not a reference for being left behind (FollowProgress.h holds the criterion itself)
    bool _armed = false;
    float _gapToBeat = 0.0f;
    float _armGap = 0.0f;  // the gap this episode started at, so the row can state what it measured
    Position _botAnchor;
    Position _stillAnchor;
    Position _leaderAnchor;
    StuckStage _stageDone = StuckStage::NONE;

    std::deque<Position> _leaderTrail;
    uint32 _trailMapId = 0;
    uint32 _trailInstanceId = 0;
    uint8 _trailTries = 0;
    Position _lastTrailTarget;

    bool _announced = false;
    uint32 _announcedMapId = 0;
    Position _announcedAt;
};

class StuckWatchdogTrigger : public Trigger
{
public:
    StuckWatchdogTrigger(PlayerbotAI* botAI) : Trigger(botAI, "stuck watchdog") {}

    bool IsActive() override;
};

// Runs the step the watchdog says is due: reset movement, follow again, rebuild the path (or side-step), then walk
// a different way along the leader's own recent trail, and as the last resort stop, log the spot as needing a path
// fix and tell the group once. Bots play like players: there is no teleport anywhere in this chain.
class StuckRecoveryAction : public MovementAction
{
public:
    StuckRecoveryAction(PlayerbotAI* botAI) : MovementAction(botAI, "stuck recovery") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool ResetMovement();
    bool Refollow(Player* leader);
    bool RebuildPath(Player* leader);
    bool WalkLeaderTrail(StuckWatchdogValue* watchdog);
    void GiveUp(StuckWatchdogValue* watchdog, Player* leader);
    void LogStep(char const* step, Player* leader);

    uint32 _lastTrailTry = 0;
};

#endif
