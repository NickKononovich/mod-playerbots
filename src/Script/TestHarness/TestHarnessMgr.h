/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTHARNESSMGR_H
#define PLAYERBOTS_TESTHARNESSMGR_H

#include "ObjectGuid.h"
#include "Position.h"
#include "G3D/Vector3.h"  // [lead97] the route leg's path
#include "TestHarnessData.h"
#include "TestHarnessLog.h"
#include "TestHarnessPrepare.h"

#include <deque>
#include <functional>
#include <map>
#include <optional>  // [lead76]
#include <memory>
#include <set>
#include <string>
#include <vector>

class ChatHandler;
class Creature;
class GameObject;
class InstanceScript;
class Map;
class Player;
class Unit;  // [lead3]
#include "HazardZone.h"  // [lead16]
#include "DungeonLeaderState.h"  // [lead107]
#include "LeaderGroup.h"         // [lead107] the leader's group view (PartyLeaderGroup)
#include "LeaderBuffing.h"       // [lead107] HasWellFed
#include "LeaderConsumables.h"   // [lead344] HasFlaskAura

namespace Leader::Travel
{
    bool BlackMorassHoldsAtMedivh(Player* member);  // [lead107] RouteMeet.h
}

namespace PullPlanner
{
    struct Decision;  // [lead15] ChooseTargetActions.h
}
class WorldObject;
struct AreaTrigger;

namespace TestHarness
{
    // Test accounts are named <prefix>NN; the random bot manager only takes "rndbot%" accounts and the
    // addclass pool only playerbots_account_type 2, so neither ever sees them.
    constexpr char const* TEST_ACCOUNT_PREFIX = "RRTEST";
    constexpr uint32 PROTECTED_ACCOUNT_ID = 101;  // the user's own account (PLAYER)
    constexpr uint32 CHARACTERS_PER_ACCOUNT = 10;
    // Character names: "Rt<class><aa..zz>" for the level-80 pool, "Rl<class><aa..zz>" for the levelling pool
    // ("test setup <n> leveling"), which is only ever used by normal dungeons below level 80.
    constexpr char const* NAME_PREFIX_MAX_LEVEL = "Rt";
    constexpr char const* NAME_PREFIX_LEVELING = "Rl";

    enum class Phase : uint8
    {
        Picking,
        LoggingIn,
        Preparing,
        Grouping,
        Idle,         // prepared party, no run
        Teleporting,
        Buffing,
        ReadyCheck,
        Traveling,
        Waiting,      // members far behind
        Drinking,
        EventStart,
        Clearing,     // trash combat
        BossFight,
        Reviving,     // corpse run after a wipe
        Done,
        Failed,
        Stalled,
        Stopped,
        // [guild60] a guild five walks in through the portal itself (no teleport, no preparation)
        Entering
    };

    char const* PhaseName(Phase phase);
    // [hleft-bm-tank-fetch] Black Morass while the event runs (rift >= 1, shield > 0): `member` holds at Medivh (within
    // BM_MEDIVH_HOLD_RANGE) while the tank fetches the rift keepers alone (tbc-bm strategy) - no far wait, no regroup
    // and no meet for him.
    using Leader::Travel::BlackMorassHoldsAtMedivh;  // [lead107] RouteMeet.h
    bool IsTerminalPhase(Phase phase);

    struct Member
    {
        ObjectGuid guid;
        std::string name;
        uint8 cls = 0;
        SpecDef const* spec = nullptr;
        CompSlot slot{};
        std::pair<uint16, uint16> professions{0, 0};
        PrepareResult prep;
        bool prepared = false;
        uint32 deadSinceMs = 0;
        uint32 awaySinceMs = 0;  // dead or outside since (AWAY_MAX_MS is per member, not "somebody away")
        // Back in through the portal (MeetMember's regroup): seen on the run's map, then off it, then on it again.
        bool seenInside = false;
        bool wasOutside = false;
        uint32 backInMs = 0;  // when it came back in (0: never during this run)
        uint32 combatMs = 0;  // time alive and in combat during the run (sampled every harness tick)
        uint32 aggroPulled = 0;  // mobs that stayed on this non-tank member for AGGRO_LOSS_MS or longer
        uint32 unplannedStarts = 0;  // fights this non-tank member entered combat first (pull_start "unplanned")
        MemberCounters pullStart;  // counters when the current pull started
        uint32 pullStartCombatMs = 0;
        // Intent given to this member (not the leader): its own "test runner move" walks to
        // the waypoint (dry spot, gather spot, link), its own "instance portal" into the instance.
        bool hasWaypoint = false;
        bool hasPortalGoal = false;
        uint32 noWalkSinceMs = 0;  // away and not moving since (bot_cannot_move "corpse run")
        // Alive outside, no walk back for OUTSIDE_GIVE_UP_STUCKS windows of GHOST_STUCK_MS: no longer waited for
        // (dtk-n build 10: a spirit-healed shaman stood 30 yd above the portal for 4 minutes).
        uint32 outsideStuckCount = 0;
        // A ping-pong hold timed out with this member still apart: until then it is neither apart nor far (gd-h /
        // wc-h build 17: Rtdruay 37 yd, dz -7.6, held the group 160 s over repeated holds).
        uint32 apartIgnoreUntilMs = 0;
        bool leftOutside = false;
        uint32 entryRefusedSinceMs = 0;  // on its entrance, still outside: the core refuses entry since
        std::string entryRefusal;        // the refusal last logged (entry_refused)
        Position noWalkPos;
        // MemberStranded: no navmesh walk to the leader either way, re-checked every STRANDED_RECHECK_MS.
        bool stranded = false;
        uint32 strandedCheckMs = 0;
        uint32 strandedSinceMs = 0;
        Position strandedPos;
        // [lead341] the stranded re-entry watch is the leader's (GroupRouteState::reenter, RouteReenter.h)
        // [lead107 stage 5] the rest / far-wait facts are the leader's (GroupRouteState::rest, Leader::MemberRest)
    };

