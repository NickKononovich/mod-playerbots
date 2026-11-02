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
#include "G3D/Vector3.h"
#include "GroupRouteState.h"
#include "LeaderLog.h"
#include "ObjectGuid.h"

#include <map>
#include <set>
#include <string>
#include <vector>

class Creature;
class InstanceScript;
class GameObject;
class Map;
class Player;
class WorldObject;

namespace Leader
{
    class LeaderGroup
    {
    public:
        // the leader's on-the-path verdict per idle candidate (GroupRouteState.h)
        using PathVerdict = GroupRouteState::PathVerdict;

        // a member as the leader sees it
        struct MemberInfo
        {
            ObjectGuid guid;
            std::string name;
            uint32 apartIgnoreUntilMs = 0;  // the group's apart rule ignores it until then (a member brought back)
            bool usesMana = false;          // its role and class use mana (the rest's mana line counts)
            uint32 backInMs = 0;            // when it came back in through the portal (0: never during this run)
            bool physical = false;          // a melee / ranged / tank role (the physical buffs are for it)
        };

        // [lead107 stage 5] what the group does now (the group owner's bookkeeping: the harness's run phase)
        enum class Activity : uint8
        {
            Waiting,
            Drinking,
            Traveling,
            Reviving
        };

        // a member's walk point (the route walk's waypoint registry the group's owner keeps)
        struct WalkPoint
        {
            uint32 mapId = 0;
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            uint32 serial = 0;      // a new waypoint, a new serial
            bool jump = false;      // a jump down off a route link's start
            bool straight = false;  // a walked route link's far end (walked straight first)
            bool gatePass = false;  // a walk through an auto-closing key gate
        };

        virtual ~LeaderGroup() = default;

        // the leader's own decision state (one per group leader)
        virtual DungeonLeaderState& State() = 0;
        // the run the group's decision lines belong to
        virtual DecisionRun Run() const = 0;
        // [lead107] the group's route state (its target, the walk, skips, verdicts, escort, doors, links ...)
        virtual GroupRouteState& Route() = 0;

        // members, the leader (the tank) and where the group is
        virtual std::vector<ObjectGuid> MemberGuids() const = 0;
        virtual std::vector<MemberInfo> Members() const = 0;
        // the group runs a content (a dungeon), and the leader decides its pulls (a tracked 5-man tank)
        virtual bool HasContent() const = 0;
        virtual bool LeaderDecides() const = 0;
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
        // the pull's own facts: its serial, who started it ("planned" / "unplanned: ..."), the dungeon script that
        // started it ("chain", "escort", ...; empty: none), a body pull walk on, the last pull's end (0: none)
        virtual uint32 PullSeq() const = 0;
        virtual std::string const& PullStarter() const = 0;
        virtual std::string const& PullScript() const = 0;
        virtual bool BodyPull() const = 0;
        virtual uint32 LastPullEndMs() const = 0;
        // the content's key ("rfc-h", ...; empty: none)
        virtual std::string ContentKey() const = 0;

        // the group needs its buffs (run start, after a wipe / a death)
        virtual bool NeedBuff() const = 0;
        // the instance's bosses (spawned when the run entered)
        virtual std::set<ObjectGuid> const& BossCreatures() const = 0;
        // the route's path verdicts and the verdict last logged per mob
        virtual std::map<ObjectGuid, PathVerdict> const& PathVerdicts() const = 0;
        virtual std::map<ObjectGuid, bool>& PathVerdictLogged() = 0;

        // the live mob of a phantom combat the group was sent to (empty: none)
        virtual ObjectGuid PhantomHunt() const = 0;

        // the route's state: when the leader arrived at its stop (0: not yet), a member cut off behind a closed door
        // the group holds for
        virtual uint32 ArrivedMs() const = 0;
        virtual bool DoorCutHold() const = 0;
        virtual ObjectGuid DoorCutMember() const = 0;
        // the pack the route must clear (a route blocker, the only way to a required target); `why` names the rule
        virtual bool NeedsPack(Map* map, WorldObject const* object, std::string& why) const = 0;
        // the event starter (index) that opens `door` and that a member can use (-1: none), and its note
        virtual int32 DoorStarterFor(GameObject const* door) const = 0;
        virtual std::string StarterNote(int32 index) const = 0;
        // a walk to a door to open by use is on for the leader
        virtual bool HasDoorWalk() const = 0;
        // a member's walk point (false: none) and its end
        virtual bool GetWaypoint(ObjectGuid bot, WalkPoint& out) const = 0;
        virtual void ClearWaypoint(ObjectGuid bot) = 0;

