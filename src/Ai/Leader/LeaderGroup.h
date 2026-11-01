/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERGROUP_H
#define PLAYERBOTS_LEADER_LEADERGROUP_H

// [lead107] the group as the dungeon leader sees it: who is in it, where it is, the route target the leader walks to
// (and the route rule that chose it), whether it fights, the bosses of the instance, and the few group actions the
// leader takes that the group's owner carries out (a skipped target, the end of a phantom-combat hunt, an event line).
// The test harness implements it from its Party; a group outside a test run implements it from its own state.

#include "DungeonLeaderState.h"
#include "LeaderLog.h"
#include "ObjectGuid.h"

#include <map>
#include <set>
#include <string>
#include <vector>

class Player;

namespace Leader
{
    class LeaderGroup
    {
    public:
        // the leader's on-the-path verdict per idle candidate (the route rules: blocks route, patrol crosses route,
        // required, boss area, joins the boss ...; off the path: its route distance and reach)
        struct PathVerdict
        {
            bool on = false;
            std::string why;
            float routeDist = -1.0f;
            float reach = 0.0f;
        };

        virtual ~LeaderGroup() = default;

        // the leader's own decision state (one per group leader)
        virtual DungeonLeaderState& State() = 0;
        // the run the group's decision lines belong to
        virtual DecisionRun Run() const = 0;

        // members, the leader (the tank) and where the group is
        virtual std::vector<ObjectGuid> MemberGuids() const = 0;
        virtual ObjectGuid LeaderGuid() const = 0;
        virtual uint32 MapId() const = 0;
        virtual uint32 InstanceId() const = 0;

        // the route target and the rule that chose it ("boss", "required", "blocks route", ...; empty: set elsewhere)
        virtual ObjectGuid RouteTarget() const = 0;
        virtual std::string const& RouteRule() const = 0;
        virtual bool RouteTargetIsBoss() const = 0;
        virtual bool RouteTargetIsStarter() const = 0;

        // the fight: the group's pull state, ONE definition of "the group fights" (Party::FightOn), the pull's start
        virtual bool InCombat() const = 0;
        virtual bool FightOn(Player* leader) const = 0;
        virtual uint32 PullStartMs() const = 0;

        // the group needs its buffs (run start, after a wipe / a death)
        virtual bool NeedBuff() const = 0;
        // the instance's bosses (spawned when the run entered)
        virtual std::set<ObjectGuid> const& BossCreatures() const = 0;
        // the route's path verdicts and the verdict last logged per mob
        virtual std::map<ObjectGuid, PathVerdict> const& PathVerdicts() const = 0;
        virtual std::map<ObjectGuid, bool>& PathVerdictLogged() = 0;

        // the live mob of a phantom combat the group was sent to (empty: none)
        virtual ObjectGuid PhantomHunt() const = 0;

        // the group actions the leader takes
        virtual void SkipTarget(ObjectGuid guid, uint32 ms, std::string const& reason) = 0;
        virtual void ClearPhantomHunt() = 0;
        virtual void LogEvent(std::string const& type, JsonObject fields) = 0;
    };
}

#endif