    struct BossTrack : Leader::BossFacts  // [lead107] the facts the leader reads
    {
        bool stateChanged = false;  // the state moved at least once during the run (IsPhantomSlot)
        std::string name;
        uint32 attempts = 0;
        std::string failReason;  // [hleft-script-slot-names] a failed slot's own reason (ScriptSlotDef::failNote)
        uint32 attemptStartMs = 0;
        std::map<ObjectGuid, MemberCounters> attemptStart;
        std::map<ObjectGuid, uint32> attemptStartCombatMs;
        std::vector<std::string> attemptDeaths;
        // boss_phantom (build 11): the boss's health while the attempt runs, and since when it has not changed.
        uint32 attemptKills = 0;  // Party::kills when the attempt was engaged (boss_attempt_loop)
        // [w24c-hor-escape] Halls of Reflection escape: walls seen up / broken (bit per ice wall), and the last
        // wall broken (a boss_attempt_loop window starts there, not at the engage).
        uint8 horWallsUp = 0;
        uint8 horWallsBroken = 0;
        uint32 loopProgressMs = 0;
        uint32 loopProgressKills = 0;
        uint32 phantomAttemptMs = 0;
        uint32 phantomHp = 0;
        uint32 phantomSinceMs = 0;
        bool phantomLogged = false;
        std::map<ObjectGuid, uint32> attemptStartAggro;

        // Identity and route order (DungeonEncounter.dbc / instance_encounters / the InstanceScript's own guids)
        bool dbc = false;         // tracked from DungeonEncounter.dbc: the InstanceScript keeps no boss states
        bool engaged = false;     // a party member fought it during the current IN_PROGRESS
        uint32 npcResets = 0;     // IN_PROGRESS -> reset without the party (script NPCs engaging the boss)
        uint32 scriptedResets = 0;  // [b35_urom] IN_PROGRESS -> NOT_STARTED by the boss's own script (no wipe)
        uint32 nextResolveMs = 0; // ResolveBoss retry (a dbc lookup scans the map's creatures)
        // The boss creature when the current attempt was engaged (suspicious-kill check: heroic templates missing)
        uint8 engageLevel = 0;
        uint32 engageMaxHp = 0;
    };

    // One DungeonEncounter.dbc row of the run's map and difficulty.
    struct EncounterInfo
    {
        uint32 index = 0;  // encounterIndex (completed-encounter bit, also the dungeon's boss order)
        std::string name;
        std::vector<uint32> credits;  // instance_encounters kill-credit creature entries
    };

    // A hostile that left the tanks for a non-tank member (aggro_loss metric).
    struct LooseMob
    {
        ObjectGuid victim;
        uint32 sinceMs = 0;
        bool logged = false;
        bool boss = false;
        uint32 entry = 0;
        std::string name;
        std::string victimName;
        std::string victimRole;
    };

    // [lead107] the starters' and the area triggers' run state live in the leader (GroupRouteState.h)
    using Leader::StarterTrack;
    using Leader::TriggerTrack;

    struct Party : Leader::GroupRouteState  // [lead107] the route state is the leader's
    {
        uint32 seq = 0;
        std::string id;  // run id, or "P<seq>" for a prepared party
        Composition const* comp = nullptr;
        ContentDef const* content = nullptr;
        uint32 bossAuraSamples = 0;  // [raid4] positions samples of boss attempts (the boss_auras cadence)
        uint32 worldBossSpawn = 0;  // [world_boss] the boss's creature spawn id (LoadEntrances), 0: not a world boss
        uint32 worldBossWaitMs = 0;  // [b342] since when the arrived group waits for the boss to appear
        std::string worldBossPrepState;  // [b342] the last logged world_boss_prep state (logged on a change)
        uint8 level = 80;
        uint32 targetIlvl = 0;
        // [kara3] a "continue" run: the members keep their raid binds and re-enter the same instance save (its dead
        // bosses and trash stay dead - the lockout, a game mechanic), the run goes on from the next boss
        bool resume = false;
        uint32 keptInstanceId = 0;  // [strat1] a 5-man continue: the instance its kept group holds
        std::vector<Member> members;
        ObjectGuid leader;
        ObjectGuid groupGuid;

        Phase phase = Phase::Picking;
        uint32 phaseSinceMs = 0;
        uint32 createdMs = 0;
        uint32 runStartMs = 0;
        uint32 nextTickMs = 0;
        uint32 timeoutMs = 0;
        std::string result;
        std::string reason;
        bool fromQueue = false;
        // [guild60] a guild five's dungeon run (GuildCoordinator): its own characters as they are (no pool, no
        // preparation, no teleport, no test setup or handouts), logged in before and after; false: a test run
        bool guild = false;
        uint32 guildFiveId = 0;

        // instance
        uint32 instanceId = 0;
        // [hleft-daily-reset] when the server's global reset of this map / difficulty comes (InstanceSaveMgr), 0: none
        time_t instanceResetAt = 0;
        uint32 teleportTries = 0;
        // Members found inside an earlier run's instance of the content map: ported out first, then unbound from it.
        std::map<ObjectGuid, uint32> staleInstances;

        // runner
        // [lead107] the route / steer state is the leader's (Leader::GroupRouteState, the Party's base)
        // [lead107] the leader's own decision state lives in the leader (Ai/Leader/DungeonLeaderState.h)
        using LeaderState = Leader::DungeonLeaderState;
        LeaderState lead;
        // [lead84] ONE definition of "the group fights", the harness's (Steer) and the leader's: the harness's pull
        // state (its phantom rule counts a no-exchange combat out), or the leader flagged with a mob on it. A leader
        // flagged in combat with nobody on it is a lingering combat, not a fight
        bool FightOn(Player* leader) const;

