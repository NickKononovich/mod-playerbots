/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERDUNGEONRUN_H
#define PLAYERBOTS_LEADER_LEADERDUNGEONRUN_H

// [guild70] user 2026-11-07: «стенд — только для тестов; в игре данж ведёт лидер пятёрки». The five-leader's own run
// of a dungeon, outside any test: it groups the five, takes it in (the Dungeon Finder - the core's LFG, an honest game
// mechanic that ports the group in - or the five's own walk through the portal), checks the key / event items the
// members carry, has the instance cleared, then reports (drops are the global leader's, deaths, bosses, duration) and
// hands authority back. No test setup anywhere: no full health / mana, no cooldown reset, no teleport, no handouts.
//
// The clearing inside (buffing, rest, route, pulls, bosses, doors, ghosts) is the ClearEngine's: the leader's modules
// (Ai/Leader: LeaderBuffing, LeaderRest, RouteSteer, RouteTargets, RouteEvents, LeaderPlanner ...) driven through a
// LeaderGroup. Today the only engine is the test harness's Party state machine adapted for the run (TestHarnessMgr.cpp
// HarnessClearEngine, honest mode: Party::guild); the run itself never calls the harness.
//
// World thread only (ticked from GuildCoordinator::Update).

#include "Common.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class Player;

namespace Leader::Dungeon
{
    struct FiveRunMember
    {
        ObjectGuid guid;
        std::string role;  // tank / healer / dps
        bool human = false;  // a human player: never commanded, waited for
    };

    // what the global leader hands the five-leader
    struct FiveRunRequest
    {
        uint32 fiveId = 0;
        std::string contentKey;  // the dungeon leader's content ("wc-n")
        std::string name;        // the dungeon's name
        uint32 mapId = 0;
        uint8 difficulty = 0;
        uint32 lfgId = 0;          // LFGDungeons.dbc id (0: no Dungeon Finder entry)
        WorldLocation entrance;    // the outside portal
        float entranceReach = 10.0f;
        std::vector<FiveRunMember> members;  // the leader included
        ObjectGuid leader;
        bool viaLfg = false;  // the five-leader queues the Dungeon Finder (CanQueue said yes), else the five walks in
    };

    // the clearing's end, as the engine hands it back
    struct ClearResult
    {
        std::string engineRunId;
        std::string result;  // done / partial / failed / stalled / stopped / timeout / void
        std::string reason;
        std::vector<std::string> bossesKilled;
        uint32 bossesTotal = 0;
        uint32 wipes = 0;
        uint32 durationSec = 0;
        std::vector<std::pair<std::string, uint32>> deaths;  // member name, its deaths in the run
    };

    // the five-leader's report to the global leader
    struct FiveRunReport
    {
        std::string runId;
        std::string contentKey;
        std::string entry;   // "lfg" / "walk"
        std::string result;  // as ClearResult; "not_started": the run never got inside (lfgRefused: try the walk)
        std::string reason;
        std::vector<std::string> bossesKilled;
        uint32 bossesTotal = 0;
        uint32 wipes = 0;
        uint32 durationSec = 0;  // from the start of the run (queue / walk in included)
        uint32 clearSec = 0;     // inside
        std::vector<std::pair<std::string, uint32>> deaths;
        std::vector<std::string> needs;  // key / event items nobody carried (with their source)
        bool lfgRefused = false;
    };

    // the instance's clearing for a five that stands inside, grouped (see the file comment)
    class ClearEngine
    {
    public:
        virtual ~ClearEngine() = default;
        // starts it; false with `error`. The engine ends it with DungeonRuns::OnCleared(fiveId, ...)
        virtual bool Begin(FiveRunRequest const& request, std::string const& runId, std::string& error) = 0;
        // its phase ("" when none runs)
        virtual std::string Phase(uint32 fiveId) const = 0;
        virtual void Stop(uint32 fiveId, std::string const& reason) = 0;
    };

    // a key / event item a member must carry in (Leader::Knowledge::EventItems without a drop of the dungeon) that no
    // member holds: its id, name and how a player gets it
    struct MissingItem
    {
        uint32 itemId = 0;
        std::string name;
        std::string source;
    };
    std::vector<MissingItem> MissingCarriedItems(uint32 mapId, uint8 difficulty, std::vector<Player*> const& members);

    class DungeonRuns
    {
    public:
        static DungeonRuns& Instance();

        void SetEngine(ClearEngine* engine) { engine_ = engine; }
        using EndHook = std::function<void(uint32 fiveId, FiveRunReport const& report)>;
        void SetEndHook(EndHook hook) { endHook_ = std::move(hook); }

        // the five-leader's choice of the way in: the Dungeon Finder takes this five (enabled, the dungeon open to
        // every member, nobody in a battleground, the five complete and online); false with `why`
        bool CanQueue(FiveRunRequest const& request, std::string& why) const;
        // the Dungeon Finder refused this five once (its role check / proposal ended without the port): it walks
        bool LfgRefused(uint32 fiveId) const { return lfgRefused_.count(fiveId) != 0; }
        void ForgetFive(uint32 fiveId) { lfgRefused_.erase(fiveId); }

        // the run starts (members online; for the walk: standing at the entrance); "" with `error`
        std::string Start(FiveRunRequest const& request, std::string& error);
        void Stop(uint32 fiveId, std::string const& reason);
        // the run's phase for the Orders tab ("" when none runs)
        std::string Phase(uint32 fiveId) const;
        void Update(uint32 diff);

        // the engine's end of the clearing
        void OnCleared(uint32 fiveId, ClearResult const& result);

    private:
        DungeonRuns() = default;

        enum class Stage : uint8
        {
            Group,     // the five-leader forms the group
            Queue,     // in the Dungeon Finder (role check, queue, proposal) until the core ports the five in
            WalkIn,    // each member walks in through the portal itself
            Inside,    // the clearing runs (the engine)
            Ended
        };

        struct Run
        {
            FiveRunRequest request;
            std::string id;
            Stage stage = Stage::Group;
            uint32 startMs = 0;
            uint32 insideMs = 0;
            ObjectGuid groupGuid;
            bool queued = false;  // the Dungeon Finder took the join (a state other than none seen)
            std::vector<std::string> needs;
            std::string entry;
        };

        static char const* StageName(Stage stage);
        bool TickGroup(Run& run);
        void TickQueue(Run& run);
        void TickWalkIn(Run& run);
        void GoInside(Run& run);
        // the run ends before the clearing (or without one): its report, the group released
        void End(Run& run, std::string const& result, std::string const& reason, bool lfgRefused = false);
        void Report(Run& run, FiveRunReport& report);
        void Release(Run& run, bool disband);

        ClearEngine* engine_ = nullptr;
        EndHook endHook_;
        std::map<uint32, Run> runs_;
        std::set<uint32> lfgRefused_;
        uint32 seq_ = 0;
        uint32 timer_ = 0;
    };
}

#endif