        // the group actions the leader takes
        virtual void SkipTarget(ObjectGuid guid, uint32 ms, std::string const& reason) = 0;
        virtual void ClearPhantomHunt() = 0;
        virtual void LogEvent(std::string const& type, JsonObject fields) = 0;
        // the route's skip of `guid` ends (released by the leader's facts)
        virtual void ReleaseSkip(ObjectGuid guid) = 0;
        // the leader's walk made progress at `now` (its declared wait is no lack of progress)
        virtual void ResetSteerClock(uint32 now) = 0;
        // the route chooses its target again at `ms`
        virtual void RetargetAt(uint32 ms) = 0;
        // the boss the planner deferred a pack to is the route target ("boss on the way")
        virtual void SetBossOnTheWay(ObjectGuid boss, uint32 now) = 0;
        // the pack is required (the planner's required marks / the old aoe-pull marks)
        virtual void MarkRequired(ObjectGuid mob) = 0;
        // the walks go round a hopeless pack
        virtual void NoteHopelessPack(Player* leader, Creature* mob) = 0;
        // the leader walks to `door` (opened by use) before the pull of `blocker`, at most until `untilMs`
        virtual void StartDoorWalk(ObjectGuid door, ObjectGuid blocker, G3D::Vector3 const& point, uint32 untilMs) = 0;
        // the run fails (planner_no_option with a stop)
        virtual void FailRun(std::string const& reason) = 0;
        // the leader's walk to (x, y, z) (the route walk's waypoint; `straight`: walked straight)
        virtual void SetLeaderWaypoint(Player* leader, float x, float y, float z, bool straight = false) = 0;
        // a boss encounter of the run is in progress
        virtual bool BossEncounterInProgress() const = 0;
        // a boss (or one of its kill credits) of `entry` was killed this run; a boss of the run is still open
        virtual bool BossKilledFor(uint32 entry) const = 0;
        virtual bool BossesOpen() const = 0;
        // a member's walk point given by the group's owner (the member walks it itself), its end, whether one is on
        virtual void SetMemberWaypoint(ObjectGuid member, Player* bot, float x, float y, float z) = 0;
        virtual void ClearMemberWaypoint(ObjectGuid member) = 0;
        virtual bool MemberHasWaypoint(ObjectGuid member) const = 0;
        virtual void SetMemberHasWaypoint(ObjectGuid member, bool has) = 0;
        // a member's walk to an instance portal is set (SelfTravel's portal goal)
        virtual void SetMemberHasPortalGoal(ObjectGuid member, bool has) = 0;
        virtual void ClearMemberIntent() = 0;
        // the run's wipes, the run's last progress (its stall watch) and a progress now
        virtual uint32 Wipes() const = 0;
        virtual uint32 LastProgressMs() const = 0;
        virtual void NoteProgress(uint32 ms) = 0;
        // the group owner's walk point for a bot (the route walk's waypoint: jumped / walked straight) and the
        // group's instance script
        virtual void GiveWaypoint(Player* bot, float x, float y, float z, bool jump = false, bool straight = false) = 0;
        virtual ::InstanceScript* InstanceScript() const = 0;
        // the bot's walk point is an auto-closing gate's pass (cleared with the pass)
        virtual void MarkGateWaypoint(Player* bot) = 0;
        // the run's bosses (their facts), the content's difficulty / raid flag / size / wing bosses, a creature's
        // boss order (-1: none)
        virtual std::vector<std::reference_wrapper<BossFacts const>> Bosses() const = 0;
        virtual uint8 Difficulty() const = 0;
        virtual bool IsRaid() const = 0;
        virtual uint8 ContentSize() const = 0;
        virtual std::vector<uint32> const& ContentBossEntries() const = 0;
        virtual int32 BossOrder(Creature const* creature) const = 0;
        // members out of the instance and in again through `entrance` (a shortcut / wing change)
        virtual void ShortcutReenter(std::vector<Player*> const& movers, size_t entrance, char const* why, float walk,
                                     float entranceWalk, std::string const& goal) = 0;
        // the group stands fully ready for a pull the route makes (its one readiness verdict when the leader leads);
        // an instance script event runs (a boss IN_PROGRESS without its encounter); the run levels (a normal dungeon
        // below the top level); a regroup after a revive runs
        virtual bool GroupFullyReady(Player* leader) const = 0;
        virtual bool ScriptEventRunning() const = 0;
        virtual bool Levelling() const = 0;
        virtual bool Regrouping() const = 0;
        // the member has no walk to the leader either way (stranded: no wait for it, no meet)
        virtual bool MemberStranded(Player* bot, Player* leader) = 0;
        // the run ends with `result` ("failed", "partial") for `reason`
        virtual void FinishRun(std::string const& result, std::string const& reason) = 0;
        // a member's own movement could not do it (bot_cannot_move)
        virtual void LogCannotMove(Player* bot, std::string const& capability, std::string const& reason, float toX,
                                   float toY, float toZ) = 0;
        // [lead107 stage 5] the group does `activity` now (its owner's bookkeeping)
        virtual void NoteActivity(Activity activity) = 0;
        // the group's apart rule ignores `member` until `untilMs`
        virtual void SetApartIgnore(ObjectGuid member, uint32 untilMs) = 0;
        // a member with no walk back to the group is put in again at the entrance by the group's owner (true: it was,
        // counted as a death)
        virtual bool StrandedReenter(ObjectGuid member, Player* bot, Player* leader) = 0;
        // the owner's test setup of a buffing pass, a member's consumables (true: it used a flask / food) and a
        // member's buff state for the log
        virtual void BuffPassSetup(std::vector<Player*> const& players) = 0;
        virtual bool UseConsumables(Player* bot, bool food) = 0;
        virtual std::string MemberBuffJson(Player* bot) = 0;
    };
}

#endif