        // combat
        bool inCombat = false;
        uint32 pullSeq = 0;
        uint32 pullStartMs = 0;
        uint32 lastPullEndMs = 0;  // [pull_label] when the last pull_end was logged (getMSTime, 0: none yet)
        std::string pullStarter;  // pull_start: "planned" or "unplanned: <name> <spec>/<role>"
        // [lead1] a pull the dungeon's script started, not a pull decision ("chain", "vehicle", "summon", "escort";
        // empty: none known): the decision log's pull_start names it who "event" instead of "none"
        std::string pullScript;
        bool pullIsBoss = false;
        uint32 pullKills = 0;
        // [b30_mech_pos] trash_stalemate: the pull this watch belongs to, since when nothing died and no enemy reached
        // a new lowest health, the kills then, each enemy's lowest health % seen, and whether the focus was set
        uint32 stalemateSeq = 0;
        uint32 stalemateSinceMs = 0;
        uint32 stalemateKills = 0;
        std::map<ObjectGuid, float> stalemateLowPct;
        bool stalemateFocused = false;
        std::vector<std::string> pullDeaths;
        std::vector<BossTrack> bosses;
        uint32 kills = 0;
        uint32 windowKills = 0;
        uint32 wipes = 0;
        uint32 wipeMs = 0;
        // [b30_st_spirit] deaths of the run (not re-entries) within DEATH_BURST_WINDOW_MS, and when they first reached
        // DEATH_BURST_COUNT: no recovery within DEATH_BURST_RECOVER_MS after that counts as a wipe
        std::deque<uint32> recentDeathMs;
        uint32 deathBurstMs = 0;
        uint32 deathBurstCount = 0;
        uint32 corpseRunMs = 0;
        uint32 combatSampleMs = 0;  // last combat-time sample (Member::combatMs)
        std::vector<EncounterInfo> encounters;  // DungeonEncounter.dbc rows, encounterIndex order
        std::map<ObjectGuid, LooseMob> looseMobs;
        uint32 aggroLosses = 0;
        uint32 lastPositionsMs = 0;  // "positions" event, every POSITIONS_MS in combat
        // members dead or outside the instance while the rest of the group is not wiped
        bool membersAway = false;
        uint32 awaySinceMs = 0;
        // Boss progress (a boss killed or failed); no progress for NO_BOSS_PROGRESS_*_MS fails the run (user rule:
        // nothing killed for that long is a failure to investigate, not a slow run).
        uint32 lastBossProgressMs = 0;
        uint32 bossProgressCount = 0;
        bool friendlyOnlyLogged = false;  // [w24c-no-done-by-exclusion] logged once per run
        uint32 killsAtBossProgress = 0;
        std::map<uint32, uint32> killsSinceBoss;  // creature entry -> kills since the last boss progress
        // Trash loop: kills go on inside TRASH_LOOP_RADIUS of loopAnchor for TRASH_LOOP_MS without boss progress
        // (reviving or endless trash: Nexus Frayers, Hadronox's adds) -> the area is skipped, the next boss is the
        // target until bossRushUntilMs.
        Position loopAnchor;
        uint32 loopSinceMs = 0;
        uint32 loopKills = 0;
        // [trash_loop_progress] the alive spawned hostiles round loopAnchor when the window began (loopAliveSinceMs:
        // the loopSinceMs it was counted for)
        uint32 loopAliveAtStart = 0;
        uint32 loopAliveSinceMs = 0;
        uint32 releasedIdleSinceMs = 0;  // Violet Hold: since when a released prisoner stands unfought
        // How often each creature came back as the target after it had died (a reviving / respawning mob), and the
        // chosen creatures seen dead since (Retarget). Choices of a mob that never died do not count: dme-n
        // 2026-10-25 skipped a living Imp / Fel Lash / Whip Lasher as "respawning trash" after four refusals.
        // Careful-pull refusals per pack member (heroic / level 80: deferred twice, then body-pulled).
        // [lead107 stage 5] the phantom-combat facts, the buffing pass and the waits are the leader's (GroupRouteState)

        // heartbeat / stall
        uint32 lastHeartbeatMs = 0;
        Position heartbeatPos;
        uint32 lastProgressMs = 0;
        Position progressPos;
        uint32 progressMapId = 0;
        uint8 stallLevel = 0;
        uint32 stuckEvents = 0;
        // Idle guard (CheckIdle, user rule 2026-10-25: two minutes without a fight is a blocker): time without a fight
        // outside rest / buffing (idleMs) and in genuine event waits (eventWaitMs), each logged once per episode.
        uint32 idleTickMs = 0;
        // ROUTE_DETOUR_FALLBACK: until then detours are targets too (everything left was a detour: the long way).
        uint32 rearmCheckLogMs = 0;  // ReopenKeyDoorHeld: the last "rearm_check" line (a failed re-key, its reason)
        // trash_phantom (build 18): the health of the mobs the party fights (no boss attempt), and since when no mob
        // lost any (or the set changed).
        uint64 trashHp = 0;
        uint32 trashMobs = 0;
        uint32 trashFlatSinceMs = 0;
        bool trashPhantomLogged = false;
        bool idleLogged = false;
        bool eventWaitLogged = false;
        // Bots walk by themselves: what the bots' own movement could not do (bot_cannot_move, throttled per bot,
        // capability and reason).
        uint32 cannotMoveEvents = 0;
        std::map<std::string, uint32> cannotMoveLoggedMs;
        uint32 ghostPosMs = 0;  // last "ghost_pos" event
        // A wing change walked (Scarlet Monastery, Dire Maul, Maraudon): out through the exit, in through the next
        // entrance, every member on its own (SelfTravel portal goal).
        bool gapsLogged = false;

        // Results that cannot be trusted (a boss far below the group's level, killed much faster than its health
        // allows): one line per boss, "suspicious" event + run_end.suspicious.
        std::vector<std::string> suspicious;
    };

    // What the boss of that track needs to appear (TestHarnessData.cpp encounterRequirements), nullptr if nothing.
    char const* TrackRequirement(uint32 mapId, BossTrack const& boss, char const** note = nullptr);
    // User rule: a boss only one class can unlock (ClassGatedBossDef) is the only one a run may leave alive; its
    // reason, nullptr for every other boss. Such a track is not required for "done" and is listed as excluded.
    char const* TrackClassGate(Party const& party, BossTrack const& boss);
    // A script slot that is no boss: EncounterCount above the real boss count (Blood Furnace 4 for 3 bosses). No
    // dbc row at its index, never named, no creature credit, state never moved. Not required for "done"; listed in
    // run_start.phantomSlots. A state change makes it a normal required track again.
    bool IsPhantomSlot(Party const& party, BossTrack const& boss);
    // Bosses the run must kill: every track but phantom slots and class-gated bosses (bossesTotal in the logs).
    uint32 RequiredBossCount(Party const& party);
    using Leader::Dungeon::HasFlaskAura;  // [lead344] LeaderConsumables.h
    using Leader::Dungeon::HasWellFed;  // [lead107] LeaderBuffing.h
    // Combat-time fields of one member between two snapshots: activeSec, activeDps, activeHps, activeDtps.
    JsonObject CombatStatsJson(MemberCounters const& now, MemberCounters const& start, uint32 combatMs);
    // Sum of ExpectedMemberDps over the party (each member's own level and item level).
    float ExpectedGroupDps(Party const& party);
    // [b30_reenter] the member's last recorded intention ("doing", "goingTo" with its movement source, "why"),
    // a JSON object, or "null"
    std::string MemberIntentJson(ObjectGuid guid);

    struct PoolCharacter
    {
        ObjectGuid guid;
        std::string name;
        uint8 cls = 0;
        uint32 account = 0;
        uint8 level = 1;
        bool leveling = false;  // "Rl" name: kept for content below level 80
        bool horde = false;     // a Horde race (Horde compositions only)
    };

    class TestHarnessMgr
    {
        friend class PartyLeaderGroup;  // [lead107] its SkipTarget / LogEvent

    public:
        static TestHarnessMgr& Instance();

        void Update(uint32 diff);
        void Shutdown();

        bool HandleSetup(ChatHandler* handler, uint32 groups, bool leveling, bool variants, bool horde = false);
        bool HandlePrepare(ChatHandler* handler, std::string const& compId, uint8 level, uint32 ilvl);
        bool HandleRun(ChatHandler* handler, std::string const& contentKey, uint32 count, std::string const& compId,
                       bool resume = false);
        void SetRunAllRandomComp(bool on) { _runAllRandomComp = on; }
        bool HandleRunAll(ChatHandler* handler, std::string const& set, uint32 parallel, bool variants,
                          uint32 window = 10);
        bool HandleStatus(ChatHandler* handler);
        bool HandleStop(ChatHandler* handler, std::string const& id);
        bool HandleList(ChatHandler* handler);

        bool IsTestCharacter(ObjectGuid guid) const;

        // [guild70] the clearing engine of the five-leader's own run (Ai/Leader/Dungeon/LeaderDungeonRun.h, the
        // HarnessClearEngine in TestHarnessMgr.cpp): the five stands inside, grouped by its five-leader; the clearing
        // runs as a party without any test setup, handouts or teleports (Party::guild), its end goes back to
        // Leader::Dungeon::DungeonRuns::OnCleared. members: (guid, "tank" / "healer" / "dps"); `leader` decides.
        // Returns the party's run id, or "" with `error`.
        std::string StartGuildRun(std::string const& contentKey,
                                  std::vector<std::pair<ObjectGuid, std::string>> const& members, ObjectGuid leader,
                                  uint32 fiveId, std::string const& runId, std::string& error);
        // the five's run ends "stopped" (its five was disbanded); its phase name ("" when none runs)
        void StopGuildRun(uint32 fiveId, std::string const& reason);
        std::string GuildRunPhase(uint32 fiveId) const;

        // World thread, read only ("playerbots watch"): every party, terminal phases included.
        std::vector<std::unique_ptr<Party>> const& Parties() const { return _parties; }

        // Route visualisation ("playerbots route show|hide", TestHarnessRunner.cpp): GM-only waypoint markers
        // (VISUAL_WAYPOINT) along the route backbone Retarget built last, the leader's walk, the target's route and
        // columns on the waypoint, the target and the next boss, in the run's instance; redrawn every
        // ROUTE_VIZ_REFRESH_MS for <seconds>. World thread only: commands and TickRouteViz run from World::Update
        // after the map updates finished (MapMgr::Update waits for its threads), so no map thread runs meanwhile.
        bool HandleRouteShow(ChatHandler* handler, uint32 seq, uint32 seconds);
        bool HandleRouteHide(ChatHandler* handler, uint32 seq);
        void TickRouteViz();

    private:
        TestHarnessMgr() = default;

        struct QueueItem
        {
            ContentDef const* content;
            Composition const* comp;
        };

        struct SetupJob
        {
            bool active = false;
            uint32 stage = 0;
            uint32 startedMs = 0;
            std::map<uint8, uint32> missing;  // class -> characters to create
            std::vector<std::string> accounts;  // accounts that must exist before characters are created
            bool leveling = false;              // new characters go to the levelling pool ("Rl" names)
            bool horde = false;                 // new characters get Horde races (RaceForClass)
        };

        // pool
        bool LoadPool(std::string& error);
        void TickSetup();
        bool CreateCharacter(uint32 accountId, uint8 cls, std::string& error);
        std::string NextName(uint8 cls, char const* prefix);
        // Characters per class for <groups> parallel groups of every listed size (the maximum over the
        // compositions of a size; only the optimal ones unless variants).
        std::map<uint8, uint32> RequiredPerClass(uint32 groups, std::vector<uint8> const& sizes,
                                                 bool variants) const;
        // Free pool character of that class for a party of that level: never one above the level; for level 80
        // the level-80 pool only (an 80 first), below 80 the highest level first (levelling pool on a tie).
        PoolCharacter const* PickCharacter(uint8 cls, uint8 level, std::set<ObjectGuid> const& picked,
                                           bool horde = false) const;

        // parties
        // [strat1] keep: the characters to take (a 5-man continue: the kept group, its leader in the leading slot)
        Party* CreateParty(Composition const* comp, ContentDef const* content, uint8 level, uint32 ilvl,
                           std::string& error, std::vector<ObjectGuid> const* keep = nullptr);
        // [strat1] a 5-man continue: the group an earlier run of this content kept. Its leader holds the instance
        // bind the group enters (InstanceSaveMgr::PlayerGetDestinationInstanceId: in a group, the leader's bind).
        bool FindKeptFive(Composition const* comp, ContentDef const* content, std::vector<ObjectGuid>& members,
                          uint32& instanceId, std::string& error) const;
        // [strat1] a character in a group of test characters only (a kept five waiting for its continue)
        bool InKeptGroup(ObjectGuid guid) const;
        void TickParty(Party& party);
        void SetPhase(Party& party, Phase phase);
        bool TickLogin(Party& party);
        bool TickPrepare(Party& party);
        bool TickGrouping(Party& party);
        bool TickTeleport(Party& party);
        // [guild60] the guild five's own walk in: a portal goal per member; the run starts once all are inside
        void TickGuildEnter(Party& party);
        // the content's entrances (areatrigger_teleport arrival points, a wing's own portals); false: none
        bool LoadEntrances(Party& party);
        // [world_boss] test preparation (harness test bots only, never guild runs): the world boss of the run is
        // alive at the start - a dead copy respawned, a pending respawn timer dropped
        // true: the boss is alive on its loaded grid (the run may start); [b342] run again once the group arrived -
        // at the first teleport try the boss's grid is usually not loaded yet ("not loaded")
        bool PrepareWorldBoss(Party& party);
        void TickRun(Party& party);
        // [lead107 stage 5] TickRun's group decisions, each its own step: the phantom-combat end (anyInCombat false
        // while a phantom combat counts out), the stale-victim drop, the wait for the dead / outside members, the
        // rest / far wait (true: the group travels on), the travel pause for a trailing member (true: it pauses) and
        // the travel step after the rest (true: the run ended)
        void TickPhantomCombat(Party& party, std::vector<Player*> const& players, Player* leader, uint32 now,
                               bool activeBoss, bool& anyInCombat);
        void TickStaleVictim(Party& party, Player* leader, uint32 now, bool anyInCombat, bool activeBoss);
        void WaitForAway(Party& party, std::vector<Player*> const& players, Player* leader,
                         std::vector<std::string> const& away);
        bool RestWait(Party& party, std::vector<Player*> const& players, Player* leader, uint32 now, bool wingWalking,
                      bool& ledGather);
        bool CompactPause(Party& party, Player* leader, uint32 now, bool ledGather);
        bool TickTravel(Party& party, std::vector<Player*> const& players, Player* leader, uint32 now,
                        bool wingWalking, bool ledGather);
        void Finish(Party& party, std::string const& result, std::string const& reason);
        void ReleaseParty(Party& party);
        void AssignProfessions(Party& party);
        void ApplyStrategies(Party& party);
        void GrantAccess(Party& party, Player* bot);
        void EnterInstance(Party& party, Player* leader);

        // runner
        void Retarget(Party& party, Player* leader);
        void Steer(Party& party, Player* leader);
        // [b46_patrol] the group steps back out of a patrol's way (TestHarnessRunner.cpp PATROL_STEP_BACK): true while
        // the leader walks there or stands there
        bool PatrolStepBack(Party& party, Player* leader);
        void SkipTarget(Party& party, ObjectGuid guid, uint32 ms, std::string const& reason);
        // route_blocker_body_pull: `blocker`'s pack stands on the way to `behind`: it becomes the target (false when
        // it may not: a boss of it, tried too often, nothing alive).
        bool TargetRouteBlocker(Party& party, Player* leader, Creature* blocker, WorldObject const* behind,
                                std::string const& why);
        bool IsRouteBlocker(Party const& party, ObjectGuid guid) const;
        // [route_blocker_required_only] `behind` is a required target (a boss or his encounter mob, an event starter
        // or keeper, a door guard, a gate / link / starter key): only a pack on the way to one is never skipped.
        // `why` names the rule.
        bool RouteBlockerBehindRequired(Party const& party, Map* map, WorldObject const* behind,
                                        std::string& why) const;
        // The route blocker that is the target is pulled by walking into it once the group is fully ready.
        void RouteBlockerBodyPull(Party& party, Player* leader, Creature* target, std::string const& why);
        // [queue-blocker-pull-gate] One gate in front of the unseen-blocker pull, TargetRouteBlocker and the body
        // pull: "" when the pull may go on; else why not, with `hold` true when the group only has to get ready.
        std::string BlockerPullGate(Party& party, Player* leader, Creature* blocker, WorldObject const* behind,
                                    bool bodyPull, bool& hold) const;
        bool UseStarter(Party& party, Player* leader, StarterTrack& starter, WorldObject* object);
        void ReopenKeyDoorInCombat(Party& party, Player* leader);
        void ReopenKeyDoorHeld(Party& party, Player* leader);
        // [expect_door] EventStarterDef::expectDoor: a use whose door is still shut EXPECT_DOOR_MS later is no use -
        // armed again.
        void CheckExpectedDoors(Party& party, Player* leader);
        // [strat_key_pass] KeyDoorPassDef: the group through an auto-closing key gate at once (Stratholme).
        void TickKeyDoorPass(Party& party, Player* leader);
        // [b32_strat_gate] who is cut off behind a closed key door (Party::doorCutMember), and the recovery: the
        // cut-off member uses its own key on the door from its side when nothing hits it.
        void TickDoorCut(Party& party, Player* leader);
        void TickRoundWalk(Party& party, Player* leader);  // [kara6] the walk round a door
        void MemberKeyAtDoor(Party& party, Player* leader);
        void EmulateAreaTriggers(Party& party, std::vector<Player*> const& players);
        void GuideGhosts(Party& party, std::vector<Player*> const& players);
        // An event was started and nothing hostile is left: walks the leader to the content's gather spot. True
        // while walking (the caller does nothing else), false at the spot or when no gather spot applies.
        bool WalkToEventGather(Party& party, Player* leader);
        bool EventGatherApplies(Party const& party) const;
        // A walked change to entrance `next`: every member's portal goal, the skips
        // cleared, wing_change logged with the reason.
        void StartWingChange(Party& party, size_t next, char const* why);
        // Proximity-started events (EventSpotDef): with nothing hostile reachable, the leader walks to the first spot
        // whose conditions hold. True while walking; false at the spot or when no spot applies.
        bool WalkToEventSpot(Party& party, Player* leader);
        // Index of the event spot that applies now (EventSpots order), -1 for none.
        int32 EventSpotIndex(Party const& party) const;
        // Route links (RouteLinkDef): the link over which target is reachable from the leader (it becomes the
        // party's link), -1 for none. The leader walks to it and over it (WalkToLink, true while it does); every
        // other member follows over the same link (CrossLinks, every tick).
        int32 LinkTo(Party& party, Player* leader, WorldObject* target);
        bool LinkUsable(Party const& party, Map* map, RouteLinkDef const& link) const;
        bool WalkToLink(Party& party, Player* leader);
        void CrossLinks(Party& party, std::vector<Player*> const& players, Player* leader);
        void EndLink(Party& party, char const* reason);
        // The leader (the tank) out of combat while a member fights far away or on another floor: true while it
        // walks to that member (the caller then sets no other waypoint).
        // Out of combat, a living member far behind (back from a corpse run): true while the leader (and with it the
        // group) walks to meet it, instead of letting it cross the idle packs alone.
        bool MeetMember(Party& party, std::vector<Player*> const& players, Player* leader);
        // Out of combat, farther than MEET_RANGE, no complete navmesh walk to the leader nor back, and staying put:
        // nobody waits for it or walks to it (re-checked, it rejoins when it can walk).
        bool MemberStranded(Party& party, Player* bot, Player* leader);
        // ROUTE_HARNESS_WALK_GUARD: a harness walk held at the aggro edge of an idle pack (harness_hold_refused_pack),
        // with the tank's careful plan verdict on it; once per 15 s per blocker.
        void LogHarnessHold(Party& party, Player* leader, Creature* blocker, char const* walk, float x, float y,
                            float z);
        // The escorted creature while the escort runs (nullptr ends it, logged once as "escort_end").
        Creature* ActiveEscort(Party& party, Map* map);
        // With an escort running: true while the leader walks back to the escorted creature (the caller then does
        // nothing else).
        bool StayWithEscort(Party& party, Player* leader);
        // Creatures of all these entries are dead (killed by the party, or a boss track naming them is done).
        bool EntriesDead(Party const& party, std::vector<uint32> const& entries) const;
        // Puts the event items of the content (EventItems) into the leader's bags, logged as "event_item": the
        // carried ones at the start (atStart), the dungeon drops once their creatures are dead (every tick).
        void PrepareEventItems(Party& party, Player* leader, bool atStart);
        // The starters naming these entries are used up (EventStarterDef::afterUsed).
        bool StartersUsed(Party const& party, std::vector<uint32> const& entries) const;
        // A gossip / escort starter the bots' own dungeon strategy already did counts as used up: the item that only
        // its gossip hands out (the UseItem starter after it names it in afterUsed) is in the leader's bags and the
        // harness never put it there (Culling of Stratholme: the CoS strategy's own Chromie talk gave the Arcane
        // Disruptor, the harness chain waited for its own Chromie use). Logged as "starter_done_by_group".
        void MarkStartersDoneByGroup(Party& party, Player* leader);
        // Black Morass: Medivh's shield and the rift counter (instance data), logged when they change, so a failed
        // event (shield 0: Medivh dies, everything despawns, cleanup 5 minutes later) is not read as "not started".
        void WatchEventState(Party& party, Map* map);
        // The leader can open the game object honestly: a lock that needs a key item needs that item in the bags
        // of a member, a lock that needs a skill (lockpicking, herbalism, ...) the skill. Generic locks are open.
        bool CanOpenLock(Party const& party, GameObject const* go) const;
        bool UseStarterItem(Party& party, Player* leader, StarterTrack& starter, GameObject* go);
        // [b51_strat] User 2026-10-26: before a door that closes again by itself (door / button autoCloseTime, or a
        // KeyDoorPassDef gate) is opened, every living member who can come stands within DOOR_GATHER_RANGE of the
        // opener, out of combat; true while it holds (the members are walked to the opener). No cap: a member who
        // cannot come is a stall for the stall recovery, the door is never opened early.
        bool DoorGatherHolds(Party& party, Player* opener, GameObject* door);
        // [b51_strat] the rat-trap alleys of Stratholme: the group walks over an armed trap point together.
        void TickGateTrapGather(Party& party, Player* leader);
        // [brd_bar_buy] [brd_bar_turnin] StarterKind::VendorBuy / QuestTurnIn at its creature; false: not done.
        bool UseStarterAtNpc(Party& party, Player* leader, StarterTrack& starter, Creature* creature);
        // Bots walk by themselves: the event for what a bot's own movement could not do (throttled), the reports the
        // bots' own actions left (SelfTravel::TakeReport), intent for a member, and the walked wing change.
        void LogCannotMove(Party& party, Player* bot, std::string const& capability, std::string const& reason,
                           float toX, float toY, float toZ);
        void DrainMoveReports(Party& party, std::vector<Player*> const& players);
        // [lead1] The tank's pull order reports (Leader::Orders) into the decision log, the order states the leader
        // observes (Fighting, Done), the pull_start owner line; [lead2] order_orphan.
        void DrainOrderReports(Party& party, Player* leader);
        // [lead7] C:/WoW/LEADER_REFACTOR_PLAN.md step 7, LEADER_PLANNER_IN_LEADER: the leader plans the tank's pull
        // (PullPlanner::Decide on the world thread, every world update, when the tank asked) and issues the decision;
        // a hopeless refusal is the planner_no_option bug event.
        void TickLeaderPlanner(Party& party);
        // [lead3] .. [lead10] C:/WoW/LEADER_REFACTOR_PLAN.md section 9: the leader's own decisions around its planner.
        // The leader decides this party's pulls (LEADER_PLANNER_IN_LEADER, a tracked 5-man tank; raids keep the old
        // harness pulls).
        bool LeaderDecides(Party const& party) const;
        // [lead6] the leader's one readiness verdict ("" = ready): the group here and topped up, nobody buffing,
        // sitting or preparing (PullReadiness::GroupPreparing)
        std::string LeaderReadyWhy(Party& party, Player* leader);
        // [lead3] the pack the leader must clear: asked of its planner whole (walk-in priced by the planner); the
        // leader holds where it stands meanwhile. `required`: no option for it is planner_no_option with a stop.
        void LeaderWholePack(Party& party, Player* leader, Creature* pack, std::string const& why, bool walkIn,
                             bool required);
        // [lead3] a boss pull: a BossPull order at the tank's next ask (PullPlanner::BossPullDecision)
        void LeaderBossPull(Party& party, Player* leader, Creature* boss, std::string const& why);
        // [lead3] the pack must be cleared (a route blocker, the only way to a boss / event / required target, a
        // boss's area trash); `why` names the rule
        bool LeaderNeedsPack(Party const& party, Map* map, WorldObject const* object, std::string& why) const;
        // [lead3] user decision 1: planner_no_option, once per pack; `stops` ends the run (LEADER_STOP_ON_NO_OPTION)
        // true: reported now (a pack reported before is not again)
        bool LeaderNoOption(Party& party, Player* leader, Unit* mob, std::vector<ObjectGuid> const& pack,
                            std::string const& why, bool stops, JsonObject fields);
        // [lead4] the one patrol owner: the harness's hold / lane hold / step-back and the planner's patrol waits as
        // one decision, a declared wait or an intercept; true: the leader holds this tick
        bool LeaderPatrolTick(Party& party, Player* leader);
        // [lead15] the leader's events to every member (LeaderOrders.h Events), every LEADER_EVENT_TICK_MS
        void TickLeaderEvents(Party& party, Player* tank);
        // [lead15] the spots of a pull (false: no gate for it) and what the pull still waits for ("" = everyone there)
        bool LeaderAssignSpots(Party& party, Player* tank, PullPlanner::Decision const& decision);
        std::string LeaderSpotsWait(Party& party, Player* tank);
        void LogHealReach(Party& party, Player* member, Player* healer, std::string const& why,
                          std::string const& fix);
        // [lead24] a former forcing timer's diagnostic: `kind` waited `ms` (once per kind per 30 s)
        void WaitedLong(Party& party, std::string const& kind, uint32 ms, std::string const& why);
        // [lead5] the leader planner's own verdict memory, aoe plan and required marks, the tank's view with the
        // leader's route target: free helpers in TestHarnessRunner.cpp (PartyRefusal, PartyVerdict, PartyAoePlanned,
        // PartyMarkRequired, PartyTankView).
        void SetMemberWaypoint(Party& party, Member& member, Player* bot, float x, float y, float z);
        // The leader's waypoint, cut before a closed door its walk runs through (ROUTE_CLOSED_DOORS_ALL: door_hold).
        // straight: a walked route link's far end (TestRunner::Waypoint::straight).
        void SetLeaderWaypoint(Party& party, Player* leader, float x, float y, float z, bool straight = false);
        void ClearMemberWaypoint(Member& member);
        void ClearMemberIntent(Party& party);
        // Diagnostic at a pull start: no alive healer within PULL_HEALER_RANGE of the tank (pull_without_healer).
        void CheckPullHealer(Party& party, std::vector<Player*> const& players);
        // Breathing: the leader's water watch (RouteWater.h); the group rests on the last dry spot, never in the water
        // (resting).
        void LeaveWater(Party& party, std::vector<Player*> const& players, bool resting);
        void LoadEncounters(Party& party, Map* map);
        void ResolveBoss(Party& party, BossTrack& boss, Map* map, InstanceScript* script);
        // [b31_dg] a script slot whose encounter opens with a gauntlet of adds (no wave-encounter fallback)
        bool GauntletSlot(Party const& party, BossTrack const& boss) const;
        // Removes the DungeonEncounter.dbc tracks a resolved script track already covers (same credit creature,
        // encounter order or name).
        void DropDbcTwins(Party& party);
        int32 BossOrder(Party const& party, Creature const* creature) const;
        // [sm-wing-done] Several entrances (Scarlet Monastery, Dire Maul): true while a living boss stands in the wing
        // of Party::entranceIdx (its nearest entrance); nextWing: the entrance of the lowest-order boss left elsewhere.
        bool WingBossesLeft(Party const& party, Map* map, Player* leader, int32& nextWing) const;
        void TrackAggro(Party& party, std::vector<Player*> const& players);
        void CloseLooseMob(Party& party, ObjectGuid guid, LooseMob& mob, char const* end);

        // buffs and consumables
        void TickBuffing(Party& party, std::vector<Player*> const& players, Player* leader);
        void BuffPassSetup(Party& party, std::vector<Player*> const& players);  // the pass's test setup

        // logging
        void LogEvent(Party& party, std::string const& type, JsonObject fields);
        void LogRunStart(Party& party);
        void LogPull(Party& party, std::vector<Player*> const& players, bool boss, std::string const& bossName);
        void LogAttemptEnd(Party& party, BossTrack& boss, std::string const& result);
        std::string PullMembersJson(Party& party);
        // [harness20] each member's stats and auras and the boss's auras at a boss engage (boss_pull_state)
        void LogBossPullState(Party& party, Unit* boss, std::string const& bossName);
        // Who started the fight (StatsRegistry::Opener): pull_start, and again in pull_end with every entry.
        std::string PullOpenerJson(Party& party, std::vector<Player*> const& players, bool atStart);
        void LogPullStart(Party& party, std::vector<Player*> const& players);
        void Heartbeat(Party& party, std::vector<Player*> const& players, Player* leader);
        void LogPositions(Party& party, std::vector<Player*> const& players);
        // [raid4] every aura on the boss now: spell, caster, stacks, ms left ("boss_auras")
        void LogBossAuras(Party& party, Creature* boss, std::string const& name);
        // [harness30] every member's own auras and the auras it put on each mob it fights, with stacks / charges and
        // the ms left, and its threat on those mobs ("aura_stacks", at the positions cadence in combat; `at`: "tick"
        // or "boss_pull")
        void LogAuraStacks(Party& party, std::vector<Player*> const& players, char const* at);
        void CheckStall(Party& party, std::vector<Player*> const& players, Player* leader, bool anyInCombat);
        // The idle guard: no fight for IDLE_GUARD_MS outside rest / buffing (IDLE_EVENT_WAIT_MS in a genuine event
        // wait) logs "[idle] <run>: no fight for N s: <components holding>" once, with a diagnostic snapshot.
        void CheckIdle(Party& party, std::vector<Player*> const& players, Player* leader, bool anyInCombat);
        // What holds the group now (the idle guard's text): phase, waits, link, target, skips, holds.
        std::string IdleHolders(Party& party, Player* leader);
        // The genuine event wait the group stands in (an event hold spot between waves, the Halls of Reflection
        // gather, Svala's intro, an escort), empty if none.
        std::string IdleEventWait(Party& party, Player* leader);
        std::string MemberBuffJson(Party& party, Player* bot);
        std::string MemberDiagnosisJson(Party& party, Player* bot, Player* leader);
        std::string PhaseLabel(Party const& party) const;

        void StartQueued();
        Party* FindParty(std::string const& id);
        bool AllBossesResolved(Party& party, Map* map);
        void WatchHorEscapeWalls(Party& party, Map* map, BossTrack& boss, Player* leader, uint32 now);
        // Every boss of the run that is not dead, with the reason (bossesMissing, boss_missing).
        JsonArray MissingBosses(Party& party, std::vector<std::string>* lines);

        // Clean-run streaks per content key (<LogsDir>/tests/streaks.json, user rule 2026-10-24): runall puts a
        // content with STREAK_CLEAN_RUNS consecutive clean runs (done, no boss missing, no death, no wipe) at the end
        // of the queue, untested or not-yet-clean contents first. Every finished run but "stopped" updates it; a
        // missing file is seeded once from the run logs since STREAK_SEED_FROM.
        struct StreakInfo
        {
            uint32 streak = 0;
            std::string lastResult;
            std::string lastTime;
        };
        void LoadStreaks();
        void SaveStreaks() const;
        void RecordStreak(std::string const& contentKey, bool clean, std::string const& result,
                          std::string const& time);
        std::map<std::string, StreakInfo> _streaks;
        bool _streaksLoaded = false;

        std::vector<std::unique_ptr<Party>> _parties;
        std::deque<QueueItem> _queue;
        uint32 _queueParallel = 8;
        bool _runAllRandomComp = false;  // [runall-random-comp]
        uint32 _nextSeq = 1;
        std::vector<PoolCharacter> _pool;
        bool _poolLoaded = false;
        std::set<ObjectGuid> _inUse;
        SetupJob _setup;
        std::deque<Composition> _guildComps;  // [guild60] one per guild run (Party::comp points into it)
        uint32 _setupTimer = 0;
        uint32 _queueTimer = 0;
    };

    // [lead107] the leader's view of a harness party (Ai/Leader/LeaderGroup.h): read from the Party, its actions
    // carried out by the harness (TestHarnessMgr::SkipTarget / LogEvent). Lives for one leader call.
    class PartyLeaderGroup : public Leader::LeaderGroup
    {
    public:
        PartyLeaderGroup(TestHarnessMgr& mgr, Party& party) : _mgr(mgr), _party(party) {}

        Leader::DungeonLeaderState& State() override { return _party.lead; }
        DecisionRun Run() const override { return DecisionRun{_party.id, _party.seq, _party.runStartMs}; }
        Leader::GroupRouteState& Route() override { return _party; }
        std::vector<ObjectGuid> MemberGuids() const override;
        std::vector<MemberInfo> Members() const override;
        bool HasContent() const override { return _party.content != nullptr; }
        bool LeaderDecides() const override { return _mgr.LeaderDecides(_party); }
        ObjectGuid LeaderGuid() const override { return _party.leader; }
        uint32 MapId() const override;
        uint32 InstanceId() const override { return _party.instanceId; }
        ObjectGuid RouteTarget() const override { return _party.target; }
        std::string const& RouteRule() const override { return _party.targetRouteRule; }
        bool RouteTargetIsBoss() const override { return _party.targetIsBoss; }
        bool RouteTargetIsStarter() const override { return _party.targetIsStarter; }
        bool InCombat() const override { return _party.inCombat; }
        bool FightOn(Player* leader) const override { return _party.FightOn(leader); }
        uint32 PullStartMs() const override { return _party.pullStartMs; }
        uint32 PullSeq() const override { return _party.pullSeq; }
        std::string const& PullStarter() const override { return _party.pullStarter; }
        std::string const& PullScript() const override { return _party.pullScript; }
        bool BodyPull() const override { return _party.approach != 0; }
        uint32 LastPullEndMs() const override { return _party.lastPullEndMs; }
        std::string ContentKey() const override { return _party.content ? _party.content->key : ""; }
        bool NeedBuff() const override { return _party.needBuff; }
        std::set<ObjectGuid> const& BossCreatures() const override { return _party.bossCreatures; }
        std::map<ObjectGuid, PathVerdict> const& PathVerdicts() const override { return _party.pathVerdicts; }
        std::map<ObjectGuid, bool>& PathVerdictLogged() override { return _party.pathVerdictLogged; }
        ObjectGuid PhantomHunt() const override { return _party.phantomHunt; }
        void ClearPhantomHunt() override { _party.phantomHunt.Clear(); }
        uint32 ArrivedMs() const override { return _party.arrivedMs; }
        bool DoorCutHold() const override { return _party.doorCutHold; }
        ObjectGuid DoorCutMember() const override { return _party.doorCutMember; }
        bool NeedsPack(Map* map, WorldObject const* object, std::string& why) const override
        {
            return _mgr.LeaderNeedsPack(_party, map, object, why);
        }
        int32 DoorStarterFor(GameObject const* door) const override;
        std::string StarterNote(int32 index) const override;
        bool HasDoorWalk() const override;
        bool GetWaypoint(ObjectGuid bot, WalkPoint& out) const override;
        void ClearWaypoint(ObjectGuid bot) override;
        void ReleaseSkip(ObjectGuid guid) override
        {
            _party.skipUntil.erase(guid);
            _party.skipReason.erase(guid);
        }
        void ResetSteerClock(uint32 now) override { _party.steerSinceMs = now; }
        void RetargetAt(uint32 ms) override { _party.nextRetargetMs = ms; }
        void SetBossOnTheWay(ObjectGuid boss, uint32 now) override;
        void MarkRequired(ObjectGuid mob) override;
        void NoteHopelessPack(Player* leader, Creature* mob) override;
        void StartDoorWalk(ObjectGuid door, ObjectGuid blocker, G3D::Vector3 const& point, uint32 untilMs) override;
        void FailRun(std::string const& reason) override;
        void SetLeaderWaypoint(Player* leader, float x, float y, float z, bool straight) override;
        bool BossKilledFor(uint32 entry) const override;
        void GiveWaypoint(Player* bot, float x, float y, float z, bool jump, bool straight) override;
        void MarkGateWaypoint(Player* bot) override;
        std::vector<std::reference_wrapper<Leader::BossFacts const>> Bosses() const override;
        uint8 Difficulty() const override { return _party.content ? _party.content->difficulty : 0; }
        bool IsRaid() const override { return _party.content && _party.content->raid; }
        uint8 ContentSize() const override { return _party.content ? _party.content->size : 0; }
        std::vector<uint32> const& ContentBossEntries() const override;
        int32 BossOrder(Creature const* creature) const override { return _mgr.BossOrder(_party, creature); }
        void RecordReentry(Player* bot, std::string const& killer, JsonObject context) override;
        void NoteReentered(ObjectGuid member, bool respawnZone, uint32 now) override;
        bool MemberHasPortalGoal(ObjectGuid member) const override;
        ::InstanceScript* InstanceScript() const override;
        bool BossesOpen() const override;
        bool GroupFullyReady(Player* leader) const override;
        bool ScriptEventRunning() const override;
        bool Levelling() const override;
        bool Regrouping() const override;
        bool MemberStranded(Player* bot, Player* leader) override { return _mgr.MemberStranded(_party, bot, leader); }
        void FinishRun(std::string const& result, std::string const& reason) override
        {
            _mgr.Finish(_party, result, reason);
        }
        void SetMemberWaypoint(ObjectGuid member, Player* bot, float x, float y, float z) override;
        void ClearMemberWaypoint(ObjectGuid member) override;
        bool MemberHasWaypoint(ObjectGuid member) const override;
        void SetMemberHasWaypoint(ObjectGuid member, bool has) override;
        void SetMemberHasPortalGoal(ObjectGuid member, bool has) override;
        void ClearMemberIntent() override { _mgr.ClearMemberIntent(_party); }
        uint32 Wipes() const override { return _party.wipes; }
        uint32 LastProgressMs() const override { return _party.lastProgressMs; }
        void NoteProgress(uint32 ms) override { _party.lastProgressMs = ms; }
        void LogCannotMove(Player* bot, std::string const& capability, std::string const& reason, float toX, float toY,
                           float toZ) override
        {
            _mgr.LogCannotMove(_party, bot, capability, reason, toX, toY, toZ);
        }
        bool BossEncounterInProgress() const override;
        void NoteActivity(Activity activity) override;
        void SetApartIgnore(ObjectGuid member, uint32 untilMs) override;
        void BuffPassSetup(std::vector<Player*> const& players) override;
        bool MemberConsumables(Player* bot, uint32& flask, uint32& food) const override;
        std::string MemberBuffJson(Player* bot) override;
        // [guild70] a test run's re-entries may teleport; a guild five's run (real play) walks out and in
        bool TestTeleports() const override { return !_party.guild; }
        void SkipTarget(ObjectGuid guid, uint32 ms, std::string const& reason) override;
        void LogEvent(std::string const& type, JsonObject fields) override;

        Party& GetParty() { return _party; }

    private:
        TestHarnessMgr& _mgr;
        Party& _party;
    };
}

#endif
